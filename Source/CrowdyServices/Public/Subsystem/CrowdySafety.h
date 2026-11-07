#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdySafety.generated.h"

class FCrowdyCppClient;

/** Who a player's voice or video reaches, or who reaches them. */
UENUM(BlueprintType)
enum class ECrowdyCommunicationScope : uint8
{
	Off,
	Contacts,
	Everyone
};

/** One of a player's four voice and video settings. */
UENUM(BlueprintType)
enum class ECrowdyCommunicationSetting : uint8
{
	VoiceSend,
	VoiceReceive,
	VideoSend,
	VideoReceive
};

/** What a parent can consent to. */
UENUM(BlueprintType)
enum class ECrowdyConsentFeature : uint8
{
	Voice,
	Video
};

/** What a request for a parent's consent did. Only Requested sent an email. */
UENUM(BlueprintType)
enum class ECrowdyConsentStatus : uint8
{
	Requested,
	AlreadyGranted,
	/** The player is 18 or over. */
	NotRequired
};

UENUM(BlueprintType)
enum class ECrowdySafetyReportReason : uint8
{
	ChildSafety,
	Harassment,
	SexualContent,
	Hate,
	SelfHarm,
	Cheating,
	Other
};

UENUM(BlueprintType)
enum class ECrowdySafetyErrorCode : uint8
{
	None,
	NetworkError,
	ServerError,
	/** The deployment has no parental consent service, or it refused or did not answer. */
	ConsentUnavailable,
	/** 100 waiting requests, 1,000 contacts or 1,000 blocks in one app. */
	LimitReached,
	RateLimited
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdySafetyError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdySafetyErrorCode Code = ECrowdySafetyErrorCode::None;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	FString Message;

	/** Why consent cannot be asked, on ConsentUnavailable: not_configured, provider_refused or provider_unavailable. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	FString Reason;

	static FCrowdySafetyError FromFailure(const FString& Message, const FString& ServerCode);
};

/** What a player chose. Show these on a settings screen; policy may keep what applies below them. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyCommunicationChoices
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VoiceSend = ECrowdyCommunicationScope::Everyone;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VoiceReceive = ECrowdyCommunicationScope::Everyone;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VideoSend = ECrowdyCommunicationScope::Everyone;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VideoReceive = ECrowdyCommunicationScope::Everyone;
};

/** A player's voice and video settings in this app. The four scopes are the ones the realtime servers apply now. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyCommunicationSettings
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bIsMinor = false;

	/** Do not offer a microphone while this is Off: its sends are refused. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VoiceSend = ECrowdyCommunicationScope::Off;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VoiceReceive = ECrowdyCommunicationScope::Off;

	/** Do not offer a camera while this is Off: its sends are refused. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VideoSend = ECrowdyCommunicationScope::Off;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VideoReceive = ECrowdyCommunicationScope::Off;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	FCrowdyCommunicationChoices Choices;

	/** The most voice may be now. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VoiceAllowed = ECrowdyCommunicationScope::Off;

	/** The most video may be now. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	ECrowdyCommunicationScope VideoAllowed = ECrowdyCommunicationScope::Off;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bVoiceConsent = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bVideoConsent = false;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyCommunicationSettings& Out);
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyPlayerContact
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	int64 UserId = 0;

	/** False until the other player accepts. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bAccepted = false;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyPlayerContact& Out);
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyContactRequest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	int64 UserId = 0;

	/** True when the other player asked this player. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bIncoming = false;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyContactRequest& Out);
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyPlayerBlock
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	int64 UserId = 0;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyPlayerBlock& Out);
};

/** What this app's safety log records now. While Voice or Video is true your game must tell players and show it. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyAppRecording
{
	GENERATED_BODY()

	/** False until a read has landed. Nothing below is known until then, so do not assume the app is not recording. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bKnown = false;

	/** The safety log itself, which always includes text. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bEnabled = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bVoice = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Safety")
	bool bVideo = false;

	bool operator==(const FCrowdyAppRecording& Other) const
	{
		return bKnown == Other.bKnown && bEnabled == Other.bEnabled && bVoice == Other.bVoice && bVideo == Other.bVideo;
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Json, FCrowdyAppRecording& Out);
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRecordingStateChanged, const FCrowdyAppRecording&, Recording);

DECLARE_DYNAMIC_DELEGATE(FOnSafetyDone);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnSafetyError, const FCrowdySafetyError&, Error);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnCommunicationSettings, const FCrowdyCommunicationSettings&, Settings);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnConsentRequested, ECrowdyConsentStatus, Status);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnPlayerContact, const FCrowdyPlayerContact&, Contact);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnPlayerContacts, const TArray<FCrowdyPlayerContact>&, Contacts);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnContactRequests, const TArray<FCrowdyContactRequest>&, Requests);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnPlayerBlocks, const TArray<FCrowdyPlayerBlock>&, Blocks);

/**
 * A player's own safety controls in the current app: who hears and sees them, contacts, blocks, a parent's consent and
 * reports. The realtime servers enforce these for what players send each other, and a change reaches live sessions
 * within a second. What your app's own server code sends is not filtered, so check blocks before relaying a player's
 * words from a hub. Needs the player signed in to the app. Keep Block and Report beside each other in your UI.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdySafety : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Fires on the first read and when what the app records changes. Read when the player connects and when their token refreshes. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Safety")
	FOnRecordingStateChanged OnRecordingStateChanged;

	/** What the app records, as last read. Check it when you bind, since the first read may already have landed. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Safety")
	FCrowdyAppRecording GetRecordingState() const { return Recording; }

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void RefreshRecordingState();

	/** Told when the microphone opens or closes, to warn about a recording app with no notice bound. */
	void SetVoiceCapturing(bool bCapturing);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void GetCommunicationSettings(FOnCommunicationSettings OnSuccess, FOnSafetyError OnError);

	/** Stores one choice and leaves the others. Policy may cap it, so read what applies from the answer. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void SetCommunicationSetting(ECrowdyCommunicationSetting Setting, ECrowdyCommunicationScope Scope, FOnCommunicationSettings OnSuccess, FOnSafetyError OnError);

	/** Emails a parent to ask for consent to Feature, unless Status says nothing was needed. Three requests an hour. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void RequestParentalConsent(ECrowdyConsentFeature Feature, const FString& ParentEmail, FOnConsentRequested OnSuccess, FOnSafetyError OnError);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void GetContacts(FOnPlayerContacts OnSuccess, FOnSafetyError OnError);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void GetContactRequests(FOnContactRequests OnSuccess, FOnSafetyError OnError);

	/** Asks another player. The answer is accepted at once if they had already asked this player. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void RequestContact(int64 UserId, FOnPlayerContact OnSuccess, FOnSafetyError OnError);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void AcceptContact(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void RemoveContact(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void GetBlocks(FOnPlayerBlocks OnSuccess, FOnSafetyError OnError);

	/** Stops voice, video, text and events between the two players, both ways. Movement and world edits still pass. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void BlockPlayer(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety")
	void UnblockPlayer(int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError);

	/**
	 * Reports a player to this app's moderators. Details and ReportedActorUuid are optional. Recordings are kept from 15
	 * minutes before OccurredAt, so pass when it happened, in UTC. Left at its default, or in the future, it is now.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Safety", meta=(AdvancedDisplay="ReportedActorUuid,OccurredAt"))
	void FileReport(int64 ReportedUserId, ECrowdySafetyReportReason Reason, const FString& Details, const FString& ReportedActorUuid,
		FDateTime OccurredAt, FOnSafetyDone OnSuccess, FOnSafetyError OnError);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId);

	bool HasWarnedAboutRecording() const { return bWarnedAboutRecording; }

	void SetAppIdForTest(int64 AppId) { AppIdForTest = AppId; }

	int64 GetAppIdForTest() const { return GetAppId(); }
#endif

private:
	UFUNCTION()
	void HandleAppTokenRefreshed();

	void WarnIfRecordingUnannounced();
	FCrowdyCppClient* ResolveClient() const;
	int64 GetAppId() const;
	TSharedPtr<FJsonObject> MakeVariables() const;
	void RunCommand(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, FOnSafetyDone OnSuccess, FOnSafetyError OnError);
	void RunUserCommand(const TCHAR* Operation, int64 UserId, FOnSafetyDone OnSuccess, FOnSafetyError OnError);
	void RunSettings(const TCHAR* Operation, const TCHAR* Field, const TSharedPtr<FJsonObject>& Variables, FOnCommunicationSettings OnSuccess, FOnSafetyError OnError);

	TSharedPtr<uint8> LiveSessionToken;
	FCrowdyAppRecording Recording;
	bool bVoiceCapturing = false;
	bool bWarnedAboutRecording = false;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	int64 AppIdForTest = 0;
#endif
};
