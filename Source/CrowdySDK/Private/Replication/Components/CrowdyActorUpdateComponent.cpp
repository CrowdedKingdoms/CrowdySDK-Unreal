// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Components/CrowdyActorUpdateComponent.h"

#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/HelperFunctions.h"


// Sets default values for this component's properties
UCrowdyActorUpdateComponent::UCrowdyActorUpdateComponent()
{
	// Set this component to be initialized when the game starts and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = false;
	
}


// Called when the game starts
void UCrowdyActorUpdateComponent::BeginPlay()
{
	Super::BeginPlay();

	AutoReplicator = GetWorld()->GetSubsystem<UCrowdyAutoReplicator>();
	
	
	check(AutoReplicator);
	check(ActorUpdateExecutor)
	
	if (!IsValid(AutoReplicator))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Update Component]: Invalid AutoReplicator subsystem."));
		return;
	}
	
	if (!IsValid(ActorUpdateExecutor.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Update Component]: Invalid ActorUpdateExecutor subsystem."));
		 return;
	}
	
	
	CachedOwner = GetOwner();
	
	SetupID();
	
	if (bAutoStartReplication)
		AutoReplicator->RegisterReplicationComponent(this);
	
}

void UCrowdyActorUpdateComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsValid(AutoReplicator))
		AutoReplicator->UnregisterReplicationComponent(this);
	
	Super::EndPlay(EndPlayReason);
}




// Called every frame
void UCrowdyActorUpdateComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UCrowdyActorUpdateComponent::StartReplication()
{
	AutoReplicator->RegisterReplicationComponent(this);
}

void UCrowdyActorUpdateComponent::StopReplication()
{
	AutoReplicator->UnregisterReplicationComponent(this);
}

void UCrowdyActorUpdateComponent::SetupID()
{
	UCrowdyGameSession* GameSession = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();
	if (!ensureMsgf(IsValid(GameSession), TEXT("[Crowdy Actor Update Component]: Invalid GameSession subsystem.")))
	{
		return;
	}
	
	bool bIsOwnerPlayer = false;
	
	if (const APawn* Pawn = Cast<APawn>(CachedOwner))
	{
		if (const APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
		{
			if (PC->IsLocalController() && PC->IsPrimaryPlayer())
			{
				bIsOwnerPlayer = true;
				
				if (bUseDeterministicID)
					UUID = UHelperFunctions::GetDeterministicID(GameSession->GetUserID()).ToString(EGuidFormats::Digits);
				else
					UUID = UHelperFunctions::GetNewUUID();
				
				ID = USerializationFunctionLibrary::ToGuid(UUID);
				GameSession->SetUUID(UUID);
			}
		}
	}
	
	if (!bIsOwnerPlayer && bUseDeterministicID)
	{
		UUID = UHelperFunctions::GetDeterministicID(Seed).ToString(EGuidFormats::Digits);
		ID = USerializationFunctionLibrary::ToGuid(UUID);
	}
	else if (!bIsOwnerPlayer)
	{
		UUID = UHelperFunctions::GetNewUUID();
		ID = USerializationFunctionLibrary::ToGuid(UUID);
	}
	
}
