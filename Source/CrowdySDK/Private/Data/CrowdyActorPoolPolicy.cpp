// Fill out your copyright notice in the Description page of Project Settings.


#include "Data/CrowdyActorPoolPolicy.h"

void UCrowdyActorPoolPolicy::OnActorPooled_Implementation(AActor* Actor)
{
	// Default: just hide it
	if (IsValid(Actor))
	{
		Actor->SetActorHiddenInGame(true);
		Actor->SetActorTickEnabled(false);
		Actor->SetActorEnableCollision(false);
	}
}

void UCrowdyActorPoolPolicy::OnActorActivated_Implementation(AActor* Actor, const FInstancedStruct& InitialState)
{
	if (IsValid(Actor))
	{
		Actor->SetActorHiddenInGame(false);
		Actor->SetActorTickEnabled(true);
		Actor->SetActorEnableCollision(true);
	}
}

void UCrowdyActorPoolPolicy::OnActorDeactivated_Implementation(AActor* Actor)
{
	if (IsValid(Actor))
	{
		Actor->SetActorHiddenInGame(true);
		Actor->SetActorTickEnabled(false);
		Actor->SetActorEnableCollision(false);
	}
}
