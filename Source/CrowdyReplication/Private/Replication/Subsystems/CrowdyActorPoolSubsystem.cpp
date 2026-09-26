// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "CrowdyReplicationLog.h"
#include "Replication/Components/CrowdyEntityComponent.h"

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
	Pool.MaxSize    = FMath::Max(Config.PoolSize, Config.MaxPoolSize);

	Pool.Slots.Reserve(Config.PoolSize);
	for (int32 i = 0; i < Config.PoolSize; i++)
		SpawnPooledActor(Pool);
}

AActor* UCrowdyActorPoolSubsystem::SpawnPooledActor(FPool& Pool)
{
	// Deferred so the entity component can be marked dormant BEFORE BeginPlay. A pooled actor that
	// registers itself is observed synchronously by every listener, and the Game Model answers by ensuring
	// a server container row for an actor that stands for nothing. OnActorPooled below unregisters the
	// record, but the round trip is already sent and cannot be recalled.
	AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(Pool.ActorClass, FTransform::Identity,
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor) return nullptr;

	if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
		Component->MarkPooledDormant();

	// Growth spawns mid-game at the origin, where a colliding actor would push whatever stands there.
	Actor->SetActorEnableCollision(false);
	Actor->FinishSpawning(FTransform::Identity);

	Pool.Policy->OnActorPooled(Actor);

	FSlot Slot;
	Slot.Actor   = Actor;
	Slot.bActive = false;
	Pool.Slots.Add(Slot);
	return Actor;
}

AActor* UCrowdyActorPoolSubsystem::AcquireActor(const TSubclassOf<AActor> ActorClass)
{
	FPool* Pool = FindPool(ActorClass.Get());
	if (!Pool) return nullptr;

	int32 Index = Pool->Slots.IndexOfByPredicate([](const FSlot& Slot) { return !Slot.bActive && Slot.Actor.IsValid(); });
	if (Index == INDEX_NONE)
	{
		// An actor destroyed by something else never comes back to the pool, so it stops counting against the cap.
		Pool->Slots.RemoveAllSwap([](const FSlot& Slot) { return !Slot.Actor.IsValid(); });

		if (Pool->Slots.Num() >= Pool->MaxSize)
		{
			UE_CLOG(!Pool->bWarnedAtCap, LogCrowdyReplication, Warning,
				TEXT("[CrowdyActorPool]: Pool for class %s is at its cap of %d actors, all in use, so further entities of this class wait until one is released. ")
				TEXT("Raise Max Pool Size Per Class on the Actor Pool Backend Config to allow more. Said once per class."),
				*ActorClass->GetName(), Pool->MaxSize);
			Pool->bWarnedAtCap = true;
			return nullptr;
		}

		if (!SpawnPooledActor(*Pool)) return nullptr;
		Index = Pool->Slots.Num() - 1;

		UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Log,
			TEXT("[CrowdyActorPool]: Pool for class %s grew to %d actors (cap %d)."),
			*ActorClass->GetName(), Pool->Slots.Num(), Pool->MaxSize);
	}

	FSlot& Slot = Pool->Slots[Index];
	Slot.bActive = true;

	AActor* Actor = Slot.Actor.Get();
	if (IsValid(Pool->Policy))
		Pool->Policy->OnActorActivated(Actor, FInstancedStruct{});

	return Actor;
}

void UCrowdyActorPoolSubsystem::ReleaseActor(AActor* Actor)
{
	check(IsInGameThread());
	if (!IsValid(Actor)) return;

	// A pool spawns only its own class, so an actor can only be in the pool keyed by its class.
	FPool* Pool = FindPool(Actor->GetClass());
	if (!Pool) return;

	for (FSlot& Slot : Pool->Slots)
	{
		if (Slot.Actor.Get() != Actor) continue;

		Slot.bActive = false;

		if (IsValid(Pool->Policy))
			Pool->Policy->OnActorDeactivated(Actor);

		return;
	}
}

bool UCrowdyActorPoolSubsystem::HasPool(const UClass* ActorClass) const
{
	return Pools.Contains(TSubclassOf<AActor>(const_cast<UClass*>(ActorClass)));
}

FPool* UCrowdyActorPoolSubsystem::FindPool(const UClass* ActorClass)
{
	return Pools.Find(TSubclassOf<AActor>(const_cast<UClass*>(ActorClass)));
}
