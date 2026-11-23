// 64-bit atomics for the Wii U.
//
// The Espresso CPU (32-bit PowerPC) has no 64-bit atomic instructions, so GCC emits calls
// to these libatomic helpers, and devkitPPC ships no libatomic. They're implemented here
// with one cross-core spinlock from coreinit, which also keeps interrupts off while held.
// 64-bit atomics are rare in PPSSPP (counters, timestamps), so one lock is plenty.

#if defined(__WIIU__)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <coreinit/spinlock.h>

static OSSpinLock g_atomic64Lock;  // Zero-initialized, which is an unlocked spinlock.

#define LOCK() OSUninterruptibleSpinLock_Acquire(&g_atomic64Lock)
#define UNLOCK() OSUninterruptibleSpinLock_Release(&g_atomic64Lock)

uint64_t __atomic_load_8(const volatile void *ptr, int order) {
	(void)order;
	LOCK();
	uint64_t v = *(const volatile uint64_t *)ptr;
	UNLOCK();
	return v;
}

void __atomic_store_8(volatile void *ptr, uint64_t val, int order) {
	(void)order;
	LOCK();
	*(volatile uint64_t *)ptr = val;
	UNLOCK();
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t val, int order) {
	(void)order;
	LOCK();
	uint64_t old = *(volatile uint64_t *)ptr;
	*(volatile uint64_t *)ptr = val;
	UNLOCK();
	return old;
}

bool __atomic_compare_exchange_8(volatile void *ptr, void *expected, uint64_t desired, bool weak, int success, int failure) {
	(void)weak; (void)success; (void)failure;
	LOCK();
	uint64_t cur = *(volatile uint64_t *)ptr;
	uint64_t exp;
	memcpy(&exp, expected, sizeof(exp));
	bool ok = cur == exp;
	if (ok)
		*(volatile uint64_t *)ptr = desired;
	else
		memcpy(expected, &cur, sizeof(cur));
	UNLOCK();
	return ok;
}

#define FETCH_OP(name, op) \
	uint64_t __atomic_fetch_##name##_8(volatile void *ptr, uint64_t val, int order) { \
		(void)order; \
		LOCK(); \
		uint64_t old = *(volatile uint64_t *)ptr; \
		*(volatile uint64_t *)ptr = old op val; \
		UNLOCK(); \
		return old; \
	} \
	uint64_t __atomic_##name##_fetch_8(volatile void *ptr, uint64_t val, int order) { \
		(void)order; \
		LOCK(); \
		uint64_t v = *(volatile uint64_t *)ptr op val; \
		*(volatile uint64_t *)ptr = v; \
		UNLOCK(); \
		return v; \
	}

FETCH_OP(add, +)
FETCH_OP(sub, -)
FETCH_OP(and, &)
FETCH_OP(or, |)
FETCH_OP(xor, ^)

#endif  // __WIIU__
