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
#include "Common/WiiUFileIO.h"
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <mutex>
#if PPSSPP_PLATFORM(WIIU)

// Memory arena for the Wii U, on coreinit's memory mapping calls.
//
// The arena is one block of the app's MEM2 heap (physically contiguous). Find4GBBase reserves a
// range of virtual addresses, and each view maps a piece of the block's physical memory into it,
// so the same memory can show up at several addresses (the PSP's mirrors). Mapping works in
// 128 KB pages (OS_PAGE_SIZE): a view that doesn't start on a page boundary (the scratchpad at
// 0x10000) maps its whole page, and its data sits at the same offset within the arena's page.
// The old aliaspider port did the same.

#include <cstdint>
#include <cstdlib>
#include <malloc.h>

#include <coreinit/memorymap.h>

#include "Common/Log.h"
#include "Common/MemArena.h"

static uint8_t *g_block = nullptr;      // The arena's memory, in MEM2.
static uint32_t g_blockPhys = 0;
static size_t g_blockSize = 0;
static uint32_t g_reserved = 0;          // The reserved virtual range for the views.
static uint32_t g_reservedSize = 0;

static uint32_t PageDown(uint32_t x) { return x & ~(OS_PAGE_SIZE - 1); }
static uint32_t PageUp(uint32_t x) { return (x + OS_PAGE_SIZE - 1) & ~(OS_PAGE_SIZE - 1); }

size_t MemArena::roundup(size_t x) {
	return PageUp((uint32_t)x);
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
	g_blockSize = roundup(size);
	g_block = (uint8_t *)memalign(OS_PAGE_SIZE, g_blockSize);
	if (!g_block) {
		ERROR_LOG(Log::MemMap, "MemArena: failed to allocate %u bytes", (unsigned)g_blockSize);
		return false;
	}
	g_blockPhys = OSEffectiveToPhysical((uint32_t)g_block);
	INFO_LOG(Log::MemMap, "MemArena: %u bytes at %p (physical %08x)", (unsigned)g_blockSize, g_block, g_blockPhys);
	return true;
}

void MemArena::ReleaseSpace() {
	if (g_reserved) {
		OSFreeVirtAddr(g_reserved, g_reservedSize);
		g_reserved = 0;
		g_reservedSize = 0;
	}
	free(g_block);
	g_block = nullptr;
	g_blockPhys = 0;
	g_blockSize = 0;
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
	const uint32_t addr = (uint32_t)(uintptr_t)base;
	const uint32_t page = PageDown(addr);
	const uint32_t within = addr - page;
	const uint32_t mapSize = PageUp(within + (uint32_t)size);
	if ((uint64_t)offset + mapSize > g_blockSize + within) {
		ERROR_LOG(Log::MemMap, "MemArena: view at %08x (offset %08x, size %08x) is outside the arena", addr, (uint32_t)offset, (uint32_t)size);
		return nullptr;
	}
	// The arena offset is page aligned; the data for addr lands at offset + within.
	if (!OSMapMemory(page, g_blockPhys + (uint32_t)offset, mapSize, OS_MAP_MEMORY_READ_WRITE)) {
		ERROR_LOG(Log::MemMap, "MemArena: OSMapMemory(%08x, phys %08x, %08x) failed", page, g_blockPhys + (uint32_t)offset, mapSize);
		return nullptr;
	}
	return base;
}

void MemArena::ReleaseView(s64 offset, void *view, size_t size) {
	const uint32_t addr = (uint32_t)(uintptr_t)view;
	const uint32_t page = PageDown(addr);
	OSUnmapMemory(page, PageUp(addr - page + (uint32_t)size));
}

u8 *MemArena::Find4GBBase() {
	// On 32-bit hosts the views are masked into 256 MB (MEMVIEW32_MASK). Keep the reservation:
	// OSMapMemory needs the range allocated, and nothing else should grab it in the meantime.
	if (!g_reserved) {
		g_reservedSize = 0x10000000;
		g_reserved = OSAllocVirtAddr(0, g_reservedSize, OS_PAGE_SIZE);
		if (!g_reserved) {
			ERROR_LOG(Log::MemMap, "MemArena: couldn't reserve 256 MB of address space");
			g_reservedSize = 0;
			return nullptr;
		}
		INFO_LOG(Log::MemMap, "MemArena: reserved address space at %08x", g_reserved);
	}
	return (u8 *)(uintptr_t)g_reserved;
}

// --- File IO into the mapped views (see WiiUFileIO.h) ---

bool WiiU_IsMappedView(const void *ptr, size_t size) {
	const uint32_t p = (uint32_t)(uintptr_t)ptr;
	return g_reserved != 0 && p + size > g_reserved && p < g_reserved + g_reservedSize;
}

static std::mutex g_bounceLock;
static constexpr size_t BOUNCE_SIZE = 256 * 1024;
static uint8_t *g_bounce = nullptr;

static uint8_t *Bounce() {
	if (!g_bounce)
		g_bounce = (uint8_t *)memalign(0x40, BOUNCE_SIZE);
	return g_bounce;
}

ssize_t WiiU_FileRead(int fd, void *data, size_t size) {
	if (!WiiU_IsMappedView(data, size))
		return read(fd, data, size);
	std::lock_guard<std::mutex> guard(g_bounceLock);
	uint8_t *bounce = Bounce();
	if (!bounce)
		return -1;
	size_t done = 0;
	while (done < size) {
		const size_t chunk = std::min(BOUNCE_SIZE, size - done);
		const ssize_t got = read(fd, bounce, chunk);
		if (got < 0)
			return done ? (ssize_t)done : got;
		memcpy((uint8_t *)data + done, bounce, got);
		done += got;
		if ((size_t)got < chunk)
			break;
	}
	return (ssize_t)done;
}

ssize_t WiiU_FileWrite(int fd, const void *data, size_t size) {
	if (!WiiU_IsMappedView(data, size))
		return write(fd, data, size);
	std::lock_guard<std::mutex> guard(g_bounceLock);
	uint8_t *bounce = Bounce();
	if (!bounce)
		return -1;
	size_t done = 0;
	while (done < size) {
		const size_t chunk = std::min(BOUNCE_SIZE, size - done);
		memcpy(bounce, (const uint8_t *)data + done, chunk);
		const ssize_t put = write(fd, bounce, chunk);
		if (put < 0)
			return done ? (ssize_t)done : put;
		done += put;
		if ((size_t)put < chunk)
			break;
	}
	return (ssize_t)done;
}

#endif  // PPSSPP_PLATFORM(WIIU)
