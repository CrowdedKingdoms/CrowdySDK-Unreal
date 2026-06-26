#include "Subsystem/AsyncActions/CrowdyAvatarsWriteActions.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyAvatars.h"

static UCrowdyAvatars* GetAvatarsSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyAvatars>() : nullptr;
}

UCrowdyAvatars_CreateAvatar* UCrowdyAvatars_CreateAvatar::CreateAvatar(UObject* WorldContextObject, const FString& Name)
{
	UCrowdyAvatars_CreateAvatar* Action = NewObject<UCrowdyAvatars_CreateAvatar>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_CreateAvatar::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_CreateAvatar::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_CreateAvatar::HandleError);
	Avatars->CreateAvatar(Name, S, E);
}

void UCrowdyAvatars_CreateAvatar::HandleSuccess(FCrowdyAvatar Avatar)
{
	OnSuccess.Broadcast(Avatar);
	SetReadyToDestroy();
}

void UCrowdyAvatars_CreateAvatar::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_UpdateAvatar* UCrowdyAvatars_UpdateAvatar::UpdateAvatar(UObject* WorldContextObject, int64 AvatarId,
                                                                       const FString& Name)
{
	UCrowdyAvatars_UpdateAvatar* Action = NewObject<UCrowdyAvatars_UpdateAvatar>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_UpdateAvatar::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_UpdateAvatar::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_UpdateAvatar::HandleError);
	Avatars->UpdateAvatar(AvatarId, Name, S, E);
}

void UCrowdyAvatars_UpdateAvatar::HandleSuccess(FCrowdyAvatar Avatar)
{
	OnSuccess.Broadcast(Avatar);
	SetReadyToDestroy();
}

void UCrowdyAvatars_UpdateAvatar::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_DeleteAvatar* UCrowdyAvatars_DeleteAvatar::DeleteAvatar(UObject* WorldContextObject, int64 AvatarId)
{
	UCrowdyAvatars_DeleteAvatar* Action = NewObject<UCrowdyAvatars_DeleteAvatar>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_DeleteAvatar::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarVoidSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_DeleteAvatar::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_DeleteAvatar::HandleError);
	Avatars->DeleteAvatar(AvatarId, S, E);
}

void UCrowdyAvatars_DeleteAvatar::HandleSuccess()
{
	OnSuccess.Broadcast();
	SetReadyToDestroy();
}

void UCrowdyAvatars_DeleteAvatar::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_UpdatePublicAvatarState* UCrowdyAvatars_UpdatePublicAvatarState::UpdatePublicAvatarState(
	UObject* WorldContextObject, int64 AvatarId, const FString& RawState)
{
	UCrowdyAvatars_UpdatePublicAvatarState* Action = NewObject<UCrowdyAvatars_UpdatePublicAvatarState>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->RawState = RawState;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_UpdatePublicAvatarState::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_UpdatePublicAvatarState::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_UpdatePublicAvatarState::HandleError);
	Avatars->UpdatePublicAvatarState(AvatarId, RawState, S, E);
}

void UCrowdyAvatars_UpdatePublicAvatarState::HandleSuccess(FCrowdyAvatar Avatar)
{
	OnSuccess.Broadcast(Avatar);
	SetReadyToDestroy();
}

void UCrowdyAvatars_UpdatePublicAvatarState::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_UpdatePrivateAvatarState* UCrowdyAvatars_UpdatePrivateAvatarState::UpdatePrivateAvatarState(
	UObject* WorldContextObject, int64 AvatarId, const FString& RawState)
{
	UCrowdyAvatars_UpdatePrivateAvatarState* Action = NewObject<UCrowdyAvatars_UpdatePrivateAvatarState>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->RawState = RawState;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_UpdatePrivateAvatarState::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAvatarSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_UpdatePrivateAvatarState::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_UpdatePrivateAvatarState::HandleError);
	Avatars->UpdatePrivateAvatarState(AvatarId, RawState, S, E);
}

void UCrowdyAvatars_UpdatePrivateAvatarState::HandleSuccess(FCrowdyAvatar Avatar)
{
	OnSuccess.Broadcast(Avatar);
	SetReadyToDestroy();
}

void UCrowdyAvatars_UpdatePrivateAvatarState::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}

UCrowdyAvatars_UpdateAvatarAppState* UCrowdyAvatars_UpdateAvatarAppState::UpdateAvatarAppState(
	UObject* WorldContextObject, int64 AvatarId, const FString& RawState)
{
	UCrowdyAvatars_UpdateAvatarAppState* Action = NewObject<UCrowdyAvatars_UpdateAvatarAppState>();
	Action->WorldContextObject = WorldContextObject;
	Action->AvatarId = AvatarId;
	Action->RawState = RawState;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyAvatars_UpdateAvatarAppState::Activate()
{
	UCrowdyAvatars* Avatars = GetAvatarsSubsystem(WorldContextObject);
	if (!Avatars)
	{
		const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(TEXT("UCrowdyAvatars not found"));
		HandleError(Err, Err.Message);
		return;
	}
	FOnAppStateSuccess S;
	S.BindDynamic(this, &UCrowdyAvatars_UpdateAvatarAppState::HandleSuccess);
	FOnAvatarError E;
	E.BindDynamic(this, &UCrowdyAvatars_UpdateAvatarAppState::HandleError);
	Avatars->UpdateAvatarAppState(AvatarId, RawState, S, E);
}

void UCrowdyAvatars_UpdateAvatarAppState::HandleSuccess(FCrowdyAppAvatarState AppState)
{
	OnSuccess.Broadcast(AppState);
	SetReadyToDestroy();
}

void UCrowdyAvatars_UpdateAvatarAppState::HandleError(FCrowdyAvatarError Error, FString Message)
{
	UE_LOG(LogCrowdyServices, Warning, TEXT("%s"), *Message);
	OnError.Broadcast(Error, Error.Message);
	SetReadyToDestroy();
}
