#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateFragmentTestTypes.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/UnrealType.h"
#include "Utils/UCrowdyClassRegistry.h"

// Covers the router's side of ICrowdyEntitySubscriber's state plane (CrowdyEventRouter.cpp): the shared
// decode scratch buffer, the class-id guard, the never-defer-once-claimed rule, the separation of "is this
// id yours" from "what class is it", and the world-travel supersede latch. None of this needs a real Mass
// entity - the subscriber double in CrowdyStateFragmentTestTypes.h stands in for one, exactly as
// CrowdyMassEventSeamTests.cpp does for the same interface's event plane. What CrowdyMass adds
// specifically for its own registration and ingestion mechanics lives in CrowdyMass's own test tree.
namespace
{
	constexpr EAutomationTestFlags CrowdyStateFragmentTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// Decoding into the shared scratch buffer (through the router and the subscriber) produces byte-identical
// results to decoding the same blob straight onto a real object, across every leaf and USTRUCT kind
// UCrowdyStateCodecTarget carries. This is the test that actually PROVES the buffer stands in for a real
// instance, rather than assuming FCrowdyStateCodec::Decode cannot tell the difference.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentScratchDecodeMatchesObjectDecodeTest,
	"CrowdySDK.StateFragment.ScratchDecodeMatchesObjectDecode", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentScratchDecodeMatchesObjectDecodeTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	const FGuid EntityID = FGuid::NewGuid();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	// One keyframe touching every leaf/USTRUCT kind the layout supports, each set to a non-default value.
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 42;
	Source->RepBigInt = static_cast<int64>(9000000001);
	Source->RepFloat = 1.25f;
	Source->RepDouble = -3.5;
	Source->bRepFlag = true;
	Source->RepByte = 200;
	Source->RepEnum = ECrowdyStateTestEnum::Gamma;
	Source->RepName = FName(TEXT("MyTag"));
	Source->RepString = TEXT("hello crowdy state");
	Source->RepVector = FVector(1.0, -2.5, 3.25);
	Source->RepRotator = FRotator(10.0, 20.0, 30.0);
	Source->RepQuantized = FVector_NetQuantize(12.4, -7.8, 3.6);

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, TBitArray<>(true, Layout->Properties.Num()), /*bKeyframe=*/true, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	// Copied out of the shared scratch buffer while ApplyDecodedState's call is still on the stack: the
	// buffer is reused by the very next entity of this class, so nothing here may outlive this call.
	UCrowdyStateCodecTarget* ViaScratch = NewObject<UCrowdyStateCodecTarget>();
	Subscriber->OnApply = [ViaScratch](const FGuid&, const FCrowdyRepLayout& L, const void* DecodedContainer, TConstArrayView<int32> Changed)
	{
		for (const int32 Index : Changed)
		{
			if (const FProperty* Prop = L.Properties[Index].Property)
			{
				Prop->CopyCompleteValue_InContainer(ViaScratch, DecodedContainer);
			}
		}
	};

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the subscriber was asked to apply exactly once"), Subscriber->ApplyDecodedStateCallCount, 1);
	TestEqual(TEXT("a full keyframe changes every layout slot"), Subscriber->LastChangedIndices.Num(), Layout->Properties.Num());

	// Decode the identical blob straight onto a real object, with no scratch buffer involved at all.
	UCrowdyStateCodecTarget* ViaObject = NewObject<UCrowdyStateCodecTarget>();
	TArray<int32> DirectChanged;
	TestTrue(TEXT("direct object decode succeeded"),
		FCrowdyStateCodec::Decode(*Layout, Layout->LayoutHash, Blob, ViaObject, DirectChanged));
	TestEqual(TEXT("both decodes report the same changed count"), DirectChanged.Num(), Subscriber->LastChangedIndices.Num());

	// Compared property by property via FProperty::Identical, so struct leaves (FVector, FRotator, the
	// quantized vector) are compared the same way the codec's own round-trip tests do it, not merely the
	// scalar kinds a hand-written switch would remember to list.
	bool bAllIdentical = true;
	for (const FCrowdyRepProperty& RepProp : Layout->Properties)
	{
		if (!RepProp.Property)
		{
			continue;
		}
		if (!RepProp.Property->Identical_InContainer(ViaScratch, ViaObject))
		{
			bAllIdentical = false;
			AddError(FString::Printf(TEXT("property '%s' differs between scratch decode and object decode"),
				*RepProp.Property->GetName()));
		}
	}
	TestTrue(TEXT("every leaf and USTRUCT property decoded identically via scratch and via a real object"), bAllIdentical);

	return true;
}

// FCrowdyStateScratchContainer's own construction/decode/destruction cycle, exercised directly (not
// through the router: UCrowdyEventRouter::Deinitialize asserts bInitialized, which only a real
// Initialize(Collection) call sets, and this seam is deliberately headless). Mirrors exactly what
// UCrowdyEventRouter::ResolveStateScratch builds - same allocation, same InitializeValue_InContainer loop
// - so this is a faithful test of the real type, not a stand-in for it.
//
// A heap-carrying leaf (FString, FName, a USTRUCT holding an FString) is decoded twice with different
// values before the container goes out of scope, so its own overwrite path (ordinary FProperty
// SerializeItem/assignment freeing the previous allocation) is exercised, then the container's destructor
// runs exactly once via normal C++ scope-exit. A double free or a skipped destroy here would most likely
// crash before this test returns, which is the strongest signal available without a memory sanitizer
// attached to the test run; see the report for why the "buffer whose class has gone away is abandoned"
// branch is not exercised here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentScratchContainerDestroysHeapPropertiesTest,
	"CrowdySDK.StateFragment.ScratchContainerDestroysHeapProperties", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentScratchContainerDestroysHeapPropertiesTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateFragmentHeapTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("heap target has a layout"), Layout))
	{
		return false;
	}

	auto MakeHeapBlob = [&](const FString& StringValue, FName NameValue, const FString& StructPayload)
	{
		UCrowdyStateFragmentHeapTarget* Source = NewObject<UCrowdyStateFragmentHeapTarget>();
		Source->RepString = StringValue;
		Source->RepName = NameValue;
		Source->RepStruct.Payload = StructPayload;

		TArray<uint8> Blob;
		FCrowdyStateCodec::Encode(*Layout, Source, TBitArray<>(true, Layout->Properties.Num()), /*bKeyframe=*/true, Blob);
		return Blob;
	};

	const TArray<uint8> FirstBlob = MakeHeapBlob(TEXT("first string"), FName(TEXT("FirstName")), TEXT("first struct string"));
	const TArray<uint8> SecondBlob = MakeHeapBlob(
		TEXT("second string, deliberately longer than the first"), FName(TEXT("SecondName")),
		TEXT("second struct string, also longer than the first"));

	{
		FCrowdyStateScratchContainer Scratch;
		Scratch.OwnerClass = TargetClass;
		Scratch.LayoutHash = Layout->LayoutHash;
		Scratch.Size = TargetClass->GetPropertiesSize();
		if (!TestTrue(TEXT("heap target has a non-zero properties size"), Scratch.Size > 0))
		{
			return false;
		}
		Scratch.Data = static_cast<uint8*>(FMemory::Malloc(Scratch.Size, FMath::Max(1, TargetClass->GetMinAlignment())));
		FMemory::Memzero(Scratch.Data, Scratch.Size);

		Scratch.InitializedProps.Reserve(Layout->Properties.Num());
		for (const FCrowdyRepProperty& RepProperty : Layout->Properties)
		{
			if (!RepProperty.Property)
			{
				continue;
			}
			RepProperty.Property->InitializeValue_InContainer(Scratch.Data);
			Scratch.InitializedProps.Add(RepProperty.Property);
		}

		TArray<int32> FirstChanged;
		TestTrue(TEXT("first decode into the scratch buffer succeeded"),
			FCrowdyStateCodec::Decode(*Layout, Layout->LayoutHash, FirstBlob, Scratch.Data, FirstChanged));

		// Overwritten in place: every heap value the first decode left behind is freed and reallocated by
		// the property system's own assignment, not accumulated and not leaked, and the buffer must
		// survive being written to twice.
		TArray<int32> SecondChanged;
		TestTrue(TEXT("second decode into the SAME scratch buffer succeeded"),
			FCrowdyStateCodec::Decode(*Layout, Layout->LayoutHash, SecondBlob, Scratch.Data, SecondChanged));

		// Scratch goes out of scope here: its destructor runs DestroyValue_InContainer on RepString,
		// RepName and RepStruct.Payload exactly once, through the properties snapshotted into
		// InitializedProps above, then frees Data.
	}

	TestTrue(TEXT("the scratch container tore down its heap-carrying values without crashing"), true);
	return true;
}

// A delta whose ClassID does not match what this client resolved locally for the entity is dropped before
// any decode: the subscriber is asked whether it holds the id and then for that id's class, but is never
// asked to apply anything, and nothing is queued to wait either - a class mismatch is drift or forgery,
// not a reason to hope a later attempt looks different.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentForgedClassIdRefusedTest,
	"CrowdySDK.StateFragment.ForgedClassIdRefused", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentForgedClassIdRefusedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	const FGuid EntityID = FGuid::NewGuid();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 555;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, TBitArray<>(true, Layout->Properties.Num()), /*bKeyframe=*/true, Blob);

	FCrowdyStateDelta Delta;
	// Forged: this client resolved TargetClass locally for the entity, but the delta claims a different
	// class id - exactly the drift/forgery DispatchStateDeltaToSubscriber's guard exists to catch.
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass)) + 1;
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the subscriber was asked whether it holds the id"), Subscriber->IsEntityKnownCallCount, 1);
	TestEqual(TEXT("and, separately, for that id's class"), Subscriber->GetEntityClassCallCount, 1);
	TestEqual(TEXT("but never asked to apply anything - the target is untouched, not merely refused"),
		Subscriber->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("a class-id mismatch is a final refusal, not a wait: nothing is queued"),
		Router->NumDeferredForTest(), 0);

	return true;
}

// An id the subscriber holds but has, honestly, nothing to write for (a hot delta with no bits set) is
// applied and forgotten, never deferred. A flood of such ids - far past both defer-queue caps - must never
// occupy the queue at all, and a genuinely unresolved id dispatched afterward must still defer exactly as
// it always has, proving the flood did not silently disable deferring altogether.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentProviderRefusalDoesNotDeferTest,
	"CrowdySDK.StateFragment.ProviderRefusalDoesNotDefer", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentProviderRefusalDoesNotDeferTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));

	// An "empty" hot delta: no bit dirty, so decode succeeds with zero changed indices. The subscriber is
	// asked to apply and has nothing to write - the "held but nothing to write to" case the interface
	// calls a refusal rather than a reason to wait.
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	TArray<uint8> EmptyBlob;
	FCrowdyStateCodec::Encode(*Layout, Source, TBitArray<>(false, Layout->Properties.Num()), /*bKeyframe=*/false, EmptyBlob);

	constexpr int32 NumOwnedIds = 300; // Past both defer-queue caps (256 distinct ids, 1024 events).
	for (int32 Index = 0; Index < NumOwnedIds; ++Index)
	{
		const FGuid Id = FGuid::NewGuid();
		Subscriber->SetOwnedClass(Id, TargetClass);

		FCrowdyStateDelta Delta;
		Delta.ClassID = ClassID;
		Delta.EntityID = Id;
		Delta.SenderID = FGuid::NewGuid();
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Blob = EmptyBlob;

		Router->DispatchEvent(MakeInboundStateEvent(Delta));
	}

	TestEqual(TEXT("every one of the flood was applied, with nothing to write"),
		Subscriber->ApplyDecodedStateCallCount, NumOwnedIds);
	TestEqual(TEXT("none of them ever entered the defer queue"), Router->NumDeferredForTest(), 0);
	TestEqual(TEXT("so none of them occupy the distinct-entity cap either"), Router->NumDeferredEntitiesForTest(), 0);

	// The queue mechanism itself still works: a genuinely unresolved id (the subscriber does not hold it, and
	// no participant is registered for it) still defers exactly as it always has. If the flood above had
	// silently disabled deferring altogether, this is the assertion that would catch it.
	FCrowdyStateDelta UnresolvedDelta;
	UnresolvedDelta.ClassID = ClassID;
	UnresolvedDelta.EntityID = FGuid::NewGuid();
	UnresolvedDelta.SenderID = FGuid::NewGuid();
	UnresolvedDelta.LayoutHash = Layout->LayoutHash;
	UnresolvedDelta.Blob = EmptyBlob;

	Router->DispatchEvent(MakeInboundStateEvent(UnresolvedDelta));
	TestEqual(TEXT("a genuinely unresolved id is still deferred - the queue was never broken by the flood"),
		Router->NumDeferredForTest(), 1);

	return true;
}

// The subscriber slot follows the world-travel supersede rule (ClaimProviderSlot/ReleaseProviderSlot in
// CrowdyEventRouter.cpp): the newest registration wins, a displaced subscriber can never take the slot
// back, and its release is ignored rather than clearing the slot out from under whoever now holds it.
// Asserted here from the state plane and in CrowdyMassEventSeamTests.cpp from the event plane, because one
// slot now serves both and a rule that held for only one of them would be invisible from either side.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentProviderIsWorldScopedTest,
	"CrowdySDK.StateFragment.ProviderIsWorldScoped", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentProviderIsWorldScopedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	// Two worlds' subscribers, as during level travel: the departing world's is still alive when the
	// arriving world's registers, and only finishes tearing down (releasing) afterward.
	UCrowdyStateFragmentSpySubscriber* DepartingWorldSubscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	UCrowdyStateFragmentSpySubscriber* ArrivingWorldSubscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();

	const FGuid EntityID = FGuid::NewGuid();
	DepartingWorldSubscriber->SetOwnedClass(EntityID, TargetClass);
	ArrivingWorldSubscriber->SetOwnedClass(EntityID, TargetClass);

	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(DepartingWorldSubscriber));
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(ArrivingWorldSubscriber));

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, TBitArray<>(false, Layout->Properties.Num()), /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = ClassID;
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the arriving world's subscriber answered"), ArrivingWorldSubscriber->ApplyDecodedStateCallCount, 1);

	// Both questions, because either alone would leave the other free to be asked. "Was it asked for a
	// class" stopped being the whole of "was it consulted" the moment the router began asking whether the
	// id is held first.
	TestEqual(TEXT("the departing world's subscriber was never asked whether it holds the id"),
		DepartingWorldSubscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("nor for a class, once superseded"),
		DepartingWorldSubscriber->GetEntityClassCallCount, 0);

	// The departing world finishes tearing down AFTER the arriving world has already registered - the
	// exact ordering this seam exists to survive. Its late registration attempt must not take the slot
	// back, and its release must not clear the arriving world's claim.
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(DepartingWorldSubscriber));
	Router->UnregisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(DepartingWorldSubscriber));

	DepartingWorldSubscriber->ApplyDecodedStateCallCount = 0;
	ArrivingWorldSubscriber->ApplyDecodedStateCallCount = 0;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the arriving world's subscriber still answers after the departing world's re-register and release attempts"),
		ArrivingWorldSubscriber->ApplyDecodedStateCallCount, 1);
	TestEqual(TEXT("the departing world's late re-registration never took the slot back"),
		DepartingWorldSubscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("and it was never asked for a class either"),
		DepartingWorldSubscriber->GetEntityClassCallCount, 0);

	return true;
}

// The load-bearing test: two entities of the same class, decoded in sequence with DIFFERENT present-field
// sets, through the SAME shared scratch buffer. A subscriber that reads only ChangedIndices never receives
// the first entity's leftover value as the second entity's own, even though the raw buffer slot genuinely
// still holds it (proven directly here, which is what keeps this from being a vacuous test).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentSharedScratchDoesNotLeakBetweenEntitiesTest,
	"CrowdySDK.StateFragment.SharedScratchDoesNotLeakBetweenEntities", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentSharedScratchDoesNotLeakBetweenEntitiesTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	const int32 RepStringIndex = IndexOfPropertyName(*Layout, TEXT("RepString"));
	const bool bRepIntFound = TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE));
	const bool bRepStringFound = TestNotEqual(TEXT("RepString present"), RepStringIndex, static_cast<int32>(INDEX_NONE));
	if (!bRepIntFound || !bRepStringFound)
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	const FGuid EntityA = FGuid::NewGuid();
	const FGuid EntityB = FGuid::NewGuid();
	Subscriber->SetOwnedClass(EntityA, TargetClass);
	Subscriber->SetOwnedClass(EntityB, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));

	// Entity A's delta carries both RepInt and RepString.
	UCrowdyStateCodecTarget* SourceA = NewObject<UCrowdyStateCodecTarget>();
	SourceA->RepInt = 111;
	SourceA->RepString = TEXT("AAA");
	TBitArray<> DirtyA(false, Layout->Properties.Num());
	DirtyA[RepIntIndex] = true;
	DirtyA[RepStringIndex] = true;
	TArray<uint8> BlobA;
	FCrowdyStateCodec::Encode(*Layout, SourceA, DirtyA, /*bKeyframe=*/false, BlobA);

	// Entity B's delta carries ONLY RepString - a genuinely different present-field set from A's.
	UCrowdyStateCodecTarget* SourceB = NewObject<UCrowdyStateCodecTarget>();
	SourceB->RepString = TEXT("BBB");
	TBitArray<> DirtyB(false, Layout->Properties.Num());
	DirtyB[RepStringIndex] = true;
	TArray<uint8> BlobB;
	FCrowdyStateCodec::Encode(*Layout, SourceB, DirtyB, /*bKeyframe=*/false, BlobB);

	UCrowdyStateCodecTarget* CaptureA = NewObject<UCrowdyStateCodecTarget>();
	UCrowdyStateCodecTarget* CaptureB = NewObject<UCrowdyStateCodecTarget>();
	int32 RawRepIntSeenDuringB = -1;

	Subscriber->OnApply = [&](const FGuid& Id, const FCrowdyRepLayout& L, const void* DecodedContainer, TConstArrayView<int32> Changed)
	{
		// The buffer really is shared: read the raw slot directly, bypassing ChangedIndices, to prove
		// entity A's leftover genuinely still sits there during entity B's call, rather than assuming the
		// danger this test exists to guard against. A subscriber must never do this in production - it is
		// done here only to make the risk observable.
		if (Id == EntityB)
		{
			if (const FIntProperty* RepIntProp = CastField<FIntProperty>(L.Properties[RepIntIndex].Property))
			{
				RawRepIntSeenDuringB = RepIntProp->GetPropertyValue_InContainer(DecodedContainer);
			}
		}

		// The correct behaviour: copy only what THIS delta's ChangedIndices actually lists.
		UCrowdyStateCodecTarget* Capture = (Id == EntityA) ? CaptureA : CaptureB;
		for (const int32 Index : Changed)
		{
			if (const FProperty* Prop = L.Properties[Index].Property)
			{
				Prop->CopyCompleteValue_InContainer(Capture, DecodedContainer);
			}
		}
	};

	FCrowdyStateDelta DeltaA;
	DeltaA.ClassID = ClassID;
	DeltaA.EntityID = EntityA;
	DeltaA.SenderID = FGuid::NewGuid();
	DeltaA.LayoutHash = Layout->LayoutHash;
	DeltaA.Blob = BlobA;
	Router->DispatchEvent(MakeInboundStateEvent(DeltaA));

	FCrowdyStateDelta DeltaB;
	DeltaB.ClassID = ClassID;
	DeltaB.EntityID = EntityB;
	DeltaB.SenderID = FGuid::NewGuid();
	DeltaB.LayoutHash = Layout->LayoutHash;
	DeltaB.Blob = BlobB;
	Router->DispatchEvent(MakeInboundStateEvent(DeltaB));

	TestEqual(TEXT("entity A's own value landed"), CaptureA->RepInt, 111);
	TestEqual(TEXT("entity A's own string landed"), CaptureA->RepString, FString(TEXT("AAA")));

	TestEqual(TEXT("entity B's real value landed"), CaptureB->RepString, FString(TEXT("BBB")));
	TestEqual(TEXT("entity B never carried RepInt, so a subscriber reading only ChangedIndices never receives one for it"),
		CaptureB->RepInt, 0);

	// The control: the shared buffer genuinely still held entity A's leftover at the moment entity B was
	// decoded. If this were ever false (the buffer got reset between entities), the assertions above would
	// be proving nothing - this is what makes the test real rather than vacuous.
	TestEqual(TEXT("the buffer's RepInt slot really did still hold entity A's leftover during B's decode"),
		RawRepIntSeenDuringB, 111);

	return true;
}

/**
 * The other half of sharing one buffer, and the more dangerous half: a value that is DELIVERED but happens to
 * equal what the previous entity left in the buffer must still reach the subscriber.
 *
 * The codec reports a slot as changed only where the decoded value differs from what the container already
 * held. That is correct when the container belongs to the entity, and wrong here, where it belongs to whoever
 * was decoded last. A subscriber driven by that set would never hear about the second entity to enter a given
 * state, and never about the third, and its fragment would keep whatever it already had. The heartbeat cannot
 * rescue it either: every re-send matches the buffer for the same reason and is suppressed the same way. So
 * the failure is permanent rather than transient, and it gets MORE likely as a crowd converges on one value,
 * which is exactly what a crowd does when everyone is dead.
 *
 * Two entities are sent the identical value in sequence here, which is the shape a real crowd produces
 * constantly and which the leak test above cannot produce, because it deliberately uses different values.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentSameValueStillDeliveredTest,
	"CrowdySDK.StateFragment.SameValueStillDelivered", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentSameValueStillDeliveredTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	const FGuid EntityA = FGuid::NewGuid();
	const FGuid EntityB = FGuid::NewGuid();
	Subscriber->SetOwnedClass(EntityA, TargetClass);
	Subscriber->SetOwnedClass(EntityB, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	const int64 ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));

	// One delta body, sent for two different entities: the same property carrying the same value, which is
	// what a crowd looks like the moment a second player enters a state the first is already in.
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 7;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	int32 DeliveredCountA = 0;
	int32 DeliveredCountB = 0;
	int32 DeliveredValueB = -1;

	Subscriber->OnApply = [&](const FGuid& Id, const FCrowdyRepLayout& L, const void* DecodedContainer, TConstArrayView<int32> Delivered)
	{
		int32& Count = (Id == EntityA) ? DeliveredCountA : DeliveredCountB;
		Count += Delivered.Num();

		if (Id != EntityB)
		{
			return;
		}

		for (const int32 Index : Delivered)
		{
			if (const FIntProperty* RepIntProp = CastField<FIntProperty>(L.Properties[Index].Property))
			{
				DeliveredValueB = RepIntProp->GetPropertyValue_InContainer(DecodedContainer);
			}
		}
	};

	FCrowdyStateDelta Delta;
	Delta.ClassID = ClassID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Delta.EntityID = EntityA;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	Delta.EntityID = EntityB;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("entity A was told about the slot its delta carried"), DeliveredCountA, 1);
	TestEqual(TEXT("entity B was told too, even though the buffer already held that exact value from A"),
		DeliveredCountB, 1);
	TestEqual(TEXT("and entity B's delivered value is the one its own delta carried"), DeliveredValueB, 7);

	return true;
}

// An entity can have a participant registered for something other than its view state: an avatar enrolled so
// the entity can carry authoritative model state is a real, resolvable object whose class declares no
// CrowdyState properties. Its presence must not take the entity's deltas away from the subscriber that does
// hold them, so the receive path falls through to the subscriber when the participant's class has no rep
// layout. Precedence between the two is a question of which one CAN hold the state, not which one exists.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentLayoutlessParticipantFallsThroughTest,
	"CrowdySDK.StateFragment.LayoutlessParticipantFallsThroughToProvider", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentLayoutlessParticipantFallsThroughTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	// A participant that is not a home for state: an object with no CrowdyState property on it.
	UObject* LayoutlessParticipant = NewObject<UCrowdyStateLayoutlessParticipant>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, LayoutlessParticipant);

	// Both controls matter. Without a resolving participant the delta would take the no-participant branch,
	// which has always reached the subscriber, and this test would prove nothing about the fall-through; and
	// if the participant's class did have a layout the delta would never reach the subscriber at all.
	TestTrue(TEXT("the participant really does resolve for this entity"),
		Entities->FindParticipant(EntityID) == LayoutlessParticipant);
	TestNull(TEXT("and its class really does carry no CrowdyState rep layout"),
		Registry->FindRepLayout(LayoutlessParticipant->GetClass()));

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	// One slot, carrying a value nothing in this fixture defaults to, so an untouched buffer or a skipped
	// delivery cannot pass for an arrival.
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 1234;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	int32 DeliveredSlots = 0;
	int32 DeliveredRepInt = INDEX_NONE;
	Subscriber->OnApply = [&](const FGuid&, const FCrowdyRepLayout& L, const void* DecodedContainer, TConstArrayView<int32> Delivered)
	{
		DeliveredSlots += Delivered.Num();
		for (const int32 Index : Delivered)
		{
			if (const FIntProperty* RepIntProp = CastField<FIntProperty>(L.Properties[Index].Property))
			{
				DeliveredRepInt = RepIntProp->GetPropertyValue_InContainer(DecodedContainer);
			}
		}
	};

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the subscriber was asked to apply exactly once"), Subscriber->ApplyDecodedStateCallCount, 1);
	TestTrue(TEXT("for the entity the delta names"), Subscriber->LastEntityUUID == EntityID);
	TestEqual(TEXT("carrying exactly the one slot the delta held"), DeliveredSlots, 1);
	TestEqual(TEXT("and the value that arrived is the one the delta carried, not a leftover or a default"),
		DeliveredRepInt, 1234);
	TestEqual(TEXT("the router counted the delta as applied on the subscriber path"),
		Router->GetStateSubscriberStats().Applied, static_cast<int64>(1));
	TestEqual(TEXT("an entity that already has a participant is never queued to wait"),
		Router->NumDeferredForTest(), 0);

	return true;
}

// The other side of the same rule, and the reason the fall-through is placed where it is: a participant whose
// class DOES declare CrowdyState properties still wins outright. The subscriber holds the same id and is
// never consulted, because the delta has a home that can genuinely hold it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentParticipantWithLayoutBeatsProviderTest,
	"CrowdySDK.StateFragment.ParticipantWithLayoutBeatsProvider", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentParticipantWithLayoutBeatsProviderTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateCodecTarget* Participant = NewObject<UCrowdyStateCodecTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Participant);

	// The subscriber holds the very same id, so nothing but the precedence rule decides where the delta goes.
	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 4321;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the value landed on the participant"), Participant->RepInt, 4321);
	TestEqual(TEXT("the subscriber was never even asked whether it holds the id"),
		Subscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("nor for its class"), Subscriber->GetEntityClassCallCount, 0);
	TestEqual(TEXT("and never asked to apply anything"), Subscriber->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("nothing reached the subscriber path's counters either"),
		Router->GetStateSubscriberStats().Applied, static_cast<int64>(0));

	return true;
}

// Who may write an entity does not change because its state happens to live in a fragment rather than on the
// participant. A world entity (HostOwned, no owner) whose participant carries no rep layout still refuses a
// delta that did not come from the host, even though a subscriber holds the entity and would otherwise take
// it. The pair below is what makes this meaningful: the identical setup with the HostSourced flag set DOES
// reach the subscriber, so this proves the gate discriminates rather than that the path is dead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentHostOwnedGateAppliesOnFallThroughTest,
	"CrowdySDK.StateFragment.HostOwnedGateAppliesOnFallThrough", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentHostOwnedGateAppliesOnFallThroughTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	// A world NPC: registered HostOwned with no owner, its view state held by a subscriber, and a participant
	// enrolled for some other plane whose class declares no CrowdyState property.
	UObject* LayoutlessParticipant = NewObject<UCrowdyStateLayoutlessParticipant>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterParticipantAs(Entities, EntityID, LayoutlessParticipant, ECrowdyRole::HostOwned, FGuid());

	TestFalse(TEXT("a HostOwned record with no owner is not locally owned, so the world-entity gate is the one that answers"),
		Entities->IsLocallyOwned(EntityID));
	TestNull(TEXT("and the participant's class carries no CrowdyState rep layout"),
		Registry->FindRepLayout(LayoutlessParticipant->GetClass()));

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 777;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	// Everything about this delta is valid except its authority: a foreign sender, no HostSourced flag.
	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;
	Delta.Flags = 0;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("a non-host delta for a world entity never reaches the subscriber"),
		Subscriber->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("and the subscriber path counted nothing"),
		Router->GetStateSubscriberStats().Applied, static_cast<int64>(0));
	TestEqual(TEXT("the subscriber is not even asked whether it holds the id: the refusal is settled before the state's destination is"),
		Subscriber->IsEntityKnownCallCount, 0);
	TestEqual(TEXT("nor for its class"), Subscriber->GetEntityClassCallCount, 0);
	TestEqual(TEXT("a refusal is final, never a wait"), Router->NumDeferredForTest(), 0);

	// The drop is reported only under the state trace, so without this counter a refused peer write and a
	// silent channel would be indistinguishable on a default build.
	TestEqual(TEXT("the refusal is counted, so it is visible with no log line at all"),
		Router->GetStateParticipantStats().DroppedNotHostSourcedForWorld, static_cast<int64>(1));

	return true;
}

// The discriminating half of the pair above: the same world entity, the same layout-less participant, the same
// subscriber, and a delta that differs only by carrying the host's mark. It must reach the subscriber and
// deliver its value, or the refusal above would be proving nothing more than that the path never works.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentHostSourcedDeltaReachesProviderTest,
	"CrowdySDK.StateFragment.HostSourcedDeltaReachesProviderForWorldEntity", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentHostSourcedDeltaReachesProviderTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UObject* LayoutlessParticipant = NewObject<UCrowdyStateLayoutlessParticipant>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterParticipantAs(Entities, EntityID, LayoutlessParticipant, ECrowdyRole::HostOwned, FGuid());

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 777;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	int32 DeliveredSlots = 0;
	int32 DeliveredRepInt = INDEX_NONE;
	Subscriber->OnApply = [&](const FGuid&, const FCrowdyRepLayout& L, const void* DecodedContainer, TConstArrayView<int32> Delivered)
	{
		DeliveredSlots += Delivered.Num();
		for (const int32 Index : Delivered)
		{
			if (const FIntProperty* RepIntProp = CastField<FIntProperty>(L.Properties[Index].Property))
			{
				DeliveredRepInt = RepIntProp->GetPropertyValue_InContainer(DecodedContainer);
			}
		}
	};

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;
	Delta.Flags = CrowdyStateDeltaFlags::HostSourced;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the host's delta reached the subscriber"), Subscriber->ApplyDecodedStateCallCount, 1);
	TestEqual(TEXT("carrying exactly the one slot it held"), DeliveredSlots, 1);
	TestEqual(TEXT("and the value that arrived is the one the delta carried"), DeliveredRepInt, 777);
	TestEqual(TEXT("counted as applied on the subscriber path"),
		Router->GetStateSubscriberStats().Applied, static_cast<int64>(1));

	return true;
}

// The end of the line for a participant that cannot hold state and an entity nothing else holds either: the
// subscriber is asked, refuses the id, and the delta is dropped there and then. Waiting is not an option and
// is not taken: the entity is already registered, so no later attempt can give this participant a layout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentLayoutlessParticipantWithNoClaimDropsTest,
	"CrowdySDK.StateFragment.LayoutlessParticipantWithNoClaimDropsWithoutDeferring", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentLayoutlessParticipantWithNoClaimDropsTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UObject* LayoutlessParticipant = NewObject<UCrowdyStateLayoutlessParticipant>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, LayoutlessParticipant);

	// A subscriber is registered but holds a DIFFERENT id, so it is genuinely consulted and genuinely
	// refuses. A test with no subscriber at all could not tell "asked and refused" from "never asked".
	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Subscriber->SetOwnedClass(FGuid::NewGuid(), TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 99;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the subscriber was consulted for the id"), Subscriber->IsEntityKnownCallCount, 1);
	TestEqual(TEXT("refused it"), Router->GetStateSubscriberStats().NotClaimed, static_cast<int64>(1));

	// An id it does not hold is settled by that one answer: asking for a class as well would be the router
	// reading a second question it has already been told the answer to.
	TestEqual(TEXT("and was never asked for a class it could not have"), Subscriber->GetEntityClassCallCount, 0);
	TestEqual(TEXT("and was never asked to apply anything"), Subscriber->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("the delta is dropped, not held: a registered participant will not grow a layout later"),
		Router->NumDeferredForTest(), 0);
	TestEqual(TEXT("and it never occupied the distinct-entity cap either"),
		Router->NumDeferredEntitiesForTest(), 0);

	// The drop's own log line is rate-limited, so under a flood most of them say nothing at all. The counter
	// is not, which is what keeps a class nobody can apply state for from looking like an idle channel.
	TestEqual(TEXT("the drop is counted, independently of whether its throttled line was emitted"),
		Router->GetStateParticipantStats().DroppedNoLayoutNoSubscriber, static_cast<int64>(1));

	return true;
}

// The same rule for the entities this client owns rather than the world's: we are the authority for what we
// own, so a foreign delta that is not a host correction is refused whether the state would have landed on the
// participant or in a fragment. Registered with the local player as owner, which is what makes this the
// owned-entity gate's case rather than the world entity's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentOwnedGateAppliesOnFallThroughTest,
	"CrowdySDK.StateFragment.OwnedGateAppliesOnFallThrough", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentOwnedGateAppliesOnFallThroughTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UObject* LayoutlessParticipant = NewObject<UCrowdyStateLayoutlessParticipant>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterParticipantAs(Entities, EntityID, LayoutlessParticipant, ECrowdyRole::Owner, LocalPlayer);

	TestTrue(TEXT("the entity is locally owned, so the owned-entity gate is the one that answers"),
		Entities->IsLocallyOwned(EntityID));

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	Subscriber->SetOwnedClass(EntityID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 55;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = EntityID;
	Delta.SenderID = FGuid::NewGuid(); // foreign, so the self-echo drop is not what refuses this
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;
	Delta.Flags = 0;

	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("a foreign non-host delta for an entity we own never reaches the subscriber"),
		Subscriber->ApplyDecodedStateCallCount, 0);
	TestEqual(TEXT("the subscriber path counted nothing"),
		Router->GetStateSubscriberStats().Applied, static_cast<int64>(0));
	TestEqual(TEXT("and the refusal is counted, which is the only account of it at default verbosity"),
		Router->GetStateParticipantStats().DroppedNotHostSourcedForOwned, static_cast<int64>(1));

	return true;
}

/**
 * An entity a subscriber holds with no class recorded for it is REFUSED, never waited on.
 *
 * This is the case that only exists because "do you hold this id" and "what class is it" are two questions.
 * An entity may legitimately be a position and nothing else, so a null class says there is nothing to read
 * the bytes against, not that the id belongs to somebody else. If the router took it for the latter, every
 * delta for such an entity would be queued on a spawn that already happened, and a crowd of them would fill
 * the shared queue and evict the entities genuinely still arriving.
 *
 * Three ids, because a refusal is only meaningful beside the two answers it has to be told apart from: one
 * held with no class (refused), one held with a class (applied, from the identical delta body), and one held
 * by nobody (deferred). Collapsing the two questions into one would move the first id into the third's
 * behaviour, and the deferral count below is what would catch it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentHeldWithNoClassRefusesWithoutDeferringTest,
	"CrowdySDK.StateFragment.HeldWithNoClassRefusesWithoutDeferring", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentHeldWithNoClassRefusesWithoutDeferringTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* TargetClass = UCrowdyStateCodecTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("codec target has a layout"), Layout))
	{
		return false;
	}

	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (!TestNotEqual(TEXT("RepInt present"), RepIntIndex, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();

	const FGuid ClasslessID = FGuid::NewGuid();
	const FGuid ClassedID = FGuid::NewGuid();
	const FGuid UnheldID = FGuid::NewGuid();
	Subscriber->SetHeldWithNoClass(ClasslessID);
	Subscriber->SetOwnedClass(ClassedID, TargetClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	// One delta body for all three ids, so nothing about the payload can explain a difference in outcome.
	UCrowdyStateCodecTarget* Source = NewObject<UCrowdyStateCodecTarget>();
	Source->RepInt = 31337;
	TBitArray<> Dirty(false, Layout->Properties.Num());
	Dirty[RepIntIndex] = true;
	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	Delta.EntityID = ClasslessID;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the subscriber was asked whether it holds the id"), Subscriber->IsEntityKnownCallCount, 1);
	TestEqual(TEXT("it said yes, so the delta was claimed rather than left to wait"),
		Router->GetStateSubscriberStats().Claimed, static_cast<int64>(1));
	TestEqual(TEXT("and nothing was counted as unclaimed"),
		Router->GetStateSubscriberStats().NotClaimed, static_cast<int64>(0));
	TestEqual(TEXT("it was then asked for a class and answered null"), Subscriber->GetEntityClassCallCount, 1);
	TestEqual(TEXT("so the delta is refused for having no class, under its own counter"),
		Router->GetStateSubscriberStats().DroppedNoEntityClass, static_cast<int64>(1));
	TestEqual(TEXT("nothing was applied: there is no layout to read the bytes against"),
		Subscriber->ApplyDecodedStateCallCount, 0);

	// The assertion the whole separation exists for. Answering IsEntityKnown from the class would put this
	// delta here instead, on a queue whose only possible outcome is a timeout.
	TestEqual(TEXT("and it is refused rather than queued: nothing waits on an entity that is already here"),
		Router->NumDeferredForTest(), 0);
	TestEqual(TEXT("so it occupies none of the distinct-entity cap either"),
		Router->NumDeferredEntitiesForTest(), 0);

	// Control one: the same bytes DO land for an entity the subscriber holds WITH a class, so the refusal
	// above was the missing class and not the delta, the layout, or the path being dead.
	Delta.EntityID = ClassedID;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("the identical delta applies for an entity that does have a class"),
		Subscriber->ApplyDecodedStateCallCount, 1);
	TestEqual(TEXT("carrying the slot it held"), Subscriber->LastChangedIndices.Num(), 1);

	// Control two: an id the subscriber does not hold at all still waits, so "refused" and "not mine" remain
	// genuinely different outcomes rather than both having quietly become one.
	Delta.EntityID = UnheldID;
	Router->DispatchEvent(MakeInboundStateEvent(Delta));

	TestEqual(TEXT("an id nobody holds is still deferred"), Router->NumDeferredForTest(), 1);
	TestEqual(TEXT("and counted as unclaimed rather than refused"),
		Router->GetStateSubscriberStats().NotClaimed, static_cast<int64>(1));
	TestEqual(TEXT("the no-class refusal count did not move for it"),
		Router->GetStateSubscriberStats().DroppedNoEntityClass, static_cast<int64>(1));

	return true;
}

// The present set is handed to the subscriber by reference and stays live for the whole of ApplyDecodedState,
// and a subscriber may dispatch another delta from inside it. What this asserts is the OUTER call's output:
// the slot set it is still reading once the inner delta has been and gone must be the set its OWN delta
// carried. The two deltas name different entities of different classes and carry disjoint slot indices, so a
// set written by the inner one is unmistakable.
//
// Both deltas carry the SAME NUMBER of slots on purpose. The outer call reads the set through a view over the
// router's array, and a set of a different size would resize that array rather than merely overwrite it, which
// would leave the outer view addressing memory that is no longer the array's. Equal sizes keep the failure
// this test is written to catch (the wrong VALUES) the only thing that can happen.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateFragmentReentrantApplyKeepsPresentSetTest,
	"CrowdySDK.StateFragment.ReentrantApplyKeepsTheOuterPresentSet", CrowdyStateFragmentTestFlags)
bool FCrowdyStateFragmentReentrantApplyKeepsPresentSetTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	UClass* OuterClass = UCrowdyStateCodecTarget::StaticClass();
	UClass* InnerClass = UCrowdyStateFragmentHeapTarget::StaticClass();
	const FCrowdyRepLayout* OuterLayout = Registry->FindRepLayout(OuterClass);
	const FCrowdyRepLayout* InnerLayout = Registry->FindRepLayout(InnerClass);
	if (!TestNotNull(TEXT("the outer class has a layout"), OuterLayout)
		|| !TestNotNull(TEXT("the inner class has a layout"), InnerLayout))
	{
		return false;
	}

	// The last two slots of one layout and the first two of the other, so neither set can be mistaken for the
	// other and both are the same length.
	const int32 OuterN = OuterLayout->Properties.Num();
	if (!TestTrue(TEXT("both layouts carry at least two slots"), OuterN >= 4 && InnerLayout->Properties.Num() >= 2))
	{
		return false;
	}
	const TArray<int32> ExpectedOuter = { OuterN - 2, OuterN - 1 };
	const TArray<int32> ExpectedInner = { 0, 1 };

	TBitArray<> OuterDirty;
	OuterDirty.Init(false, OuterN);
	OuterDirty[ExpectedOuter[0]] = true;
	OuterDirty[ExpectedOuter[1]] = true;
	UCrowdyStateCodecTarget* OuterSource = NewObject<UCrowdyStateCodecTarget>();
	TArray<uint8> OuterBlob;
	FCrowdyStateCodec::Encode(*OuterLayout, OuterSource, OuterDirty, /*bKeyframe=*/false, OuterBlob);

	TBitArray<> InnerDirty;
	InnerDirty.Init(false, InnerLayout->Properties.Num());
	InnerDirty[ExpectedInner[0]] = true;
	InnerDirty[ExpectedInner[1]] = true;
	UCrowdyStateFragmentHeapTarget* InnerSource = NewObject<UCrowdyStateFragmentHeapTarget>();
	InnerSource->RepString = TEXT("inner");
	InnerSource->RepName = FName(TEXT("Inner"));
	TArray<uint8> InnerBlob;
	FCrowdyStateCodec::Encode(*InnerLayout, InnerSource, InnerDirty, /*bKeyframe=*/false, InnerBlob);

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateFragmentSpySubscriber* Subscriber = NewObject<UCrowdyStateFragmentSpySubscriber>();
	const FGuid OuterID = FGuid::NewGuid();
	const FGuid InnerID = FGuid::NewGuid();
	Subscriber->SetOwnedClass(OuterID, OuterClass);
	Subscriber->SetOwnedClass(InnerID, InnerClass);
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(Subscriber));

	FCrowdyStateDelta InnerDelta;
	InnerDelta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(InnerClass));
	InnerDelta.EntityID = InnerID;
	InnerDelta.SenderID = FGuid::NewGuid();
	InnerDelta.LayoutHash = InnerLayout->LayoutHash;
	InnerDelta.Blob = InnerBlob;

	FCrowdyStateDelta OuterDelta;
	OuterDelta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(OuterClass));
	OuterDelta.EntityID = OuterID;
	OuterDelta.SenderID = FGuid::NewGuid();
	OuterDelta.LayoutHash = OuterLayout->LayoutHash;
	OuterDelta.Blob = OuterBlob;

	int32 ApplyDepth = 0;
	TArray<int32> OuterSeenBefore;
	TArray<int32> OuterSeenAfter;
	TArray<int32> InnerSeen;
	Subscriber->OnApply = [&](const FGuid& EntityUUID, const FCrowdyRepLayout&, const void*,
		TConstArrayView<int32> Present)
	{
		if (EntityUUID != OuterID)
		{
			InnerSeen = TArray<int32>(Present.GetData(), Present.Num());
			return;
		}

		// Once only: the inner delta's own apply must not start the chain again.
		if (ApplyDepth++ > 0)
		{
			return;
		}

		OuterSeenBefore = TArray<int32>(Present.GetData(), Present.Num());
		Router->DispatchEvent(MakeInboundStateEvent(InnerDelta));
		OuterSeenAfter = TArray<int32>(Present.GetData(), Present.Num());
	};

	Router->DispatchEvent(MakeInboundStateEvent(OuterDelta));

	TestEqual(TEXT("both deltas reached the subscriber"), Subscriber->ApplyDecodedStateCallCount, 2);
	TestTrue(TEXT("the two present sets are genuinely distinguishable"), ExpectedOuter != ExpectedInner);
	TestTrue(TEXT("the inner delta carried its own two slots"), InnerSeen == ExpectedInner);
	TestTrue(TEXT("the outer call saw its own slots before the nested dispatch"), OuterSeenBefore == ExpectedOuter);

	// The load-bearing assertion: the set the outer call is still reading is the one its own delta carried.
	if (!(OuterSeenAfter == ExpectedOuter))
	{
		AddError(FString::Printf(
			TEXT("the outer call's present set was overwritten by the nested dispatch: it read [%s] where its own delta carried [%s]"),
			*FString::JoinBy(OuterSeenAfter, TEXT(","), [](int32 I) { return FString::FromInt(I); }),
			*FString::JoinBy(ExpectedOuter, TEXT(","), [](int32 I) { return FString::FromInt(I); })));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
