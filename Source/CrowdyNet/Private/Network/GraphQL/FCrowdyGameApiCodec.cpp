// Fill out your copyright notice in the Description page of Project Settings.

#include "Network/GraphQL/FCrowdyGameApiCodec.h"

#include "CrowdyNetLog.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Dom/JsonValue.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Compact (no whitespace) JSON for paramsJson; "{}" when there are no params.
	FString SerializeParamsCompact(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return TEXT("{}");
		}
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Params.ToSharedRef(), Writer);
		return Out;
	}

	// Pulls the inner "data" object out of a GraphQL envelope ({ data, errors }). Null when absent.
	TSharedPtr<FJsonObject> GetDataObject(const TSharedPtr<FJsonObject>& Envelope)
	{
		if (!Envelope.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* DataPtr = nullptr;
		if (!Envelope->TryGetObjectField(TEXT("data"), DataPtr) || !DataPtr)
		{
			return nullptr;
		}
		return *DataPtr;
	}

	TSharedPtr<FJsonObject> ParseJsonObjectString(const FString& Json)
	{
		if (!CrowdyJsonSafety::IsNestingWithinLimit(Json))
		{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("[CrowdyGameApi] rejected a container JSON payload exceeding the max nesting depth (%d)."),
				CrowdyJsonSafety::MaxNestingDepth);
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		TSharedPtr<FJsonObject> Parsed;
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			return Parsed;
		}
		return nullptr;
	}

	// A BigInt response field may arrive as a JSON string or a JSON number depending on the server, so read
	// either shape. Returns false when the field is absent or JSON null, letting callers distinguish "no value"
	// (e.g. no current turn) from a real id. The wire contract sends BigInts as strings, so the number branch
	// is purely defensive.
	bool ReadOptionalBigInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, int64& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		FString AsString;
		if (Obj->TryGetStringField(Field, AsString))
		{
			LexFromString(OutValue, *AsString);
			return true;
		}
		int64 AsNumber = 0;
		if (Obj->TryGetNumberField(Field, AsNumber))
		{
			OutValue = AsNumber;
			return true;
		}
		return false;
	}

	// Reads an Int response field. The wire sends a JSON number; a numeric string also reads (TryGetNumberField
	// parses one). Returns false when the field is absent or JSON null.
	bool ReadOptionalInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, int32& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Field, OutValue);
	}

	// Wraps a mutation's input object as { input: ... }, the shape every session mutation takes.
	TSharedPtr<FJsonObject> WrapSessionInput(const TSharedPtr<FJsonObject>& Input)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), Input);
		return Variables;
	}

	// expectedHostTerm is a nullable Int (a JSON number); omitted when the caller holds no term to assert.
	void WriteExpectedHostTerm(const TSharedPtr<FJsonObject>& Input, int32 ExpectedHostTerm)
	{
		if (ExpectedHostTerm > 0)
		{
			Input->SetNumberField(TEXT("expectedHostTerm"), ExpectedHostTerm);
		}
	}

	void WriteIdempotencyKey(const TSharedPtr<FJsonObject>& Input, const FString& IdempotencyKey)
	{
		if (!IdempotencyKey.IsEmpty())
		{
			Input->SetStringField(TEXT("idempotencyKey"), IdempotencyKey);
		}
	}

	// The object under data.<FieldName>, or null on a transport failure, a GraphQL error, or a null/absent field.
	TSharedPtr<FJsonObject> ReadDataObjectField(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, const TCHAR* FieldName)
	{
		if (!bHttpOk || TransportErrors.Num() > 0)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* FieldPtr = nullptr;
		if (!Data->TryGetObjectField(FieldName, FieldPtr) || !FieldPtr)
		{
			return nullptr;
		}
		return *FieldPtr;
	}
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildInvokeVariables(const FCrowdyInvokeRequest& Req)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	// appId is a BigInt! scalar it MUST be a JSON string, never a number, or the server rejects it.
	Input->SetStringField(TEXT("appId"), LexToString(Req.AppId));
	Input->SetStringField(TEXT("functionName"), Req.FunctionName);
	Input->SetStringField(TEXT("selfContainerId"), Req.SelfContainerId);
	// sessionId is nullable; omit it entirely for app-global scope.
	if (!Req.SessionId.IsEmpty())
	{
		Input->SetStringField(TEXT("sessionId"), Req.SessionId);
	}
	Input->SetStringField(TEXT("paramsJson"), SerializeParamsCompact(Req.Params));

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

ECrowdyPlayerFaultBlame CrowdyPlayerFaultBlameFromWireString(const FString& Wire)
{
	if (Wire.Equals(TEXT("PLATFORM"), ESearchCase::IgnoreCase)) return ECrowdyPlayerFaultBlame::Platform;
	if (Wire.Equals(TEXT("AUTHOR"), ESearchCase::IgnoreCase))   return ECrowdyPlayerFaultBlame::Author;
	if (Wire.Equals(TEXT("BUDGET"), ESearchCase::IgnoreCase))   return ECrowdyPlayerFaultBlame::Budget;
	return ECrowdyPlayerFaultBlame::Unknown;
}

const TCHAR* CrowdyPlayerFaultBlameToWord(ECrowdyPlayerFaultBlame Blame)
{
	switch (Blame)
	{
	case ECrowdyPlayerFaultBlame::Platform: return TEXT("PLATFORM");
	case ECrowdyPlayerFaultBlame::Author:   return TEXT("AUTHOR");
	case ECrowdyPlayerFaultBlame::Budget:   return TEXT("BUDGET");
	default:                                return TEXT("unknown");
	}
}

FCrowdyInvokeResult FCrowdyGameApiCodec::ParseInvokeEnvelope(const TSharedPtr<FJsonObject>& Envelope,
	bool bHttpOk, const TArray<FString>& TransportErrors)
{
	FCrowdyInvokeResult Result;

	if (!bHttpOk)
	{
		Result.ErrorMessage = TransportErrors.Num() > 0 ? TransportErrors[0] : TEXT("transport: request failed");
		return Result; // bTransportOk stays false
	}
	if (TransportErrors.Num() > 0)
	{
		// Reached the server, but the GraphQL layer rejected the operation (malformed query, a server
		// exception, or an authorization error surfaced as a GraphQL error). Kept distinct from a
		// rolled-back invoke: bTransportOk stays false and the message is surfaced.
		//
		// This is also the thrown channel, which is where the platform's overload refusal and its per-player rate
		// limit both arrive, carrying blame, retryable and retryAfterMs in errors[].extensions. TransportErrors is a
		// flat array of message strings with no extensions beside them, so none of that can be populated here.
		//
		// That is a limitation of this parser and no longer a gap in the SDK: the shipping path reaches the server
		// through CrowdyCPP, whose ReadOutcomeFault reads all three, and this function has had no non-test caller
		// since. Widening TransportErrors would be the fix if it ever regains one; until then it is dead weight, and
		// the honest reading of a result from here is that the attribution fields were never populated at all.
		Result.ErrorMessage = TransportErrors[0];
		return Result;
	}

	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		Result.ErrorMessage = TEXT("malformed response: missing data");
		return Result;
	}

	const TSharedPtr<FJsonObject>* InvokePtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelInvoke"), InvokePtr) || !InvokePtr)
	{
		Result.ErrorMessage = TEXT("malformed response: missing gameModelInvoke");
		return Result;
	}
	const TSharedPtr<FJsonObject>& Invoke = *InvokePtr;

	// The server returned a real GmInvokeResult, so this is transport-OK regardless of the logic outcome.
	Result.bTransportOk = true;

	bool bInvokeSuccess = false;
	Invoke->TryGetBoolField(TEXT("success"), bInvokeSuccess);
	Result.bSuccess = bInvokeSuccess;

	Invoke->TryGetStringField(TEXT("returnValueJson"), Result.ReturnValueJson);
	// errorMessage is DEPRECATED and slated for removal, but it is not empty: the server authors a player-safe
	// sentence matching fault, carrying no engine detail. Worth reading while it exists, since it is the one piece of
	// text a game can show without writing its own wording. fault is what a caller BRANCHES on.
	Invoke->TryGetStringField(TEXT("errorMessage"), Result.ErrorMessage);

	const TSharedPtr<FJsonObject>* FaultPtr = nullptr;
	if (Invoke->TryGetObjectField(TEXT("fault"), FaultPtr) && FaultPtr && FaultPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& Fault = *FaultPtr;
		Fault->TryGetStringField(TEXT("code"), Result.FaultCode);
		FString BlameString;
		Fault->TryGetStringField(TEXT("blame"), BlameString);
		Result.Blame = CrowdyPlayerFaultBlameFromWireString(BlameString);
		Fault->TryGetBoolField(TEXT("retryable"), Result.bRetryable);
	}

	const TArray<TSharedPtr<FJsonValue>>* Mutations = nullptr;
	if (Invoke->TryGetArrayField(TEXT("mutationsApplied"), Mutations) && Mutations)
	{
		Result.Mutations.Reserve(Mutations->Num());
		for (const TSharedPtr<FJsonValue>& Entry : *Mutations)
		{
			const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			FCrowdyMutationApplied M;
			Obj->TryGetStringField(TEXT("containerId"), M.ContainerId);
			Obj->TryGetStringField(TEXT("key"), M.Key);
			Obj->TryGetStringField(TEXT("oldValueJson"), M.OldValueJson);
			Obj->TryGetStringField(TEXT("newValueJson"), M.NewValueJson);
			Result.Mutations.Add(MoveTemp(M));
		}
	}

	return Result;
}

bool FCrowdyGameApiCodec::ParseContainerStateEnvelope(const TSharedPtr<FJsonObject>& Envelope,
	bool bHttpOk, const TArray<FString>& TransportErrors, TSharedPtr<FJsonObject>& OutState)
{
	OutState.Reset();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* StatePtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelContainerState"), StatePtr) || !StatePtr)
	{
		return false; // container not found / null
	}
	// The visible properties ride as a JSON-encoded string (propertiesJson), not a raw JSON object.
	FString PropertiesJson;
	if (!(*StatePtr)->TryGetStringField(TEXT("propertiesJson"), PropertiesJson))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Parsed = ParseJsonObjectString(PropertiesJson);
	if (!Parsed.IsValid())
	{
		return false;
	}
	OutState = Parsed;
	return true;
}

bool FCrowdyGameApiCodec::ParseContainersEnvelope(const TSharedPtr<FJsonObject>& Envelope,
	bool bHttpOk, const TArray<FString>& TransportErrors, TArray<TSharedPtr<FJsonObject>>& OutContainers)
{
	OutContainers.Reset();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Data->TryGetArrayField(TEXT("gameModelContainers"), Arr) || !Arr)
	{
		return false;
	}
	OutContainers.Reserve(Arr->Num());
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (Obj.IsValid())
		{
			OutContainers.Add(Obj);
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildCreateContainerVariables(int64 AppId, const FString& TypeName,
	const FString& DisplayName, const FString& SessionId, const FString& MetadataJson)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	// appId is a BigInt! scalar it MUST be a JSON string, never a number.
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("typeName"), TypeName);
	// displayName is non-null on the server; fall back to the type name so a caller need not supply one.
	Input->SetStringField(TEXT("displayName"), DisplayName.IsEmpty() ? TypeName : DisplayName);
	// sessionId and metadataJson are nullable; omit them entirely when empty.
	if (!SessionId.IsEmpty())
	{
		Input->SetStringField(TEXT("sessionId"), SessionId);
	}
	if (!MetadataJson.IsEmpty())
	{
		Input->SetStringField(TEXT("metadataJson"), MetadataJson);
	}
	// ownerUserId is deliberately NOT set: the server defaults it to the authenticated caller for
	// member/owner instantiation, so the client cannot claim another user's container (fail-safe by omission).

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

bool FCrowdyGameApiCodec::ParseCreateContainerEnvelope(const TSharedPtr<FJsonObject>& Envelope,
	bool bHttpOk, const TArray<FString>& TransportErrors, FString& OutContainerId, int64& OutOwnerUserId)
{
	OutContainerId.Reset();
	OutOwnerUserId = 0;
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ContainerPtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelCreateContainer"), ContainerPtr) || !ContainerPtr)
	{
		return false;
	}
	if (!(*ContainerPtr)->TryGetStringField(TEXT("containerId"), OutContainerId) || OutContainerId.IsEmpty())
	{
		return false;
	}
	// ownerUserId is a BigInt returned as a string; best-effort read (the server pinned it to the caller).
	FString OwnerStr;
	if ((*ContainerPtr)->TryGetStringField(TEXT("ownerUserId"), OwnerStr))
	{
		LexFromString(OutOwnerUserId, *OwnerStr);
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildEnsureContainerVariables(int64 AppId, const FString& TypeName,
	const FString& BindingKey, const FString& DisplayName, const FString& SessionId, const FString& MetadataJson)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	// appId is a BigInt! scalar it MUST be a JSON string, never a number.
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("typeName"), TypeName);
	Input->SetStringField(TEXT("bindingKey"), BindingKey);
	// displayName is non-null on the server; fall back to the type name so a caller need not supply one.
	Input->SetStringField(TEXT("displayName"), DisplayName.IsEmpty() ? TypeName : DisplayName);
	// sessionId and metadataJson are nullable; omit them entirely when empty.
	if (!SessionId.IsEmpty())
	{
		Input->SetStringField(TEXT("sessionId"), SessionId);
	}
	if (!MetadataJson.IsEmpty())
	{
		Input->SetStringField(TEXT("metadataJson"), MetadataJson);
	}
	// ownerUserId is deliberately NOT set: the server pins it to the caller (member/owner types) or null (admin
	// types), so the client cannot claim another user's container (fail-safe by omission).

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

bool FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(const TSharedPtr<FJsonObject>& Envelope,
	bool bHttpOk, const TArray<FString>& TransportErrors, FString& OutContainerId, int64& OutOwnerUserId,
	bool& OutCreated)
{
	OutContainerId.Reset();
	OutOwnerUserId = 0;
	OutCreated = false;
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelEnsureContainer"), ResultPtr) || !ResultPtr)
	{
		return false;
	}
	(*ResultPtr)->TryGetBoolField(TEXT("created"), OutCreated);
	const TSharedPtr<FJsonObject>* ContainerPtr = nullptr;
	if (!(*ResultPtr)->TryGetObjectField(TEXT("container"), ContainerPtr) || !ContainerPtr)
	{
		return false;
	}
	if (!(*ContainerPtr)->TryGetStringField(TEXT("containerId"), OutContainerId) || OutContainerId.IsEmpty())
	{
		return false;
	}
	// ownerUserId is a BigInt returned as a string; best-effort read (null for a shared/admin-type row).
	FString OwnerStr;
	if ((*ContainerPtr)->TryGetStringField(TEXT("ownerUserId"), OwnerStr))
	{
		LexFromString(OutOwnerUserId, *OwnerStr);
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildReadContainerByKeyVariables(int64 AppId, const FString& TypeName,
	const FString& SessionId, const FString& BindingKey)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	if (!TypeName.IsEmpty())
	{
		Variables->SetStringField(TEXT("typeName"), TypeName);
	}
	if (!SessionId.IsEmpty())
	{
		Variables->SetStringField(TEXT("sessionId"), SessionId);
	}
	Variables->SetStringField(TEXT("bindingKey"), BindingKey);
	return Variables;
}

bool FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(const TSharedPtr<FJsonObject>& Envelope,
	bool bHttpOk, const TArray<FString>& TransportErrors, const FString& ExpectedBindingKey, bool& OutFound,
	FString& OutContainerId, int64& OutOwnerUserId)
{
	OutFound = false;
	OutContainerId.Reset();
	OutOwnerUserId = 0;
	TArray<TSharedPtr<FJsonObject>> Containers;
	if (!ParseContainersEnvelope(Envelope, bHttpOk, TransportErrors, Containers))
	{
		return false;
	}
	// A binding key is unique per (app, type, session), so the list holds 0 or 1 row. Verify the returned row's
	// bindingKey matches what we asked for: a drifted server that ignored the filter would return unrelated rows,
	// and binding one would point a remote proxy at the wrong player's container.
	for (const TSharedPtr<FJsonObject>& Container : Containers)
	{
		if (!Container.IsValid())
		{
			continue;
		}
		FString RowKey;
		if (!Container->TryGetStringField(TEXT("bindingKey"), RowKey) || RowKey != ExpectedBindingKey)
		{
			continue; // not the key we asked for (server did not honor the filter) - never bind it
		}
		FString Id;
		if (Container->TryGetStringField(TEXT("containerId"), Id) && !Id.IsEmpty())
		{
			OutContainerId = Id;
			FString OwnerStr;
			if (Container->TryGetStringField(TEXT("ownerUserId"), OwnerStr))
			{
				LexFromString(OutOwnerUserId, *OwnerStr);
			}
			OutFound = true;
			break;
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildListContainersByTypeVariables(int64 AppId, const FString& TypeName,
	const FString& SessionId, int32 Limit, int32 Offset)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	if (!TypeName.IsEmpty()) { Variables->SetStringField(TEXT("typeName"), TypeName); }
	if (!SessionId.IsEmpty()) { Variables->SetStringField(TEXT("sessionId"), SessionId); }
	Variables->SetNumberField(TEXT("limit"), FMath::Clamp(Limit, 1, MaxContainersPerPage));
	Variables->SetNumberField(TEXT("offset"), FMath::Max(0, Offset));
	return Variables;
}

bool FCrowdyGameApiCodec::ParseContainerRowsEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, TArray<FContainerRow>& OutRows, int32* OutRawRowCount)
{
	OutRows.Reset();
	if (OutRawRowCount) { *OutRawRowCount = 0; }
	TArray<TSharedPtr<FJsonObject>> Containers;
	if (!ParseContainersEnvelope(Envelope, bHttpOk, TransportErrors, Containers))
	{
		return false;
	}
	if (OutRawRowCount) { *OutRawRowCount = Containers.Num(); }
	for (const TSharedPtr<FJsonObject>& Container : Containers)
	{
		if (!Container.IsValid())
		{
			continue;
		}
		FContainerRow Row;
		if (!Container->TryGetStringField(TEXT("containerId"), Row.ContainerId) || Row.ContainerId.IsEmpty()
			|| !Container->TryGetStringField(TEXT("bindingKey"), Row.BindingKey) || Row.BindingKey.IsEmpty())
		{
			continue;
		}
		Container->TryGetStringField(TEXT("typeName"), Row.TypeName);
		Container->TryGetStringField(TEXT("sessionId"), Row.SessionId);
		FString OwnerStr;
		if (Container->TryGetStringField(TEXT("ownerUserId"), OwnerStr))
		{
			LexFromString(Row.OwnerUserId, *OwnerStr);
		}
		OutRows.Add(MoveTemp(Row));
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildContainerStatesVariables(int64 AppId, const TArray<FString>& ContainerIds)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	TArray<TSharedPtr<FJsonValue>> Ids;
	Ids.Reserve(ContainerIds.Num());
	for (const FString& Id : ContainerIds)
	{
		if (!Id.IsEmpty())
		{
			Ids.Add(MakeShared<FJsonValueString>(Id));
		}
	}
	Variables->SetArrayField(TEXT("containerIds"), Ids);
	return Variables;
}

bool FCrowdyGameApiCodec::ParseContainerStatesEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bTransportOk,
	const TArray<FString>& TransportErrors, TArray<FContainerStateRow>& OutRows)
{
	OutRows.Reset();
	if (!bTransportOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Data->TryGetArrayField(TEXT("gameModelContainerStates"), Arr) || !Arr)
	{
		return false;
	}
	// A reply can never legitimately exceed the per-call id limit, so anything past a generous multiple is dropped.
	constexpr int32 MaxRows = 4 * MaxContainerStatesPerCall;
	const int32 RowCount = FMath::Min(Arr->Num(), MaxRows);
	UE_CLOG(Arr->Num() > MaxRows, LogCrowdyNet, Warning,
		TEXT("[CrowdyGameApi] gameModelContainerStates returned %d rows; reading only the first %d."), Arr->Num(), MaxRows);
	OutRows.Reserve(RowCount);
	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		const TSharedPtr<FJsonValue>& V = (*Arr)[Index];
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}
		FContainerStateRow Row;
		if (!Obj->TryGetStringField(TEXT("containerId"), Row.ContainerId) || Row.ContainerId.IsEmpty())
		{
			continue;
		}
		Obj->TryGetStringField(TEXT("typeName"), Row.TypeName);
		Obj->TryGetStringField(TEXT("sessionId"), Row.SessionId);
		ReadOptionalBigInt(Obj, TEXT("ownerUserId"), Row.OwnerUserId);
		FString PropertiesJson;
		if (Obj->TryGetStringField(TEXT("propertiesJson"), PropertiesJson))
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_GM_DecodeResponse);
			Row.State = ParseJsonObjectString(PropertiesJson);
		}
		OutRows.Add(MoveTemp(Row));
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildCreateSessionVariables(int64 AppId, const FString& Name,
	const TArray<int64>& ParticipantUserIds, const FString& MetadataJson, const FCrowdyCreateSessionOptions& Options)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	// appId is a BigInt! scalar it MUST be a JSON string, never a number.
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	// name and metadataJson are nullable; omit them entirely when empty.
	if (!Name.IsEmpty())
	{
		Input->SetStringField(TEXT("name"), Name);
	}
	if (!MetadataJson.IsEmpty())
	{
		Input->SetStringField(TEXT("metadataJson"), MetadataJson);
	}
	// participantUserIds is a list of BigInt STRINGS; omit the field entirely when there are none.
	if (ParticipantUserIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Ids;
		Ids.Reserve(ParticipantUserIds.Num());
		for (const int64 Id : ParticipantUserIds)
		{
			Ids.Add(MakeShared<FJsonValueString>(LexToString(Id)));
		}
		Input->SetArrayField(TEXT("participantUserIds"), Ids);
	}
	// The Int options are JSON NUMBERS; each optional is omitted at its sentinel so the server default applies.
	if (Options.MaxParticipants > 0)
	{
		Input->SetNumberField(TEXT("maxParticipants"), Options.MaxParticipants);
	}
	if (!Options.Admission.IsEmpty())
	{
		Input->SetStringField(TEXT("admission"), Options.Admission);
	}
	// 0 is a real value here (it disables the empty-session timeout), so only a negative one is omitted.
	if (Options.EmptyTimeoutSec >= 0)
	{
		Input->SetNumberField(TEXT("emptyTimeoutSec"), Options.EmptyTimeoutSec);
	}
	if (!Options.Presence.IsEmpty())
	{
		Input->SetStringField(TEXT("presence"), Options.Presence);
	}
	WriteIdempotencyKey(Input, Options.IdempotencyKey);
	// seedFromApp is written only for a non-empty type list; an empty initialState is omitted so the server default applies.
	TArray<TSharedPtr<FJsonValue>> SeedTypeNames;
	SeedTypeNames.Reserve(Options.SeedFromAppTypeNames.Num());
	for (const FString& TypeName : Options.SeedFromAppTypeNames)
	{
		const FString Trimmed = TypeName.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			SeedTypeNames.Add(MakeShared<FJsonValueString>(Trimmed));
		}
	}
	if (SeedTypeNames.Num() > 0)
	{
		const TSharedPtr<FJsonObject> Seed = MakeShared<FJsonObject>();
		Seed->SetArrayField(TEXT("typeNames"), SeedTypeNames);
		if (!Options.SeedInitialState.IsEmpty())
		{
			Seed->SetStringField(TEXT("initialState"), Options.SeedInitialState);
		}
		Input->SetObjectField(TEXT("seedFromApp"), Seed);
	}
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildJoinSessionVariables(int64 AppId, const FString& SessionId,
	const FString& Role, const FString& ActorUuid, const FString& IdempotencyKey)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	// role and actorUuid are nullable; omit each entirely when empty (the server assigns its default role).
	if (!Role.IsEmpty())
	{
		Input->SetStringField(TEXT("role"), Role);
	}
	if (!ActorUuid.IsEmpty())
	{
		Input->SetStringField(TEXT("actorUuid"), ActorUuid);
	}
	WriteIdempotencyKey(Input, IdempotencyKey);
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildLeaveSessionVariables(int64 AppId, const FString& SessionId,
	int32 Incarnation, const FString& IdempotencyKey)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	// incarnation is a required Int (a JSON number), always written.
	Input->SetNumberField(TEXT("incarnation"), Incarnation);
	WriteIdempotencyKey(Input, IdempotencyKey);
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSetSessionAdmissionVariables(int64 AppId, const FString& SessionId,
	const FString& Admission, int32 ExpectedHostTerm)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	Input->SetStringField(TEXT("admission"), Admission);
	WriteExpectedHostTerm(Input, ExpectedHostTerm);
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildTransferSessionHostVariables(int64 AppId, const FString& SessionId,
	int64 ToUserId, int32 ExpectedHostTerm)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	// toUserId is a BigInt: a JSON string, never a number.
	Input->SetStringField(TEXT("toUserId"), LexToString(ToUserId));
	WriteExpectedHostTerm(Input, ExpectedHostTerm);
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildEndSessionVariables(int64 AppId, const FString& SessionId,
	const FString& Reason, int32 ExpectedHostTerm)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	// reason is nullable; omitted when empty (the server records "completed").
	if (!Reason.IsEmpty())
	{
		Input->SetStringField(TEXT("reason"), Reason);
	}
	WriteExpectedHostTerm(Input, ExpectedHostTerm);
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSetSessionTurnVariables(int64 AppId, const FString& SessionId,
	int64 UserId, bool bHasUserId, int32 ExpectedHostTerm)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	// userId is a BigInt string when SETTING the turn. To CLEAR it, the schema wants an EXPLICIT JSON null, not
	// an omitted field: the resolver treats an absent userId as "leave the turn unchanged" and only an explicit
	// null as "clear it", so omitting would silently fail to end the turn.
	if (bHasUserId)
	{
		Input->SetStringField(TEXT("userId"), LexToString(UserId));
	}
	else
	{
		Input->SetField(TEXT("userId"), MakeShared<FJsonValueNull>());
	}
	WriteExpectedHostTerm(Input, ExpectedHostTerm);
	return WrapSessionInput(Input);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildListSessionsVariables(int64 AppId, const FString& Status,
	const FString& Admission, int64 HostUserId, int32 Limit)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	// Every filter is nullable; omit each entirely when unset (status/admission empty, hostUserId 0, limit <= 0).
	if (!Status.IsEmpty())
	{
		Variables->SetStringField(TEXT("status"), Status);
	}
	if (!Admission.IsEmpty())
	{
		Variables->SetStringField(TEXT("admission"), Admission);
	}
	if (HostUserId != 0)
	{
		Variables->SetStringField(TEXT("hostUserId"), LexToString(HostUserId));
	}
	if (Limit > 0)
	{
		Variables->SetNumberField(TEXT("limit"), Limit);
	}
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildGetSessionVariables(int64 AppId, const FString& SessionId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	Variables->SetStringField(TEXT("sessionId"), SessionId);
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSessionSnapshotVariables(int64 AppId, const FString& SessionId)
{
	return BuildGetSessionVariables(AppId, SessionId);
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSessionEventsVariables(int64 AppId, const FString& SessionId,
	int64 AfterRevision, int32 Limit)
{
	const TSharedPtr<FJsonObject> Variables = BuildGetSessionVariables(AppId, SessionId);
	// afterRevision is a String! on the wire (a revision compares as an integer but rides as text).
	Variables->SetStringField(TEXT("afterRevision"), LexToString(AfterRevision));
	if (Limit > 0)
	{
		Variables->SetNumberField(TEXT("limit"), Limit);
	}
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSessionChangedVariables(int64 AppId, const FString& SessionId,
	int64 AfterRevision, bool bHasAfterRevision)
{
	const TSharedPtr<FJsonObject> Variables = BuildGetSessionVariables(AppId, SessionId);
	// afterRevision is nullable here: omitted, the stream starts at the current revision.
	if (bHasAfterRevision)
	{
		Variables->SetStringField(TEXT("afterRevision"), LexToString(AfterRevision));
	}
	return Variables;
}

FCrowdyGameSessionData FCrowdyGameApiCodec::ParseSessionObject(const TSharedPtr<FJsonObject>& SessionObj)
{
	FCrowdyGameSessionData Session;
	if (!SessionObj.IsValid())
	{
		return Session;
	}
	SessionObj->TryGetStringField(TEXT("sessionId"), Session.SessionId);
	ReadOptionalBigInt(SessionObj, TEXT("appId"), Session.AppId);
	SessionObj->TryGetStringField(TEXT("name"), Session.Name);
	SessionObj->TryGetStringField(TEXT("status"), Session.Status);
	ReadOptionalBigInt(SessionObj, TEXT("createdByUserId"), Session.CreatedByUserId);
	// bHasCurrentTurn is true only when currentTurnUserId was present AND non-null.
	Session.bHasCurrentTurn = ReadOptionalBigInt(SessionObj, TEXT("currentTurnUserId"), Session.CurrentTurnUserId);
	SessionObj->TryGetStringField(TEXT("metadataJson"), Session.MetadataJson);
	SessionObj->TryGetStringField(TEXT("admission"), Session.Admission);
	// maxParticipants and hostUserId are nullable; each bHas flag is true only for a present, non-null value.
	Session.bHasMaxParticipants = ReadOptionalInt(SessionObj, TEXT("maxParticipants"), Session.MaxParticipants);
	ReadOptionalInt(SessionObj, TEXT("participantCount"), Session.ParticipantCount);
	Session.bHasHost = ReadOptionalBigInt(SessionObj, TEXT("hostUserId"), Session.HostUserId);
	ReadOptionalInt(SessionObj, TEXT("hostTerm"), Session.HostTerm);
	// revision rides as a decimal string; the string-first BigInt read keeps it exact.
	ReadOptionalBigInt(SessionObj, TEXT("revision"), Session.Revision);
	SessionObj->TryGetStringField(TEXT("endedAt"), Session.EndedAt);
	SessionObj->TryGetStringField(TEXT("endReason"), Session.EndReason);
	SessionObj->TryGetStringField(TEXT("createdAt"), Session.CreatedAt);
	SessionObj->TryGetStringField(TEXT("presence"), Session.Presence);
	// Only the create response carries a seeded count; every other read returns null and leaves the flag false.
	Session.bHasSeededContainerCount = ReadOptionalInt(SessionObj, TEXT("seededContainerCount"), Session.SeededContainerCount);
	return Session;
}

FCrowdyGameSessionParticipantData FCrowdyGameApiCodec::ParseSessionParticipantObject(const TSharedPtr<FJsonObject>& Obj)
{
	FCrowdyGameSessionParticipantData Participant;
	if (!Obj.IsValid())
	{
		return Participant;
	}
	Obj->TryGetStringField(TEXT("sessionId"), Participant.SessionId);
	ReadOptionalBigInt(Obj, TEXT("userId"), Participant.UserId);
	Obj->TryGetStringField(TEXT("role"), Participant.Role);
	Obj->TryGetStringField(TEXT("state"), Participant.State);
	ReadOptionalInt(Obj, TEXT("incarnation"), Participant.Incarnation);
	Obj->TryGetStringField(TEXT("actorUuid"), Participant.ActorUuid);
	Obj->TryGetStringField(TEXT("joinedAt"), Participant.JoinedAt);
	Obj->TryGetStringField(TEXT("leftAt"), Participant.LeftAt);
	Obj->TryGetStringField(TEXT("leftReason"), Participant.LeftReason);
	return Participant;
}

FCrowdyGameSessionEventData FCrowdyGameApiCodec::ParseSessionEventObject(const TSharedPtr<FJsonObject>& Obj)
{
	FCrowdyGameSessionEventData Event;
	if (!Obj.IsValid())
	{
		return Event;
	}
	ReadOptionalBigInt(Obj, TEXT("appId"), Event.AppId);
	Obj->TryGetStringField(TEXT("sessionId"), Event.SessionId);
	ReadOptionalBigInt(Obj, TEXT("revision"), Event.Revision);
	Obj->TryGetStringField(TEXT("kind"), Event.Kind);
	Obj->TryGetStringField(TEXT("payloadJson"), Event.PayloadJson);
	Obj->TryGetStringField(TEXT("createdAt"), Event.CreatedAt);
	return Event;
}

bool FCrowdyGameApiCodec::ParseSessionEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, const TCHAR* FieldName, FCrowdyGameSessionData& OutSession)
{
	OutSession = FCrowdyGameSessionData();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SessionPtr = nullptr;
	if (!Data->TryGetObjectField(FieldName, SessionPtr) || !SessionPtr)
	{
		return false; // session not found / null
	}
	OutSession = ParseSessionObject(*SessionPtr);
	return true;
}

bool FCrowdyGameApiCodec::ParseSessionsEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, TArray<FCrowdyGameSessionData>& OutSessions)
{
	OutSessions.Reset();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Data->TryGetArrayField(TEXT("gameModelSessions"), Arr) || !Arr)
	{
		return false;
	}
	OutSessions.Reserve(Arr->Num());
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (Obj.IsValid())
		{
			OutSessions.Add(ParseSessionObject(Obj));
		}
	}
	return true;
}

bool FCrowdyGameApiCodec::ParseJoinSessionEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, FCrowdyGameSessionParticipantData& OutParticipant)
{
	return ParseSessionParticipantEnvelope(Envelope, bHttpOk, TransportErrors, TEXT("gameModelJoinSession"), OutParticipant);
}

bool FCrowdyGameApiCodec::ParseSessionParticipantEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, const TCHAR* FieldName, FCrowdyGameSessionParticipantData& OutParticipant)
{
	OutParticipant = FCrowdyGameSessionParticipantData();
	const TSharedPtr<FJsonObject> ParticipantObj = ReadDataObjectField(Envelope, bHttpOk, TransportErrors, FieldName);
	if (!ParticipantObj.IsValid())
	{
		return false;
	}
	OutParticipant = ParseSessionParticipantObject(ParticipantObj);
	return true;
}

bool FCrowdyGameApiCodec::ParseSessionSnapshotEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, FCrowdyGameSessionSnapshotData& OutSnapshot)
{
	OutSnapshot = FCrowdyGameSessionSnapshotData();
	const TSharedPtr<FJsonObject> SnapshotObj =
		ReadDataObjectField(Envelope, bHttpOk, TransportErrors, TEXT("gameModelSessionSnapshot"));
	if (!SnapshotObj.IsValid())
	{
		return false;
	}
	ReadOptionalBigInt(SnapshotObj, TEXT("revision"), OutSnapshot.Revision);
	const TSharedPtr<FJsonObject>* SessionPtr = nullptr;
	if (SnapshotObj->TryGetObjectField(TEXT("session"), SessionPtr) && SessionPtr)
	{
		OutSnapshot.Session = ParseSessionObject(*SessionPtr);
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!SnapshotObj->TryGetArrayField(TEXT("participants"), Arr) || !Arr)
	{
		return true;
	}
	OutSnapshot.Participants.Reserve(Arr->Num());
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (Obj.IsValid())
		{
			OutSnapshot.Participants.Add(ParseSessionParticipantObject(Obj));
		}
	}
	return true;
}

bool FCrowdyGameApiCodec::ParseSessionEventsEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, TArray<FCrowdyGameSessionEventData>& OutEvents)
{
	OutEvents.Reset();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Data->TryGetArrayField(TEXT("gameModelSessionEvents"), Arr) || !Arr)
	{
		return false;
	}
	OutEvents.Reserve(Arr->Num());
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (Obj.IsValid())
		{
			OutEvents.Add(ParseSessionEventObject(Obj));
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildAddEdgeVariables(int64 AppId, const FString& FromContainerId,
	const FString& ToContainerId, const FString& RelationshipType, double Weight, bool bHasWeight,
	const FString& MetadataJson)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("fromContainerId"), FromContainerId);
	Input->SetStringField(TEXT("toContainerId"), ToContainerId);
	Input->SetStringField(TEXT("relationshipType"), RelationshipType);
	// weight is a nullable JSON NUMBER; write it only when the caller supplied one so 0 is not forced.
	if (bHasWeight)
	{
		Input->SetNumberField(TEXT("weight"), Weight);
	}
	if (!MetadataJson.IsEmpty())
	{
		Input->SetStringField(TEXT("metadataJson"), MetadataJson);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildTraverseVariables(int64 AppId, const FString& RootId,
	const FString& RelationshipType, int32 Depth)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	Variables->SetStringField(TEXT("rootId"), RootId);
	Variables->SetStringField(TEXT("relationshipType"), RelationshipType);
	// depth is a GraphQL Int! (a JSON number, never a string), clamped to the server's [1,5] range.
	const int32 ClampedDepth = FMath::Clamp(Depth, 1, 5);
	Variables->SetNumberField(TEXT("depth"), ClampedDepth);
	return Variables;
}

FCrowdyEdgeData FCrowdyGameApiCodec::ParseEdgeObject(const TSharedPtr<FJsonObject>& EdgeObj)
{
	FCrowdyEdgeData Edge;
	if (!EdgeObj.IsValid())
	{
		return Edge;
	}
	EdgeObj->TryGetStringField(TEXT("edgeId"), Edge.EdgeId);
	EdgeObj->TryGetStringField(TEXT("fromContainerId"), Edge.FromContainerId);
	EdgeObj->TryGetStringField(TEXT("toContainerId"), Edge.ToContainerId);
	EdgeObj->TryGetStringField(TEXT("relationshipType"), Edge.RelationshipType);
	// weight is a nullable number; bHasWeight separates a real 0 from an absent weight.
	Edge.bHasWeight = EdgeObj->TryGetNumberField(TEXT("weight"), Edge.Weight);
	return Edge;
}

bool FCrowdyGameApiCodec::ParseAddEdgeEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, FCrowdyEdgeData& OutEdge)
{
	OutEdge = FCrowdyEdgeData();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* EdgePtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelAddEdge"), EdgePtr) || !EdgePtr)
	{
		return false;
	}
	OutEdge = ParseEdgeObject(*EdgePtr);
	// A real edge always carries an id; an empty one means nothing usable came back.
	return !OutEdge.EdgeId.IsEmpty();
}

bool FCrowdyGameApiCodec::ParseTraverseEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, FCrowdyTraverseData& OutResult)
{
	OutResult = FCrowdyTraverseData();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* TraversePtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelTraverse"), TraversePtr) || !TraversePtr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& TraverseObj = *TraversePtr;
	TraverseObj->TryGetStringField(TEXT("rootId"), OutResult.RootId);

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (TraverseObj->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
	{
		OutResult.Nodes.Reserve(Nodes->Num());
		for (const TSharedPtr<FJsonValue>& V : *Nodes)
		{
			const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
			if (Obj.IsValid())
			{
				OutResult.Nodes.Add(Obj);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	if (TraverseObj->TryGetArrayField(TEXT("edges"), Edges) && Edges)
	{
		OutResult.Edges.Reserve(Edges->Num());
		for (const TSharedPtr<FJsonValue>& V : *Edges)
		{
			const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
			if (Obj.IsValid())
			{
				OutResult.Edges.Add(ParseEdgeObject(Obj));
			}
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSetPropertyVariables(int64 AppId, const FString& ContainerId,
	const FString& Key, const FString& ValueType, const FString& ValueJson)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("containerId"), ContainerId);
	Input->SetStringField(TEXT("key"), Key);
	Input->SetStringField(TEXT("valueType"), ValueType);
	// valueJson is ALREADY a JSON-encoded value string (e.g. "\"Aria\"" or "42"); pass it through verbatim,
	// never re-encoded.
	Input->SetStringField(TEXT("valueJson"), ValueJson);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

bool FCrowdyGameApiCodec::ParseSetPropertyEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, FString& OutContainerId)
{
	OutContainerId.Reset();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ContainerPtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelSetProperty"), ContainerPtr) || !ContainerPtr)
	{
		return false;
	}
	if (!(*ContainerPtr)->TryGetStringField(TEXT("containerId"), OutContainerId) || OutContainerId.IsEmpty())
	{
		return false;
	}
	return true;
}

namespace
{
	// Reads a top-level Boolean! delete result (data.<FieldName>). Returns false on a transport/parse failure or
	// a GraphQL error (an authorization/refusal); true with OutDeleted set on a clean read.
	bool ReadBooleanDeleteResult(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
		const TArray<FString>& TransportErrors, const TCHAR* FieldName, bool& OutDeleted)
	{
		OutDeleted = false;
		if (!bHttpOk || TransportErrors.Num() > 0)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}
		bool bValue = false;
		if (!Data->TryGetBoolField(FieldName, bValue))
		{
			return false;
		}
		OutDeleted = bValue;
		return true;
	}
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildDeleteContainerVariables(int64 AppId, const FString& ContainerId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId)); // BigInt! as a JSON string
	Variables->SetStringField(TEXT("containerId"), ContainerId);
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildDeleteEdgeVariables(int64 AppId, const FString& EdgeId)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId)); // BigInt! as a JSON string
	Variables->SetStringField(TEXT("edgeId"), EdgeId);
	return Variables;
}

bool FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, bool& OutDeleted)
{
	return ReadBooleanDeleteResult(Envelope, bHttpOk, TransportErrors, TEXT("gameModelDeleteContainer"), OutDeleted);
}

bool FCrowdyGameApiCodec::ParseDeleteEdgeEnvelope(const TSharedPtr<FJsonObject>& Envelope, bool bHttpOk,
	const TArray<FString>& TransportErrors, bool& OutDeleted)
{
	return ReadBooleanDeleteResult(Envelope, bHttpOk, TransportErrors, TEXT("gameModelDeleteEdge"), OutDeleted);
}
