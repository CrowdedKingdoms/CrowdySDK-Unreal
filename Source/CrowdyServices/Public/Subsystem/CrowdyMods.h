#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyMods.generated.h"

class FCrowdyCppClient;
class FCrowdyNativeExecConnection;
class FJsonObject;
struct FCrowdyCppJsonResult;

/** How a mod call ended. Values are only ever added at the end. */
UENUM(BlueprintType)
enum class ECrowdyModResult : uint8
{
	Success,
	/** The player has no app token yet. */
	NotSignedIn,
	/** A missing permission, a grid that is not the player's, or a billing switch; the message names which. */
	Denied,
	/** No such mod, build or grid. */
	NotFound,
	/** Any other refusal, such as a name or a limit, the platform's or this SDK's before sending. */
	Refused,
	/** The request did not complete. */
	Failed,
	/** The answer was malformed, so nothing from it is returned. */
	BadReply
};

/** Where a mod build is. */
UENUM(BlueprintType)
enum class ECrowdyModBuildStatus : uint8
{
	Queued,
	Building,
	Succeeded,
	Failed
};

/** One source file of a mod's crate. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModFile
{
	GENERATED_BODY()

	/** Cargo.toml, README.md, or a .rs file under src/, with forward slashes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mods")
	FString Path;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mods")
	FString Content;
};

/** A mod build so far, or as it ended. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModBuild
{
	GENERATED_BODY()

	/** Success when the build ran to an end; a crate that does not compile is Success with Status Failed. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** The platform's id for the build, as given; empty until it answered. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString BuildId;

	/** The last status the platform gave; Failed when it gave none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModBuildStatus Status = ECrowdyModBuildStatus::Failed;

	/** The compiler's output as the platform keeps it, at most 64 KB with the end kept. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Log;

	/** The ckx-sdk version the build compiled against, once it started. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString SdkVersion;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** One mod on one grid. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyMod
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString ModId;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 GridId = 0;

	/** The player it runs as. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 OwnerUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 Version = 0;

	/** SHA-256 of its module, hex. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Digest;

	/** Its owner switched it on. It runs only while this holds and it is not blocked. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	bool bEnabled = false;

	/** An instance of it runs now. A switched-on mod starts on its first call. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	bool bRunning = false;

	/** A switch, such as billing's, stops it whatever bEnabled says. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	bool bBlocked = false;

	/** Why it is blocked; empty unless bBlocked. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString BlockedReason;

	/** Unix time in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 UpdatedAtMs = 0;
};

/** The answer to Deploy Mod and Set Mod Enabled. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** The mod as it now is; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FCrowdyMod Mod;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** The answer to List Mods and List My Mods. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModList
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** In the platform's order; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	TArray<FCrowdyMod> Mods;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** One line a mod logged. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModLogLine
{
	GENERATED_BODY()

	/** Unix time in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 AtMs = 0;

	/** 0 error, 1 warning, 2 info, 3 debug, as the platform gives it. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int32 Level = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Text;
};

/** The answer to Get Mod Logs. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModLogs
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** Oldest first, from the last 24 hours; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	TArray<FCrowdyModLogLine> Lines;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** The answer to Delete Mod. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModDelete
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** The platform removed the mod, with its state and versions. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	bool bDeleted = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** The answer to Get Starter: the platform's own starter crate. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModStarter
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** The crate's name, which is also the mod's. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	TArray<FCrowdyModFile> Files;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** The answer to Call Mod. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModCall
{
	GENERATED_BODY()

	/** A switched-off mod answers Denied, a deleted one NotFound. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** The mod's answer as JSON; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString ReplyJson;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/**
 * Players' own server code on grids they own: builds a mod from source, deploys it, switches it on and off, reads its
 * state and logs, deletes it and calls it. Every call is the signed-in player's, on the app token, and nothing is cached.
 * Each OnDone runs once on the game thread, never inside the call, unless the game instance ends first.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyMods : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The most files one crate may hold. */
	static constexpr int32 MaxFiles = 64;

	/** The most source one crate may hold, in UTF-8 bytes. */
	static constexpr int64 MaxSourceBytes = 2 * 1024 * 1024;

	/** How often a build's status is read while it runs. */
	static constexpr double BuildPollSeconds = 2.0;

	/** How long a build is waited for before it is answered Failed. */
	static constexpr double BuildLimitSeconds = 11.0 * 60.0;

	/** The most log lines one read may ask for; 0 asks for DefaultLogLines. */
	static constexpr int32 MaxLogLines = 500;
	static constexpr int32 DefaultLogLines = 100;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Builds one crate on the platform and waits for it, reading its status every 2 seconds for up to 11 minutes.
	 * OnProgress runs once per status change; OnDone once at the end. Too many files, too much source, or a file other
	 * than Cargo.toml, README.md or a .rs under src/ answers Refused without a request.
	 */
	void BuildMod(const FString& CrateName, const TArray<FCrowdyModFile>& Files, TFunction<void(const FCrowdyModBuild&)> OnProgress,
		TFunction<void(const FCrowdyModBuild&)> OnDone);

	/** Deploys a successful build as the named mod on one of the player's grids. A new mod starts switched off. */
	void DeployMod(int64 GridId, const FString& Name, const FString& BuildId, TFunction<void(const FCrowdyModInfo&)> OnDone);

	/** Switches a mod on or off. A switched-on mod waits for its app's admission and starts on its first call. */
	void SetModEnabled(int64 GridId, const FString& Name, bool bEnabled, TFunction<void(const FCrowdyModInfo&)> OnDone);

	/** Every mod on one grid. */
	void ListMods(int64 GridId, TFunction<void(const FCrowdyModList&)> OnDone);

	/** The signed-in player's mods on every grid of the app. */
	void ListMyMods(TFunction<void(const FCrowdyModList&)> OnDone);

	/** A mod's latest log lines, oldest first. Limit is 1 to 500, or 0 for 100; any other answers Refused without a request. */
	void GetModLogs(int64 GridId, const FString& Name, int32 Limit, TFunction<void(const FCrowdyModLogs&)> OnDone);

	/** Stops a mod and removes it, with its state and its versions. */
	void DeleteMod(int64 GridId, const FString& Name, TFunction<void(const FCrowdyModDelete&)> OnDone);

	/** The platform's starter crate, ready for Build Mod. */
	void GetStarter(TFunction<void(const FCrowdyModStarter&)> OnDone);

	/**
	 * Calls Method on the named mod of a grid, with ParamsJson as its argument (empty sends {}), over the platform's
	 * gateway. The first call to a switched-on mod starts it.
	 */
	void CallMod(int64 GridId, const FString& Name, const FString& Method, const FString& ParamsJson, TFunction<void(const FCrowdyModCall&)> OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, const FString& GatewayDomain = FString());

	/** Replaces the clock a build's time limit is measured with; a negative value restores the real one. */
	void SetNowForTest(double Seconds) { NowForTest = Seconds; }
#endif

private:
	struct FBuild;

	void PollBuild(const TSharedRef<FBuild>& Build);
	void SchedulePoll(const TSharedRef<FBuild>& Build);
	void ReadBuild(const TSharedRef<FBuild>& Build, const FCrowdyCppJsonResult& Result, const TCHAR* Field);
	/** Sends one Exec operation for this app. OnAnswer gets an answer that arrived whole; OnFailed every other ending. */
	void SendModOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
		TFunction<void(ECrowdyModResult Result, const FString& Message, const FString& Code)> OnFailed);
	void ReleaseCallConnection(const TSharedPtr<FCrowdyNativeExecConnection>& Connection);
	FCrowdyCppClient* ResolveClient() const;
	int64 GetAppId() const;
	bool HasAppToken() const;
	FString GetAppToken() const;
	double Now() const;

	TSharedPtr<uint8> LiveSessionToken;

	/** One gateway connection per Call Mod, closed once it answers. */
	TArray<TSharedPtr<FCrowdyNativeExecConnection>> CallConnections;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	int64 AppIdForTest = 0;
	FString GatewayDomainForTest;
	double NowForTest = -1.0;
#endif
};
