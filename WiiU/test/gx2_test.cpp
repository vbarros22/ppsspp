// GX2 test for the Wii U port. Lessons from running it on hardware:
//  - Shader programs from CafeGLSL must be flushed (GX2Invalidate CPU_SHADER) before the
//    first draw, or the GPU runs a stale program and hangs, taking the video down with it.
//  - Drain the GPU (GX2DrawDone) in the ProcUI release callback. Once the foreground is
//    gone (after the main loop) GX2 calls block forever, so exiting through HOME hangs.
//  - Uniform block data is little-endian (stage 3 is colored, stage 4 is black).
// One stage per press of A. Each stage is appended to sd:/ppsspp_wiiu_gx2_test.txt as it starts.
//   1. clear only, background fades grey/white
//   2. triangle, shaders without uniforms, dark green background
//   3. triangle tinted by a little-endian uniform block, dark blue background
//   4. same with a native (big-endian) uniform block, dark red background
// HOME exits.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <proc_ui/procui.h>
#include <gx2/mem.h>
#include <gx2/shaders.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>

#include <cafeglsl/CafeGLSLCompiler.h>

static const char *kLog = "fs:/vol/external01/ppsspp_wiiu_gx2_test.txt";

static void Log(const char *fmt, ...) {
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	FILE *f = fopen(kLog, "a");
	if (f) {
		fprintf(f, "%s\n", buf);
		fclose(f);
	}
}

static const char *kVS = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 0) out vec3 vColor;
void main() {
	vColor = aColor;
	gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char *kPSPlain = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 fragColor;
void main() {
	fragColor = vec4(vColor, 1.0);
}
)";

static const char *kPSTint = R"(#version 450
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

static bool Build(Program &p, const char *ps, GLSLCompileMode mode) {
	char log[1024] = {};
	p.mode = CompileShaderPair(kVS, ps, mode, &p.vs, &p.ps, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
	if (p.mode == GLSL_SHADER_MODE_ERROR) {
		Log("compile failed: %s", log);
		return false;
	}
	// CafeGLSL writes the shader programs with the CPU: flush them to memory before the GPU
	// reads them. Without this the first draw runs a stale program and hangs the GPU.
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.vs->program, p.vs->size);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.ps->program, p.ps->size);
	p.group.vertexShader = p.vs;
	p.group.pixelShader = p.ps;
	bool ok = WHBGfxInitShaderAttribute(&p.group, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
	ok = WHBGfxInitShaderAttribute(&p.group, "aColor", 0, 8, GX2_ATTRIB_FORMAT_FLOAT_32_32_32) && ok;
	ok = WHBGfxInitFetchShader(&p.group) && ok;
	Log("program: mode=%d, setup %s", (int)p.mode, ok ? "ok" : "FAILED");
	return ok;
}

// Runs when the app is about to lose the foreground (HOME menu, or closing): still allowed to
// use GX2 here, so wait for the GPU before anything gets freed. After the main loop it's too
// late, GX2 calls block forever once the foreground is gone.
static uint32_t OnRelease(void *) {
	GX2DrawDone();
	Log("release: GPU idle");
	return 0;
}

static bool PressedA() {
	VPADStatus status;
	VPADReadError err;
	if (VPADRead(VPAD_CHAN_0, &status, 1, &err) > 0 && err == VPAD_READ_SUCCESS)
		return (status.trigger & VPAD_BUTTON_A) != 0;
	return false;
}

static uint32_t FloatBits(float f, bool swap) {
	uint32_t bits;
	memcpy(&bits, &f, 4);
	return swap ? __builtin_bswap32(bits) : bits;
}

int main(int argc, char **argv) {
	WHBProcInit();
	FILE *f = fopen(kLog, "w");
	if (f)
		fclose(f);

	WHBGfxInit();
	Log("WHBGfxInit done");
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, OnRelease, nullptr, 200);
	InitGLSLCompiler();
	Program plain, tint;
	bool ok = Build(plain, kPSPlain, GLSL_COMPILE_AUTO);
	ok = Build(tint, kPSTint, GLSL_COMPILE_BLOCK) && ok;

	const uint32_t stride = 5 * sizeof(float);
	float *verts = (float *)MEMAllocFromDefaultHeapEx(3 * stride, GX2_VERTEX_BUFFER_ALIGNMENT);
	uint32_t *tintLE = (uint32_t *)MEMAllocFromDefaultHeapEx(GX2_UNIFORM_BLOCK_ALIGNMENT, GX2_UNIFORM_BLOCK_ALIGNMENT);
	uint32_t *tintNative = (uint32_t *)MEMAllocFromDefaultHeapEx(GX2_UNIFORM_BLOCK_ALIGNMENT, GX2_UNIFORM_BLOCK_ALIGNMENT);
	for (int i = 0; i < 4; i++) {
		tintLE[i] = FloatBits(1.0f, true);
		tintNative[i] = FloatBits(1.0f, false);
	}
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), tintLE, GX2_UNIFORM_BLOCK_ALIGNMENT);
	GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), tintNative, GX2_UNIFORM_BLOCK_ALIGNMENT);
	uint32_t tintLocation = tint.ps && tint.ps->uniformBlockCount ? tint.ps->uniformBlocks[0].offset : 1;
	Log("setup %s, tint location %u", ok ? "ok" : "FAILED", (unsigned)tintLocation);

	static const char *names[] = { "", "clear only", "triangle, no uniforms", "triangle, little-endian uniform block", "triangle, native uniform block" };
	int stage = 1;
	int frames = 0;
	Log("stage 1: %s", names[1]);
	OSTime start = OSGetSystemTime();
	float angle = 0.0f;

	while (WHBProcIsRunning()) {
		if (PressedA() && ok) {
			Log("stage %d: %d frames", stage, frames);
			stage = stage % 4 + 1;
			frames = 0;
			Log("stage %d: %s", stage, names[stage]);
		}

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

		float bg[3];
		int ms = (int)OSTicksToMilliseconds(OSGetSystemTime() - start);
		switch (stage) {
		case 1: { float t = 0.5f + 0.5f * sinf(ms * 0.003f); bg[0] = bg[1] = bg[2] = 0.3f + 0.7f * t; break; }
		case 2: bg[0] = 0.0f; bg[1] = 0.25f; bg[2] = 0.0f; break;
		case 3: bg[0] = 0.0f; bg[1] = 0.0f; bg[2] = 0.3f; break;
		default: bg[0] = 0.3f; bg[1] = 0.0f; bg[2] = 0.0f; break;
		}

		WHBGfxBeginRender();
		for (int screen = 0; screen < 2; screen++) {
			if (screen == 0)
				WHBGfxBeginRenderTV();
			else
				WHBGfxBeginRenderDRC();
			WHBGfxClearColor(bg[0], bg[1], bg[2], 1.0f);
			if (stage >= 2) {
				Program &p = stage == 2 ? plain : tint;
				GX2SetShaderMode(p.mode);
				GX2SetFetchShader(&p.group.fetchShader);
				GX2SetVertexShader(p.vs);
				GX2SetPixelShader(p.ps);
				if (stage == 3)
					GX2SetPixelUniformBlock(tintLocation, 16, tintLE);
				else if (stage == 4)
					GX2SetPixelUniformBlock(tintLocation, 16, tintNative);
				GX2SetAttribBuffer(0, 3 * stride, stride, verts);
				GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1);
			}
			if (screen == 0)
				WHBGfxFinishRenderTV();
			else
				WHBGfxFinishRenderDRC();
		}
		WHBGfxFinishRender();
		frames++;
	}
	Log("stage %d: %d frames", stage, frames);
	Log("exit requested");

	// No GX2 calls from here on: the GPU was drained in OnRelease.
	MEMFreeToDefaultHeap(verts);
	MEMFreeToDefaultHeap(tintLE);
	MEMFreeToDefaultHeap(tintNative);
	Log("exit: buffers freed");
	FreeShaders(plain.vs, plain.ps);
	FreeShaders(tint.vs, tint.ps);
	Log("exit: shaders freed");
	WHBGfxShutdown();
	Log("exit: WHBGfxShutdown done");
	DestroyGLSLCompiler();
	Log("exit: CafeGLSL destroyed");
	WHBProcShutdown();
	Log("exit: shutdown complete");
	return 0;
}
