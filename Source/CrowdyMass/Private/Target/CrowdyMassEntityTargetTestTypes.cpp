#include "Target/CrowdyMassEntityTargetTestTypes.h"

// Every body below is empty: these receivers exist only to be resolved by reflection and addressed by
// member-function pointer (MarshalCall / CheckEventClass never dispatch through ProcessEvent in the tests
// that use them), so there is nothing to record.

void UCrowdyMassEventClassCheckBaseTestTarget::CheckEvent_Implementation(int32 InValue)
{
}

void ACrowdyMassEventParityTestTarget::ParityEvent_Implementation(int32 InAmmo, float InSpeed, const FString& InLabel, FVector InDir)
{
}

void ACrowdyMassEventParityTestTarget::HostRecipientEvent_Implementation(int32 InValue)
{
}

void ACrowdyMassEventParityTestTarget::MulticastRecipientEvent_Implementation(int32 InValue)
{
}

void ACrowdyMassEventParityTestTarget::SpatialRecipientEvent_Implementation(int32 InValue)
{
}

void ACrowdyMassEventParityTestTarget::NotAnEvent_Implementation(int32 InValue)
{
}

void UCrowdyMassEventClassCheckTestComponent::ComponentEvent_Implementation(int32 InValue)
{
}
