#include "Dev/CrowdyMassDispatchProxyTestTypes.h"

#include "Animation/AnimInstance.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"

ACrowdyMassDispatchProxyTestActor::ACrowdyMassDispatchProxyTestActor()
{
	PrimaryActorTick.bCanEverTick = false;

	ProxyRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ProxyRoot"));
	SetRootComponent(ProxyRoot);

	ProxyMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("ProxyMesh"));
	ProxyMesh->SetupAttachment(ProxyRoot);
}

void ACrowdyMassDispatchProxyTestActor::ProxyBody(const int32 InValue)
{
	++BodyRunCount;
	LastValue = InValue;

	bBodySawWorld = GetWorld() != nullptr;
	bBodySawMesh = ProxyMesh != nullptr;
	bBodySawRegisteredMesh = ProxyMesh && ProxyMesh->IsRegistered();
	bBodySawAnimInstance = ProxyMesh && ProxyMesh->GetAnimInstance() != nullptr;
	BodySawLocation = GetActorLocation();
}

void ACrowdyMassDispatchProxyTestActor::ProxyPresent_Implementation(const int32 Value)
{
	// Read BEFORE it is written, which is the only order that can tell a reset apart from an overwrite:
	// recording it afterwards would show this dispatch's own value whether or not the previous entity's
	// survived.
	PresentSawCarriedValueOnEntry = CarriedValue;

	++PresentRunCount;
	CarriedValue = Value;
	PresentSawLocation = GetActorLocation();
}
