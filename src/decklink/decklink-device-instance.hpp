#pragma once

#include "decklink-device.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

struct audio_data;
struct video_data;

class DeckLinkDeviceInstance {
public:
	static constexpr uint32_t audioSampleRate = 48000;
	static constexpr uint32_t audioChannels = 2;

	DeckLinkDeviceInstance(DeckLinkDevice *device, DeckLinkDeviceMode *mode);
	~DeckLinkDeviceInstance();

	bool StartOutput();
	void StopOutput();
	void UpdateVideoFrame(const video_data *frame);
	void WriteAudio(const audio_data *frames);
	uint64_t GetDroppedFrames() const;

private:
	using Clock = std::chrono::steady_clock;
	using VideoFrames = std::array<ComPtr<IDeckLinkMutableVideoFrame>, 2>;
	static constexpr auto maxAudioAge = std::chrono::milliseconds(100);
	static constexpr size_t maxQueuedAudioFrames = audioSampleRate / 10;

	struct AudioBlock {
		std::vector<int16_t> samples;
		uint64_t timestamp;
		Clock::time_point received;

		size_t frameCount() const { return samples.size() / audioChannels; }
	};

	void RunOutput(std::promise<bool> &ready);
	void SendFrames(IDeckLinkOutput *output, const VideoFrames &frames);

	ComPtr<DeckLinkDevice> device;
	DeckLinkDeviceMode *mode;
	std::thread worker;
	std::mutex mutex;
	std::condition_variable workReady;
	bool stopping = false;
	std::array<uint8_t *, 2> frameBytes{};
	size_t rowBytes = 0;
	int pendingSlot = 0;
	bool videoPending = false;
	uint64_t pendingTimestamp = 0;
	std::deque<AudioBlock> audioQueue;
	size_t queuedAudioFrames = 0;
	std::atomic<uint64_t> droppedVideoFrames{0};
	std::atomic<uint64_t> droppedAudioFrames{0};
};
