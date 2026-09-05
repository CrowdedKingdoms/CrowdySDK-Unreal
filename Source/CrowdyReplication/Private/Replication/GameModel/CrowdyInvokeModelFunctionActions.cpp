// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyInvokeModelFunctionActions.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Parse a JSON-encoded value literal ("5", "12.5", "true", "\"text\"") into an FJsonValue. The JSON reader
	// rejects a bare top-level scalar, so wrap it (mirrors CrowdyEffects::BuildInvokeParams). Null on malformed input.
	// The nesting pre-scan bounds the depth before Deserialize so a pathological literal (a designer may thread an
	// untrusted runtime string through a param value) cannot build a DOM that overflows the stack on teardown; an
	// over-deep literal returns null and is sent as a plain string instead.
	TSharedPtr<FJsonValue> ParseParamLiteral(const FString& Literal)
	{
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *Literal);
		if (!CrowdyJsonSafety::IsNestingWithinLimit(Wrapped))
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
		TSharedPtr<FJsonObject> Object;
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			return Object->TryGetField(TEXT("v"));
		}
		return nullptr;
	}
}

TSharedPtr<FJsonObject> UCrowdyInvokeModelFunctionAction::BuildParamsJson(const TMap<FName, FString>& InParams)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	for (const TPair<FName, FString>& Pair : InParams)
	{
		if (Pair.Key.IsNone())
		{
			continue;
		}
		TSharedPtr<FJsonValue> Value = ParseParamLiteral(Pair.Value);
		if (!Value.IsValid())
		{
			// Not valid JSON: send the raw text as a string so a bare word or id round-trips without quotes.
			Value = MakeShared<FJsonValueString>(Pair.Value);
		}
		Out->SetField(Pair.Key.ToString(), Value);
	}
	return Out;
}

UCrowdyInvokeModelFunctionAction* UCrowdyInvokeModelFunctionAction::CallModelFunction(UObject* WorldContext,
	UObject* InTarget, const FString& InContainerId, const FString& InFunctionName, const TMap<FName, FString>& InParams,
	const FString& InSessionId)
{
	UCrowdyInvokeModelFunctionAction* Action = NewObject<UCrowdyInvokeModelFunctionAction>();
	Action->WorldContextObject = WorldContext;
	Action->Target = InTarget;
	Action->ContainerId = InContainerId;
	Action->FunctionName = InFunctionName;
	Action->Params = InParams;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyInvokeModelFunctionAction::Activate()
{
	// Resolve the world from the Target participant when set, else the explicit WorldContext.
	const UObject* Ctx = Target.IsValid() ? Target.Get() : WorldContextObject.Get();
	const UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
	UCrowdyGameModelSubsystem* Model = World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
	if (!Model)
	{
		Failed.Broadcast(false, FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (FunctionName.IsEmpty())
	{
		Failed.Broadcast(false, FString(), TEXT("no function name"));
		SetReadyToDestroy();
		return;
	}

	const TSharedPtr<FJsonObject> ParamsJson = BuildParamsJson(Params);

	TWeakObjectPtr<UCrowdyInvokeModelFunctionAction> WeakThis(this);
	TFunction<void(FCrowdyInvokeResult)> OnDone = [WeakThis](FCrowdyInvokeResult Result)
	{
		UCrowdyInvokeModelFunctionAction* Action = WeakThis.Get();
		if (!Action)
		{
			return;
		}
		if (Result.bTransportOk && Result.bSuccess)
		{
			Action->Succeeded.Broadcast(Result.bSuccess, Result.ReturnValueJson, Result.ErrorMessage);
		}
		else
		{
			Action->Failed.Broadcast(Result.bSuccess, Result.ReturnValueJson, Result.ErrorMessage);
		}
		Action->SetReadyToDestroy();
	};

	// Entity-bound: the Target rides as SelfNetID (resolved to its container server-side), never a param.
	if (Target.IsValid())
	{
		FGuid NetID;
		if (!Model->ResolveTargetNetID(Target.Get(), NetID))
		{
			Failed.Broadcast(false, FString(), TEXT("Target is not a registered Game Model entity"));
			SetReadyToDestroy();
			return;
		}
		Model->InvokeAndApply(NetID, FunctionName, ParamsJson, SessionId, MoveTemp(OnDone));
		return;
	}

	// Free/data model addressed by id (no actor).
	if (ContainerId.IsEmpty())
	{
		Failed.Broadcast(false, FString(), TEXT("no Target and no model id"));
		SetReadyToDestroy();
		return;
	}
	Model->InvokeOnContainer(ContainerId, FunctionName, ParamsJson, SessionId, MoveTemp(OnDone));
}
