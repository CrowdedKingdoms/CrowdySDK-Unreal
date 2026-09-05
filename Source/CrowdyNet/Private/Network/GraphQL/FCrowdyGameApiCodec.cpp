// Fill out your copyright notice in the Description page of Project Settings.

#include "Network/GraphQL/FCrowdyGameApiCodec.h"

#include "CrowdyNetLog.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Dom/JsonValue.h"
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

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildCreateSessionVariables(int64 AppId, const FString& Name,
	const TArray<int64>& ParticipantUserIds, const FString& MetadataJson)
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

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildJoinSessionVariables(int64 AppId, const FString& SessionId,
	const FString& Role)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	Input->SetStringField(TEXT("sessionId"), SessionId);
	// role is nullable; omit it entirely when empty (the server assigns its default role).
	if (!Role.IsEmpty())
	{
		Input->SetStringField(TEXT("role"), Role);
	}

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildSetSessionTurnVariables(int64 AppId, const FString& SessionId,
	int64 UserId, bool bHasUserId)
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

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

TSharedPtr<FJsonObject> FCrowdyGameApiCodec::BuildListSessionsVariables(int64 AppId, const FString& Status)
{
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), LexToString(AppId));
	// status is nullable; omit it entirely when empty (return sessions of any status).
	if (!Status.IsEmpty())
	{
		Variables->SetStringField(TEXT("status"), Status);
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
	return Session;
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
	const TArray<FString>& TransportErrors, FString& OutSessionId, int64& OutUserId, FString& OutRole)
{
	OutSessionId.Reset();
	OutUserId = 0;
	OutRole.Reset();
	if (!bHttpOk || TransportErrors.Num() > 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Data = GetDataObject(Envelope);
	if (!Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* JoinPtr = nullptr;
	if (!Data->TryGetObjectField(TEXT("gameModelJoinSession"), JoinPtr) || !JoinPtr)
	{
		return false;
	}
	(*JoinPtr)->TryGetStringField(TEXT("sessionId"), OutSessionId);
	ReadOptionalBigInt(*JoinPtr, TEXT("userId"), OutUserId);
	(*JoinPtr)->TryGetStringField(TEXT("role"), OutRole);
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
