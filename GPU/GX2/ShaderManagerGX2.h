// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <gx2/shaders.h>

#include "Common/CommonTypes.h"
#include "GPU/Common/ShaderCommon.h"
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/ShaderUniforms.h"

class GX2NativeContext;
enum class ClipInfoFlags;
struct ComputedPipelineState;

// The generated shaders are Vulkan GLSL; each stage is compiled on its own by CafeGLSL, with its
// uniforms in blocks. The uniform blocks and samplers are found by name.
class GX2VertexShaderObj {
public:
	GX2VertexShaderObj(VShaderID id, const char *code, bool useHWTransform);
	~GX2VertexShaderObj();

	bool Failed() const { return shader == nullptr; }
	bool UseHWTransform() const { return useHWTransform_; }
	const VShaderID &GetID() const { return id_; }
	std::string GetShaderString(DebugShaderStringType type) const;

	GX2VertexShader *shader = nullptr;
	int baseLocation = -1;
	int lightLocation = -1;
	// Which attribute locations the shader reads (bit per PspAttributeLocation).
	uint32_t attribMask = 0;

private:
	VShaderID id_;
	std::string source_;
	bool useHWTransform_;
};

class GX2FragmentShaderObj {
public:
	GX2FragmentShaderObj(FShaderID id, const char *code);
	~GX2FragmentShaderObj();

	bool Failed() const { return shader == nullptr; }
	const FShaderID &GetID() const { return id_; }
	std::string GetShaderString(DebugShaderStringType type) const;

	GX2PixelShader *shader = nullptr;
	int baseLocation = -1;
	// GX2 sampler locations of tex, fbotex and pal (-1 when unused).
	int samplerLocation[3]{ -1, -1, -1 };

private:
	FShaderID id_;
	std::string source_;
};

class ShaderManagerGX2 : public ShaderManagerCommon {
public:
	ShaderManagerGX2(Draw::DrawContext *draw);
	~ShaderManagerGX2();

	void GetShaders(int prim, u32 vertexType, GX2VertexShaderObj **vshader, GX2FragmentShaderObj **fshader, const ComputedPipelineState &pipelineState, bool useHWTransform, ClipInfoFlags clipInfoFlags);
	void ClearShaders() override;

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;
	int GetNumVertexShaders() const { return (int)vsCache_.size(); }
	int GetNumFragmentShaders() const { return (int)fsCache_.size(); }

	std::vector<std::string> DebugGetShaderIDs(DebugShaderType type) override;
	std::string DebugGetShaderString(std::string id, DebugShaderType type, DebugShaderStringType stringType) override;

	// Updates the CPU copies of the uniform blocks.
	void UpdateUniforms(bool useBufferedRendering, bool pixelMapped);
	// Uploads them (little-endian, as GX2 wants) when changed and points the shaders at them.
	void BindUniforms(GX2NativeContext *native, GX2VertexShaderObj *vs, GX2FragmentShaderObj *fs);

private:
	void Clear();

	std::map<FShaderID, GX2FragmentShaderObj *> fsCache_;
	std::map<VShaderID, GX2VertexShaderObj *> vsCache_;

	char *codeBuffer_;

	UB_VS_FS_Base ub_base;
	UB_VS_Lights ub_lights;

	// GPU copies of the blocks this frame; uploaded again when changed or in a new frame.
	void *gpuBase_ = nullptr;
	void *gpuLights_ = nullptr;
	int gpuFrame_ = -1;
	bool baseChanged_ = true;
	bool lightsChanged_ = true;

	GX2FragmentShaderObj *lastFShader_ = nullptr;
	GX2VertexShaderObj *lastVShader_ = nullptr;
	FShaderID lastFSID_;
	VShaderID lastVSID_;
};
