// thin3d backend for the Wii U's GX2.
//
// Shaders: thin3d and the GPU emulation generate Vulkan-flavoured GLSL (GLSL_VULKAN). That is
// nearly plain GLSL 4.50, so it is lightly preprocessed (no descriptor sets, gl_VertexIndex)
// and compiled on the console by CafeGLSL. Bindings and locations carry over unchanged.
//
// Conventions match Direct3D 11: clip space Y up (the vertex shaders flip, viewportYSign "-"),
// depth 0..1 (GX2's "half Z"), textures and render targets with the origin at the top left.
//
// Rules learned on hardware (see WiiU/test/gx2_test.cpp):
// - Shader programs and fetch shaders written by the CPU must be flushed with GX2Invalidate
//   before the GPU first runs them, or the GPU hangs.
// - Uniform block data is little-endian: every 32-bit word is byte-swapped on upload.
// - No GX2 calls once the app has lost the foreground (the frontend handles that).
//
// Memory: everything here lives in MEM2 (the default heap), which survives trips to the HOME
// menu. The backbuffer belongs to the frontend (WHBGfx's TV buffers, in MEM1).
//
// Simplifications for now: one mip level per texture, no depth textures, no readback, no
// blits (copies only), 8888 and 8-bit textures only.

#include "ppsspp_config.h"

#if PPSSPP_API(GX2)

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <coreinit/memdefaultheap.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2/texture.h>
#include <gx2/utils.h>

#include <cafeglsl/CafeGLSLCompiler.h>

#include "Common/GPU/thin3d.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/GPU/Shader.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"

namespace Draw {

// --- Small helpers ---

static void *AllocGPU(size_t size, size_t align) {
	void *p = MEMAllocFromDefaultHeapEx((uint32_t)size, (int)std::max<size_t>(align, 64));
	if (!p)
		ERROR_LOG(Log::G3D, "GX2: out of memory allocating %u bytes", (unsigned)size);
	return p;
}

static void FreeGPU(void *p) {
	if (p)
		MEMFreeToDefaultHeap(p);
}

static GX2CompareFunction ToGX2(Comparison c) {
	return (GX2CompareFunction)c;  // Same order: NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS.
}

static GX2StencilFunction ToGX2(StencilOp op) {
	return (GX2StencilFunction)op;  // Same order.
}

static GX2BlendMode ToGX2(BlendFactor f) {
	switch (f) {
	case BlendFactor::ZERO: return GX2_BLEND_MODE_ZERO;
	case BlendFactor::ONE: return GX2_BLEND_MODE_ONE;
	case BlendFactor::SRC_COLOR: return GX2_BLEND_MODE_SRC_COLOR;
	case BlendFactor::ONE_MINUS_SRC_COLOR: return GX2_BLEND_MODE_INV_SRC_COLOR;
	case BlendFactor::DST_COLOR: return GX2_BLEND_MODE_DST_COLOR;
	case BlendFactor::ONE_MINUS_DST_COLOR: return GX2_BLEND_MODE_INV_DST_COLOR;
	case BlendFactor::SRC_ALPHA: return GX2_BLEND_MODE_SRC_ALPHA;
	case BlendFactor::ONE_MINUS_SRC_ALPHA: return GX2_BLEND_MODE_INV_SRC_ALPHA;
	case BlendFactor::DST_ALPHA: return GX2_BLEND_MODE_DST_ALPHA;
	case BlendFactor::ONE_MINUS_DST_ALPHA: return GX2_BLEND_MODE_INV_DST_ALPHA;
	case BlendFactor::CONSTANT_COLOR: return GX2_BLEND_MODE_BLEND_FACTOR;
	case BlendFactor::ONE_MINUS_CONSTANT_COLOR: return GX2_BLEND_MODE_INV_BLEND_FACTOR;
	// GX2 has one constant; alpha factors use the same register.
	case BlendFactor::CONSTANT_ALPHA: return GX2_BLEND_MODE_BLEND_FACTOR;
	case BlendFactor::ONE_MINUS_CONSTANT_ALPHA: return GX2_BLEND_MODE_INV_BLEND_FACTOR;
	// Dual source blending isn't exposed (caps), so these shouldn't come up.
	default: return GX2_BLEND_MODE_ONE;
	}
}

static GX2BlendCombineMode ToGX2(BlendOp op) {
	switch (op) {
	case BlendOp::ADD: return GX2_BLEND_COMBINE_MODE_ADD;
	case BlendOp::SUBTRACT: return GX2_BLEND_COMBINE_MODE_SUB;
	case BlendOp::REV_SUBTRACT: return GX2_BLEND_COMBINE_MODE_REV_SUB;
	case BlendOp::MIN: return GX2_BLEND_COMBINE_MODE_MIN;
	case BlendOp::MAX: return GX2_BLEND_COMBINE_MODE_MAX;
	default: return GX2_BLEND_COMBINE_MODE_ADD;
	}
}

static GX2TexClampMode ToGX2(TextureAddressMode mode) {
	switch (mode) {
	case TextureAddressMode::REPEAT: return GX2_TEX_CLAMP_MODE_WRAP;
	case TextureAddressMode::REPEAT_MIRROR: return GX2_TEX_CLAMP_MODE_MIRROR;
	case TextureAddressMode::CLAMP_TO_EDGE: return GX2_TEX_CLAMP_MODE_CLAMP;
	case TextureAddressMode::CLAMP_TO_BORDER: return GX2_TEX_CLAMP_MODE_CLAMP_BORDER;
	default: return GX2_TEX_CLAMP_MODE_CLAMP;
	}
}

static GX2PrimitiveMode ToGX2(Primitive prim) {
	switch (prim) {
	case Primitive::POINT_LIST: return GX2_PRIMITIVE_MODE_POINTS;
	case Primitive::LINE_LIST: return GX2_PRIMITIVE_MODE_LINES;
	case Primitive::LINE_STRIP: return GX2_PRIMITIVE_MODE_LINE_STRIP;
	case Primitive::TRIANGLE_STRIP: return GX2_PRIMITIVE_MODE_TRIANGLE_STRIP;
	case Primitive::TRIANGLE_FAN: return GX2_PRIMITIVE_MODE_TRIANGLE_FAN;
	case Primitive::TRIANGLE_LIST:
	default: return GX2_PRIMITIVE_MODE_TRIANGLES;
	}
}

static GX2LogicOp ToGX2(LogicOp op) {
	switch (op) {
	case LogicOp::LOGIC_CLEAR: return GX2_LOGIC_OP_CLEAR;
	case LogicOp::LOGIC_SET: return GX2_LOGIC_OP_SET;
	case LogicOp::LOGIC_COPY: return GX2_LOGIC_OP_COPY;
	case LogicOp::LOGIC_COPY_INVERTED: return GX2_LOGIC_OP_INV_COPY;
	case LogicOp::LOGIC_NOOP: return GX2_LOGIC_OP_NOP;
	case LogicOp::LOGIC_INVERT: return GX2_LOGIC_OP_INV;
	case LogicOp::LOGIC_AND: return GX2_LOGIC_OP_AND;
	case LogicOp::LOGIC_NAND: return GX2_LOGIC_OP_NOT_AND;
	case LogicOp::LOGIC_OR: return GX2_LOGIC_OP_OR;
	case LogicOp::LOGIC_NOR: return GX2_LOGIC_OP_NOR;
	case LogicOp::LOGIC_XOR: return GX2_LOGIC_OP_XOR;
	case LogicOp::LOGIC_EQUIV: return GX2_LOGIC_OP_EQUIV;
	case LogicOp::LOGIC_AND_REVERSE: return GX2_LOGIC_OP_REV_AND;
	case LogicOp::LOGIC_AND_INVERTED: return GX2_LOGIC_OP_INV_AND;
	case LogicOp::LOGIC_OR_REVERSE: return GX2_LOGIC_OP_REV_OR;
	case LogicOp::LOGIC_OR_INVERTED: return GX2_LOGIC_OP_INV_OR;
	default: return GX2_LOGIC_OP_COPY;
	}
}

// Texture formats. compMap reorders channels as the shader sees them.
static bool TextureFormatToGX2(DataFormat fmt, TextureSwizzle swizzle, GX2SurfaceFormat *out, uint32_t *compMap) {
	*compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
	switch (fmt) {
	case DataFormat::R8G8B8A8_UNORM:
		*out = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
		return true;
	case DataFormat::B8G8R8A8_UNORM:
		*out = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
		*compMap = GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_A);
		return true;
	case DataFormat::R8_UNORM:
		*out = GX2_SURFACE_FORMAT_UNORM_R8;
		switch (swizzle) {
		case TextureSwizzle::R8_AS_ALPHA: *compMap = GX2_COMP_MAP(GX2_SQ_SEL_1, GX2_SQ_SEL_1, GX2_SQ_SEL_1, GX2_SQ_SEL_R); break;
		case TextureSwizzle::R8_AS_GRAYSCALE: *compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_R, GX2_SQ_SEL_R, GX2_SQ_SEL_1); break;
		case TextureSwizzle::R8_AS_PREMUL_ALPHA: *compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_R, GX2_SQ_SEL_R, GX2_SQ_SEL_R); break;
		default: *compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1); break;
		}
		return true;
	default:
		return false;
	}
}

static bool AttribFormatToGX2(DataFormat fmt, GX2AttribFormat *out, uint32_t *mask, GX2EndianSwapMode *swap) {
	*swap = GX2_ENDIAN_SWAP_DEFAULT;
	switch (fmt) {
	case DataFormat::R32_FLOAT:
		*out = GX2_ATTRIB_FORMAT_FLOAT_32;
		*mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
		return true;
	case DataFormat::R32G32_FLOAT:
		*out = GX2_ATTRIB_FORMAT_FLOAT_32_32;
		*mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
		return true;
	case DataFormat::R32G32B32_FLOAT:
		*out = GX2_ATTRIB_FORMAT_FLOAT_32_32_32;
		*mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1);
		return true;
	case DataFormat::R32G32B32A32_FLOAT:
		*out = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
		*mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
		return true;
	case DataFormat::R8G8B8A8_UNORM:
		// PPSSPP writes these colors as one native u32 (0xAABBGGRR). On this big-endian CPU
		// that puts A first in memory, so swap the bytes back.
		*out = GX2_ATTRIB_FORMAT_UNORM_8_8_8_8;
		*mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
		*swap = GX2_ENDIAN_SWAP_8_IN_32;
		return true;
	default:
		return false;
	}
}

// --- Per-frame scratch memory for uniforms and DrawUP data ---
//
// The frontend waits for the GPU at the end of each frame (GX2DrawDone in Present), so it
// can simply start over every frame. If a frame needs more, wait for the GPU and wrap.
class GX2ScratchRing {
public:
	explicit GX2ScratchRing(size_t size) : size_(size) {
		base_ = (uint8_t *)AllocGPU(size, 256);
	}
	~GX2ScratchRing() { FreeGPU(base_); }

	void Reset() { pos_ = 0; }

	uint8_t *Alloc(size_t size, size_t align) {
		size_t start = (pos_ + align - 1) & ~(align - 1);
		if (start + size > size_) {
			_assert_msg_(size <= size_, "GX2 scratch allocation too large: %u", (unsigned)size);
			GX2DrawDone();  // Everything queued so far is done with the old contents.
			start = 0;
		}
		pos_ = start + size;
		return base_ + start;
	}

private:
	uint8_t *base_ = nullptr;
	size_t size_ = 0;
	size_t pos_ = 0;
};

// --- State objects ---

class GX2DepthStencilState : public DepthStencilState {
public:
	explicit GX2DepthStencilState(const DepthStencilStateDesc &d) : desc(d) {}
	DepthStencilStateDesc desc;
};

class GX2BlendState : public BlendState {
public:
	explicit GX2BlendState(const BlendStateDesc &d) : desc(d) {}
	BlendStateDesc desc;
};

class GX2RasterState : public RasterState {
public:
	explicit GX2RasterState(const RasterStateDesc &d) : desc(d) {}
	RasterStateDesc desc;
};

class GX2SamplerState : public SamplerState {
public:
	explicit GX2SamplerState(const SamplerStateDesc &desc) {
		GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
		GX2InitSamplerClamping(&sampler, ToGX2(desc.wrapU), ToGX2(desc.wrapV), ToGX2(desc.wrapW));
		GX2TexAnisoRatio aniso = GX2_TEX_ANISO_RATIO_NONE;
		if (desc.maxAniso >= 16.0f) aniso = GX2_TEX_ANISO_RATIO_16_TO_1;
		else if (desc.maxAniso >= 8.0f) aniso = GX2_TEX_ANISO_RATIO_8_TO_1;
		else if (desc.maxAniso >= 4.0f) aniso = GX2_TEX_ANISO_RATIO_4_TO_1;
		else if (desc.maxAniso >= 2.0f) aniso = GX2_TEX_ANISO_RATIO_2_TO_1;
		GX2InitSamplerXYFilter(&sampler,
			desc.magFilter == TextureFilter::LINEAR ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT,
			desc.minFilter == TextureFilter::LINEAR ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT,
			aniso);
		GX2InitSamplerZMFilter(&sampler, GX2_TEX_Z_FILTER_MODE_POINT,
			desc.mipFilter == TextureFilter::LINEAR ? GX2_TEX_MIP_FILTER_MODE_LINEAR : GX2_TEX_MIP_FILTER_MODE_POINT);
		GX2InitSamplerBorderType(&sampler, GX2_TEX_BORDER_TYPE_TRANSPARENT_BLACK);
	}
	GX2Sampler sampler;
};

class GX2InputLayout : public InputLayout {
public:
	explicit GX2InputLayout(const InputLayoutDesc &d) : desc(d) {}
	InputLayoutDesc desc;
};

// --- Buffers ---

class GX2Buffer : public Buffer {
public:
	GX2Buffer(size_t size, uint32_t usage) : size(size), usage(usage) {
		data = (uint8_t *)AllocGPU(size, 256);
		if (data)
			memset(data, 0, size);
	}
	~GX2Buffer() {
		FreeGPU(data);
	}
	uint8_t *data;
	size_t size;
	uint32_t usage;
};

// --- Textures ---

class GX2TextureObject : public Texture {
public:
	GX2TextureObject() {}
	~GX2TextureObject() {
		FreeGPU(tex.surface.image);
	}

	bool Create(const TextureDesc &desc) {
		width_ = desc.width;
		height_ = desc.height;
		depth_ = 1;
		format_ = desc.format;
		uint32_t compMap;
		GX2SurfaceFormat gxFormat;
		if (desc.type != TextureType::LINEAR2D || !TextureFormatToGX2(desc.format, desc.swizzle, &gxFormat, &compMap)) {
			ERROR_LOG(Log::G3D, "GX2: unsupported texture (type %d, format %s)", (int)desc.type, DataFormatToString(desc.format));
			return false;
		}
		memset(&tex, 0, sizeof(tex));
		tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
		tex.surface.width = desc.width;
		tex.surface.height = desc.height;
		tex.surface.depth = 1;
		tex.surface.mipLevels = 1;  // Only the top level for now, see the top of the file.
		tex.surface.format = gxFormat;
		tex.surface.aa = GX2_AA_MODE1X;
		tex.surface.use = GX2_SURFACE_USE_TEXTURE;
		tex.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;  // So the CPU can write rows directly.
		tex.viewNumMips = 1;
		tex.viewNumSlices = 1;
		tex.compMap = compMap;
		GX2CalcSurfaceSizeAndAlignment(&tex.surface);
		GX2InitTextureRegs(&tex);
		tex.surface.image = AllocGPU(tex.surface.imageSize, tex.surface.alignment);
		if (!tex.surface.image)
			return false;
		bytesPerPixel = (int)DataFormatSizeInBytes(desc.format);

		if (!desc.initData.empty() || desc.initDataCallback) {
			const uint8_t *top = desc.initData.empty() ? nullptr : desc.initData[0];
			Upload(top, desc.initDataCallback);
		}
		return true;
	}

	// Rows are pitch pixels apart in GPU memory.
	uint32_t RowBytes() const { return tex.surface.pitch * bytesPerPixel; }

	void Upload(const uint8_t *data, const TextureCallback &callback) {
		uint8_t *dst = (uint8_t *)tex.surface.image;
		if (callback && callback(dst, data, width_, height_, 1, RowBytes(), RowBytes() * height_)) {
			// Written directly.
		} else if (data) {
			const int srcStride = width_ * bytesPerPixel;
			for (int y = 0; y < height_; y++)
				memcpy(dst + y * RowBytes(), data + y * srcStride, srcStride);
		}
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, tex.surface.image, tex.surface.imageSize);
	}

	void UploadRegion(const TextureRegionUpdate &r) {
		uint8_t *dst = (uint8_t *)tex.surface.image;
		const int srcStride = r.byteStride ? r.byteStride : r.w * bytesPerPixel;
		for (int y = 0; y < r.h; y++)
			memcpy(dst + (r.y + y) * RowBytes() + r.x * bytesPerPixel, r.data + y * srcStride, r.w * bytesPerPixel);
	}

	GX2Texture tex;
	int bytesPerPixel = 4;
	int lastUsedFrame = -1;
};

// --- Framebuffers ---

class GX2FramebufferObject : public Framebuffer {
public:
	GX2FramebufferObject() {}
	~GX2FramebufferObject() {
		FreeGPU(color.surface.image);
		FreeGPU(depth.surface.image);
	}

	bool Create(const FramebufferDesc &desc) {
		width_ = desc.width;
		height_ = desc.height;
		layers_ = 1;
		multiSampleLevel_ = 0;
		hasDepthStencil_ = desc.z_stencil;
		tag_ = desc.tag ? desc.tag : "";

		memset(&color, 0, sizeof(color));
		color.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
		color.surface.width = desc.width;
		color.surface.height = desc.height;
		color.surface.depth = 1;
		color.surface.mipLevels = 1;
		color.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
		color.surface.aa = GX2_AA_MODE1X;
		color.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
		color.surface.tileMode = GX2_TILE_MODE_DEFAULT;
		color.viewNumSlices = 1;
		GX2CalcSurfaceSizeAndAlignment(&color.surface);
		GX2InitColorBufferRegs(&color);
		color.surface.image = AllocGPU(color.surface.imageSize, color.surface.alignment);
		if (!color.surface.image)
			return false;
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU, color.surface.image, color.surface.imageSize);

		// A texture view of the same memory, for BindFramebufferAsTexture.
		memset(&colorTex, 0, sizeof(colorTex));
		colorTex.surface = color.surface;
		colorTex.surface.use = GX2_SURFACE_USE_TEXTURE;
		colorTex.viewNumMips = 1;
		colorTex.viewNumSlices = 1;
		colorTex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
		GX2InitTextureRegs(&colorTex);

		memset(&depth, 0, sizeof(depth));
		if (desc.z_stencil) {
			depth.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
			depth.surface.width = desc.width;
			depth.surface.height = desc.height;
			depth.surface.depth = 1;
			depth.surface.mipLevels = 1;
			depth.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;
			depth.surface.aa = GX2_AA_MODE1X;
			depth.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
			depth.surface.tileMode = GX2_TILE_MODE_DEFAULT;
			depth.viewNumSlices = 1;
			depth.depthClear = 1.0f;
			GX2CalcSurfaceSizeAndAlignment(&depth.surface);
			GX2InitDepthBufferRegs(&depth);
			depth.surface.image = AllocGPU(depth.surface.imageSize, depth.surface.alignment);
			if (!depth.surface.image)
				return false;
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU, depth.surface.image, depth.surface.imageSize);
		}
		return true;
	}

	void UpdateTag(const char *tag) override { tag_ = tag ? tag : ""; }
	const char *Tag() const override { return tag_.c_str(); }

	GX2ColorBuffer color;
	GX2Texture colorTex;
	GX2DepthBuffer depth;
	std::string tag_;
};

// --- Shaders ---

// Vulkan GLSL -> the GLSL 4.50 that CafeGLSL (Mesa, OpenGL rules) accepts:
// drop descriptor set qualifiers, map the Vulkan-only builtins.
static std::string PreprocessVulkanGLSL(const char *src, size_t size) {
	std::string out;
	out.reserve(size + 128);
	size_t i = 0;
	while (i < size) {
		// "set = N," (any spacing) inside layout qualifiers.
		if (src[i] == 's' && i + 3 < size && src[i + 1] == 'e' && src[i + 2] == 't' && (i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_'))) {
			size_t j = i + 3;
			while (j < size && (src[j] == ' ' || src[j] == '\t')) j++;
			if (j < size && src[j] == '=') {
				j++;
				while (j < size && (src[j] == ' ' || src[j] == '\t')) j++;
				size_t digits = j;
				while (j < size && isdigit((unsigned char)src[j])) j++;
				if (j > digits) {
					while (j < size && (src[j] == ' ' || src[j] == '\t')) j++;
					if (j < size && src[j] == ',') {
						j++;
						while (j < size && (src[j] == ' ' || src[j] == '\t')) j++;
						i = j;
						continue;
					}
				}
			}
		}
		out.push_back(src[i++]);
	}
	// After the #version line.
	size_t eol = out.find('\n');
	if (eol == std::string::npos)
		eol = out.size() - 1;
	out.insert(eol + 1, "#define gl_VertexIndex gl_VertexID\n#define gl_InstanceIndex gl_InstanceID\n");
	return out;
}

class GX2ShaderModule : public ShaderModule {
public:
	GX2ShaderModule(ShaderStage stage, std::string source, const char *tag) : stage_(stage), source(std::move(source)), tag(tag ? tag : "") {}
	ShaderStage GetStage() const override { return stage_; }
	ShaderStage stage_;
	std::string source;
	std::string tag;
};

class GX2Pipeline : public Pipeline {
public:
	~GX2Pipeline() {
		if (vs || ps)
			FreeShaders(vs, ps);
		FreeGPU(fetchProgram);
		if (depthStencil) depthStencil->Release();
		if (blend) blend->Release();
		if (raster) raster->Release();
		if (input) input->Release();
	}

	GX2VertexShader *vs = nullptr;
	GX2PixelShader *ps = nullptr;
	GX2ShaderMode mode = GX2_SHADER_MODE_UNIFORM_BLOCK;
	GX2FetchShader fetch{};
	uint8_t *fetchProgram = nullptr;
	GX2PrimitiveMode prim = GX2_PRIMITIVE_MODE_TRIANGLES;
	int stride = 0;

	GX2DepthStencilState *depthStencil = nullptr;
	GX2BlendState *blend = nullptr;
	GX2RasterState *raster = nullptr;
	GX2InputLayout *input = nullptr;

	// thin3d texture slot -> GX2 sampler location (the binding in the shader).
	int samplerLocation[MAX_TEXTURE_SLOTS];
	// Where the dynamic uniform buffer goes, -1 if a stage has none.
	int vsUniformLocation = -1;
	int psUniformLocation = -1;
	size_t uniformSize = 0;
};

static bool BuildFetchShader(GX2Pipeline *p, const InputLayoutDesc &desc) {
	std::vector<GX2AttribStream> streams;
	for (const AttributeDesc &attr : desc.attributes) {
		GX2AttribStream s{};
		uint32_t mask;
		GX2AttribFormat format;
		GX2EndianSwapMode swap;
		if (!AttribFormatToGX2(attr.format, &format, &mask, &swap)) {
			ERROR_LOG(Log::G3D, "GX2: unsupported vertex attribute format %s", DataFormatToString(attr.format));
			return false;
		}
		// The shaders declare their inputs at the semantic's location, like on Vulkan.
		bool used = false;
		for (uint32_t i = 0; i < p->vs->attribVarCount; i++) {
			if ((int)p->vs->attribVars[i].location == attr.location)
				used = true;
		}
		if (!used)
			continue;  // Declared in the layout but not read by this shader.
		s.location = attr.location;
		s.buffer = 0;
		s.offset = attr.offset;
		s.format = format;
		s.type = GX2_ATTRIB_INDEX_PER_VERTEX;
		s.aluDivisor = 0;
		s.mask = mask;
		s.endianSwap = swap;
		streams.push_back(s);
	}
	uint32_t size = GX2CalcFetchShaderSizeEx((uint32_t)streams.size(), GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
	p->fetchProgram = (uint8_t *)AllocGPU(size, GX2_SHADER_PROGRAM_ALIGNMENT);
	if (!p->fetchProgram)
		return false;
	GX2InitFetchShaderEx(&p->fetch, p->fetchProgram, (uint32_t)streams.size(), streams.data(), GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p->fetchProgram, size);
	p->stride = desc.stride;
	return true;
}

// --- The context ---

class GX2DrawContext : public DrawContext {
public:
	GX2DrawContext(GX2ColorBuffer *backColor, GX2DepthBuffer *backDepth);
	~GX2DrawContext();

	const DeviceCaps &GetDeviceCaps() const override { return caps_; }
	uint32_t GetSupportedShaderLanguages() const override { return (uint32_t)ShaderLanguage::GLSL_VULKAN; }
	uint32_t GetDataFormatSupport(DataFormat fmt) const override;

	DepthStencilState *CreateDepthStencilState(const DepthStencilStateDesc &desc) override { return new GX2DepthStencilState(desc); }
	BlendState *CreateBlendState(const BlendStateDesc &desc) override { return new GX2BlendState(desc); }
	SamplerState *CreateSamplerState(const SamplerStateDesc &desc) override { return new GX2SamplerState(desc); }
	RasterState *CreateRasterState(const RasterStateDesc &desc) override { return new GX2RasterState(desc); }
	InputLayout *CreateInputLayout(const InputLayoutDesc &desc) override { return new GX2InputLayout(desc); }
	ShaderModule *CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize, const char *tag) override;
	Pipeline *CreateGraphicsPipeline(const PipelineDesc &desc, const char *tag) override;

	Buffer *CreateBuffer(size_t size, uint32_t usageFlags) override { return new GX2Buffer(size, usageFlags); }
	Texture *CreateTexture(const TextureDesc &desc) override;
	Framebuffer *CreateFramebuffer(const FramebufferDesc &desc) override;

	void UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) override;
	void UpdateTextureLevels(Texture *texture, const uint8_t **data, TextureCallback initDataCallback, int numLevels) override;
	void UpdateTextureRegions(Texture *texture, int level, const TextureRegionUpdate *regions, int numRegions) override;

	void CopyFramebufferImage(Framebuffer *src, int level, int x, int y, int z, Framebuffer *dst, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth, Aspect aspects, const char *tag) override;
	bool BlitFramebuffer(Framebuffer *src, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *dst, int dstX1, int dstY1, int dstX2, int dstY2, Aspect aspects, FBBlitFilter filter, const char *tag) override { return false; }

	void BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp, const char *tag) override;
	void BindFramebufferAsTexture(Framebuffer *fbo, int binding, Aspect aspect, int layer) override;
	void GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) override;

	void SetScissorRect(int left, int top, int width, int height) override;
	void SetViewport(const Viewport &viewport) override;
	void SetBlendFactor(float color[4]) override;
	void SetStencilParams(uint8_t refValue, uint8_t writeMask, uint8_t compareMask) override;

	void BindSamplerStates(int start, int count, SamplerState **states) override;
	void BindTextures(int start, int count, Texture **textures, TextureBindFlags flags) override;
	void BindVertexBuffer(Buffer *vertexBuffer, int offset) override;
	void BindIndexBuffer(Buffer *indexBuffer, int offset) override;
	void BindNativeTexture(int sampler, void *nativeTexture) override;

	void UpdateDynamicUniformBuffer(const void *ub, size_t size) override;
	void Invalidate(InvalidationFlags flags) override;
	void BindPipeline(Pipeline *pipeline) override;

	void Draw(int vertexCount, int offset) override;
	void DrawIndexed(int vertexCount, int offset) override;
	void DrawUP(const void *vdata, int vertexCount) override;
	void DrawIndexedUP(const void *vdata, int vertexCount, const void *idata, int indexCount) override;
	void DrawIndexedClippedBatchUP(const void *vdata, int vertexCount, const void *idata, int indexCount, Slice<ClippedDraw> draws, const void *dynUniforms, size_t size) override;

	void BeginFrame(DebugFlags debugFlags) override;
	void EndFrame() override;
	void Present(PresentMode presentMode) override;
	PresentMode GetCurrentPresentMode() const override { return PresentMode::FIFO; }

	void Clear(Aspect aspects, uint32_t colorval, float depthVal, int stencilVal) override;

	std::string GetInfoString(InfoField info) const override;
	uint64_t GetNativeObject(NativeObject obj, void *srcObject) override { return 0; }
	void HandleEvent(Event ev, int width, int height, void *param1, void *param2) override {}
	void SetInvalidationCallback(InvalidationCallback callback) override { invalidationCallback_ = callback; }
	int GetFrameCount() override { return frameCount_; }

private:
	GX2ColorBuffer *CurColor() { return curFB_ ? &curFB_->color : backColor_; }
	GX2DepthBuffer *CurDepth() { return curFB_ ? (curFB_->HasDepthStencil() ? &curFB_->depth : nullptr) : backDepth_; }
	void RestoreState();  // After GX2Clear* or a copy, which clobber the GPU state.
	void ApplyPipelineState();
	void ApplyDrawState();
	void ApplyStencil();
	void SetTarget(GX2FramebufferObject *fb);

	DeviceCaps caps_{};
	GX2ColorBuffer *backColor_;
	GX2DepthBuffer *backDepth_;
	GX2ContextState *context_ = nullptr;
	GX2ScratchRing scratch_{ 4 * 1024 * 1024 };

	GX2FramebufferObject *curFB_ = nullptr;
	GX2Pipeline *curPipeline_ = nullptr;
	bool pipelineDirty_ = true;

	const GX2Texture *boundTextures_[MAX_TEXTURE_SLOTS]{};
	const GX2Sampler *boundSamplers_[MAX_TEXTURE_SLOTS]{};

	uint8_t *vertexData_ = nullptr;
	size_t vertexDataSize_ = 0;
	uint8_t *indexData_ = nullptr;

	uint8_t *uniformData_ = nullptr;
	size_t uniformDataSize_ = 0;

	Viewport viewport_{};
	int scissor_[4]{};
	float blendFactor_[4]{};
	uint8_t stencilRef_ = 0, stencilWriteMask_ = 0xFF, stencilCompareMask_ = 0xFF;

	int frameCount_ = 0;
	InvalidationCallback invalidationCallback_;
};

GX2DrawContext::GX2DrawContext(GX2ColorBuffer *backColor, GX2DepthBuffer *backDepth)
	: backColor_(backColor), backDepth_(backDepth) {
	caps_.vendor = GPUVendor::VENDOR_AMD;  // Latte is an R7xx.
	caps_.deviceName = "Latte (GX2)";
	caps_.coordConvention = CoordConvention::Direct3D11;
	caps_.maxTextureSize = 8192;
	caps_.depthRangeMinusOneToOne = false;
	caps_.preferredDepthBufferFormat = DataFormat::D24_S8;
	caps_.framebufferCopySupported = true;
	caps_.fragmentShaderInt32Supported = true;
	caps_.fragmentShaderFullPrecisionFloat = true;
	caps_.blendMinMaxSupported = true;
	caps_.anisoSupported = true;
	caps_.samplerLodControl = false;
	caps_.multiSampleLevelsMask = 1;
	caps_.presentMaxInterval = 1;
	caps_.presentModesSupported = PresentMode::FIFO;
	caps_.presentInstantModeChange = false;
	caps_.isTilingGPU = false;
	caps_.provokingVertexLast = false;
	caps_.verySlowShaderCompiler = true;  // Compiling on an Espresso, with Mesa.

	shaderLanguageDesc_.Init(GLSL_VULKAN);
	// Our clip space is Y up like Direct3D's, so the vertex shaders flip Y.
	shaderLanguageDesc_.viewportYSign = "-";

	context_ = (GX2ContextState *)AllocGPU(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
	GX2SetupContextStateEx(context_, TRUE);
	GX2SetContextState(context_);
	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
	// Clip like D3D, with depth 0..1.
	GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);
	SetTarget(nullptr);

	for (int i = 0; i < MAX_TEXTURE_SLOTS; i++) {
		boundTextures_[i] = nullptr;
		boundSamplers_[i] = nullptr;
	}
	INFO_LOG(Log::G3D, "GX2 thin3d context created, backbuffer %dx%d", backColor_->surface.width, backColor_->surface.height);
}

GX2DrawContext::~GX2DrawContext() {
	DestroyPresets();
	// The caller (frontend) makes sure the GPU is idle before tearing down.
	FreeGPU(context_);
}

uint32_t GX2DrawContext::GetDataFormatSupport(DataFormat fmt) const {
	switch (fmt) {
	case DataFormat::R8G8B8A8_UNORM:
		return FMT_TEXTURE | FMT_RENDERTARGET | FMT_INPUTLAYOUT;
	case DataFormat::B8G8R8A8_UNORM:
	case DataFormat::R8_UNORM:
		return FMT_TEXTURE;
	case DataFormat::R32_FLOAT:
	case DataFormat::R32G32_FLOAT:
	case DataFormat::R32G32B32_FLOAT:
	case DataFormat::R32G32B32A32_FLOAT:
		return FMT_INPUTLAYOUT;
	case DataFormat::D24_S8:
		return FMT_DEPTHSTENCIL;
	default:
		return 0;
	}
}

std::string GX2DrawContext::GetInfoString(InfoField info) const {
	switch (info) {
	case InfoField::APINAME: return "GX2";
	case InfoField::APIVERSION: return "wut";
	case InfoField::VENDORSTRING: return "AMD";
	case InfoField::VENDOR: return "AMD";
	case InfoField::DRIVER: return "Latte";
	case InfoField::SHADELANGVERSION: return std::string("CafeGLSL ") + GetGLSLCompilerVersion();
	default: return "";
	}
}

ShaderModule *GX2DrawContext::CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize, const char *tag) {
	if (language != ShaderLanguage::GLSL_VULKAN || (stage != ShaderStage::Vertex && stage != ShaderStage::Fragment)) {
		ERROR_LOG(Log::G3D, "GX2: unsupported shader (language %d, stage %d)", (int)language, (int)stage);
		return nullptr;
	}
	return new GX2ShaderModule(stage, PreprocessVulkanGLSL((const char *)data, dataSize), tag);
}

Pipeline *GX2DrawContext::CreateGraphicsPipeline(const PipelineDesc &desc, const char *tag) {
	GX2ShaderModule *vsModule = nullptr, *fsModule = nullptr;
	for (ShaderModule *m : desc.shaders) {
		if (!m)
			continue;
		if (m->GetStage() == ShaderStage::Vertex)
			vsModule = (GX2ShaderModule *)m;
		else if (m->GetStage() == ShaderStage::Fragment)
			fsModule = (GX2ShaderModule *)m;
	}
	if (!vsModule || !fsModule) {
		ERROR_LOG(Log::G3D, "GX2: pipeline '%s' needs a vertex and a fragment shader", tag);
		return nullptr;
	}

	GX2Pipeline *p = new GX2Pipeline();
	char log[4096] = {};
	GX2ShaderMode mode = CompileShaderPair(vsModule->source.c_str(), fsModule->source.c_str(), GLSL_COMPILE_BLOCK, &p->vs, &p->ps, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
	if (mode == GLSL_SHADER_MODE_ERROR) {
		ERROR_LOG(Log::G3D, "GX2: compiling pipeline '%s' failed:\n%s\n--- vs (%s) ---\n%s\n--- fs (%s) ---\n%s", tag, log,
			vsModule->tag.c_str(), vsModule->source.c_str(), fsModule->tag.c_str(), fsModule->source.c_str());
		p->vs = nullptr;
		p->ps = nullptr;
		delete p;
		return nullptr;
	}
	p->mode = mode;
	// The CPU wrote the programs: flush them before the GPU sees them.
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p->vs->program, p->vs->size);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p->ps->program, p->ps->size);

	p->input = (GX2InputLayout *)desc.inputLayout;
	if (p->input) {
		p->input->AddRef();
		if (!BuildFetchShader(p, p->input->desc)) {
			delete p;
			return nullptr;
		}
	} else {
		BuildFetchShader(p, InputLayoutDesc{});
	}

	p->prim = ToGX2(desc.prim);
	p->depthStencil = (GX2DepthStencilState *)desc.depthStencil;
	p->blend = (GX2BlendState *)desc.blend;
	p->raster = (GX2RasterState *)desc.raster;
	if (p->depthStencil) p->depthStencil->AddRef();
	if (p->blend) p->blend->AddRef();
	if (p->raster) p->raster->AddRef();

	for (int i = 0; i < MAX_TEXTURE_SLOTS; i++)
		p->samplerLocation[i] = i + 1;  // Vulkan's layout: textures from binding 1.
	for (const SamplerDef &def : desc.samplers) {
		for (uint32_t i = 0; i < p->ps->samplerVarCount; i++) {
			if (def.binding < MAX_TEXTURE_SLOTS && def.name && !strcmp(def.name, p->ps->samplerVars[i].name))
				p->samplerLocation[def.binding] = p->ps->samplerVars[i].location;
		}
	}
	// The one dynamic uniform buffer is at binding 0 (thin3d) / DRAW_BINDING_DYNUBO_BASE in each stage.
	if (p->vs->uniformBlockCount)
		p->vsUniformLocation = p->vs->uniformBlocks[0].offset;
	if (p->ps->uniformBlockCount)
		p->psUniformLocation = p->ps->uniformBlocks[0].offset;
	if (desc.uniformDesc)
		p->uniformSize = desc.uniformDesc->uniformBufferSize;
	return p;
}

Texture *GX2DrawContext::CreateTexture(const TextureDesc &desc) {
	GX2TextureObject *tex = new GX2TextureObject();
	if (!tex->Create(desc)) {
		tex->Release();
		return nullptr;
	}
	return tex;
}

Framebuffer *GX2DrawContext::CreateFramebuffer(const FramebufferDesc &desc) {
	GX2FramebufferObject *fb = new GX2FramebufferObject();
	if (!fb->Create(desc)) {
		fb->Release();
		return nullptr;
	}
	return fb;
}

void GX2DrawContext::UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) {
	GX2Buffer *buf = (GX2Buffer *)buffer;
	if (!buf || !buf->data || offset + size > buf->size)
		return;
	// Simple and safe: let the GPU finish with the old contents.
	GX2DrawDone();
	memcpy(buf->data + offset, data, size);
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), buf->data + offset, (uint32_t)size);
}

void GX2DrawContext::UpdateTextureLevels(Texture *texture, const uint8_t **data, TextureCallback initDataCallback, int numLevels) {
	GX2TextureObject *tex = (GX2TextureObject *)texture;
	if (!tex)
		return;
	if (tex->lastUsedFrame == frameCount_)
		GX2DrawDone();
	tex->Upload(data ? data[0] : nullptr, initDataCallback);
}

void GX2DrawContext::UpdateTextureRegions(Texture *texture, int level, const TextureRegionUpdate *regions, int numRegions) {
	GX2TextureObject *tex = (GX2TextureObject *)texture;
	if (!tex || level != 0)
		return;
	if (tex->lastUsedFrame == frameCount_)
		GX2DrawDone();
	for (int i = 0; i < numRegions; i++)
		tex->UploadRegion(regions[i]);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, tex->tex.surface.image, tex->tex.surface.imageSize);
}

void GX2DrawContext::CopyFramebufferImage(Framebuffer *srcfb, int level, int x, int y, int z, Framebuffer *dstfb, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth, Aspect aspects, const char *tag) {
	GX2FramebufferObject *src = (GX2FramebufferObject *)PrimaryToNull(srcfb);
	GX2FramebufferObject *dst = (GX2FramebufferObject *)PrimaryToNull(dstfb);
	GX2Surface *srcSurface = src ? &src->color.surface : &backColor_->surface;
	GX2Surface *dstSurface = dst ? &dst->color.surface : &backColor_->surface;
	if (!(aspects & Aspect::COLOR_BIT))
		return;  // Depth copies aren't supported (caps).
	GX2Rect rect{ x, y, x + width, y + height };
	GX2Point point{ dstX, dstY };
	GX2CopySurfaceEx(srcSurface, level, 0, dstSurface, dstLevel, 0, 1, &rect, &point);
	RestoreState();
}

void GX2DrawContext::SetTarget(GX2FramebufferObject *fb) {
	curFB_ = fb;
	GX2SetColorBuffer(CurColor(), GX2_RENDER_TARGET_0);
	GX2DepthBuffer *depth = CurDepth();
	if (depth)
		GX2SetDepthBuffer(depth);
	int w = CurColor()->surface.width, h = CurColor()->surface.height;
	viewport_ = Viewport{ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
	GX2SetViewport(0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);
	scissor_[0] = 0; scissor_[1] = 0; scissor_[2] = w; scissor_[3] = h;
	GX2SetScissor(0, 0, w, h);
}

void GX2DrawContext::BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp, const char *tag) {
	SetTarget((GX2FramebufferObject *)PrimaryToNull(fbo));
	GX2ColorBuffer *color = CurColor();
	GX2DepthBuffer *depth = CurDepth();
	bool cleared = false;
	if (rp.color == RPAction::CLEAR) {
		uint32_t c = rp.clearColor;
		GX2ClearColor(color, (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, (c >> 24) / 255.0f);
		cleared = true;
	}
	if (depth && (rp.depth == RPAction::CLEAR || rp.stencil == RPAction::CLEAR)) {
		uint32_t flags = 0;
		if (rp.depth == RPAction::CLEAR) flags |= GX2_CLEAR_FLAGS_DEPTH;
		if (rp.stencil == RPAction::CLEAR) flags |= GX2_CLEAR_FLAGS_STENCIL;
		GX2ClearDepthStencilEx(depth, rp.clearDepth, rp.clearStencil, (GX2ClearFlags)flags);
		cleared = true;
	}
	if (cleared)
		RestoreState();
}

void GX2DrawContext::BindFramebufferAsTexture(Framebuffer *fbo, int binding, Aspect aspect, int layer) {
	GX2FramebufferObject *fb = (GX2FramebufferObject *)PrimaryToNull(fbo);
	if (!fb || binding >= (int)MAX_TEXTURE_SLOTS || !(aspect & Aspect::COLOR_BIT))
		return;
	// Make sure rendering to it is finished and visible to the texture unit.
	GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, fb->color.surface.image, fb->color.surface.imageSize);
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, fb->color.surface.image, fb->color.surface.imageSize);
	boundTextures_[binding] = &fb->colorTex;
}

void GX2DrawContext::GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) {
	GX2FramebufferObject *fb = (GX2FramebufferObject *)PrimaryToNull(fbo);
	if (fb) {
		*w = fb->Width();
		*h = fb->Height();
	} else {
		*w = backColor_->surface.width;
		*h = backColor_->surface.height;
	}
}

void GX2DrawContext::SetScissorRect(int left, int top, int width, int height) {
	int w = CurColor()->surface.width, h = CurColor()->surface.height;
	int x1 = std::clamp(left, 0, w), y1 = std::clamp(top, 0, h);
	int x2 = std::clamp(left + width, x1, w), y2 = std::clamp(top + height, y1, h);
	scissor_[0] = x1; scissor_[1] = y1; scissor_[2] = x2 - x1; scissor_[3] = y2 - y1;
	GX2SetScissor(x1, y1, x2 - x1, y2 - y1);
}

void GX2DrawContext::SetViewport(const Viewport &vp) {
	viewport_ = vp;
	GX2SetViewport(vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth);
}

void GX2DrawContext::SetBlendFactor(float color[4]) {
	memcpy(blendFactor_, color, sizeof(blendFactor_));
	GX2SetBlendConstantColor(color[0], color[1], color[2], color[3]);
}

void GX2DrawContext::SetStencilParams(uint8_t refValue, uint8_t writeMask, uint8_t compareMask) {
	stencilRef_ = refValue;
	stencilWriteMask_ = writeMask;
	stencilCompareMask_ = compareMask;
	ApplyStencil();
}

void GX2DrawContext::ApplyStencil() {
	GX2SetStencilMask(stencilCompareMask_, stencilWriteMask_, stencilRef_, stencilCompareMask_, stencilWriteMask_, stencilRef_);
}

void GX2DrawContext::BindSamplerStates(int start, int count, SamplerState **states) {
	for (int i = 0; i < count && start + i < (int)MAX_TEXTURE_SLOTS; i++)
		boundSamplers_[start + i] = states[i] ? &((GX2SamplerState *)states[i])->sampler : nullptr;
}

void GX2DrawContext::BindTextures(int start, int count, Texture **textures, TextureBindFlags flags) {
	for (int i = 0; i < count && start + i < (int)MAX_TEXTURE_SLOTS; i++) {
		GX2TextureObject *tex = (GX2TextureObject *)textures[i];
		if (tex)
			tex->lastUsedFrame = frameCount_;
		boundTextures_[start + i] = tex ? &tex->tex : nullptr;
	}
}

void GX2DrawContext::BindNativeTexture(int sampler, void *nativeTexture) {
	if (sampler < (int)MAX_TEXTURE_SLOTS)
		boundTextures_[sampler] = (const GX2Texture *)nativeTexture;
}

void GX2DrawContext::BindVertexBuffer(Buffer *vertexBuffer, int offset) {
	GX2Buffer *buf = (GX2Buffer *)vertexBuffer;
	vertexData_ = buf ? buf->data + offset : nullptr;
	vertexDataSize_ = buf ? buf->size - offset : 0;
}

void GX2DrawContext::BindIndexBuffer(Buffer *indexBuffer, int offset) {
	GX2Buffer *buf = (GX2Buffer *)indexBuffer;
	indexData_ = buf ? buf->data + offset : nullptr;
}

void GX2DrawContext::UpdateDynamicUniformBuffer(const void *ub, size_t size) {
	// GX2 wants uniform blocks little-endian: swap every 32-bit word (all our uniforms are 32-bit).
	size_t padded = (size + 15) & ~15;
	uint32_t *dst = (uint32_t *)scratch_.Alloc(padded, GX2_UNIFORM_BLOCK_ALIGNMENT);
	const uint32_t *src = (const uint32_t *)ub;
	for (size_t i = 0; i < size / 4; i++)
		dst[i] = __builtin_bswap32(src[i]);
	for (size_t i = size / 4; i < padded / 4; i++)
		dst[i] = 0;
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), dst, (uint32_t)padded);
	uniformData_ = (uint8_t *)dst;
	uniformDataSize_ = padded;
}

void GX2DrawContext::Invalidate(InvalidationFlags flags) {
	if (flags & InvalidationFlags::CACHED_RENDER_STATE) {
		curPipeline_ = nullptr;
		pipelineDirty_ = true;
		for (int i = 0; i < MAX_TEXTURE_SLOTS; i++) {
			boundTextures_[i] = nullptr;
			boundSamplers_[i] = nullptr;
		}
	}
}

void GX2DrawContext::BindPipeline(Pipeline *pipeline) {
	if (curPipeline_ != pipeline) {
		curPipeline_ = (GX2Pipeline *)pipeline;
		pipelineDirty_ = true;
	}
}

void GX2DrawContext::ApplyPipelineState() {
	GX2Pipeline *p = curPipeline_;
	GX2SetShaderMode(p->mode);
	GX2SetFetchShader(&p->fetch);
	GX2SetVertexShader(p->vs);
	GX2SetPixelShader(p->ps);

	if (p->blend) {
		const BlendStateDesc &b = p->blend->desc;
		GX2SetBlendControl(GX2_RENDER_TARGET_0, ToGX2(b.srcCol), ToGX2(b.dstCol), ToGX2(b.eqCol), TRUE, ToGX2(b.srcAlpha), ToGX2(b.dstAlpha), ToGX2(b.eqAlpha));
		GX2SetColorControl(b.logicEnabled ? ToGX2(b.logicOp) : GX2_LOGIC_OP_COPY, b.enabled ? 1 : 0, FALSE, TRUE);
		GX2SetTargetChannelMasks((GX2ChannelMask)(b.colorMask & 0xF), (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
			(GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
	}
	if (p->depthStencil) {
		const DepthStencilStateDesc &d = p->depthStencil->desc;
		const StencilSetup &s = d.stencil;
		bool depthTest = d.depthTestEnabled && CurDepth() != nullptr;
		GX2SetDepthStencilControl(depthTest, depthTest && d.depthWriteEnabled, ToGX2(d.depthCompare),
			d.stencilEnabled && CurDepth() != nullptr, FALSE,
			ToGX2(s.compareOp), ToGX2(s.passOp), ToGX2(s.depthFailOp), ToGX2(s.failOp),
			ToGX2(s.compareOp), ToGX2(s.passOp), ToGX2(s.depthFailOp), ToGX2(s.failOp));
		ApplyStencil();
	}
	if (p->raster) {
		const RasterStateDesc &r = p->raster->desc;
		bool cullFront = r.cull == CullMode::FRONT || r.cull == CullMode::FRONT_AND_BACK;
		bool cullBack = r.cull == CullMode::BACK || r.cull == CullMode::FRONT_AND_BACK;
		GX2SetCullOnlyControl(r.frontFace == Facing::CW ? GX2_FRONT_FACE_CW : GX2_FRONT_FACE_CCW, cullFront, cullBack);
	}
	pipelineDirty_ = false;
}

void GX2DrawContext::ApplyDrawState() {
	GX2Pipeline *p = curPipeline_;
	if (pipelineDirty_)
		ApplyPipelineState();
	for (int i = 0; i < (int)MAX_TEXTURE_SLOTS; i++) {
		if (boundTextures_[i])
			GX2SetPixelTexture(boundTextures_[i], p->samplerLocation[i]);
		if (boundSamplers_[i])
			GX2SetPixelSampler(boundSamplers_[i], p->samplerLocation[i]);
	}
	if (uniformData_) {
		if (p->vsUniformLocation >= 0)
			GX2SetVertexUniformBlock(p->vsUniformLocation, (uint32_t)uniformDataSize_, uniformData_);
		if (p->psUniformLocation >= 0)
			GX2SetPixelUniformBlock(p->psUniformLocation, (uint32_t)uniformDataSize_, uniformData_);
	}
}

void GX2DrawContext::Draw(int vertexCount, int offset) {
	if (!curPipeline_ || !vertexData_)
		return;
	ApplyDrawState();
	GX2SetAttribBuffer(0, (uint32_t)vertexDataSize_, curPipeline_->stride, vertexData_);
	GX2DrawEx(curPipeline_->prim, vertexCount, offset, 1);
}

void GX2DrawContext::DrawIndexed(int indexCount, int offset) {
	if (!curPipeline_ || !vertexData_ || !indexData_)
		return;
	ApplyDrawState();
	GX2SetAttribBuffer(0, (uint32_t)vertexDataSize_, curPipeline_->stride, vertexData_);
	// Indices are native (big-endian) u16s.
	GX2DrawIndexedEx(curPipeline_->prim, indexCount, GX2_INDEX_TYPE_U16, (const uint16_t *)indexData_ + offset, 0, 1);
}

void GX2DrawContext::DrawUP(const void *vdata, int vertexCount) {
	if (!curPipeline_ || vertexCount <= 0)
		return;
	size_t size = (size_t)vertexCount * curPipeline_->stride;
	uint8_t *dst = scratch_.Alloc(size, GX2_VERTEX_BUFFER_ALIGNMENT);
	memcpy(dst, vdata, size);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, dst, (uint32_t)size);
	ApplyDrawState();
	GX2SetAttribBuffer(0, (uint32_t)size, curPipeline_->stride, dst);
	GX2DrawEx(curPipeline_->prim, vertexCount, 0, 1);
}

void GX2DrawContext::DrawIndexedUP(const void *vdata, int vertexCount, const void *idata, int indexCount) {
	if (!curPipeline_ || vertexCount <= 0 || indexCount <= 0)
		return;
	size_t vsize = (size_t)vertexCount * curPipeline_->stride;
	uint8_t *v = scratch_.Alloc(vsize, GX2_VERTEX_BUFFER_ALIGNMENT);
	memcpy(v, vdata, vsize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, v, (uint32_t)vsize);
	size_t isize = (size_t)indexCount * sizeof(uint16_t);
	uint8_t *ind = scratch_.Alloc(isize, 64);
	memcpy(ind, idata, isize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, ind, (uint32_t)isize);
	ApplyDrawState();
	GX2SetAttribBuffer(0, (uint32_t)vsize, curPipeline_->stride, v);
	GX2DrawIndexedEx(curPipeline_->prim, indexCount, GX2_INDEX_TYPE_U16, ind, 0, 1);
}

void GX2DrawContext::DrawIndexedClippedBatchUP(const void *vdata, int vertexCount, const void *idata, int indexCount, Slice<ClippedDraw> draws, const void *ub, size_t ubSize) {
	if (draws.is_empty() || !vertexCount || !indexCount)
		return;
	BindPipeline(draws[0].pipeline);
	if (!curPipeline_)
		return;
	size_t vsize = (size_t)vertexCount * curPipeline_->stride;
	uint8_t *v = scratch_.Alloc(vsize, GX2_VERTEX_BUFFER_ALIGNMENT);
	memcpy(v, vdata, vsize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, v, (uint32_t)vsize);
	size_t isize = (size_t)indexCount * sizeof(uint16_t);
	uint16_t *ind = (uint16_t *)scratch_.Alloc(isize, 64);
	memcpy(ind, idata, isize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, ind, (uint32_t)isize);
	UpdateDynamicUniformBuffer(ub, ubSize);

	for (size_t i = 0; i < draws.size(); i++) {
		const ClippedDraw &d = draws[i];
		BindPipeline(d.pipeline);
		if (d.bindTexture) {
			Texture *t = d.bindTexture;
			BindTextures(0, 1, &t, TextureBindFlags::NONE);
		} else if (d.bindFramebufferAsTex) {
			BindFramebufferAsTexture(d.bindFramebufferAsTex, 0, Aspect::COLOR_BIT, 0);
		} else if (d.bindNativeTexture) {
			BindNativeTexture(0, d.bindNativeTexture);
		}
		if (d.samplerState) {
			SamplerState *s = d.samplerState;
			BindSamplerStates(0, 1, &s);
		}
		SetScissorRect(d.clipx, d.clipy, d.clipw, d.cliph);
		ApplyDrawState();
		GX2SetAttribBuffer(0, (uint32_t)vsize, curPipeline_->stride, v);
		GX2DrawIndexedEx(curPipeline_->prim, d.indexCount, GX2_INDEX_TYPE_U16, ind + d.indexOffset, 0, 1);
	}
}

void GX2DrawContext::Clear(Aspect aspects, uint32_t colorval, float depthVal, int stencilVal) {
	GX2ColorBuffer *color = CurColor();
	GX2DepthBuffer *depth = CurDepth();
	if (aspects & Aspect::COLOR_BIT) {
		uint32_t c = colorval;
		GX2ClearColor(color, (c & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, ((c >> 16) & 0xFF) / 255.0f, (c >> 24) / 255.0f);
	}
	if (depth && (aspects & (Aspect::DEPTH_BIT | Aspect::STENCIL_BIT))) {
		uint32_t flags = 0;
		if (aspects & Aspect::DEPTH_BIT) flags |= GX2_CLEAR_FLAGS_DEPTH;
		if (aspects & Aspect::STENCIL_BIT) flags |= GX2_CLEAR_FLAGS_STENCIL;
		GX2ClearDepthStencilEx(depth, depthVal, (uint8_t)stencilVal, (GX2ClearFlags)flags);
	}
	RestoreState();
}

void GX2DrawContext::RestoreState() {
	// GX2's clears and copies leave the GPU state changed. Our context state shadows every
	// register we set, so setting it again restores all of it.
	GX2SetContextState(context_);
	// The target and dynamic state, in case the shadow missed them.
	GX2SetColorBuffer(CurColor(), GX2_RENDER_TARGET_0);
	if (CurDepth())
		GX2SetDepthBuffer(CurDepth());
	GX2SetViewport(viewport_.TopLeftX, viewport_.TopLeftY, viewport_.Width, viewport_.Height, viewport_.MinDepth, viewport_.MaxDepth);
	GX2SetScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
	pipelineDirty_ = true;
}

void GX2DrawContext::BeginFrame(DebugFlags debugFlags) {
	FrameTimeData &frameTimeData = frameTimeHistory_.Add(frameCount_);
	frameTimeData.afterFenceWait = time_now_d();
	frameTimeData.frameBegin = frameTimeData.afterFenceWait;

	// Wait until the last swap has been shown (vsync), at most ~10 refreshes.
	for (int i = 0; i < 10; i++) {
		uint32_t swapCount, flipCount;
		OSTime lastFlip, lastVsync;
		GX2GetSwapStatus(&swapCount, &flipCount, &lastFlip, &lastVsync);
		if (flipCount >= swapCount)
			break;
		GX2WaitForVsync();
	}

	// The previous frame finished on the GPU in Present, so the scratch memory is free.
	scratch_.Reset();
	uniformData_ = nullptr;
	GX2SetContextState(context_);
	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
	GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);
	SetTarget(nullptr);
	pipelineDirty_ = true;
}

void GX2DrawContext::EndFrame() {
	frameTimeHistory_[frameCount_].firstSubmit = time_now_d();
	curPipeline_ = nullptr;
}

void GX2DrawContext::Present(PresentMode presentMode) {
	frameTimeHistory_[frameCount_].queuePresent = time_now_d();
	// The backbuffer is the TV-sized buffer. The GamePad gets the same picture, scaled.
	GX2CopyColorBufferToScanBuffer(backColor_, GX2_SCAN_TARGET_TV);
	GX2CopyColorBufferToScanBuffer(backColor_, GX2_SCAN_TARGET_DRC);
	GX2SwapScanBuffers();
	GX2Flush();
	GX2DrawDone();  // Frees this frame's scratch memory for the next one.
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
	GX2SetContextState(context_);
	frameCount_++;
}

DrawContext *T3DCreateGX2Context(void *tvColorBuffer, void *tvDepthBuffer) {
	return new GX2DrawContext((GX2ColorBuffer *)tvColorBuffer, (GX2DepthBuffer *)tvDepthBuffer);
}

}  // namespace Draw

#endif  // PPSSPP_API(GX2)
