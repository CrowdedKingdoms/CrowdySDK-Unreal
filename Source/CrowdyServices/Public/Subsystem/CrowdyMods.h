#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystem/CrowdyStudioTypes.h"
#include "CrowdyMods.generated.h"

class FCrowdyCppClient;
class FCrowdyNativeExecConnection;
class FJsonObject;
class UCrowdyStudioProjects;
struct FCrowdyCppJsonResult;

/** How a mod call ended. Values are only ever added at the end. */
UENUM(BlueprintType)
enum class ECrowdyModResult : uint8
{
	Success,
	/** The player has no app token yet. */
	NotSignedIn,
	/** A missing permission, a grid that is not the player's, a billing switch or the app's Allow Live Modding switch; the message names which. */
	Denied,
	/** No such mod, build or grid. */
	NotFound,
	/** Any other refusal, such as a name or a limit, the platform's or this SDK's before sending. */
	Refused,
	/** The request did not complete. */
	Failed,
	/** The answer was malformed, so nothing from it is returned. */
	BadReply,
	/** Another run for this app is still going, or the platform is already building one for this player; try again once it ends. */
	Busy,
	/** The project's saved revision or module names are not the ones asked for, so nothing was built. */
	RevisionChanged,
	/** This SDK version does not carry the call yet, so nothing was sent. */
	NotBuilt
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

/** A mod's CLIENT half on one grid: the Mod Script its grid serves to the players standing in it. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModClient
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString ModId;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Name;

	/** The author, who owns the grid. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 OwnerUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 ClientVersion = 0;

	/** SHA-256 of its module, hex. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Digest;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 SizeBytes = 0;

	/** The host calls it may make, as JSON. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString CapabilitySummaryJson;

	/** What a player's consent to it names; it changes with its host calls. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString CapabilityHash;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int32 TickIntervalMs = 0;

	/** Unix time in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 UpdatedAtMs = 0;
};

/** The answer to Deploy Client Mod. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModClientInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	/** The CLIENT half as it now is; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FCrowdyModClient Client;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** Where one half of a project's run is, as read back from the platform and the running Mod Scripts. */
UENUM(BlueprintType)
enum class ECrowdyModRunPhase : uint8
{
	NotStarted UMETA(DisplayName="Not Started"),
	Building,
	BuildFailed UMETA(DisplayName="Build Failed"),
	Deploying,
	DeployFailed UMETA(DisplayName="Deploy Failed"),
	/** Deployed, but not running now; the message says why. */
	Deployed,
	Running
};

/** One half of a project on one grid. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModRunTarget
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyStudioTarget Target = ECrowdyStudioTarget::Server;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModRunPhase Phase = ECrowdyModRunPhase::NotStarted;

	/** The module's name in the project. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Module;

	/** The mod it runs as; empty until deployed. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString ModId;

	/** SHA-256 of the module the platform serves for this half, hex; empty when it serves none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Digest;

	/** The platform's version of what it serves for this half: the mod's version, or the CLIENT half's; 0 when it serves none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 Version = 0;

	/**
	 * The project revision this SDK deployed under Digest, reported only while the platform still serves that digest,
	 * else empty; the platform keeps no Studio revision of its own.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString RunningRevision;

	/** The project's saved revision when this was read. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString SavedRevision;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

UENUM(BlueprintType)
enum class ECrowdyModDiagnosticSeverity : uint8
{
	Error,
	Warning,
	Info,
	Hint
};

/** What reported a diagnostic. Values are only ever added at the end. */
UENUM(BlueprintType)
enum class ECrowdyModDiagnosticSource : uint8
{
	/** The Rust compiler, from the platform's build log. */
	Rustc UMETA(DisplayName="rustc"),
	/** An advisory check made on this machine, not by the compiler. */
	Local,
	/** The module as it ran. */
	Runtime
};

/** One message about a project file, such as a compiler error parsed from a build log. Lines and columns count from 1. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModDiagnostic
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyStudioTarget Target = ECrowdyStudioTarget::Server;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModDiagnosticSource Source = ECrowdyModDiagnosticSource::Rustc;

	/** The project file, with forward slashes. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int32 Line = 1;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int32 Column = 1;

	/** 0 when the compiler gave no end. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int32 EndLine = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int32 EndColumn = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModDiagnosticSeverity Severity = ECrowdyModDiagnosticSeverity::Error;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;

	/** The compiler's code, such as E0425; empty when it gave none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Code;
};

/** The answer to Run Project. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModRun
{
	GENERATED_BODY()

	/** Success when the run went to its end; a build that does not compile is Success with that target's Phase Build Failed. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyStudioRunMode Mode = ECrowdyStudioRunMode::DraftTest;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString ProjectId;

	/** The revision built, which is always the one asked for. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Revision;

	/** Each half the project has, SERVER first. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	TArray<FCrowdyModRunTarget> Targets;

	/** The compiler's output for every half built, at most 64 KB each with the end kept. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString BuildLog;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	TArray<FCrowdyModDiagnostic> Diagnostics;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString Message;
};

/** The answer to Get Project Status. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModProjectStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	ECrowdyModResult Result = ECrowdyModResult::Failed;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	FString ProjectId;

	/** A run of this project is still going; Targets show where it is. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	bool bRunInFlight = false;

	/** Each half the project has, SERVER first; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Mods")
	TArray<FCrowdyModRunTarget> Targets;

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

	/**
	 * Builds one CLIENT crate on the platform and waits for it, as Build Mod does, with the same limits and status reads.
	 * A primitive with neither the one-run lock nor a pinned revision, and no Allow Live Modding check; the Studio dock uses RunProject.
	 */
	void BuildClientMod(const FString& CrateName, const TArray<FCrowdyModFile>& Files, TFunction<void(const FCrowdyModBuild&)> OnProgress,
		TFunction<void(const FCrowdyModBuild&)> OnDone);

	/**
	 * Attaches a successful CLIENT build as the named mod's CLIENT half on one of the player's grids; the mod must exist.
	 * A primitive with neither the one-run lock nor a pinned revision, and no Allow Live Modding check; the Studio dock uses RunProject.
	 */
	void DeployClientMod(int64 GridId, const FString& Name, const FString& BuildId, TFunction<void(const FCrowdyModClientInfo&)> OnDone);

	/**
	 * Builds and deploys exactly ExpectedRevision of a Studio project on a grid. The CLIENT half is built first, so a failed
	 * build publishes nothing; the SERVER half is then built, deployed and switched on (a CLIENT-only project rides the
	 * player's mod named for its CLIENT module, deployed from the platform's starter when the grid has none, and refused
	 * when it is there but switched off); last the CLIENT half is attached and consented to as its author. Neither this
	 * nor the platform's calls check the app's Allow Live Modding switch: ask UCrowdyStudioAgent::AuthorizeLiveModding
	 * first. An empty ExpectedRevision, a GridId below 1 and a signed-out player are refused before sending, and
	 * RevisionChanged answers a saved revision that is another. One run per app in this process at a time: a second
	 * answers Busy. The run keeps to its grid and to the player and app it started for: if either changes it answers
	 * Failed and sends nothing more. If the game instance ends first it sends nothing more, OnDone does not run, and a
	 * step already sent may still complete. The module names are not checked; the overload below checks them.
	 */
	void RunProject(int64 GridId, const FString& ProjectId, const FString& ExpectedRevision, ECrowdyStudioRunMode Mode,
		TFunction<void(const FCrowdyModRun&)> OnDone);

	/**
	 * As above, and answers RevisionChanged before building when the project's SERVER or CLIENT module name is not the one
	 * given, since those names pick the mods the run overwrites. An empty name is not checked.
	 */
	void RunProject(int64 GridId, const FString& ProjectId, const FString& ExpectedRevision, ECrowdyStudioRunMode Mode,
		const FString& ExpectedServerModule, const FString& ExpectedClientModule, TFunction<void(const FCrowdyModRun&)> OnDone);

	/**
	 * Where each half of a project is on a grid, read back from the platform every time: the project's saved revision, its
	 * SERVER mod and its served CLIENT half. RunningRevision is the revision a run in this process deployed, given only
	 * while the platform still serves that run's digest. A GridId below 1 is refused before sending.
	 */
	void GetProjectStatus(int64 GridId, const FString& ProjectId, TFunction<void(const FCrowdyModProjectStatus&)> OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, const FString& GatewayDomain = FString());

	/** Replaces the clock a build's time limit is measured with; a negative value restores the real one. */
	void SetNowForTest(double Seconds) { NowForTest = Seconds; }

	/** Reads Studio projects through Projects instead of the game instance's. */
	void SetStudioProjectsForTest(UCrowdyStudioProjects* Projects);

	/** Stands in for the signed-in player's id on a subsystem given a test client. */
	void SetUserIdForTest(int64 UserId) { UserIdForTest = UserId; }

	/** Moves the subsystem to another app, as a session for another app would. */
	void SetAppIdForTest(int64 AppId) { AppIdForTest = AppId; }
#endif

private:
	struct FBuild;
	struct FRun;
	struct FStatusRead;

	void StartBuild(const TCHAR* Call, const TCHAR* Operation, const TCHAR* Field, const FString& CrateName, const TArray<FCrowdyModFile>& Files,
		TFunction<void(const FCrowdyModBuild&)> OnProgress, TFunction<void(const FCrowdyModBuild&)> OnDone);
	void PollBuild(const TSharedRef<FBuild>& Build);
	void SchedulePoll(const TSharedRef<FBuild>& Build);
	void ReadBuild(const TSharedRef<FBuild>& Build, const FCrowdyCppJsonResult& Result, const TCHAR* Field);
	/** Sends one Exec operation for this app. OnAnswer gets an answer that arrived whole; OnFailed every other ending. */
	void SendModOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
		TFunction<void(ECrowdyModResult Result, const FString& Message, const FString& Code)> OnFailed);
	void ReleaseCallConnection(const TSharedPtr<FCrowdyNativeExecConnection>& Connection);
	FCrowdyCppClient* ResolveClient() const;
	UCrowdyStudioProjects* ResolveStudioProjects() const;
	int64 GetAppId() const;
	int64 GetUserId() const;
	/** Whether the signed-in player and app are still the ones a multi-step call started for. */
	bool IsCaller(int64 AppId, int64 UserId) const { return GetAppId() == AppId && GetUserId() == UserId; }
	bool HasAppToken() const;
	FString GetAppToken() const;
	double Now() const;

	TSharedPtr<uint8> LiveSessionToken;

	/** One gateway connection per Call Mod, closed once it answers. */
	TArray<TSharedPtr<FCrowdyNativeExecConnection>> CallConnections;

	/** This game instance's Run Projects still going; Deinitialize ends them and frees their apps. */
	TArray<TSharedPtr<FRun>> Runs;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	TWeakObjectPtr<UCrowdyStudioProjects> ProjectsForTest;
	int64 AppIdForTest = 0;
	int64 UserIdForTest = 0;
	FString GatewayDomainForTest;
	double NowForTest = -1.0;
#endif
};
