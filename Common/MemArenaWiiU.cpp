// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include "ppsspp_config.h"
#if PPSSPP_PLATFORM(WIIU)

// The Wii U build uses NO_MMAP (see CMakeLists.txt): PSP memory is a single plain
// allocation with masked addressing, so no views are ever created. These stubs only
// exist so MemArena links. A real implementation could use OSAllocVirtAddr/OSMapMemory
// from coreinit/memorymap.h, like the old aliaspider port did.

#include "Common/MemArena.h"

size_t MemArena::roundup(size_t x) {
	return x;
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
	return false;
}

void MemArena::ReleaseSpace() {
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
	return nullptr;
}

void MemArena::ReleaseView(s64 offset, void *view, size_t size) {
}

u8 *MemArena::Find4GBBase() {
	return nullptr;
}

#endif  // PPSSPP_PLATFORM(WIIU)
