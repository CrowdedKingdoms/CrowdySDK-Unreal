#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyStudioAgent.h"
#include "Subsystem/CrowdyStudioProjects.h"
#include "CrowdyStudioActions.generated.h"

// One signature serves each node's pins, because a Blueprint async node builds its pins from its delegates.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyStudioBootAsyncResult, const FCrowdyStudioAgentBoot&, Boot, ECrowdyStudioResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FCrowdyStudioConsentAsyncResult, bool, bConsented, int64, ConsentedAtMs, ECrowdyStudioResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyStudioUsageAsyncResult, const FCrowdyStudioModelUsage&, Usage, ECrowdyStudioResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyStudioLiveModdingAsyncResult, bool, bAllowed, ECrowdyStudioResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyStudioProjectAsyncResult, const FCrowdyStudioProject&, Project, ECrowdyStudioResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FCrowdyStudioProjectListAsyncResult, const TArray<FCrowdyStudioProject>&, Projects, bool, bMore,
	ECrowdyStudioResult, Result, const FString&, Message);

/** The Studio agent's boot material. C++ callers use UCrowdyStudioAgent::GetAgentBoot. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetAgentBoot : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioBootAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioBootAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Agent Boot"), Category="Crowdy SDK|Studio")
	static UCrowdyGetAgentBoot* GetAgentBoot(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	void Finish(const FCrowdyStudioAgentBoot& Boot);
};

/** Whether the player consented to provider data use. C++ callers use UCrowdyStudioAgent::GetProviderConsent. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetProviderConsent : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioConsentAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioConsentAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Provider Consent"), Category="Crowdy SDK|Studio")
	static UCrowdyGetProviderConsent* GetProviderConsent(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	void Finish(const FCrowdyStudioConsent& Consent);
};

/** Records the player's provider-data consent. C++ callers use UCrowdyStudioAgent::RecordProviderConsent. */
UCLASS()
class CROWDYSERVICES_API UCrowdyRecordProviderConsent : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioConsentAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioConsentAsyncResult OnFailed;

	/** Show Get Consent Notice's text first. Consented false takes the consent back. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Record Provider Consent"), Category="Crowdy SDK|Studio")
	static UCrowdyRecordProviderConsent* RecordProviderConsent(UObject* WorldContextObject, bool bConsented = true);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	bool bConsented = true;

	void Finish(const FCrowdyStudioConsent& Consent);
};

/** Who pays for the agent and today's use. C++ callers use UCrowdyStudioAgent::GetModelUsage. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetModelUsage : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioUsageAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioUsageAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Model Usage"), Category="Crowdy SDK|Studio")
	static UCrowdyGetModelUsage* GetModelUsage(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	void Finish(const FCrowdyStudioModelUsage& Usage);
};

/** Asks the app's Allow Live Modding switch. C++ callers use UCrowdyStudioAgent::AuthorizeLiveModding. */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuthorizeLiveModding : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The switch answered; Allowed says whether Action may go ahead. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioLiveModdingAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioLiveModdingAsyncResult OnFailed;

	/** Asks the app's Allow Live Modding switch before a run. Call it before every Run Project: neither Run Project nor the platform's build and deploy calls check the switch. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Authorize Live Modding"), Category="Crowdy SDK|Studio")
	static UCrowdyAuthorizeLiveModding* AuthorizeLiveModding(UObject* WorldContextObject, ECrowdyStudioRunMode Action);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	ECrowdyStudioRunMode Action = ECrowdyStudioRunMode::DraftTest;

	void Finish(const FCrowdyStudioLiveModding& LiveModding);
};

/** The player's Studio projects, newest first, up to 50; More says the platform holds others. C++ callers use UCrowdyStudioProjects::ListProjects. */
UCLASS()
class CROWDYSERVICES_API UCrowdyListStudioProjects : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioProjectListAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioProjectListAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="List Studio Projects"), Category="Crowdy SDK|Studio")
	static UCrowdyListStudioProjects* ListProjects(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	void Finish(const FCrowdyStudioProjectList& List);
};

/** One Studio project, read fresh. C++ callers use UCrowdyStudioProjects::GetProject. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetStudioProject : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioProjectAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioProjectAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Studio Project"), Category="Crowdy SDK|Studio")
	static UCrowdyGetStudioProject* GetProject(UObject* WorldContextObject, const FString& ProjectId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ProjectId;

	void Finish(const FCrowdyStudioProjectInfo& Info);
};

/** A new Studio project, filled with the starter for each half it has. C++ callers use UCrowdyStudioProjects::CreateProject. */
UCLASS()
class CROWDYSERVICES_API UCrowdyCreateStudioProject : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioProjectAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyStudioProjectAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Create Studio Project"), Category="Crowdy SDK|Studio")
	static UCrowdyCreateStudioProject* CreateProject(UObject* WorldContextObject, const FString& Name, ECrowdyStudioProjectKind Kind);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString Name;
	ECrowdyStudioProjectKind Kind = ECrowdyStudioProjectKind::Server;

	void Finish(const FCrowdyStudioProjectInfo& Info);
};
