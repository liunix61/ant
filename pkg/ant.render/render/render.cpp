#include "ecs/world.h"
#include "ecs/select.h"
#include "ecs/component.hpp"

extern "C"{
	#include "math3d.h"
	#include "math3dfunc.h"
	#include "material.h"

	#include "render_material.h"
}

#include "lua.hpp"
#include "luabgfx.h"
#include <bgfx/c99/bgfx.h>
#include <cstdint>
#include <cassert>
#include <array>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory.h>
#include <string.h>
#include <algorithm>

struct transform {
	uint32_t tid;
	uint32_t stride;
};

enum queue_type{
	qt_mat_def			= 0,
	qt_mat_pre_depth,
	qt_mat_pickup,
	qt_mat_csm,
	qt_mat_lightmap,
	qt_mat_outline,
	qt_mat_velocity,
	qt_count,
};

static constexpr uint8_t MAX_VISIBLE_QUEUE = 64;

using obj_transforms = std::unordered_map<const ecs::render_object*, transform>;
static inline transform
update_transform(struct ecs_world* w, const ecs::render_object *ro, obj_transforms &trans){
	auto it = trans.find(ro);
	if (it == trans.end()){
		const math_t wm = ro->worldmat;
		assert(math_valid(w->math3d->M, wm) && !math_isnull(wm) && "Invalid world mat");
		const float * v = math_value(w->math3d->M, wm);
		const int num = math_size(w->math3d->M, wm);
		transform t;
		bgfx_transform_t bt;
		t.tid = w->bgfx->encoder_alloc_transform(w->holder->encoder, &bt, (uint16_t)num);
		t.stride = num;
		memcpy(bt.data, v, sizeof(float)*16*num);
		it = trans.insert(std::make_pair(ro, t)).first;
	}

	return it->second;
}

#define INVALID_BUFFER_TYPE		UINT16_MAX
#define BUFFER_TYPE(_HANDLE)	(_HANDLE >> 16) & 0xffff

static inline bool is_indirect_draw(const ecs::render_object *ro){
	return ro->draw_num != 0 && ro->draw_num != UINT32_MAX;
}
static bool
mesh_submit(struct ecs_world* w, const ecs::render_object* ro, int vid, uint8_t mat_idx){
	if (ro->vb_num == 0 || ro->draw_num == 0)
		return false;

	const uint16_t ibtype = BUFFER_TYPE(ro->ib_handle);
	if (ibtype != INVALID_BUFFER_TYPE && ro->ib_num == 0)
		return false;

	const uint16_t vb_type = BUFFER_TYPE(ro->vb_handle);
	
	switch (vb_type){
		case BGFX_HANDLE_VERTEX_BUFFER:	w->bgfx->encoder_set_vertex_buffer(w->holder->encoder, 0, bgfx_vertex_buffer_handle_t{(uint16_t)ro->vb_handle}, ro->vb_start, ro->vb_num); break;
		case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER_TYPELESS:	//walk through
		case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER: w->bgfx->encoder_set_dynamic_vertex_buffer(w->holder->encoder, 0, bgfx_dynamic_vertex_buffer_handle_t{(uint16_t)ro->vb_handle}, ro->vb_start, ro->vb_num); break;
		default: assert(false && "Invalid vertex buffer type");
	}

	const uint16_t vb2_type = BUFFER_TYPE(ro->vb2_handle);
	if((vb2_type != INVALID_BUFFER_TYPE) && (mat_idx == qt_mat_def) || (mat_idx == qt_mat_lightmap)){
		switch (vb2_type){
			case BGFX_HANDLE_VERTEX_BUFFER:	w->bgfx->encoder_set_vertex_buffer(w->holder->encoder, 1, bgfx_vertex_buffer_handle_t{(uint16_t)ro->vb2_handle}, ro->vb2_start, ro->vb2_num); break;
			case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER_TYPELESS:	//walk through
			case BGFX_HANDLE_DYNAMIC_VERTEX_BUFFER: w->bgfx->encoder_set_dynamic_vertex_buffer(w->holder->encoder, 1, bgfx_dynamic_vertex_buffer_handle_t{(uint16_t)ro->vb2_handle}, ro->vb2_start, ro->vb2_num); break;
			default: assert(false && "Invalid vertex buffer type");
		}
	}

	if (ro->ib_num > 0){
		switch (ibtype){
			case BGFX_HANDLE_INDEX_BUFFER: w->bgfx->encoder_set_index_buffer(w->holder->encoder, bgfx_index_buffer_handle_t{(uint16_t)ro->ib_handle}, ro->ib_start, ro->ib_num); break;
			case BGFX_HANDLE_DYNAMIC_INDEX_BUFFER:	//walk through
			case BGFX_HANDLE_DYNAMIC_INDEX_BUFFER_32: w->bgfx->encoder_set_dynamic_index_buffer(w->holder->encoder, bgfx_dynamic_index_buffer_handle_t{(uint16_t)ro->ib_handle}, ro->ib_start, ro->ib_num); break;
			default: assert(false && "Unknown index buffer type"); break;
		}
	}

	if(is_indirect_draw(ro)){
		const auto itb = bgfx_dynamic_vertex_buffer_handle_t{(uint16_t)ro->itb_handle};
		assert(BGFX_HANDLE_IS_VALID(itb));
		w->bgfx->encoder_set_instance_data_from_dynamic_vertex_buffer(w->holder->encoder, itb, 0, ro->draw_num);
	}
	return true;
}

static inline struct material_instance*
get_material(struct render_material * R, const ecs::render_object* ro, size_t midx){
	assert(midx < qt_count);
	//TODO: get all the materials by mask
	const uint64_t mask = 1ull << midx;
	void* mat[64] = {nullptr};
	render_material_fetch(R, ro->rm_idx, mask, mat);
	return (struct material_instance*)(mat[midx]);
}

using matrix_array = std::vector<math_t>;
using group_matrices = std::unordered_map<int, matrix_array>;
struct obj_data {
	const ecs::render_object* obj;
	const matrix_array* mats;
#if defined(_MSC_VER) && defined(_DEBUG)
	uint64_t id;
#endif
};

using objarray = std::vector<obj_data>;

static inline transform
update_hitch_transform(struct ecs_world *w, const ecs::render_object *ro, const matrix_array& worldmats, obj_transforms &trans_cache){
	auto it = trans_cache.find(ro);
	if (trans_cache.end() == it){
		transform t;
		t.stride = math_size(w->math3d->M, ro->worldmat);
		const auto nummat = worldmats.size();
		const auto num = nummat * t.stride;
		bgfx_transform_t trans;
		t.tid = w->bgfx->encoder_alloc_transform(w->holder->encoder, &trans, (uint16_t)num);
		for (size_t i=0; i<nummat; ++i){
			math_t r = math_ref(w->math3d->M, trans.data+i*t.stride*16, MATH_TYPE_MAT, t.stride);
			math3d_mul_matrix_array(w->math3d->M, worldmats[i], ro->worldmat, r);
		}

		it = trans_cache.insert(std::make_pair(ro, t)).first;
	}

	return it->second;
}

static inline void
submit_draw(struct ecs_world*w, bgfx_view_id_t viewid, const ecs::render_object *obj, bgfx_program_handle_t prog, uint8_t discardflags){
	if(is_indirect_draw(obj)){
		const auto idb = bgfx_indirect_buffer_handle_t{(uint16_t)obj->idb_handle};
		assert(BGFX_HANDLE_IS_VALID(idb));
		w->bgfx->encoder_submit_indirect(w->holder->encoder, viewid, prog, idb, 0, obj->draw_num, obj->render_layer, discardflags);
	}else{
		w->bgfx->encoder_submit(w->holder->encoder, viewid, prog, obj->render_layer, discardflags);
	}
}

static inline void
draw_obj(lua_State *L, struct ecs_world *w, const ecs::render_args* ra, const ecs::render_object *obj, const matrix_array *mats, obj_transforms &trans){
	auto mi = get_material(w->R, obj, ra->material_index);
	if (mi && mesh_submit(w, obj, ra->viewid, ra->material_index)) {
		apply_material_instance(L, mi, w);
		const auto prog = material_prog(L, mi);
		transform t;
		if (mats){
			t = update_hitch_transform(w, obj, *mats, trans);
			for (int i=0; i<mats->size()-1; ++i) {
				w->bgfx->encoder_set_transform_cached(w->holder->encoder, t.tid, t.stride);
				submit_draw(w, ra->viewid, obj, prog, BGFX_DISCARD_TRANSFORM);
				t.tid += t.stride;
			}
		} else {
			t = update_transform(w, obj, trans);
		}

		w->bgfx->encoder_set_transform_cached(w->holder->encoder, t.tid, t.stride);
		submit_draw(w, ra->viewid, obj, prog, BGFX_DISCARD_ALL);
	}
}

static inline void
add_obj(struct ecs_world* w, cid_t main_id, int index, const ecs::render_object* obj, const matrix_array* mats, objarray &objs){
#if defined(_MSC_VER) && defined(_DEBUG)
	ecs::eid id = (ecs::eid)entity_sibling(w->ecs, main_id, index, ecs_api::component<ecs::eid>::id);
	objs.emplace_back(obj_data{ obj, mats, id });
#else
	(void)main_id; (void) index;
	objs.emplace_back(obj_data{ obj, mats });
#endif
}

struct submit_cache{
	obj_transforms	transforms;

	//TODO: need more fine control of the cache
	group_matrices	groups;

	const ecs::render_args* ra[MAX_VISIBLE_QUEUE] = {0};
	uint8_t ra_count = 0;

	void clear(){
		transforms.clear();
		groups.clear();

		ra_count = 0;

#ifdef _DEBUG
		memset(ra, 0xdeaddead, sizeof(ra));
#endif //_DEBUG
	}
};

static inline void
draw_objs(lua_State *L, struct ecs_world *w, cid_t main_id, int index, const matrix_array *mats, submit_cache &cc){
	const ecs::render_object* obj = (const ecs::render_object*)entity_sibling(w->ecs, main_id, index, ecs_api::component<ecs::render_object>::id);
	if (obj){
		#ifdef _DEBUG
		const ecs::eid id = (ecs::eid)entity_sibling(w->ecs, main_id, index, ecs_api::component<ecs::eid>::id);
		#endif //_DEBUG
		for (uint8_t ii=0; ii<cc.ra_count; ++ii){
			const auto ra = cc.ra[ii];
			if (0 != (obj->visible_masks & ra->queue_mask) && 
				(0 == (obj->cull_masks & ra->queue_mask))){
				draw_obj(L, w, cc.ra[ii], obj, mats, cc.transforms);
			}
		}
	}
}

static inline void
find_render_args(struct ecs_world *w, submit_cache &cc) {
	for (auto a : ecs_api::select<ecs::render_args>(w->ecs)){
		auto& r = a.get<ecs::render_args>();
		cc.ra[cc.ra_count++] = &r;
	}
}

static int
lsubmit(lua_State *L) {
	auto w = getworld(L);

	static submit_cache cc;

	for (auto e : ecs_api::select<ecs::view_visible, ecs::hitch, ecs::scene>(w->ecs)){
		const auto &h = e.get<ecs::hitch>();
		const auto &s = e.get<ecs::scene>();
		if (h.group != 0){
			cc.groups[h.group].push_back(s.worldmat);
		}
	}

	find_render_args(w, cc);
	
	const cid_t vv_id = ecs_api::component<ecs::view_visible>::id;
	for (int i=0; entity_iter(w->ecs, vv_id, i); ++i){
		draw_objs(L, w, vv_id, i, nullptr, cc);
	}
	for (auto const& [groupid, mats] : cc.groups) {
		int gids[] = {groupid};
		ecs_api::group_enable<ecs::hitch_tag>(w->ecs, gids);
		const cid_t h_id = ecs_api::component<ecs::hitch_tag>::id;
		for (int i=0; entity_iter(w->ecs, h_id, i); ++i){
			draw_objs(L, w, h_id, i, &mats, cc);
		}
	}

	cc.clear();
	return 0;
}

static int
lnull(lua_State *L){
	lua_pushlightuserdata(L, nullptr);
	return 1;
}

static int
lrm_dealloc(lua_State *L){
	auto w = getworld(L);
	const int index = (int)luaL_checkinteger(L, 1);
	render_material_dealloc(w->R, index);
	return 0;
}

static int
lrm_alloc(lua_State *L){
	auto w = getworld(L);
	lua_pushinteger(L, render_material_alloc(w->R));
	return 1;
}

static int
lrm_set(lua_State *L){
	auto w = getworld(L);
	const int index = (int)luaL_checkinteger(L, 1);
	if (index < 0){
		return luaL_error(L, "Invalid index:%d", index);
	}
	const int type = (int)luaL_checkinteger(L, 2);
	if (type < 0 || type > RENDER_MATERIAL_TYPE_MAX){
		luaL_error(L, "Invalid render_material type: %d, should be : 0 <= type <= %d", type, RENDER_MATERIAL_TYPE_MAX);
	}

	const int valuetype = lua_type(L, 3);
	if (valuetype != LUA_TLIGHTUSERDATA && valuetype != LUA_TNIL){
		luaL_error(L, "Set render_material material type should be lightuserdata:%s", lua_typename(L, 3));
	}
	const auto m = lua_touserdata(L, 3);

	render_material_set(w->R, index, type, m);
	return 0;
}

extern "C" int
luaopen_render_material(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "null",		lnull},
		{ "dealloc",	lrm_dealloc},
		{ "alloc",		lrm_alloc},
		{ "set",		lrm_set},

		{ nullptr, 		nullptr },
	};
	luaL_newlibtable(L,l);
	lua_pushnil(L);
	luaL_setfuncs(L,l,1);
	return 1;
}

static int
linit(lua_State *L){
	auto w = getworld(L);
	w->R = render_material_create();
	return 1;
}

static int
lexit(lua_State *L){
	auto w = getworld(L);
	render_material_release(w->R);
	w->R = nullptr;
	return 0;
}

extern "C" int
luaopen_system_render(lua_State *L){
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "init",			linit},
		{ "exit",			lexit},
		{ "render_submit", 	lsubmit},
		{ nullptr, 			nullptr },
	};
	luaL_newlibtable(L,l);
	lua_pushnil(L);
	luaL_setfuncs(L,l,1);
	return 1;
}