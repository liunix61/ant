#include "hierarchy.h"
#include "meshbase/meshbase.h"

extern "C" {
#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
}

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/skeleton.h>

#include <ozz/geometry/runtime/skinning_job.h>
#include <ozz/base/platform.h>

#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/soa_float4x4.h>

#include <ozz/base/memory/allocator.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/math_ex.h>

#include <../samples/framework/mesh.h>

// glm
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// stl
#include <string>
#include <cstring>
#include <algorithm>
#include <sstream>

struct animation_node {
	ozz::animation::Animation		*ani;	
};

struct sampling_node {
	ozz::animation::SamplingCache *		cache;	
};

struct aligned_memory{
	void* ptr;
};

struct joint_remap{
	ozz::Vector<uint16_t>::Std joints;
};

template<typename DataType>
struct vertex_data {
	struct data_stride {
		typedef DataType Type;
		DataType* data;
		uint32_t offset;
		uint32_t stride;
	};

	data_stride positions;
	data_stride normals;
	data_stride tangents;
};

struct in_vertex_data : public vertex_data<const void>{
	data_stride joint_weights;
	data_stride joint_indices;
};

typedef vertex_data<void> out_vertex_data;

template<typename DataStride>
static void
read_data_stride(lua_State *L, const char* name, int index, DataStride &ds){
	const int type = lua_getfield(L, index, name);
	if (type != LUA_TNIL)
	{
		lua_geti(L, -1, 1);
		const int type = lua_type(L, -1);
		switch (type){
		case LUA_TSTRING: ds.data = (typename DataStride::Type*)lua_tostring(L, -1); break;
		case LUA_TLIGHTUSERDATA:
		case LUA_TUSERDATA: ds.data = (typename DataStride::Type*)lua_touserdata(L, -1); break;
		default:
			luaL_error(L, "not support data type in data stride, only string and userdata is support, type:%d", type);
			return;
		}
		lua_pop(L, 1);

		lua_geti(L, -1, 2);
		ds.offset = (uint32_t)lua_tointeger(L, -1) - 1;
		lua_pop(L, 1);

		lua_geti(L, -1, 3);
		ds.stride = (uint32_t)lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

template<typename DataType>
static void
read_vertex_data(lua_State* L, int index, vertex_data<DataType>& vd) {
	read_data_stride(L, "POSITION", index, vd.positions);
	read_data_stride(L, "NORMAL", index, vd.normals);
	read_data_stride(L, "TANGENT", index, vd.tangents);
}

static void
read_in_vertex_data(lua_State *L, int index, in_vertex_data &vd){
	read_vertex_data(L, index, vd);
	read_data_stride(L, "WEIGHT", index, vd.joint_weights);
	read_data_stride(L, "INDICES", index, vd.joint_indices);
}

template<typename T, typename DataT>
static void
fill_skinning_job_field(uint32_t num_vertices, const DataT &d, ozz::Range<T> &r, size_t &stride) {
	const uint8_t* begin_data = (const uint8_t*)d.data + d.offset;
	r.begin = (T*)(begin_data);
	r.end	= (T*)(begin_data + d.stride * num_vertices);
	stride = d.stride;
}

static void
build_skinning_matrices(bind_pose *skinning_matrices, 
	const bind_pose* current_pose, 
	const bind_pose* inverse_bind_matrices, 
	const joint_remap *jarray){
	if (jarray){
		assert(jarray->joints.size() == inverse_bind_matrices->pose.size());
		for (size_t ii = 0; ii < jarray->joints.size(); ++ii){
			skinning_matrices->pose[ii] = current_pose->pose[jarray->joints[ii]] * inverse_bind_matrices->pose[ii];
		}
	} else {
		assert(skinning_matrices->pose.size() == inverse_bind_matrices->pose.size() &&
			skinning_matrices->pose.size() == current_pose->pose.size());
		for (size_t ii = 0; ii < inverse_bind_matrices->pose.size(); ++ii){
			skinning_matrices->pose[ii] = current_pose->pose[ii] * inverse_bind_matrices->pose[ii];
		}
	}
}

static int
lbuild_skinning_matrices(lua_State *L){
	luaL_checkudata(L, 1, "OZZ_BIND_POSE");
	auto skinning_matrices = (bind_pose*)lua_touserdata(L, 1);

	luaL_checkudata(L, 2, "OZZ_BIND_POSE");
	auto current_bind_pose = (bind_pose*)lua_touserdata(L, 2);

	luaL_checkudata(L, 3, "OZZ_BIND_POSE");
	auto inverse_bind_matrices = (bind_pose*)lua_touserdata(L, 3);

	const joint_remap *jarray = lua_isnoneornil(L, 4) ? nullptr : (const joint_remap*)lua_touserdata(L, 4);

	if (skinning_matrices->pose.size() < inverse_bind_matrices->pose.size()){
		return luaL_error(L, "invalid skinning matrices and inverse bind matrices, skinning matrices must larger than inverse bind matrices");
	}

	build_skinning_matrices(skinning_matrices, current_bind_pose, inverse_bind_matrices, jarray);
	return 0;
}

static int
lmesh_skinning(lua_State *L){
	luaL_checkudata(L, 1, "OZZ_BIND_POSE");
	bind_pose *skinning_matrices = (bind_pose*)lua_touserdata(L, 1);

	luaL_checktype(L, 2, LUA_TTABLE);
	in_vertex_data vd = {0};
	read_in_vertex_data(L, 2, vd);

	luaL_checktype(L, 3, LUA_TTABLE);
	out_vertex_data ovd = {0};
	read_vertex_data(L, 3, ovd);

	luaL_checktype(L, 4, LUA_TNUMBER);
	const uint32_t num_vertices = (uint32_t)lua_tointeger(L, 4);

	const uint32_t influences_count = (uint32_t)luaL_optinteger(L, 5, 4);

	ozz::geometry::SkinningJob skinning_job;
	skinning_job.vertex_count = num_vertices;
	skinning_job.influences_count = influences_count;
	skinning_job.joint_matrices = ozz::make_range(skinning_matrices->pose);
	
	assert(vd.positions.data && "skinning system must provide 'position' attribute");

	fill_skinning_job_field(num_vertices, vd.positions, skinning_job.in_positions, skinning_job.in_positions_stride);
	fill_skinning_job_field(num_vertices, ovd.positions, skinning_job.out_positions, skinning_job.out_positions_stride);

	if (vd.normals.data) {
		fill_skinning_job_field(num_vertices, vd.normals, skinning_job.in_normals, skinning_job.in_normals_stride);
	}

	if (ovd.normals.data) {
		fill_skinning_job_field(num_vertices, ovd.normals, skinning_job.out_normals, skinning_job.out_normals_stride);
	}
	
	if (vd.tangents.data) {
		fill_skinning_job_field(num_vertices, vd.tangents, skinning_job.in_tangents, skinning_job.in_tangents_stride);
	}

	if (ovd.tangents.data) {
		fill_skinning_job_field(num_vertices, ovd.tangents, skinning_job.out_tangents, skinning_job.out_tangents_stride);
	}

	if (influences_count > 1) {
		assert(vd.joint_weights.data && "joint weight data is not valid!");
		fill_skinning_job_field(num_vertices, vd.joint_weights, skinning_job.joint_weights, skinning_job.joint_weights_stride);
	}
		
	assert(vd.joint_indices.data && "skinning job must provide 'indices' attribute");
	fill_skinning_job_field(num_vertices, vd.joint_indices, skinning_job.joint_indices, skinning_job.joint_indices_stride);

	if (!skinning_job.Run()) {
		luaL_error(L, "running skinning failed!");
	}

	return 0;
}

static inline ozz::animation::Skeleton*
get_ske(lua_State *L, int idx = 1) {
	luaL_checktype(L, idx, LUA_TUSERDATA);
	hierarchy_build_data *builddata = (hierarchy_build_data *)lua_touserdata(L, 1);

	auto ske = builddata->skeleton;
	if (ske == nullptr) {
		luaL_error(L, "skeleton is not init!");
	}

	return ske;
}

static inline animation_node*
get_aninode(lua_State *L, int idx = 2) {
	luaL_checktype(L, idx, LUA_TUSERDATA);
	animation_node * aninode = (animation_node*)lua_touserdata(L, 2);
	if (aninode->ani == nullptr) {
		luaL_error(L, "animation is not init!");
		return 0;
	}

	return aninode;
}

static inline float
get_ratio(lua_State*L, int idx = 4) {
	luaL_checktype(L, idx, LUA_TNUMBER);
	return (float)lua_tonumber(L, idx);
}

static inline bind_pose*
get_aniresult(lua_State *L, ozz::animation::Skeleton* ske, int idx) {
	luaL_checktype(L, idx, LUA_TUSERDATA);
	bind_pose* result = (bind_pose*) lua_touserdata(L, idx);
	if (result->pose.size() != (size_t)ske->num_joints()) {
		luaL_error(L, "animation result joint count:%d, is not equal to skeleton joint number: %d", result->pose.size(), ske->num_joints());
	}

	return result;
}

static inline bind_pose_soa*
get_bindpose(lua_State *L, int idx) {
	luaL_checktype(L, idx, LUA_TUSERDATA);
	return (bind_pose_soa*)lua_touserdata(L, idx);
}

struct sample_info {
	animation_node *aninode;
	sampling_node *sampling;
	float ratio;
	float weight;
};


static inline bool
do_sample(const ozz::animation::Skeleton *ske, 
			const sample_info &si, bind_pose_soa &result) {
	ozz::animation::SamplingJob job;
	job.animation = si.aninode->ani;
	job.cache = si.sampling->cache;
	job.ratio = si.ratio;
	job.output = ozz::make_range(result.pose);

	return job.Run();
}

bool
do_ltm(ozz::animation::Skeleton *ske, 
	const bind_pose_soa::bind_pose_type &intermediateResult, 
	bind_pose::bind_pose_type &joints,
	const ozz::math::Float4x4 *root = nullptr,
	int from = ozz::animation::Skeleton::kNoParent,
	int to = ozz::animation::Skeleton::kMaxJoints) {
	ozz::animation::LocalToModelJob ltmjob;
	ltmjob.root = root;
	ltmjob.input = ozz::make_range(intermediateResult);
	ltmjob.skeleton = ske;
	ltmjob.output = ozz::make_range(joints);

	return ltmjob.Run();
}

struct blendlayers {
	ozz::Vector<ozz::animation::BlendingJob::Layer>::Std layers;
	ozz::Vector<bind_pose_soa>::Std results;
};

static inline void
load_sample_info(lua_State *L, int index, const ozz::animation::Skeleton* ske, sample_info &si) {
	luaL_checktype(L, index, LUA_TTABLE);

	lua_getfield(L, index, "handle");
	si.aninode = (animation_node*)lua_touserdata(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, index, "sampling_cache");
	auto sampling = (sampling_node*)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (ske->num_joints() > sampling->cache->max_tracks()){
		sampling->cache->Resize(ske->num_joints());
	}
	si.sampling = sampling;

	lua_getfield(L, index, "ratio");
	si.ratio = (float)lua_tonumber(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, -1, "weight");
	si.weight = (float)lua_tonumber(L, -1);
	lua_pop(L, 1);
}

static inline bool
sample_animation(const ozz::animation::Skeleton *ske, const sample_info &si, bind_pose_soa *bindpose) {
	bindpose->pose.resize(ske->num_soa_joints());
	//assert(ske->joint_parents()[0] == ozz::animation::Skeleton::kNoParent);
	return do_sample(ske, si, *bindpose);
}

static int
lsample_animation(lua_State *L) {
	auto ske = get_ske(L, 1);	
	sample_info si;
	load_sample_info(L, 2, ske, si);
	bind_pose_soa *bindpose = (bind_pose_soa*)lua_touserdata(L, 3);

	if (!sample_animation(ske, si, bindpose)) {
		luaL_error(L, "sampling animation failed");
	}
	return 0;
}

static inline int
find_root_index(const ozz::animation::Skeleton *ske) {
	const auto jointcount = ske->num_joints();
	const auto &parents = ske->joint_parents();
	for (auto ii = 0; ii < jointcount; ++ii) {
		auto &parent = parents[ii];
		if (parent == ozz::animation::Skeleton::kNoParent)
			return ii;
	}

	return -1;
}

static inline void
fetch_float4x4(const ozz::math::SoaTransform &trans, int subidx, ozz::math::Float4x4 &f4x4) {
	const ozz::math::SoaFloat4x4 local_soa_matrices = ozz::math::SoaFloat4x4::FromAffine(
		trans.translation, trans.rotation, trans.scale);

	// Converts to aos matrices.
	ozz::math::Float4x4 local_aos_matrices[4];
	ozz::math::Transpose16x16(&local_soa_matrices.cols[0].x,
		local_aos_matrices->cols);

	f4x4 = local_aos_matrices[subidx];
}

static inline void
fix_root_translation(ozz::animation::Skeleton *ske, bind_pose_soa::bind_pose_type &pose){
	auto rootidx = find_root_index(ske);
	const auto soa_rootidx = rootidx / 4;

	auto& trans = pose[soa_rootidx];
	const auto newtrans = ozz::math::simd_float4::zero();
	trans.translation.x = ozz::math::SetI(trans.translation.x, newtrans, 0);
	trans.translation.z = ozz::math::SetI(trans.translation.z, newtrans, 0);
}

static inline bool
transform_bindpose(ozz::animation::Skeleton *ske, 
	bind_pose_soa::bind_pose_type &pose,
	bind_pose::bind_pose_type& resultpose,
	bool fixroot){
	if (fixroot)
		fix_root_translation(ske, pose);
	
	return do_ltm(ske, pose, resultpose);
}

static int
ltransform_to_bindpose_result(lua_State *L) {
	auto ske = get_ske(L, 1);
	auto bindpose = get_bindpose(L, 2);
	auto result = get_aniresult(L, ske, 3);
	auto fixroot = lua_isnoneornil(L, 4) ? false : lua_toboolean(L, 4);

	if (!transform_bindpose(ske, bindpose->pose, result->pose, fixroot)) {
		luaL_error(L, "transform bind pose is failed!");
	}
	return 0;
}

static void
create_blend_layers(lua_State *L, int index, 
	const ozz::animation::Skeleton *ske, 
	blendlayers &bl) {
	const int numani = (int)lua_rawlen(L, index);

	bl.layers.resize(numani);
	bl.results.resize(numani);

	auto& layers = bl.layers;
	auto& results = bl.results;

	for (int ii = 0; ii < numani; ++ii) {
		lua_geti(L, index, ii + 1);

		sample_info si;
		load_sample_info(L, -1, ske, si);

		auto &result = results[ii];
		if (!sample_animation(ske, si, &result)) {
			luaL_error(L, "sampling animation failed!");
		}

		layers[ii].weight = si.weight;
		layers[ii].transform = ozz::make_range(result.pose);

		lua_pop(L, 1);
	}
}

static inline bool
do_blend(const ozz::animation::Skeleton *ske, 
	const ozz::Vector<ozz::animation::BlendingJob::Layer>::Std &layers, 
	const char* blendtype, 
	float threshold, 
	bind_pose_soa *finalpose) {
	ozz::animation::BlendingJob blendjob;
	blendjob.bind_pose = ske->joint_bind_poses();

	auto jobrange = ozz::make_range(layers);
	if (strcmp(blendtype, "blend") == 0) {
		blendjob.layers = jobrange;
	} else if (strcmp(blendtype, "additive") == 0) {
		blendjob.additive_layers = jobrange;
	} else {
		return false;
	}

	blendjob.threshold = threshold;
	blendjob.output = ozz::make_range(finalpose->pose);

	return blendjob.Run();
}

static bool
blend_animations(lua_State* L,
	int ani_index,
	const char* blendtype, const ozz::animation::Skeleton* ske, float threshold,
	bind_pose_soa* bindpose) {

	blendlayers bl;
	create_blend_layers(L, ani_index, ske, bl);

	if (bl.layers.empty()) {
		return true;
	}

	if (bl.layers.size() > 1) {
		bindpose->pose.resize(ske->num_soa_joints());
		do_blend(ske, bl.layers, blendtype, threshold, bindpose);
	}
	else {
		auto& result = bl.results.back();
		bindpose->pose = std::move(result.pose);
	}

	return true;
}

static int
lblend_animations(lua_State* L) {
	auto ske = get_ske(L, 1);
	const char* blendtype = lua_tostring(L, 3);
	auto bindpose = get_bindpose(L, 4);

	const float threshold = (float)luaL_optnumber(L, 5, 0.1f);

	blend_animations(L, 2, blendtype, ske, threshold, bindpose);

	assert(bindpose->pose.size() == ske->joint_bind_poses().count());

	return 0;
}

static bool
blend_bind_poses(lua_State *L, int idx, const char* blendtype, const ozz::animation::Skeleton *ske, float threshold, bind_pose_soa*bindpose) {
	int numposes = (int)lua_rawlen(L, idx);
	blendlayers bl;
	bl.layers.resize(numposes);
	bl.results.resize(numposes);
	std::vector<bind_pose_soa> poseset(numposes);
	for (int ii = 0; ii < numposes; ++ii) {
		lua_geti(L, idx, ii+1);
		{
			luaL_checktype(L, -1, LUA_TTABLE);
			blend_animations(L, lua_absindex(L, -1), blendtype, ske, threshold, &poseset[ii]);

			lua_getfield(L, -1, "weight");
			const float weight = (float)lua_tonumber(L, -1);
			lua_pop(L, 1);

			bl.layers[ii].weight = weight;
			bl.layers[ii].transform = ozz::make_range(poseset[ii].pose);
		}
		lua_pop(L, 1);
	}
	bindpose->pose.resize(ske->num_soa_joints());
	do_blend(ske, bl.layers, blendtype, threshold, bindpose);
	return true;
}

static int
lmotion(lua_State *L) {
	auto ske = get_ske(L, 1);	
	luaL_checktype(L, 2, LUA_TTABLE);
	luaL_checktype(L, 3, LUA_TSTRING);
	const char* blendtype = lua_tostring(L, 3);
	auto aniresult = get_aniresult(L, ske, 4);
	const float threshold = (float)luaL_optnumber(L, 5, 0.1f);
	const bool fixroot = lua_isnoneornil(L, 6) ? false : lua_toboolean(L, 6);
 
	bind_pose_soa bindpose;
	int numposes = (int)lua_rawlen(L, 2);
	if (numposes == 1) {
		lua_geti(L, 2, 1);
		luaL_checktype(L, -1, LUA_TTABLE);
		blend_animations(L, lua_absindex(L, -1), blendtype, ske, threshold, &bindpose);
	}
	else if (numposes > 1) {
		blend_bind_poses(L, 2, blendtype, ske, threshold, &bindpose);
	}
	else {
		return luaL_error(L, "pose cannot be empty.");
	}
	if (!transform_bindpose(ske, bindpose.pose, aniresult->pose, fixroot)){
		return luaL_error(L, "doing blend result to ltm job failed!");
	}
	return 0;
}

static inline void
create_joint_table(lua_State *L, const ozz::math::Float4x4 &joint) {
	lua_createtable(L, 16, 0);
	for (auto icol = 0; icol < 4; ++icol) {
		for (auto ii = 0; ii < 4; ++ii) {
			const float* col = (const float*)(&(joint.cols[icol]));
			lua_pushnumber(L, col[ii]);
			lua_seti(L, -2, icol * 4 + ii + 1);
		}
	}
}

static int
lbp_result_init(lua_State *L){
	luaL_checktype(L, 1, LUA_TSTRING);
	size_t size;
	const char* buffer = lua_tolstring(L, 1, &size);
	float* dstbuffer = (float*)lua_touserdata(L, 2);

	memcpy(dstbuffer, buffer, size);
	return 0;
}

static int
lbp_result_joint(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	bind_pose * result = (bind_pose*)lua_touserdata(L, 1);

	luaL_checktype(L, 2, LUA_TNUMBER);
	const size_t idx = (size_t)lua_tointeger(L, 2) - 1;
	const auto joint_count = result->pose.size();

	if (idx >= joint_count) {
		luaL_error(L, "invalid index:%d, joints count:%d", idx, joint_count);
	}

	auto &joint = result->pose[idx];
	
	if (lua_isnoneornil(L, 3)){
		auto p = &(joint.cols[0]);
		lua_pushlightuserdata(L, (void*)p);
		return 1;
	}

	auto intype = lua_type(L, 3);
	const ozz::math::Float4x4 *inputdata = nullptr;
	if (intype == LUA_TLIGHTUSERDATA || intype == LUA_TUSERDATA){
		inputdata = (const ozz::math::Float4x4*)lua_touserdata(L, 3);
	} else if (intype == LUA_TSTRING){
		size_t size;
		inputdata = (const ozz::math::Float4x4*)lua_tolstring(L, 3, &size);
		if (sizeof(ozz::math::Float4x4) != size){
			return luaL_error(L, "invalid string data size:%d, not match joint data size:%d", size, sizeof(joint));
		}
	}

	if (inputdata){
		joint = *inputdata;
	}
	
	return 0;
}

static int
lbp_result_joints(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	const bind_pose * result = (bind_pose*)lua_touserdata(L, 1);

	auto jointcount = result->pose.size();
	lua_createtable(L, (int)jointcount, 0);

	for (size_t ii = 0; ii < jointcount; ++ii) {
		create_joint_table(L, result->pose[ii]);
		lua_seti(L, -2, ii + 1);
	}
	return 1;
}

static int
lbp_result_count(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	const bind_pose * result = (bind_pose*)lua_touserdata(L, 1);

	lua_pushinteger(L, result->pose.size());
	return 1;
}

static int
lbp_result_transform(lua_State *L){
	luaL_checktype(L, 1, LUA_TUSERDATA);
	bind_pose * result = (bind_pose*)lua_touserdata(L, 1);

	auto mat = (const ozz::math::Float4x4*)lua_touserdata(L, 2);
	auto except_root = lua_isnoneornil(L, 3) ? false : lua_toboolean(L, 3);

	for (size_t ii = (except_root ? 1 : 0); ii < result->pose.size(); ++ii){
		result->pose[ii] = *mat * result->pose[ii];
	}
	
	return 0;
}

static int
ldel_sampling(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	sampling_node *sampling = (sampling_node *)lua_touserdata(L, 1);
	OZZ_DELETE(ozz::memory::default_allocator(), sampling->cache);

	return 0;
}

static int
lnew_sampling_cache(lua_State *L) {
	const int numjoints = (int)luaL_optinteger(L, 1, 0);

	sampling_node* samplingnode = (sampling_node*)lua_newuserdatauv(L, sizeof(sampling_node), 0);
	luaL_getmetatable(L, "SAMPLING_NODE");
	lua_setmetatable(L, -2);

	samplingnode->cache = OZZ_NEW(ozz::memory::default_allocator(), ozz::animation::SamplingCache)(numjoints);
	return 1;
}

static int
ldel_bpresult(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	bind_pose *result = (bind_pose *)lua_touserdata(L, 1);	
	result->pose.~vector();
	return 0;
}

static int
lnew_bind_pose(lua_State *L) {
	luaL_checktype(L, 1, LUA_TNUMBER);
	const size_t numjoints = (size_t)lua_tointeger(L, 1);

	if (numjoints <= 0) {
		luaL_error(L, "joints number should be > 0");
		return 0;
	}

	size_t initdata_size = 0;
	const float* initdata = nullptr;

	if (!lua_isnoneornil(L, 2)){
		switch (lua_type(L, 2)){
			case LUA_TSTRING: 
				initdata = (const float*)lua_tolstring(L, 2, &initdata_size); 
				if (initdata_size != sizeof(ozz::math::Float4x4) * numjoints){
					return luaL_error(L, "init data size is not valid, need:%d", sizeof(ozz::math::Float4x4) * numjoints);
				}
			break;
			case LUA_TUSERDATA:
			case LUA_TLIGHTUSERDATA:
				initdata = (const float*)lua_touserdata(L, 2);
				initdata_size = numjoints * sizeof(ozz::math::Float4x4);
			break;
			default:
			return luaL_error(L, "argument 2 is not support type, only support string/userdata/light userdata");
		}
	} 

	bind_pose *result = (bind_pose*)lua_newuserdatauv(L, sizeof(bind_pose), 0);
	luaL_getmetatable(L, "OZZ_BIND_POSE");
	lua_setmetatable(L, -2);
	new(&result->pose)bind_pose::bind_pose_type(numjoints);

	if (initdata){
		memcpy(&result->pose[0], initdata, initdata_size);
	}

	return 1;
}

static int
ldel_animation(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);

	animation_node *node = (animation_node*)lua_touserdata(L, 1);
	OZZ_DELETE(ozz::memory::default_allocator(), node->ani);	
	
	return 0;
}

static int
lnew_animation(lua_State *L) {
	luaL_checktype(L, 1, LUA_TSTRING);
	const char * path = lua_tostring(L, 1);

	animation_node *node = (animation_node*)lua_newuserdatauv(L, sizeof(animation_node), 0);
	luaL_getmetatable(L, "ANIMATION_NODE");
	lua_setmetatable(L, -2);
	
	node->ani = OZZ_NEW(ozz::memory::default_allocator(), ozz::animation::Animation);

	ozz::io::File file(path, "rb");
	if (!file.opened()) {
		luaL_error(L, "file could not open : %s", path);
	}

	ozz::io::IArchive archive(&file);
	if (!archive.TestTag<ozz::animation::Animation>()) {		
		luaL_error(L, "file is not ozz::animation, file : %s", path);
	}
	archive >> *(node->ani);
	return 1;
}

static int
lduration_animation(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	animation_node *node = (animation_node*)lua_touserdata(L, 1);
	lua_pushnumber(L, node->ani->duration());
	return 1;
}

static int
lsize_animation(lua_State *L){
	luaL_checktype(L, 1, LUA_TUSERDATA);
	animation_node *node = (animation_node*)lua_touserdata(L, 1);
	lua_pushinteger(L, node->ani->size());
	return 1;
}

static int
lnew_memory(lua_State *L){
	const size_t sizebytes = (size_t)lua_tointeger(L, 1);
	const size_t aligned = (size_t)luaL_optinteger(L, 2, 4);

	aligned_memory* am =  (aligned_memory*)lua_newuserdatauv(L, sizeof(aligned_memory), 0);
	luaL_getmetatable(L, "ALIGNED_MEMORY");
	lua_setmetatable(L, -2);

	am->ptr = ozz::memory::default_allocator()->Allocate(sizebytes, aligned);
	return 1;
}

static int
lpointer_memory(lua_State *L){
	auto am = (aligned_memory*)lua_touserdata(L, 1);
	lua_pushlightuserdata(L, am->ptr);
	return 1;
}

static int
ldel_memory(lua_State *L){
	luaL_checkudata(L, 1, "ALIGNED_MEMORY");
	auto am = (aligned_memory*)lua_touserdata(L, 1);
	ozz::memory::default_allocator()->Deallocate(am->ptr);

	return 0;
}

static int
lnew_joint_remap(lua_State *L){
	decltype(joint_remap::joints)	joints;

	switch (lua_type(L, 1)){
		case LUA_TTABLE:{
			const uint32_t jointnum = (uint32_t)lua_rawlen(L, 1);
			joints.resize(jointnum);
			for (uint32_t ii = 0; ii < jointnum; ++ii){
				lua_geti(L, 1, ii+1);
				joints[ii] = (uint16_t)lua_tointeger(L, -1);
				lua_pop(L, 1);
			}
		}
		break;
		case LUA_TLIGHTUSERDATA:{
			luaL_checktype(L, 2, LUA_TNUMBER);
			const uint32_t jointnum = (uint32_t)lua_tointeger(L, 2);
			joints.resize(jointnum);

			const uint16_t *p = (const uint16_t*)lua_touserdata(L, 1);
			memcpy(&joints.front(), p, jointnum * sizeof(uint16_t));
		}
		break;
		default:
		return luaL_error(L, "not support type in argument 1");
	}

	auto jarray = (joint_remap*)lua_newuserdatauv(L, sizeof(joint_remap), 0);
	luaL_getmetatable(L, "JOINT_REMAP");
	lua_setmetatable(L, -2);

	new(&jarray->joints) decltype(jarray->joints)(std::move(joints));
	return 1;
}

static int
ldel_joint_remap(lua_State *L){
	luaL_checkudata(L, 1, "JOINT_REMAP");
	auto jarray = (joint_remap*)lua_touserdata(L, 1);
	jarray->joints.~vector();
	return 0;
}

static void 
register_animation_mt(lua_State *L) {
	luaL_newmetatable(L, "ANIMATION_NODE");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");	// ANIMATION_NODE.__index = ANIMATION_NODE

	luaL_Reg l[] = {		
		{"duration", lduration_animation},
		{"size", lsize_animation},
		{"__gc", ldel_animation},
		{nullptr, nullptr},
	};

	luaL_setfuncs(L, l, 0);
}

static void
register_sampling_mt(lua_State *L) {
	luaL_newmetatable(L, "SAMPLING_NODE");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	luaL_Reg l[] = {
		{"__gc", ldel_sampling},
		{nullptr, nullptr},
	};

	luaL_setfuncs(L, l, 0);
}

static void
register_bind_pose_mt(lua_State *L) {
	luaL_newmetatable(L, "OZZ_BIND_POSE");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	luaL_Reg l[] = {
		{"", lbp_result_init},
		{"joint", lbp_result_joint},
		{"joints", lbp_result_joints},
		{"count", lbp_result_count},
		{"transform", lbp_result_transform},
		{"__gc", ldel_bpresult},
		{nullptr, nullptr},
	};

	luaL_setfuncs(L, l, 0);
}

static int
ldel_bind_pose(lua_State *L) {
	luaL_checktype(L, 1, LUA_TUSERDATA);
	bind_pose_soa *pose = (bind_pose_soa*)lua_touserdata(L, 1);
	pose->pose.~vector();
	return 0;
}

static int
lnew_bind_pose_soa(lua_State *L) {
	auto bp = (bind_pose_soa*)lua_newuserdatauv(L, sizeof(bind_pose_soa), 0);
	luaL_getmetatable(L, "OZZ_BING_POSE_SOA");
	lua_setmetatable(L, -2);

	new(&bp->pose)ozz::Vector<ozz::math::SoaTransform>::Std();	
	return 1;
}

static void
register_bind_pose_soa_mt(lua_State *L) {
	luaL_newmetatable(L, "OZZ_BING_POSE_SOA");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	luaL_Reg l[] = {
		{"__gc", ldel_bind_pose},
		{nullptr, nullptr},
	};

	luaL_setfuncs(L, l, 0);
}

static void
register_aligned_memory(lua_State *L){
	luaL_newmetatable(L, "ALIGNED_MEMORY");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_Reg l[] = {
		{"pointer", lpointer_memory},
		{"__gc", ldel_memory},
		{nullptr, nullptr},
	};

	luaL_setfuncs(L, l, 0);
}

static void
register_joint_remap(lua_State *L){
	luaL_newmetatable(L, "JOINT_REMAP");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	luaL_Reg l[] = {
		{"__gc", ldel_joint_remap},
		{nullptr, nullptr},
	};

	luaL_setfuncs(L, l, 0);
}

extern "C" {
LUAMOD_API int
luaopen_hierarchy_animation(lua_State *L) {
	register_animation_mt(L);
	register_sampling_mt(L);
	
	register_bind_pose_mt(L);
	register_bind_pose_soa_mt(L);
	register_aligned_memory(L);
	register_joint_remap(L);

	luaL_Reg l[] = {
		{ "mesh_skinning", 			lmesh_skinning},
		{ "build_skinning_matrices", lbuild_skinning_matrices},

		{ "motion", 			lmotion},
		{ "blend_animations", 	lblend_animations},
		{ "sample_animation", 	lsample_animation},
		{ "transform", 			ltransform_to_bindpose_result},
		{ "new_ani", 			lnew_animation},
		{ "new_sampling_cache", lnew_sampling_cache},
		{ "new_bind_pose", 		lnew_bind_pose,},
		{ "new_bind_pose_soa", 	lnew_bind_pose_soa},

		{ "new_aligned_memory", lnew_memory},

		{ "new_joint_remap", 	lnew_joint_remap},
		{ NULL, NULL },
	};
	luaL_newlib(L, l);
	return 1;
}

}