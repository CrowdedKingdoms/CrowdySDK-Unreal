#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyMods.h"
#include "CrowdyModsActions.generated.h"

// One signature serves each node's pins, because a Blueprint async node builds its pins from its delegates.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_SixParams(FCrowdyModBuildAsyncResult, const FString&, BuildId, ECrowdyModBuildStatus, Status, const FString&, Log,
	const FString&, SdkVersion, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModInfoAsyncResult, const FCrowdyMod&, Mod, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModListAsyncResult, const TArray<FCrowdyMod>&, Mods, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModLogsAsyncResult, const TArray<FCrowdyModLogLine>&, Lines, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModDeleteAsyncResult, bool, bDeleted, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FCrowdyModStarterAsyncResult, const FString&, Name, const TArray<FCrowdyModFile>&, Files, ECrowdyModResult, Result,
	const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModCallAsyncResult, const FString&, ReplyJson, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModClientInfoAsyncResult, const FCrowdyModClient&, Client, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModRunAsyncResult, const FCrowdyModRun&, Run, ECrowdyModResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyModProjectStatusAsyncResult, const FCrowdyModProjectStatus&, Status, ECrowdyModResult, Result,
	const FString&, Message);

/** Builds one crate on the platform and waits for it. C++ callers use UCrowdyMods::BuildMod. */
UCLASS()
class CROWDYSERVICES_API UCrowdyBuildMod : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The build's status changed; fires once per change. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModBuildAsyncResult OnProgress;

	/** The build ended: Status Succeeded, or Failed with the compiler's log saying why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModBuildAsyncResult OnDone;

	/** The build could not be run or waited for; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModBuildAsyncResult OnFailed;

	/** At most 64 files and 2 MB of source: Cargo.toml, README.md, and .rs files under src/. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Build Mod"), Category="Crowdy SDK|Mods")
	static UCrowdyBuildMod* BuildMod(UObject* WorldContextObject, const FString& CrateName, const TArray<FCrowdyModFile>& Files);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString CrateName;
	TArray<FCrowdyModFile> Files;

	void Broadcast(FCrowdyModBuildAsyncResult& Pin, const FCrowdyModBuild& Build);
	void Finish(const FCrowdyModBuild& Build);
};

/** Deploys a successful build as a mod on one of the player's grids. C++ callers use UCrowdyMods::DeployMod. */
UCLASS()
class CROWDYSERVICES_API UCrowdyDeployMod : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The mod as deployed; a new mod starts switched off. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModInfoAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModInfoAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Deploy Mod"), Category="Crowdy SDK|Mods")
	static UCrowdyDeployMod* DeployMod(UObject* WorldContextObject, int64 GridId, const FString& Name, const FString& BuildId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString Name;
	FString BuildId;

	void Finish(const FCrowdyModInfo& Info);
};

/** Switches a mod on or off. C++ callers use UCrowdyMods::SetModEnabled. */
UCLASS()
class CROWDYSERVICES_API UCrowdySetModEnabled : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The mod as it now is. A switched-on mod starts on its first call. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModInfoAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModInfoAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Set Mod Enabled"), Category="Crowdy SDK|Mods")
	static UCrowdySetModEnabled* SetModEnabled(UObject* WorldContextObject, int64 GridId, const FString& Name, bool bEnabled);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString Name;
	bool bEnabled = false;

	void Finish(const FCrowdyModInfo& Info);
};

/** Every mod on one grid. C++ callers use UCrowdyMods::ListMods. */
UCLASS()
class CROWDYSERVICES_API UCrowdyListMods : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The grid's mods; empty when it has none. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModListAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModListAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="List Mods"), Category="Crowdy SDK|Mods")
	static UCrowdyListMods* ListMods(UObject* WorldContextObject, int64 GridId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;

	void Finish(const FCrowdyModList& List);
};

/** The signed-in player's mods on every grid of the app. C++ callers use UCrowdyMods::ListMyMods. */
UCLASS()
class CROWDYSERVICES_API UCrowdyListMyMods : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModListAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModListAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="List My Mods"), Category="Crowdy SDK|Mods")
	static UCrowdyListMyMods* ListMyMods(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	void Finish(const FCrowdyModList& List);
};

/** A mod's latest log lines, oldest first. C++ callers use UCrowdyMods::GetModLogs. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetModLogs : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModLogsAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModLogsAsyncResult OnFailed;

	/** Limit is 1 to 500, or 0 for 100. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Mod Logs"), Category="Crowdy SDK|Mods")
	static UCrowdyGetModLogs* GetModLogs(UObject* WorldContextObject, int64 GridId, const FString& Name, int32 Limit);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString Name;
	int32 Limit = 0;

	void Finish(const FCrowdyModLogs& Logs);
};

/** Stops a mod and removes it. C++ callers use UCrowdyMods::DeleteMod. */
UCLASS()
class CROWDYSERVICES_API UCrowdyDeleteMod : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModDeleteAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModDeleteAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Delete Mod"), Category="Crowdy SDK|Mods")
	static UCrowdyDeleteMod* DeleteMod(UObject* WorldContextObject, int64 GridId, const FString& Name);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString Name;

	void Finish(const FCrowdyModDelete& Delete);
};

/** The platform's starter crate, ready for Build Mod. C++ callers use UCrowdyMods::GetStarter. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetModStarter : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Name is the crate's and the mod's. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModStarterAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModStarterAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Starter"), Category="Crowdy SDK|Mods")
	static UCrowdyGetModStarter* GetStarter(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;

	void Finish(const FCrowdyModStarter& Starter);
};

/** Calls a method of a mod on a grid. C++ callers use UCrowdyMods::CallMod. */
UCLASS()
class CROWDYSERVICES_API UCrowdyCallMod : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The mod's answer, as JSON. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModCallAsyncResult OnDone;

	/** A switched-off mod answers Denied, a deleted one NotFound. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModCallAsyncResult OnFailed;

	/** Params Json is the method's argument as JSON; empty sends {}. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Call Mod"), Category="Crowdy SDK|Mods")
	static UCrowdyCallMod* CallMod(UObject* WorldContextObject, int64 GridId, const FString& Name, const FString& Method, const FString& ParamsJson);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString Name;
	FString Method;
	FString ParamsJson;

	void Finish(const FCrowdyModCall& Call);
};

/** Builds one CLIENT crate on the platform and waits for it, with no run lock, pinned revision or Allow Live Modding check; use Run Project for a Studio run. C++ callers use UCrowdyMods::BuildClientMod. */
UCLASS()
class CROWDYSERVICES_API UCrowdyBuildClientMod : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The build's status changed; fires once per change. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModBuildAsyncResult OnProgress;

	/** The build ended: Status Succeeded, or Failed with the compiler's log saying why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModBuildAsyncResult OnDone;

	/** The build could not be run or waited for; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModBuildAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Build Client Mod"), Category="Crowdy SDK|Mods")
	static UCrowdyBuildClientMod* BuildClientMod(UObject* WorldContextObject, const FString& CrateName, const TArray<FCrowdyModFile>& Files);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString CrateName;
	TArray<FCrowdyModFile> Files;

	void Broadcast(FCrowdyModBuildAsyncResult& Pin, const FCrowdyModBuild& Build);
	void Finish(const FCrowdyModBuild& Build);
};

/** Attaches a successful CLIENT build as a mod's CLIENT half, with no run lock, pinned revision or Allow Live Modding check; use Run Project for a Studio run. C++ callers use UCrowdyMods::DeployClientMod. */
UCLASS()
class CROWDYSERVICES_API UCrowdyDeployClientMod : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModClientInfoAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModClientInfoAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Deploy Client Mod"), Category="Crowdy SDK|Mods")
	static UCrowdyDeployClientMod* DeployClientMod(UObject* WorldContextObject, int64 GridId, const FString& Name, const FString& BuildId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString Name;
	FString BuildId;

	void Finish(const FCrowdyModClientInfo& Info);
};

/** Builds and deploys one revision of a Studio project on a grid. C++ callers use UCrowdyMods::RunProject. */
UCLASS()
class CROWDYSERVICES_API UCrowdyRunProject : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The run went to its end; each target's Phase says where it ended. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModRunAsyncResult OnDone;

	/** The run did not go ahead or did not finish; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyModRunAsyncResult OnFailed;

	/** Does not check the app's Allow Live Modding switch: run Authorize Live Modding first. Fails with Refused for an empty Expected Revision. One run per app at a time: a second fails with Busy. A saved revision other than Expected Revision fails with Revision Changed. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Run Project"), Category="Crowdy SDK|Mods")
	static UCrowdyRunProject* RunProject(UObject* WorldContextObject, int64 GridId, const FString& ProjectId, const FString& ExpectedRevision, ECrowdyStudioRunMode Mode);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString ProjectId;
	FString ExpectedRevision;
	ECrowdyStudioRunMode Mode = ECrowdyStudioRunMode::DraftTest;

	void Finish(const FCrowdyModRun& Run);
};

/** Where each half of a Studio project is on a grid, read back from the platform. C++ callers use UCrowdyMods::GetProjectStatus. */
UCLASS()
class CROWDYSERVICES_API UCrowdyGetProjectStatus : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FCrowdyModProjectStatusAsyncResult OnDone;

	UPROPERTY(BlueprintAssignable)
	FCrowdyModProjectStatusAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Get Project Status"), Category="Crowdy SDK|Mods")
	static UCrowdyGetProjectStatus* GetProjectStatus(UObject* WorldContextObject, int64 GridId, const FString& ProjectId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;
	FString ProjectId;

	void Finish(const FCrowdyModProjectStatus& Status);
};
