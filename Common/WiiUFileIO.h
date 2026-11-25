// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later.

#pragma once

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(WIIU)
#include <cstddef>
#include <sys/types.h>

// File reads and writes on the Wii U go to the IOP by DMA, to the physical address of the
// buffer. PSP memory is mapped a second time (OSMapMemory, see MemArenaWiiU.cpp), and DMA to
// and from those views doesn't reach the right memory. These go through a bounce buffer
// whenever the buffer is in the mapped views.
ssize_t WiiU_FileRead(int fd, void *data, size_t size);
ssize_t WiiU_FileWrite(int fd, const void *data, size_t size);
bool WiiU_IsMappedView(const void *ptr, size_t size);
#endif
