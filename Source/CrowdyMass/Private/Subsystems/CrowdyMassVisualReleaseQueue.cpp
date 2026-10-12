#include "Subsystems/CrowdyMassVisualReleaseQueue.h"

#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

bool UCrowdyMassVisualReleaseQueue::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

void UCrowdyMassVisualReleaseQueue::Deinitialize()
{
	// Dropped rather than issued. Every record names a mesh component in this world, and those go with
	// the world, taking the animation tracks recorded against them with it.
	PendingVisualReleases.Empty();
	Super::Deinitialize();
}

int32 UCrowdyMassVisualReleaseQueue::QueueVisualReleases(TConstArrayView<FCrowdyMassPendingVisualRelease> Releases)
{
	const int32 FreeSlots = FMath::Max(0, CrowdyMassMaxPendingVisualReleases - PendingVisualReleases.Num());
	const int32 RecordedCount = FMath::Min(FreeSlots, Releases.Num());

	PendingVisualReleases.Append(Releases.GetData(), RecordedCount);

	const int32 DroppedCount = Releases.Num() - RecordedCount;
	if (DroppedCount == 0)
	{
		return RecordedCount;
	}

	// Warned here rather than left to the caller because this is the one place that knows why the record
	// was refused. Each dropped record is one animation track that stays allocated for the session, and
	// the queue can only fill if nothing is draining it, so the message names that cause.
	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds >= NextVisualReleaseOverflowWarningSeconds)
	{
		NextVisualReleaseOverflowWarningSeconds = NowSeconds + 1.0;
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassVisualReleaseQueue]: The renderer visual release queue is full at %d records, so %d destroyed entities could not be queued and their animation tracks stay allocated for the rest of the session. The queue is drained every frame, so a full queue means nothing is draining it: check that UCrowdyMassVisualReleaseProcessor is registered and running in this world."),
			PendingVisualReleases.Num(), DroppedCount);
	}

	return RecordedCount;
}

void UCrowdyMassVisualReleaseQueue::DrainPendingVisualReleases(TArray<FCrowdyMassPendingVisualRelease>& OutReleases)
{
	// Swapped rather than copied, so the caller's buffer becomes the queue's buffer for the next frame
	// and neither side reallocates once the crowd has settled at its usual burst size.
	OutReleases.Reset();
	Swap(OutReleases, PendingVisualReleases);
}
