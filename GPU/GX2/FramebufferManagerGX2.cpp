// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#include "Common/GPU/thin3d.h"
#include "GPU/Common/PresentationCommon.h"
#include "GPU/GX2/FramebufferManagerGX2.h"

FramebufferManagerGX2::FramebufferManagerGX2(Draw::DrawContext *draw)
	: FramebufferManagerCommon(draw) {
	presentation_->SetLanguage(GLSL_VULKAN);
}
