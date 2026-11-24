// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#pragma once

#include <map>

#include "GPU/GPU.h"
#include "GPU/Common/TextureCacheCommon.h"

class FramebufferManagerGX2;

// Textures are thin3d textures, bound through thin3d like the framebuffers, so the draw engine
// finds everything in thin3d's texture slots (GX2NativeContext::BoundTexture).
// For now: RGBA8888 only (no 16-bit formats), the top mip level only.
class TextureCacheGX2 : public TextureCacheCommon {
public:
	TextureCacheGX2(Draw::DrawContext *draw, Draw2D *draw2D);
	~TextureCacheGX2();

	void SetFramebufferManager(FramebufferManagerGX2 *fbManager);
	void ForgetLastTexture() override;

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

protected:
	void BindTexture(TexCacheEntry *entry) override;
	void Unbind() override;
	void ReleaseTexture(TexCacheEntry *entry, bool delete_them) override;
	void BindAsClutTexture(Draw::Texture *tex, bool smooth) override;
	void ApplySamplerByKey(const SamplerCacheKey &key) override;
	void *GetNativeTextureView(const TexCacheEntry *entry, bool flat) const override;

private:
	void BuildTexture(TexCacheEntry *const entry) override;
	void DestroySamplers();

	std::map<SamplerCacheKey, Draw::SamplerState *> samplerCache_;
	Draw::SamplerState *samplerPointClamp_ = nullptr;
	Draw::SamplerState *samplerLinearClamp_ = nullptr;
};
