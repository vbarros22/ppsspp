// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#include <cstring>

#include <gx2/mem.h>
#include <gx2/enum.h>
#include <cafeglsl/CafeGLSLCompiler.h>

#include "Common/GPU/thin3d.h"
#include "Common/GPU/GX2/GX2Native.h"
#include "Common/Log.h"
#include "GPU/GPUState.h"
#include "GPU/Common/VertexShaderGenerator.h"
#include "GPU/Common/FragmentShaderGenerator.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/GX2/ShaderManagerGX2.h"

static constexpr size_t CODE_BUFFER_SIZE = 32768;

static int FindUniformBlock(const GX2UniformBlock *blocks, uint32_t count, const char *name) {
	for (uint32_t i = 0; i < count; i++) {
		if (blocks[i].name && !strcmp(blocks[i].name, name))
			return (int)blocks[i].offset;
	}
	return -1;
}

GX2VertexShaderObj::GX2VertexShaderObj(VShaderID id, const char *code, bool useHWTransform)
	: id_(id), source_(code), useHWTransform_(useHWTransform) {
	std::string src = GX2PreprocessVulkanGLSL(code, strlen(code));
	char log[2048] = {};
	GX2VertexShader *vs = nullptr;
	GX2ShaderMode mode = CompileVertexShader(src.c_str(), GLSL_COMPILE_BLOCK, &vs, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
	if (mode == GLSL_SHADER_MODE_ERROR || !vs) {
		ERROR_LOG(Log::G3D, "GX2: vertex shader %s failed to compile:\n%s\n%s", id.Description().c_str(), log, src.c_str());
		return;
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, vs->program, vs->size);
	shader = vs;
	baseLocation = FindUniformBlock(vs->uniformBlocks, vs->uniformBlockCount, "baseVars");
	lightLocation = FindUniformBlock(vs->uniformBlocks, vs->uniformBlockCount, "lightVars");
	// Match the inputs by name: what counts is the location CafeGLSL gave each one, which the fetch shader
	// must feed, not necessarily the layout qualifier's.
	static const struct { const char *name; PspAttributeLocation loc; } kInputs[] = {
		{ "position", PspAttributeLocation::POSITION },
		{ "texcoord", PspAttributeLocation::TEXCOORD },
		{ "normal", PspAttributeLocation::NORMAL },
		{ "fog", PspAttributeLocation::NORMAL },
		{ "cpulit0", PspAttributeLocation::CPU_LIT0 },
		{ "cpulit1", PspAttributeLocation::CPU_LIT1 },
		{ "color0", PspAttributeLocation::COLOR0 },
		{ "color1", PspAttributeLocation::COLOR1 },
	};
	for (int i = 0; i < (int)PspAttributeLocation::COUNT; i++)
		attribLocation[i] = -1;
	for (uint32_t i = 0; i < vs->attribVarCount; i++) {
		const GX2AttribVar &v = vs->attribVars[i];
		bool found = false;
		for (const auto &in : kInputs) {
			if (v.name && !strcmp(v.name, in.name)) {
				attribLocation[(int)in.loc] = (int)v.location;
				attribMask |= 1u << (int)in.loc;
				found = true;
				if ((int)v.location != (int)in.loc)
					INFO_LOG(Log::G3D, "GX2: vertex input %s at location %d, not %d", v.name, (int)v.location, (int)in.loc);
				break;
			}
		}
		if (!found)
			WARN_LOG(Log::G3D, "GX2: unknown vertex input %s (location %d)", v.name ? v.name : "?", (int)v.location);
	}
}

GX2VertexShaderObj::~GX2VertexShaderObj() {
	FreeVertexShader(shader);
}

std::string GX2VertexShaderObj::GetShaderString(DebugShaderStringType type) const {
	switch (type) {
	case SHADER_STRING_SOURCE_CODE: return source_;
	case SHADER_STRING_SHORT_DESC: return id_.Description();
	default: return "N/A";
	}
}

GX2FragmentShaderObj::GX2FragmentShaderObj(FShaderID id, const char *code) : id_(id), source_(code) {
	std::string src = GX2PreprocessVulkanGLSL(code, strlen(code));
	char log[2048] = {};
	GX2PixelShader *ps = nullptr;
	GX2ShaderMode mode = CompilePixelShader(src.c_str(), GLSL_COMPILE_BLOCK, &ps, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
	if (mode == GLSL_SHADER_MODE_ERROR || !ps) {
		ERROR_LOG(Log::G3D, "GX2: fragment shader %s failed to compile:\n%s\n%s", id.Description().c_str(), log, src.c_str());
		return;
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, ps->program, ps->size);
	shader = ps;
	baseLocation = FindUniformBlock(ps->uniformBlocks, ps->uniformBlockCount, "baseUBO");
	static const char *const samplerNames[3] = { "tex", "fbotex", "pal" };
	for (uint32_t i = 0; i < ps->samplerVarCount; i++) {
		for (int j = 0; j < 3; j++) {
			if (ps->samplerVars[i].name && !strcmp(ps->samplerVars[i].name, samplerNames[j]))
				samplerLocation[j] = (int)ps->samplerVars[i].location;
		}
	}
}

GX2FragmentShaderObj::~GX2FragmentShaderObj() {
	FreePixelShader(shader);
}

std::string GX2FragmentShaderObj::GetShaderString(DebugShaderStringType type) const {
	switch (type) {
	case SHADER_STRING_SOURCE_CODE: return source_;
	case SHADER_STRING_SHORT_DESC: return id_.Description();
	default: return "N/A";
	}
}

ShaderManagerGX2::ShaderManagerGX2(Draw::DrawContext *draw) : ShaderManagerCommon(draw) {
	codeBuffer_ = new char[CODE_BUFFER_SIZE];
	memset(&ub_base, 0, sizeof(ub_base));
	memset(&ub_lights, 0, sizeof(ub_lights));
}

ShaderManagerGX2::~ShaderManagerGX2() {
	Clear();
	delete[] codeBuffer_;
}

void ShaderManagerGX2::DeviceLost() {
	Clear();
	draw_ = nullptr;
}

void ShaderManagerGX2::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
}

void ShaderManagerGX2::Clear() {
	for (auto &[_, fs] : fsCache_)
		delete fs;
	for (auto &[_, vs] : vsCache_)
		delete vs;
	fsCache_.clear();
	vsCache_.clear();
	lastFSID_.set_invalid();
	lastVSID_.set_invalid();
	lastVShader_ = nullptr;
	lastFShader_ = nullptr;
	gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE);
}

void ShaderManagerGX2::ClearShaders() {
	Clear();
	gstate_c.Dirty(DIRTY_ALL_UNIFORMS);
}

void ShaderManagerGX2::UpdateUniforms(bool useBufferedRendering, bool pixelMapped) {
	DirtyDepalForDraw(pixelMapped);
	uint64_t dirty = gstate_c.GetDirtyUniforms();
	if (dirty & DIRTY_BASE_UNIFORMS) {
		BaseUpdateUniforms(&ub_base, dirty, useBufferedRendering, pixelMapped);
		baseChanged_ = true;
	}
	if (dirty & DIRTY_LIGHT_UNIFORMS) {
		LightUpdateUniforms(&ub_lights, dirty);
		lightsChanged_ = true;
	}
	gstate_c.CleanUniforms();
}

// GX2 reads uniform blocks little-endian: every 32-bit word swapped (all fields are 32-bit).
static void *UploadBlock(GX2NativeContext *native, const void *data, size_t size) {
	uint32_t *dst = (uint32_t *)native->AllocScratch(size, GX2_UNIFORM_BLOCK_ALIGNMENT);
	const uint32_t *src = (const uint32_t *)data;
	for (size_t i = 0; i < size / 4; i++)
		dst[i] = __builtin_bswap32(src[i]);
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), dst, (uint32_t)size);
	return dst;
}

void ShaderManagerGX2::BindUniforms(GX2NativeContext *native, GX2VertexShaderObj *vs, GX2FragmentShaderObj *fs) {
	const int gen = (int)native->ScratchGeneration();
	if (gen != gpuFrame_) {
		// The scratch memory started over.
		gpuFrame_ = gen;
		baseChanged_ = true;
		lightsChanged_ = true;
	}
	if (baseChanged_) {
		gpuBase_ = UploadBlock(native, &ub_base, sizeof(ub_base));
		baseChanged_ = false;
	}
	if (lightsChanged_) {
		gpuLights_ = UploadBlock(native, &ub_lights, sizeof(ub_lights));
		lightsChanged_ = false;
	}
	if (vs->baseLocation >= 0)
		GX2SetVertexUniformBlock(vs->baseLocation, sizeof(ub_base), gpuBase_);
	if (vs->lightLocation >= 0)
		GX2SetVertexUniformBlock(vs->lightLocation, sizeof(ub_lights), gpuLights_);
	if (fs->baseLocation >= 0)
		GX2SetPixelUniformBlock(fs->baseLocation, sizeof(ub_base), gpuBase_);
}

void ShaderManagerGX2::GetShaders(int prim, u32 vertexType, GX2VertexShaderObj **vshader, GX2FragmentShaderObj **fshader, const ComputedPipelineState &pipelineState, bool useHWTransform, ClipInfoFlags clipInfoFlags) {
	VShaderID VSID;
	FShaderID FSID;

	if (gstate_c.IsDirty(DIRTY_VERTEXSHADER_STATE)) {
		gstate_c.Clean(DIRTY_VERTEXSHADER_STATE);
		ComputeVertexShaderID(&VSID, vertexType, useHWTransform, clipInfoFlags);
	} else {
		VSID = lastVSID_;
	}

	if (gstate_c.IsDirty(DIRTY_FRAGMENTSHADER_STATE)) {
		gstate_c.Clean(DIRTY_FRAGMENTSHADER_STATE);
		ComputeFragmentShaderID(&FSID, pipelineState, draw_->GetBugs(), clipInfoFlags);
	} else {
		FSID = lastFSID_;
	}

	if (lastVShader_ && lastFShader_ && VSID == lastVSID_ && FSID == lastFSID_) {
		*vshader = lastVShader_;
		*fshader = lastFShader_;
		return;
	}

	GX2VertexShaderObj *vs;
	auto vsIter = vsCache_.find(VSID);
	if (vsIter == vsCache_.end()) {
		std::string genErrorString;
		uint32_t attrMask;
		uint64_t uniformMask;
		VertexShaderFlags flags;
		GenerateVertexShader(VSID, codeBuffer_, draw_->GetShaderLanguageDesc(), draw_->GetBugs(), &attrMask, &uniformMask, &flags, &genErrorString);
		vs = new GX2VertexShaderObj(VSID, codeBuffer_, useHWTransform);
		vsCache_[VSID] = vs;
	} else {
		vs = vsIter->second;
	}
	lastVSID_ = VSID;

	GX2FragmentShaderObj *fs;
	auto fsIter = fsCache_.find(FSID);
	if (fsIter == fsCache_.end()) {
		std::string genErrorString;
		uint64_t uniformMask;
		FragmentShaderFlags flags;
		GenerateFragmentShader(FSID, codeBuffer_, draw_->GetShaderLanguageDesc(), draw_->GetBugs(), &uniformMask, &flags, &genErrorString);
		fs = new GX2FragmentShaderObj(FSID, codeBuffer_);
		fsCache_[FSID] = fs;
	} else {
		fs = fsIter->second;
	}
	lastFSID_ = FSID;

	lastVShader_ = vs;
	lastFShader_ = fs;
	*vshader = vs;
	*fshader = fs;
}

std::vector<std::string> ShaderManagerGX2::DebugGetShaderIDs(DebugShaderType type) {
	std::vector<uint64_t> ids;
	if (type == SHADER_TYPE_VERTEX) {
		for (auto &iter : vsCache_)
			ids.push_back(iter.first.ToUint64());
	} else if (type == SHADER_TYPE_FRAGMENT) {
		for (auto &iter : fsCache_)
			ids.push_back(iter.first.ToUint64());
	}
	return ToSortedDebugShaderIdVec(ids);
}

std::string ShaderManagerGX2::DebugGetShaderString(std::string id, DebugShaderType type, DebugShaderStringType stringType) {
	ShaderID shaderId;
	shaderId.FromString(id);
	if (type == SHADER_TYPE_VERTEX) {
		auto iter = vsCache_.find(VShaderID(shaderId));
		return iter == vsCache_.end() ? "" : iter->second->GetShaderString(stringType);
	} else if (type == SHADER_TYPE_FRAGMENT) {
		auto iter = fsCache_.find(FShaderID(shaderId));
		return iter == fsCache_.end() ? "" : iter->second->GetShaderString(stringType);
	}
	return "N/A";
}
