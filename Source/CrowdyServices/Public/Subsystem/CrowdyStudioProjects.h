#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystem/CrowdyStudioTypes.h"
#include "CrowdyStudioProjects.generated.h"

class FCrowdyCppClient;
class FJsonObject;
class UCrowdyMods;
struct FCrowdyCppJsonResult;
struct FCrowdyModStarter;

/** Which halves a Studio project has: the platform's pairingPreference SERVER_ONLY, CLIENT_ONLY, PAIRED and INDEPENDENT, in that order. */
UENUM(BlueprintType)
enum class ECrowdyStudioProjectKind : uint8
{
	/** A SERVER mod only. */
	Server,
	/** A CLIENT Mod Script only, riding the platform's starter mod. */
	Client,
	/** A SERVER mod and its CLIENT Mod Script, which run only together. */
	Paired,
	/** A SERVER mod and a CLIENT Mod Script, either of which may run without the other. */
	Independent
};

/** Where a project's files are authored. */
UENUM(BlueprintType)
enum class ECrowdyStudioProjectSource : uint8
{
	Studio,
	/** A bound repository is the working tree; the platform keeps a mirror of it. */
	GitHub UMETA(DisplayName="GitHub")
};

/** One of the signed-in player's Studio projects. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioProject
{
	GENERATED_BODY()

	/** The platform's id for the project, as given. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString ProjectId;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioProjectSource Source = ECrowdyStudioProjectSource::Studio;

	/** An opaque token that changes whenever the project's source does; hand it back as is. For a GitHub project it is the revision joined to the commit as "revision@sha". */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Revision;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioProjectKind Kind = ECrowdyStudioProjectKind::Server;

	/** The SERVER module's name, which is also its mod's; empty when the project has none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString ServerModule;

	/** The CLIENT module's name; empty when the project has none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString ClientModule;

	/** Unix time in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 UpdatedAtMs = 0;

	/** The bound repository's commit the mirror holds; empty unless Source is GitHub. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString GitHubSha;

	/** The bound repository as owner/repo@branch; empty unless Source is GitHub. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString GitHubRef;

	/** The grid the project is bound to; 0 when none. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	int64 GridId = 0;
};

/** One source file of a Studio project. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioProjectFile
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioTarget Target = ECrowdyStudioTarget::Server;

	/** Cargo.toml, README.md or a Rust file under src/. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Content;

	/** This file's own revision, as given; it rises with every save of the file. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Revision;
};

/** The answer to Get Studio Project and Create Studio Project. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioProjectInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioResult Result = ECrowdyStudioResult::Failed;

	/** Empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FCrowdyStudioProject Project;

	/** The project's files, ordered by target and path; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	TArray<FCrowdyStudioProjectFile> Files;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Message;
};

/** The answer to List Studio Projects. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyStudioProjectList
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	ECrowdyStudioResult Result = ECrowdyStudioResult::Failed;

	/** Newest first, archived ones left out; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	TArray<FCrowdyStudioProject> Projects;

	/** The platform holds more projects than this answer lists. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	bool bMore = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Studio")
	FString Message;
};

/**
 * The signed-in player's Studio projects in this app: list, read and create. Nothing is cached: each call asks the
 * platform. Each OnDone runs once on the game thread, never inside the call, unless the game instance ends first.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyStudioProjects : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The player's projects in this app, newest first, up to 50 per call. */
	void ListProjects(TFunction<void(const FCrowdyStudioProjectList&)> OnDone);

	/** One project, read fresh. */
	void GetProject(const FString& ProjectId, TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone);

	/**
	 * A new project of Kind from the platform's starters: its mod starter for a SERVER half, its CLIENT half starter for a
	 * CLIENT half, with its package named for the CLIENT module, or this SDK's copy of that starter when the platform does
	 * not serve it; a starter the platform refuses or that does not arrive fails the create. The module names come from
	 * Name: the SERVER one is its lowercase slug and the CLIENT one is the slug plus "-client", so a CLIENT module never
	 * shares a name with a SERVER mod. A name with no ASCII letters or digits is refused.
	 */
	void CreateProject(const FString& Name, ECrowdyStudioProjectKind Kind, TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone);

	/**
	 * As above with the module names given: 1 to 48 lowercase letters, digits, - and _, different from each other. Every half
	 * Kind has needs a name, and a name for a half Kind does not have is refused.
	 */
	void CreateProject(const FString& Name, ECrowdyStudioProjectKind Kind, const FString& ServerModule, const FString& ClientModule,
		TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client and reading the starters through Mods. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client = nullptr, int64 AppId = 0, UCrowdyMods* Mods = nullptr);
#endif

private:
	/** Sends one CrowdyStudio operation. OnAnswer gets an answer that arrived whole; OnFailed every other ending. */
	void SendStudioOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
		TFunction<void(ECrowdyStudioResult Result, const FString& Message, const FString& Code)> OnFailed);
	void SendCreate(const TSharedPtr<FJsonObject>& Input, TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone);
	void ReadClientStarter(const FString& ClientModule, TFunction<void(const FCrowdyModStarter&)> OnRead);
	/** Asks whether a project exists past a full page, then answers List with bMore set. */
	void ProbeMore(FCrowdyStudioProjectList List, int32 Offset, TFunction<void(const FCrowdyStudioProjectList&)> OnDone);
	UCrowdyMods* ResolveMods() const;
	FCrowdyCppClient* ResolveClient() const;
	int64 GetAppId() const;
	bool HasAppToken() const;

	TSharedPtr<uint8> LiveSessionToken;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	TWeakObjectPtr<UCrowdyMods> ModsForTest;
	int64 AppIdForTest = 0;
#endif
};
