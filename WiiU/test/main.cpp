// PPSSPP Wii U port: hardware test.
//
// Part 1 (text on the screens): runs the checks below and writes them to
// sd:/ppsspp_wiiu_test.txt too. Press A to continue.
// Part 2 (GX2): a spinning triangle drawn with shaders compiled on the console by CafeGLSL.
// Its tint comes from a uniform block written in little-endian byte order:
//   colored triangle -> uniform blocks want little-endian data (expected)
//   black triangle   -> they want native (big-endian) data
// HOME exits.

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/shaders.h>
#include <gx2/registers.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

#include <cafeglsl/CafeGLSLCompiler.h>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Common/Thread/ThreadLocal.h"

static std::string g_report;

static void Report(const char *fmt, ...) {
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	WHBLogPrint(buf);
	g_report += buf;
	g_report += "\n";
}

static bool Check(const char *name, bool ok, const std::string &detail = "") {
	Report("[%s] %s%s%s", ok ? " OK " : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
	return ok;
}

// --- Checks ---

struct Counter { int value = 100; };

static bool TestThreadLocal() {
	PPSSPP_THREAD_LOCAL(Counter, t_counter);
	PPSSPP_TLS_GET(t_counter).value = 1;
	int other = -1;
	std::thread t([&] { other = PPSSPP_TLS_GET(t_counter).value; PPSSPP_TLS_GET(t_counter).value = 2; });
	t.join();
	int mine = PPSSPP_TLS_GET(t_counter).value;
	char d[64];
	snprintf(d, sizeof(d), "main=%d other=%d (want 1, 100)", mine, other);
	return Check("thread-local emulation", mine == 1 && other == 100, d);
}

static bool TestAtomic64CAS() {
	std::atomic<uint64_t> value{0x100000000ULL};
	uint64_t expected = 0x100000000ULL;
	bool cas = value.compare_exchange_strong(expected, 0x123456789ULL);
	uint64_t wrong = 1;
	bool casFail = !value.compare_exchange_strong(wrong, 0) && wrong == 0x123456789ULL;
	uint64_t old = value.exchange(0xFFFFFFFF00000000ULL);
	char d[96];
	snprintf(d, sizeof(d), "cas=%d casFail=%d exchange=%d", cas, casFail, old == 0x123456789ULL);
	return Check("64-bit compare-exchange", cas && casFail && old == 0x123456789ULL && value.load() == 0xFFFFFFFF00000000ULL, d);
}

static bool TestSum() {
	std::atomic<uint64_t> value{0};
	std::vector<std::thread> pool;
	for (int i = 0; i < 3; i++)
		pool.emplace_back([&] { for (int j = 0; j < 100000; j++) value.fetch_add(1); });
	for (auto &t : pool)
		t.join();
	char d[64];
	snprintf(d, sizeof(d), "%llu (want 300000)", (unsigned long long)value.load());
	return Check("64-bit atomic counter", value.load() == 300000, d);
}

static bool TestLittleEndian() {
	u32_le v = 0x11223344;
	const u8 *bytes = (const u8 *)&v;
	bool layout = bytes[0] == 0x44 && bytes[3] == 0x11;
	bool value = (u32)v == 0x11223344;
	v += 1;
	float_le f = 1.5f;
	bool fl = (float)f == 1.5f;
	char d[96];
	snprintf(d, sizeof(d), "bytes %02x..%02x, +1=%08x, float ok=%d", bytes[0], bytes[3], (u32)v, fl);
	return Check("u32_le / float_le", layout && value && (u32)v == 0x11223345 && fl, d);
}

// --- Shaders ---

static const char *kVertexShader = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 0) out vec3 vColor;
void main() {
	vColor = aColor;
	gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char *kPixelShader = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 1) uniform Tint {
	vec4 tint;
};
void main() {
	fragColor = vec4(vColor * tint.rgb, 1.0);
}
)";

static GX2VertexShader *g_vs;
static GX2PixelShader *g_ps;
static GX2ShaderMode g_shaderMode;

static bool CompileShaders() {
	InitGLSLCompiler();
	Report("CafeGLSL %s", GetGLSLCompilerVersion());
	char log[2048] = {};
	g_shaderMode = CompileShaderPair(kVertexShader, kPixelShader, GLSL_COMPILE_BLOCK, &g_vs, &g_ps, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
	bool ok = g_shaderMode != GLSL_SHADER_MODE_ERROR && g_vs && g_ps;
	char d[128];
	if (ok)
		snprintf(d, sizeof(d), "mode=%d, vs attribs=%u, ps blocks=%u", (int)g_shaderMode, (unsigned)g_vs->attribVarCount, (unsigned)g_ps->uniformBlockCount);
	else
		snprintf(d, sizeof(d), "see log below");
	Check("CafeGLSL compile on console", ok, d);
	if (log[0])
		Report("%s", log);
	return ok;
}

static void WriteReportToSD() {
	FILE *f = fopen("fs:/vol/external01/ppsspp_wiiu_test.txt", "w");
	if (f) {
		fwrite(g_report.data(), 1, g_report.size(), f);
		fclose(f);
		Report("Report saved to sd:/ppsspp_wiiu_test.txt");
	} else {
		Report("Could not write sd:/ppsspp_wiiu_test.txt");
	}
}

static bool PressedA() {
	VPADStatus status;
	VPADReadError err;
	if (VPADRead(VPAD_CHAN_0, &status, 1, &err) > 0 && err == VPAD_READ_SUCCESS)
		return (status.trigger & VPAD_BUTTON_A) != 0;
	return false;
}

static u32 SwapFloatBits(float f) {
	u32 bits;
	memcpy(&bits, &f, 4);
	return __builtin_bswap32(bits);
}

int main(int argc, char **argv) {
	WHBProcInit();
	WHBLogUdpInit();
	WHBLogConsoleInit();

	Report("PPSSPP Wii U port - hardware test");
	Report("---------------------------------");
	bool allOk = true;
	allOk &= TestLittleEndian();
	allOk &= TestThreadLocal();
	allOk &= TestSum();
	allOk &= TestAtomic64CAS();
	bool shadersOk = CompileShaders();
	allOk &= shadersOk;
	Report("---------------------------------");
	Report(allOk ? "All checks passed." : "Some checks FAILED.");
	WriteReportToSD();
	Report("");
	Report(shadersOk ? "Press A for the GX2 triangle, HOME to exit." : "HOME to exit.");

	bool toGfx = false;
	while (WHBProcIsRunning()) {
		WHBLogConsoleDraw();
		if (shadersOk && PressedA()) {
			toGfx = true;
			break;
		}
		OSSleepTicks(OSMillisecondsToTicks(16));
	}
	WHBLogConsoleFree();

	if (toGfx && WHBGfxInit()) {
		WHBGfxShaderGroup group = {};
		group.vertexShader = g_vs;
		group.pixelShader = g_ps;
		WHBGfxInitShaderAttribute(&group, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
		WHBGfxInitShaderAttribute(&group, "aColor", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32_32);
		WHBGfxInitFetchShader(&group);

		const uint32_t stride = 5 * sizeof(float);
		float *verts = (float *)MEMAllocFromDefaultHeapEx(3 * stride, GX2_VERTEX_BUFFER_ALIGNMENT);
		u32 *tint = (u32 *)MEMAllocFromDefaultHeapEx(GX2_UNIFORM_BLOCK_ALIGNMENT, GX2_UNIFORM_BLOCK_ALIGNMENT);
		// Little-endian on purpose, see the top of the file.
		tint[0] = SwapFloatBits(1.0f);
		tint[1] = SwapFloatBits(1.0f);
		tint[2] = SwapFloatBits(1.0f);
		tint[3] = SwapFloatBits(1.0f);
		GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), tint, GX2_UNIFORM_BLOCK_ALIGNMENT);
		uint32_t tintLocation = g_ps->uniformBlockCount ? g_ps->uniformBlocks[0].offset : 1;

		float angle = 0.0f;
		while (WHBProcIsRunning()) {
			static const float colors[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			for (int i = 0; i < 3; i++) {
				float a = angle + i * 2.0943951f;
				verts[i * 5 + 0] = 0.7f * sinf(a);
				verts[i * 5 + 1] = 0.7f * cosf(a);
				verts[i * 5 + 2] = colors[i][0];
				verts[i * 5 + 3] = colors[i][1];
				verts[i * 5 + 4] = colors[i][2];
			}
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, verts, 3 * stride);
			angle += 0.02f;

			WHBGfxBeginRender();
			for (int screen = 0; screen < 2; screen++) {
				if (screen == 0)
					WHBGfxBeginRenderTV();
				else
					WHBGfxBeginRenderDRC();
				WHBGfxClearColor(0.1f, 0.1f, 0.2f, 1.0f);
				GX2SetShaderMode(g_shaderMode);
				GX2SetFetchShader(&group.fetchShader);
				GX2SetVertexShader(g_vs);
				GX2SetPixelShader(g_ps);
				GX2SetPixelUniformBlock(tintLocation, 16, tint);
				GX2SetAttribBuffer(0, 3 * stride, stride, verts);
				GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1);
				if (screen == 0)
					WHBGfxFinishRenderTV();
				else
					WHBGfxFinishRenderDRC();
			}
			WHBGfxFinishRender();
		}

		MEMFreeToDefaultHeap(verts);
		MEMFreeToDefaultHeap(tint);
		WHBGfxShutdown();
	}

	FreeShaders(g_vs, g_ps);
	DestroyGLSLCompiler();
	WHBLogUdpDeinit();
	WHBProcShutdown();
	return 0;
}
