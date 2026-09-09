// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "CrowdyReplicationLog.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// An update that arrives before its entity's spawn event is parked with a render slot already allocated
// to it. If nothing ever resolves the entity's class the entry has no way out, so the slot is held for as
// long as the world lives. These cases drive the park's ageing rules directly: they cover that an entry
// past the eviction bound gives its slot back and says so by name, that an entry still under the bound is
// left alone, and that each report is made once per entry rather than once per tick.
namespace
{
	constexpr EAutomationTestFlags CrowdyPendingActivationParkTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Collects LogCrowdyReplication warnings for as long as it is in scope. The message is the deliverable
	// here, so the cases read the real log line rather than a counter kept beside it, which would still
	// read green if the line itself were dropped.
	class FCrowdyParkWarningCapture : public FOutputDevice
	{
	public:

		FCrowdyParkWarningCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FCrowdyParkWarningCapture()
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity != ELogVerbosity::Warning || Category != LogCrowdyReplication.GetCategoryName())
			{
				return;
			}

			FScopeLock Lock(&Mutex);
			Lines.Add(Message);
		}

		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		TArray<FString> TakeLines()
		{
			GLog->FlushThreadedLogs();

			FScopeLock Lock(&Mutex);
			TArray<FString> Taken = MoveTemp(Lines);
			Lines.Reset();
			return Taken;
		}

	private:

		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	UCrowdyActorManager::FPendingActivation MakeParkedEntry(const int32 SlotId, const UScriptStruct* PayloadStruct)
	{
		UCrowdyActorManager::FPendingActivation Entry;
		Entry.SlotId = SlotId;

		if (PayloadStruct)
			Entry.InitialState.InitializeAs(PayloadStruct);

		return Entry;
	}

	int32 CountLinesContaining(const TArray<FString>& Lines, const FString& Needle)
	{
		int32 Count = 0;
		for (const FString& Line : Lines)
		{
			if (Line.Contains(Needle))
				Count++;
		}

		return Count;
	}
}

/**
 * The bound itself: an entry aged past it leaves the park and is named on the way out, while an entry
 * parked later and still under it keeps its place. Both are in one run so the two outcomes are decided
 * by how long each entry has waited and by nothing else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPendingActivationEvictedPastBoundTest,
	"CrowdySDK.ActorManager.PendingActivationEvictedPastBound",
	CrowdyPendingActivationParkTestFlags)

bool FCrowdyPendingActivationEvictedPastBoundTest::RunTest(const FString& Parameters)
{
	constexpr int32 WarnTicks = 3;
	constexpr int32 EvictTicks = 5;

	const FGuid Stranded(1, 2, 3, 4);
	const FGuid Recent(5, 6, 7, 8);

	TMap<FGuid, UCrowdyActorManager::FPendingActivation> Park;
	Park.Add(Stranded, MakeParkedEntry(0, TBaseStructure<FVector>::Get()));

	FCrowdyParkWarningCapture Capture;

	TArray<FGuid> Evicted;
	for (int32 Tick = 0; Tick < EvictTicks - 1; Tick++)
		UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, EvictTicks, Evicted);

	TestTrue(TEXT("An entry one tick under the eviction bound is still parked"), Park.Contains(Stranded));
	TestEqual(TEXT("Nothing is evicted while every entry is under the bound"), Evicted.Num(), 0);

	Park.Add(Recent, MakeParkedEntry(1, TBaseStructure<FVector>::Get()));
	Capture.TakeLines();

	UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, EvictTicks, Evicted);

	TestEqual(TEXT("Exactly the entry past the bound is evicted"), Evicted.Num(), 1);
	if (Evicted.Num() == 1)
		TestTrue(TEXT("The evicted entry is the one that waited past the bound"), Evicted[0] == Stranded);

	TestFalse(TEXT("The evicted entry has left the park"), Park.Contains(Stranded));
	TestTrue(TEXT("An entry parked under the bound in the same sweep keeps its place"), Park.Contains(Recent));

	const TArray<FString> EvictionLines = Capture.TakeLines();
	TestEqual(TEXT("The eviction is reported once"), CountLinesContaining(EvictionLines, TEXT("has been released")), 1);
	TestEqual(TEXT("The eviction names the entity it evicted"), CountLinesContaining(EvictionLines, Stranded.ToString()), 1);
	TestEqual(TEXT("The eviction names the payload the entry was parked with"), CountLinesContaining(EvictionLines, TEXT("'Vector'")), 1);
	TestEqual(TEXT("The entry still under the bound is not named by any eviction"), CountLinesContaining(EvictionLines, Recent.ToString()), 0);

	// An evicted entry is gone from the park, so the eviction cannot repeat for it however long the world
	// runs on. Nothing memoises this: leaving is what makes it once per entry.
	for (int32 Tick = 0; Tick < EvictTicks * 3; Tick++)
		UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, EvictTicks, Evicted);

	const TArray<FString> LaterLines = Capture.TakeLines();
	TestEqual(TEXT("An evicted entry is never named again"), CountLinesContaining(LaterLines, Stranded.ToString()), 0);
	TestEqual(TEXT("A different entry still reaches the bound and is evicted, exactly once"), CountLinesContaining(LaterLines, TEXT("has been released")), 1);
	TestTrue(TEXT("That eviction names the entry it evicted"), CountLinesContaining(LaterLines, Recent.ToString()) > 0);
	TestTrue(TEXT("The park is empty once every entry has passed the bound"), Park.IsEmpty());

	return true;
}

/**
 * The stranded report is made once per entry and not once per tick, and a second entry crossing the same
 * bound later still gets its own. The eviction bound is set out of reach here so the two reports cannot be
 * confused with each other.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPendingActivationReportsEachEntryOnceTest,
	"CrowdySDK.ActorManager.PendingActivationReportsEachEntryOnce",
	CrowdyPendingActivationParkTestFlags)

bool FCrowdyPendingActivationReportsEachEntryOnceTest::RunTest(const FString& Parameters)
{
	constexpr int32 WarnTicks = 3;
	constexpr int32 UnreachableEvictTicks = 1000;

	const FGuid First(11, 12, 13, 14);
	const FGuid Second(21, 22, 23, 24);

	TMap<FGuid, UCrowdyActorManager::FPendingActivation> Park;
	Park.Add(First, MakeParkedEntry(0, TBaseStructure<FVector>::Get()));

	FCrowdyParkWarningCapture Capture;

	TArray<FGuid> Evicted;
	for (int32 Tick = 0; Tick < WarnTicks * 4; Tick++)
		UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, UnreachableEvictTicks, Evicted);

	const TArray<FString> FirstLines = Capture.TakeLines();
	TestEqual(TEXT("An entry past the warn bound is reported once, not once per tick"), CountLinesContaining(FirstLines, First.ToString()), 1);
	TestEqual(TEXT("Nothing is evicted while the eviction bound is out of reach"), Evicted.Num(), 0);

	Park.Add(Second, MakeParkedEntry(1, TBaseStructure<FVector>::Get()));

	for (int32 Tick = 0; Tick < WarnTicks; Tick++)
		UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, UnreachableEvictTicks, Evicted);

	const TArray<FString> SecondLines = Capture.TakeLines();
	TestEqual(TEXT("A second entry reaching the same bound is reported in its own right"), CountLinesContaining(SecondLines, Second.ToString()), 1);
	TestEqual(TEXT("The first entry is not reported a second time"), CountLinesContaining(SecondLines, First.ToString()), 0);

	return true;
}

/**
 * An entry parked from an update whose state blob decoded into nothing has no payload name to print. The
 * eviction says that in words rather than printing an empty name, because a reader sent to look for a
 * struct that was never registered here has been sent to look for something that does not exist.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPendingActivationEvictionNamesUndecodedPayloadTest,
	"CrowdySDK.ActorManager.PendingActivationEvictionNamesUndecodedPayload",
	CrowdyPendingActivationParkTestFlags)

bool FCrowdyPendingActivationEvictionNamesUndecodedPayloadTest::RunTest(const FString& Parameters)
{
	constexpr int32 WarnTicks = 3;
	constexpr int32 EvictTicks = 5;

	const FGuid Undecoded(31, 32, 33, 34);

	TMap<FGuid, UCrowdyActorManager::FPendingActivation> Park;
	Park.Add(Undecoded, MakeParkedEntry(0, nullptr));

	FCrowdyParkWarningCapture Capture;

	TArray<FGuid> Evicted;
	for (int32 Tick = 0; Tick < EvictTicks - 1; Tick++)
		UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, EvictTicks, Evicted);

	// The stranded report crossed its own bound during those ticks and says the same thing about the
	// payload, so it is taken out of the way and the eviction sweep is measured on its own.
	Capture.TakeLines();

	UCrowdyActorManager::AgePendingActivations(Park, WarnTicks, EvictTicks, Evicted);

	TestEqual(TEXT("An entry with no decoded payload is still evicted at the bound"), Evicted.Num(), 1);
	TestFalse(TEXT("The evicted entry has left the park"), Park.Contains(Undecoded));

	const TArray<FString> Lines = Capture.TakeLines();
	TestEqual(TEXT("The eviction names the entity"), CountLinesContaining(Lines, Undecoded.ToString()), 1);
	TestEqual(TEXT("The eviction says the state blob decoded into no registered struct"),
		CountLinesContaining(Lines, TEXT("decoded into no struct registered on this client")), 1);
	TestEqual(TEXT("The eviction does not send the reader after a struct it cannot name"),
		CountLinesContaining(Lines, TEXT("RegisterStateClass")), 0);

	return true;
}

/**
 * A slot is reachable from the moment it is allocated, so a parked entry's slot receives the updates that
 * kept arriving while it waited. Freeing that slot without telling the backend to clean it leaves those
 * samples in place for whichever entity is handed the slot next, which then draws at, and interpolates in
 * from, the evicted entity's last known position.
 *
 * The eviction is what makes this reachable at all: before it, a parked slot was never given back.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEvictedSlotIsCleanedBeforeReuseTest,
	"CrowdySDK.ActorManager.EvictedSlotIsCleanedBeforeReuse",
	CrowdyPendingActivationParkTestFlags)

bool FCrowdyEvictedSlotIsCleanedBeforeReuseTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));

	Manager->SetBackend(Backend.Get());

	const FGuid Stranded = FGuid::NewGuid();
	Manager->ParkActivationForTest(Stranded);

	// The premise, measured rather than assumed: a parked entry's slot is fed by the update path even though
	// the backend was never told to activate it.
	Manager->ExtractUpdateForTest(Stranded, FInstancedStruct());

	TestEqual(TEXT("A parked slot receives updates while it waits"), Backend->ExtractedSlots.Num(), 1);

	if (Backend->ExtractedSlots.Num() != 1)
		return false;

	const int32 ParkedSlot = Backend->ExtractedSlots[0];

	TestEqual(TEXT("Nothing has been activated in that slot"), Backend->ActivatedSlots.Num(), 0);
	TestEqual(TEXT("Nothing has been cleaned up yet"), Backend->DeactivatedSlots.Num(), 0);

	// Past the eviction bound. Ticking to it rather than calling the release directly keeps the case on the
	// path the eviction actually takes.
	// The bound is counted in ticks waited, so the entry survives one fewer than that and goes on the next.
	const int32 Bound = UCrowdyActorManager::GetPendingActivationEvictTicksForTest();
	for (int32 Tick = 0; Tick < Bound - 1; Tick++)
		Manager->TickPendingActivationsForTest();

	// Held right up to the bound. Without this the case would pass just as well on a bound of zero, so it is
	// what makes the eviction wait for the bound rather than merely happen eventually.
	TestEqual(TEXT("A parked entry is still held one tick short of the bound"), Backend->DeactivatedSlots.Num(), 0);

	Manager->TickPendingActivationsForTest();

	TestEqual(TEXT("The evicted slot is cleaned up exactly once"), Backend->DeactivatedSlots.Num(), 1);

	if (Backend->DeactivatedSlots.Num() == 1)
		TestEqual(TEXT("The cleanup names the slot the entry was holding"), Backend->DeactivatedSlots[0], ParkedSlot);

	// And the slot really is handed on, which is what makes the cleanup load-bearing rather than tidy.
	const FGuid Newcomer = FGuid::NewGuid();

	TestEqual(TEXT("The freed slot is given to the next entity"),
		Manager->AllocateSlotForTest(Newcomer), ParkedSlot);

	return true;
}

/**
 * The same rule on the other paths that give a parked slot back. A spawn event that arrives carrying a class
 * path nothing can resolve releases the slot immediately rather than waiting out the eviction bound, and it
 * has had exactly as long to accumulate updates.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReleasedSlotIsCleanedOnEveryPathTest,
	"CrowdySDK.ActorManager.ReleasedSlotIsCleanedOnEveryPath",
	CrowdyPendingActivationParkTestFlags)

bool FCrowdyReleasedSlotIsCleanedOnEveryPathTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));

	Manager->SetBackend(Backend.Get());

	const FGuid Parked = FGuid::NewGuid();
	Manager->ParkActivationForTest(Parked);
	Manager->ReleaseSlotForTest(Parked);

	TestEqual(TEXT("A slot released before the bound is cleaned up too"), Backend->DeactivatedSlots.Num(), 1);

	// A UUID holding no slot is not a slot release, so nothing should be said about one.
	Manager->ReleaseSlotForTest(FGuid::NewGuid());

	TestEqual(TEXT("Releasing a UUID that holds no slot cleans nothing up"), Backend->DeactivatedSlots.Num(), 1);

	return true;
}

#endif
