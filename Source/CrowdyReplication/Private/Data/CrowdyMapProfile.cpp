#include "Data/CrowdyMapProfile.h"

#if WITH_EDITOR
#include "Data/CrowdyRenderingBackend.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "Misc/DataValidation.h"

// The server stops relaying an actor whose presence it has not refreshed for about this long.
static constexpr float CrowdyServerPresenceStaleSeconds = 5.f;

EDataValidationResult UCrowdyMapProfile::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	const bool bIdleActorsSend = bUseAutoReplicator && bSendActorStateOnlyOnChange;
	const float Keyframe = ActorKeyframeIntervalSeconds;
	const float Timeout = ActorManagement.ActorTimeoutThreshold;

	// Heartbeats only refresh the server's presence; other players see an idle actor through its keyframes alone.
	if (bIdleActorsSend && ActorManagement.bUseCrowdyActorTracker && (Keyframe <= 0.f || Keyframe >= Timeout))
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("Other players see an actor that stops moving only through its keyframes, so with a Keyframe Interval of %.1f s they drop it after the %.1f s Actor Timeout. Set a Keyframe Interval above 0 and below the Actor Timeout."),
			Keyframe, Timeout)));
	}

	const float PresenceGap = FMath::Min(
		Keyframe > 0.f ? Keyframe : TNumericLimits<float>::Max(),
		ActorHeartbeatIntervalSeconds > 0.f ? ActorHeartbeatIntervalSeconds : TNumericLimits<float>::Max());

	if (bIdleActorsSend && PresenceGap >= CrowdyServerPresenceStaleSeconds)
	{
		Result = EDataValidationResult::Invalid;
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("An actor that stops moving refreshes its presence on the server less often than every %.0f s (the shorter of Keyframe Interval and Heartbeat Interval), so the server stops relaying it to everyone. Set a Heartbeat Interval below %.0f s."),
			CrowdyServerPresenceStaleSeconds, CrowdyServerPresenceStaleSeconds)));
	}

	const UClass* BackendClass = ActorManagement.BackendClass.Get();
	const UCrowdyRenderingBackendConfig* BackendConfig = ActorManagement.BackendConfig;
	if (!BackendClass || !BackendConfig)
		return Result;

	const UCrowdyRenderingBackend* BackendDefaults = Cast<UCrowdyRenderingBackend>(BackendClass->GetDefaultObject());
	const UClass* ExpectedConfig = BackendDefaults ? BackendDefaults->GetBackendConfigClass() : nullptr;
	if (!ExpectedConfig || BackendConfig->IsA(ExpectedConfig))
		return Result;

	Context.AddError(FText::FromString(FString::Printf(
		TEXT("Backend Config is a %s, but %s reads a %s, so the map cannot use it. Pick a %s or clear Backend Config."),
		*BackendConfig->GetClass()->GetDisplayNameText().ToString(), *BackendClass->GetDisplayNameText().ToString(),
		*ExpectedConfig->GetDisplayNameText().ToString(), *ExpectedConfig->GetDisplayNameText().ToString())));
	return EDataValidationResult::Invalid;
}
#endif
