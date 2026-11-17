#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyStudioTypes.h"
#include "CrowdyStudioAgent.generated.h"

class FCrowdyCppClient;
class FJsonObject;
class UCrowdyGameSession;
struct FCrowdyCppJsonResult;

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyAgentTokenChanged, int64 /* UserId */, const FString& /* Token */);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnCrowdyAgentBootChanged);

/** Who pays for the agent's model requests, as the app's agent policy decides. Values are only ever added at the end. */
UENUM(BlueprintType)
enum class ECrowdyStudioPayer : uint8
{
	/** The platform has not said. */
	Unknown,
	/** The signed-in player's wallet. */
	Player,
	/** The app's organization's wallet, the platform's ORG. */
	App,
	/** The platform itself. */
	Platform
};

/** One model the agent may use. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioModel
{
	GENERATED_BODY()

	/** The id the model endpoint takes. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString ModelId;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Name;

	/** In tokens; 0 when not given. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 ContextWindow = 0;

	/** Such as "text" and "image". */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	TArray<FString> InputModalities;
};

/** What a page hosting the Studio agent needs to boot it. It holds no token. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioAgentBoot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioResult Result = ECrowdyStudioResult::Failed;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 AppId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString GraphQLUrl;

	/** Where the model endpoint's /v1/model paths are served. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString ModelBaseUrl;

	/** Every origin the agent may connect to; nothing else belongs in the page's connect-src. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	TArray<FString> ConnectOrigins;

	/** The tier's Studio web origin. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString StudioOrigin;

	/** Where the player adds funds, opened in their own browser. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString WalletUrl;

	/** The models the app allows, in the platform's order; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	TArray<FCrowdyStudioModel> Models;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Message;
};

/** The player's provider-data consent for this app. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioConsent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioResult Result = ECrowdyStudioResult::Failed;

	/** Only meaningful when Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	bool bConsented = false;

	/** Unix time in milliseconds; 0 when never consented. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 ConsentedAtMs = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Message;
};

/** Today's metered model use for this app and player. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioModelUsage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioResult Result = ECrowdyStudioResult::Failed;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioPayer Payer = ECrowdyStudioPayer::Unknown;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 TodayRequests = 0;

	/** Millionths of a US dollar. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 TodayChargeMicroUsd = 0;

	/** Today's spend ceiling from the app's effective agent policy, in millionths of a US dollar. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 DayLimitMicroUsd = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Message;
};

/** The app's Allow Live Modding answer for one action. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioLiveModding
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioResult Result = ECrowdyStudioResult::Failed;

	/** The action may go ahead; false unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	bool bAllowed = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Message;
};

/**
 * The Studio agent's session: the boot material, the app token handed to the agent, provider consent, model usage and
 * the app's Allow Live Modding check. This is the one place the agent's credentials leave the SDK; the session token
 * never does. Each OnDone runs once on the game thread, never inside the call, unless the game instance ends first.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyStudioAgent : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The boot material: app id, API addresses, the origins the page may reach, and the app's model list. */
	void GetAgentBoot(TFunction<void(const FCrowdyStudioAgentBoot&)> OnDone);

	/** The app token for the agent, never the session token; C++ only, so no Blueprint graph can read it. Empty while signed out. */
	FString GetAgentToken() const;

	/**
	 * The signed-in user's app token changed; hand Token to the agent. Fires after a token rotation and after each sign-in
	 * (the user may have changed), with an empty Token on sign-out; UserId is the user the token is or was for. C++ only.
	 */
	FOnCrowdyAgentTokenChanged OnAgentTokenChanged;

	/**
	 * The SDK's API origin moved, so the boot material is stale; it carries no secret, so Blueprint may bind it. Nothing signals
	 * a move, so it is noticed when the app token changes and when the boot material is read.
	 */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Studio")
	FOnCrowdyAgentBootChanged OnAgentBootChanged;

	/** Whether the player has consented to provider data use for this app. */
	void GetProviderConsent(TFunction<void(const FCrowdyStudioConsent&)> OnDone);

	/** Records the player's consent, after they were shown Get Consent Notice's text; bConsented false takes it back. */
	void RecordProviderConsent(TFunction<void(const FCrowdyStudioConsent&)> OnDone, bool bConsented = true);

	/** The notice to show before Record Provider Consent: what reaches the model provider, who pays, and where sessions stay. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Studio")
	FText GetConsentNotice() const;

	/** Who pays for the agent's requests, and today's requests, charge and limit. */
	void GetModelUsage(TFunction<void(const FCrowdyStudioModelUsage&)> OnDone);

	/** Asks the app's Allow Live Modding switch whether Action may go ahead. Call it before every Run Project: neither Run Project nor the platform's build and deploy calls check the switch. */
	void AuthorizeLiveModding(ECrowdyStudioRunMode Action, TFunction<void(const FCrowdyStudioLiveModding&)> OnDone);

	/** The player signed out: hands the agent an empty token. Called by the SDK's sign-out. */
	void HandleSignedOut();

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client and reading Session and Auth. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client = nullptr, UCrowdyGameSession* Session = nullptr, UCrowdyAuthentication* Auth = nullptr);

	/** Makes the agent read Endpoint as the client's API endpoint, to stand in for the client moving datacenters; empty goes back to the client's own. */
	void SetApiEndpointForTest(const FString& Endpoint) { ApiEndpointForTest = Endpoint; }
#endif

private:
	UFUNCTION()
	void HandleSignedIn(FCrowdyAuthResult Result);

	UFUNCTION()
	void HandleAppTokenRefreshed();

	void BindAuthentication(UCrowdyAuthentication* Auth);
	void PublishToken();
	void NoteApiOrigin(const FString& Origin);
	void SendAgentOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
		TFunction<void(ECrowdyStudioResult Result, const FString& Message, const FString& Code)> OnFailed);
	void SendConsent(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, const TCHAR* Field, const TCHAR* Call, TOptional<bool> Expected,
		TFunction<void(const FCrowdyStudioConsent&)> OnDone);
	UCrowdyGameSession* ResolveSession() const;
	FCrowdyCppClient* ResolveClient() const;
	FString CurrentApiEndpoint(const FCrowdyCppClient& Client) const;
	int64 GetAppId() const;

	TSharedPtr<uint8> LiveSessionToken;
	TWeakObjectPtr<UCrowdyAuthentication> BoundAuth;
	FString LastApiOrigin;
	int64 LastUserId = 0;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	TWeakObjectPtr<UCrowdyGameSession> SessionForTest;
	FString ApiEndpointForTest;
#endif
};
