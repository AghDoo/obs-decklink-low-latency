#pragma once

#include "DecklinkBase.h"

#include <media-io/video-scaler.h>

class DeckLinkOutput final : public DecklinkBase {
protected:
	obs_output_t *output = nullptr;
	int width = 0;
	int height = 0;
	int bufferCount = 2;

	static void DevicesChanged(void *param, DeckLinkDevice *device, bool added);

public:
	const char *deviceHash = nullptr;
	long long modeID = 0;
	uint64_t start_timestamp = 0;
	uint32_t audio_samplerate = 48000;
	size_t audio_planes = 2;
	size_t audio_size = 0;

	DeckLinkOutput(obs_output_t *output, DeckLinkDeviceDiscovery *discovery);
	~DeckLinkOutput() override;

	obs_output_t *GetOutput() const;
	bool Activate(DeckLinkDevice *device, long long modeId) override;
	void Deactivate() override;

	void UpdateVideoFrame(video_data *frame);
	void WriteAudio(audio_data *frames);

	void SetSize(int width, int height);
	int GetWidth() const;
	int GetHeight() const;

	void SetBufferCount(int count);
	int GetBufferCount() const;
};
