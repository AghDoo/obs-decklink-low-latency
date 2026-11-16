#include <obs-module.h>

#include "const.h"
#include "DeckLinkOutput.hpp"
#include "decklink-device.hpp"
#include "decklink-devices.hpp"

#include <media-io/video-scaler.h>
#include <util/util_uint64.h>

#include <algorithm>
#include <cstring>
#include <limits>

static void decklink_output_destroy(void *data)
{
	delete static_cast<DeckLinkOutput *>(data);
}

static void decklink_output_set_video_conversion(DeckLinkOutput *decklink)
{
	if (!decklink->deviceHash || !*decklink->deviceHash) {
		return;
	}

	ComPtr<DeckLinkDevice> device;
	device.Set(deviceEnum->FindByHash(decklink->deviceHash));
	if (!device) {
		return;
	}

	DeckLinkDeviceMode *mode = device->FindOutputMode(decklink->modeID);
	if (!mode) {
		return;
	}

	struct video_scale_info conversion = {};
	conversion.format = VIDEO_FORMAT_BGRA;
	conversion.width = mode->GetWidth();
	conversion.height = mode->GetHeight();
	conversion.range = VIDEO_RANGE_FULL;
	conversion.colorspace = VIDEO_CS_709;
	obs_output_set_video_conversion(decklink->GetOutput(), &conversion);
}

static void *decklink_output_create(obs_data_t *settings, obs_output_t *output)
{
	auto *decklink = new DeckLinkOutput(output, deviceEnum);
	decklink->deviceHash = obs_data_get_string(settings, DEVICE_HASH);
	decklink->modeID = obs_data_get_int(settings, MODE_ID);
	decklink->SetBufferCount(static_cast<int>(obs_data_get_int(settings, BUFFER_COUNT)));

	decklink_output_set_video_conversion(decklink);
	return decklink;
}

static void decklink_output_update(void *data, obs_data_t *settings)
{
	auto *decklink = static_cast<DeckLinkOutput *>(data);
	decklink->deviceHash = obs_data_get_string(settings, DEVICE_HASH);
	decklink->modeID = obs_data_get_int(settings, MODE_ID);
	decklink->SetBufferCount(static_cast<int>(obs_data_get_int(settings, BUFFER_COUNT)));
	decklink_output_set_video_conversion(decklink);
}

static void decklink_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, BUFFER_COUNT, 2);
}

static bool decklink_output_start(void *data)
{
	auto *decklink = static_cast<DeckLinkOutput *>(data);
	struct obs_audio_info audioInfo = {};

	if (!obs_get_audio_info(&audioInfo)) {
		blog(LOG_WARNING, "No active OBS audio configuration");
		return false;
	}

	if (!decklink->deviceHash || !*decklink->deviceHash) {
		blog(LOG_WARNING, "No DeckLink device has been selected");
		return false;
	}

	decklink->audio_samplerate = 48000;
	decklink->audio_planes = 2;
	decklink->audio_size = get_audio_size(AUDIO_FORMAT_16BIT, SPEAKERS_STEREO, 1);
	decklink->start_timestamp = 0;

	ComPtr<DeckLinkDevice> device;
	device.Set(deviceEnum->FindByHash(decklink->deviceHash));
	if (!device) {
		blog(LOG_ERROR, "Selected DeckLink device is not available");
		return false;
	}

	DeckLinkDeviceMode *mode = device->FindOutputMode(decklink->modeID);
	if (!mode) {
		blog(LOG_ERROR, "Selected DeckLink output mode is not available");
		return false;
	}

	struct obs_video_info videoInfo = {};
	if (!obs_get_video_info(&videoInfo)) {
		blog(LOG_ERROR, "Could not retrieve OBS video information");
		return false;
	}

	if (!mode->IsEqualFrameRate(videoInfo.fps_num, videoInfo.fps_den)) {
		blog(LOG_ERROR, "OBS frame rate does not match the selected DeckLink output mode");
		return false;
	}

	decklink->SetSize(mode->GetWidth(), mode->GetHeight());
	if (!decklink->Activate(device, decklink->modeID)) {
		blog(LOG_ERROR, "Could not activate the selected DeckLink output");
		return false;
	}

	struct audio_convert_info audioConversion = {};
	audioConversion.format = AUDIO_FORMAT_16BIT;
	audioConversion.speakers = SPEAKERS_STEREO;
	audioConversion.samples_per_sec = 48000;
	obs_output_set_audio_conversion(decklink->GetOutput(), &audioConversion);

	if (!obs_output_begin_data_capture(decklink->GetOutput(), 0)) {
		blog(LOG_ERROR, "OBS could not begin DeckLink data capture");
		decklink->Deactivate();
		return false;
	}

	return true;
}

static void decklink_output_stop(void *data, uint64_t)
{
	auto *decklink = static_cast<DeckLinkOutput *>(data);
	obs_output_end_data_capture(decklink->GetOutput());
	decklink->Deactivate();
}

static void decklink_output_raw_video(void *data, video_data *frame)
{
	auto *decklink = static_cast<DeckLinkOutput *>(data);
	if (!decklink->start_timestamp) {
		decklink->start_timestamp = frame->timestamp;
	}
	decklink->UpdateVideoFrame(frame);
}

static bool prepare_audio(DeckLinkOutput *decklink, const audio_data *frame, audio_data *output)
{
	*output = *frame;

	if (frame->timestamp < decklink->start_timestamp) {
		const uint64_t duration = util_mul_div64(frame->frames, 1000000000ULL, decklink->audio_samplerate);
		const uint64_t endTimestamp = frame->timestamp + duration;
		if (endTimestamp <= decklink->start_timestamp) {
			return false;
		}

		uint64_t cutoff = decklink->start_timestamp - frame->timestamp;
		output->timestamp += cutoff;
		cutoff = util_mul_div64(cutoff, decklink->audio_samplerate, 1000000000ULL);

		for (size_t i = 0; i < decklink->audio_planes; ++i) {
			output->data[i] += decklink->audio_size * static_cast<size_t>(cutoff);
		}
		output->frames -= static_cast<uint32_t>(cutoff);
	}

	return true;
}

static void decklink_output_raw_audio(void *data, audio_data *frames)
{
	auto *decklink = static_cast<DeckLinkOutput *>(data);
	if (!decklink->start_timestamp) {
		return;
	}

	audio_data output = {};
	if (prepare_audio(decklink, frames, &output)) {
		decklink->WriteAudio(&output);
	}
}

static bool decklink_output_device_changed(obs_properties_t *props, obs_property_t *list, obs_data_t *settings)
{
	const char *hash = obs_data_get_string(settings, DEVICE_HASH);
	if (!hash || !*hash) {
		return true;
	}

	const char *deviceName = obs_data_get_string(settings, DEVICE_NAME);
	const char *modeName = obs_data_get_string(settings, MODE_NAME);
	const long long modeId = obs_data_get_int(settings, MODE_ID);

	bool found = false;
	const size_t itemCount = obs_property_list_item_count(list);
	for (size_t i = 0; i < itemCount; ++i) {
		const char *currentHash = obs_property_list_item_string(list, i);
		if (currentHash && strcmp(hash, currentHash) == 0) {
			found = true;
			break;
		}
	}

	if (!found) {
		obs_property_list_insert_string(list, 0, deviceName && *deviceName ? deviceName : "Unavailable device", hash);
		obs_property_list_item_disable(list, 0, true);
	}

	obs_property_t *modeList = obs_properties_get(props, MODE_ID);
	obs_property_list_clear(modeList);

	ComPtr<DeckLinkDevice> device;
	device.Set(deviceEnum->FindByHash(hash));
	if (!device) {
		obs_property_list_add_int(modeList, modeName && *modeName ? modeName : "Unavailable mode", modeId);
		obs_property_list_item_disable(modeList, 0, true);
		return true;
	}

	struct obs_video_info videoInfo = {};
	const bool haveVideoInfo = obs_get_video_info(&videoInfo);
	for (DeckLinkDeviceMode *mode : device->GetOutputModes()) {
		if (!haveVideoInfo || mode->IsEqualFrameRate(videoInfo.fps_num, videoInfo.fps_den)) {
			obs_property_list_add_int(modeList, mode->GetName().c_str(), mode->GetId());
		}
	}

	return true;
}

static obs_properties_t *decklink_output_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	auto *decklink = static_cast<DeckLinkOutput *>(data);

	obs_property_t *deviceList =
		obs_properties_add_list(props, DEVICE_HASH, TEXT_DEVICE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(deviceList, decklink_output_device_changed);
	fill_out_devices(deviceList);

	obs_properties_add_list(props, MODE_ID, TEXT_MODE, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	if (decklink && decklink->deviceHash && *decklink->deviceHash) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, DEVICE_HASH, decklink->deviceHash);
		obs_data_set_int(settings, MODE_ID, decklink->modeID);

		ComPtr<DeckLinkDevice> device;
		device.Set(deviceEnum->FindByHash(decklink->deviceHash));
		if (device) {
			obs_data_set_string(settings, DEVICE_NAME, device->GetDisplayName().c_str());
			DeckLinkDeviceMode *mode = device->FindOutputMode(decklink->modeID);
			if (mode) {
				obs_data_set_string(settings, MODE_NAME, mode->GetName().c_str());
			}
		}

		decklink_output_device_changed(props, deviceList, settings);
		obs_data_release(settings);
	}

	obs_property_t *bufferList =
		obs_properties_add_list(props, BUFFER_COUNT, TEXT_BUFFER_COUNT, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bufferList, "2 - Low latency", 2);
	obs_property_list_add_int(bufferList, "3 - Compatibility", 3);

	return props;
}

static const char *decklink_output_get_name(void *)
{
	return TEXT_OUTPUT_NAME;
}

static int decklink_output_get_dropped_frames(void *data)
{
	return static_cast<DeckLinkOutput *>(data)->GetDroppedFrames();
}

struct obs_output_info create_decklink_output_info()
{
	struct obs_output_info info = {};
	info.id = "obs_decklink_low_latency_output";
	info.flags = OBS_OUTPUT_AV;
	info.get_name = decklink_output_get_name;
	info.create = decklink_output_create;
	info.destroy = decklink_output_destroy;
	info.start = decklink_output_start;
	info.stop = decklink_output_stop;
	info.get_properties = decklink_output_properties;
	info.raw_video = decklink_output_raw_video;
	info.raw_audio = decklink_output_raw_audio;
	info.update = decklink_output_update;
	info.get_defaults = decklink_output_defaults;
	info.get_dropped_frames = decklink_output_get_dropped_frames;
	return info;
}
