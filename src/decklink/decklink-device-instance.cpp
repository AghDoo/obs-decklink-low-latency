#include "decklink-device-instance.hpp"

#include "DeckLinkOutput.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <util/platform.h>

void FrameQueue::reset(size_t capacity)
{
	std::lock_guard<std::mutex> lock(mutex);
	slots.assign(capacity, nullptr);
	readIndex = 0;
	writeIndex = 0;
	count = 0;
}

bool FrameQueue::push(uint8_t *frame)
{
	if (!frame) {
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (slots.empty() || count == slots.size()) {
		return false;
	}

	slots[writeIndex] = frame;
	writeIndex = (writeIndex + 1) % slots.size();
	++count;
	return true;
}

uint8_t *FrameQueue::pop()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (count == 0) {
		return nullptr;
	}

	uint8_t *frame = slots[readIndex];
	slots[readIndex] = nullptr;
	readIndex = (readIndex + 1) % slots.size();
	--count;
	return frame;
}

DeckLinkDeviceInstance::DeckLinkDeviceInstance(DecklinkBase *decklink_, DeckLinkDevice *device_)
	: decklink(decklink_), device(device_)
{
	if (device) {
		device->AddRef();
	}
}

DeckLinkDeviceInstance::~DeckLinkDeviceInstance()
{
	StopOutput();
	if (device) {
		device->Release();
		device = nullptr;
	}
}

ULONG DeckLinkDeviceInstance::AddRef()
{
	return ++refCount;
}

ULONG DeckLinkDeviceInstance::Release()
{
	const ULONG remaining = --refCount;
	if (remaining == 0) {
		delete this;
	}
	return remaining;
}

DeckLinkDevice *DeckLinkDeviceInstance::GetDevice() const
{
	return device;
}

long long DeckLinkDeviceInstance::GetActiveModeId() const
{
	return mode ? mode->GetId() : 0;
}

int DeckLinkDeviceInstance::GetBufferCount() const
{
	return bufferCount;
}

uint64_t DeckLinkDeviceInstance::GetDroppedFrames() const
{
	return droppedFrames.load(std::memory_order_relaxed);
}

bool DeckLinkDeviceInstance::StartOutput(DeckLinkDeviceMode *mode_, int bufferCount_)
{
	if (mode || !mode_ || !device || !decklink) {
		return false;
	}

	auto *decklinkOutput = dynamic_cast<DeckLinkOutput *>(decklink);
	if (!decklinkOutput) {
		return false;
	}

	ComPtr<IDeckLinkOutput> output_;
	if (!device->GetOutput(&output_)) {
		LOG(LOG_ERROR, "Could not acquire the DeckLink output interface");
		return false;
	}

	HRESULT result = output_->EnableVideoOutput(mode_->GetDisplayMode(), bmdVideoOutputFlagDefault);
	if (result != S_OK) {
		LOG(LOG_ERROR, "EnableVideoOutput failed: 0x%08X", result);
		return false;
	}

	result = output_->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2,
						    bmdAudioOutputStreamTimestamped);
	if (result != S_OK) {
		LOG(LOG_ERROR, "EnableAudioOutput failed: 0x%08X", result);
		output_->DisableVideoOutput();
		return false;
	}

	if (!mode_->GetFrameRate(&frameDuration, &frameTimescale) || frameDuration <= 0 || frameTimescale <= 0) {
		LOG(LOG_ERROR, "Could not read the selected DeckLink mode frame rate");
		output_->DisableAudioOutput();
		output_->DisableVideoOutput();
		return false;
	}

	const int width = decklinkOutput->GetWidth();
	const int height = decklinkOutput->GetHeight();
	if (width <= 0 || height <= 0 || width > (std::numeric_limits<int>::max() / 4)) {
		LOG(LOG_ERROR, "Invalid DeckLink output dimensions: %dx%d", width, height);
		output_->DisableAudioOutput();
		output_->DisableVideoOutput();
		return false;
	}

	const int rowSize = width * 4;
	const size_t frameSize = static_cast<size_t>(rowSize) * static_cast<size_t>(height);
	if (frameSize / static_cast<size_t>(height) != static_cast<size_t>(rowSize)) {
		LOG(LOG_ERROR, "DeckLink frame buffer size overflow");
		output_->DisableAudioOutput();
		output_->DisableVideoOutput();
		return false;
	}

	bufferCount = (bufferCount_ <= 2) ? 2 : 3;
	frameBlobs.clear();
	frameBlobs.resize(static_cast<size_t>(bufferCount));
	freeFrames.reset(static_cast<size_t>(bufferCount));
	readyFrames.reset(static_cast<size_t>(bufferCount));
	activeBlob = nullptr;
	droppedFrames.store(0, std::memory_order_relaxed);

	for (auto &blob : frameBlobs) {
		blob.assign(frameSize, 0);
		if (!freeFrames.push(blob.data())) {
			LOG(LOG_ERROR, "Could not initialize the frame buffer pool");
			frameBlobs.clear();
			output_->DisableAudioOutput();
			output_->DisableVideoOutput();
			return false;
		}
	}

	const int64_t reportedMinimum = device->GetMinimumPrerollFrames();
	const int64_t minimumPrerollFrames = std::max<int64_t>(reportedMinimum, bufferCount);
	LOG(LOG_INFO, "Starting %s at %dx%d, %lld/%lld fps; software buffers=%d, device minimum preroll=%lld, scheduled preroll=%lld",
	    device->GetDisplayName().c_str(), width, height, static_cast<long long>(frameTimescale),
	    static_cast<long long>(frameDuration), bufferCount, static_cast<long long>(reportedMinimum),
	    static_cast<long long>(minimumPrerollFrames));

	for (int64_t i = 0; i < minimumPrerollFrames; ++i) {
		ComPtr<IDeckLinkMutableVideoFrame> prerollFrame;
		result = output_->CreateVideoFrame(width, height, rowSize, bmdFormat8BitBGRA, bmdFrameFlagDefault,
						   &prerollFrame);
		if (result != S_OK || !prerollFrame) {
			LOG(LOG_ERROR, "CreateVideoFrame failed during preroll: 0x%08X", result);
			output_->DisableAudioOutput();
			output_->DisableVideoOutput();
			frameBlobs.clear();
			return false;
		}

		void *bytes = nullptr;
		if (SUCCEEDED(prerollFrame->GetBytes(&bytes)) && bytes) {
			std::memset(bytes, 0, frameSize);
		}

		result = output_->ScheduleVideoFrame(prerollFrame, i * frameDuration, frameDuration, frameTimescale);
		if (result != S_OK) {
			LOG(LOG_ERROR, "ScheduleVideoFrame failed during preroll: 0x%08X", result);
			output_->DisableAudioOutput();
			output_->DisableVideoOutput();
			frameBlobs.clear();
			return false;
		}
	}

	totalFramesScheduled = minimumPrerollFrames;
	*renderDelegate.Assign() = new RenderDelegate<DeckLinkDeviceInstance>(this);

	output = std::move(output_);
	mode = mode_;

	result = output->SetScheduledFrameCompletionCallback(renderDelegate);
	if (result != S_OK) {
		LOG(LOG_ERROR, "SetScheduledFrameCompletionCallback failed: 0x%08X", result);
		StopOutput();
		return false;
	}

	result = output->StartScheduledPlayback(0, 100, 1.0);
	if (result != S_OK) {
		LOG(LOG_ERROR, "StartScheduledPlayback failed: 0x%08X", result);
		StopOutput();
		return false;
	}

	return true;
}

bool DeckLinkDeviceInstance::StopOutput()
{
	if (!output) {
		mode = nullptr;
		return false;
	}

	output->SetScheduledFrameCompletionCallback(nullptr);
	output->StopScheduledPlayback(0, nullptr, frameTimescale);
	output->DisableVideoOutput();
	output->DisableAudioOutput();
	output.Clear();
	renderDelegate.Clear();

	freeFrames.reset(0);
	readyFrames.reset(0);
	frameBlobs.clear();
	activeBlob = nullptr;
	mode = nullptr;
	totalFramesScheduled = 0;

	return true;
}

void DeckLinkDeviceInstance::UpdateVideoFrame(video_data *frame)
{
	if (!frame || !frame->data[0] || !mode) {
		return;
	}

	auto *decklinkOutput = dynamic_cast<DeckLinkOutput *>(decklink);
	if (!decklinkOutput) {
		return;
	}

	const int width = decklinkOutput->GetWidth();
	const int height = decklinkOutput->GetHeight();
	const size_t rowBytes = static_cast<size_t>(width) * 4;
	if (width <= 0 || height <= 0 || frame->linesize[0] < rowBytes) {
		return;
	}

	uint8_t *blob = freeFrames.pop();

	// If every buffer is in use, replace the oldest waiting frame instead of
	// allowing stale frames to build up behind the DeckLink playback clock.
	if (!blob) {
		blob = readyFrames.pop();
		if (blob) {
			droppedFrames.fetch_add(1, std::memory_order_relaxed);
		}
	}

	if (!blob) {
		droppedFrames.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	for (int y = 0; y < height; ++y) {
		std::memcpy(blob + static_cast<size_t>(y) * rowBytes,
			    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0], rowBytes);
	}

	if (!readyFrames.push(blob)) {
		freeFrames.push(blob);
		droppedFrames.fetch_add(1, std::memory_order_relaxed);
	}
}

void DeckLinkDeviceInstance::ScheduleVideoFrame(IDeckLinkVideoFrame *frame)
{
	if (!output || !frame) {
		return;
	}

	void *bytes = nullptr;
	if (FAILED(frame->GetBytes(&bytes)) || !bytes) {
		LOG(LOG_WARNING, "Could not access a completed DeckLink video frame");
		return;
	}

	uint8_t *nextBlob = readyFrames.pop();
	if (nextBlob) {
		if (activeBlob && !freeFrames.push(activeBlob)) {
			LOG(LOG_WARNING, "Frame buffer pool recycle failed");
		}
		activeBlob = nextBlob;
	}

	const size_t frameSize = static_cast<size_t>(frame->GetRowBytes()) *
				 static_cast<size_t>(frame->GetHeight());
	if (activeBlob && !frameBlobs.empty() && frameSize <= frameBlobs.front().size()) {
		std::memcpy(bytes, activeBlob, frameSize);
	} else {
		std::memset(bytes, 0, frameSize);
	}

	const HRESULT result = output->ScheduleVideoFrame(frame, totalFramesScheduled * frameDuration,
							  frameDuration, frameTimescale);
	if (result == S_OK) {
		++totalFramesScheduled;
	} else {
		LOG(LOG_ERROR, "Could not schedule the next DeckLink frame: 0x%08X", result);
	}
}

void DeckLinkDeviceInstance::WriteAudio(audio_data *frames)
{
	if (!output || !frames || !frames->data[0] || frames->frames == 0) {
		return;
	}

	uint32_t sampleFramesWritten = 0;
	const HRESULT result = output->WriteAudioSamplesSync(frames->data[0], frames->frames, &sampleFramesWritten);
	if (result != S_OK) {
		LOG(LOG_WARNING, "WriteAudioSamplesSync failed: 0x%08X", result);
	}
}
