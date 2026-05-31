// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Shared/Types/Structures/Versioning/FGameVersion.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Dom/JsonObject.h"
#include "TimerManager.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "CrowdyQuerySubsystem.generated.h"

class FCrowdyDataRegistry;
class FCrowdyQueryParser;
class UGraphQLQueryDatabase;


DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnGraphQLResponse, bool, bSuccess, const FString&, ResponseJson);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTeleportResponse, bool, bTeleportAllowed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGetVersionInfo, FGameVersion, ServerVersion, FGameVersion, ClientVersion);



USTRUCT(BlueprintType)
struct FQueryStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy Query Subsystem")
	int32 QueriesSentPerSecond;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy Query Subsystem")
	int32 QueryBytesSentPerSecond;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy Query Subsystem")
	int32 ResponseBytesReceivedPerSecond;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy Query Subsystem")
	int32 ResponseReceivedPerSecond;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy Query Subsystem")
	bool bIsReceivingData;
};


/**
 * 
 */
UCLASS()
class CROWDYSDK_API UCrowdyQuerySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY(BlueprintReadWrite, Category = "Crowdy Query Subsystem")
	UGraphQLQueryDatabase* QueryDatabase;
	
	UPROPERTY(BlueprintAssignable, Category = "Crowdy Query Subsystem")
	FOnGetVersionInfo OnGetVersionInfo;
	
	UPROPERTY()
	FOnGraphQLResponse OnComplete;
	
	UPROPERTY(BlueprintAssignable, Category = "GraphQL Service")
	FOnTeleportResponse OnTeleport;
	
	void InitializeQuerySubsystem(FCrowdyDataRegistry* InDataRegistry);
	
	void SetAuthToken(const FString& InAuthToken);
	
	void ClearAuthToken();
	
	[[nodiscard]] bool HasAuthToken() const;
	
	void ExecuteQueryByID(const EGraphQLQuery QueryID, const TMap<FString, FString>& RuntimeVariables, const bool bIncludeAuthToken = true, const bool bUseNestedJson = false);

	/**
	 * Like ExecuteQueryByID but uses InlineBody directly instead of looking up
	 * the data asset.  Used for SDK-internal requests with embedded query bodies.
	 */
	void ExecuteQueryWithBody(EGraphQLQuery QueryID, const FString& InlineBody,
	                          const TMap<FString, FString>& RuntimeVariables,
	                          bool bIncludeAuthToken = true);
	
	void SetManagementEndpoint(const FString& InEndpoint);
	void SetGameEndpoint(const FString& InEndpoint);
	
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy Query Subsystem")
	FQueryStats GetQueryStats() const;

private:
	
	FCrowdyQueryParser* QueryParser;
	FCrowdyDataRegistry* DataRegistry;
	
	UPROPERTY()
	FString AuthToken;
	
	UPROPERTY()
	FString ManagementEndpoint;
	
	UPROPERTY()
	FString GameEndpoint;
	
	UPROPERTY()
	FTimerHandle StatsTimerHandle;
	
	std::atomic<int32> QueriesSentPerSecond{0};
	std::atomic<int32> QueryBytesSentPerSecond{0};
	std::atomic<int32> ResponseBytesReceivedPerSecond{0};
	std::atomic<int32> ResponseReceivedPerSecond{0};
	std::atomic<bool> bIsReceivingData{false};
	
	UFUNCTION()
	void UpdateStats();
	
	void ExecuteQuery(EGraphQLQuery QueryID, const FString& Query, const bool bIncludeAuthToken, const TSharedPtr<FJsonObject>& Variables);
	void OnHttpsRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, EGraphQLQuery QueryID);
	void ParseAndDispatchToServices(const FString& ResponseContent, EGraphQLQuery QueryID) const;

	/** Creates an invalid response for QueryID and dispatches it through DataRegistry
	 *  so that the correct per-query failure delegate fires on the game thread. */
	void DispatchFailedResponse(EGraphQLQuery QueryID, const FString& ErrorMsg) const;
};
