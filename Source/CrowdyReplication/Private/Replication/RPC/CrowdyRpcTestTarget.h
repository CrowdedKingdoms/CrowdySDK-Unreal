#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "CrowdyRpcTestTarget.generated.h"

/**
 * A struct that buries a container. Used only by the struct-buried-container rejection test: a
 * container reached through a struct cannot be bounded on decode (once inside the struct's
 * SerializeItem the untrusted element count drives an allocation), so such a parameter is rejected
 * at registration.
 */
USTRUCT()
struct FCrowdyRpcNestedContainer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> Values;
};

/**
 * Receiver used by the RPC serializer automation tests. Each receiver records what
 * it was invoked with so a test can drive SendChecked and assert the values that
 * survived the marshal / unmarshal / ProcessEvent round-trip.
 */
UCLASS()
class UCrowdyRpcTestTarget : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION()
	void Primitives_Implementation(bool bInFlag, int32 InI32, int64 InI64, float InF, double InD,
		const FString& InStr, FName InName);

	UFUNCTION()
	void Structs_Implementation(FVector InVector, FRotator InRotator, FTransform InTransform);

	UFUNCTION()
	void NoArgs_Implementation();

	// Same parameter set as a subset of Primitives but a different order, used to
	// prove the FunctionID changes when the signature changes.
	UFUNCTION()
	void Reordered_Implementation(int32 InI32, bool bInFlag);

	// The receiver is a literal reflected UFUNCTION carrying the routing metadata;
	// CROWDY_EVENT adds the call-site thunk. The send tests confirm the receiver
	// reflects, its routing resolves, and the thunk marshals/routes correctly.
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient",
		CrowdyDecay = "Exponential_Decay", CrowdyDistance = "Four_Chunks"))
	void MacroEvent_Implementation(int32 InAmmo, FVector InDir);

	CROWDY_EVENT(MacroEvent)

	// Invalid signatures used by the registration-rejection test. They are deliberately
	// not tagged meta=(CrowdyEvent) so the live scan ignores them; the test calls the
	// signature validator on each directly to confirm it reports a readable problem.
	UFUNCTION()
	bool ReturnsValue_Implementation(int32 InValue);

	UFUNCTION()
	void OutParam_Implementation(int32 InValue, int32& OutResult);

	UFUNCTION()
	void ObjectParam_Implementation(UObject* InObject);


	// Tier 1 (containers): a receiver whose parameters are containers of supported leaf
	// types, used by the container round-trip test and by the canonical-type regression
	// (its TArray<int32> and TArray<FVector> params must canonicalize differently).
	UFUNCTION()
	void Containers_Implementation(const TArray<int32>& InInts, const TArray<FVector>& InVecs,
		const TMap<FName, int32>& InMap);

	// Tier 3 (containers of object references): an array of bare object pointers. The validation
	// test confirms it registers an array of objects is supported once the per-element codec
	// exists; the actor/class array receivers below cover the actual encode/decode round-trip.
	UFUNCTION()
	void ObjectArray_Implementation(const TArray<UObject*>& InObjects);

	// Single const-ref array, used by the out-parm-frame test: it stands in for a Blueprint
	// event whose array input the VM keeps in the out-parm records instead of the locals block.
	UFUNCTION()
	void SingleIntArray_Implementation(const TArray<int32>& In);

	// Tier 2 (object references): receivers for the object-identity round-trips and the
	// canonical-distinctness regression. ObjectParam (UObject*) doubles as the null and asset target.
	UFUNCTION()
	void ClassRef_Implementation(UClass* InClass);

	UFUNCTION()
	void ActorRef_Implementation(AActor* InActor);

	// Tier 3 (containers of object references): array receivers for the round-trips. The class array
	// resolves by path with no world or subsystem; the actor array needs a live entity subsystem and
	// is exercised in the manual PIE matrix (the codec mechanics are covered world-lessly by the class
	// array). ObjectMap stands in for the still-rejected set/map-of-objects case.
	UFUNCTION()
	void ClassArray_Implementation(const TArray<UClass*>& InClasses);

	UFUNCTION()
	void ActorArray_Implementation(const TArray<AActor*>& InActors);

	UFUNCTION()
	void ObjectMap_Implementation(const TMap<FName, AActor*>& InMap);

	// Single-container receivers whose one parameter sits right after the version byte, so a test can
	// hand-craft a blob with a forged element count and assert a clean drop. IntSet also drives the
	// set snapshot round-trip. NameIntMap mirrors the map arm of Containers as a single param.
	UFUNCTION()
	void IntSet_Implementation(const TSet<int32>& In);

	UFUNCTION()
	void NameIntMap_Implementation(const TMap<FName, int32>& In);

	// Map with a struct value, so the map snapshot's value path (a struct serialized through the
	// structured element stream) is round-trip covered directly, not just the int-valued map.
	UFUNCTION()
	void NameVecMap_Implementation(const TMap<FName, FVector>& In);

	// A struct parameter that buries a container. Not tagged CrowdyEvent, so the live scan ignores it;
	// the rejection test calls the signature validator on it directly (mirrors ReturnsValue/OutParam).
	UFUNCTION()
	void StructWithContainer_Implementation(const FCrowdyRpcNestedContainer& In);

	int32 CallCount = 0;

	bool GotFlag = false;
	int32 GotI32 = 0;
	int64 GotI64 = 0;
	float GotF = 0.f;
	double GotD = 0.0;
	FString GotStr;
	FName GotName = NAME_None;
	FVector GotVector = FVector::ZeroVector;
	FRotator GotRotator = FRotator::ZeroRotator;
	FTransform GotTransform = FTransform::Identity;

	TArray<int32> GotInts;
	TArray<FVector> GotVecs;
	TMap<FName, int32> GotMap;
	TMap<FName, FVector> GotVecMap;
	TSet<int32> GotSet;

	UObject* GotObject = nullptr;
	UClass* GotClass = nullptr;

	TArray<UClass*> GotClasses;
	TArray<AActor*> GotActors;
};

/**
 * Subsystem RPC fixture (Subsystem Replication Phase 2): a plain UObject (NOT an actor), standing in for a
 * host-owned subsystem participant that sends and receives RPC CrowdyEvents over the reliable channel. Each
 * receiver records the value and bumps a call count so a test can assert it ran; a plain UObject fires
 * ProcessEvent without a world, so the channel apply tests need no editor world.
 */
UCLASS()
class UCrowdyRpcSubsystemTestTarget : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void SubMulticast_Implementation(int32 InValue);
	CROWDY_EVENT(SubMulticast)

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Host"))
	void SubHostOnly_Implementation(int32 InValue);
	CROWDY_EVENT(SubHostOnly)

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient"))
	void SubOwnerOnly_Implementation(int32 InValue);
	CROWDY_EVENT(SubOwnerOnly)

	int32 GotValue = 0;
	int32 CallCount = 0;
};

/**
 * Actor RPC fixture used only by the actor-path regression test: an owner-only CrowdyEvent on an AActor, so the
 * receive gate's actor branch (owner/host-only broadcasts dropped, targeted sends run) can be exercised. An
 * AActor's ProcessEvent no-ops without a world, so that test spawns this into an editor world.
 */
UCLASS()
class ACrowdyRpcActorTestTarget : public AActor
{
	GENERATED_BODY()

public:

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient"))
	void ActorOwnerOnly_Implementation(int32 InValue);
	CROWDY_EVENT(ActorOwnerOnly)

	int32 GotValue = 0;
	int32 CallCount = 0;
};
