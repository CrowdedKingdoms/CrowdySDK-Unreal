#include "Replication/Subsystems/CrowdyReplicatedSubsystem.h"

#include "Engine/World.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyReplicatedSubsystemLibrary.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"

void UCrowdyReplicatedWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Force both the entity registry AND the state replicator to initialize before we enroll. The replicator
	// binds OnEntityRegistered in its own Initialize; if we enrolled first, our RegisterEntity ->
	// OnEntityRegistered broadcast would fire before the replicator was listening and this subsystem would never
	// be tracked (no rescan recovers it). Depending on the entity registry alone is not enough; it does not pull
	// the replicator in.
	Collection.InitializeDependency(UCrowdyEntitySubsystem::StaticClass());
	Collection.InitializeDependency(UCrowdyStateReplicator::StaticClass());

	UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystem(this, GetReplicatedOwnership());
}

void UCrowdyReplicatedWorldSubsystem::Deinitialize()
{
	UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystem(this);
	Super::Deinitialize();
}

bool UCrowdyReplicatedWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// The entity registry exists only in PIE/Game worlds, so enrolling anywhere else would just warn. Mirrors
	// UCrowdyStateReplicator / UCrowdyEntitySubsystem.
	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

ECrowdyOwnership UCrowdyReplicatedWorldSubsystem::GetReplicatedOwnership() const
{
	return ECrowdyOwnership::Host;
}

void UCrowdyReplicatedGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// A game instance subsystem has no world of its own and outlives worlds, so it cannot enroll here. Instead it
	// re-enrolls into each world's per-world entity registry when that world begins play, and unenrolls as the
	// world is cleaned up. FWorldDelegates are process-global; the handlers filter to this subsystem's own game
	// instance. AddWeakLambda auto-invalidates if this object dies, but the handles are still Removed explicitly
	// in Deinitialize.
	WorldInitHandle = FWorldDelegates::OnWorldInitializedActors.AddWeakLambda(
		this, [this](const UWorld::FActorsInitializedParams& Params) { HandleWorldInitialized(Params.World); });

	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddWeakLambda(
		this, [this](UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/) { HandleWorldCleanup(World); });
}

void UCrowdyReplicatedGameInstanceSubsystem::Deinitialize()
{
	if (WorldInitHandle.IsValid())
	{
		FWorldDelegates::OnWorldInitializedActors.Remove(WorldInitHandle);
		WorldInitHandle.Reset();
	}
	if (WorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		WorldCleanupHandle.Reset();
	}

	// Backstop unenroll from the current world in case teardown ordering deinitializes us before OnWorldCleanup
	// fires for the world. Resolves this->GetWorld() (a game instance subsystem's current world); a null/no-ES
	// world is a clean no-op in the library wrapper.
	UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystem(this);

	Super::Deinitialize();
}

void UCrowdyReplicatedGameInstanceSubsystem::HandleWorldInitialized(UWorld* World)
{
	// FWorldDelegates fire for every world across every game instance; act only on ours. A transient world at
	// UGameEngine::Init and standalone editor/preview worlds have a null or foreign game instance, so this guard
	// also rejects those (the World null-check must short-circuit first).
	if (!World || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(
		World->GetSubsystem<UCrowdyEntitySubsystem>(), this, GetReplicatedOwnership());
}

void UCrowdyReplicatedGameInstanceSubsystem::HandleWorldCleanup(UWorld* World)
{
	if (!World || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystemInto(
		World->GetSubsystem<UCrowdyEntitySubsystem>(), this);
}

ECrowdyOwnership UCrowdyReplicatedGameInstanceSubsystem::GetReplicatedOwnership() const
{
	return ECrowdyOwnership::Host;
}
