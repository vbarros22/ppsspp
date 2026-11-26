// Copyright (c) 2026- PPSSPP Project.
// Licensed under GPL 2.0 or later.

// Audio output through AX (sndcore2): two looping 16-bit mono voices, left and right, playing
// from a ring buffer that the AX frame callback (every 3 ms) keeps filled a few frames ahead
// of the playback position, with PPSSPP's mixer at 48 kHz.

#include <algorithm>
#include <cstring>
#include <vector>

#include <coreinit/cache.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>

#include "Common/Audio/AudioBackend.h"
#include "Common/Log.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr uint32_t kRingSamples = 4096;   // Per channel, a power of two.
constexpr uint32_t kLeadSamples = 144 * 4;  // How far ahead of playback to keep (about 12 ms).

class WiiUAudioBackend;
WiiUAudioBackend *g_backend = nullptr;

class WiiUAudioBackend : public AudioBackend {
public:
	~WiiUAudioBackend() override { Shutdown(); }

	void EnumerateDevices(std::vector<AudioDeviceDesc> *outputDevices, bool captureDevices) override {
		if (!captureDevices)
			outputDevices->push_back(AudioDeviceDesc{ "Wii U", "wiiu" });
	}
	void SetRenderCallback(RenderCallback callback, void *userdata) override {
		callback_ = callback;
		userdata_ = userdata;
	}
	bool InitOutputDevice(std::string_view uniqueId, LatencyMode latencyMode, bool *revertedToDefault) override {
		*revertedToDefault = false;
		return Init();
	}
	int SampleRate() const override { return kSampleRate; }
	int BufferSize() const override { return kRingSamples; }
	int PeriodFrames() const override { return frameSamples_; }
	std::string GetCurrentDeviceName() const override { return "Wii U (AX)"; }

	void FrameCallback();

private:
	bool Init();
	void Shutdown();
	void MixInto(uint32_t pos, uint32_t count);

	RenderCallback callback_ = nullptr;
	void *userdata_ = nullptr;
	bool initialized_ = false;
	AXVoice *voices_[2]{};
	int16_t *ring_[2]{};
	uint32_t writePos_ = 0;
	int frameSamples_ = 144;
	std::vector<float> mix_;
};

void AxFrameCallback() {
	if (g_backend)
		g_backend->FrameCallback();
}

bool WiiUAudioBackend::Init() {
	if (initialized_)
		return true;

	AXInitParams params{};
	params.renderer = AX_INIT_RENDERER_48KHZ;
	params.pipeline = AX_INIT_PIPELINE_SINGLE;
	AXInitWithParams(&params);
	if (!AXIsInit()) {
		ERROR_LOG(Log::Audio, "AX: init failed");
		return false;
	}
	frameSamples_ = (int)AXGetInputSamplesPerFrame();
	if (frameSamples_ <= 0 || frameSamples_ > 1024)
		frameSamples_ = 144;
	mix_.resize(frameSamples_ * 2);

	for (int ch = 0; ch < 2; ch++) {
		ring_[ch] = (int16_t *)aligned_alloc(64, kRingSamples * sizeof(int16_t));
		if (!ring_[ch]) {
			ERROR_LOG(Log::Audio, "AX: out of memory");
			Shutdown();
			return false;
		}
		memset(ring_[ch], 0, kRingSamples * sizeof(int16_t));
		DCFlushRange(ring_[ch], kRingSamples * sizeof(int16_t));
		voices_[ch] = AXAcquireVoice(31, nullptr, nullptr);
		if (!voices_[ch]) {
			ERROR_LOG(Log::Audio, "AX: no voice");
			Shutdown();
			return false;
		}
	}

	for (int ch = 0; ch < 2; ch++) {
		AXVoice *v = voices_[ch];
		AXVoiceBegin(v);
		AXSetVoiceType(v, 0);

		AXVoiceOffsets offsets{};
		offsets.dataType = AX_VOICE_FORMAT_LPCM16;
		offsets.loopingEnabled = AX_VOICE_LOOP_ENABLED;
		offsets.loopOffset = 0;
		offsets.endOffset = kRingSamples - 1;
		offsets.currentOffset = 0;
		offsets.data = ring_[ch];
		AXSetVoiceOffsets(v, &offsets);

		AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_NONE);
		AXSetVoiceSrcRatio(v, 1.0f);
		AXVoiceVeData ve{ 0x8000, 0 };
		AXSetVoiceVe(v, &ve);

		// Left to the left speaker(s), right to the right, on the TV and the GamePad.
		AXVoiceDeviceMixData tv[6]{};
		tv[ch].bus[0].volume = 0x8000;
		AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, tv);
		AXVoiceDeviceMixData drc[4]{};
		drc[ch].bus[0].volume = 0x8000;
		AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, drc);
		AXVoiceEnd(v);
	}

	writePos_ = kLeadSamples;
	g_backend = this;
	AXRegisterAppFrameCallback(&AxFrameCallback);

	// Start both together.
	AXVoiceBegin(voices_[0]);
	AXVoiceBegin(voices_[1]);
	AXSetVoiceState(voices_[0], AX_VOICE_STATE_PLAYING);
	AXSetVoiceState(voices_[1], AX_VOICE_STATE_PLAYING);
	AXVoiceEnd(voices_[1]);
	AXVoiceEnd(voices_[0]);

	initialized_ = true;
	INFO_LOG(Log::Audio, "AX: audio started, %d samples per frame", frameSamples_);
	return true;
}

void WiiUAudioBackend::Shutdown() {
	if (g_backend == this) {
		AXDeregisterAppFrameCallback(&AxFrameCallback);
		g_backend = nullptr;
	}
	for (int ch = 0; ch < 2; ch++) {
		if (voices_[ch]) {
			AXSetVoiceState(voices_[ch], AX_VOICE_STATE_STOPPED);
			AXFreeVoice(voices_[ch]);
			voices_[ch] = nullptr;
		}
	}
	if (AXIsInit())
		AXQuit();
	for (int ch = 0; ch < 2; ch++) {
		free(ring_[ch]);
		ring_[ch] = nullptr;
	}
	initialized_ = false;
}

void WiiUAudioBackend::MixInto(uint32_t pos, uint32_t count) {
	if ((uint32_t)mix_.size() < count * 2)
		mix_.resize(count * 2);
	if (callback_) {
		callback_(mix_.data(), (int)count, kSampleRate, userdata_);
	} else {
		std::fill(mix_.begin(), mix_.begin() + count * 2, 0.0f);
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t p = (pos + i) & (kRingSamples - 1);
		const float l = std::clamp(mix_[i * 2], -1.0f, 1.0f);
		const float r = std::clamp(mix_[i * 2 + 1], -1.0f, 1.0f);
		ring_[0][p] = (int16_t)(l * 32767.0f);
		ring_[1][p] = (int16_t)(r * 32767.0f);
	}
	// Flush what was written (the range may wrap).
	const uint32_t first = std::min(count, kRingSamples - pos);
	for (int ch = 0; ch < 2; ch++) {
		DCFlushRange(ring_[ch] + pos, first * sizeof(int16_t));
		if (count > first)
			DCFlushRange(ring_[ch], (count - first) * sizeof(int16_t));
	}
}

void WiiUAudioBackend::FrameCallback() {
	if (!initialized_)
		return;
	AXVoiceOffsets offsets;
	AXGetVoiceOffsets(voices_[0], &offsets);
	const uint32_t play = offsets.currentOffset & (kRingSamples - 1);
	uint32_t ahead = (writePos_ - play) & (kRingSamples - 1);
	if (ahead > kRingSamples / 2) {
		// Playback overtook us: start again just ahead of it.
		writePos_ = (play + (uint32_t)frameSamples_) & (kRingSamples - 1);
		ahead = (uint32_t)frameSamples_;
	}
	while (ahead < kLeadSamples) {
		const uint32_t count = (uint32_t)frameSamples_;
		MixInto(writePos_, count);
		writePos_ = (writePos_ + count) & (kRingSamples - 1);
		ahead += count;
	}
}

}  // namespace

AudioBackend *WiiU_CreateAudioBackend() {
	return new WiiUAudioBackend();
}
