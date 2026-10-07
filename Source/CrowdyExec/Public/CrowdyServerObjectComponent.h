#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CrowdyServerObject.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "CrowdyServerObjectComponent.generated.h"

class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectSubsystem;

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
	FromServerValue UMETA(DisplayName = "From Server Value")
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
 * waits for sign-in, and joins again after a sign-in or a new account that follows a sign-out.
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

	/** The Server Object this component holds, or None before it joins one or after it could not. */
	UFUNCTION(BlueprintPure, Category = "Server Object")
	UCrowdyServerObject* GetServerObject() const { return Object; }

	/** Why there is no usable Server Object, or empty. */
	UFUNCTION(BlueprintPure, Category = "Server Object")
	FString GetFailureReason() const;

	/** Joins the Server Object again, after it Failed or after changing Definition or Instance Id. Ignored while it is joining. */
	UFUNCTION(BlueprintCallable, Category = "Server Object")
	void Rejoin();

	/** The Instance Id InstanceMode gives, or empty with OutError saying why. Only One Instance types always give theirs. */
	FString ResolveInstanceId(FString& OutError) const;

	/** Handlers bound to one variable of whatever Server Object this component holds, run again for each object it joins. */
	FCrowdyServerVariableBindings& GetVariableBindings() { return VariableBindings; }

	/** The game instance's Server Objects, or null outside a game. */
	UCrowdyServerObjectSubsystem* FindServerObjects() const;

protected:
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend struct FCrowdyServerObjectComponentTestAccess;

	void Join();
	void Leave();
	void Refuse(const FString& Reason);

	UFUNCTION()
	void HandleLogin(bool bSuccess, FString Message);

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

	bool bTeamsBound = false;
	bool bTeamsRequested = false;

	FCrowdyServerVariableBindings VariableBindings;

	/** From Server Value, unless the type has Only One Instance. */
	bool FollowsSourceValue() const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for the game instance's Server Objects, which a component outside a world cannot find. */
	TWeakObjectPtr<UCrowdyServerObjectSubsystem> ServerObjectsForTest;
#endif
};
