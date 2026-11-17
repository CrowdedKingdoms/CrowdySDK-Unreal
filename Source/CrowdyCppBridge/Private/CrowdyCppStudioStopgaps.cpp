#include "CrowdyCppStudioStopgaps.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/graphql/http.hpp"
THIRD_PARTY_INCLUDES_END

#include <string>

namespace
{
	constexpr int32 MaxModelIdChars = 256;
	constexpr int32 MaxModalities = 16;
	constexpr int32 MaxModalityChars = 32;
	constexpr int32 MaxErrorMessageChars = 1024;
	constexpr int32 MaxErrorCodeChars = 64;

	struct FHandWrittenDocument
	{
		ECrowdyCppApiDomain Domain;
		std::string_view Operation;
		std::string_view Document;
	};

	// The generated client has no document for these yet.
	constexpr FHandWrittenDocument HandWrittenDocuments[] = {
		{ECrowdyCppApiDomain::CrowdyStudioAgent, "CrowdyStudioLiveModding",
			"query CrowdyStudioLiveModding($appId: BigInt!, $action: String!) { crowdyStudioLiveModding(appId: $appId, action: $action) }"},
	};

	FString StopgapUtf8ToFString(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Value.data()), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	std::string StopgapToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value, Value.Len());
		return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
	}

	// A JSON string of at most MaxChars characters, else empty.
	FString BoundedString(const TSharedPtr<FJsonValue>& Value, int32 MaxChars)
	{
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return FString();
		}
		FString Text = Value->AsString();
		return Text.Len() <= MaxChars ? Text : FString();
	}

	TSharedPtr<FJsonObject> ParseBoundedJsonObject(const FString& Body)
	{
		TSharedPtr<FJsonObject> Root;
		if (!CrowdyJsonSafety::IsNestingWithinLimit(Body) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Root))
		{
			return nullptr;
		}
		return Root;
	}

	// False for an entry that is not an object or has no usable id; a context window that is null or not a token count reads as 0.
	bool ReadModel(const TSharedPtr<FJsonValue>& Entry, FCrowdyCppStudioModel& Out)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(Object) || !Object || !Object->IsValid())
		{
			return false;
		}
		Out.Id = BoundedString((*Object)->TryGetField(TEXT("id")), MaxModelIdChars);
		if (Out.Id.IsEmpty())
		{
			return false;
		}
		Out.Name = BoundedString((*Object)->TryGetField(TEXT("name")), MaxModelIdChars);

		const TSharedPtr<FJsonValue> Window = (*Object)->TryGetField(TEXT("context_window"));
		const double Tokens = Window.IsValid() && Window->Type == EJson::Number ? Window->AsNumber() : 0.0;
		Out.ContextWindow = Tokens >= 1.0 && Tokens <= static_cast<double>(MAX_int32) && FMath::Frac(Tokens) == 0.0
			? static_cast<int32>(Tokens) : 0;

		const TArray<TSharedPtr<FJsonValue>>* Modalities = nullptr;
		if (!(*Object)->TryGetArrayField(TEXT("input_modalities"), Modalities) || !Modalities)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Modality : *Modalities)
		{
			const FString Text = BoundedString(Modality, MaxModalityChars);
			if (!Text.IsEmpty() && Out.InputModalities.Num() < MaxModalities)
			{
				Out.InputModalities.Add(Text);
			}
		}
		return true;
	}

	// The OpenAI-style { "error": { message, code, retryable } } body the model endpoint refuses with.
	void ReadErrorBody(const FString& Body, FCrowdyCppStudioModelsResult& Result)
	{
		const TSharedPtr<FJsonObject> Root = ParseBoundedJsonObject(Body);
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("error"), Error) || !Error || !Error->IsValid())
		{
			return;
		}
		FString Message;
		(*Error)->TryGetStringField(TEXT("message"), Message);
		Result.ErrorMessage = Message.Left(MaxErrorMessageChars);
		Result.ErrorCode = BoundedString((*Error)->TryGetField(TEXT("code")), MaxErrorCodeChars);
		bool bRetryable = false;
		Result.bRetryable = (*Error)->TryGetBoolField(TEXT("retryable"), bRetryable) && bRetryable;
	}

	bool IsSuccessStatus(int32 Status)
	{
		return Status >= 200 && Status < 300;
	}
}

namespace CrowdyCppStudioStopgaps
{
	std::string_view Document(ECrowdyCppApiDomain Domain, std::string_view Operation)
	{
		for (const FHandWrittenDocument& Entry : HandWrittenDocuments)
		{
			if (Entry.Domain == Domain && Entry.Operation == Operation)
			{
				return Entry.Document;
			}
		}
		return std::string_view();
	}

	FString UrlOrigin(const FString& Url)
	{
		const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd == INDEX_NONE)
		{
			return FString();
		}
		const int32 AuthorityStart = SchemeEnd + 3;
		int32 AuthorityEnd = AuthorityStart;
		while (AuthorityEnd < Url.Len() && Url[AuthorityEnd] != TEXT('/') && Url[AuthorityEnd] != TEXT('?')
			&& Url[AuthorityEnd] != TEXT('#'))
		{
			++AuthorityEnd;
		}
		return AuthorityEnd > AuthorityStart ? Url.Left(AuthorityEnd) : FString();
	}

	// The app token travels over https, or over plain http to this machine in a build that is not Shipping.
	static bool StopgapOriginMayCarryAToken(const FString& Origin)
	{
		if (Origin.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			return true;
		}
#if UE_BUILD_SHIPPING
		return false;
#else
		const FString Authority = Origin.Mid(Origin.Find(TEXT("://")) + 3);
		static const TCHAR* const Hosts[] = {TEXT("localhost"), TEXT("127.0.0.1"), TEXT("[::1]")};
		for (const TCHAR* Host : Hosts)
		{
			if (Origin.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)
				&& (Authority.Equals(Host, ESearchCase::IgnoreCase) || Authority.StartsWith(FString(Host) + TEXT(":"), ESearchCase::IgnoreCase)))
			{
				return true;
			}
		}
		return false;
#endif
	}

	bool BuildModelsRequest(const FString& ApiEndpoint, int64 AppId, const FString& AppToken,
		crowdy::graphql::HttpRequest& OutRequest, FString& OutRefusal)
	{
		const FString Origin = UrlOrigin(ApiEndpoint);
		if (AppId <= 0 || Origin.IsEmpty())
		{
			OutRefusal = AppId <= 0 ? TEXT("no app id to list models for") : TEXT("the API endpoint names no origin");
			return false;
		}
		if (!StopgapOriginMayCarryAToken(Origin))
		{
			OutRefusal = TEXT("the API endpoint is not https, so the model list was not requested");
			return false;
		}
		if (AppToken.IsEmpty())
		{
			OutRefusal = TEXT("no app token is installed");
			return false;
		}
		OutRequest.method = "GET";
		OutRequest.url = StopgapToUtf8(Origin + TEXT("/v1/model/models?appId=") + LexToString(AppId));
		OutRequest.headers.emplace_back("Authorization", "Bearer " + StopgapToUtf8(AppToken));
		OutRequest.headers.emplace_back("Accept", "application/json");
		return true;
	}

	FCrowdyCppStudioModelsResult MapModels(const crowdy::graphql::HttpOutcome& Out)
	{
		FCrowdyCppStudioModelsResult Result;
		if (!Out.status.ok())
		{
			Result.ErrorMessage = Out.errorMessage.empty() ? FString(TEXT("HTTP request failed")) : StopgapUtf8ToFString(Out.errorMessage);
			return Result;
		}
		Result.HttpStatus = Out.response.status;
		if (Out.response.body.size() > static_cast<size_t>(MaxModelsBodyBytes))
		{
			Result.ErrorMessage = TEXT("the model list exceeds the size limit");
			return Result;
		}

		const FString Body = StopgapUtf8ToFString(Out.response.body);
		if (!IsSuccessStatus(Result.HttpStatus))
		{
			ReadErrorBody(Body, Result);
			Result.ErrorMessage = Result.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Server returned HTTP %d"), Result.HttpStatus) : Result.ErrorMessage;
			return Result;
		}

		const TSharedPtr<FJsonObject> Root = ParseBoundedJsonObject(Body);
		const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("data"), Data) || !Data)
		{
			Result.ErrorMessage = TEXT("malformed response: expected a model list");
			return Result;
		}
		Result.Models.Reserve(FMath::Min(Data->Num(), FCrowdyCppClient::MaxStudioModels));
		for (const TSharedPtr<FJsonValue>& Entry : *Data)
		{
			FCrowdyCppStudioModel Model;
			if (Result.Models.Num() < FCrowdyCppClient::MaxStudioModels && ReadModel(Entry, Model))
			{
				Result.Models.Add(MoveTemp(Model));
			}
		}
		Result.bOk = true;
		return Result;
	}

	FString ModelsFailureReason(const FCrowdyCppStudioModelsResult& Result)
	{
		if (!Result.ErrorCode.IsEmpty())
		{
			return Result.ErrorCode;
		}
		if (Result.HttpStatus == 0)
		{
			return TEXT("transport");
		}
		return IsSuccessStatus(Result.HttpStatus) ? FString(TEXT("rejected")) : FString::Printf(TEXT("http %d"), Result.HttpStatus);
	}
}
