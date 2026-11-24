// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#pragma once

#include <map>
#include <utility>

#include <gx2/shaders.h>

#include "Common/GPU/thin3d.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/DrawEngineCommon.h"

class GX2NativeContext;
class GX2VertexShaderObj;
class ShaderManagerGX2;
class TextureCacheGX2;
class FramebufferManagerGX2;

// Handles transform, lighting and drawing, like DrawEngineD3D11, but sets the GX2 registers itself.
// thin3d owns the render targets and the bound textures; the draw engine reads those back through
// GX2NativeContext, and tells thin3d afterwards that it changed the GPU state behind its back.
class DrawEngineGX2 : public DrawEngineCommon {
public:
	DrawEngineGX2(Draw::DrawContext *draw);
	~DrawEngineGX2();

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

	void SetShaderManager(ShaderManagerGX2 *shaderManager) { shaderManager_ = shaderManager; }
	void SetTextureCache(TextureCacheGX2 *textureCache) { textureCache_ = textureCache; }
	void SetFramebufferManager(FramebufferManagerGX2 *fbManager) { framebufferManager_ = fbManager; }

	void BeginFrame() override;
	void Flush() override;
	void FinishDeferred() { Flush(); }

	void NotifyConfigChanged() override;
	// The fetch shaders are keyed by vertex shader, so they go when the shaders do.
	void ClearFetchShaders();

private:
	void Invalidate(InvalidationCallbackFlags flags);
	void ApplyDrawState(int prim);
	void ApplyDrawStateLate(bool applyStencilRef, uint8_t stencilRef);
	GX2FetchShader *GetFetchShader(GX2VertexShaderObj *vs, const DecVtxFormat &decFmt, bool transformed);
	bool DrawNative(GX2VertexShaderObj *vs, void *fs, GX2FetchShader *fetch, int prim, const void *verts, int vertsSize, int stride, const u16 *inds, int count);

	Draw::DrawContext *draw_;
	GX2NativeContext *native_ = nullptr;

	ShaderManagerGX2 *shaderManager_ = nullptr;
	TextureCacheGX2 *textureCache_ = nullptr;
	FramebufferManagerGX2 *framebufferManager_ = nullptr;

	struct FetchShader {
		GX2FetchShader fetch;
		uint8_t *program;
	};
	// (vertex shader, decoded format id), 0xFFFFFFFF for TransformedVertex.
	std::map<std::pair<GX2VertexShaderObj *, uint32_t>, FetchShader> fetchShaders_;

	// The GX2 state of the draws, computed when dirty, set on every flush (thin3d's draws change it).
	struct State {
		bool blendEnable = false;
		uint8_t srcColor = 1, dstColor = 0, eqColor = 0;
		uint8_t srcAlpha = 1, dstAlpha = 0, eqAlpha = 0;
		uint8_t colorMask = 0xF;
		bool useBlendColor = false;
		float blendColor[4]{};

		bool cullEnable = false;
		bool cullFront = false;

		bool depthTest = false;
		bool depthWrite = false;
		uint8_t depthFunc = 7;
		bool stencilTest = false;
		uint8_t stencilFunc = 7, stencilPass = 0, stencilFail = 0, stencilZFail = 0;
		uint8_t stencilCompareMask = 0xFF, stencilWriteMask = 0xFF, stencilRef = 0;
	} state_;
	Draw::Viewport viewport_{};
	int scissor_[4]{};

	// Diagnostics, logged every few seconds.
	int diagFrames_ = 0;
	int diagHW_ = 0, diagSW_ = 0, diagClears_ = 0, diagSkipped_ = 0;
};
