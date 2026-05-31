#include "AWinAudioDeviceMonitor.h"

#if PLATFORM_WINDOWS

#include "Microsoft/AllowMicrosoftPlatformAtomics.h"
#include "Async/Async.h"

// Windows notification client class
class FWindowsAudioDeviceNotificationClient : public IMMNotificationClient
{
public:
    FWindowsAudioDeviceNotificationClient(AWinAudioDeviceMonitor* InOwner)
        : RefCount(1)
        , Owner(InOwner)
    {
    }

    // IUnknown methods
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&RefCount);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG ulRef = InterlockedDecrement(&RefCount);
        if (0 == ulRef)
        {
            delete this;
        }
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) override
    {
        if (IID_IUnknown == riid)
        {
            AddRef();
            *ppvInterface = (IUnknown*)this;
        }
        else if (__uuidof(IMMNotificationClient) == riid)
        {
            AddRef();
            *ppvInterface = (IMMNotificationClient*)this;
        }
        else
        {
            *ppvInterface = NULL;
            return E_NOINTERFACE;
        }
        return S_OK;
    }

    // IMMNotificationClient methods
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override
    {
        if (!Owner.IsValid()) return S_OK;
        
        FString DeviceId = FString(pwstrDeviceId);
        
        // Execute in the game thread
        AsyncTask(ENamedThreads::GameThread, [this, DeviceId, dwNewState]() {
            if (!Owner.IsValid()) return;
            
            if (dwNewState == DEVICE_STATE_ACTIVE)
            {
                Owner->HandleDeviceConnected(DeviceId);
            }
            else if (dwNewState == DEVICE_STATE_UNPLUGGED || dwNewState == DEVICE_STATE_NOTPRESENT)
            {
                Owner->HandleDeviceDisconnected(DeviceId);
            }
        });
        
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override
    {
        // Device added doesn't necessarily mean it's active yet
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override
    {
        // Device removed is handled in OnDeviceStateChanged
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override
    {
        // We're not handling default device changes in this implementation
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override
    {
        // We're not handling property changes in this implementation
        return S_OK;
    }

private:
    LONG RefCount;
    TWeakObjectPtr<AWinAudioDeviceMonitor> Owner;
};

#endif
AWinAudioDeviceMonitor::AWinAudioDeviceMonitor(): bIsMonitoringActive(false)
{
    PrimaryActorTick.bCanEverTick = false;
}

AWinAudioDeviceMonitor::~AWinAudioDeviceMonitor()
{
    ShutdownAudioDeviceMonitoring();
}

void AWinAudioDeviceMonitor::BeginPlay()
{
	InitializeAudioDeviceMonitoring();
}

void AWinAudioDeviceMonitor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownAudioDeviceMonitoring();
}

void AWinAudioDeviceMonitor::InitializeAudioDeviceMonitoring()
{
#if PLATFORM_WINDOWS
    if (bIsMonitoringActive)
    {
        UE_LOG(LogTemp, Warning, TEXT("Audio device monitoring is already active"));
        return;
    }

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)DeviceEnumerator.GetAddressOf()
    );

    if (SUCCEEDED(hr))
    {
        // Create the notification client
        NotificationClient = MakeShareable(new FWindowsAudioDeviceNotificationClient(this));
        
        // Register for notifications
        hr = DeviceEnumerator->RegisterEndpointNotificationCallback(NotificationClient.Get());
        
        if (SUCCEEDED(hr))
        {
            bIsMonitoringActive = true;
            UE_LOG(LogTemp, Log, TEXT("Audio device monitoring initialized successfully"));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to register for audio device notifications. Error code: %d"), hr);
        }
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to create audio device enumerator. Error code: %d"), hr);
    }
#else
    UE_LOG(LogTemp, Warning, TEXT("Audio device monitoring is only supported on Windows"));
#endif
}

void AWinAudioDeviceMonitor::ShutdownAudioDeviceMonitoring()
{
#if PLATFORM_WINDOWS
    if (!bIsMonitoringActive)
    {
        return;
    }

    if (DeviceEnumerator && NotificationClient)
    {
        // Unregister from notifications
        DeviceEnumerator->UnregisterEndpointNotificationCallback(NotificationClient.Get());
    }
    
    // Release COM references
    NotificationClient.Reset();
    DeviceEnumerator.Reset();
    
    bIsMonitoringActive = false;
    UE_LOG(LogTemp, Log, TEXT("Audio device monitoring shutdown"));
#endif
}

TArray<FString> AWinAudioDeviceMonitor::GetAvailableAudioDevices(bool bOutputDevices)
{
    TArray<FString> DeviceList;
    
#if PLATFORM_WINDOWS
    if (!DeviceEnumerator)
    {
        UE_LOG(LogTemp, Warning, TEXT("Device enumerator not initialized"));
        return DeviceList;
    }
    
    // Determine data flow direction
    EDataFlow DataFlow = bOutputDevices ? eRender : eCapture;
    
    // Get device collection
    ComPtr<IMMDeviceCollection> DeviceCollection;
    HRESULT hr = DeviceEnumerator->EnumAudioEndpoints(
        DataFlow,
        DEVICE_STATE_ACTIVE,
        DeviceCollection.GetAddressOf()
    );
    
    if (SUCCEEDED(hr))
    {
        // Get count of devices
        UINT DeviceCount;
        hr = DeviceCollection->GetCount(&DeviceCount);
        
        if (SUCCEEDED(hr))
        {
            // Iterate through devices
            for (UINT i = 0; i < DeviceCount; i++)
            {
                ComPtr<IMMDevice> Device;
                hr = DeviceCollection->Item(i, Device.GetAddressOf());
                
                if (SUCCEEDED(hr))
                {
                    // Get device ID
                    LPWSTR DeviceId;
                    hr = Device->GetId(&DeviceId);
                    
                    if (SUCCEEDED(hr))
                    {
                        // Get device friendly name
                        ComPtr<IPropertyStore> PropertyStore;
                        hr = Device->OpenPropertyStore(STGM_READ, PropertyStore.GetAddressOf());
                        
                        if (SUCCEEDED(hr))
                        {
                            PROPVARIANT PropVariant;
                            PropVariantInit(&PropVariant);
                            
                            hr = PropertyStore->GetValue(PKEY_Device_FriendlyName, &PropVariant);
                            
                            if (SUCCEEDED(hr) && PropVariant.vt == VT_LPWSTR)
                            {
                                FString DeviceName = FString(PropVariant.pwszVal);
                                DeviceList.Add(DeviceName);
                            }
                            
                            PropVariantClear(&PropVariant);
                        }
                        
                        // Free device ID string
                        CoTaskMemFree(DeviceId);
                    }
                }
            }
        }
    }
#endif
    
    return DeviceList;
}

void AWinAudioDeviceMonitor::HandleDeviceConnected(const FString& DeviceId)
{
    UE_LOG(LogTemp, Log, TEXT("Audio device connected: %s"), *DeviceId);
    OnAudioDeviceConnected.Broadcast(DeviceId);
}

void AWinAudioDeviceMonitor::HandleDeviceDisconnected(const FString& DeviceId)
{
    UE_LOG(LogTemp, Log, TEXT("Audio device disconnected: %s"), *DeviceId);
    OnAudioDeviceDisconnected.Broadcast(DeviceId);
}
