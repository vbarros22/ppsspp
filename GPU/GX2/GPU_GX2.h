// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#pragma once

#include "GPU/GPUCommonHW.h"
#include "GPU/GX2/DrawEngineGX2.h"

class FramebufferManagerGX2;
class ShaderManagerGX2;
class TextureCacheGX2;
class StringWriter;

// The Wii U's hardware GPU backend: GX2, with shaders generated as Vulkan GLSL and compiled on the
// console by CafeGLSL. Modeled on the D3D11 backend; framebuffers and presentation go through
// thin3d, drawing goes to GX2 directly (DrawEngineGX2).
class GPU_GX2 : public GPUCommonHW {
public:
	GPU_GX2(GraphicsContext *gfxCtx, Draw::DrawContext *draw);
	~GPU_GX2();

	u32 CheckGPUFeatures() const override;

	void GetStats(StringWriter &w) override;
	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

protected:
	void FinishDeferred() override;

private:
	void BeginHostFrame(const DisplayLayoutConfig &config) override;

	FramebufferManagerGX2 *framebufferManagerGX2_;
	TextureCacheGX2 *textureCacheGX2_;
	DrawEngineGX2 drawEngine_;
	ShaderManagerGX2 *shaderManagerGX2_;
};
