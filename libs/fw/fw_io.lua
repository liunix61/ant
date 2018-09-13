local log, pkg_dir, sand_box_dir = ...

--package.path = package.path .. ";/fw/?.lua;" .. pkg_dir .. "/fw/?.lua;" .. pkg_dir .. "/?.lua;" .. "/fw/?.lua;/libs/?.lua;/?.lua;./?/?.lua;/libs/?/?.lua;"
--TODO: find a way to set this
--path for the remote script
lanes = require "lanes"
if lanes.configure then lanes.configure({with_timers = false, on_state_create = custom_on_state_create}) end
linda = lanes.linda()


--"cat" means categories for different log
--for now we have "Script" for lua script log
--and "Bgfx" for bgfx log
--"Device" for deivce msg
--project entrance
entrance = nil

origin_print = print
function sendlog(cat, ...)
    linda:send("log", {cat, os.clock(),...})
    --origin_print(cat, ...)
end

function app_log(cat, ...)

    local output_log_string = {}
    for _, v in ipairs({...}) do
        table.insert(output_log_string, tostring(v))
    end

    sendlog(cat, table.unpack(output_log_string))
end

print = function(...)
    origin_print(...)
    --print will have a priority 1
    app_log("Script", ...)
end

perror = function(...)
    origin_print("error!", ...)
    app_log("Error", ...)
    error(...)
end

local function get_require_search_path(r_name)
    --return a table of possible path the file is on
    local search_string = package.path
    local search_table = {}

    --separate with ";"
    --"../" not support

    --print("require search string", search_string)
    for s_path in string.gmatch(search_string, ".-;") do
        --print("get requrie search path: "..s_path)

        local r_path = string.gsub(r_name, "%.", "/")
        s_path = string.gsub(s_path, "?", r_path)
        --get rid of ";" symbol
        s_path = string.gsub(s_path, ";", "")
        table.insert(search_table, s_path)
    end

    return search_table
end

function CreateIOThread(linda, pkg_dir, sb_dir)
    package.path = "./libs/dev/Common/?.lua;"..package.path
    print("init client repo", pkg_dir, sb_dir)

    --[[
    local vfs_cloud = require "firmware.vfs_cloud"
    local root_dir = sb_dir.."/Documents"
    local dir_table = {libs = root_dir .. "/libs", assets = root_dir .. "/assets"}
    local io_vfs_cloud = vfs_cloud.new(pkg_dir, dir_table)
--]]
    local vfs = require "firmware.vfs"
    local root_dir = sb_dir .. "/Documents"
    local io_vfs = vfs.new(pkg_dir, root_dir)

    ---[[
    local origin_require = require
    require = function(require_path)
        print("requiring io"..require_path)

        local path_table = get_require_search_path(require_path)
        local err_msg = ""
        for _, v in ipairs(path_table) do
            --local status, err = ant_load(v, io_repo)
            local status, err = ant_load(v, io_vfs_cloud)
            if status then
                local result, ret = xpcall(status, debug.traceback)
                if result then
                    return ret
                else
                    return nil, ret
                end
            else
                err_msg = err_msg .. err .. "\n"
            end
        end

        print("use origin require")
        return origin_require(require_path)
    end
    --]]

    print("create io")
    --print(pcall(require, "fw.client_io"))
    local client_io = require "fw.client_io"
    print("create io data", linda, pkg_dir, sb_dir)
    local c = client_io.new("127.0.0.1", 8888, linda, pkg_dir, sb_dir, io_vfs)

    print("create io finished")
    while true do
        c:mainloop(0.001)
    end
end

local lanes_err
io_thread, lanes_err = lanes.gen("*",{globals = {ant_load = ant_load}}, CreateIOThread)(linda, pkg_dir, sand_box_dir)
if not io_thread then
    assert(false, "lanes error: ".. lanes_err)
end

--send package to io
function SendIORequest(pkg)
    linda:send("request", pkg)
end

--register io call back function
IoCommand_name = {"run", "screenshot_req"}

IoCommand_func = {}
IoCommand_func["run"] = function(value) run(value) end

local bgfx = require "bgfx"
local screenshot_cache_num = 0
IoCommand_func["screenshot_req"] = function(value)
    if entrance then
        bgfx.request_screenshot()
        screenshot_cache_num = screenshot_cache_num + 1
        print("request screenshot: " .. value[2] .. " num: " .. screenshot_cache_num)
    end
end

function RegisterIOCommand(cmd, func)
    table.insert(IoCommand_name, cmd)
    IoCommand_func[cmd] = func
    --register command in io
    linda:send("RegisterTransmit", cmd)
end

