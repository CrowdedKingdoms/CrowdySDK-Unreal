#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystem/CrowdyModScriptHost.h"
#include "Subsystem/CrowdyModScriptTypes.h"
#include "CrowdyModScripts.generated.h"

class FCrowdyCppClient;
class FCrowdyModScriptModule;
class FCrowdyModScriptRun;
class FJsonValue;
class UCrowdyGameSession;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptPrompt, const FCrowdyModScriptPrompt&, Prompt);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptPromptWithdrawn, int32, PromptId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptStarted, const FCrowdyModScript&, Script);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyModScriptStopped, const FCrowdyModScript&, Script, ECrowdyModScriptStop, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyModScriptError, const FCrowdyModScriptError&, Error);

/**
 * Mod Scripts: the code a mod's grid serves to the players standing in it (the platform's CLIENT halves). The game says
 * which chunk the player stands in, asks the player before an author's scripts run, and answers the calls only it can
 * through SetHost. The SDK answers the player's state, the grid's info and permissions, the clock and the sends itself; the
 * world, input and presentation calls go to the host, and the rest are refused as not offered.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyModScripts : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** How often the grid's Mod Scripts are listed again while the player stands in it. */
	static constexpr double ListSeconds = 10.0;

	/** The most log lines kept in memory per Mod Script. */
	static constexpr int32 MaxLogLines = 200;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Whether this build can run Mod Scripts: only on Win64, with the runtime started. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Mod Scripts")
	bool IsAvailable() const;

	/** The local player stands in this chunk; the SDK finds the grid that decides it. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void EnterGrid(int64 ChunkX, int64 ChunkY, int64 ChunkZ);

	/** The local player left the grid: every Mod Script of it stops. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void LeaveGrid();

	/** The grid the scripts run in, as the platform reported it; empty outside a grid. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Mod Scripts")
	FCrowdyModScriptGrid GetGrid() const;

	/** A host being replaced is first told to clear every running script. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void SetHost(TScriptInterface<ICrowdyModScriptHost> Host);

	/** Clears the host only if it is the one set, telling it first to clear every running script. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void ClearHost(TScriptInterface<ICrowdyModScriptHost> Host);

	/** Run trusts the prompt's author on this grid for its calls; a decline lasts until the player next enters the grid or the calls change. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void AnswerPrompt(int32 PromptId, bool bRun);

	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Mod Scripts")
	TArray<FCrowdyModScript> GetRunning() const;

	/** Stops the script and takes back the player's agreement to it. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void Revoke(const FString& ModId);

	/** Stops all of the author's scripts on this grid and takes back the player's trust in the author. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Mod Scripts")
	void ForgetAuthor(int64 AuthorId);

	/**
	 * Where each of a grid's Mod Scripts is. OnDone runs once on the game thread, never inside the call. OnDone runs even
	 * after this world ends; capture weakly. Every script the grid lists is answered, agreed to or not. NotInGrid unless
	 * GridId is the grid the player stands in; Failed when the Mod Script runtime is not running.
	 */
	void GetScriptStatus(int64 GridId, TFunction<void(const FCrowdyModScriptStatusList&)> OnDone);

	/**
	 * A Mod Script's last log lines, oldest first, from memory. OnDone runs as for GetScriptStatus. OnDone runs even after
	 * this world ends; capture weakly. The lines outlive each run of the script, so a crash's last lines stay readable.
	 * NotInGrid unless GridId is the grid the player stands in and it lists ModId.
	 */
	void GetScriptLog(int64 GridId, const FString& ModId, TFunction<void(const FCrowdyModScriptLog&)> OnDone);

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptPrompt OnModScriptPrompt;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptPromptWithdrawn OnModScriptPromptWithdrawn;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptStarted OnModScriptStarted;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptStopped OnModScriptStopped;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Mod Scripts")
	FOnCrowdyModScriptError OnModScriptError;

	struct FPlatformCall;
	struct FPlatformReply;
	struct FRealtimeSend;

	/** Sends one platform call and answers it once, never inside the call. */
	using FPlatform = TFunction<void(const FPlatformCall& Call, TFunction<void(const FPlatformReply& Reply)> OnReply)>;

	/** Queues one spatial or channel send on the realtime connection; false sets OutError. */
	using FRealtime = TFunction<bool(const FRealtimeSend& Send, FString& OutError)>;

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a world, sending every platform call to InPlatform. */
	void InitializeForTest(FPlatform InPlatform, UCrowdyGameSession* InSession = nullptr);

	/** Runs one frame at NowSeconds, which every later call also reads as the time. */
	void TickForTest(double NowSeconds);

	/** How many host calls the listed script's runs have had answered. */
	int32 GetHostCallsForTest(const FString& ModId) const;

	/** Replaces the frame's host-call budget, so a spent budget can be tested. */
	void SetHostBudgetForTest(double BudgetMs) { HostBudgetMsForTest = BudgetMs; }

	/** Stands in for the voxel world's count of unsent edits; below 0 reads the voxel world again. */
	void SetUnsentEditsForTest(int32 Count) { UnsentEditsForTest = Count; }

	/** Reads a chunk from Host as a script's chunk call does, at most once a frame; true when it is loaded. */
	bool ReadChunkForTest(UObject& Host, int64 ChunkX, int64 ChunkY, int64 ChunkZ) { return ReadChunkOnce(Host, FInt64Vector(ChunkX, ChunkY, ChunkZ)).IsValid(); }

	/** Sends every spatial and channel send to InRealtime instead of the realtime connection. */
	void SetRealtimeForTest(FRealtime InRealtime) { Realtime = MoveTemp(InRealtime); }
#endif

private:
	struct FScript;
	struct FAuthor;

	bool HandleTicker(float DeltaSeconds);
	void TickAt(double NowSeconds);
	double Now() const;
	void Send(const FPlatformCall& Call, TFunction<void(const FPlatformReply&)> OnReply);
	void SendAgreement(const FPlatformCall& Call, TFunction<void(const FPlatformReply&, bool bSameGrid)> OnReply);
	void SendToPlatform(const FPlatformCall& Call, TFunction<void(const FPlatformReply&)> OnReply);
	void SendStateOrChannels(FCrowdyCppClient& Client, const FPlatformCall& Call, int64 AppId, TFunction<void(const FPlatformReply&)> OnReply);
	void ReadGrid(int32 Serial, const FPlatformReply& Reply);
	void AdvanceGrid();
	void LeaveGridFor(ECrowdyModScriptStop Reason);
	void List();
	void ListAgain();
	void LoseListing();
	void ReadListing(const TArray<TSharedPtr<FJsonValue>>& Rows);
	void Ask();
	void StartAgreed();
	void Fetch(const TSharedRef<FScript>& Script);
	void ReadArtifact(const TSharedRef<FScript>& Script, const FPlatformReply& Reply);
	void Start(const TSharedRef<FScript>& Script, const FPlatformReply& Artifact);
	void StartRun(const TSharedRef<FScript>& Script, const TSharedRef<FCrowdyModScriptModule>& Module, int64 FuelPerDispatch, int32 TickIntervalMs);
	TArray<uint8> AnswerHostCall(FScript& Script, int32 Generation, TConstArrayView<uint8> Call);
	int32 SendEvent(const FScript& Sender, const TArray<uint8>& EventPrefix, const FString& Target, bool bTargeted);
	TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe> ReadChunkOnce(UObject& Host, const FInt64Vector& Chunk);
	bool AnswerVoxelSet(UObject& Host, const FScript& Script, const FCrowdyVoxelWrite& Write, FString& OutError);
	TArray<uint8> AnswerState(FScript& Script, const FPlatformCall& Call);
	TArray<uint8> AnswerSend(const FRealtimeSend& Message, const TCHAR* Name);
	TArray<uint8> AnswerChannelLater(FScript& Script, FRealtimeSend&& Message);
	void ReadGridChannels(int32 Serial, const FPlatformReply& Reply);
	bool SendRealtime(const FRealtimeSend& Message, FString& OutError);
	int32 UnsentEdits() const;
	void DrainClicks();
	void ClearFromHost();
	void Fail(const TSharedRef<FScript>& Script, double HoldSeconds, const FString& Message);
	static FCrowdyModScriptStatus StatusOf(const FScript& Script, int64 GridId);
	void StopRun(const TSharedRef<FScript>& Script, ECrowdyModScriptStop Reason);
	void RefuseScript(const FString& ModId, const FPlatformReply& Reply, bool bSameGrid);
	void RefuseAuthor(int64 AuthorId, const FPlatformReply& Reply);
	void Withdraw(FAuthor& Author);
	TSharedRef<FAuthor> FindOrAddAuthor(int64 AuthorId);
	TArray<TSharedRef<FScript>> ScriptsOf(int64 AuthorId) const;

	FPlatform Platform;
	FRealtime Realtime;
	TSharedPtr<uint8> LiveToken;
	FTSTicker::FDelegateHandle TickHandle;
	TWeakObjectPtr<UObject> HostObject;
	TWeakObjectPtr<UCrowdyGameSession> Session;

	FCrowdyModScriptGrid Grid;

	/** The player's own keys on Grid, from its last lookup, for grid_permission_check. */
	TArray<FString> GridKeys;
	TMap<FString, TSharedRef<FScript>> Scripts;
	TMap<int64, TSharedRef<FAuthor>> Authors;
	TArray<TSharedPtr<FCrowdyModScriptRun>> TickRuns;

	/** This frame's chunk reads, null for a chunk not loaded, so each chunk is read from the host once a frame. */
	TMap<FInt64Vector, TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe>> FrameReads;

	/** What the host last said is held down; each script's clicks queue on its FScript. */
	FCrowdyModScriptClicks HeldClicks;

	/** voxel_set calls admitted in the last second across every script, as a ring. */
	TArray<double> VoxelSetTimes;
	int32 VoxelSetOldest = 0;
	bool bWarnedChunkSize = false;

	/** Grid's own channels, read on the first emit_channel; the sends waiting for them; whether they are read or being read. */
	TSet<int64> GridChannels;
	TArray<TFunction<void()>> ChannelWaiting;
	double GridChannelsRetryAt = 0.0;
	bool bGridChannelsRead = false;
	bool bGridChannelsReading = false;

	/** The chunk last looked up, so a game naming the same chunk every frame asks once. */
	FInt64Vector AskedChunk = FInt64Vector(0, 0, 0);
	bool bAskedChunk = false;

	/** When a failed lookup of AskedChunk may be asked again. */
	double AskAgainAt = 0.0;

	/** The signed-in user when the grid was entered; another one means the player signed out. */
	int64 GridUserId = 0;
	double NextListAt = 0.0;

	/** Advances on every EnterGrid and LeaveGrid, so a grid answer for an earlier chunk is dropped. */
	int32 EnterSerial = 0;

	/** Advances on every change of grid, so an answer meant for the last grid is dropped. */
	int32 GridSerial = 0;

	/** Advances when an agreement call is sent or answered, so a listing that raced one is dropped. */
	int32 AgreementEpoch = 0;
	int32 PendingAgreements = 0;
	int32 NextPromptId = 0;
	int32 NextGeneration = 0;
	int32 FailedListings = 0;

	/** The run that drains host calls first; it rotates each frame. */
	uint32 DrainStart = 0;
	bool bListing = false;

	/** The listing was refused or kept failing: nothing starts until one reads. */
	bool bListingLost = false;

	/** List again as soon as the listing in flight answers. */
	bool bListAgain = false;

#if WITH_DEV_AUTOMATION_TESTS
	double NowForTest = -1.0;
	double HostBudgetMsForTest = -1.0;
	int32 UnsentEditsForTest = -1;
#endif
};
