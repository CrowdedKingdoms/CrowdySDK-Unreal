#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/Ticker.h"
#include "Engine/TimerHandle.h"
#include "CrowdyServerObject.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "CrowdyServerObjectComponent.generated.h"

class UCrowdyEntityComponent;
class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectSubsystem;
class APawn;
class FDoubleProperty;

/** Location is in Unreal units, at the owner's height. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyPositionKept, FVector, Location);

UENUM(BlueprintType)
enum class ECrowdyServerObjectInstanceMode : uint8
{
	/** The Instance Id typed on the component: every actor with the same one shares a Server Object. */
	InstanceId UMETA(DisplayName = "Instance Id"),
	/** The signed-in player's user id: each player gets their own. Use it for Owner Only types. */
	SignedInPlayer UMETA(DisplayName = "Signed-In Player"),
	/** This actor's placement in the level: one per placed actor, the same on every player's machine. */
	ThisActor UMETA(DisplayName = "This Actor"),
	/** The signed-in player's Crowdy Team id (Team Id picks one when they are in several). For Crowdy Team members. */
	PlayersTeam UMETA(DisplayName = "Player's Team"),
	/** A variable of another Server Object, such as a player's TeamId, followed as it changes. */
	FromServerValue UMETA(DisplayName = "From Server Value"),
	/** The area this actor stands in, by the definition's Area Size, followed as the actor moves. */
	ThisActorsArea UMETA(DisplayName = "This Actor's Area")
};

/** Which actor a Crowdy Server Object component reports to a starter that tracks players. */
UENUM(BlueprintType)
enum class ECrowdyPositionReport : uint8
{
	/** The owner when it is a pawn this game's player controls. */
	PlayerPawn UMETA(DisplayName = "Player Pawn"),
	/** This actor wherever it is, for an owner that stands in for the player. */
	ThisActor UMETA(DisplayName = "This Actor"),
	Off
};

/** Which instance of the Source Definition a From Server Value component reads. */
UENUM(BlueprintType)
enum class ECrowdyServerObjectSourceInstance : uint8
{
	InstanceId UMETA(DisplayName = "Instance Id"),
	SignedInPlayer UMETA(DisplayName = "Signed-In Player")
};

/**
 * Holds one Server Object for its actor from BeginPlay to EndPlay and passes on its events. With Signed-In Player it
 * waits for sign-in, and joins again after a sign-in or a new account that follows a sign-out. For a definition that
 * holds a record per entity, its variables are those of the record of its actor's Crowdy entity.
 */
UCLASS(ClassGroup = (Crowdy), meta = (BlueprintSpawnableComponent, DisplayName = "Crowdy Server Object"))
class CROWDYEXEC_API UCrowdyServerObjectComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdyServerObjectComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object")
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	/** Which Server Object of that type this actor uses. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object")
	ECrowdyServerObjectInstanceMode InstanceMode = ECrowdyServerObjectInstanceMode::InstanceId;

	/** Left empty on a Game Kit starter keyed by player, such as player, it is the signed-in player's own. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (EditCondition = "InstanceMode == ECrowdyServerObjectInstanceMode::InstanceId", EditConditionHides))
	FString InstanceId;

	/** The team to use when the player is in several; 0 uses their only team. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Server Object", meta = (EditCondition = "InstanceMode == ECrowdyServerObjectInstanceMode::PlayersTeam", EditConditionHides))
	int64 TeamId = 0;

	/** The Server Object type whose variable gives the Instance Id. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (EditCondition = "InstanceMode == ECrowdyServerObjectInstanceMode::FromServerValue", EditConditionHides))
	TObjectPtr<UCrowdyServerObjectDefinition> SourceDefinition;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (EditCondition = "InstanceMode == ECrowdyServerObjectInstanceMode::FromServerValue", EditConditionHides))
	ECrowdyServerObjectSourceInstance SourceInstance = ECrowdyServerObjectSourceInstance::SignedInPlayer;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (EditCondition = "InstanceMode == ECrowdyServerObjectInstanceMode::FromServerValue && SourceInstance == ECrowdyServerObjectSourceInstance::InstanceId", EditConditionHides))
	FString SourceInstanceId;

	/** The variable holding the Instance Id, a String or an integer; while it is empty the component waits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (EditCondition = "InstanceMode == ECrowdyServerObjectInstanceMode::FromServerValue", EditConditionHides))
	FName SourceVariable;

	/** After every change of the watched variables, and once when they are first read. */
	UPROPERTY(BlueprintAssignable, Category = "Server Object")
	FCrowdyServerVariablesChanged OnVariablesChanged;

	/** Object is None when it could not be joined; Get Failure Reason says why. */
	UPROPERTY(BlueprintAssignable, Category = "Server Object")
	FCrowdyServerStatusChanged OnStatusChanged;

	/** Sends an actor's location to a starter that tracks players (its Position Function) while the Server Object is Ready. By default only the pawn this game's player controls is reported, and only by one component per Server Object. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Server Object|Position")
	ECrowdyPositionReport PositionReport = ECrowdyPositionReport::PlayerPawn;

	/** Moves the owner by the correction the server made when it cut a move short or refused it, keeping the owner's height and any movement made since the report. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Server Object|Position")
	bool bSnapToKeptPosition = true;

	/** The server kept another position than the one reported; Location is the owner's location with that correction applied (the owner is moved there when Snap To Kept Position is on). */
	UPROPERTY(BlueprintAssignable, Category = "Server Object|Position")
	FOnCrowdyPositionKept OnPositionKept;

	/** The Server Object this component holds, or None before it joins one or after it could not. */
	UFUNCTION(BlueprintPure, Category = "Server Object")
	UCrowdyServerObject* GetServerObject() const { return Object; }

	/** Why there is no usable Server Object, or empty. */
	UFUNCTION(BlueprintPure, Category = "Server Object")
	FString GetFailureReason() const;

	/** The variables the server last sent: the Server Object's, or for a record per entity this actor's record. Their defaults until they are read. */
	UFUNCTION(BlueprintPure, Category = "Server Object")
	FInstancedStruct GetVariables() const;

	/** Whether the definition holds a record per entity, so the variables are this actor's record. */
	bool ReadsRecord() const;

	/** The record this actor's variables are: its Crowdy entity's id, or empty while it has none. */
	const FString& GetRecordId() const { return RecordId; }

	/** The object the variables are read from: the held one, or after a move to another area, the area left until the new one holds this actor's record. */
	UCrowdyServerObject* GetReadObject() const;

	/** Joins the Server Object again, after it Failed or after changing Definition or Instance Id. Ignored while it is joining. */
	UFUNCTION(BlueprintCallable, Category = "Server Object")
	void Rejoin();

	/** The Instance Id InstanceMode gives, or empty with OutError saying why. Only One Instance types always give theirs. */
	FString ResolveInstanceId(FString& OutError) const;

	/** Handlers bound to one variable of whatever Server Object this component holds, run again for each object it joins. */
	FCrowdyServerVariableBindings& GetVariableBindings() { return VariableBindings; }

	/** Handlers bound to one message of whatever Server Object this component holds; the component listens to each message on every object it joins. */
	FCrowdyServerMessageBindings& GetMessageBindings() { return MessageBindings; }

	/** The game instance's Server Objects, or null outside a game. */
	UCrowdyServerObjectSubsystem* FindServerObjects() const;

protected:
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend struct FCrowdyServerObjectComponentTestAccess;
	friend struct FCrowdyServerObjectRecordsTestAccess;
	friend struct FCrowdyServerObjectMessageTestAccess;
	friend struct FCrowdyServerObjectPositionTestAccess;
	friend class UCrowdyServerObjectSubsystem;

	void Join();
	void Leave();
	/** Reports the reason as Failed; bWarn also logs it. */
	void Refuse(const FString& Reason, bool bWarn = true);

	/** Run by the Server Objects subsystem after each sign-in or registration. */
	void HandleLogin(bool bSuccess);

	/** What a sign-in changes once the component has begun play. */
	void HandleSignedIn();

	UFUNCTION()
	void HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships);

	/** From Server Value: holds the source object and joins again whenever its value changes. */
	void WatchSource();
	void StopWatchingSource();
	void HandleSourceVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields);
	void HandleSourceStatus(UCrowdyServerObject* InObject, ECrowdyServerObjectStatus Status);

	void HandleVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields);
	void HandleMessage(UCrowdyServerObject* InObject, FName Message, const FInstancedStruct& Payload);
	void HandleStatus(UCrowdyServerObject* InObject, ECrowdyServerObjectStatus Status);

	UPROPERTY(Transient)
	TObjectPtr<UCrowdyServerObject> Object;

	/** Read at registration: a cooked level releases it before BeginPlay. */
	FGuid PlacementGuid;

	FString JoinError;
	FDelegateHandle ValuesHandle;
	FDelegateHandle StatusHandle;
	bool bLoginBound = false;

	/** Set while Join runs: its events run Blueprint code, which must not start another Join inside it. */
	bool bJoining = false;

	UPROPERTY(Transient)
	TObjectPtr<UCrowdyServerObject> SourceObject;

	FDelegateHandle SourceValuesHandle;
	FDelegateHandle SourceStatusHandle;

	/** The Instance Id the source last gave, so an unchanged value does not rejoin. */
	FString SourceValue;

	/** Starts the position reports on the core ticker while Held is Ready, or stops them; the core ticker keeps running while the game is paused, and the component's own tick stays the user's. */
	void SetPositionArmed(bool bArmed);

	/** Finds the Position Function's inputs and reply and their X and Y once, so a report writes and reads them without a name search. */
	bool ResolvePositionAxes();

	/** One ticker fire: sends the owner's location unless a report is out, the owner stands still, or a refusal holds reports. */
	void ReportPosition();
	bool TickPosition(float DeltaSeconds);
	void HandlePositionAnswer(uint32 Serial, const FVector& Sent, const FCrowdyServerCallResult& Result);

	/** Whether PositionReport says this owner is the one to report right now. */
	bool ShouldReportOwner() const;

	/** Lets another component on the same Server Object report, when this one was the reporter. */
	void ReleasePositionClaim();
	double PositionNow() const;

	static constexpr double PositionRefusalSeconds = 1.0;
	static constexpr double PositionStillSeconds = 1.0;
	static constexpr double PositionCodedRefusalSeconds = 5.0;

	FTSTicker::FDelegateHandle PositionTicker;
	FName PositionFunction;
	UPROPERTY(Transient)
	FInstancedStruct PositionInputs;
	const FDoubleProperty* PositionInX = nullptr;
	const FDoubleProperty* PositionInY = nullptr;
	const FDoubleProperty* PositionReplyX = nullptr;
	const FDoubleProperty* PositionReplyY = nullptr;
	const UScriptStruct* PositionReplyStruct = nullptr;
	uint32 PositionGeneration = 0;
	uint32 PositionSerial = 0;
	TWeakObjectPtr<const APawn> PositionPawn;
	bool bPositionInFlight = false;
	bool bPositionSent = false;

	/** Whether the hold follows a coded refusal, which a change of the State shortens to the plain hold. */
	bool bPositionHoldCoded = false;
	double PositionRefusedAt = 0.0;
	float PositionPeriod = 0.2f;
	double PositionSentAt = 0.0;
	double PositionHoldUntil = 0.0;
	FVector PositionLast = FVector::ZeroVector;

	bool bTeamsBound = false;
	float TeamsRetryIn = 0.f;
	int32 TeamsRequests = 0;
	static constexpr float TeamsRetrySeconds = 5.f;

	/** While it waits for the teams, asks again every TeamsRetrySeconds from a world timer, so the component's own tick stays the user's. */
	FTimerHandle TeamsRetryTimer;
	bool bTeamsRetryArmed = false;

	void RequestTeams();
	void TickTeamsRetry(float DeltaSeconds);
	void SetTeamsRetryArmed(bool bArmed);

	FCrowdyServerVariableBindings VariableBindings;
	FCrowdyServerMessageBindings MessageBindings;

	/** From Server Value, unless the type has Only One Instance. */
	bool FollowsSourceValue() const;

	/** Signed-In Player, or a Game Kit starter keyed by player with no Instance Id, which is the signed-in player's own. */
	bool FollowsSignedInPlayer() const;

	/** This Actor's Area's Instance Id, or empty with OutError saying why. */
	FString ResolveAreaKey(FString& OutError) const;

	/** Whether the component must look again every FollowSeconds: for the actor's area, or for its entity's id until it has one. */
	bool NeedsFollow() const;

	/** Joins the new area's object when the actor moved to another area, and watches the record of a new entity id. */
	void TickFollow();

	/** Whether the actor stands in another chunk than at the last look; false while it has no chunk. */
	bool MovedChunk();

	/** Whether Held is Ready and holds this actor's record. */
	bool HoldsOwnRecord(const UCrowdyServerObject* Held) const;

	/** The area left behind, still read while the held object does not hold this actor's record yet; not an owner of it. */
	UPROPERTY(Transient)
	TObjectPtr<UCrowdyServerObject> Previous;

	int64 FollowedChunkX = 0;
	int64 FollowedChunkY = 0;
	bool bFollowedChunk = false;

	/** Watches the record of the entity's current id, once it has one. */
	void FollowRecordId();

	void SetFollowArmed(bool bArmed);

	static constexpr float FollowSeconds = 0.5f;
	FTimerHandle FollowTimer;

	/** The entity whose id names the record, found at join. */
	TWeakObjectPtr<const UCrowdyEntityComponent> Entity;
	FGuid RecordNetId;
	FString RecordId;
	FDelegateHandle RecordHandle;

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for the game instance's Server Objects, which a component outside a world cannot find. */
	TWeakObjectPtr<UCrowdyServerObjectSubsystem> ServerObjectsForTest;

	/** The clock of a component outside a world, which has no world time to report by. */
	double PositionTimeForTest = 0.0;

	/** Lets a component outside a world arm its position ticker. */
	bool bPositionTickerForTest = false;
#endif
};
