#pragma once

// Portable thread-local variables.
//
//   PPSSPP_THREAD_LOCAL(std::vector<int>, scratch);   // where you'd write "thread_local"
//   std::vector<int> &v = PPSSPP_TLS_GET(scratch);
//
// Everywhere but the Wii U this is plain thread_local. The Wii U toolchain (wut) has no TLS
// support at all (RPX executables can't carry TLS relocations), so there each variable is a
// heap object per thread, looked up by the current OSThread. Those objects are never freed
// when a thread exits, so keep this to a handful of small variables, and don't rely on
// their destructors running.

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(WIIU)

#include <mutex>
#include <unordered_map>

#include <coreinit/thread.h>

template <typename T>
class EmulatedThreadLocal {
public:
	T &get() {
		OSThread *thread = OSGetCurrentThread();
		std::lock_guard<std::mutex> guard(lock_);
		T *&value = values_[thread];
		if (!value)
			value = new T();
		return *value;
	}

private:
	std::mutex lock_;
	std::unordered_map<OSThread *, T *> values_;
};

#define PPSSPP_THREAD_LOCAL(type, name) static EmulatedThreadLocal<type> name
#define PPSSPP_TLS_GET(name) ((name).get())

#else

#define PPSSPP_THREAD_LOCAL(type, name) static thread_local type name
#define PPSSPP_TLS_GET(name) (name)

#endif
