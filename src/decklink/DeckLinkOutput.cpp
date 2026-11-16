#include "DeckLinkOutput.hpp"

#include "decklink-device.hpp"

#include <limits>

DeckLinkOutput::DeckLinkOutput(obs_output_t *output_, DeckLinkDeviceDiscovery *discovery_)
	: DecklinkBase(discovery_), output(output_)
{
	discovery->AddCallback(DeckLinkOutput::DevicesChanged, this);
}

DeckLinkOutput::~DeckLinkOutput()
{
	discovery->RemoveCallback(DeckLinkOutput::DevicesChanged, this);
	Deactivate();
}

void DeckLinkOutput::DevicesChanged(void *param, DeckLinkDevice *device, bool added)
{
	UNUSED_PARAMETER(param);
	blog(LOG_DEBUG, "DeckLink device %s: %s", added ? "added" : "removed", device->GetDisplayName().c_str());
}

bool DeckLinkOutput::Activate(DeckLinkDevice *device, long long modeId)
{
	std::lock_guard<std::recursive_mutex> lock(deviceMutex);

	DeckLinkDevice *currentDevice = GetDevice();
	const bool sameDevice = device == currentDevice;
	const bool isActive = instance != nullptr;

	if (sameDevice) {
		if (!isActive) {
			return false;
		}

		if (instance->GetActiveModeId() == modeId && instance->GetBufferCount() == bufferCount) {
			return false;
		}
	}

	if (isActive) {
		instance->StopOutput();
	}

	if (!sameDevice) {
		instance.Set(new DeckLinkDeviceInstance(this, device));
	}

	if (!instance) {
		return false;
	}

	DeckLinkDeviceMode *mode = GetDevice()->FindOutputMode(modeId);
	if (!mode || !instance->StartOutput(mode, bufferCount)) {
		instance = nullptr;
		return false;
	}

	++activateRefs;
	return true;
}

void DeckLinkOutput::Deactivate()
{
	std::lock_guard<std::recursive_mutex> lock(deviceMutex);

	if (instance) {
		instance->StopOutput();
		instance = nullptr;
	}

	if (activateRefs > 0) {
		--activateRefs;
	}
}

obs_output_t *DeckLinkOutput::GetOutput() const
{
	return output;
}

void DeckLinkOutput::UpdateVideoFrame(video_data *frame)
{
	if (instance) {
		instance->UpdateVideoFrame(frame);
	}
}

void DeckLinkOutput::WriteAudio(audio_data *frames)
{
	if (instance) {
		instance->WriteAudio(frames);
	}
}

void DeckLinkOutput::SetSize(int width_, int height_)
{
	width = width_;
	height = height_;
}

int DeckLinkOutput::GetWidth() const
{
	return width;
}

int DeckLinkOutput::GetHeight() const
{
	return height;
}

void DeckLinkOutput::SetBufferCount(int count)
{
	bufferCount = (count <= 2) ? 2 : 3;
}

int DeckLinkOutput::GetBufferCount() const
{
	return bufferCount;
}

int DeckLinkOutput::GetDroppedFrames() const
{
	std::lock_guard<std::recursive_mutex> lock(deviceMutex);
	if (!instance) {
		return 0;
	}

	const uint64_t dropped = instance->GetDroppedFrames();
	return dropped > static_cast<uint64_t>(std::numeric_limits<int>::max())
		       ? std::numeric_limits<int>::max()
		       : static_cast<int>(dropped);
}
