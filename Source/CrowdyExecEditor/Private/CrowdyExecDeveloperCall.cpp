#include "CrowdyExecDeveloperCall.h"

#include "CrowdyExecCodec.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObjectDefinition.h"
#include "Dom/JsonObject.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace CrowdyExecDeveloperCallDetail
{
	const TCHAR* StatusName(ECrowdyNativeExecStatus Status)
	{
		switch (Status)
		{
		case ECrowdyNativeExecStatus::Ok: return TEXT("Ok");
		case ECrowdyNativeExecStatus::AppError: return TEXT("AppError");
		case ECrowdyNativeExecStatus::Busy: return TEXT("Busy");
		case ECrowdyNativeExecStatus::Moved: return TEXT("Moved");
		case ECrowdyNativeExecStatus::NotFound: return TEXT("NotFound");
		case ECrowdyNativeExecStatus::DeadlineExceeded: return TEXT("DeadlineExceeded");
		case ECrowdyNativeExecStatus::Denied: return TEXT("Denied");
		case ECrowdyNativeExecStatus::RateLimited: return TEXT("RateLimited");
		case ECrowdyNativeExecStatus::Unavailable: return TEXT("Unavailable");
		case ECrowdyNativeExecStatus::Internal: return TEXT("Internal");
		case ECrowdyNativeExecStatus::Trapped: return TEXT("Trapped");
		case ECrowdyNativeExecStatus::BadRequest: return TEXT("BadRequest");
		default: return TEXT("Unknown");
		}
	}

	CrowdyExecDeveloperCall::FResult Failed(const TCHAR* Status, const FString& Message)
	{
		CrowdyExecDeveloperCall::FResult Result;
		Result.Status = Status;
		Result.Message = Message;
		return Result;
	}
}

bool CrowdyExecDeveloperCall::IsForDevelopers(const FCrowdyServerFunction& Function)
{
	return Function.WhoCanCall == ECrowdyServerFunctionCaller::ServerOnly || Function.WhoCanCall == ECrowdyServerFunctionCaller::DevelopersOnly;
}

bool CrowdyExecDeveloperCall::InputsFromJson(const FCrowdyServerFunction& Function, const FString& Json, FInstancedStruct& OutInputs, FString& OutError)
{
	Function.InitializeParams(OutInputs);
	if (Json.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}
	if (!OutInputs.IsValid())
	{
		OutError = FString::Printf(TEXT("%s takes no inputs"), *Function.Name.ToString());
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object)
	{
		OutError = FString::Printf(TEXT("the inputs are not a JSON object: %s"), *Json);
		return false;
	}
	// The converter skips a name it does not know, which would send that input's default without a word.
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		bool bKnown = false;
		for (TFieldIterator<FProperty> It(OutInputs.GetScriptStruct()); It && !bKnown; ++It)
		{
			bKnown = It->GetAuthoredName().Equals(Field.Key, ESearchCase::IgnoreCase);
		}
		if (!bKnown)
		{
			OutError = FString::Printf(TEXT("%s has no input named %s"), *Function.Name.ToString(), *Field.Key);
			return false;
		}
	}
	if (!FJsonObjectConverter::JsonObjectToUStruct(Object.ToSharedRef(), OutInputs.GetScriptStruct(), OutInputs.GetMutableMemory()))
	{
		OutError = FString::Printf(TEXT("the inputs do not fit %s's: %s"), *Function.Name.ToString(), *Json);
		return false;
	}
	return true;
}

void CrowdyExecDeveloperCall::Call(const CrowdyExecDeveloper::FClient& Client, const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function,
	const FString& Key, const FInstancedStruct& Inputs, TFunction<void(const FResult&)> OnDone)
{
	using namespace CrowdyExecDeveloperCallDetail;
	const UScriptStruct* const ParamsStruct = Function.GetParamsStruct();
	if (Inputs.GetScriptStruct() != ParamsStruct)
	{
		OnDone(Failed(TEXT("BadRequest"), FString::Printf(TEXT("the inputs are not %s's"), *Function.Name.ToString())));
		return;
	}
	TArray<uint8> Payload;
	FString Error;
	if (!CrowdyExec::Encode(Definition, ParamsStruct, Inputs.GetMemory(), Payload, Error))
	{
		OnDone(Failed(TEXT("BadRequest"), Error));
		return;
	}
	// As a game acquires it: a type with one instance has one Instance Id, whatever was typed.
	const FString UsedKey = Definition.bOnlyOneInstance ? FString(UCrowdyServerObjectDefinition::OnlyInstanceId) : Key;
	if (UsedKey.IsEmpty())
	{
		OnDone(Failed(TEXT("BadRequest"), TEXT("Give the Instance Id of the Server Object to call")));
		return;
	}
	const TSharedPtr<FCrowdyNativeExecConnection> Connection = Client.ConnectAsDeveloper(Definition.TypeName, UsedKey);
	if (!Connection)
	{
		OnDone(Failed(TEXT("Unavailable"), TEXT("Could not open a connection as the developer; sign in to Crowdy Studio and try again")));
		return;
	}
	const TWeakObjectPtr<const UCrowdyServerObjectDefinition> Held(&Definition);
	// A List's struct is remade when the List is edited, so the call keeps the one it will decode into.
	const TStrongObjectPtr<UScriptStruct> ReplyStruct(const_cast<UScriptStruct*>(Function.GetReplyStruct()));
	const FString NodeType = Definition.TypeName;
	const FString Method = Function.GetMethodName();
	// Connected first, so a refused dial says why rather than the call's "no host"; the connection lives until its one answer.
	Connection->Connect([Connection, Held, ReplyStruct, NodeType, Key = UsedKey, Method, Payload = MoveTemp(Payload), OnDone = MoveTemp(OnDone)](bool bConnected, const FString& Why) mutable
	{
		if (!bConnected)
		{
			Connection->Close();
			OnDone(Failed(TEXT("Unavailable"), FString::Printf(TEXT("could not connect as the developer: %s"), *Why)));
			return;
		}
		Connection->Call(NodeType, Key, Method, MoveTemp(Payload), [Connection, Held, ReplyStruct, OnDone = MoveTemp(OnDone)](const FCrowdyNativeExecReply& Reply)
		{
			FResult Result;
			Result.Status = StatusName(Reply.Status);
			Result.Message = Reply.Message;
			Result.bOk = Reply.Status == ECrowdyNativeExecStatus::Ok;
			if (Result.bOk && ReplyStruct && Held.IsValid())
			{
				FInstancedStruct Values(ReplyStruct.Get());
				FString DecodeError;
				if (CrowdyExec::Decode(*Held.Get(), *ReplyStruct, Reply.Payload, Values.GetMutableMemory(), DecodeError))
				{
					FJsonObjectConverter::UStructToJsonObjectString(ReplyStruct.Get(), Values.GetMemory(), Result.ReplyJson, 0, 0, 0, nullptr, false);
				}
				else
				{
					Result.bOk = false;
					Result.Message = FString::Printf(TEXT("the reply does not fit the function's outputs: %s"), *DecodeError);
				}
			}
			Connection->Close();
			OnDone(Result);
		});
	});
}
