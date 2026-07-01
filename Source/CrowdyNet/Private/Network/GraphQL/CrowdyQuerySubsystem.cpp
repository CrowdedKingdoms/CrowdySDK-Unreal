// Fill out your copyright notice in the Description page of Project Settings.

#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "CrowdyNetLog.h"
#include "Core/GraphQL/DataAssets/GraphQLQueryDatabase.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "Network/GraphQL/FCrowdyGraphQLClient.h"
#include "Network/GraphQL/FCrowdyQueryDescriptor.h"
#include "Network/GraphQL/FCrowdyResponseFactory.h"
#include "Queries/UDP/FUDPAddressNotify.h"
#include "Serialization/FCrowdyQueryParser.h"
#include "Tasks/Task.h"
#include "Engine/World.h"

// Lifecycle

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
		UE_LOG(LogCrowdyNet, Fatal, TEXT("Failed to load GraphQL query database"));
	}

	QueryParser = new FCrowdyQueryParser();

	UE_CLOG(CrowdyNetTrace::Query(), LogCrowdyNet, Log, TEXT("Query Subsystem Initialized."))

	GetWorld()->GetTimerManager().SetTimer(
		StatsTimerHandle, this, &UCrowdyQuerySubsystem::UpdateStats, 1.0f, true);
}

// Auth
void UCrowdyQuerySubsystem::ClearAuthToken()
{
	SessionToken.Empty();
	AppToken.Empty();
}

bool UCrowdyQuerySubsystem::HasAuthToken() const
{
	return !SessionToken.IsEmpty() || !AppToken.IsEmpty();
}

void UCrowdyQuerySubsystem::SetSessionToken(const FString& InSessionToken)
{
	SessionToken = InSessionToken;
}

void UCrowdyQuerySubsystem::ClearSessionToken()
{
	SessionToken.Empty();
}

bool UCrowdyQuerySubsystem::HasSessionToken() const
{
	return !SessionToken.IsEmpty();
}

void UCrowdyQuerySubsystem::SetAppToken(const FString& InAppToken)
{
	AppToken = InAppToken;
}

void UCrowdyQuerySubsystem::ClearAppToken()
{
	AppToken.Empty();
}

bool UCrowdyQuerySubsystem::HasAppToken() const
{
	return !AppToken.IsEmpty();
}

// Endpoint config

void UCrowdyQuerySubsystem::SetManagementEndpoint(const FString& InEndpoint)
{
	ManagementEndpoint = InEndpoint;
	UE_CLOG(CrowdyNetTrace::Query(), LogCrowdyNet, Log, TEXT("Management Endpoint set to %s"), *InEndpoint);
}

void UCrowdyQuerySubsystem::SetGameEndpoint(const FString& InEndpoint)
{
	GameEndpoint = InEndpoint;
	UE_CLOG(CrowdyNetTrace::Query(), LogCrowdyNet, Log, TEXT("Game Endpoint set to %s"), *InEndpoint);
}

//Stats

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

// Query Execution
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
			UE_LOG(LogCrowdyNet, Error, TEXT("GraphQL: Query not found for ID %d"),
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

void UCrowdyQuerySubsystem::ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery QueryID, const FString& InlineBody,
                                                             const TSharedPtr<FJsonObject>& Variables,
                                                             bool bIncludeAuthToken)
{
	TSharedPtr<FJsonObject> VarsCopy = Variables.IsValid() ? Variables : MakeShared<FJsonObject>();
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[this, QueryID, InlineBody, VarsCopy, bIncludeAuthToken]()
		{
			ExecuteQuery(QueryID, InlineBody, bIncludeAuthToken, VarsCopy);
		}, LowLevelTasks::ETaskPriority::BackgroundNormal);
}

void UCrowdyQuerySubsystem::ExecuteQuery(EGraphQLQuery QueryID, const FString& Query,
                                          const bool bIncludeAuthToken, const TSharedPtr<FJsonObject>& Variables)
{
	if (Query.IsEmpty())
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("GraphQL: Empty query body for query ID %d"),
			static_cast<int32>(QueryID));
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this]()
		{
			OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"Empty query provided\"}"));
		}, LowLevelTasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		return;
	}

	// Descriptor-driven routing
	// Per-query endpoint, timeout, and auth requirements all come from the
	// descriptor table in FCrowdyQueryDescriptor.cpp. No hard-coded values here.

	const FCrowdyQueryDescriptor* Desc        = FCrowdyQueryDescriptors::Find(QueryID);
	const bool                    bIsManagement  = Desc ? Desc->ApiTarget == ECrowdyApiTarget::Management : false;
	const float                   TimeoutSeconds = Desc ? Desc->TimeoutSeconds : 10.f;

	FCrowdyGqlRequest Request;
	Request.Query          = Query;
	Request.Variables      = Variables;
	Request.TimeoutSeconds = TimeoutSeconds;

	if (bIsManagement)
	{
		// Management URL has no /graphql suffix per the SDK spec — we append it here
		Request.Endpoint = ManagementEndpoint.IsEmpty()
			? TEXT("https://api.dev.crowdedkingdoms.com/graphql")
			: ManagementEndpoint + TEXT("/graphql");
	}
	else
	{
		// Game URL already includes /graphql
		Request.Endpoint = GameEndpoint.IsEmpty()
			? TEXT("https://game.dev1.dev.cks-env.com/graphql")
			: GameEndpoint;
	}

	// Token-plane selection: Management-plane queries authenticate with the
	// identity SESSION token; gameplay queries with the app-scoped token. The
	// descriptor pins the scope (Auto resolves by ApiTarget; refreshAppToken is
	// explicitly App). This is the only enforcement that keeps a session token
	// off the Game API / UDP path.
	if (bIncludeAuthToken)
	{
		const ECrowdyTokenScope Scope = FCrowdyQueryDescriptors::GetTokenScope(QueryID);

		// Fail closed: only an explicit Session/App scope attaches a bearer. None
		// is a public mutation; Auto (returned only when the descriptor is missing)
		// and any unknown scope send no token rather than guessing one.
		const FString* Bearer = nullptr;
		switch (Scope)
		{
		case ECrowdyTokenScope::Session: Bearer = &SessionToken; break;
		case ECrowdyTokenScope::App:     Bearer = &AppToken;     break;
		case ECrowdyTokenScope::None:    break;
		default:
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("GraphQL: unresolved token scope for query %d; sending no bearer"),
				static_cast<int32>(QueryID));
			break;
		}

		if (Bearer && !Bearer->IsEmpty())
	{
			Request.BearerToken = *Bearer;
	}
		else if (Bearer)
	{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("GraphQL: %s token required for query %d but none is set"),
				Scope == ECrowdyTokenScope::Session ? TEXT("session") : TEXT("app"),
			static_cast<int32>(QueryID));
	}
	}

	// The shared client owns the HTTP POST, JSON serialize, and GraphQL-error parsing.
	// QueryID rides the lambda capture so the response always knows which query it answers.
	TWeakObjectPtr<UCrowdyQuerySubsystem> WeakThis(this);
	FCrowdyGraphQLClient::Send(Request, [WeakThis, QueryID](FCrowdyGqlResult Result)
	{
		UCrowdyQuerySubsystem* Self = WeakThis.Get();
		if (!Self)
		{
		return;
	}

		Self->QueriesSentPerSecond.fetch_add(1, std::memory_order_relaxed);
		Self->QueryBytesSentPerSecond.fetch_add(Result.RequestBytes, std::memory_order_relaxed);

		if (Result.HttpCode == 0)
		{
			UE_LOG(LogCrowdyNet, Error, TEXT("GraphQL: HTTP request failed for query %d"),
				static_cast<int32>(QueryID));
			Self->DispatchFailedResponse(QueryID, TEXT("HTTP request failed"));
			Self->OnComplete.ExecuteIfBound(false, TEXT("{\"error\": \"HTTP request failed\"}"));
			return;
		}

		if (!Result.bSuccess)
		{
			UE_LOG(LogCrowdyNet, Error, TEXT("GraphQL: Server returned %d for query %d"),
				Result.HttpCode, static_cast<int32>(QueryID));
			Self->DispatchFailedResponse(QueryID, FString::Printf(TEXT("Server returned HTTP %d"), Result.HttpCode));
			Self->OnComplete.ExecuteIfBound(false, Result.RawJson);
			return;
		}

		Self->ResponseReceivedPerSecond.fetch_add(1, std::memory_order_relaxed);
		Self->ResponseBytesReceivedPerSecond.fetch_add(Result.RawJson.Len(), std::memory_order_relaxed);
		Self->bIsReceivingData.store(true, std::memory_order_relaxed);

		Self->ParseAndDispatchToServices(Result.Data, QueryID);

		// Also fire the raw-JSON delegate for callers that still want unprocessed data
		Self->OnComplete.ExecuteIfBound(true, Result.RawJson);
		});
}

// Parse & dispatch
// Previous implementation: ~240 lines of duplicated switch statements that had
// to be kept in sync manually for every query type (both success and error paths).

// This implementation: descriptor lookup + factory + single ParseResponse call.
// Adding a new query type no longer requires touching this function at all.

void UCrowdyQuerySubsystem::ParseAndDispatchToServices(const TSharedPtr<FJsonObject>& ParsedData, EGraphQLQuery QueryID) const
{
	// 1. The client already deserialized the body; a null here means it failed to parse.
	if (!ParsedData.IsValid())
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("GraphQL: JSON parse failure for query %d"),
			static_cast<int32>(QueryID));
		return;
	}

	// 2. QueryID -> ResponseType via descriptor table (no field sniffing)
	const EQueryResponseType ResponseType = FCrowdyQueryDescriptors::GetResponseType(QueryID);

	// 3. Construct the correct response object via factory (no switch statement)
	TSharedPtr<ICrowdyQueryResponse> Response = FCrowdyResponseFactory::Get().Create(ResponseType);

	if (!Response.IsValid())
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("ParseAndDispatch: No factory entry for query %d (response type %d). "
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
			UE_LOG(LogCrowdyNet, Error, TEXT("GraphQL Error [%d/%d] for query %d: %s"),
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

			Response->MarkInvalid(Errors[0]);
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
