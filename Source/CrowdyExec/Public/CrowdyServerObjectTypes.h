#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "CrowdyServerObjectTypes.generated.h"

class UCrowdyServerObjectDefinition;

UENUM(BlueprintType)
enum class ECrowdyServerObjectStatus : uint8
{
	/** Joining the Server Object and reading its watched values for the first time. */
	Connecting,
	/** The watched values are current and functions can be called. */
	Ready,
	/** It cannot be used; the failure reason says why. Acquiring it again tries afresh. */
	Failed,
	/** Given back. Nothing on it works any more. */
	Released
};

UENUM(BlueprintType)
enum class ECrowdyServerCallOutcome : uint8
{
	Success,
	/** The server code refused the call or failed; the reason carries its message. */
	ServerError,
	/** The server stayed busy through every retry. */
	Busy,
	/** The server could not be reached. */
	Unavailable,
	/** This Server Object type is not deployed on the server. */
	NotDeployed,
	/** The server did not answer in time. */
	Timeout,
	/** This player may not call that function on this Server Object. */
	Denied,
	/** The server code crashed while handling the call; its log says why. */
	ServerCrashed,
	/** The call could not be sent as asked: an unknown function, or params of the wrong type. */
	BadRequest,
	/** The Server Object was given back, or the player signed out, before the call finished. */
	Canceled
};

/** Which instance of a Server Object type to find, when it is found by its definition. */
UENUM(BlueprintType)
enum class ECrowdyServerObjectFind : uint8
{
	/** The Instance Id given. */
	InstanceId UMETA(DisplayName = "Instance Id"),
	/** The signed-in player's user id: each player gets their own. */
	SignedInPlayer UMETA(DisplayName = "Signed-In Player"),
	/** The signed-in player's Crowdy Team id; Team Id picks one when they are in several. */
	PlayersTeam UMETA(DisplayName = "Player's Team")
};

USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerCallResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Server Object")
	ECrowdyServerCallOutcome Outcome = ECrowdyServerCallOutcome::Canceled;

	/** The function's Reply struct, on Success. Empty for a function without one. */
	UPROPERTY(BlueprintReadOnly, Category = "Server Object")
	FInstancedStruct Reply;

	/** A plain sentence saying what went wrong, for anything but Success. */
	UPROPERTY(BlueprintReadOnly, Category = "Server Object")
	FString Reason;

	/** Calling again later may succeed. */
	UPROPERTY(BlueprintReadOnly, Category = "Server Object")
	bool bRetryable = false;

	bool IsSuccess() const { return Outcome == ECrowdyServerCallOutcome::Success; }
};

/** Identifies one Server Object: its type and its Instance Id. Instance Ids compare case-sensitively. */
USTRUCT()
struct CROWDYEXEC_API FCrowdyServerObjectKey
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	UPROPERTY()
	FString InstanceId;

	bool operator==(const FCrowdyServerObjectKey& Other) const
	{
		return Definition == Other.Definition && InstanceId.Equals(Other.InstanceId, ESearchCase::CaseSensitive);
	}

	friend uint32 GetTypeHash(const FCrowdyServerObjectKey& Key)
	{
		return HashCombine(GetTypeHash(Key.Definition), GetTypeHash(Key.InstanceId));
	}
};

/** Identifies one Server Object Link: its owner and the arguments it was found by. */
USTRUCT()
struct CROWDYEXEC_API FCrowdyServerObjectLinkKey
{
	GENERATED_BODY()

	UPROPERTY()
	TWeakObjectPtr<UObject> Owner;

	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	UPROPERTY()
	ECrowdyServerObjectFind Find = ECrowdyServerObjectFind::InstanceId;

	UPROPERTY()
	FString InstanceId;

	UPROPERTY()
	int64 TeamId = 0;

	bool operator==(const FCrowdyServerObjectLinkKey& Other) const
	{
		return Owner == Other.Owner && Definition == Other.Definition && Find == Other.Find && TeamId == Other.TeamId
			&& InstanceId.Equals(Other.InstanceId, ESearchCase::CaseSensitive);
	}

	friend uint32 GetTypeHash(const FCrowdyServerObjectLinkKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.Owner), GetTypeHash(Key.Definition)), HashCombine(GetTypeHash(Key.InstanceId), GetTypeHash(Key.TeamId + static_cast<int64>(Key.Find))));
	}
};
