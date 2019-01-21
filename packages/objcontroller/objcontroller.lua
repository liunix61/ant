local objcontroller = {}; objcontroller.__index = objcontroller

function objcontroller:bind_message_event(msg)
	local queue = {}

	local function get_eventlist(name)
		local eventlist = queue[name]
		if eventlist == nil then
			eventlist = {}
			queue[name] = eventlist
		end
		return eventlist
	end

	msg.observers:add  {
		mouse_click = function (_, what, press, x, y, state)
			local eventlist = get_eventlist("mouse_click")
			eventlist[#eventlist+1] = {what=what, press=press, x=x, y=y, state=state}
		end,
		mouse_move = function (_, x, y, state)
			local eventlist = get_eventlist("mouse_move")
			eventlist[#eventlist+1] = {x=x, y=y, state=state}
		end,
		mouse_wheel = function (_, x, y, delta)
			local eventlist = get_eventlist("mouse_wheel")
			eventlist[#eventlist+1] = {x=x, y=y, delta=delta}
		end,
		keyboard = function (_, key, press, state)
			local eventlist = get_eventlist("keyboard")
			eventlist[#eventlist+1] = {key=key, press=press, state=state}
		end,
		touch = function (...)
			error "not implement"
		end,
	}

	self.msgqueue = queue
end

local function get_tigger(oc, name)
	local tigger = oc.tiggers[name]
	if tigger == nil then
		tigger = {keys={}}
		oc.tiggers[name] = tigger
		local names = oc.tigger_names
	
		names[#names+1] = name
		table.sort(names, function(a, b) return a > b end)		
	end

	return tigger
end

function objcontroller.register_tigger(name, tiggerkey)
	local tigger = get_tigger(self, name)	
	tigger.keys[#tigger.keys+1] = tiggerkey	
end

function objcontroller:register(tiggermap)
	for name, tiggerkey in pairs(tiggermap) do
		local tigger = get_tigger(self, name)
		local keys = tigger.keys
		table.move(tiggerkey, 1, #tiggerkey, #keys+1, keys)
	end
end

function objcontroller:bind_tigger(tiggername, cb)
	local tigger = self.tiggers[tiggername]
	if tigger == nil then
		error(string.format("not found tigger name:%s", tiggername))
	end

	tigger.cb = cb
end

local function is_state_match(state1, state2)
	local function key_names(state)
		local t = {}
		for k in pairs(state) do
			t[#t+1] = k
		end
		return t
	end

	local keys1 = key_names(state1)
	local keys2 = key_names(state2)
	if #keys1 ~= #keys2 then
		return false
	end

	for idx, k1 in ipairs(keys1) do
		local k2 = keys2[idx]
		if k1 ~= k2 then
			return false
		end

		local v1 = state1[k1]
		local v2 = state2[k2]
		if v1 ~= v2 then
			return false
		end
	end

	return true
end

local function match_event(tiggerkey, name, event)	
	if name ~= tiggerkey.name then
		return false
	end

	if name == "mouse_click" then
		return 	event.what == tiggerkey.what and 
				event.press == tiggerkey.press and
				is_state_match(event.state, tiggerkey.state)
	elseif name == "mouse_move" then
		return is_state_match(event.state, tiggerkey.state)
	elseif name == "mouse_wheel" then
		return is_state_match(event.state, tiggerkey.state)
	elseif name == "keyboard" then		
		return event.key == tiggerkey.key and 
				event.press == tiggerkey.press and
				is_state_match(event.state, tiggerkey.state)
	end
	error "not implement"
end



function objcontroller:update()
	for eventname, eventlist in pairs(self.msgqueue) do
		local tiggers = self.tiggers

		for _, e in ipairs(eventlist) do
			for _, name in ipairs(self.tigger_names) do
				local tigger = tiggers[name]
				local keys = tigger.keys
				for _, key in ipairs(keys) do
					if match_event(key, eventname, e) then
						tigger.cb(e)
					end
				end
			end
		end

		self.msgqueue[eventname] = {}
	end	
end


function objcontroller.new(bindings)
	local inst = setmetatable({
		tiggers = {},
		tigger_names = {},
	}, objcontroller)
	inst:register(bindings)
	return inst
end

return objcontroller