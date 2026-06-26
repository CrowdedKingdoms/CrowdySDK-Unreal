#pragma once

#include "CoreMinimal.h"
#include "Engine/LatentActionManager.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"
#include "Queries/Data/Avatar/Types/FCrowdyAppAvatarState.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatarError.h"
#include "CrowdyAvatars.generated.h"

class UCrowdyQuerySubsystem;
class FCrowdyDataRegistry;

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAvatarSuccess, FCrowdyAvatar, Avatar);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAvatarsSuccess, TArray<FCrowdyAvatar>, Avatars);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAppStateSuccess, FCrowdyAppAvatarState, AppState);

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnAppStatesSuccess, TArray<FCrowdyAppAvatarState>, AppStates);

DECLARE_DYNAMIC_DELEGATE(FOnAvatarVoidSuccess);

DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnAvatarError, FCrowdyAvatarError, Error, FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMyAvatarsCacheChanged, TArray<FCrowdyAvatar>, Avatars);

UCLASS()
class CROWDYSERVICES_API UCrowdyAvatars : public UGameInstanceSubsystem, public ICrowdyQueryReceptionLayer
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void InjectDependencies(FCrowdyDataRegistry* InDataRegistry, UCrowdyQuerySubsystem* InQuerySubsystem);

	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) override;
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const override;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Avatars")
	FOnMyAvatarsCacheChanged OnMyAvatarsCacheChanged;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Avatars|Cache")
	bool HasCachedAvatars() const { return bCachePopulated; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|Avatars|Cache")
	TArray<FCrowdyAvatar> GetCachedMyAvatars() const { return CachedMyAvatars; }

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Cache")
	bool GetMyAvatarById(int64 AvatarId, FCrowdyAvatar& OutAvatar) const;

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Queries|Avatar")
	void GetMyAvatars(FOnAvatarsSuccess OnSuccess, FOnAvatarError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Queries|Avatar")
	void GetAvatar(int64 AvatarId, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Queries|Avatar")
	void GetUserAvatars(int64 UserId, FOnAvatarsSuccess OnSuccess, FOnAvatarError OnError);

	/** AppId is resolved from project settings. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Queries|State")
	void GetAvatarAppState(int64 AvatarId, FOnAppStateSuccess OnSuccess, FOnAvatarError OnError);

	/** Batch-reads per-avatar app state for many avatars. AppId from project settings. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Queries|State")
	void GetAvatarAppStates(const TArray<int64>& AvatarIds, FOnAppStatesSuccess OnSuccess, FOnAvatarError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|Avatar")
	void CreateAvatar(const FString& Name, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|Avatar")
	void UpdateAvatar(int64 AvatarId, const FString& Name, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|Avatar")
	void DeleteAvatar(int64 AvatarId, FOnAvatarVoidSuccess OnSuccess, FOnAvatarError OnError);

	/** Updates only publicState; privateState is untouched on the server. State must be base64-encoded. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|State")
	void UpdatePublicAvatarState(int64 AvatarId, const FString& PublicState,
	                             FOnAvatarSuccess OnSuccess, FOnAvatarError OnError);

	/** Updates only privateState; publicState is untouched on the server. State must be base64-encoded. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|State")
	void UpdatePrivateAvatarState(int64 AvatarId, const FString& PrivateState,
	                              FOnAvatarSuccess OnSuccess, FOnAvatarError OnError);

	/** Updates both public and private state atomically. Prefer the separate methods when only one field changes. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|State")
	void UpdateAvatarState(int64 AvatarId, const FString& PublicState, const FString& PrivateState,
	                       FOnAvatarSuccess OnSuccess, FOnAvatarError OnError);

	/** Pass an empty string to clear. AppId from project settings. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Avatars|Mutations|State")
	void UpdateAvatarAppState(int64 AvatarId, const FString& State,
	                          FOnAppStateSuccess OnSuccess, FOnAvatarError OnError);

	/** Serializes any USTRUCT to a base64 string for use as avatar state. Connect any struct pin to State. */
	UFUNCTION(BlueprintCallable, CustomThunk,
		meta=(CustomStructureParam="State"),
		Category="Crowdy SDK|Avatars|State", DisplayName="Serialize Struct to Avatar State")
	static FString SerializeToAvatarState(const int32& State);
	DECLARE_FUNCTION(execSerializeToAvatarState);

	/** Deserializes a base64 avatar state string back into a USTRUCT. Returns false if decoding fails. */
	UFUNCTION(BlueprintCallable, CustomThunk,
		meta=(CustomStructureParam="OutStruct"),
		Category="Crowdy SDK|Avatars|State", DisplayName="Deserialize Avatar State to Struct")
	static bool DeserializeFromAvatarState(const FString& State, int32& OutStruct);
	DECLARE_FUNCTION(execDeserializeFromAvatarState);

	/** Fetches this app's per-avatar state and deserializes it into OutState (any USTRUCT). Branch on bSuccess. */
	UFUNCTION(BlueprintCallable, CustomThunk,
		meta=(Latent, LatentInfo="LatentInfo", CustomStructureParam="OutState", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|State", DisplayName="Get Avatar App State As")
	void GetAvatarAppStateAs(UObject* WorldContextObject, FLatentActionInfo LatentInfo, int64 AvatarId, int32& OutState,
	                         bool& bSuccess, FCrowdyAvatarError& OutError);
	DECLARE_FUNCTION(execGetAvatarAppStateAs);

	/** Serializes any USTRUCT and sets it as this app's per-avatar state. Branch on bSuccess. */
	UFUNCTION(BlueprintCallable, CustomThunk,
		meta=(Latent, LatentInfo="LatentInfo", CustomStructureParam="State", WorldContext="WorldContextObject"),
		Category="Crowdy SDK|Avatars|State", DisplayName="Set Avatar App State As")
	void SetAvatarAppStateAs(UObject* WorldContextObject, FLatentActionInfo LatentInfo, int64 AvatarId,
	                         const int32& State, bool& bSuccess, FCrowdyAvatarError& OutError);
	DECLARE_FUNCTION(execSetAvatarAppStateAs);

private:
	UPROPERTY()
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;

	mutable FCriticalSection CallbackMutex;
	TMap<EQueryResponseType, TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>> PendingCallbacks;

	TArray<FCrowdyAvatar> CachedMyAvatars;
	bool bCachePopulated = false;

	int64 GetAppId() const;

	void PushCallback(EQueryResponseType Type, TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback);
	void FireCallback(TSharedPtr<ICrowdyQueryResponse> Response);
};
