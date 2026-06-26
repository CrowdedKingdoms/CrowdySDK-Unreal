// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "CrowdyReplicationLog.h"

void UCrowdyActorPoolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyActorPoolSubsystem::Deinitialize()
{
	for (auto& [Class, Pool] : Pools)
	{
		for (FSlot& Slot : Pool.Slots)
		{
			if (!Slot.Actor.IsValid())
				continue;

			if (Slot.bActive && Pool.Policy && GetWorld() && !GetWorld()->bIsTearingDown)
				Pool.Policy->OnActorDeactivated(Slot.Actor.Get());

			AActor* Actor = Slot.Actor.Get();
			if (IsValid(Actor))
				Actor->Destroy();

			Slot.Actor = nullptr;
		}

		Pool.Slots.Empty();
		Pool.Policy = nullptr;
	}

	Pools.Empty();

	Super::Deinitialize();
}

bool UCrowdyActorPoolSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
		return false;

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
		return false;

	return World->WorldType == EWorldType::PIE
		|| World->WorldType == EWorldType::Game;
}

void UCrowdyActorPoolSubsystem::RegisterPool(const FCrowdyPoolConfig& Config)
{
	if (!Config.ActorClass || Pools.Contains(Config.ActorClass.Get()))
		return;

	const TSubclassOf<UCrowdyActorPoolPolicy> PolicyClass = Config.PoolPolicyClass
		? Config.PoolPolicyClass
		: TSubclassOf<UCrowdyActorPoolPolicy>(UCrowdyActorPoolPolicy::StaticClass());

	FPool& Pool     = Pools.Add(Config.ActorClass.Get());
	Pool.ActorClass = Config.ActorClass;
	Pool.Policy     = NewObject<UCrowdyActorPoolPolicy>(this, PolicyClass);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	Pool.Slots.Reserve(Config.PoolSize);
	for (int32 i = 0; i < Config.PoolSize; i++)
	{
		AActor* Actor = GetWorld()->SpawnActor<AActor>(Config.ActorClass, FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor) continue;

		Pool.Policy->OnActorPooled(Actor);

		FSlot Slot;
		Slot.Actor   = Actor;
		Slot.bActive = false;
		Pool.Slots.Add(Slot);
	}
}

AActor* UCrowdyActorPoolSubsystem::AcquireActor(const TSubclassOf<AActor> ActorClass)
{
	FPool* Pool = FindPool(ActorClass.Get());
	if (!Pool) return nullptr;

	for (FSlot& Slot : Pool->Slots)
	{
		if (Slot.bActive || !Slot.Actor.IsValid()) continue;

		AActor* Actor = Slot.Actor.Get();
		if (!IsValid(Actor)) continue;

		Slot.bActive = true;

		if (Pool->Policy && IsValid(Pool->Policy))
			Pool->Policy->OnActorActivated(Actor, FInstancedStruct{});

		return Actor;
	}

	UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyActorPool]: Pool exhausted for class %s"), *ActorClass->GetName());
	return nullptr;
}

void UCrowdyActorPoolSubsystem::ReleaseActor(AActor* Actor)
{
	check(IsInGameThread());
	if (!IsValid(Actor)) return;

	for (auto& [Class, Pool] : Pools)
	{
		for (FSlot& Slot : Pool.Slots)
		{
			if (Slot.Actor.Get() != Actor) continue;

			Slot.bActive = false;

			if (Pool.Policy && IsValid(Pool.Policy))
				Pool.Policy->OnActorDeactivated(Actor);

			return;
		}
	}
}

FPool* UCrowdyActorPoolSubsystem::FindPool(const UClass* ActorClass)
{
	return Pools.Find(TSubclassOf<AActor>(const_cast<UClass*>(ActorClass)));
}
