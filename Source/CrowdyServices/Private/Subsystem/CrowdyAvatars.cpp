#include "Subsystem/CrowdyAvatars.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "LatentActions.h"
#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Misc/Base64.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "UObject/UnrealType.h"

using namespace CrowdyServiceApi;

namespace
{
	// The generated operation set these calls are looked up in, which is also what decides the endpoint each one
	// reaches and the bearer it carries.
	constexpr ECrowdyCppApiDomain AvatarsDomain = ECrowdyCppApiDomain::Avatars;

	constexpr const TCHAR* AvatarsLogName = TEXT("CrowdyAvatars");
}

void UCrowdyAvatars::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyAvatars::Deinitialize()
{
	// Released first: a completion can still arrive from the client's own pump after this point, and it must not
	// broadcast a cache change or run a Blueprint delegate while the game instance is shutting down.
	LiveSessionToken.Reset();

	Super::Deinitialize();
}

int64 UCrowdyAvatars::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

bool UCrowdyAvatars::GetMyAvatarById(int64 AvatarId, FCrowdyAvatar& OutAvatar) const
{
	for (const FCrowdyAvatar& A : CachedMyAvatars)
	{
		if (A.AvatarId == AvatarId)
		{
			OutAvatar = A;
			return true;
		}
	}
	return false;
}

void UCrowdyAvatars::GetMyAvatars(FOnAvatarsSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TWeakObjectPtr<UCrowdyAvatars> WeakThis(this);
	Client->RunOp(AvatarsDomain, TEXT("MyAvatars"), MakeShared<FJsonObject>(),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [WeakThis, OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyAvatar> Avatars;
			FCrowdyAvatarError Error;
			if (!ReadArray(Result, TEXT("myAvatars"), Avatars, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}

			if (UCrowdyAvatars* Self = WeakThis.Get())
			{
				Self->CachedMyAvatars = Avatars;
				Self->bCachePopulated = true;
				Self->OnMyAvatarsCacheChanged.Broadcast(Self->CachedMyAvatars);
			}

			OnSuccess.ExecuteIfBound(Avatars);
		}));
}

void UCrowdyAvatars::GetAvatar(int64 AvatarId, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("id"), BigInt(AvatarId));

	Client->RunOp(AvatarsDomain, TEXT("AvatarById"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAvatar Avatar;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("avatar"), Avatar, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Avatar);
		}));
}

void UCrowdyAvatars::GetUserAvatars(int64 UserId, FOnAvatarsSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("userId"), BigInt(UserId));

	Client->RunOp(AvatarsDomain, TEXT("UserAvatars"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyAvatar> Avatars;
			FCrowdyAvatarError Error;
			if (!ReadArray(Result, TEXT("userAvatars"), Avatars, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Avatars);
		}));
}

void UCrowdyAvatars::GetAvatarAppState(int64 AvatarId, FOnAppStateSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	Client->RunOp(AvatarsDomain, TEXT("AvatarAppState"), BuildAppStateVariables(GetAppId(), AvatarId),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAppAvatarState AppState;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("avatarAppState"), AppState, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(AppState);
		}));
}

void UCrowdyAvatars::GetAvatarAppStates(const TArray<int64>& AvatarIds, FOnAppStatesSuccess OnSuccess,
                                        FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));

	TArray<TSharedPtr<FJsonValue>> Ids;
	for (int64 Id : AvatarIds)
	{
		Ids.Add(MakeShared<FJsonValueString>(BigInt(Id)));
	}
	Variables->SetArrayField(TEXT("avatarIds"), Ids);

	Client->RunOp(AvatarsDomain, TEXT("AvatarAppStates"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			TArray<FCrowdyAppAvatarState> AppStates;
			FCrowdyAvatarError Error;
			if (!ReadArray(Result, TEXT("avatarAppStates"), AppStates, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(AppStates);
		}));
}

void UCrowdyAvatars::CreateAvatar(const FString& Name, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), Name);

	Client->RunOp(AvatarsDomain, TEXT("CreateAvatar"), WrapInput(Input),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAvatar Avatar;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("createAvatar"), Avatar, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Avatar);
		}));
}

void UCrowdyAvatars::UpdateAvatar(int64 AvatarId, const FString& Name,
                                  FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), Name);

	TSharedPtr<FJsonObject> Variables = WrapInput(Input);
	Variables->SetStringField(TEXT("id"), BigInt(AvatarId));

	Client->RunOp(AvatarsDomain, TEXT("UpdateAvatar"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAvatar Avatar;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("updateAvatar"), Avatar, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Avatar);
		}));
}

void UCrowdyAvatars::DeleteAvatar(int64 AvatarId, FOnAvatarVoidSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("id"), BigInt(AvatarId));

	Client->RunOp(AvatarsDomain, TEXT("DeleteAvatar"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			// The mutation answers with the row it removed, and the caller is told nothing about it. Reading it
			// anyway is what distinguishes a deletion from a null answer, which is the only way this operation
			// reports "there was nothing there" without a GraphQL error.
			FCrowdyAvatar Removed;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("deleteAvatar"), Removed, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound();
		}));
}

void UCrowdyAvatars::UpdatePublicAvatarState(int64 AvatarId, const FString& PublicState,
                                             FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("publicState"), PublicState);
	DispatchAvatarStateUpdate(AvatarId, Input, OnSuccess, OnError);
}

void UCrowdyAvatars::UpdatePrivateAvatarState(int64 AvatarId, const FString& PrivateState,
                                              FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("privateState"), PrivateState);
	DispatchAvatarStateUpdate(AvatarId, Input, OnSuccess, OnError);
}

void UCrowdyAvatars::UpdateAvatarState(int64 AvatarId, const FString& PublicState,
                                       const FString& PrivateState,
                                       FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("publicState"), PublicState);
	Input->SetStringField(TEXT("privateState"), PrivateState);
	DispatchAvatarStateUpdate(AvatarId, Input, OnSuccess, OnError);
}

void UCrowdyAvatars::DispatchAvatarStateUpdate(int64 AvatarId, const TSharedPtr<FJsonObject>& Input,
                                               FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	// Which of the two state fields the input carries is the whole difference between the public-only, private-only
	// and both-at-once entry points: a field the input omits is left as it is on the server.
	TSharedPtr<FJsonObject> Variables = WrapInput(Input);
	Variables->SetStringField(TEXT("id"), BigInt(AvatarId));

	Client->RunOp(AvatarsDomain, TEXT("UpdateAvatarState"), Variables,
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAvatar Avatar;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("updateAvatarState"), Avatar, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(Avatar);
		}));
}

void UCrowdyAvatars::UpdateAvatarAppState(int64 AvatarId, const FString& State,
                                          FOnAppStateSuccess OnSuccess, FOnAvatarError OnError)
{
	FCrowdyCppClient* Client = ResolveApiClient(GetGameInstance(), AvatarsLogName);
	if (!Client)
	{
		const FCrowdyAvatarError Error = ClientUnavailableError<FCrowdyAvatarError>();
		OnError.ExecuteIfBound(Error, Error.Message);
		return;
	}

	Client->RunOp(AvatarsDomain, TEXT("UpdateAvatarAppState"),
		WrapInput(BuildAppStateVariables(GetAppId(), AvatarId, State)),
		GuardLifetime<FCrowdyCppJsonResult>(LiveSessionToken, [OnSuccess, OnError](FCrowdyCppJsonResult Result)
		{
			FCrowdyAppAvatarState AppState;
			FCrowdyAvatarError Error;
			if (!ReadObject(Result, TEXT("updateAvatarAppState"), AppState, Error))
			{
				OnError.ExecuteIfBound(Error, Error.Message);
				return;
			}
			OnSuccess.ExecuteIfBound(AppState);
		}));
}

TSharedPtr<FJsonObject> UCrowdyAvatars::BuildAppStateVariables(int64 AppId, int64 AvatarId,
                                                               const TOptional<FString>& State)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("appId"), BigInt(AppId));
	Object->SetStringField(TEXT("avatarId"), BigInt(AvatarId));
	if (State.IsSet())
	{
		Object->SetStringField(TEXT("state"), State.GetValue());
	}
	return Object;
}


namespace AvatarStateSerialization
{
	static void ToBytes(FProperty* Prop, void* Data, TArray<uint8>& OutBytes)
	{
		FMemoryWriter Writer(OutBytes, true);
		if (const FStructProperty* SP = CastField<FStructProperty>(Prop))
			SP->Struct->SerializeBin(Writer, Data);
		else if (CastField<FStrProperty>(Prop))
			Writer << *reinterpret_cast<FString*>(Data);
		else
			Writer.Serialize(Data, Prop->GetSize()); // bool, int32, int64, float, double, etc.
	}

	static FString ToBase64(FProperty* Prop, void* Data)
	{
		if (!Prop || !Data) return {};
		TArray<uint8> Bytes;
		ToBytes(Prop, Data, Bytes);
		return FBase64::Encode(Bytes);
	}

	static bool FromBytes(const TArray<uint8>& Bytes, FProperty* Prop, void* Data)
	{
		if (Bytes.Num() == 0 || !Prop || !Data) return false;
		FMemoryReader Reader(Bytes, true);
		// Avatar state is server-stored but client-written, so these bytes are wire input. Bounding the archive
		// stops a forged length prefix asking for an allocation far larger than the blob that declared it.
		Reader.ArMaxSerializeSize = Bytes.Num();
		if (const FStructProperty* SP = CastField<FStructProperty>(Prop))
			SP->Struct->SerializeBin(Reader, Data);
		else if (CastField<FStrProperty>(Prop))
			Reader << *reinterpret_cast<FString*>(Data);
		else
			Reader.Serialize(Data, Prop->GetSize());
		return true;
	}

	static bool FromBase64(const FString& Base64, FProperty* Prop, void* Data)
	{
		if (Base64.IsEmpty()) return false;
		TArray<uint8> Bytes;
		return FBase64::Decode(Base64, Bytes) && Bytes.Num() > 0 && FromBytes(Bytes, Prop, Data);
	}
}


DEFINE_FUNCTION(UCrowdyAvatars::execSerializeToAvatarState)
{
	Stack.StepCompiledIn<FProperty>(nullptr);
	void* Data = Stack.MostRecentPropertyAddress;
	FProperty* Prop = CastField<FProperty>(Stack.MostRecentProperty);

	P_FINISH;
	P_NATIVE_BEGIN;
		*static_cast<FString*>(RESULT_PARAM) = AvatarStateSerialization::ToBase64(Prop, Data);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UCrowdyAvatars::execDeserializeFromAvatarState)
{
	P_GET_PROPERTY_REF(FStrProperty, State);
	Stack.StepCompiledIn<FProperty>(nullptr);
	void* Data = Stack.MostRecentPropertyAddress;
	FProperty* Prop = CastField<FProperty>(Stack.MostRecentProperty);

	P_FINISH;
	P_NATIVE_BEGIN;
		*static_cast<bool*>(RESULT_PARAM) = AvatarStateSerialization::FromBase64(State, Prop, Data);
	P_NATIVE_END;
}

namespace
{
	struct FAppStateLatentResult
	{
		bool bCompleted = false;
		bool bSuccess = false;
		FString RawState;
		FCrowdyAvatarError Error;
	};

	class FGetAppStateLatentAction final : public FPendingLatentAction
	{
	public:
		FName ExecutionFunction;
		int32 OutputLink;
		FWeakObjectPtr CallbackTarget;
		TSharedRef<FAppStateLatentResult> Result;
		void* OutStateData;
		FProperty* OutStateProp;
		bool* OutbSuccess;
		FCrowdyAvatarError* OutError;

		FGetAppStateLatentAction(const FLatentActionInfo& Info,
		                         TSharedRef<FAppStateLatentResult> InResult,
		                         void* InStateData, FProperty* InStateProp,
		                         bool* InbSuccess, FCrowdyAvatarError* InError)
			: ExecutionFunction(Info.ExecutionFunction), OutputLink(Info.Linkage)
			  , CallbackTarget(Info.CallbackTarget), Result(InResult)
			  , OutStateData(InStateData), OutStateProp(InStateProp)
			  , OutbSuccess(InbSuccess), OutError(InError)
		{
		}

		virtual void UpdateOperation(FLatentResponse& Response) override
		{
			if (!Result->bCompleted) return;
			if (OutbSuccess) *OutbSuccess = Result->bSuccess;
			if (Result->bSuccess && OutStateProp && OutStateData)
				AvatarStateSerialization::FromBase64(Result->RawState, OutStateProp, OutStateData);
			else if (!Result->bSuccess && OutError)
				*OutError = Result->Error;
			Response.FinishAndTriggerIf(true, ExecutionFunction, OutputLink, CallbackTarget);
		}
	};

	class FSetAppStateLatentAction final : public FPendingLatentAction
	{
	public:
		FName ExecutionFunction;
		int32 OutputLink;
		FWeakObjectPtr CallbackTarget;
		TSharedRef<FAppStateLatentResult> Result;
		bool* OutbSuccess;
		FCrowdyAvatarError* OutError;

		FSetAppStateLatentAction(const FLatentActionInfo& Info,
		                         TSharedRef<FAppStateLatentResult> InResult,
		                         bool* InbSuccess, FCrowdyAvatarError* InError)
			: ExecutionFunction(Info.ExecutionFunction), OutputLink(Info.Linkage)
			  , CallbackTarget(Info.CallbackTarget), Result(InResult)
			  , OutbSuccess(InbSuccess), OutError(InError)
		{
		}

		virtual void UpdateOperation(FLatentResponse& Response) override
		{
			if (!Result->bCompleted) return;
			if (OutbSuccess) *OutbSuccess = Result->bSuccess;
			if (!Result->bSuccess && OutError) *OutError = Result->Error;
			Response.FinishAndTriggerIf(true, ExecutionFunction, OutputLink, CallbackTarget);
		}
	};

	/** Report a failure into a latent result the action is already waiting on, so the node still finishes. */
	void FailLatentResult(const TSharedRef<FAppStateLatentResult>& Result, const FCrowdyAvatarError& Error)
	{
		Result->bSuccess = false;
		Result->Error = Error;
		Result->bCompleted = true;
	}

	/**
	 * The world a latent node registers its action in. The node's own world context is preferred over the
	 * subsystem's, which is null outside a running game.
	 */
	UWorld* ResolveLatentWorld(UObject* WorldContextObject, const UCrowdyAvatars* Subsystem)
	{
		if (UWorld* FromContext = GEngine
			? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
			: nullptr)
		{
			return FromContext;
		}
		return Subsystem ? Subsystem->GetWorld() : nullptr;
	}
}

DEFINE_FUNCTION(UCrowdyAvatars::execGetAvatarAppStateAs)
{
	P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
	P_GET_STRUCT(FLatentActionInfo, Z_Param_LatentInfo);
	P_GET_PROPERTY(FInt64Property, Z_Param_AvatarId);
	Stack.StepCompiledIn<FProperty>(nullptr);
	void* StateData = Stack.MostRecentPropertyAddress;
	FProperty* StateProp = CastField<FProperty>(Stack.MostRecentProperty);
	Stack.StepCompiledIn<FBoolProperty>(nullptr);
	bool* bSuccessAddr = reinterpret_cast<bool*>(Stack.MostRecentPropertyAddress);
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	FCrowdyAvatarError* ErrorAddr = reinterpret_cast<FCrowdyAvatarError*>(Stack.MostRecentPropertyAddress);
	P_FINISH;
	P_NATIVE_BEGIN;
		UWorld* World = ResolveLatentWorld(Z_Param_WorldContextObject, P_THIS);
		if (!World)
		{
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[%s] Get Avatar App State As has no world to run in; the node cannot continue."), AvatarsLogName);
			return;
		}

		FLatentActionManager& LatentActions = World->GetLatentActionManager();
		if (LatentActions.FindExistingAction<FGetAppStateLatentAction>(
			Z_Param_LatentInfo.CallbackTarget, Z_Param_LatentInfo.UUID))
		{
			// Already in flight for this node. A second registration would append rather than replace, and both
			// actions write the same output addresses, so the node would continue twice off one invocation.
			return;
		}

		// The action is registered before anything can fail, so every path from here reaches the node's output pin
		// rather than leaving it waiting on a request that was never issued.
		TSharedRef<FAppStateLatentResult> Result = MakeShared<FAppStateLatentResult>();
		LatentActions.AddNewAction(
			Z_Param_LatentInfo.CallbackTarget, Z_Param_LatentInfo.UUID,
			new FGetAppStateLatentAction(Z_Param_LatentInfo, Result, StateData, StateProp, bSuccessAddr, ErrorAddr));

		FCrowdyCppClient* Client = ResolveApiClient(P_THIS->GetGameInstance(), AvatarsLogName);
		if (!Client)
		{
			FailLatentResult(Result, ClientUnavailableError<FCrowdyAvatarError>());
			return;
		}

		Client->RunOp(AvatarsDomain, TEXT("AvatarAppState"),
			UCrowdyAvatars::BuildAppStateVariables(P_THIS->GetAppId(), Z_Param_AvatarId),
			[Result](FCrowdyCppJsonResult Response)
			{
				FCrowdyAppAvatarState AppState;
				FCrowdyAvatarError Error;
				if (!ReadObject(Response, TEXT("avatarAppState"), AppState, Error))
				{
					FailLatentResult(Result, Error);
					return;
				}
				Result->RawState = AppState.RawState;
				Result->bSuccess = true;
				Result->bCompleted = true;
			});
	P_NATIVE_END;
}

DEFINE_FUNCTION(UCrowdyAvatars::execSetAvatarAppStateAs)
{
	P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
	P_GET_STRUCT(FLatentActionInfo, Z_Param_LatentInfo);
	P_GET_PROPERTY(FInt64Property, Z_Param_AvatarId);
		Stack.StepCompiledIn<FProperty>(nullptr);
		void* StateData = Stack.MostRecentPropertyAddress;
		FProperty* StateProp = CastField<FProperty>(Stack.MostRecentProperty);
		Stack.StepCompiledIn<FBoolProperty>(nullptr);
		bool* bSuccessAddr = reinterpret_cast<bool*>(Stack.MostRecentPropertyAddress);
		Stack.StepCompiledIn<FStructProperty>(nullptr);
		FCrowdyAvatarError* ErrorAddr = reinterpret_cast<FCrowdyAvatarError*>(Stack.MostRecentPropertyAddress);
	P_FINISH;

	P_NATIVE_BEGIN;
		UWorld* World = ResolveLatentWorld(Z_Param_WorldContextObject, P_THIS);
		if (!World)
		{
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[%s] Set Avatar App State As has no world to run in; the node cannot continue."), AvatarsLogName);
			return;
		}

		FLatentActionManager& LatentActions = World->GetLatentActionManager();
		if (LatentActions.FindExistingAction<FSetAppStateLatentAction>(
			Z_Param_LatentInfo.CallbackTarget, Z_Param_LatentInfo.UUID))
		{
			return; // Already in flight for this node; see the matching guard on the read.
		}

		const FString SerializedState = AvatarStateSerialization::ToBase64(StateProp, StateData);

		TSharedRef<FAppStateLatentResult> Result = MakeShared<FAppStateLatentResult>();
		LatentActions.AddNewAction(
			Z_Param_LatentInfo.CallbackTarget, Z_Param_LatentInfo.UUID,
			new FSetAppStateLatentAction(Z_Param_LatentInfo, Result, bSuccessAddr, ErrorAddr));

		FCrowdyCppClient* Client = ResolveApiClient(P_THIS->GetGameInstance(), AvatarsLogName);
		if (!Client)
		{
			FailLatentResult(Result, ClientUnavailableError<FCrowdyAvatarError>());
			return;
		}

		Client->RunOp(AvatarsDomain, TEXT("UpdateAvatarAppState"),
			WrapInput(UCrowdyAvatars::BuildAppStateVariables(P_THIS->GetAppId(), Z_Param_AvatarId, SerializedState)),
			[Result](FCrowdyCppJsonResult Response)
			{
				FCrowdyAppAvatarState AppState;
				FCrowdyAvatarError Error;
				if (!ReadObject(Response, TEXT("updateAvatarAppState"), AppState, Error))
				{
					FailLatentResult(Result, Error);
					return;
				}
				Result->bSuccess = true;
				Result->bCompleted = true;
			});
	P_NATIVE_END;
}
