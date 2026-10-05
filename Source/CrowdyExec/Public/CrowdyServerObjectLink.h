#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectTypes.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "UObject/Object.h"
#include "CrowdyServerObjectLink.generated.h"

class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectSubsystem;
class UCrowdyTeams;

/** Follows one instance of a Server Object type for an owner such as a widget, waiting for sign-in or teams and rejoining after a sign-in. */
UCLASS(BlueprintType)
class CROWDYEXEC_API UCrowdyServerObjectLink : public UObject
{
	GENERATED_BODY()

public:
	/** The Server Object followed, or None while it waits or could not join. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	UCrowdyServerObject* GetServerObject() const { return Object; }

	/** Why there is no Server Object yet, or empty. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	FString GetFailureReason() const;

	const UCrowdyServerObjectDefinition* GetDefinition() const { return Definition; }

	/** Handlers bound to one variable of whatever Server Object this Link follows, run again for each object it joins. */
	FCrowdyServerVariableBindings& GetVariableBindings() { return VariableBindings; }

private:
	friend class UCrowdyServerObjectSubsystem;
	friend struct FCrowdyServerObjectLinkTestAccess;

	void Start(UObject* InOwner, UCrowdyServerObjectDefinition* InDefinition, ECrowdyServerObjectFind InFind, const FString& InInstanceId, int64 InTeamId);

	/** Lets go of the object and of every event it listens to. */
	void Stop();

	bool IsOwnerAlive() const { return Owner.IsValid(); }

	/** Asks for the teams again while it waits for them, at most every TeamsRetrySeconds. */
	void Tick(float DeltaSeconds);

	/** Arrived is the teams just received, or null to read the teams cache. */
	void Join(const TArray<FCrowdyTeamMembership>* Arrived = nullptr);
	void Leave();
	FString ResolveInstanceId(FString& OutError, const TArray<FCrowdyTeamMembership>* Arrived = nullptr) const;
	void RequestTeams();

	UCrowdyServerObjectSubsystem* GetSubsystem() const;
	UCrowdyTeams* FindTeams() const;

	UFUNCTION()
	void HandleLogin(bool bSuccess, FString Message);

	void HandleSignedIn();

	UFUNCTION()
	void HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships);

	void HandleVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields);

	TWeakObjectPtr<UObject> Owner;

	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	ECrowdyServerObjectFind Find = ECrowdyServerObjectFind::InstanceId;
	FString InstanceId;
	int64 TeamId = 0;

	UPROPERTY(Transient)
	TObjectPtr<UCrowdyServerObject> Object;

	FDelegateHandle ValuesHandle;
	FString JoinError;
	FCrowdyServerVariableBindings VariableBindings;

	/** Seconds since the subsystem last handed this Link out. */
	float SinceFound = 0.f;

	/** Seconds before the teams may be asked for again. */
	float TeamsRetryIn = 0.f;
	int32 TeamsRequests = 0;

	bool bLoginBound = false;
	bool bTeamsBound = false;

	static constexpr float TeamsRetrySeconds = 5.f;
};
