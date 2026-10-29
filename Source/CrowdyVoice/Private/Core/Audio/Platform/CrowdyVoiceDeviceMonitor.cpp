#include "Core/Audio/Platform/CrowdyVoiceDeviceMonitor.h"

#include "CrowdyVoiceLog.h"

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <mmdeviceapi.h>
#include "Windows/HideWindowsPlatformTypes.h"

/** Reference counted by COM, so a notification already in flight keeps it alive after unregistering. */
class FCrowdyVoiceNotificationClient final : public IMMNotificationClient
{
public:
	explicit FCrowdyVoiceNotificationClient(const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>& InChanged)
		: Changed(InChanged)
	{
	}

	virtual ULONG STDMETHODCALLTYPE AddRef() override
	{
		return ++RefCount;
	}

	virtual ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG Remaining = --RefCount;
		if (Remaining == 0)
		{
			delete this;
		}
		return Remaining;
	}

	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Riid, void** OutInterface) override
	{
		if (Riid != __uuidof(IUnknown) && Riid != __uuidof(IMMNotificationClient))
		{
			*OutInterface = nullptr;
			return E_NOINTERFACE;
		}
		*OutInterface = static_cast<IMMNotificationClient*>(this);
		AddRef();
		return S_OK;
	}

	// The OS also calls this with no device when the last microphone goes away.
	virtual HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow Flow, ERole, LPCWSTR) override
	{
		if (Flow == eCapture || Flow == eAll)
		{
			Changed->store(true);
		}
		return S_OK;
	}

	virtual HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
	virtual HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
	virtual HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
	virtual HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
	std::atomic<ULONG> RefCount{1};
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Changed;
};

FCrowdyVoiceDeviceMonitor::FCrowdyVoiceDeviceMonitor()
	: bChanged(MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false))
{
	bComInitialized = FWindowsPlatformMisc::CoInitialize();

	HRESULT Result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&Enumerator));
	if (FAILED(Result) || !Enumerator)
	{
		Enumerator = nullptr;
		UE_LOG(LogCrowdyVoice, Warning, TEXT("Voice chat cannot watch audio devices (0x%08x); switching microphones needs Stop and Start Voice Chat."),
			static_cast<uint32>(Result));
		return;
	}

	Client = new FCrowdyVoiceNotificationClient(bChanged);
	Result = Enumerator->RegisterEndpointNotificationCallback(Client);
	if (SUCCEEDED(Result))
	{
		return;
	}

	Client->Release();
	Client = nullptr;
	Enumerator->Release();
	Enumerator = nullptr;
	UE_LOG(LogCrowdyVoice, Warning, TEXT("Voice chat cannot watch audio devices (0x%08x); switching microphones needs Stop and Start Voice Chat."),
		static_cast<uint32>(Result));
}

FCrowdyVoiceDeviceMonitor::~FCrowdyVoiceDeviceMonitor()
{
	if (Enumerator && Client)
	{
		Enumerator->UnregisterEndpointNotificationCallback(Client);
	}
	if (Client)
	{
		Client->Release();
	}
	if (Enumerator)
	{
		Enumerator->Release();
	}
	if (bComInitialized)
	{
		FWindowsPlatformMisc::CoUninitialize();
	}
}

bool FCrowdyVoiceDeviceMonitor::IsWatching() const
{
	return Client != nullptr;
}

#else

FCrowdyVoiceDeviceMonitor::FCrowdyVoiceDeviceMonitor()
	: bChanged(MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false))
{
	static bool bLogged = false;
	UE_CLOG(!bLogged && CrowdyVoiceTrace::Voice(), LogCrowdyVoice, Log,
		TEXT("Voice chat does not watch audio devices on this platform; a missing microphone is retried every few seconds."));
	bLogged = true;
}

FCrowdyVoiceDeviceMonitor::~FCrowdyVoiceDeviceMonitor() = default;

bool FCrowdyVoiceDeviceMonitor::IsWatching() const
{
	return false;
}

#endif

bool FCrowdyVoiceDeviceMonitor::ConsumeCaptureDeviceChange()
{
	return bChanged->exchange(false);
}
