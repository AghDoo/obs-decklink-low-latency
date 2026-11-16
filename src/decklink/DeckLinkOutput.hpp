#pragma once

#include "decklink-device-instance.hpp"

#include <obs.h>

#include <memory>
#include <mutex>
#include <string>

class DeckLinkOutput final {
public:
	explicit DeckLinkOutput(obs_output_t *output);

	std::string deviceHash;
	long long modeID = 0;

	obs_output_t *GetOutput() const;
	bool Activate(DeckLinkDevice *device, DeckLinkDeviceMode *mode);
	void Deactivate();
	void UpdateVideoFrame(const video_data *frame);
	void WriteAudio(const audio_data *frames);
	int GetDroppedFrames() const;

private:
	obs_output_t *output;
	mutable std::mutex instanceMutex;
	std::unique_ptr<DeckLinkDeviceInstance> instance;
	uint64_t finalDroppedFrames = 0;
};
