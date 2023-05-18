local ltask = require "ltask"
local SupportGesture <const> = require "bee.platform".os == "ios"
local scheduling

local S = {}

local event = {
    init = {},
    exit = {},
    size = {},
    mouse_wheel = {},
    mouse = {},
    touch = {},
    gesture = {},
    keyboard = {},
    char = {},
}

local SCHEDULE_SUCCESS <const> = 3

local CMD = {}
local queue = {}
local initialized = {}
local noempty = {}

for cmd in pairs(event) do
    CMD[cmd] = function (...)
        queue[#queue+1] = table.pack(cmd, ...)
        if #queue == 1 then
            ltask.wakeup(noempty)
        end
    end
end

local function dispatch(cmd,...)
    CMD[cmd](...)
end

local function call_event(cmd, ...)
    local user = event[cmd]
    for i = 1, #user do
        if ltask.call(user[i], cmd, ...) then
            return
        end
    end
end

ltask.fork(function ()
    ltask.wait(initialized)
    while true do
        if #queue == 0 then
            ltask.wait(noempty)
        else
            local q = queue
            queue = {}
            for k = 1, #q do
                local e = q[k]
                call_event(table.unpack(e, 1, e.n))
            end
        end
    end
end)

if SupportGesture then
    local gesture = require "ios.gesture"
    local function gesture_init()
        gesture.tap {}
        gesture.pinch {}
        gesture.long_press {}
        gesture.pan {}
    end
    local function gesture_dispatch(name, ...)
        if not name then
            return
        end
        ltask.send(ltask.self(), "send_gesture", name, ...)
        return true
    end
    local event_init = event.init
    function CMD.init(...)
        ltask.fork(function (...)
            gesture_init()
            for i = 1, #event_init do
                if ltask.call(event_init[i], 'init', ...) then
                    return
                end
            end
            ltask.wakeup(initialized)
        end, ...)
    end
    function CMD.update()
        while gesture_dispatch(gesture.event()) do
        end
        repeat
            scheduling()
        until ltask.schedule_message() ~= SCHEDULE_SUCCESS
    end
else
    local event_init = event.init
    function CMD.init(...)
        ltask.fork(function (...)
            for i = 1, #event_init do
                if ltask.call(event_init[i], 'init', ...) then
                    return
                end
            end
            ltask.wakeup(initialized)
        end, ...)
    end
    function CMD.update()
        repeat
            scheduling()
        until ltask.schedule_message() ~= SCHEDULE_SUCCESS
    end
end

function S.create_window()
    local ServiceWorld = ltask.queryservice "ant.window|world"
    for _, v in pairs(event) do
        v[1] = ServiceWorld
    end

    ltask.fork(function ()
        local ServiceRmlui = ltask.queryservice "ant.rmlui|rmlui"
        for _, e in ipairs {"mouse", "touch", "gesture"} do
            table.insert(event[e], 1, ServiceRmlui)
        end
    end)

    local exclusive = require "ltask.exclusive"
    scheduling = exclusive.scheduling()
    local window = require "window"
    local handle = window.init(dispatch)
    ltask.fork(function()
        window.mainloop(handle, true)
        ltask.multi_wakeup "quit"
    end)
end

function S.wait()
    ltask.multi_wait "quit"
end

return S
