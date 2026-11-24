// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#include "Common/Log.h"
#include "Common/GPU/GraphicsContext.h"
#include "Common/Data/Text/StringWriter.h"

#include "GPU/GPUState.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/GX2/GPU_GX2.h"
#include "GPU/GX2/FramebufferManagerGX2.h"
#include "GPU/GX2/ShaderManagerGX2.h"
#include "GPU/GX2/TextureCacheGX2.h"

GPU_GX2::GPU_GX2(GraphicsContext *gfxCtx, Draw::DrawContext *draw)
	: GPUCommonHW(gfxCtx, draw), drawEngine_(draw) {
	shaderManagerGX2_ = new ShaderManagerGX2(draw);
	framebufferManagerGX2_ = new FramebufferManagerGX2(draw);
	framebufferManager_ = framebufferManagerGX2_;
	textureCacheGX2_ = new TextureCacheGX2(draw, framebufferManager_->GetDraw2D());
	textureCache_ = textureCacheGX2_;
	drawEngineCommon_ = &drawEngine_;
	shaderManager_ = shaderManagerGX2_;
	drawEngine_.SetGPUCommon(this);
	drawEngine_.SetShaderManager(shaderManagerGX2_);
	drawEngine_.SetTextureCache(textureCacheGX2_);
	drawEngine_.SetFramebufferManager(framebufferManagerGX2_);
	drawEngine_.Init();
	framebufferManagerGX2_->SetTextureCache(textureCacheGX2_);
	framebufferManagerGX2_->SetShaderManager(shaderManagerGX2_);
	framebufferManagerGX2_->SetDrawEngine(&drawEngine_);
	framebufferManagerGX2_->Init(msaaLevel_);
	textureCacheGX2_->SetFramebufferManager(framebufferManagerGX2_);
	textureCacheGX2_->SetShaderManager(shaderManagerGX2_);

	UpdateCmdInfo();
	gstate_c.SetUseFlags(CheckGPUFeatures());

	BuildReportingInfo();
	textureCache_->NotifyConfigChanged();
	NOTICE_LOG(Log::G3D, "GX2 GPU backend created");
}

GPU_GX2::~GPU_GX2() {}

u32 GPU_GX2::CheckGPUFeatures() const {
	u32 features = CheckGPUFeaturesLate(GPUCommonHW::CheckGPUFeatures());
	// Not (yet) on GX2: these need dual-source blending, reading the framebuffer in the shader,
	// clip/cull distances, depth clamping or 16-bit texture uploads, none of which the backend does.
	features &= ~(GPU_USE_DUALSOURCE_BLEND | GPU_USE_FRAMEBUFFER_FETCH | GPU_USE_SHADER_BLENDING |
		GPU_USE_CLIP_DISTANCE | GPU_USE_CULL_DISTANCE | GPU_USE_DEPTH_CLAMP | GPU_USE_16BIT_FORMATS |
		GPU_USE_LOGIC_OP | GPU_USE_FRAMEBUFFER_ARRAYS | GPU_USE_DEPTH_TEXTURE | GPU_USE_VIRTUAL_REALITY |
		GPU_USE_SINGLE_PASS_STEREO | GPU_USE_SIMPLE_STEREO_PERSPECTIVE);
	return features;
}

void GPU_GX2::DeviceLost() {
	draw_->Invalidate(InvalidationFlags::CACHED_RENDER_STATE);
	shaderManager_->ClearShaders();
	drawEngine_.ClearFetchShaders();
	GPUCommonHW::DeviceLost();
}

void GPU_GX2::DeviceRestore(Draw::DrawContext *draw) {
	GPUCommonHW::DeviceRestore(draw);
}

void GPU_GX2::BeginHostFrame(const DisplayLayoutConfig &config) {
	GPUCommonHW::BeginHostFrame(config);

	textureCache_->StartFrame();
	drawEngine_.BeginFrame();
	framebufferManager_->BeginFrame(config);

	if (gstate_c.useFlagsChanged) {
		WARN_LOG(Log::G3D, "Shader use flags changed, clearing all shaders and depth buffers");
		shaderManager_->ClearShaders();
		framebufferManager_->ClearAllDepthBuffers();
		drawEngine_.ClearFetchShaders();
		gstate_c.useFlagsChanged = false;
	}
}

void GPU_GX2::FinishDeferred() {
	drawEngine_.FinishDeferred();
}

void GPU_GX2::GetStats(StringWriter &w) {
	FormatGPUStatsCommon(w);
	w.F("Vertex, Fragment shaders loaded: %d, %d\n",
		shaderManagerGX2_->GetNumVertexShaders(),
		shaderManagerGX2_->GetNumFragmentShaders());
}
