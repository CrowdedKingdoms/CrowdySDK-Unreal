#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

#include <atomic>

#if PLATFORM_WINDOWS
struct IMMDeviceEnumerator;
class FCrowdyVoiceNotificationClient;
#endif

/**
 * Notices when the default microphone changes or goes away, so voice chat can reopen it. Output devices
 * are ignored. Windows only; elsewhere it never reports a change.
 *
 * The OS calls in on its own thread and only sets a flag; the game thread collects it.
 */
class FCrowdyVoiceDeviceMonitor
{
public:
	FCrowdyVoiceDeviceMonitor();
	~FCrowdyVoiceDeviceMonitor();

	/** True once for each burst of default capture device changes since the last call. */
	bool ConsumeCaptureDeviceChange();

	/** False where device changes cannot be seen, so a missing microphone must be polled for instead. */
	bool IsWatching() const;

private:
	FCrowdyVoiceDeviceMonitor(const FCrowdyVoiceDeviceMonitor&) = delete;
	FCrowdyVoiceDeviceMonitor& operator=(const FCrowdyVoiceDeviceMonitor&) = delete;

	// Shared with the OS callback object, which can outlive this monitor by a call already in flight.
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bChanged;

#if PLATFORM_WINDOWS
	IMMDeviceEnumerator* Enumerator = nullptr;
	FCrowdyVoiceNotificationClient* Client = nullptr;
	bool bComInitialized = false;
#endif
};
