#pragma once

#include "platform.hpp"
#include "decklink-device.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#define LOG(level, message, ...) blog(level, "%s: " message, "decklink-low-latency", ##__VA_ARGS__)

class DecklinkBase;

class FrameQueue {
private:
	std::mutex mutex;
	std::vector<uint8_t *> slots;
	size_t readIndex = 0;
	size_t writeIndex = 0;
	size_t count = 0;

public:
	void reset(size_t capacity);
	bool push(uint8_t *frame);
	uint8_t *pop();
};

class DeckLinkDeviceInstance;

template<typename T> class RenderDelegate final : public IDeckLinkVideoOutputCallback {
private:
	std::atomic<ULONG> refCount{1};
	T *owner;

public:
	explicit RenderDelegate(T *owner_) : owner(owner_) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override
	{
		if (!ppv) {
			return E_POINTER;
		}

		*ppv = nullptr;
		if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDeckLinkVideoOutputCallback)) {
			*ppv = static_cast<IDeckLinkVideoOutputCallback *>(this);
			AddRef();
			return S_OK;
		}

		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++refCount;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining = --refCount;
		if (remaining == 0) {
			delete this;
		}
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame *completedFrame,
								  BMDOutputFrameCompletionResult) override
	{
		if (owner) {
			owner->ScheduleVideoFrame(completedFrame);
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
	{
		return S_OK;
	}
};

class DeckLinkDeviceInstance {
private:
	std::atomic<ULONG> refCount{1};
	DecklinkBase *decklink = nullptr;
	DeckLinkDevice *device = nullptr;
	DeckLinkDeviceMode *mode = nullptr;

	ComPtr<IDeckLinkOutput> output;
	ComPtr<RenderDelegate<DeckLinkDeviceInstance>> renderDelegate;

	std::vector<std::vector<uint8_t>> frameBlobs;
	FrameQueue freeFrames;
	FrameQueue readyFrames;
	uint8_t *activeBlob = nullptr;

	BMDTimeValue frameDuration = 0;
	BMDTimeScale frameTimescale = 0;
	int64_t totalFramesScheduled = 0;
	int bufferCount = 2;
	std::atomic<uint64_t> droppedFrames{0};

public:
	DeckLinkDeviceInstance(DecklinkBase *decklink_, DeckLinkDevice *device_);
	~DeckLinkDeviceInstance();

	ULONG AddRef();
	ULONG Release();

	DeckLinkDevice *GetDevice() const;
	long long GetActiveModeId() const;
	int GetBufferCount() const;
	uint64_t GetDroppedFrames() const;

	bool StartOutput(DeckLinkDeviceMode *mode_, int bufferCount_);
	bool StopOutput();

	void UpdateVideoFrame(video_data *frame);
	void ScheduleVideoFrame(IDeckLinkVideoFrame *frame);
	void WriteAudio(audio_data *frames);
};
