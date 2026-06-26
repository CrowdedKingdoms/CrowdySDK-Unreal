// Fill out your copyright notice in the Description page of Project Settings.

#include "Network/GraphQL/FCrowdyGraphQLClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tasks/Task.h"

namespace
{
	TArray<FString> ExtractErrorMessages(const TSharedPtr<FJsonObject>& Envelope)
	{
		TArray<FString> Messages;
		if (!Envelope.IsValid())
		{
			return Messages;
		}

		const TArray<TSharedPtr<FJsonValue>>* ErrorArray = nullptr;
		if (Envelope->TryGetArrayField(TEXT("errors"), ErrorArray))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *ErrorArray)
			{
				const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
				FString Message;
				if (Entry->TryGetObject(ErrorObject) && (*ErrorObject)->TryGetStringField(TEXT("message"), Message))
				{
					Messages.Add(Message);
				}
			}
		}

		return Messages;
	}

	void DeliverOnGameThread(const TFunction<void(FCrowdyGqlResult)>& OnDone, FCrowdyGqlResult&& Result)
	{
		if (!OnDone)
		{
			return;
		}

		UE::Tasks::Launch(UE_SOURCE_LOCATION,
			[OnDone, Result = MoveTemp(Result)]() mutable
			{
				OnDone(MoveTemp(Result));
			},
			LowLevelTasks::ETaskPriority::Normal,
			UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
}

void FCrowdyGraphQLClient::Send(const FCrowdyGqlRequest& Request, TFunction<void(FCrowdyGqlResult)> OnDone)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("query"), Request.Query);
	if (Request.Variables.IsValid())
	{
		Body->SetObjectField(TEXT("variables"), Request.Variables);
	}

	FString BodyString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
	FJsonSerializer::Serialize(Body, Writer);

	const int32 RequestBytes = BodyString.Len();

	const TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Request.Endpoint);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetTimeout(Request.TimeoutSeconds);
	if (!Request.BearerToken.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Request.BearerToken));
	}
	HttpRequest->SetContentAsString(BodyString);

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnDone, RequestBytes](FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			// Parse off the game thread, then hand the result back on it.
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [OnDone, RequestBytes, Response, bWasSuccessful]()
			{
				FCrowdyGqlResult Result;
				Result.RequestBytes = RequestBytes;

				if (bWasSuccessful && Response.IsValid())
				{
					Result.HttpCode = Response->GetResponseCode();
					Result.RawJson  = Response->GetContentAsString();
					Result.bSuccess = Result.HttpCode >= 200 && Result.HttpCode < 300;

					if (!Result.RawJson.IsEmpty())
					{
						const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Result.RawJson);
						FJsonSerializer::Deserialize(Reader, Result.Data);
						Result.Errors = ExtractErrorMessages(Result.Data);
					}
				}

				DeliverOnGameThread(OnDone, MoveTemp(Result));
			}, LowLevelTasks::ETaskPriority::BackgroundNormal);
		});

	if (!HttpRequest->ProcessRequest())
	{
		// The request never left — report it like any other failure (HttpCode stays 0).
		FCrowdyGqlResult Result;
		Result.RequestBytes = RequestBytes;
		DeliverOnGameThread(OnDone, MoveTemp(Result));
	}
}
