// Fill out your copyright notice in the Description page of Project Settings.

#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "HttpModule.h"
#include "Core/GraphQL/DataAssets/GraphQLQueryDatabase.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Interfaces/IHttpResponse.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Network/GraphQL/FCrowdyQueryDescriptor.h"
#include "Network/GraphQL/FCrowdyResponseFactory.h"
#include "Queries/UDP/FUDPAddressNotify.h"
#include "Serialization/FCrowdyQueryParser.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Tasks/Task.h"
#include "Async/Async.h"
#include "Engine/World.h"

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void UCrowdyQuerySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyQuerySubsystem::Deinitialize()
{
	ClearAuthToken();
	delete QueryParser;
	QueryParser = nullptr;
	GetWorld()->GetTimerManager().ClearTimer(StatsTimerHandle);
	Super::Deinitialize();
}

void UCrowdyQuerySubsystem::InitializeQuerySubsystem(FCrowdyDataRegistry* InDataRegistry)
{
	DataRegistry = InDataRegistry;

	QueryDatabase = LoadObject<UGraphQLQueryDatabase>(nullptr, TEXT("/CrowdySDK/Data/DA_CrowdyQueries.DA_CrowdyQueries"));
	ensure(QueryDatabase);

	if (!QueryDatabase)
	{
		UE_LOG(LogTemp, Fatal, TEXT("[CrowdySDK] Failed to load GraphQL query database"));
	}

	QueryParser = new FCrowdyQueryParser();

	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Query Subsystem Initialized."))

	GetWorld()->GetTimerManager().SetTimer(
		StatsTimerHandle, this, &UCrowdyQuerySubsystem::UpdateStats, 1.0f, true);
}

// ─── Auth ─────────────────────────────────────────────────────────────────────

void UCrowdyQuerySubsystem::SetAuthToken(const FString& InAuthToken)
{
	AuthToken = InAuthToken;
}

void UCrowdyQuerySubsystem::ClearAuthToken()
{
	AuthToken.Empty();
}

bool UCrowdyQuerySubsystem::HasAuthToken() const
{
	return !AuthToken.IsEmpty();
}

// ─── Endpoint configuration ───────────────────────────────────────────────────

void UCrowdyQuerySubsystem::SetManagementEndpoint(const FString& InEndpoint)
{
	ManagementEndpoint = InEndpoint;
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Management Endpoint set to %s"), *InEndpoint);
}

void UCrowdyQuerySubsystem::SetGameEndpoint(const FString& InEndpoint)
{
	GameEndpoint = InEndpoint;
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Game Endpoint set to %s"), *InEndpoint);
}

// ─── Stats ────────────────────────────────────────────────────────────────────

FQueryStats UCrowdyQuerySubsystem::GetQueryStats() const
{
	FQueryStats Stats;
	Stats.bIsReceivingData               = bIsReceivingData.load();
	Stats.QueriesSentPerSecond           = QueriesSentPerSecond.load();
	Stats.QueryBytesSentPerSecond        = QueryBytesSentPerSecond.load();
	Stats.ResponseBytesReceivedPerSecond = ResponseBytesReceivedPerSecond.load();
	Stats.ResponseReceivedPerSecond      = ResponseReceivedPerSecond.load();
	return Stats;
}

void UCrowdyQuerySubsystem::UpdateStats()
{
	QueriesSentPerSecond.store(0, std::memory_order_relaxed);
	QueryBytesSentPerSecond.store(0, std::memory_order_relaxed);
	ResponseBytesReceivedPerSecond.store(0, std::memory_order_relaxed);
	ResponseReceivedPerSecond.store(0, std::memory_order_relaxed);
	bIsReceivingData.store(false);
}

// ─── Query execution ──────────────────────────────────────────────────────────

void UCrowdyQuerySubsystem::ExecuteQueryByID(const EGraphQLQuery QueryID,
                                              const TMap<FString, FString>& RuntimeVariables,
                                              const bool bIncludeAuthToken, const bool bUseNestedJson)
{
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, QueryID, RuntimeVariables, bIncludeAuthToken, bUseNestedJson]()
	{
		if (!IsValid(QueryDatabase))
		{
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
			{
				OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"Internal error: query database missing\"}"));
			}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
			return;
		}

		FGraphQLQueryDef QueryDef;
		if (!QueryDatabase->GetQueryByID(QueryID, QueryDef))
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: Query not found for ID %d"),
				static_cast<int32>(QueryID));
			UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
			{
				OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"Query not found\"}"));
			}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
			return;
		}

		// Merge default variables from the data asset with caller-supplied runtime values
		const TSharedPtr<FJsonObject> FinalVariables = MakeShareable(new FJsonObject);

		for (const auto& Pair : QueryDef.DefaultVariables)
		{
			FinalVariables->SetStringField(Pair.Key, Pair.Value);
		}

		if (!bUseNestedJson)
		{
			for (const auto& Pair : RuntimeVariables)
			{
				FinalVariables->SetStringField(Pair.Key, Pair.Value);
			}
		}
		else
		{
			// Dot-notation key expansion: "input.email" -> { input: { email: ... } }
			for (const auto& Pair : RuntimeVariables)
			{
				const FString& DotKey = Pair.Key;
				const FString& Value  = Pair.Value;

				TArray<FString> Keys;
				DotKey.ParseIntoArray(Keys, TEXT("."));

				TSharedPtr<FJsonObject> Current = FinalVariables;

				for (int32 i = 0; i < Keys.Num(); ++i)
				{
					const FString& Key = Keys[i];

					if (i == Keys.Num() - 1)
					{
						if (Value.IsNumeric() && !Value.Contains(TEXT(".")))
						{
							Current->SetNumberField(Key, static_cast<double>(FCString::Atoi64(*Value)));
						}
						else if (Value.IsNumeric())
						{
							Current->SetNumberField(Key, FCString::Atod(*Value));
						}
						else
						{
							Current->SetStringField(Key, Value);
						}
					}
					else
					{
						TSharedPtr<FJsonObject> Next;
						const TSharedPtr<FJsonObject>* ExistingPtr = nullptr;
						if (Current->TryGetObjectField(Key, ExistingPtr) && ExistingPtr && ExistingPtr->IsValid())
						{
							Next = *ExistingPtr;
						}
						else
						{
							Next = MakeShared<FJsonObject>();
							Current->SetObjectField(Key, Next);
						}
						Current = Next;
					}
				}
			}
		}

		ExecuteQuery(QueryID, QueryDef.QueryBody, bIncludeAuthToken, FinalVariables);

	}, LowLevelTasks::ETaskPriority::BackgroundNormal);
}

void UCrowdyQuerySubsystem::ExecuteQueryWithBody(EGraphQLQuery QueryID, const FString& InlineBody,
                                                  const TMap<FString, FString>& RuntimeVariables,
                                                  bool bIncludeAuthToken)
{
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[this, QueryID, InlineBody, RuntimeVariables, bIncludeAuthToken]()
		{
			const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
			for (const auto& Pair : RuntimeVariables)
				Vars->SetStringField(Pair.Key, Pair.Value);

			ExecuteQuery(QueryID, InlineBody, bIncludeAuthToken, Vars);
		}, LowLevelTasks::ETaskPriority::BackgroundNormal);
}

void UCrowdyQuerySubsystem::ExecuteQuery(EGraphQLQuery QueryID, const FString& Query,
                                          const bool bIncludeAuthToken, const TSharedPtr<FJsonObject>& Variables)
{
	if (Query.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: Empty query body for query ID %d"),
			static_cast<int32>(QueryID));
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
		{
			OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"Empty query provided\"}"));
		}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		return;
	}

	FHttpModule* Http = &FHttpModule::Get();
	if (!Http)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: HTTP module unavailable"));
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
		{
			OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"HTTP module not available\"}"));
		}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		return;
	}

	// ── Descriptor-driven routing ────────────────────────────────────────────
	// Per-query endpoint, timeout, and auth requirements all come from the
	// descriptor table in FCrowdyQueryDescriptor.cpp. No hard-coded values here.

	const FCrowdyQueryDescriptor* Desc        = FCrowdyQueryDescriptors::Find(QueryID);
	const bool                    bIsManagement  = Desc ? Desc->ApiTarget == ECrowdyApiTarget::Management : false;
	const float                   TimeoutSeconds = Desc ? Desc->TimeoutSeconds : 10.f;

	FString ResolvedEndpoint;
	if (bIsManagement)
	{
		// Management URL has no /graphql suffix per the SDK spec — we append it here
		ResolvedEndpoint = ManagementEndpoint.IsEmpty()
			? TEXT("https://api.dev.crowdedkingdoms.com/graphql")
			: ManagementEndpoint + TEXT("/graphql");
	}
	else
	{
		// Game URL already includes /graphql
		ResolvedEndpoint = GameEndpoint.IsEmpty()
			? TEXT("https://game.dev1.dev.cks-env.com/graphql")
			: GameEndpoint;
	}

	const TSharedRef<IHttpRequest> Request = Http->CreateRequest();
	Request->SetURL(ResolvedEndpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(TimeoutSeconds);

	if (bIncludeAuthToken && HasAuthToken())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
	}
	else if (bIncludeAuthToken && !HasAuthToken())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK] GraphQL: Auth requested for query %d but no token is set"),
			static_cast<int32>(QueryID));
	}

	// Serialize query + variables to JSON
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
	JsonObject->SetStringField(TEXT("query"), Query);
	if (Variables.IsValid())
	{
		JsonObject->SetObjectField(TEXT("variables"), Variables);
	}

	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	Request->SetContentAsString(OutputString);

	// ── Bind completion: capture QueryID by value in the lambda so the response
	//    handler always knows which query produced this response.
	//    This eliminates JSON field sniffing in ParseAndDispatchToServices.
	TWeakObjectPtr<UCrowdyQuerySubsystem> WeakThis(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, QueryID](FHttpRequestPtr Req, FHttpResponsePtr Res, bool bWasSuccessful)
		{
			if (UCrowdyQuerySubsystem* Self = WeakThis.Get())
				Self->OnHttpsRequestComplete(Req, Res, bWasSuccessful, QueryID);
		});

	if (!Request->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: Failed to send request for query %d"),
			static_cast<int32>(QueryID));
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"Failed to process HTTP request\"}"));
		});
		return;
	}

	// ── Stats: measure the actual query body bytes, not just the HTTP verb
	QueriesSentPerSecond.fetch_add(1, std::memory_order_relaxed);
	QueryBytesSentPerSecond.fetch_add(OutputString.Len(), std::memory_order_relaxed);
}

// ─── Response handling ────────────────────────────────────────────────────────

void UCrowdyQuerySubsystem::OnHttpsRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response,
                                                    bool bWasSuccessful, EGraphQLQuery QueryID)
{
	TWeakObjectPtr<UCrowdyQuerySubsystem> WeakThis = this;

	UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, bWasSuccessful, Response, QueryID]()
	{
		if (!WeakThis.IsValid())
			return;

		if (!bWasSuccessful || !Response.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: HTTP request failed for query %d"),
				static_cast<int32>(QueryID));
			const FString ErrMsg = TEXT("HTTP request failed");
			WeakThis->DispatchFailedResponse(QueryID, ErrMsg);
			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				WeakThis->OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"HTTP request failed\"}"));
			});
			return;
		}

		const int32 ResponseCode    = Response->GetResponseCode();
		FString     ResponseContent = Response->GetContentAsString();

		if (!WeakThis.IsValid())
			return;

		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: Server returned %d for query %d"),
				ResponseCode, static_cast<int32>(QueryID));
			const FString ErrMsg = FString::Printf(TEXT("Server returned HTTP %d"), ResponseCode);
			WeakThis->DispatchFailedResponse(QueryID, ErrMsg);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, ResponseContent]()
			{
				WeakThis->OnComplete.ExecuteIfBound(false, ResponseContent);
			});
			return;
		}

		WeakThis->ResponseReceivedPerSecond.fetch_add(1, std::memory_order_relaxed);
		WeakThis->ResponseBytesReceivedPerSecond.store(
			WeakThis->ResponseBytesReceivedPerSecond.load(std::memory_order_relaxed) + ResponseContent.Len(),
			std::memory_order_relaxed);
		WeakThis->bIsReceivingData.store(true, std::memory_order_relaxed);

		WeakThis->ParseAndDispatchToServices(ResponseContent, QueryID);

		// Also fire the raw-JSON delegate for callers that still want unprocessed data
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ResponseContent]()
		{
			WeakThis->OnComplete.ExecuteIfBound(true, ResponseContent);
		});

	}, LowLevelTasks::ETaskPriority::BackgroundNormal);
}

// ─── Parse & dispatch ─────────────────────────────────────────────────────────
//
// Previous implementation: ~240 lines of duplicated switch statements that had
// to be kept in sync manually for every query type (both success and error paths).
//
// This implementation: descriptor lookup + factory + single ParseResponse call.
// Adding a new query type no longer requires touching this function at all.

void UCrowdyQuerySubsystem::ParseAndDispatchToServices(const FString& ResponseContent, EGraphQLQuery QueryID) const
{
	// 1. Deserialize JSON
	TSharedPtr<FJsonObject> ParsedData;
	const TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(ResponseContent);

	if (!FJsonSerializer::Deserialize(JsonReader, ParsedData) || !ParsedData.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL: JSON parse failure for query %d"),
			static_cast<int32>(QueryID));
		return;
	}

	// 2. QueryID -> ResponseType via descriptor table (no field sniffing)
	const EQueryResponseType ResponseType = FCrowdyQueryDescriptors::GetResponseType(QueryID);

	// 3. Construct the correct response object via factory (no switch statement)
	TSharedPtr<ICrowdyQueryResponse> Response = FCrowdyResponseFactory::Get().Create(ResponseType);

	if (!Response.IsValid())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CrowdySDK] ParseAndDispatch: No factory entry for query %d (response type %d). "
			     "Add a Register() line in FCrowdyResponseFactory::RegisterAll()."),
			static_cast<int32>(QueryID), static_cast<int32>(ResponseType));
		return;
	}

	// 4. Handle GraphQL-level errors
	if (QueryParser->HasErrors(ParsedData))
	{
		const TArray<FString> Errors = QueryParser->GetErrorMessages(ParsedData);

		for (int32 i = 0; i < Errors.Num(); ++i)
		{
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK] GraphQL Error [%d/%d] for query %d: %s"),
				i + 1, Errors.Num(), static_cast<int32>(QueryID), *Errors[i]);
		}

		if (!Errors.IsEmpty())
		{
			// Special case: UDP gate-keep ("Insufficient permissions") is a valid
			// transient state, not a hard failure. Setting bGateKeep lets the
			// response parse as valid so the subsystem can broadcast correctly.
			if (ResponseType == EQueryResponseType::UDP_Info
				&& Errors[0].Contains(TEXT("Insufficient"), ESearchCase::IgnoreCase))
			{
				FUDPAddressNotify& UDPResponse = static_cast<FUDPAddressNotify&>(*Response);
				UDPResponse.bGateKeep = true;
				UDPResponse.ParseResponse(ParsedData); // returns early due to bGateKeep flag
				DataRegistry->DispatchResponse(Response);
				return;
			}

			Response->ErrorMessage = Errors[0];
		}

		DataRegistry->DispatchResponse(Response);
		return;
	}

	// 5. Success path
	Response->ParseResponse(ParsedData);
	DataRegistry->DispatchResponse(Response);
}

void UCrowdyQuerySubsystem::DispatchFailedResponse(EGraphQLQuery QueryID, const FString& ErrorMsg) const
{
	if (!DataRegistry)
		return;

	const EQueryResponseType ResponseType = FCrowdyQueryDescriptors::GetResponseType(QueryID);
	TSharedPtr<ICrowdyQueryResponse> Response = FCrowdyResponseFactory::Get().Create(ResponseType);
	if (!Response.IsValid())
		return;

	Response->MarkInvalid(ErrorMsg);
	DataRegistry->DispatchResponse(Response);
}
