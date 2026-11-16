#include "DeckLinkOutput.hpp"

#include <algorithm>
#include <limits>

DeckLinkOutput::DeckLinkOutput(obs_output_t *output_) : output(output_) {}

obs_output_t *DeckLinkOutput::GetOutput() const
{
	return output;
}

bool DeckLinkOutput::Activate(DeckLinkDevice *device, DeckLinkDeviceMode *mode)
{
	std::lock_guard<std::mutex> lock(instanceMutex);
	if (instance) {
		return false;
	}
	finalDroppedFrames = 0;
	auto candidate = std::make_unique<DeckLinkDeviceInstance>(device, mode);
	if (!candidate->StartOutput()) {
		return false;
	}
	instance = std::move(candidate);
	return true;
}

void DeckLinkOutput::Deactivate()
{
	std::lock_guard<std::mutex> lock(instanceMutex);
	if (instance) {
		instance->StopOutput();
		finalDroppedFrames = instance->GetDroppedFrames();
		instance.reset();
	}
}

void DeckLinkOutput::UpdateVideoFrame(const video_data *frame)
{
	std::lock_guard<std::mutex> lock(instanceMutex);
	if (instance) {
		instance->UpdateVideoFrame(frame);
	}
}

void DeckLinkOutput::WriteAudio(const audio_data *frames)
{
	std::lock_guard<std::mutex> lock(instanceMutex);
	if (instance) {
		instance->WriteAudio(frames);
	}
}

int DeckLinkOutput::GetDroppedFrames() const
{
	std::lock_guard<std::mutex> lock(instanceMutex);
	const uint64_t dropped = instance ? instance->GetDroppedFrames() : finalDroppedFrames;
	return static_cast<int>(std::min<uint64_t>(dropped, std::numeric_limits<int>::max()));
}
