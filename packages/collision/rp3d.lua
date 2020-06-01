local lib = require "rp3d.core"
local math3d_adapter = require "math3d.adapter"

local function shape_interning()
	local shape = lib.shape
	local all_shapes = {}
	local function all_shapes_gc(self)
		for key , shape in pairs(all_shapes) do
			all_shapes[key] = nil
			local what = (key:match "[^:]*") or key
			lib.destroy_shape[what](shape)
		end
	end
	setmetatable(all_shapes, { __gc = all_shapes_gc })

	local world_mt = lib.collision_world_mt

	local heightfield_idx = 1

	function world_mt:new_shape(typename, ...)
		local value
		if typename ~= "heightfield" then
			-- interning shapes
			local key = table.concat ({ typename, ... } , ":")
			value = all_shapes[key]
			if value then
				return value
			end
			value = shape[typename](...)
			all_shapes[key] = value
		else
			local function generate_heightfield_name()
				local name = "__heightfield"
				while true do
					if all_shapes[name] == nil then
						break
					end
					name = name .. heightfield_idx
					heightfield_idx = heightfield_idx + 1
				end
				return name
			end
			value = shape.heightfield(...)
			local key = generate_heightfield_name()
			all_shapes[key] = value
		end
		return value
	end
end

function lib.init(logger)
	if logger then
		lib.logger(logger)
	end
	shape_interning()
	local world_mt = lib.collision_world_mt

	world_mt.__index = world_mt

	world_mt.body_create 	= math3d_adapter.format(world_mt.body_create, "vq", 2)
	world_mt.set_transform 	= math3d_adapter.format(world_mt.set_transform, "vq", 3)
	world_mt.get_aabb 		= math3d_adapter.getter(world_mt.get_aabb, "vv")
	world_mt.add_shape 		= math3d_adapter.vector(world_mt.add_shape, 4)

	lib.shape.heightfield	= math3d_adapter.vector(lib.shape.heightfield, 7)

	local rayfilter 		= math3d_adapter.vector(lib.rayfilter, 1)
	local raycast 			= math3d_adapter.getter(world_mt.raycast, "vv")

	function world_mt.raycast(world, p0, p1, maskbits)
		p0,p1 = rayfilter(p0,p1)
		local hit, pos, norm = raycast(world, p0, p1, maskbits or 0)
		if hit then
			return pos, norm
		end
	end
end

return lib
