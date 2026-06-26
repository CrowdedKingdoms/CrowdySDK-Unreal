#pragma once

#include "CoreMinimal.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "CrowdyRpcTestTarget.generated.h"

class AActor;

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
	// test confirms it registers — an array of objects is supported once the per-element codec
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

	UObject* GotObject = nullptr;
	UClass* GotClass = nullptr;

	TArray<UClass*> GotClasses;
	TArray<AActor*> GotActors;
};
