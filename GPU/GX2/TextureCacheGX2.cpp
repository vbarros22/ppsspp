// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#include <algorithm>
#include <cstring>

#include "Common/GPU/thin3d.h"
#include "Common/Log.h"
#include "Common/MemoryUtil.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/GX2/TextureCacheGX2.h"
#include "GPU/GX2/FramebufferManagerGX2.h"

TextureCacheGX2::TextureCacheGX2(Draw::DrawContext *draw, Draw2D *draw2D)
	: TextureCacheCommon(draw, draw2D) {
}

TextureCacheGX2::~TextureCacheGX2() {
	Clear(true);
	DestroySamplers();
}

void TextureCacheGX2::DestroySamplers() {
	for (auto &[_, s] : samplerCache_)
		s->Release();
	samplerCache_.clear();
	if (samplerPointClamp_) samplerPointClamp_->Release();
	if (samplerLinearClamp_) samplerLinearClamp_->Release();
	samplerPointClamp_ = nullptr;
	samplerLinearClamp_ = nullptr;
}

void TextureCacheGX2::DeviceLost() {
	TextureCacheCommon::DeviceLost();
	DestroySamplers();
	draw_ = nullptr;
}

void TextureCacheGX2::DeviceRestore(Draw::DrawContext *draw) {
	TextureCacheCommon::DeviceRestore(draw);
}

void TextureCacheGX2::SetFramebufferManager(FramebufferManagerGX2 *fbManager) {
	framebufferManager_ = fbManager;
}

void TextureCacheGX2::ReleaseTexture(TexCacheEntry *entry, bool delete_them) {
	Draw::Texture *tex = (Draw::Texture *)entry->texturePtr;
	if (tex) {
		tex->Release();
		entry->texturePtr = nullptr;
	}
}

void TextureCacheGX2::ForgetLastTexture() {
	Draw::Texture *none = nullptr;
	draw_->BindTextures(0, 1, &none);
}

void TextureCacheGX2::Unbind() {
	ForgetLastTexture();
}

void TextureCacheGX2::BindTexture(TexCacheEntry *entry) {
	Draw::Texture *tex = entry ? (Draw::Texture *)entry->texturePtr : nullptr;
	draw_->BindTextures(TEX_SLOT_PSP_TEXTURE, 1, &tex);
}

void TextureCacheGX2::BindAsClutTexture(Draw::Texture *tex, bool smooth) {
	if (!samplerPointClamp_) {
		Draw::SamplerStateDesc desc{};
		desc.wrapU = desc.wrapV = desc.wrapW = Draw::TextureAddressMode::CLAMP_TO_EDGE;
		desc.magFilter = desc.minFilter = desc.mipFilter = Draw::TextureFilter::NEAREST;
		samplerPointClamp_ = draw_->CreateSamplerState(desc);
		desc.magFilter = desc.minFilter = Draw::TextureFilter::LINEAR;
		samplerLinearClamp_ = draw_->CreateSamplerState(desc);
	}
	draw_->BindTextures(TEX_SLOT_CLUT, 1, &tex);
	Draw::SamplerState *s = smooth ? samplerLinearClamp_ : samplerPointClamp_;
	draw_->BindSamplerStates(TEX_SLOT_CLUT, 1, &s);
}

void TextureCacheGX2::ApplySamplerByKey(const SamplerCacheKey &key) {
	Draw::SamplerState *state;
	auto iter = samplerCache_.find(key);
	if (iter != samplerCache_.end()) {
		state = iter->second;
	} else {
		Draw::SamplerStateDesc desc{};
		desc.wrapU = key.sClamp ? Draw::TextureAddressMode::CLAMP_TO_EDGE : Draw::TextureAddressMode::REPEAT;
		desc.wrapV = key.tClamp ? Draw::TextureAddressMode::CLAMP_TO_EDGE : Draw::TextureAddressMode::REPEAT;
		desc.wrapW = Draw::TextureAddressMode::CLAMP_TO_EDGE;
		desc.magFilter = key.magFilt ? Draw::TextureFilter::LINEAR : Draw::TextureFilter::NEAREST;
		desc.minFilter = key.minFilt ? Draw::TextureFilter::LINEAR : Draw::TextureFilter::NEAREST;
		desc.mipFilter = key.mipFilt ? Draw::TextureFilter::LINEAR : Draw::TextureFilter::NEAREST;
		desc.maxAniso = key.aniso ? (float)(1 << key.anisoLevel) : 1.0f;
		state = draw_->CreateSamplerState(desc);
		samplerCache_[key] = state;
	}
	draw_->BindSamplerStates(TEX_SLOT_PSP_TEXTURE, 1, &state);
}

void *TextureCacheGX2::GetNativeTextureView(const TexCacheEntry *entry, bool flat) const {
	// What thin3d's BindNativeTexture takes: the GX2Texture.
	return (void *)(uintptr_t)draw_->GetNativeObject(Draw::NativeObject::TEXTURE_VIEW, entry->texturePtr);
}

void TextureCacheGX2::BuildTexture(TexCacheEntry *const entry) {
	BuildTexturePlan plan;
	if (!PrepareBuildTexture(plan, entry)) {
		return;
	}

	Draw::DataFormat dstFmt = plan.decodeToClut8 && !plan.doReplace ? Draw::DataFormat::R8_UNORM : Draw::DataFormat::R8G8B8A8_UNORM;
	const int bpp = dstFmt == Draw::DataFormat::R8_UNORM ? 1 : 4;

	// The top level only, for now.
	int w, h;
	plan.GetMipSize(0, &w, &h);
	const int decodeStride = std::max(w * bpp, 16);
	u8 *data = (u8 *)AllocateAlignedMemory((size_t)decodeStride * h, 16);
	if (!data) {
		ERROR_LOG(Log::G3D, "GX2: out of memory decoding a %dx%d texture", w, h);
		return;
	}
	LoadTextureLevel(*entry, data, 0, decodeStride, plan, plan.baseLevelSrc, dstFmt, TexDecodeFlags{});
	if (decodeStride != w * bpp) {
		// thin3d wants the rows packed.
		for (int y = 1; y < h; y++)
			memmove(data + y * w * bpp, data + y * decodeStride, w * bpp);
	}

	Draw::TextureDesc desc{};
	desc.type = Draw::TextureType::LINEAR2D;
	desc.format = dstFmt;
	desc.width = w;
	desc.height = h;
	desc.depth = 1;
	desc.mipLevels = 1;
	desc.tag = "game";
	desc.initData.push_back(data);
	Draw::Texture *tex = draw_->CreateTexture(desc);
	FreeAlignedMemory(data);
	if (!tex) {
		return;
	}

	entry->texturePtr = tex;
	entry->status |= TexStatus::NO_MIPS;
	if (plan.doReplace) {
		entry->SetAlphaStatus(plan.replaced->AlphaStatus());
	}
}
