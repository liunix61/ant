local cpath, repopath, fddata = ...

package.path = "engine/?.lua"
package.cpath = cpath

local vfs = require "vfs"
local thread = require "bee.thread"
local socket = require "bee.socket"
local io_req = thread.channel "IOreq"
thread.setname "ant - IO thread"

local quit = false
local channelfd = socket.undump(fddata)

local function loadfile(path)
	local f = io.open(path)
	if not f then
		return nil, path..':No such file or directory.'
	end
	local str = f:read 'a'
	f:close()
	return load(str, "@" .. path)
end

local function dofile(path)
	return assert(loadfile(path))()
end

dofile "engine/common/log.lua"

local access = dofile "engine/vfs/repoaccess.lua"
dofile "engine/editor/create_repo.lua" (repopath, access)

local function response_id(id, ...)
	if id then
		assert(type(id) == "userdata")
		thread.rpc_return(id, ...)
	end
end

local CMD = {
	GET = vfs.realpath,
	LIST = vfs.list,
	TYPE = vfs.type,
	REPOPATH = vfs.repopath,
	MOUNT = vfs.mount,
	FETCH = function() end,
}

local S_CMD = {}
for k, f in pairs(CMD) do
	S_CMD[k] = function (id, ...)
		response_id(id, f(...))
	end
end
for k, f in pairs(S_CMD) do
	CMD["S_"..k] = f
end

local function dispatch(ok, id, cmd, ...)
	if not ok then
		return
	end
    local f = CMD[cmd]
    if not f then
        print("Unsupported command : ", cmd)
        response_id(id)
    else
        response_id(id, f(...))
    end
	return true
end

local event = {channelfd}
local eventfunc = {}

local function event_del(fd)
	if fd then
		eventfunc[fd] = nil
		for i, h in ipairs(event) do
			if h == fd then
				table.remove(event, i)
				break
			end
		end
	end
end

eventfunc[channelfd] = function ()
	channelfd:recv()
	if nil == channelfd:recv() then
		event_del(channelfd)
		return
	end
	while dispatch(io_req:pop()) do
	end
end

local exclusive = require "ltask.exclusive"
local ltask

local function ltask_ready()
	return coroutine.yield() == nil
end

local function ltask_init()
	assert(loadfile "engine/task/service/service.lua")(true)
	ltask = require "ltask"
	ltask.dispatch(CMD)
	local waitfunc, fd = exclusive.eventinit()
	local ltaskfd = socket.fd(fd, "tcp6", "connect")
	event[#event+1] = ltaskfd
	eventfunc[ltaskfd] = function ()
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
	end
end

function CMD.SWITCH()
	while not ltask_ready() do
		exclusive.sleep(1)
	end
	ltask_init()
end

function CMD.quit()
	quit = true
end

local function work()
	while not quit do
		local rds = socket.select(event)
		if rds then
			for _, rd in ipairs(rds) do
				eventfunc[rd]()
			end
		end
	end
end

work()
