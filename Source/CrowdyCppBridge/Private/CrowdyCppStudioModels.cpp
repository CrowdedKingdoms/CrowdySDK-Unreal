#include "CrowdyCppStudioModels.h"

#include "Serialization/CrowdyJsonSafety.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/domains/crowdy_studio_agent.hpp"
#include "crowdy/graphql/http.hpp"
THIRD_PARTY_INCLUDES_END

namespace
{
	constexpr int32 MaxModelIdChars = 256;
	constexpr int32 MaxModalities = 16;
	constexpr int32 MaxModalityChars = 32;
	constexpr int32 MaxErrorMessageChars = 1024;
	constexpr int32 MaxErrorCodeChars = 64;

	FString ModelsUtf8ToFString(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Value.data()), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	// Value when it is at most MaxChars characters, else empty.
	FString ModelsBounded(const std::string& Value, int32 MaxChars)
	{
		FString Text = ModelsUtf8ToFString(Value);
		return Text.Len() <= MaxChars ? Text : FString();
	}

	// False for a model whose id is too long; a context window that is not a token count within int32 reads as 0.
	bool ReadModel(const crowdy::domains::CrowdyStudioModel& Model, FCrowdyCppStudioModel& Out)
	{
		Out.Id = ModelsBounded(Model.id, MaxModelIdChars);
		if (Out.Id.IsEmpty())
		{
			return false;
		}
		Out.Name = ModelsBounded(Model.name, MaxModelIdChars);
		const int64 Tokens = Model.contextWindow.value_or(0);
		Out.ContextWindow = Tokens >= 1 && Tokens <= MAX_int32 ? static_cast<int32>(Tokens) : 0;
		for (const std::string& Modality : Model.inputModalities)
		{
			const FString Text = ModelsBounded(Modality, MaxModalityChars);
			if (!Text.IsEmpty() && Out.InputModalities.Num() < MaxModalities)
			{
				Out.InputModalities.Add(Text);
			}
		}
		const crowdy::domains::CrowdyStudioModelPricing& Pricing = Model.pricingMicrousdPerMillion;
		Out.InputMicroUsdPerMillion = FMath::Max<int64>(0, Pricing.input);
		Out.OutputMicroUsdPerMillion = FMath::Max<int64>(0, Pricing.output);
		Out.ReasoningMicroUsdPerMillion = FMath::Max<int64>(0, Pricing.reasoning);
		Out.CachedInputMicroUsdPerMillion = FMath::Max<int64>(0, Pricing.cachedInput);
		return true;
	}

	// The platform's own refusal body gives the message; a body of any other shape leaves it to the status.
	void ReadRefusal(const crowdy::domains::CrowdyStudioModelError& Error, FCrowdyCppStudioModelsResult& Result)
	{
		Result.bRetryable = Error.retryable;
		Result.ErrorCode = ModelsBounded(Error.code, MaxErrorCodeChars);
		if (!Error.code.empty() || !Error.type.empty())
		{
			Result.ErrorMessage = ModelsUtf8ToFString(Error.message).Left(MaxErrorMessageChars);
		}
	}

	bool IsSuccessStatus(int32 Status)
	{
		return Status >= 200 && Status < 300;
	}

	// The app token travels over https, or over plain http to this machine in a build that is not Shipping.
	bool ModelsOriginMayCarryAToken(const FString& Origin)
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
}

namespace CrowdyCppStudioModels
{
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

	FString ModelsRefusal(const FString& ApiEndpoint, int64 AppId, const FString& AppToken)
	{
		const FString Origin = UrlOrigin(ApiEndpoint);
		if (AppId <= 0)
		{
			return TEXT("no app id to list models for");
		}
		if (Origin.IsEmpty())
		{
			return TEXT("the API endpoint names no origin");
		}
		// CrowdyCPP drops the userinfo, so http://localhost:1@other.host would carry the token to other.host.
		if (Origin.Contains(TEXT("@")))
		{
			return TEXT("the API endpoint names credentials, so the model list was not requested");
		}
		if (!ModelsOriginMayCarryAToken(Origin))
		{
			return TEXT("the API endpoint is not https, so the model list was not requested");
		}
		return AppToken.IsEmpty() ? FString(TEXT("no app token is installed")) : FString();
	}

	std::string ModelsPath(int64 AppId)
	{
		return "/v1/model/models?appId=" + std::to_string(AppId);
	}

	FCrowdyCppStudioModelsResult MapModels(const crowdy::graphql::HttpOutcome& Out)
	{
		FCrowdyCppStudioModelsResult Result;
		if (!Out.status.ok())
		{
			Result.ErrorMessage = Out.errorMessage.empty() ? FString(TEXT("HTTP request failed")) : ModelsUtf8ToFString(Out.errorMessage);
			return Result;
		}
		Result.HttpStatus = Out.response.status;
		if (Out.response.body.size() > static_cast<size_t>(MaxModelsBodyBytes))
		{
			Result.ErrorMessage = TEXT("the model list exceeds the size limit");
			return Result;
		}
		if (!CrowdyJsonSafety::IsNestingWithinLimit(ModelsUtf8ToFString(Out.response.body)))
		{
			Result.ErrorMessage = TEXT("malformed response: the model list is nested too deeply");
			return Result;
		}

		const crowdy::domains::CrowdyStudioModelList List = crowdy::domains::crowdyStudioModelListFrom(Out);
		if (List.error)
		{
			ReadRefusal(*List.error, Result);
		}
		if (!List.ok())
		{
			const FString Fallback = IsSuccessStatus(Result.HttpStatus) ? ModelsUtf8ToFString(List.errorMessage)
				: FString::Printf(TEXT("Server returned HTTP %d"), Result.HttpStatus);
			Result.ErrorMessage = Result.ErrorMessage.IsEmpty() ? Fallback : Result.ErrorMessage;
			return Result;
		}
		Result.Models.Reserve(FMath::Min(static_cast<int32>(List.models.size()), FCrowdyCppClient::MaxStudioModels));
		for (const crowdy::domains::CrowdyStudioModel& Model : List.models)
		{
			FCrowdyCppStudioModel Read;
			if (Result.Models.Num() < FCrowdyCppClient::MaxStudioModels && ReadModel(Model, Read))
			{
				Result.Models.Add(MoveTemp(Read));
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
