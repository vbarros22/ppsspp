// PPSSPP for the Wii U: frontend.
//
// Layout on the SD card:
//   sd:/wiiu/apps/ppsspp/          PPSSPP.wuhb (or .rpx), config, and assets/
//   sd:/wiiu/apps/ppsspp/assets/   PPSSPP's assets folder
//   sd:/ppsspp/                    the memory stick (PSP/ folder, saves, ...)
//   sd:/ppsspp/ppsspp_log.txt      log
//
// Video: WHBGfx owns the TV and GamePad buffers and handles the HOME menu (it frees and
// reallocates them on release/acquire). PPSSPP draws into the TV buffer, and the GX2
// backend copies it to both screens when presenting.
//
// Rules from testing on hardware (WiiU/test/gx2_test.cpp): the GPU must be drained in the
// ProcUI release callback, and nothing may touch GX2 after the main loop.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <coreinit/time.h>
#include <gx2/event.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>

#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
#include "Common/GPU/GraphicsContext.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/Log.h"
#include "Common/Log/LogManager.h"
#include "Common/System/Display.h"
#include "Common/System/NativeApp.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "GPU/GPUCommon.h"
#include "GPU/Debugger/Record.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/CmdLine.h"

static const char *kAppDir = "fs:/vol/external01/wiiu/apps/ppsspp";
static const char *kMemstickDir = "fs:/vol/external01/ppsspp";
static const char *kSDRoot = "fs:/vol/external01";

static int g_backbufferWidth = 1280;
static int g_backbufferHeight = 720;

// Early startup messages, before PPSSPP's logging is up (and if it never comes up).
static void BootLog(const char *msg) {
	std::string path = std::string(kMemstickDir) + "/ppsspp_boot.txt";
	FILE *f = fopen(path.c_str(), "a");
	if (f) {
		fprintf(f, "%s\n", msg);
		fclose(f);
	}
}

class WiiUGraphicsContext : public GraphicsContext {
public:
	bool Init() {
		draw_ = Draw::T3DCreateGX2Context(WHBGfxGetTVColourBuffer(), WHBGfxGetTVDepthBuffer());
		if (!draw_->CreatePresets()) {
			BootLog("CreatePresets failed (see ppsspp_log.txt)");
			return false;
		}
		return true;
	}
	void Shutdown() {
		delete draw_;
		draw_ = nullptr;
	}
	void Resize() override {}
	Draw::DrawContext *GetDrawContext() override { return draw_; }

private:
	Draw::DrawContext *draw_ = nullptr;
};

// Drain the GPU while we still own it (see the top of the file).
static uint32_t OnReleaseForeground(void *) {
	GX2DrawDone();
	return 0;
}

// --- Input ---

struct ButtonMap {
	uint32_t vpad;
	InputKeyCode key;
};

// PPSSPP's default mapping here is the generic gamepad one (Core/KeyMapDefaults.cpp,
// defaultPadMap): BUTTON_2 = Cross, 3 = Circle, 4 = Square, 1 = Triangle, 7/8 = L/R,
// 10 = Start, 9 = Select. The face buttons go by position, like on a PSP: B (bottom) is
// Cross, A (right) Circle, Y (left) Square, X (top) Triangle.
static const ButtonMap kButtons[] = {
	{ VPAD_BUTTON_B, NKCODE_BUTTON_2 },
	{ VPAD_BUTTON_A, NKCODE_BUTTON_3 },
	{ VPAD_BUTTON_Y, NKCODE_BUTTON_4 },
	{ VPAD_BUTTON_X, NKCODE_BUTTON_1 },
	{ VPAD_BUTTON_L, NKCODE_BUTTON_7 },
	{ VPAD_BUTTON_R, NKCODE_BUTTON_8 },
	{ VPAD_BUTTON_ZL, NKCODE_BUTTON_L2 },  // Pause menu (VIRTKEY_PAUSE).
	{ VPAD_BUTTON_ZR, NKCODE_BUTTON_R2 },
	{ VPAD_BUTTON_PLUS, NKCODE_BUTTON_10 },
	{ VPAD_BUTTON_MINUS, NKCODE_BUTTON_9 },
	{ VPAD_BUTTON_UP, NKCODE_DPAD_UP },
	{ VPAD_BUTTON_DOWN, NKCODE_DPAD_DOWN },
	{ VPAD_BUTTON_LEFT, NKCODE_DPAD_LEFT },
	{ VPAD_BUTTON_RIGHT, NKCODE_DPAD_RIGHT },
	{ VPAD_BUTTON_STICK_L, NKCODE_BUTTON_THUMBL },
	{ VPAD_BUTTON_STICK_R, NKCODE_BUTTON_THUMBR },
};

static bool g_touchDown = false;

static void PollGamePad() {
	VPADStatus status;
	VPADReadError err;
	if (VPADRead(VPAD_CHAN_0, &status, 1, &err) <= 0 || err != VPAD_READ_SUCCESS)
		return;

	// Both sticks pressed in: record a GE frame dump of the next frame (PSP/SYSTEM/DUMP), for
	// replaying on a PC.
	const uint32_t bothSticks = VPAD_BUTTON_STICK_L | VPAD_BUTTON_STICK_R;
	if ((status.hold & bothSticks) == bothSticks && (status.trigger & bothSticks) && gpu) {
		gpu->GetRecorder()->RecordNextFrame([](const Path &dumpPath) {
			NOTICE_LOG(Log::G3D, "WiiU: frame dump written to %s", dumpPath.c_str());
		});
		NOTICE_LOG(Log::G3D, "WiiU: recording a frame dump");
		extern int g_gx2FlushLog;
		g_gx2FlushLog = 600;
	}

	for (const ButtonMap &b : kButtons) {
		if (status.trigger & b.vpad)
			NativeKey(KeyInput(DEVICE_ID_PAD_0, b.key, KeyInputFlags::DOWN));
		if (status.release & b.vpad)
			NativeKey(KeyInput(DEVICE_ID_PAD_0, b.key, KeyInputFlags::UP));
	}

	AxisInput axes[4];
	axes[0] = AxisInput{ DEVICE_ID_PAD_0, JOYSTICK_AXIS_X, status.leftStick.x };
	axes[1] = AxisInput{ DEVICE_ID_PAD_0, JOYSTICK_AXIS_Y, -status.leftStick.y };
	axes[2] = AxisInput{ DEVICE_ID_PAD_0, JOYSTICK_AXIS_Z, status.rightStick.x };
	axes[3] = AxisInput{ DEVICE_ID_PAD_0, JOYSTICK_AXIS_RZ, -status.rightStick.y };
	NativeAxis(axes, 4);

	// The GamePad shows the whole backbuffer, scaled to its 854x480 screen.
	VPADTouchData tp;
	VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &tp, &status.tpFiltered1);
	bool down = tp.touched && tp.validity == VPAD_VALID;
	if (down || g_touchDown) {
		TouchInput touch{};
		touch.x = tp.x * (float)g_backbufferWidth / 854.0f * g_display.dpi_scale_x;
		touch.y = tp.y * (float)g_backbufferHeight / 480.0f * g_display.dpi_scale_y;
		touch.id = 0;
		touch.timestamp = time_now_d();
		if (down && !g_touchDown)
			touch.flags = TouchInputFlags::DOWN;
		else if (down)
			touch.flags = TouchInputFlags::MOVE;
		else
			touch.flags = TouchInputFlags::UP;
		NativeTouch(touch);
		g_touchDown = down;
	}
}

// --- System hooks ---

std::string System_GetProperty(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_NAME: return "Wii U";
	case SYSPROP_LANGREGION: return "en_US";
	case SYSPROP_BUILD_VERSION: return PPSSPP_GIT_VERSION;
	default: return "";
	}
}

std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_TEMP_DIRS:
		return { std::string(kMemstickDir) + "/temp" };
	default:
		return {};
	}
}

int64_t System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DEVICE_TYPE: return DEVICE_TYPE_TV;
	case SYSPROP_DISPLAY_XRES: return g_backbufferWidth;
	case SYSPROP_DISPLAY_YRES: return g_backbufferHeight;
	case SYSPROP_AUDIO_SAMPLE_RATE: return 48000;
	case SYSPROP_SYSTEMVERSION: return 0;
	default: return -1;
	}
}

float System_GetPropertyFloat(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DISPLAY_REFRESH_RATE: return 60.0f;
	case SYSPROP_DISPLAY_DPI: return 96.0f;
	case SYSPROP_DISPLAY_LOGICAL_DPI: return 96.0f;
	case SYSPROP_DISPLAY_SAFE_INSET_LEFT:
	case SYSPROP_DISPLAY_SAFE_INSET_RIGHT:
	case SYSPROP_DISPLAY_SAFE_INSET_TOP:
	case SYSPROP_DISPLAY_SAFE_INSET_BOTTOM:
		return 0.0f;
	default: return -1.0f;
	}
}

bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_HAS_BACK_BUTTON: return true;
	case SYSPROP_APP_GOLD: return false;
	case SYSPROP_CAN_JIT: return false;
	default: return false;
	}
}

void System_Notify(SystemNotification notification) {}
void System_Toast(std::string_view text) {}
void System_ShowKeyboard() {}
void System_Vibrate(int length_ms) {}
void System_LaunchUrl(LaunchUrlType urlType, std::string_view url) {}
std::vector<std::string> System_GetCameraDeviceList() { return {}; }
void System_AskForPermission(SystemPermission permission) {}
PermissionStatus System_GetPermissionStatus(SystemPermission permission) { return PERMISSION_STATUS_GRANTED; }
bool System_AudioRecordingIsAvailable() { return false; }
bool System_AudioRecordingState() { return false; }

// AX output, see WiiUAudio.cpp. (The System_Audio* helpers live in UI/.)
AudioBackend *WiiU_CreateAudioBackend();
AudioBackend *System_CreateAudioBackend() { return WiiU_CreateAudioBackend(); }

// Set once ProcUI has ended the main loop: from then on, no more system calls like
// SYSLaunchMenu (they never return). NativeShutdown itself asks to exit (System_ExitApp).
static bool g_leftForeground = false;
static bool g_exitRequested = false;

bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1, const std::string &param2, int64_t param3, int64_t param4) {
	switch (type) {
	case SystemRequestType::EXIT_APP:
		// Leave through the system menu, ProcUI wants to drive the exit. Only once, and only
		// while we're still running.
		if (!g_leftForeground && !g_exitRequested) {
			g_exitRequested = true;
			SYSLaunchMenu();
		}
		return true;
	default:
		return false;
	}
}

// --- main ---

int main(int argc, char **argv) {
	WHBProcInit();
	mkdir(kMemstickDir, 0777);
	mkdir(kAppDir, 0777);
	BootLog("--- PPSSPP starting ---");

	if (!WHBGfxInit()) {
		BootLog("WHBGfxInit failed");
		WHBProcShutdown();
		return 0;
	}
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, OnReleaseForeground, nullptr, 200);
	g_backbufferWidth = WHBGfxGetTVColourBuffer()->surface.width;
	g_backbufferHeight = WHBGfxGetTVColourBuffer()->surface.height;
	BootLog("WHBGfxInit ok");

	const char *args[] = { "PPSSPP" };
	CommandLineOptions opts{};
	NativeInit(1, args, opts, kAppDir, kMemstickDir, nullptr);
	g_logManager.SetFileLogPath(Path(std::string(kMemstickDir) + "/ppsspp_log.txt"));
	g_logManager.SetOutputsEnabled(LogOutput::File);
	g_logManager.SetAllLogLevels(LogLevel::LINFO);
	g_logManager.SetAllLogEnable(true);
	INFO_LOG(Log::System, "Wii U frontend: NativeInit done, backbuffer %dx%d", g_backbufferWidth, g_backbufferHeight);
	BootLog("NativeInit done");

	Native_UpdateScreenScale(g_backbufferWidth, g_backbufferHeight, 1.0f);

	// Whether ProcUI still has us running. Once WHBProcIsRunning has returned false (exit),
	// calling it again blocks forever, so remember the answer.
	bool running = true;
	WiiUGraphicsContext graphics;
	if (!graphics.Init() || !NativeInitGraphics(&graphics)) {
		BootLog("graphics init failed (see ppsspp_log.txt)");
		ERROR_LOG(Log::System, "Wii U frontend: graphics init failed");
	} else {
		BootLog("graphics init done, running");
		while ((running = WHBProcIsRunning())) {
			PollGamePad();
			NativeFrame(&graphics);
		}
		g_leftForeground = true;
		BootLog("main loop done");
		// The GPU was drained in OnReleaseForeground, and no GX2 calls are allowed from here.
		NativeShutdownGraphics(&graphics);
		BootLog("NativeShutdownGraphics done");
		graphics.Shutdown();
		BootLog("draw context destroyed");
	}

	// A Wii U app can't just return from main: ask for the HOME menu and let ProcUI drive
	// the exit. (After an error, or EXIT_APP, the loop above ends without that.)
	if (running) {
		BootLog("leaving through the system menu");
		if (!g_exitRequested)
			SYSLaunchMenu();
		g_exitRequested = true;
		while (WHBProcIsRunning()) {
		}
	}
	g_leftForeground = true;

	BootLog("NativeShutdown...");
	NativeShutdown();
	BootLog("shutdown done");
	WHBGfxShutdown();
	WHBProcShutdown();
	return 0;
}
