#pragma once

#include <mutex>

#include <obs-module.h>

#include "platform.hpp"
#include "decklink-device-discovery.hpp"
#include "decklink-device-instance.hpp"

class DecklinkBase {
protected:
	DecklinkBase(DeckLinkDeviceDiscovery *discovery_);

	ComPtr<DeckLinkDeviceInstance> instance;
	DeckLinkDeviceDiscovery *discovery;
	mutable std::recursive_mutex deviceMutex;
	volatile long activateRefs = 0;

public:
	virtual ~DecklinkBase() = default;
	virtual bool Activate(DeckLinkDevice *device, long long modeId);
	virtual void Deactivate() = 0;

	DeckLinkDevice *GetDevice() const;
};
