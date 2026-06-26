#include "Subsystem/AsyncActions/CrowdyAvatarsQueryActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyAvatars.h"

static UCrowdyAvatars* GetAvatarsSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyAvatars>() : nullptr;
}

UCrowdyAvatars_GetMyAvatars* UCrowdyAvatars_GetMyAvatars::GetMyAvatars(UObject* WorldContextObject)
{
	UCrowdyAvatars_GetMyAvatars* Action = NewObject<UCrowdyAvatars_GetMyAvatars>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_GetMyAvatars::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarsSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_GetMyAvatars::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_GetMyAvatars::HandleError);
	Avatars->GetMyAvatars(S, E);
}

void UCrowdyAvatars_GetMyAvatars::HandleSuccess(TArray<FCrowdyAvatar> Avatars)
{
	FCrowdyAvatarList R;
	R.Avatars = MoveTemp(Avatars);
	OnSuccess.Broadcast(R);
	SetReadyToDestroy();
}

void UCrowdyAvatars_GetMyAvatars::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_GetAvatar* UCrowdyAvatars_GetAvatar::GetAvatar(UObject* WorldContextObject, int64 AvatarId)
{
	UCrowdyAvatars_GetAvatar* Action = NewObject<UCrowdyAvatars_GetAvatar>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_GetAvatar::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_GetAvatar::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_GetAvatar::HandleError);
	Avatars->GetAvatar(AvatarId, S, E);
}

void UCrowdyAvatars_GetAvatar::HandleSuccess(FCrowdyAvatar Avatar)
{
	OnSuccess.Broadcast(Avatar);
	SetReadyToDestroy();
}

void UCrowdyAvatars_GetAvatar::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_GetUserAvatars* UCrowdyAvatars_GetUserAvatars::GetUserAvatars(UObject* WorldContextObject, int64 UserId)
{
	UCrowdyAvatars_GetUserAvatars* Action = NewObject<UCrowdyAvatars_GetUserAvatars>();
	Action->WorldContextObject = WorldContextObject;
	Action->UserId = UserId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_GetUserAvatars::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarsSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_GetUserAvatars::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_GetUserAvatars::HandleError);
	Avatars->GetUserAvatars(UserId, S, E);
}

void UCrowdyAvatars_GetUserAvatars::HandleSuccess(TArray<FCrowdyAvatar> Avatars)
{
	FCrowdyAvatarList R;
	R.Avatars = MoveTemp(Avatars);
	OnSuccess.Broadcast(R);
	SetReadyToDestroy();
}

void UCrowdyAvatars_GetUserAvatars::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_GetAvatarAppState* UCrowdyAvatars_GetAvatarAppState::GetAvatarAppState(
	UObject* WorldContextObject, int64 AvatarId)
{
	UCrowdyAvatars_GetAvatarAppState* Action = NewObject<UCrowdyAvatars_GetAvatarAppState>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_GetAvatarAppState::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAppStateSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_GetAvatarAppState::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_GetAvatarAppState::HandleError);
	Avatars->GetAvatarAppState(AvatarId, S, E);
}

void UCrowdyAvatars_GetAvatarAppState::HandleSuccess(FCrowdyAppAvatarState AppState)
{
	OnSuccess.Broadcast(AppState);
	SetReadyToDestroy();
}

void UCrowdyAvatars_GetAvatarAppState::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_GetAvatarAppStates* UCrowdyAvatars_GetAvatarAppStates::GetAvatarAppStates(
	UObject* WorldContextObject, const TArray<int64>& AvatarIds)
{
	UCrowdyAvatars_GetAvatarAppStates* Action = NewObject<UCrowdyAvatars_GetAvatarAppStates>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarIds = AvatarIds;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_GetAvatarAppStates::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAppStatesSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_GetAvatarAppStates::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_GetAvatarAppStates::HandleError);
	Avatars->GetAvatarAppStates(AvatarIds, S, E);
}

void UCrowdyAvatars_GetAvatarAppStates::HandleSuccess(TArray<FCrowdyAppAvatarState> AppStates)
{
	FCrowdyAppAvatarStateList R;
	R.AppStates = MoveTemp(AppStates);
	OnSuccess.Broadcast(R);
	SetReadyToDestroy();
}

void UCrowdyAvatars_GetAvatarAppStates::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}
