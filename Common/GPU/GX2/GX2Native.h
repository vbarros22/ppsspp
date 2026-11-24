// What the GX2 hardware GPU backend (GPU/GX2) needs from the GX2 thin3d context.
//
// The GPU backend draws with GX2 directly (its own shaders, vertex formats and state), on top of
// what thin3d set up: the render target, viewport and scissor, and the textures bound through
// thin3d (framebuffers, the texture cache's textures, the CLUT). Get this with
// draw->GetNativeObject(Draw::NativeObject::CONTEXT).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <gx2/sampler.h>
#include <gx2/texture.h>

class GX2NativeContext {
public:
	virtual ~GX2NativeContext() {}

	// GPU-visible memory valid until the end of the frame (the GPU is done with it at Present).
	// Flushed for the GPU by the caller (GX2Invalidate).
	virtual uint8_t *AllocScratch(size_t size, size_t align) = 0;
	virtual uint32_t ScratchGeneration() const = 0;

	// What thin3d currently has bound in a texture slot (nullptr if nothing).
	virtual const GX2Texture *BoundTexture(int slot) const = 0;
	virtual const GX2Sampler *BoundSampler(int slot) const = 0;

	// Whether the current render target has a depth/stencil buffer.
	virtual bool TargetHasDepth() const = 0;

	// Call after drawing with GX2 directly: thin3d sets all of its state again on its next draw.
	virtual void ExternalStateChanged() = 0;
};

// Vulkan GLSL -> the GLSL 4.50 that CafeGLSL (Mesa, OpenGL rules) accepts: drop the descriptor set
// qualifiers, map the Vulkan-only builtins. Bindings and locations carry over unchanged.
std::string GX2PreprocessVulkanGLSL(const char *src, size_t size);
