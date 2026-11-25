// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#include <algorithm>
#include <cstring>

#include <coreinit/memdefaultheap.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/enum.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/utils.h>

#include "Common/GPU/GX2/GX2Native.h"
#include "Common/Log.h"
#include "Common/Profiler/Profiler.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUCommon.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/GX2/DrawEngineGX2.h"
#include "GPU/GX2/FramebufferManagerGX2.h"
#include "GPU/GX2/ShaderManagerGX2.h"
#include "GPU/GX2/TextureCacheGX2.h"

// thin3d texture slots (see thin3d's TEX_SLOT_*): the PSP texture, the framebuffer copy, the CLUT.
static const int kSlotForSampler[3] = { 0, 1, 3 };

static const GX2PrimitiveMode gx2prim[8] = {
	GX2_PRIMITIVE_MODE_TRIANGLES,  // Points are expanded to triangles.
	GX2_PRIMITIVE_MODE_TRIANGLES,  // Lines too.
	GX2_PRIMITIVE_MODE_TRIANGLES,
	GX2_PRIMITIVE_MODE_TRIANGLES,
	GX2_PRIMITIVE_MODE_TRIANGLE_STRIP,
	GX2_PRIMITIVE_MODE_TRIANGLES,  // Fans are turned into lists.
	GX2_PRIMITIVE_MODE_TRIANGLES,  // Rectangles are expanded.
	GX2_PRIMITIVE_MODE_TRIANGLES,
};

static const uint8_t gx2BlendFactor[(size_t)BlendFactor::COUNT] = {
	GX2_BLEND_MODE_ZERO,
	GX2_BLEND_MODE_ONE,
	GX2_BLEND_MODE_SRC_COLOR,
	GX2_BLEND_MODE_INV_SRC_COLOR,
	GX2_BLEND_MODE_DST_COLOR,
	GX2_BLEND_MODE_INV_DST_COLOR,
	GX2_BLEND_MODE_SRC_ALPHA,
	GX2_BLEND_MODE_INV_SRC_ALPHA,
	GX2_BLEND_MODE_DST_ALPHA,
	GX2_BLEND_MODE_INV_DST_ALPHA,
	GX2_BLEND_MODE_BLEND_FACTOR,
	GX2_BLEND_MODE_INV_BLEND_FACTOR,
	GX2_BLEND_MODE_CONSTANT_ALPHA,
	GX2_BLEND_MODE_INV_CONSTANT_ALPHA,
	GX2_BLEND_MODE_SRC1_COLOR,
	GX2_BLEND_MODE_INV_SRC1_COLOR,
	GX2_BLEND_MODE_SRC1_ALPHA,
	GX2_BLEND_MODE_INV_SRC1_ALPHA,
	GX2_BLEND_MODE_ZERO,  // INVALID
};

static const uint8_t gx2BlendEq[(size_t)BlendEq::COUNT] = {
	GX2_BLEND_COMBINE_MODE_ADD,
	GX2_BLEND_COMBINE_MODE_SUB,
	GX2_BLEND_COMBINE_MODE_REV_SUB,
	GX2_BLEND_COMBINE_MODE_MIN,
	GX2_BLEND_COMBINE_MODE_MAX,
};

// In GE order.
static const uint8_t gx2Compare[8] = {
	GX2_COMPARE_FUNC_NEVER,
	GX2_COMPARE_FUNC_ALWAYS,
	GX2_COMPARE_FUNC_EQUAL,
	GX2_COMPARE_FUNC_NOT_EQUAL,
	GX2_COMPARE_FUNC_LESS,
	GX2_COMPARE_FUNC_LEQUAL,
	GX2_COMPARE_FUNC_GREATER,
	GX2_COMPARE_FUNC_GEQUAL,
};

static const uint8_t gx2StencilOp[8] = {
	GX2_STENCIL_FUNCTION_KEEP,
	GX2_STENCIL_FUNCTION_ZERO,
	GX2_STENCIL_FUNCTION_REPLACE,
	GX2_STENCIL_FUNCTION_INV,
	GX2_STENCIL_FUNCTION_INCR_CLAMP,
	GX2_STENCIL_FUNCTION_DECR_CLAMP,
	GX2_STENCIL_FUNCTION_KEEP,
	GX2_STENCIL_FUNCTION_KEEP,
};

DrawEngineGX2::DrawEngineGX2(Draw::DrawContext *draw) : draw_(draw) {
	native_ = (GX2NativeContext *)draw->GetNativeObject(Draw::NativeObject::CONTEXT);
	decOptions_.expand8BitNormalsToFloat = true;
}

DrawEngineGX2::~DrawEngineGX2() {
	ClearFetchShaders(false);
}

void DrawEngineGX2::DeviceLost() {
	if (draw_) {
		draw_->SetInvalidationCallback(InvalidationCallback());
		invalidationCallbackInstalled_ = false;
	}
	ClearFetchShaders(false);
	draw_ = nullptr;
	native_ = nullptr;
}

void DrawEngineGX2::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	native_ = (GX2NativeContext *)draw->GetNativeObject(Draw::NativeObject::CONTEXT);
}

void DrawEngineGX2::NotifyConfigChanged() {
	DrawEngineCommon::NotifyConfigChanged();
	ClearFetchShaders();
}

void DrawEngineGX2::ClearFetchShaders(bool waitForGPU) {
	if (fetchShaders_.empty())
		return;
	if (waitForGPU)
		GX2DrawDone();  // The GPU may still be reading them.
	for (auto &[_, f] : fetchShaders_)
		MEMFreeToDefaultHeap(f.program);
	fetchShaders_.clear();
}

void DrawEngineGX2::BeginFrame() {
	DrawEngineCommon::BeginFrame();
	if (++diagFrames_ >= 120) {
		NOTICE_LOG(Log::G3D, "GX2 draws in the last %d frames: %d hw, %d sw, %d clears, %d skipped", diagFrames_, diagHW_, diagSW_, diagClears_, diagSkipped_);
		diagFrames_ = diagHW_ = diagSW_ = diagClears_ = diagSkipped_ = 0;
	}
	if (!invalidationCallbackInstalled_) {
		draw_->SetInvalidationCallback(std::bind(&DrawEngineGX2::Invalidate, this, std::placeholders::_1));
		invalidationCallbackInstalled_ = true;
	}
}

// thin3d drew something (or changed the target): the bound texture, viewport and scissor may be other ones now.
void DrawEngineGX2::Invalidate(InvalidationCallbackFlags flags) {
	if (flags & InvalidationCallbackFlags::RENDER_PASS_STATE) {
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
	}
}

static bool DecFormatToGX2(int fmt, GX2AttribFormat *out, uint32_t *mask, GX2EndianSwapMode *swap) {
	const uint32_t X = GX2_SQ_SEL_X, Y = GX2_SQ_SEL_Y, Z = GX2_SQ_SEL_Z, W = GX2_SQ_SEL_W, S0 = GX2_SQ_SEL_0, S1 = GX2_SQ_SEL_1;
	*swap = GX2_ENDIAN_SWAP_NONE;
	switch (fmt) {
	// Floats are native (big-endian) floats, which the default swap handles.
	case DEC_FLOAT_1: *out = GX2_ATTRIB_FORMAT_FLOAT_32; *mask = GX2_SEL_MASK(X, S0, S0, S1); *swap = GX2_ENDIAN_SWAP_DEFAULT; return true;
	case DEC_FLOAT_2: *out = GX2_ATTRIB_FORMAT_FLOAT_32_32; *mask = GX2_SEL_MASK(X, Y, S0, S1); *swap = GX2_ENDIAN_SWAP_DEFAULT; return true;
	case DEC_FLOAT_3: *out = GX2_ATTRIB_FORMAT_FLOAT_32_32_32; *mask = GX2_SEL_MASK(X, Y, Z, S1); *swap = GX2_ENDIAN_SWAP_DEFAULT; return true;
	case DEC_FLOAT_4: *out = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32; *mask = GX2_SEL_MASK(X, Y, Z, W); *swap = GX2_ENDIAN_SWAP_DEFAULT; return true;
	// Bytes are in memory order.
	case DEC_S8_3: *out = GX2_ATTRIB_FORMAT_SNORM_8_8_8_8; *mask = GX2_SEL_MASK(X, Y, Z, S1); return true;
	case DEC_U8_1: *out = GX2_ATTRIB_FORMAT_UNORM_8; *mask = GX2_SEL_MASK(X, S0, S0, S1); return true;
	case DEC_U8_2: *out = GX2_ATTRIB_FORMAT_UNORM_8_8; *mask = GX2_SEL_MASK(X, Y, S0, S1); return true;
	case DEC_U8_3: *out = GX2_ATTRIB_FORMAT_UNORM_8_8_8_8; *mask = GX2_SEL_MASK(X, Y, Z, S1); return true;
	case DEC_U8_4: *out = GX2_ATTRIB_FORMAT_UNORM_8_8_8_8; *mask = GX2_SEL_MASK(X, Y, Z, W); return true;
	// 16-bit values are native u16s/s16s.
	case DEC_S16_3: *out = (GX2AttribFormat)(GX2_ATTRIB_FLAG_SIGNED | GX2_ATTRIB_TYPE_16_16_16_16); *mask = GX2_SEL_MASK(X, Y, Z, S1); *swap = GX2_ENDIAN_SWAP_8_IN_16; return true;
	case DEC_U16_1: *out = (GX2AttribFormat)GX2_ATTRIB_TYPE_16; *mask = GX2_SEL_MASK(X, S0, S0, S1); *swap = GX2_ENDIAN_SWAP_8_IN_16; return true;
	case DEC_U16_2: *out = (GX2AttribFormat)GX2_ATTRIB_TYPE_16_16; *mask = GX2_SEL_MASK(X, Y, S0, S1); *swap = GX2_ENDIAN_SWAP_8_IN_16; return true;
	case DEC_U16_3: *out = (GX2AttribFormat)GX2_ATTRIB_TYPE_16_16_16_16; *mask = GX2_SEL_MASK(X, Y, Z, S1); *swap = GX2_ENDIAN_SWAP_8_IN_16; return true;
	case DEC_U16_4: *out = (GX2AttribFormat)GX2_ATTRIB_TYPE_16_16_16_16; *mask = GX2_SEL_MASK(X, Y, Z, W); *swap = GX2_ENDIAN_SWAP_8_IN_16; return true;
	default: return false;
	}
}

GX2FetchShader *DrawEngineGX2::GetFetchShader(GX2VertexShaderObj *vs, const DecVtxFormat &decFmt, bool transformed) {
	const auto key = std::make_pair(vs, transformed ? 0xFFFFFFFFU : decFmt.id);
	auto it = fetchShaders_.find(key);
	if (it != fetchShaders_.end())
		return &it->second.fetch;

	std::vector<GX2AttribStream> streams;
	auto add = [&](PspAttributeLocation loc, uint32_t offset, GX2AttribFormat format, uint32_t mask, GX2EndianSwapMode swap) {
		if (!(vs->attribMask & (1U << (int)loc)))
			return;  // Not read by this shader.
		GX2AttribStream s{};
		s.location = (uint32_t)loc;
		s.buffer = 0;
		s.offset = offset;
		s.format = format;
		s.type = GX2_ATTRIB_INDEX_PER_VERTEX;
		s.aluDivisor = 0;
		s.mask = mask;
		s.endianSwap = swap;
		streams.push_back(s);
	};
	auto addDec = [&](PspAttributeLocation loc, int fmt, int offset) {
		GX2AttribFormat format;
		uint32_t mask;
		GX2EndianSwapMode swap;
		if (fmt == DEC_NONE)
			return;
		if (!DecFormatToGX2(fmt, &format, &mask, &swap)) {
			WARN_LOG_N_TIMES(gx2decfmt, 5, Log::G3D, "GX2: unsupported decoded vertex format %d", fmt);
			return;
		}
		add(loc, offset, format, mask, swap);
	};

	const uint32_t X = GX2_SQ_SEL_X, Y = GX2_SQ_SEL_Y, Z = GX2_SQ_SEL_Z, W = GX2_SQ_SEL_W, S0 = GX2_SQ_SEL_0, S1 = GX2_SQ_SEL_1;
	if (transformed) {
		add(PspAttributeLocation::POSITION, offsetof(TransformedVertex, pos), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, GX2_SEL_MASK(X, Y, Z, W), GX2_ENDIAN_SWAP_DEFAULT);
		add(PspAttributeLocation::TEXCOORD, offsetof(TransformedVertex, uv), GX2_ATTRIB_FORMAT_FLOAT_32_32_32, GX2_SEL_MASK(X, Y, Z, S1), GX2_ENDIAN_SWAP_DEFAULT);
		// Native u32s (0xAABBGGRR), so A comes first in memory: swap back.
		add(PspAttributeLocation::COLOR0, offsetof(TransformedVertex, color0_32), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8, GX2_SEL_MASK(X, Y, Z, W), GX2_ENDIAN_SWAP_8_IN_32);
		add(PspAttributeLocation::COLOR1, offsetof(TransformedVertex, color1_32), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8, GX2_SEL_MASK(X, Y, Z, W), GX2_ENDIAN_SWAP_8_IN_32);
		add(PspAttributeLocation::NORMAL, offsetof(TransformedVertex, fog), GX2_ATTRIB_FORMAT_FLOAT_32, GX2_SEL_MASK(X, S0, S0, S1), GX2_ENDIAN_SWAP_DEFAULT);
	} else {
		addDec(PspAttributeLocation::TEXCOORD, decFmt.uvfmt, decFmt.uvoff);
		addDec(PspAttributeLocation::COLOR0, decFmt.c0fmt, decFmt.c0off);
		addDec(PspAttributeLocation::COLOR1, decFmt.c1fmt, decFmt.c1off);
		addDec(PspAttributeLocation::NORMAL, decFmt.nrmfmt, decFmt.nrmoff);
		addDec(PspAttributeLocation::POSITION, DecVtxFormat::PosFmt(), decFmt.posoff);
	}

	uint32_t size = GX2CalcFetchShaderSizeEx((uint32_t)streams.size(), GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
	uint8_t *program = (uint8_t *)MEMAllocFromDefaultHeapEx(size, GX2_SHADER_PROGRAM_ALIGNMENT);
	if (!program)
		return nullptr;
	FetchShader &f = fetchShaders_[key];
	f.program = program;
	GX2InitFetchShaderEx(&f.fetch, program, (uint32_t)streams.size(), streams.data(), GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, program, size);
	return &f.fetch;
}

void DrawEngineGX2::ApplyDrawState(int prim) {
	if (!gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
		return;
	}

	State &s = state_;
	if (gstate_c.IsDirty(DIRTY_BLEND_STATE)) {
		if (gstate.isModeClear()) {
			s.blendEnable = false;
			s.useBlendColor = false;
			s.colorMask = ClearModeChannelMask();
		} else {
			pipelineState_.Convert(draw_->GetShaderLanguageDesc().bitwiseOps, gstate_c.Use(GPU_USE_SHADER_BLENDING));
			GenericMaskState &maskState = pipelineState_.maskState;
			GenericBlendState &blendState = pipelineState_.blendState;

			if (pipelineState_.FramebufferRead()) {
				FBOTexState fboTexBindState = FBO_TEX_NONE;
				ApplyFramebufferRead(&fboTexBindState);
				ApplyStencilReplaceAndLogicOpIgnoreBlend(blendState.replaceAlphaWithStencil, blendState);
				if (fboTexBindState == FBO_TEX_COPY_BIND_TEX) {
					framebufferManager_->BindFramebufferAsColorTexture(1, framebufferManager_->GetCurrentRenderVFB(), ShaderReadBindFlags(), 0);
					fboTexBound_ = true;
					framebufferManager_->RebindFramebuffer("RebindFramebuffer - ApplyDrawState");
					dirtyRequiresRecheck_ |= DIRTY_BLEND_STATE;
					gstate_c.Dirty(DIRTY_BLEND_STATE);
				}
				dirtyRequiresRecheck_ |= DIRTY_FRAGMENTSHADER_STATE;
				gstate_c.Dirty(DIRTY_FRAGMENTSHADER_STATE);
			} else if (fboTexBound_) {
				fboTexBound_ = false;
				dirtyRequiresRecheck_ |= DIRTY_FRAGMENTSHADER_STATE;
				gstate_c.Dirty(DIRTY_FRAGMENTSHADER_STATE);
			}

			if (blendState.blendEnabled) {
				s.blendEnable = true;
				s.eqColor = gx2BlendEq[(size_t)blendState.eqColor];
				s.eqAlpha = gx2BlendEq[(size_t)blendState.eqAlpha];
				s.srcColor = gx2BlendFactor[(size_t)blendState.srcColor];
				s.srcAlpha = gx2BlendFactor[(size_t)blendState.srcAlpha];
				s.dstColor = gx2BlendFactor[(size_t)blendState.dstColor];
				s.dstAlpha = gx2BlendFactor[(size_t)blendState.dstAlpha];
				if (blendState.dirtyShaderBlendFixValues) {
					dirtyRequiresRecheck_ |= DIRTY_SHADERBLEND;
					gstate_c.Dirty(DIRTY_SHADERBLEND);
				}
				s.useBlendColor = blendState.useBlendColor;
				if (blendState.useBlendColor)
					blendState.blendColorToFloat4(s.blendColor);
			} else {
				s.blendEnable = false;
				s.useBlendColor = false;
			}
			s.colorMask = maskState.channelMask;
		}
	}

	if (gstate_c.IsDirty(DIRTY_RASTER_STATE)) {
		s.cullEnable = !gstate.isModeClear() && prim != GE_PRIM_RECTANGLES && prim > GE_PRIM_LINE_STRIP && gstate.isCullEnabled();
		s.cullFront = gstate.getCullMode() != 0;
	}

	if (gstate_c.IsDirty(DIRTY_DEPTHSTENCIL_STATE)) {
		GenericStencilFuncState stencilState;
		ConvertStencilFuncState(stencilState);

		if (gstate.isModeClear()) {
			s.depthTest = true;
			s.depthFunc = GX2_COMPARE_FUNC_ALWAYS;
			s.depthWrite = gstate.isClearModeDepthMask();
			if (gstate.isClearModeAlphaMask()) {
				s.stencilTest = true;
				s.stencilFunc = GX2_COMPARE_FUNC_ALWAYS;
				s.stencilPass = s.stencilFail = s.stencilZFail = GX2_STENCIL_FUNCTION_REPLACE;
				s.stencilRef = 0xFF;
				s.stencilCompareMask = 0xFF;
				s.stencilWriteMask = stencilState.writeMask;
			} else {
				s.stencilTest = false;
			}
		} else {
			if (!IsDepthTestEffectivelyDisabled()) {
				s.depthTest = true;
				s.depthFunc = gx2Compare[DepthTestFunctionForDraw()];
				s.depthWrite = gstate.isDepthWriteEnabled();
				UpdateEverUsedEqualDepth(gstate.getDepthTestFunction());
			} else {
				s.depthTest = false;
				s.depthWrite = false;
				s.depthFunc = GX2_COMPARE_FUNC_ALWAYS;
			}

			if (stencilState.enabled) {
				s.stencilTest = true;
				s.stencilFunc = gx2Compare[stencilState.testFunc];
				s.stencilPass = gx2StencilOp[stencilState.zPass];
				s.stencilFail = gx2StencilOp[stencilState.sFail];
				s.stencilZFail = gx2StencilOp[stencilState.zFail];
				s.stencilCompareMask = stencilState.testMask;
				s.stencilWriteMask = stencilState.writeMask;
				s.stencilRef = stencilState.testRef;

				if (SpongebobDepthInverseConditions(stencilState)) {
					s.blendEnable = true;
					s.eqColor = s.eqAlpha = GX2_BLEND_COMBINE_MODE_ADD;
					s.srcColor = s.dstColor = s.srcAlpha = s.dstAlpha = GX2_BLEND_MODE_ZERO;
					s.colorMask = 0x8;
					s.depthFunc = GX2_COMPARE_FUNC_LESS;
					s.stencilFunc = GX2_COMPARE_FUNC_ALWAYS;
					s.stencilPass = GX2_STENCIL_FUNCTION_ZERO;
					s.stencilFail = GX2_STENCIL_FUNCTION_ZERO;
					s.stencilZFail = GX2_STENCIL_FUNCTION_KEEP;
					dirtyRequiresRecheck_ |= DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE;
					gstate_c.Dirty(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE);
				}
			} else {
				s.stencilTest = false;
			}
		}
	}

	if (gstate_c.IsDirty(DIRTY_VIEWPORTSCISSOR_STATE)) {
		ViewportAndScissor vpAndScissor;
		ConvertViewportAndScissor(
			framebufferManager_->GetDisplayLayoutConfigCopy(),
			framebufferManager_->UseBufferedRendering(),
			framebufferManager_->GetRenderWidth(), framebufferManager_->GetRenderHeight(),
			framebufferManager_->GetTargetBufferWidth(), framebufferManager_->GetTargetBufferHeight(),
			vpAndScissor);
		viewport_.TopLeftX = vpAndScissor.viewportX;
		viewport_.TopLeftY = vpAndScissor.viewportY;
		viewport_.Width = vpAndScissor.viewportW;
		viewport_.Height = vpAndScissor.viewportH;
		viewport_.MinDepth = 0.0f;
		viewport_.MaxDepth = 1.0f;
		scissor_[0] = vpAndScissor.scissorX;
		scissor_[1] = vpAndScissor.scissorY;
		scissor_[2] = std::max(0, vpAndScissor.scissorW);
		scissor_[3] = std::max(0, vpAndScissor.scissorH);
	}
}

// Sets every register the draws depend on: thin3d's own draws in between change them.
void DrawEngineGX2::ApplyDrawStateLate(bool applyStencilRef, uint8_t stencilRef) {
	const State &s = state_;
	if (gstate_c.IsDirty(DIRTY_VIEWPORTSCISSOR_STATE)) {
		// Through thin3d, which flips the viewport and remembers both for its RestoreState.
		draw_->SetViewport(viewport_);
		draw_->SetScissorRect(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
	}

	GX2SetBlendControl(GX2_RENDER_TARGET_0, (GX2BlendMode)s.srcColor, (GX2BlendMode)s.dstColor, (GX2BlendCombineMode)s.eqColor,
		TRUE, (GX2BlendMode)s.srcAlpha, (GX2BlendMode)s.dstAlpha, (GX2BlendCombineMode)s.eqAlpha);
	GX2SetColorControl(GX2_LOGIC_OP_COPY, s.blendEnable ? 1 : 0, FALSE, TRUE);
	GX2SetTargetChannelMasks((GX2ChannelMask)(s.colorMask & 0xF), (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
		(GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
	if (s.useBlendColor)
		GX2SetBlendConstantColor(s.blendColor[0], s.blendColor[1], s.blendColor[2], s.blendColor[3]);
	GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);

	const bool hasDepth = native_->TargetHasDepth();
	GX2SetDepthStencilControl(s.depthTest && hasDepth, s.depthTest && s.depthWrite && hasDepth, (GX2CompareFunction)s.depthFunc,
		s.stencilTest && hasDepth, FALSE,
		(GX2CompareFunction)s.stencilFunc, (GX2StencilFunction)s.stencilPass, (GX2StencilFunction)s.stencilZFail, (GX2StencilFunction)s.stencilFail,
		(GX2CompareFunction)s.stencilFunc, (GX2StencilFunction)s.stencilPass, (GX2StencilFunction)s.stencilZFail, (GX2StencilFunction)s.stencilFail);
	const uint8_t ref = applyStencilRef ? stencilRef : s.stencilRef;
	GX2SetStencilMask(s.stencilCompareMask, s.stencilWriteMask, ref, s.stencilCompareMask, s.stencilWriteMask, ref);

	// The PSP's front faces are counter-clockwise like in D3D11's setup here, and the flipped viewport mirrors them.
	GX2SetCullOnlyControl(GX2_FRONT_FACE_CW, s.cullEnable && s.cullFront, s.cullEnable && !s.cullFront);

	gstate_c.Clean(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_BLEND_STATE);
	gstate_c.Dirty(dirtyRequiresRecheck_);
	dirtyRequiresRecheck_ = 0;
}

bool DrawEngineGX2::DrawNative(GX2VertexShaderObj *vs, void *fsPtr, GX2FetchShader *fetch, int prim, const void *verts, int vertsSize, int stride, const u16 *inds, int count) {
	GX2FragmentShaderObj *fs = (GX2FragmentShaderObj *)fsPtr;
	if (!fetch || vertsSize <= 0 || count <= 0)
		return false;

	uint8_t *v = native_->AllocScratch(vertsSize, GX2_VERTEX_BUFFER_ALIGNMENT);
	memcpy(v, verts, vertsSize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, v, (uint32_t)vertsSize);
	uint16_t *ind = nullptr;
	if (inds) {
		ind = (uint16_t *)native_->AllocScratch(count * sizeof(u16), 64);
		memcpy(ind, inds, count * sizeof(u16));
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, ind, (uint32_t)(count * sizeof(u16)));
	}

	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
	GX2SetFetchShader(fetch);
	GX2SetVertexShader(vs->shader);
	GX2SetPixelShader(fs->shader);
	shaderManager_->BindUniforms(native_, vs, fs);
	for (int i = 0; i < 3; i++) {
		const int loc = fs->samplerLocation[i];
		if (loc < 0)
			continue;
		const GX2Texture *tex = native_->BoundTexture(kSlotForSampler[i]);
		const GX2Sampler *sampler = native_->BoundSampler(kSlotForSampler[i]);
		if (tex)
			GX2SetPixelTexture(tex, loc);
		if (sampler)
			GX2SetPixelSampler(sampler, loc);
	}

	GX2SetAttribBuffer(0, (uint32_t)vertsSize, (uint32_t)stride, v);
	if (ind) {
		// Native (big-endian) u16s.
		GX2DrawIndexedEx(gx2prim[prim & 7], (uint32_t)count, GX2_INDEX_TYPE_U16, ind, 0, 1);
	} else {
		GX2DrawEx(gx2prim[prim & 7], (uint32_t)count, 0, 1);
	}
	native_->ExternalStateChanged();
	return true;
}

void DrawEngineGX2::Flush() {
	if (!numDrawVerts_) {
		return;
	}
	PROFILE_THIS_SCOPE("flush");

	GEPrimitiveType prim = prevPrim_;

	// Always use software for flat shading to fix the provoking index.
	bool useHWTransform = CanUseHardwareTransform(prim) && gstate.getShadeMode() != GE_SHADE_FLAT;
	if (clipInfoFlags_ & ClipInfoFlags::Valid) {
		if (clipInfoFlags_ & ClipInfoFlags::SoftClipCull) {
			useHWTransform = false;
		}
	}
	CheckSkinnedMinMaxZ(useHWTransform);
	if (clipInfoFlags_ != lastClipInfoFlags_) {
		ClipInfoFlags changed = (ClipInfoFlags)((u32)clipInfoFlags_ ^ (u32)lastClipInfoFlags_);
		if (changed & (ClipInfoFlags::DepthClampFragment | ClipInfoFlags::MinMaxZDiscard)) {
			gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE);
		}
		if (changed & ClipInfoFlags::NoNearClip) {
			gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE);
		}
		if (changed & ClipInfoFlags::FlatZ) {
			gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
		}
		lastClipInfoFlags_ = clipInfoFlags_;
	}

	if (useHWTransform != lastUseHwTransform_) {
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE);
		lastUseHwTransform_ = useHWTransform;
	}

	CpuClip cpuClip = BeginCpuClip(useHWTransform);

	if (useHWTransform) {
		int vertexCount;
		int maxIndex;
		bool useElements;
		if (cpuClip.active) {
			DecodeClipCull(&cpuClip, &prim, &vertexCount, &maxIndex, &useElements);
		} else {
			DecodeVerts(dec_, decoded_);
			DecodeIndsAndGetData(&prim, &vertexCount, &maxIndex, &useElements, false);
			cpuClip.numVerts = numDecodedVerts_;
		}
		gpuStats.perFrame.numVertsDrawn += vertexCount;

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
			gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
			TextureApplyResult textureResult = textureCache_->ApplyTexture(true);
			textureCache_->ApplySampler(textureResult, clipInfoFlags_ & ClipInfoFlags::FlatZ, false);
		} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
			gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		}

		// After ApplyTexture: depal can start a render pass.
		ApplyDrawState(prim);
		ApplyDrawStateLate(true, state_.stencilRef);

		GX2VertexShaderObj *vshader;
		GX2FragmentShaderObj *fshader;
		shaderManager_->GetShaders(prim, dec_->VertexType(), &vshader, &fshader, pipelineState_, useHWTransform, cpuClip.ShaderFlags(clipInfoFlags_));
		if (vshader->Failed() || fshader->Failed()) {
			WARN_LOG_N_TIMES(gx2shaderfail, 5, Log::G3D, "Skipping draw, shader compilation failed");
			diagSkipped_++;
			goto bail;
		}
		shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering(), false);

		GX2FetchShader *fetch = GetFetchShader(vshader, cpuClip.fmt, false);
		const int vSize = cpuClip.numVerts * cpuClip.fmt.stride;
		diagHW_++;
		if (useElements) {
			DrawNative(vshader, fshader, fetch, prim, decoded_, vSize, cpuClip.fmt.stride, decIndex_, vertexCount);
		} else {
			DrawNative(vshader, fshader, fetch, prim, decoded_, vSize, cpuClip.fmt.stride, nullptr, vertexCount);
		}
		if (useDepthRaster_) {
			DepthRasterSubmitHW(prim, vertexCount, cpuClip);
		}
	} else {
		PROFILE_LABEL("soft");
		DecodeVerts(dec_, decoded_);
		int vertexCount = DecodeInds();

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		prim = IndexGenerator::GeneralPrim((GEPrimitiveType)drawInds_[0].prim);

		u16 *inds = decIndex_;
		if (gstate.getShadeMode() == GE_SHADE_FLAT) {
			IndexBufferProvokingLastToFirst(prim, inds, vertexCount);
		}

		if (useDepthRaster_) {
			DepthRasterPredecoded(prim, decoded_, numDecodedVerts_, dec_, vertexCount);
		}

		bool textureNeedsApply = false;
		TextureApplyResult textureResult;
		if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
			gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
			gstate_c.dstSquared = false;
			textureResult = textureCache_->ApplyTexture(true);
			textureNeedsApply = true;
		} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
			gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		}

		SoftwareTransformResult result{};
		SoftwareTransformParams params{};
		params.everUsedEqualDepth = everUsedEqualDepth_;
		params.decoded = decoded_;
		params.transformed = transformed_;
		params.transformedExpanded = transformedExpanded_;
		params.allowClear = true;
		params.allowSeparateAlphaClear = false;
		params.clipInfoFlags = clipInfoFlags_;
		params.trianglesReversed = BatchTrianglesReversed();

		const SoftwareTransformAction action = RunSoftwareTransform(params, prim, dec_->VertexType(), dec_->GetDecVtxFmt(), numDecodedVerts_, VERTEX_BUFFER_MAX, vertexCount, inds, RemainingIndices(inds), &result);

		if (textureNeedsApply) {
			textureCache_->ApplySampler(textureResult, clipInfoFlags_ & ClipInfoFlags::FlatZ, result.pixelMapped);
		}

		ApplyDrawState(prim);
		ApplyDrawStateLate(result.setStencil, result.stencilValue);

		if (action == SW_DRAW_INDEXED) {
			GX2VertexShaderObj *vshader;
			GX2FragmentShaderObj *fshader;
			shaderManager_->GetShaders(prim, dec_->VertexType(), &vshader, &fshader, pipelineState_, false, clipInfoFlags_);
			if (vshader->Failed() || fshader->Failed()) {
				WARN_LOG_N_TIMES(gx2shaderfail2, 5, Log::G3D, "Skipping draw, shader compilation failed");
				diagSkipped_++;
				goto bail;
			}
			shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering(), result.pixelMapped);

			GX2FetchShader *fetch = GetFetchShader(vshader, dec_->GetDecVtxFmt(), true);
			const int stride = sizeof(TransformedVertex);
			diagSW_++;
			DrawNative(vshader, fshader, fetch, prim, result.drawBuffer, result.drawVertexCount * stride, stride, inds, result.drawIndexCount);
			gpuStats.perFrame.numVertsDrawn += result.drawIndexCount;
		} else if (action == SW_CLEAR) {
			u32 clearColor = result.color;
			float clearDepth = result.depth;

			Draw::Aspect clearFlag = Draw::Aspect::NO_BIT;
			if (gstate.isClearModeColorMask()) clearFlag |= Draw::Aspect::COLOR_BIT;
			if (gstate.isClearModeAlphaMask()) clearFlag |= Draw::Aspect::STENCIL_BIT;
			if (gstate.isClearModeDepthMask()) clearFlag |= Draw::Aspect::DEPTH_BIT;

			uint8_t clearStencil = clearColor >> 24;
			diagClears_++;
			draw_->Clear(clearFlag, clearColor, clearDepth, clearStencil);
			// thin3d's clear restores its own state.
			gstate_c.Dirty(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);

			if (gstate_c.Use(GPU_USE_CLEAR_RAM_HACK) && gstate.isClearModeColorMask() && (gstate.isClearModeAlphaMask() || gstate_c.framebufFormat == GE_FORMAT_565)) {
				int scissorX1 = gstate.getScissorX1();
				int scissorY1 = gstate.getScissorY1();
				int scissorX2 = gstate.getScissorX2() + 1;
				int scissorY2 = gstate.getScissorY2() + 1;
				framebufferManager_->ApplyClearToMemory(scissorX1, scissorY1, scissorX2, scissorY2, clearColor);
			}
		}
	}

bail:
	ResetAfterDrawInline();
	framebufferManager_->SetColorUpdated(gstate_c.skipDrawReason);
	gpuCommon_->NotifyFlush();
}
