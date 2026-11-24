// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later, see the other GPU backends.

#pragma once

#include "GPU/Common/FramebufferManagerCommon.h"
#include "Common/GPU/thin3d.h"

// Framebuffers are all thin3d (Common/GPU/GX2/thin3d_gx2.cpp), like on D3D11.
class FramebufferManagerGX2 : public FramebufferManagerCommon {
public:
	FramebufferManagerGX2(Draw::DrawContext *draw);
};
