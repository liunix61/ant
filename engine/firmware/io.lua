local loadfile, config, host, fddata = ...

-- C libs only
local thread = require "bee.thread"
local socket = require "bee.socket"
local protocol = require "protocol"
local _print
local QUIT = false
local OFFLINE = false

local channelfd = fddata and socket.undump(fddata) or nil

thread.setname "ant - IO thread"

local status = {}
local repo

local io_req

local connection = {
	request = {},
	sendq = {},
	recvq = {},
	fd = nil,
}

local function connection_send(...)
	local pack = protocol.packmessage({...})
	table.insert(connection.sendq, 1, pack)
end

local function init_channels()
	io_req = thread.channel "IOreq"

	local origin = os.time() - os.clock()
	local function os_date(fmt)
		local ti, tf = math.modf(origin + os.clock())
		return os.date(fmt, ti):gsub('{ms}', ('%03d'):format(math.floor(tf*1000)))
	end
	local function round(x, increment)
		increment = increment or 1
		x = x / increment
		return (x > 0 and math.floor(x + 0.5) or math.ceil(x - 0.5)) * increment
	end
	local function packstring(...)
		local t = {}
		for i = 1, select('#', ...) do
			local x = select(i, ...)
			if math.type(x) == 'float' then
				x = round(x, 0.01)
			end
			t[#t + 1] = tostring(x)
		end
		return table.concat(t, '\t')
	end
	_print = _G.print
	function _G.print(...)
		local info = debug.getinfo(2, 'Sl')
		local text = ('[%s][IO   ](%s:%3d) %s'):format(os_date('%Y-%m-%d %H:%M:%S:{ms}'), info.short_src, info.currentline, packstring(...))
		if OFFLINE then
			_print(text)
		else
			connection_send("LOG", text)
		end
	end
end

local function init_repo()
	local vfs = assert(loadfile(config.vfspath))()
	repo = vfs.new(config.repopath)
	status.repo = repo
end

local function connect_server(address, port)
	print("Connecting", address, port)
	local fd, err = socket "tcp"
	if not fd then
		print("[ERROR] socket:", err)
		return
	end
	local ok
	ok, err = fd:connect(address, port)
	if ok == nil then
		fd:close()
		print("[ERROR] connect:", err)
		return
	end
	if ok == false then
		local rd,wt = socket.select(nil, {fd})
		if not rd then
			print("[ERROR] select:", wt)	-- select error
			fd:close()
			return
		end
	end
	local ok, err = fd:status()
	if not ok then
		fd:close()
		print("[ERROR] status:", err)
		return
	end
	print("Connected")
	return fd
end

local function listen_server(address, port)
	print("Listening", address, port)
	local fd, err = socket "tcp"
	if not fd then
		print("[ERROR] socket:", err)
		return
	end
	fd:option("reuseaddr", 1)
	local ok
	ok, err = fd:bind(address, port)
	if not ok then
		print("[ERROR] bind:", err)
		return
	end
	ok, err = fd:listen()
	if not ok then
		print("[ERROR] listen:", err)
		return
	end
	local rd,wt = socket.select({fd}, nil, 2)
	if rd == false then
		print("[ERROR] select:", 'timeout')
		fd:close()
		return
	elseif rd == nil then
		print("[ERROR] select:", wt)	-- select error
		fd:close()
		return
	end
	local newfd, err = fd:accept()
	if not newfd then
		fd:close()
		print("[ERROR] accept:", err)
		return
	end
	print("Accepted")
	return newfd
end

local function wait_server()
	if config.socket then
		return socket.undump(config.socket)
	end
	if config.nettype == nil then
		return
	end
	if config.nettype == "listen" then
		return listen_server(config.address, tonumber(config.port))
	end
	if config.nettype == "connect" then
		return connect_server(config.address, tonumber(config.port))
	end
end

-- response io request with id
local function response_id(id, ...)
	if id then
		assert(type(id) ~= "string")
		thread.rpc_return(id, ...)
	end
end

local function response_err(id, msg)
	print("[ERROR]", msg)
	response_id(id, nil)
end

local CMD = {}

local rdset = {}
local wtset = {}
local rdfunc = {}
local wtfunc = {}
local function event_addr(fd, func)
	if fd then
		rdset[#rdset+1] = fd
		rdfunc[fd] = func
	end
end
local function event_addw(fd, func)
	if fd then
		wtset[#wtset+1] = fd
		wtfunc[fd] = func
	end
end
local function event_delr(fd)
	if fd then
		rdfunc[fd] = nil
		for i, h in ipairs(rdset) do
			if h == fd then
				table.remove(rdset, i)
				break
			end
		end
	end
end
local function event_delw(fd)
	if fd then
		wtfunc[fd] = nil
		for i, h in ipairs(wtset) do
			if h == fd then
				table.remove(wtset, i)
				break
			end
		end
	end
end

local function event_select(timeout)
	local sending = connection.sendq
	local rd, wt
	if #sending > 0  then
		rd, wt = socket.select(rdset, wtset, timeout)
	else
		rd, wt = socket.select(rdset, nil, timeout)
	end
	if not rd then
		if rd == false then
			-- timeout
			return false
		end
		-- select error
		return nil, wt
	end
	for _, fd in ipairs(wt) do
		wtfunc[fd](fd)
	end
	for _, fd in ipairs(rd) do
		rdfunc[fd](fd)
	end
	return true
end

local function request_start(req, args, promise)
	if OFFLINE then
		print("[ERROR] " .. req .. " failed in offline mode.")
		promise.reject()
		return
	end
	local list = connection.request[args]
	if list then
		list[#list+1] = promise
	else
		connection.request[args] = { promise }
		connection_send(req, args)
	end
end
local function request_send(...)
	if OFFLINE then
		return
	end
	connection_send(...)
end

status.request = request_start

local function request_complete(args, ok, err)
	local list = connection.request[args]
	if not list then
		return
	end
	connection.request[args] = nil
	if ok then
		for _, promise in ipairs(list) do
			promise.resolve(args)
		end
	else
		for _, promise in ipairs(list) do
			promise.reject(args, err)
		end
	end
end

local function request_file(id, req, hash, res, path)
	local promise = {
		resolve = function ()
			CMD[res](id, path)
		end,
		reject = function ()
			local errmsg = "MISSING "
			if type(path) == "table" then
				errmsg = errmsg .. table.concat(path, " ")
			else
				errmsg = errmsg .. path
			end
			response_err(id, errmsg)
		end
	}
	request_start(req, hash, promise)
end

-- response functions from file server (connection)
local response = {}

function response.ROOT(hash)
	if hash == '' then
		_print("[ERROR] INVALID ROOT")
		os.exit(-1, true)
		return
	end
	print("[response] ROOT", hash)
	repo:updatehistory(hash)
	repo:changeroot(hash)
end

-- REMARK: Main thread may reading the file while writing, if file server update file.
-- It's rare because the file name is sha1 of file content. We don't need update the file.
-- Client may not request the file already exist.
function response.BLOB(hash, data)
	print("[response] BLOB", hash, #data)
	if repo:write_blob(hash, data) then
		request_complete(hash, true)
	end
end

function response.FILE(hash, size)
	print("[response] FILE", hash, size)
	repo:write_file(hash, size)
end

function response.MISSING(hash)
	print("[response] MISSING", hash)
	request_complete(hash, false)
end

function response.SLICE(hash, offset, data)
	print("[response] SLICE", hash, offset, #data)
	if repo:write_slice(hash, offset, data) then
		request_complete(hash, true)
	end
end

function response.RESOURCE(fullpath, hash)
	print("[response] RESOURCE", fullpath, hash)
	repo:set_resource(fullpath, hash)
	request_complete(fullpath, true)
end

function response.FETCH(path, hashs)
	print("[response] FETCH", path, hashs)
	local waiting = {}
	local missing = {}
	local function finish()
		if next(waiting) == nil then
			local res = {}
			for h in pairs(missing) do
				res[#res+1] = h
			end
			if #res == 0 then
				request_complete(path, true)
			else
				table.insert(res, 1, "MISSING")
				request_complete(path, false, table.concat(res))
			end
		end
	end
	local promise = {
		resolve = function (hash)
			waiting[hash] = nil
			finish()
		end,
		reject = function (hash)
			missing[hash] = true
			waiting[hash] = nil
			finish()
		end
	}
	hashs:gsub("[^|]+", function(hash)
		local realpath = repo:hashpath(hash)
		local f <close> = io.open(realpath, "rb")
		if not f then
			waiting[hash] = true
			request_start("GET", hash, promise)
		end
	end)
	finish()
end

local ListNeedGet <const> = 3
local ListNeedResource <const> = 4

function CMD.LIST(id, path)
	print("[request] LIST", path)
	local dir, r, hash = repo:list(path)
	if dir then
		response_id(id, dir)
		return
	end
	if r == ListNeedGet then
		request_file(id, "GET", hash, "LIST", path)
		return
	end
	if r == ListNeedResource then
		request_file(id, "RESOURCE", hash, "LIST", path)
		return
	end
	response_id(id, nil)
end

function CMD.FETCH(id, path)
	print("[request] FETCH", path)
	request_start("FETCH", path, {
		resolve = function ()
			response_id(id)
		end,
		reject = function (_, err)
			response_err(id, err)
		end
	})
end

function CMD.TYPE(id, fullpath)
	print("[request] TYPE", fullpath)
	local path, name = fullpath:match "(.*)/(.-)$"
	if path == nil then
		if fullpath == "" then
			response_id(id, "dir")
			return
		end
		path = ""
		name = fullpath
	end
	local dir, r, hash = repo:list(path)
	if dir then
		local v = dir[name]
		if not v then
			response_id(id, nil)
		elseif v.type == 'f' then
			response_id(id, "file")
		else
			response_id(id, "dir")
		end
		return
	end

	if r == ListNeedGet then
		request_file(id, "GET", hash, "TYPE", fullpath)
		return
	end
	if r == ListNeedResource then
		request_file(id, "RESOURCE", hash, "TYPE", fullpath)
		return
	end
	response_id(id, nil)
end

function CMD.GET(id, fullpath)
	print("[request] GET", fullpath)
	local path, name = fullpath:match "(.*)/(.-)$"
	if path == nil then
		path = ""
		name = fullpath
	end
	local dir, r, hash = repo:list(path)
	if not dir then
		if r == ListNeedGet then
			request_file(id, "GET", hash, "GET", fullpath)
			return
		end
		if r == ListNeedResource then
			request_file(id, "RESOURCE", hash, "GET", fullpath)
			return
		end
		response_err(id, "Not exist<1> " .. path)
		return
	end

	local v = dir[name]
	if not v then
		response_err(id, "Not exist<2> " .. fullpath)
		return
	end
	if v.type ~= 'f' then
		response_id(id, false, v.hash)
		return
	end
	local realpath = repo:hashpath(v.hash)
	local f = io.open(realpath,"rb")
	if not f then
		request_file(id, "GET", v.hash, "GET", fullpath)
	else
		f:close()
		response_id(id, realpath)
	end
end

function CMD.RESOURCE_SETTING(id, ext, setting)
	print("[request] RESOURCE_SETTING", ext, setting)
	request_send("RESOURCE_SETTING", ext, setting)
	response_id(id)
end

function CMD.SEND(_, ...)
	request_send(...)
end

function CMD.quit(id)
	QUIT = true
end

-- dispatch package from connection
local function dispatch_net(cmd, ...)
	local f = response[cmd]
	if not f then
		print("[ERROR] Unsupport net command", cmd)
		return
	end
	f(...)
end

local function dispatch(ok, id, cmd, ...)
	if not ok then
		-- no req
		return false
	end
	local f = CMD[cmd]
	if not f then
		print("[ERROR] Unsupported command : ", cmd)
	else
		f(id, ...)
		--local ok, err = xpcall(f, debug.traceback, ...)
		--if not ok then
		--	print(err)
		--end
	end
	return true
end

local exclusive = require "ltask.exclusive"
local ltask

local S = {}; do
	local session = 0
	for v in pairs(CMD) do
		S[v] = function (...)
			session = session + 1
			ltask.fork(function (...)
				dispatch(true, ...)
			end, session, v, ...)
			return ltask.wait(session)
		end
	end
	for v in pairs(CMD) do
		S["S_"..v] = function (id, ...)
			dispatch(true, id, v, ...)
		end
	end
	function response_id(id, ...)
		if id then
			assert(type(id) ~= "string")
			if type(id) == "userdata" then
				thread.rpc_return(id, ...)
			else
				ltask.wakeup(id, ...)
			end
		end
	end
end

local function ltask_ready()
	return coroutine.yield() == nil
end

local function ltask_init()
	assert(loadfile "/engine/task/service/service.lua")(true)
	ltask = require "ltask"
	ltask.dispatch(S)
	local waitfunc, fd = exclusive.eventinit()
	local ltaskfd = socket.fd(fd, "tcp6", "connect")
	event_addr(ltaskfd, function ()
		waitfunc()
		local SCHEDULE_IDLE <const> = 1
		local SCHEDULE_QUIT <const> = 2
		local SCHEDULE_SUCCESS <const> = 3
		while true do
			local s = ltask.schedule_message()
			if s == SCHEDULE_QUIT then
				ltask.log "${quit}"
				return
			end
			if s == SCHEDULE_IDLE then
				break
			end
			coroutine.yield()
		end
	end)
end

function CMD.SWITCH()
	while not ltask_ready() do
		exclusive.sleep(1)
	end
	ltask_init()
end

local function work_offline()
	OFFLINE = true

	while true do
		event_select()
	end
end

local function work_online()
	request_send("ROOT")
	while not QUIT do
		if host.update(status) then
			break
		end
		local ok, err = event_select()
		if ok == nil then
			print("[ERROR] Connection Error", err)
			break
		end
	end
end

if not host then
	init_channels()
	host = {}
	function host.update(_)
	end
	function host.exit(_)
		print("Working offline")
		work_offline()
	end
end

local function init_event()
	local result = {}
	local reqs = {}
	local reading = connection.recvq
	local sending = connection.sendq
	event_addr(connection.fd, function (fd)
		local data, err = fd:recv()
		if not data then
			if err then
				-- socket error
				return nil, err
			end
			return nil, "Closed by remote"
		end
		table.insert(reading, data)
		while protocol.readmessage(reading, result) do
			if reqs then
				if result[1] ~= "ROOT" then
					table.insert(reqs, result)
				else
					dispatch_net(table.unpack(result))
					for _, req in ipairs(reqs) do
						dispatch_net(table.unpack(req))
					end
					reqs = nil
				end
			else
				dispatch_net(table.unpack(result))
			end
		end
	end)
	event_addw(connection.fd, function (fd)
		while true do
			local data = table.remove(sending)
			if data == nil then
				break
			end
			local nbytes, err = fd:send(data)
			if nbytes then
				if nbytes < #data then
					table.insert(sending, data:sub(nbytes+1))
					break
				end
			else
				if err then
					return nil, err
				else
					table.insert(sending, data)	-- push back
				end
				break
			end
		end
	end)
end

local function main()
	init_repo()
	event_addr(channelfd, function (fd)
		if nil == fd:recv() then
			event_delr(fd)
			return
		end
		while dispatch(io_req:pop()) do
		end
	end)
	if config.address then
		connection.fd = wait_server()
		if connection.fd then
			status.fd = connection.fd
			init_event()
			work_online()
			event_delr(connection.fd)
			event_delw(connection.fd)
			-- socket error or closed
		end
	end
	local uncomplete_req = {}
	for hash in pairs(connection.request) do
		table.insert(uncomplete_req, hash)
	end
	for _, hash in ipairs(uncomplete_req) do
		request_complete(hash, false)
	end
	if QUIT then
		return
	end
	host.exit(status)
end

main()
