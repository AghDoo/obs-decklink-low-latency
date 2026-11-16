#include <obs-module.h>
#include <oleauto.h>

#include "decklink-devices.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-decklink-low-latency", "en-US")

DeckLinkDeviceDiscovery *deviceEnum = nullptr;

extern struct obs_output_info create_decklink_output_info();

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Independent low-latency output for Blackmagic DeckLink devices";
}

static bool log_sdk_version()
{
	ComPtr<IDeckLinkIterator> iterator;
	ComPtr<IDeckLinkAPIInformation> apiInformation;

	iterator.Set(CreateDeckLinkIteratorInstance());
	if (!iterator) {
		blog(LOG_WARNING, "Could not create a DeckLink iterator. Check that the Blackmagic Desktop Video driver is installed.");
		return false;
	}

	const HRESULT result = iterator->QueryInterface(IID_IDeckLinkAPIInformation, (void **)&apiInformation);
	if (result == S_OK) {
		decklink_string_t version = nullptr;
		if (apiInformation->GetString(BMDDeckLinkAPIVersion, &version) == S_OK && version) {
			std::string versionString;
			DeckLinkStringToStdString(version, versionString);
			blog(LOG_INFO, "DeckLink SDK compiled API version %s; installed API version %s",
			     BLACKMAGIC_DECKLINK_API_VERSION_STRING, versionString.c_str());
			SysFreeString(version);
		}
	}

	return true;
}

bool obs_module_load(void)
{
	if (!log_sdk_version()) {
		return false;
	}

	deviceEnum = new DeckLinkDeviceDiscovery();
	if (!deviceEnum->Init()) {
		delete deviceEnum;
		deviceEnum = nullptr;
		return false;
	}

	struct obs_output_info outputInfo = create_decklink_output_info();
	obs_register_output(&outputInfo);
	blog(LOG_INFO, "OBS DeckLink Low Latency output module loaded");
	return true;
}

void obs_module_unload(void)
{
	delete deviceEnum;
	deviceEnum = nullptr;
}
