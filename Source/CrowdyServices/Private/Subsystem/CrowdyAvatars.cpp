#include "Subsystem/CrowdyAvatars.h"
#include "LatentActions.h"
#include "Engine/LatentActionManager.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Async/Async.h"
#include "Misc/Base64.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "UObject/UnrealType.h"
#include "Queries/Data/Avatar/FAvatarCreateResponse.h"
#include "Queries/Data/Avatar/FAvatarNameUpdateResponse.h"
#include "Queries/Data/Avatar/FAvatarStateUpdateResponse.h"
#include "Queries/Data/Avatar/FAvatarDeleteResponse.h"
#include "Queries/Data/Avatar/Responses/FMyAvatarsResponse.h"
#include "Queries/Data/Avatar/Responses/FAvatarResponse.h"
#include "Queries/Data/Avatar/Responses/FUserAvatarsResponse.h"
#include "Queries/Data/Avatar/Responses/FAvatarAppStateResponse.h"
#include "Queries/Data/Avatar/Responses/FAvatarAppStatesResponse.h"
#include "Queries/Data/Avatar/Responses/FUpdateAvatarAppStateResponse.h"


namespace AvatarQueries
{
	static const TCHAR* MyAvatars =
		TEXT("query MyAvatars { myAvatars { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* GetAvatar =
		TEXT(
			"query GetAvatar($id: BigInt!) { avatar(id: $id) { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* GetUserAvatars =
		TEXT(
			"query GetUserAvatars($userId: BigInt!) { userAvatars(userId: $userId) { avatarId userId name publicState createdAt } }");

	static const TCHAR* GetAvatarAppState =
		TEXT(
			"query GetAvatarAppState($appId: BigInt!, $avatarId: BigInt!) { avatarAppState(appId: $appId, avatarId: $avatarId) { appId avatarId state createdAt updatedAt } }");

	static const TCHAR* GetAvatarAppStates =
		TEXT(
			"query GetAvatarAppStates($appId: BigInt!, $avatarIds: [BigInt!]!) { avatarAppStates(appId: $appId, avatarIds: $avatarIds) { appId avatarId state createdAt updatedAt } }");

	static const TCHAR* CreateAvatar =
		TEXT(
			"mutation CreateAvatar($name: String!) { createAvatar(input: { name: $name }) { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* UpdateAvatar =
		TEXT(
			"mutation UpdateAvatar($id: BigInt!, $name: String!) { updateAvatar(id: $id, input: { name: $name }) { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* DeleteAvatar =
		TEXT("mutation DeleteAvatar($id: BigInt!) { deleteAvatar(id: $id) { avatarId userId name createdAt } }");

	static const TCHAR* UpdatePublicAvatarState =
		TEXT(
			"mutation UpdatePublicAvatarState($id: BigInt!, $publicState: String) { updateAvatarState(id: $id, input: { publicState: $publicState }) { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* UpdatePrivateAvatarState =
		TEXT(
			"mutation UpdatePrivateAvatarState($id: BigInt!, $privateState: String) { updateAvatarState(id: $id, input: { privateState: $privateState }) { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* UpdateAvatarState =
		TEXT(
			"mutation UpdateAvatarState($id: BigInt!, $publicState: String, $privateState: String) { updateAvatarState(id: $id, input: { publicState: $publicState, privateState: $privateState }) { avatarId userId name publicState privateState createdAt } }");

	static const TCHAR* UpdateAvatarAppState =
		TEXT(
			"mutation UpdateAvatarAppState($appId: BigInt!, $avatarId: BigInt!, $state: String) { updateAvatarAppState(input: { appId: $appId, avatarId: $avatarId, state: $state }) { appId avatarId state createdAt updatedAt } }");
}


void UCrowdyAvatars::InjectDependencies(FCrowdyDataRegistry* InDataRegistry, UCrowdyQuerySubsystem* InQuerySubsystem)
{
	if (InDataRegistry) InDataRegistry->RegisterLayer(this);
	QuerySubsystem = InQuerySubsystem;
}

void UCrowdyAvatars::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyAvatars::Deinitialize()
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.Empty();
	Super::Deinitialize();
}


TArray<EQueryResponseType> UCrowdyAvatars::GetSupportedResponseType() const
{
	return {
		EQueryResponseType::CreateAvatar,
		EQueryResponseType::MyAvatars,
		EQueryResponseType::UpdateAvatar,
		EQueryResponseType::UpdateAvatarState,
		EQueryResponseType::DeleteAvatar,
		EQueryResponseType::GetAvatar,
		EQueryResponseType::GetUserAvatars,
		EQueryResponseType::GetAvatarAppState,
		EQueryResponseType::GetAvatarAppStates,
		EQueryResponseType::UpdateAvatarAppState,
	};
}

void UCrowdyAvatars::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	if (!Response.IsValid()) return;
	FireCallback(Response);
}


int64 UCrowdyAvatars::GetAppId() const
{
	return GetDefault<UCrowdySDKDeveloperSettings>()->AppID;
}

void UCrowdyAvatars::PushCallback(EQueryResponseType Type,
                                  TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback)
{
	FScopeLock Lock(&CallbackMutex);
	PendingCallbacks.FindOrAdd(Type).Add(MoveTemp(Callback));
}

void UCrowdyAvatars::FireCallback(TSharedPtr<ICrowdyQueryResponse> Response)
{
	TFunction<void(TSharedPtr<ICrowdyQueryResponse>)> Callback;
	{
		FScopeLock Lock(&CallbackMutex);
		TArray<TFunction<void(TSharedPtr<ICrowdyQueryResponse>)>>* Queue =
			PendingCallbacks.Find(Response->GetResponseType());
		if (Queue && Queue->Num() > 0)
		{
			Callback = MoveTemp((*Queue)[0]);
			Queue->RemoveAt(0, 1, EAllowShrinking::No);
		}
	}

	if (Callback)
	{
		TSharedPtr<ICrowdyQueryResponse> ResponseCopy = Response;
		AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), ResponseCopy]()
		{
			Callback(ResponseCopy);
		});
	}
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
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::MyAvatars, [this, OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
		{
			const FMyAvatarsResponse& R = static_cast<FMyAvatarsResponse&>(*Resp);
			CachedMyAvatars = R.Avatars;
			bCachePopulated = true;
			OnMyAvatarsCacheChanged.Broadcast(CachedMyAvatars);
			OnSuccess.ExecuteIfBound(R.Avatars);
		}
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::MyAvatars, AvatarQueries::MyAvatars, Vars);
}

void UCrowdyAvatars::GetAvatar(int64 AvatarId, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::GetAvatar, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarResponse&>(*Resp).Avatar);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("id"), FString::Printf(TEXT("%lld"), AvatarId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::GetAvatar, AvatarQueries::GetAvatar, Vars);
}

void UCrowdyAvatars::GetUserAvatars(int64 UserId, FOnAvatarsSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::GetUserAvatars, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FUserAvatarsResponse&>(*Resp).Avatars);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("userId"), FString::Printf(TEXT("%lld"), UserId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::GetUserAvatars, AvatarQueries::GetUserAvatars, Vars);
}

void UCrowdyAvatars::GetAvatarAppState(int64 AvatarId, FOnAppStateSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::GetAvatarAppState, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarAppStateResponse&>(*Resp).AppState);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	Vars->SetStringField(TEXT("avatarId"), FString::Printf(TEXT("%lld"), AvatarId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::GetAvatarAppState, AvatarQueries::GetAvatarAppState,
	                                                Vars);
}

void UCrowdyAvatars::GetAvatarAppStates(const TArray<int64>& AvatarIds, FOnAppStatesSuccess OnSuccess,
                                        FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::GetAvatarAppStates, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarAppStatesResponse&>(*Resp).AppStates);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));

	TArray<TSharedPtr<FJsonValue>> IdsJson;
	for (int64 Id : AvatarIds)
		IdsJson.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Id)));
	Vars->SetArrayField(TEXT("avatarIds"), IdsJson);

	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::GetAvatarAppStates,
	                                                AvatarQueries::GetAvatarAppStates, Vars);
}


void UCrowdyAvatars::CreateAvatar(const FString& Name, FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::CreateAvatar, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarCreateResponse&>(*Resp).Avatar);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("name"), Name);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::CreateAvatar, AvatarQueries::CreateAvatar, Vars);
}

void UCrowdyAvatars::UpdateAvatar(int64 AvatarId, const FString& Name,
                                  FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateAvatar, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarNameUpdateResponse&>(*Resp).Avatar);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("id"), FString::Printf(TEXT("%lld"), AvatarId));
	Vars->SetStringField(TEXT("name"), Name);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateAvatarName, AvatarQueries::UpdateAvatar, Vars);
}

void UCrowdyAvatars::DeleteAvatar(int64 AvatarId, FOnAvatarVoidSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::DeleteAvatar, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound();
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("id"), FString::Printf(TEXT("%lld"), AvatarId));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::DeleteAvatar, AvatarQueries::DeleteAvatar, Vars);
}

void UCrowdyAvatars::UpdatePublicAvatarState(int64 AvatarId, const FString& PublicState,
                                             FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateAvatarState, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarStateUpdateResponse&>(*Resp).Avatar);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("id"), FString::Printf(TEXT("%lld"), AvatarId));
	Vars->SetStringField(TEXT("publicState"), PublicState);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateAvatarState,
	                                                AvatarQueries::UpdatePublicAvatarState, Vars);
}

void UCrowdyAvatars::UpdatePrivateAvatarState(int64 AvatarId, const FString& PrivateState,
                                              FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateAvatarState, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarStateUpdateResponse&>(*Resp).Avatar);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("id"), FString::Printf(TEXT("%lld"), AvatarId));
	Vars->SetStringField(TEXT("privateState"), PrivateState);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateAvatarState,
	                                                AvatarQueries::UpdatePrivateAvatarState, Vars);
}

void UCrowdyAvatars::UpdateAvatarState(int64 AvatarId, const FString& PublicState,
                                       const FString& PrivateState,
                                       FOnAvatarSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateAvatarState, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FAvatarStateUpdateResponse&>(*Resp).Avatar);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("id"), FString::Printf(TEXT("%lld"), AvatarId));
	Vars->SetStringField(TEXT("publicState"), PublicState);
	Vars->SetStringField(TEXT("privateState"), PrivateState);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateAvatarState, AvatarQueries::UpdateAvatarState,
	                                                Vars);
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
		if (!P_THIS->QuerySubsystem) return;
		UWorld* World = P_THIS->GetWorld();
		if (!World) return;
		auto Result = MakeShared<FAppStateLatentResult>();
		World->GetLatentActionManager().AddNewAction(
			Z_Param_LatentInfo.CallbackTarget, Z_Param_LatentInfo.UUID,
			new FGetAppStateLatentAction(Z_Param_LatentInfo, Result, StateData, StateProp, bSuccessAddr, ErrorAddr));
		P_THIS->PushCallback(EQueryResponseType::GetAvatarAppState,
		                     [Result](TSharedPtr<ICrowdyQueryResponse> Resp)
		                     {
			                     AsyncTask(ENamedThreads::GameThread, [Result, Resp]()
			                     {
				                     if (Resp->IsValid())
				                     {
					                     Result->RawState = static_cast<FAvatarAppStateResponse&>(*Resp).AppState.
						                     RawState;
					                     Result->bSuccess = true;
				                     }
				                     else
				                     {
					                     Result->Error = FCrowdyAvatarError::FromMessage(Resp->GetError());
					                     Result->bSuccess = false;
				                     }
				                     Result->bCompleted = true;
			                     });
		                     });
		TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
		Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), P_THIS->GetAppId()));
		Vars->SetStringField(TEXT("avatarId"), FString::Printf(TEXT("%lld"), Z_Param_AvatarId));
		P_THIS->QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::GetAvatarAppState,
		                                                        AvatarQueries::GetAvatarAppState, Vars);
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
		if (!P_THIS->QuerySubsystem) return;
		UWorld* World = P_THIS->GetWorld();
		if (!World) return;
		const FString SerializedState = AvatarStateSerialization::ToBase64(StateProp, StateData);
		auto Result = MakeShared<FAppStateLatentResult>();
		World->GetLatentActionManager().AddNewAction(
			Z_Param_LatentInfo.CallbackTarget, Z_Param_LatentInfo.UUID,
			new FSetAppStateLatentAction(Z_Param_LatentInfo, Result, bSuccessAddr, ErrorAddr));
		P_THIS->PushCallback(EQueryResponseType::UpdateAvatarAppState,
		                     [Result](TSharedPtr<ICrowdyQueryResponse> Resp)
		                     {
			                     AsyncTask(ENamedThreads::GameThread, [Result, Resp]()
			                     {
				                     Result->bSuccess = Resp->IsValid();
				                     Result->Error = Resp->IsValid()
					                                     ? FCrowdyAvatarError{}
					                                     : FCrowdyAvatarError::FromMessage(Resp->GetError());
				                     Result->bCompleted = true;
			                     });
		                     });
		TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
		Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), P_THIS->GetAppId()));
		Vars->SetStringField(TEXT("avatarId"), FString::Printf(TEXT("%lld"), Z_Param_AvatarId));
		Vars->SetStringField(TEXT("state"), SerializedState);
		P_THIS->QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateAvatarAppState,
		                                                        AvatarQueries::UpdateAvatarAppState, Vars);
	P_NATIVE_END;
}


void UCrowdyAvatars::UpdateAvatarAppState(int64 AvatarId, const FString& State,
                                          FOnAppStateSuccess OnSuccess, FOnAvatarError OnError)
{
	if (!QuerySubsystem) return;

	PushCallback(EQueryResponseType::UpdateAvatarAppState, [OnSuccess, OnError](TSharedPtr<ICrowdyQueryResponse> Resp)
	{
		if (Resp->IsValid())
			OnSuccess.ExecuteIfBound(static_cast<FUpdateAvatarAppStateResponse&>(*Resp).AppState);
		else
		{
			const FCrowdyAvatarError Err = FCrowdyAvatarError::FromMessage(Resp->GetError());
			OnError.ExecuteIfBound(Err, Err.Message);
		}
	});

	TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GetAppId()));
	Vars->SetStringField(TEXT("avatarId"), FString::Printf(TEXT("%lld"), AvatarId));
	Vars->SetStringField(TEXT("state"), State);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::UpdateAvatarAppState,
	                                                AvatarQueries::UpdateAvatarAppState, Vars);
}
