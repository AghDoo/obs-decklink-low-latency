#include "decklink-device-instance.hpp"

#include <obs-module.h>
#include <util/util_uint64.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

#define LOG(level, message, ...) blog(level, "decklink-low-latency: " message, __VA_ARGS__)

DeckLinkDeviceInstance::DeckLinkDeviceInstance(DeckLinkDevice *device_, DeckLinkDeviceMode *mode_)
	: device(device_),
	  mode(mode_)
{
}

DeckLinkDeviceInstance::~DeckLinkDeviceInstance()
{
	StopOutput();
}

bool DeckLinkDeviceInstance::StartOutput()
{
	std::promise<bool> ready;
	auto started = ready.get_future();
	try {
		worker = std::thread([this, ready = std::move(ready)]() mutable {
			const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (FAILED(result)) {
				LOG(LOG_ERROR, "Could not initialize DeckLink output thread COM: 0x%08lX", result);
				ready.set_value(false);
				return;
			}
			RunOutput(ready);
			CoUninitialize();
		});
	} catch (const std::system_error &error) {
		LOG(LOG_ERROR, "Could not start the DeckLink output thread: %s", error.what());
		return false;
	}
	return started.get();
}

void DeckLinkDeviceInstance::StopOutput()
{
	if (!worker.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
	}
	workReady.notify_one();
	worker.join();
}

void DeckLinkDeviceInstance::RunOutput(std::promise<bool> &ready)
{
	ComPtr<IDeckLinkOutput> output;
	if (!device->GetOutput(&output)) {
		LOG(LOG_ERROR, "Could not acquire the DeckLink output interface for %s",
		    device->GetDisplayName().c_str());
		ready.set_value(false);
		return;
	}

	const int width = mode->GetWidth();
	const int height = mode->GetHeight();
	if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4) {
		LOG(LOG_ERROR, "Invalid DeckLink output dimensions: %dx%d", width, height);
		ready.set_value(false);
		return;
	}

	HRESULT result = output->EnableVideoOutput(mode->GetDisplayMode(), bmdVideoOutputFlagDefault);
	if (result != S_OK) {
		LOG(LOG_ERROR, "EnableVideoOutput failed: 0x%08lX", result);
		ready.set_value(false);
		return;
	}

	VideoFrames frames;
	rowBytes = static_cast<size_t>(width) * 4;
	for (size_t i = 0; i < frames.size(); ++i) {
		result = output->CreateVideoFrame(width, height, static_cast<int32_t>(rowBytes), bmdFormat8BitBGRA,
						  bmdFrameFlagDefault, &frames[i]);
		void *bytes = nullptr;
		if (result != S_OK || !frames[i] || frames[i]->GetBytes(&bytes) != S_OK || !bytes) {
			LOG(LOG_ERROR, "Could not allocate a writable DeckLink video frame (CreateVideoFrame: 0x%08lX)",
			    result);
			output->DisableVideoOutput();
			ready.set_value(false);
			return;
		}
		frameBytes[i] = static_cast<uint8_t *>(bytes);
	}

	result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, audioChannels,
					   bmdAudioOutputStreamContinuous);
	if (result != S_OK) {
		LOG(LOG_ERROR, "EnableAudioOutput failed: 0x%08lX", result);
		output->DisableVideoOutput();
		ready.set_value(false);
		return;
	}

	LOG(LOG_INFO, "Synchronous output on %s: %s (%dx%d), stereo PCM 48 kHz, one pending video frame",
	    device->GetDisplayName().c_str(), mode->GetName().c_str(), width, height);
	ready.set_value(true);
	SendFrames(output, frames);
	output->FlushBufferedAudioSamples();
	output->DisableAudioOutput();
	output->DisableVideoOutput();
}

void DeckLinkDeviceInstance::UpdateVideoFrame(const video_data *frame)
{
	if (!frame || !frame->data[0] || frame->linesize[0] < rowBytes) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping) {
			return;
		}
		// 待送緩衝只保留最新畫面，驅動呼叫中的緩衝由輸出執行緒獨佔。
		if (videoPending) {
			droppedVideoFrames.fetch_add(1, std::memory_order_relaxed);
		}
		uint8_t *destination = frameBytes[pendingSlot];
		for (int y = 0; y < mode->GetHeight(); ++y) {
			const auto row = static_cast<size_t>(y);
			std::memcpy(destination + row * rowBytes, frame->data[0] + row * frame->linesize[0], rowBytes);
		}
		pendingTimestamp = frame->timestamp;
		videoPending = true;
	}
	workReady.notify_one();
}

void DeckLinkDeviceInstance::WriteAudio(const audio_data *frames)
{
	if (!frames || !frames->data[0] || frames->frames == 0) {
		return;
	}

	const size_t count = std::min<size_t>(frames->frames, maxQueuedAudioFrames);
	const size_t skipped = frames->frames - count;
	AudioBlock block{std::vector<int16_t>(count * audioChannels),
			 frames->timestamp + util_mul_div64(skipped, 1000000000ULL, audioSampleRate), Clock::now()};
	constexpr size_t bytesPerFrame = audioChannels * sizeof(int16_t);
	std::memcpy(block.samples.data(), frames->data[0] + skipped * bytesPerFrame, count * bytesPerFrame);
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping) {
			return;
		}
		droppedAudioFrames.fetch_add(skipped, std::memory_order_relaxed);
		while (queuedAudioFrames + count > maxQueuedAudioFrames) {
			const size_t discarded = audioQueue.front().frameCount();
			droppedAudioFrames.fetch_add(discarded, std::memory_order_relaxed);
			queuedAudioFrames -= discarded;
			audioQueue.pop_front();
		}
		queuedAudioFrames += count;
		audioQueue.push_back(std::move(block));
	}
	workReady.notify_one();
}

void DeckLinkDeviceInstance::SendFrames(IDeckLinkOutput *output, const VideoFrames &frames)
{
	std::optional<uint64_t> firstVideoTimestamp;
	std::optional<AudioBlock> audio;
	size_t audioOffset = 0;
	auto audioRetryAt = Clock::time_point::min();
	uint64_t displayed = 0;
	uint64_t videoErrors = 0;
	uint64_t audioErrors = 0;
	uint64_t audioSubmitted = 0;
	uint64_t audioPartialWrites = 0;
	std::chrono::microseconds longestVideoCall{0};

	for (;;) {
		std::optional<size_t> videoSlot;
		uint64_t videoTimestamp = 0;
		{
			std::unique_lock<std::mutex> lock(mutex);
			const auto hasWork = [&] {
				return stopping || videoPending ||
				       (firstVideoTimestamp &&
					(audio ? Clock::now() >= audioRetryAt : !audioQueue.empty()));
			};
			if (audio) {
				workReady.wait_until(lock, audioRetryAt, hasWork);
			} else {
				workReady.wait(lock, hasWork);
			}
			if (stopping) {
				break;
			}
			if (videoPending) {
				videoSlot = pendingSlot;
				videoTimestamp = pendingTimestamp;
				pendingSlot = 1 - pendingSlot;
				videoPending = false;
			}
		}

		if (videoSlot) {
			const auto started = Clock::now();
			const HRESULT result = output->DisplayVideoFrameSync(frames[*videoSlot]);
			longestVideoCall =
				std::max(longestVideoCall,
					 std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started));
			if (result == S_OK) {
				if (!firstVideoTimestamp) {
					firstVideoTimestamp = videoTimestamp;
					LOG(LOG_INFO,
					    "First video frame accepted by DisplayVideoFrameSync (OBS timestamp %llu)",
					    static_cast<unsigned long long>(videoTimestamp));
				}
				++displayed;
			} else {
				if (++videoErrors == 1) {
					LOG(LOG_ERROR, "DisplayVideoFrameSync failed: 0x%08lX", result);
				}
				droppedVideoFrames.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// 一次影像呼叫期間可能收到多個 PCM 區塊，依序送出已可用的樣本。
		size_t audioBudget = maxQueuedAudioFrames;
		while (firstVideoTimestamp && audioBudget > 0 && Clock::now() >= audioRetryAt) {
			if (!audio) {
				std::lock_guard<std::mutex> lock(mutex);
				if (stopping || audioQueue.empty()) {
					break;
				}
				queuedAudioFrames -= audioQueue.front().frameCount();
				audio.emplace(std::move(audioQueue.front()));
				audioQueue.pop_front();
				const uint64_t trim = *firstVideoTimestamp > audio->timestamp
							      ? *firstVideoTimestamp - audio->timestamp
							      : 0;
				audioOffset = static_cast<size_t>(std::min<uint64_t>(
					audio->frameCount(), util_mul_div64(trim, audioSampleRate, 1000000000ULL)));
			}

			const size_t remaining = audio->frameCount() - audioOffset;
			audioBudget -= std::min(audioBudget, audio->frameCount());
			if (remaining == 0 || Clock::now() - audio->received >= maxAudioAge) {
				droppedAudioFrames.fetch_add(remaining, std::memory_order_relaxed);
				audio.reset();
				continue;
			}

			uint32_t written = 0;
			const HRESULT result =
				output->WriteAudioSamplesSync(audio->samples.data() + audioOffset * audioChannels,
							      static_cast<uint32_t>(remaining), &written);
			if (result != S_OK) {
				if (++audioErrors == 1) {
					LOG(LOG_ERROR, "WriteAudioSamplesSync failed: 0x%08lX", result);
				}
				droppedAudioFrames.fetch_add(remaining, std::memory_order_relaxed);
				audio.reset();
				continue;
			}

			const size_t accepted = std::min<size_t>(written, remaining);
			if (accepted > 0 && audioSubmitted == 0) {
				LOG(LOG_INFO,
				    "First stereo PCM block accepted by WriteAudioSamplesSync (%u sample frames)",
				    written);
			}
			audioSubmitted += accepted;
			if (accepted < remaining) {
				++audioPartialWrites;
				audioOffset += accepted;
				audioRetryAt = Clock::now() + std::chrono::milliseconds(2);
				break;
			}
			audio.reset();
		}
	}

	LOG(LOG_INFO,
	    "Synchronous output stopped: video sent=%llu, video dropped=%llu, video errors=%llu, max video call=%lld us, "
	    "audio sent sample frames=%llu, audio discarded=%llu, audio partial writes=%llu, audio errors=%llu",
	    static_cast<unsigned long long>(displayed), static_cast<unsigned long long>(GetDroppedFrames()),
	    static_cast<unsigned long long>(videoErrors), static_cast<long long>(longestVideoCall.count()),
	    static_cast<unsigned long long>(audioSubmitted),
	    static_cast<unsigned long long>(droppedAudioFrames.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(audioPartialWrites), static_cast<unsigned long long>(audioErrors));
}

uint64_t DeckLinkDeviceInstance::GetDroppedFrames() const
{
	return droppedVideoFrames.load(std::memory_order_relaxed);
}
