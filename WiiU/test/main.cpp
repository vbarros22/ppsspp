// PPSSPP Wii U port: hardware test.
//
// Runs by itself, in stages, and appends each stage it reaches to sd:/ppsspp_wiiu_test.txt,
// so a black screen still tells us how far it got.
//   1. Text (OSScreen): the checks below. 8 seconds, or A to skip.
//   2. GX2 clear only: the background fades between grey and white.
//   3. Triangle, shaders without uniforms. Dark green background.
//   4. Triangle tinted by a uniform block in little-endian order. Dark blue background.
//   5. Same, uniform block in native (big-endian) order. Dark red background.
// Stages 2-5 last 5 seconds each and then repeat. HOME exits.
//
// In 4 and 5, one of the two triangles should be colored and the other black: the colored
// one tells which byte order GX2 uniform blocks want.

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

static const char *kReportPath = "fs:/vol/external01/ppsspp_wiiu_test.txt";

// Appends a line to the report on the SD card right away (open, write, close), so it
// survives a hang.
static void Progress(const char *fmt, ...) {
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	WHBLogPrint(buf);
	FILE *f = fopen(kReportPath, "a");
	if (f) {
		fprintf(f, "%s\n", buf);
		fclose(f);
	}
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

static const char *kPixelShaderPlain = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fragColor;
void main() {
	fragColor = vec4(vColor, 1.0);
}
)";

static const char *kPixelShaderTint = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 1) uniform Tint {
	vec4 tint;
};
void main() {
	fragColor = vec4(vColor * tint.rgb, 1.0);
}
)";

struct Program {
	GX2VertexShader *vs = nullptr;
	GX2PixelShader *ps = nullptr;
	GX2ShaderMode mode = GLSL_SHADER_MODE_ERROR;
	WHBGfxShaderGroup group = {};
};

static Program g_plain, g_tint;

static bool CompileProgram(Program &prog, const char *name, const char *ps, GLSLCompileMode mode) {
	char log[2048] = {};
	prog.mode = CompileShaderPair(kVertexShader, ps, mode, &prog.vs, &prog.ps, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
	bool ok = prog.mode != GLSL_SHADER_MODE_ERROR && prog.vs && prog.ps;
	char d[128];
	if (ok)
		snprintf(d, sizeof(d), "mode=%d, vs attribs=%u, ps blocks=%u", (int)prog.mode, (unsigned)prog.vs->attribVarCount, (unsigned)prog.ps->uniformBlockCount);
	else
		snprintf(d, sizeof(d), "see log below");
	Check(name, ok, d);
	if (log[0])
		Report("%s", log);
	return ok;
}

static bool CompileShaders() {
	InitGLSLCompiler();
	Report("CafeGLSL %s", GetGLSLCompilerVersion());
	bool ok = CompileProgram(g_plain, "CafeGLSL compile (no uniforms)", kPixelShaderPlain, GLSL_COMPILE_AUTO);
	ok &= CompileProgram(g_tint, "CafeGLSL compile (uniform block)", kPixelShaderTint, GLSL_COMPILE_BLOCK);
	return ok;
}

static void WriteReportToSD() {
	FILE *f = fopen(kReportPath, "w");
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

static u32 NativeFloatBits(float f) {
	u32 bits;
	memcpy(&bits, &f, 4);
	return bits;
}

static bool SetupGroup(Program &prog) {
	prog.group.vertexShader = prog.vs;
	prog.group.pixelShader = prog.ps;
	bool ok = WHBGfxInitShaderAttribute(&prog.group, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
	ok = WHBGfxInitShaderAttribute(&prog.group, "aColor", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32_32) && ok;
	ok = WHBGfxInitFetchShader(&prog.group) && ok;
	return ok;
}

// One check per press of A, with the text screen drawn in between, so a check that kills the
// video shows up as the last line still visible. Each step is also appended to the SD report.
struct Step {
	const char *name;
	bool (*run)();
};

static bool StepUdp() { WHBLogUdpInit(); return true; }
static bool StepCafeInit() { InitGLSLCompiler(); Report("CafeGLSL %s", GetGLSLCompilerVersion()); return true; }
static bool StepCompilePlain() { return CompileProgram(g_plain, "CafeGLSL compile (no uniforms)", kPixelShaderPlain, GLSL_COMPILE_AUTO); }
static bool StepCompileTint() { return CompileProgram(g_tint, "CafeGLSL compile (uniform block)", kPixelShaderTint, GLSL_COMPILE_BLOCK); }

static const Step kSteps[] = {
	{ "u32_le / float_le", TestLittleEndian },
	{ "thread-local emulation (1 thread)", TestThreadLocal },
	{ "64-bit compare-exchange (no threads)", TestAtomic64CAS },
	{ "64-bit atomic counter (3 threads)", TestSum },
	{ "UDP log init", StepUdp },
	{ "CafeGLSL init", StepCafeInit },
	{ "CafeGLSL compile, no uniforms", StepCompilePlain },
	{ "CafeGLSL compile, uniform block", StepCompileTint },
};

static void DrawFor(int ms) {
	OSTime start = OSGetSystemTime();
	while (WHBProcIsRunning() && OSTicksToMilliseconds(OSGetSystemTime() - start) < ms) {
		WHBLogConsoleDraw();
		OSSleepTicks(OSMillisecondsToTicks(16));
	}
}

int main(int argc, char **argv) {
	WHBProcInit();
	WHBLogConsoleInit();

	FILE *f = fopen(kReportPath, "w");
	if (f)
		fclose(f);

	Report("PPSSPP Wii U port - all checks before the first frame (diagnostic 4)");
	Progress("start: console initialized, nothing drawn yet");
	const int total = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
#ifdef CHECKS_BEFORE_FIRST_FRAME
	// Teste G: the checks run before anything is drawn, like the first test app, except
	// for the UDP log, which Teste F covers.
	for (int i = 0; i < total; i++) {
		if (kSteps[i].run == StepUdp)
			continue;
		Progress("step %d (%s): starting", i + 1, kSteps[i].name);
		bool ok = kSteps[i].run();
		Progress("step %d (%s): %s", i + 1, kSteps[i].name, ok ? "ok" : "FAILED");
		Report("Step %d: %s: %s", i + 1, kSteps[i].name, ok ? "ok" : "FAILED");
	}
	Report("All checks ran before the first frame.");
	Report("If you can read this, they don't kill the video. HOME to exit.");
	Progress("first frame now");
#endif

	const int count = total;
#ifdef CHECKS_BEFORE_FIRST_FRAME
	int next = count;
#else
	int next = 0;
#endif
	while (WHBProcIsRunning()) {
		WHBLogConsoleDraw();
		if (next < count && PressedA()) {
			Report("");
			Report("Step %d/%d: %s ...", next + 1, count, kSteps[next].name);
			DrawFor(300);  // Make sure this line reaches the screen before the step runs.
			Progress("step %d (%s): starting", next + 1, kSteps[next].name);
			bool ok = kSteps[next].run();
			Progress("step %d (%s): %s", next + 1, kSteps[next].name, ok ? "ok" : "FAILED");
			Report("Step %d done: %s. %s", next + 1, ok ? "ok" : "FAILED", next + 1 < count ? "A: next" : "All steps done, HOME to exit.");
			next++;
		}
		OSSleepTicks(OSMillisecondsToTicks(16));
	}
	Progress("exit requested");

	FreeShaders(g_plain.vs, g_plain.ps);
	FreeShaders(g_tint.vs, g_tint.ps);
	WHBLogConsoleFree();
	WHBProcShutdown();
	return 0;
}
