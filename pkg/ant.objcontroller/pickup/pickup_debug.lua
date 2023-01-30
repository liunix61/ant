local ecs   = ...
local world = ecs.world
local w     = world.w

local math3d = require "math3d"

local renderpkg = import_package "ant.render"
local fbmgr     = renderpkg.fbmgr

local mathpkg	= import_package "ant.math"
local mu		= mathpkg.util

local ientity   = ecs.import.interface "ant.render|ientity"
local imaterial = ecs.import.interface "ant.asset|imaterial"

local frustum_entity
local function create_frustum_entity()
	local pq = w:first("pickup_queue camera_ref:in")
	local camera <close> = w:entity(pq.camera_ref, "camera:in")
	local points = math3d.frustum_points(camera.camera.viewprojmat)
	return ientity.create_frustum_entity(points, "pickup_frustum")
end

local pickup_debug_sys = ecs.system "pickup_debug_system"

local function create_view_buffer_entity()
	local m = ientity.quad_mesh(mu.texture_uv{x=0,y=0,w=120, h=120})
	m.ib.owned, m.vb.owned = true, true
	ecs.create_entity{
		policy = {
			"ant.render|simplerender",
			"ant.general|name",
		},
		data = {
			name = "pick_buffer_entity",
			owned_mesh_buffer = true,
			simplemesh = m,
			material = "/pkg/ant.resources/materials/texquad.material",
			visible_state = "main_view",
			render_layer = "translcuent",
			scene = {},
			on_ready = function (e)
				local pq = w:first("pickup_queue render_target:in")
				local rt = pq.render_target
				imaterial.set_property(e, "s_tex", fbmgr.get_rb(rt.fb_idx, 1).handle)
			end,
		}
	}
end

function pickup_debug_sys:init()
    create_view_buffer_entity()
end

local function log_pickup_queue_entities()
	log.info "pickup queue entities:"
	local entities = {}
	for e in w:select("pickup_queue_visible eid:in name?in") do
		entities[#entities+1] = ("%d-%s"):format(e.eid, e.name or "")
	end

	log.info(table.concat(entities, "\t"))
end


local mousemb = world:sub{"mouse"}

function pickup_debug_sys:data_changed()
    for _, btn, state in mousemb:unpack() do
        if btn == "LEFT" and state == "DOWN" then
            if frustum_entity then
                w:remove(frustum_entity)
                frustum_entity = nil
            end
        end
    end

	log_pickup_queue_entities()
end

function pickup_debug_sys:camera_usage()
    if frustum_entity == nil then
        frustum_entity = create_frustum_entity()
    end
end
