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
		case ECrowdyNativeExecStatus::AppPaused: return TEXT("AppPaused");
		case ECrowdyNativeExecStatus::AppDisabled: return TEXT("AppDisabled");
		case ECrowdyNativeExecStatus::TypeDisabled: return TEXT("TypeDisabled");
		default: return TEXT("Unknown");
		}
	}

	const FProperty* FindField(const UStruct& Struct, const FString& Name)
	{
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			if (It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Where in Value a 64-bit integer is written as a JSON number too large to be exact, such as "Ids[2]"; empty when nowhere. */
	FString FindInexactInt64(const FProperty& Property, const FJsonValue& Value, const FString& Path)
	{
		constexpr double LargestExactNumber = 9007199254740991.0;
		double Number = 0.0;
		if (CastField<FInt64Property>(&Property) || CastField<FUInt64Property>(&Property))
		{
			return Value.Type == EJson::Number && Value.TryGetNumber(Number) && FMath::Abs(Number) > LargestExactNumber ? Path : FString();
		}
		const FProperty* Element = nullptr;
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			Element = Array->Inner;
		}
		else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
		{
			Element = Set->ElementProp;
		}
		if (Element && Value.Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value.AsArray();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FString Found = Items[Index] ? FindInexactInt64(*Element, *Items[Index], FString::Printf(TEXT("%s[%d]"), *Path, Index)) : FString();
				if (!Found.IsEmpty())
				{
					return Found;
				}
			}
			return FString();
		}
		const FStructProperty* Struct = CastField<FStructProperty>(&Property);
		const FMapProperty* Map = CastField<FMapProperty>(&Property);
		if ((!Struct && !Map) || Value.Type != EJson::Object)
		{
			return FString();
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Value.AsObject()->Values)
		{
			const FProperty* Inner = Map ? Map->ValueProp : FindField(*Struct->Struct, Field.Key);
			const FString Found = Inner && Field.Value ? FindInexactInt64(*Inner, *Field.Value, Path + TEXT(".") + Field.Key) : FString();
			if (!Found.IsEmpty())
			{
				return Found;
			}
		}
		return FString();
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
		const FProperty* Known = CrowdyExecDeveloperCallDetail::FindField(*OutInputs.GetScriptStruct(), Field.Key);
		if (!Known)
		{
			OutError = FString::Printf(TEXT("%s has no input named %s"), *Function.Name.ToString(), *Field.Key);
			return false;
		}
		// The converter reads a JSON number through a double, which drops the low digits of a 64-bit id; a string is read exactly.
		const FString Inexact = Field.Value ? CrowdyExecDeveloperCallDetail::FindInexactInt64(*Known, *Field.Value, Field.Key) : FString();
		if (!Inexact.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s is above 9007199254740991; write it as a string, for example \"9007199254740993\"."), *Inexact);
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
	const FString UsedKey = Definition.HasOnlyOneInstance() ? Definition.GetOnlyInstanceId() : Key;
	if (UsedKey.IsEmpty() && !Definition.HasOnlyOneInstance())
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
