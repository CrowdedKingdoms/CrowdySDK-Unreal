#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Backend/CrowdyMassRenderingBackend.h"
#include "CrowdyMassViewStateTestTypes.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "MassCommonFragments.h"
#include "MassEntityManager.h"
#include "UObject/StrongObjectPtr.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "View/CrowdyMassViewState.h"

// Covers the view-state ingestion mechanism end to end: real wire bytes in, through the same decode
// and the same reflection-driven field resolution the live receive path uses, and out as the fragment
// values that actually land. The test wire structs stand in for a game's own, so this stays inside
// CrowdyMass with no dependency on any specific game plugin.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassViewStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct from any TypeID another module's tests register against the same process-wide registry
	// singleton (CrowdyNet's own actor-state wire tests use a different constant).
	constexpr FCrowdyTypeID ViewStateTestActorStateTypeID = 47001;
	constexpr FCrowdyTypeID ViewStateWideFieldTestActorStateTypeID = 47002;

	// Values no decode path ever produces, written onto an entity before an update so that a decode
	// step which silently does nothing leaves an assertion failure behind instead of coincidentally
	// matching the defaults a freshly constructed fragment already has.
	constexpr uint8 PoisonedPoseId = 77;
	constexpr int32 PoisonedSequenceIndex = 4242;
	constexpr int32 PoisonedActionId = 99;

	void RegisterViewStateTestWireStructs()
	{
		UActorUpdatePayloadRegistry::Get()->RegisterStruct(
			FCrowdyMassViewStateTestActorState::StaticStruct(), ViewStateTestActorStateTypeID);
		UActorUpdatePayloadRegistry::Get()->RegisterStruct(
			FCrowdyMassViewStateWideFieldTestActorState::StaticStruct(), ViewStateWideFieldTestActorStateTypeID);
	}

	template <typename T>
	void AppendLE(TArray<uint8>& Out, T Value)
	{
		const int32 Base = Out.Num();
		Out.SetNumUninitialized(Base + sizeof(T));
		FMemory::Memcpy(Out.GetData() + Base, &Value, sizeof(T));
	}

	// Any non-zero value serves: these tests pin view-state ingestion, not which class sent a frame.
	// Zero is the one class id the framing refuses outright.
	constexpr FCrowdyClassID ViewStateTestClassID = 0x5EE17A9Eu;

	// The actor-state framing prefix, written by hand: [uint16 sentinel][uint8 format version][uint16
	// type tag][uint32 class id]. Authored here rather than obtained from SerializeActorState for the
	// same reason every payload below is authored byte by byte: a defect shared between the writer and
	// the reader is invisible to a round trip that only checks the two against each other.
	void AppendViewStateFraming(TArray<uint8>& Out, const FCrowdyTypeID TypeID)
	{
		AppendLE(Out, static_cast<FCrowdyTypeID>(CROWDY_ACTOR_STATE_SENTINEL));
		Out.Add(CROWDY_ACTOR_STATE_FORMAT_VERSION);
		AppendLE(Out, TypeID);
		AppendLE(Out, ViewStateTestClassID);
	}

	// 48 zero bytes: FVector::ZeroVector and FRotator::ZeroRotator both decode from an all-zero region,
	// which is what keeps every payload below focused on the appended view-state bytes that come after
	// it. Their own values are never asserted on.
	void AppendZeroTransform(TArray<uint8>& Out)
	{
		for (int32 Index = 0; Index < 48; ++Index)
		{
			Out.Add(0);
		}
	}

	// A standalone Mass entity manager with no World, mirroring the fixture in
	// Mass/CrowdyMassAvatarEventTests.cpp: PostInitialize constructs relation observers outered to
	// whatever owner the manager was given, and an owner-less manager takes the process down with
	// "Object is not packaged".
	struct FCrowdyMassViewStateTestFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassViewStateTestFixture()
		{
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());

			// Initialize() is never called on a subsystem built this way (there is no World subsystem
			// collection to construct it through), so without this the field registrations ApplyViewState
			// resolves against are empty and every ingested byte would be silently dropped, which would
			// make every assertion below pass whether or not decode actually happened.
			Subsystem->RegisterStandardViewStateFieldsForTest();

			// The pose ingestor these cases use as their vehicle is registered HERE, through the same
			// public call a consuming game uses. CrowdyMass stopped shipping one on this plane in Phase
			// A.4: a byte on the echo is a row id the SENDER chose, and the pose now travels as the game's
			// own enum on the CrowdyState plane instead.
			//
			// Nothing about what these cases measure changed. They are about the ingestion LOOP (decode,
			// per-struct resolution, fail-safe defaults, an unclaimed field, a wider field refused), and
			// the pose byte is simply the field they watch land. Registering it here makes them measure
			// that loop through its public surface, which is what the loop is now for.
			Subsystem->RegisterViewStateField(TEXT("PoseId"),
				[](const void* PropertyValuePtr, const FCrowdyMassViewStateContext& Context)
				{
					if (FCrowdyMassViewPoseFragment* PoseFragment = Context.GetFragment<FCrowdyMassViewPoseFragment>())
					{
						// Stored exactly as sent. One case asserts a forged id is NOT clamped here, because
						// clamping would turn it into a plausible wrong pose instead of no pose.
						PoseFragment->PoseId = *static_cast<const uint8*>(PropertyValuePtr);
					}
				});
		}

		~FCrowdyMassViewStateTestFixture()
		{
			EntityManager->Deinitialize();
		}

		// A real entity carrying the pose fragment the standard fields land on, plus three fragments no
		// registered field touches, tracked under UUID exactly as SpawnRemoteEntity's own bookkeeping
		// would.
		FMassEntityHandle MakeTrackedEntity(const FGuid& UUID)
		{
			const UScriptStruct* Fragments[] = {
				FCrowdyMassViewPoseFragment::StaticStruct(), FCrowdyMassTraceCollisionFragment::StaticStruct(),
				FCrowdyMassAnimStateFragment::StaticStruct(), FCrowdyMassActionFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 4));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}

		// An entity with nothing for a pose to land on: a config with no animation trait, or a headless
		// server with nothing to draw. A pose arriving for it has to be dropped rather than land
		// somewhere, so this is the archetype that catches an ingestor assuming every entity carries the
		// fragment its field targets.
		// The pose fragment plus the two the transform half writes, so a case measuring one whole update
		// exercises both halves rather than entering the second and returning at its first guard.
		FMassEntityHandle MakeTransformTrackedEntity(const FGuid& UUID)
		{
			const UScriptStruct* Fragments[] = {
				FCrowdyMassViewPoseFragment::StaticStruct(), FTransformFragment::StaticStruct(),
				FCrowdyRemoteInterpolationFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 3));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}

		FMassEntityHandle MakePoselessEntity(const FGuid& UUID)
		{
			const UScriptStruct* Fragments[] = {
				FCrowdyMassTraceCollisionFragment::StaticStruct(), FCrowdyMassAnimStateFragment::StaticStruct(),
				FCrowdyMassActionFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 3));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}

		FCrowdyMassViewPoseFragment* GetPose(FMassEntityHandle Entity)
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Entity);
		}

		FCrowdyMassTraceCollisionFragment* GetTraceCollision(FMassEntityHandle Entity)
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>(Entity);
		}

		FCrowdyMassAnimStateFragment* GetAnimState(FMassEntityHandle Entity)
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassAnimStateFragment>(Entity);
		}

		FCrowdyMassActionFragment* GetAction(FMassEntityHandle Entity)
		{
			return EntityManager->GetFragmentDataPtr<FCrowdyMassActionFragment>(Entity);
		}

		void PoisonEntity(FMassEntityHandle Entity)
		{
			if (FCrowdyMassViewPoseFragment* Pose = GetPose(Entity))
			{
				Pose->PoseId = PoisonedPoseId;
			}

			if (FCrowdyMassAnimStateFragment* AnimState = GetAnimState(Entity))
			{
				AnimState->SequenceIndex = PoisonedSequenceIndex;
			}

			if (FCrowdyMassActionFragment* Action = GetAction(Entity))
			{
				Action->ActionId = PoisonedActionId;
			}
		}
	};

	// Builds a full FCrowdyMassViewStateTestActorState wire frame: tag, zero transform, then the four
	// appended bytes in their declared order. TrailingJunkByteCount appends that many extra 0xEE bytes
	// after the struct, for the leftover-bytes cases.
	TArray<uint8> BuildFullPayload(uint8 Version, uint8 StanceFlags, uint8 HealthPercent, uint8 PoseId, int32 TrailingJunkByteCount = 0)
	{
		TArray<uint8> Payload;
		AppendViewStateFraming(Payload, ViewStateTestActorStateTypeID);
		AppendZeroTransform(Payload);
		Payload.Add(Version);
		Payload.Add(StanceFlags);
		Payload.Add(HealthPercent);
		Payload.Add(PoseId);

		for (int32 Index = 0; Index < TrailingJunkByteCount; ++Index)
		{
			Payload.Add(0xEE);
		}

		return Payload;
	}

	// A frame carrying only the transform: the wire shape a client built before view state existed
	// actually sends.
	TArray<uint8> BuildLegacyPayload()
	{
		TArray<uint8> Payload;
		AppendViewStateFraming(Payload, ViewStateTestActorStateTypeID);
		AppendZeroTransform(Payload);
		return Payload;
	}

	// A frame that starts writing the appended bytes and stops partway through them, distinct from the
	// legacy case above: this one is a genuinely corrupt or cut-off frame, not a sender that never had
	// these fields.
	TArray<uint8> BuildMidTruncatedPayload()
	{
		TArray<uint8> Payload;
		AppendViewStateFraming(Payload, ViewStateTestActorStateTypeID);
		AppendZeroTransform(Payload);
		Payload.Add(1); // Version
		Payload.Add(0); // StanceFlags
		// HealthPercent and PoseId are missing.
		return Payload;
	}

	// The first of the four float bytes below. On a little-endian target that is the byte at the
	// property's own address, so it is what a read through a byte pointer would pick up. Nothing depends
	// on which float the four bytes spell; what matters is that this one is a value an assertion can see
	// if the width check were ever dropped.
	constexpr uint8 WideFieldLeadingByte = 0x2A;

	// A wire frame whose PoseId is a float rather than a byte. Declared order is transform, version,
	// then the four float bytes.
	TArray<uint8> BuildWideFieldPayload()
	{
		TArray<uint8> Payload;
		AppendViewStateFraming(Payload, ViewStateWideFieldTestActorStateTypeID);
		AppendZeroTransform(Payload);
		Payload.Add(1); // Version
		Payload.Add(WideFieldLeadingByte);
		Payload.Add(0x00);
		Payload.Add(0x92);
		Payload.Add(0x42);
		return Payload;
	}
}

// The load-bearing test for the ingestion mechanism. Builds authored literal byte payloads and pushes
// them through the real decode path (USerializationFunctionLibrary::DeserializeActorState, the exact
// function the live receive path calls) and the real ingestion path
// (UCrowdyMassEntitySubsystem::ApplyViewState), then asserts the fragment values that actually land.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateDecodeBoundsTest,
	"CrowdySDK.Mass.ViewStateDecodeBounds", CrowdyMassViewStateTestFlags)
bool FCrowdyMassViewStateDecodeBoundsTest::RunTest(const FString& Parameters)
{
	RegisterViewStateTestWireStructs();
	FCrowdyMassViewStateTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity(UUID);

	// Case A: a legacy payload carrying only the transform, none of the appended bytes. A sender one
	// build behind must still decode, and its fields must read as the resting state the wire struct's
	// own defaults encode, or every player on an older client shows whatever the receiver happened to
	// have on the entity already.
	{
		Fixture.PoisonEntity(Entity);

		FInstancedStruct Decoded;
		const bool bDecoded = USerializationFunctionLibrary::DeserializeActorState(BuildLegacyPayload(), Decoded);
		TestTrue(TEXT("a legacy payload with none of the appended bytes still decodes"), bDecoded);

		Fixture.Subsystem->ApplyViewState(UUID, Decoded);
		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (TestNotNull(TEXT("pose fragment present"), Pose))
		{
			TestEqual(TEXT("a legacy payload lands the wire struct's fail-safe default, which is no pose"),
				Pose->PoseId, static_cast<uint8>(0));
		}
	}

	// Case B: a frame that starts writing the appended bytes and is cut off partway through them.
	//
	// It is accepted, and the fields past the cut keep their defaults, which is the same treatment the
	// legacy frame in case A gets. That is not an oversight in the policy, it is the policy: the two
	// frames are indistinguishable to the decoder. SerializeBin walks properties in declared order and
	// a short payload simply stops satisfying reads, so "a sender that never had this field" and "a
	// sender that had it and was cut off" produce the identical archive error at the identical offset.
	// Nothing on the wire separates them, so the only choice available is which way to be wrong, and
	// the fail-safe defaults are what make accepting the safe direction: rejecting would drop every
	// update from a peer one version behind.
	{
		Fixture.PoisonEntity(Entity);

		FInstancedStruct Decoded;
		const bool bDecoded = USerializationFunctionLibrary::DeserializeActorState(BuildMidTruncatedPayload(), Decoded);
		TestTrue(TEXT("a frame truncated partway through the appended bytes still decodes"), bDecoded);

		Fixture.Subsystem->ApplyViewState(UUID, Decoded);
		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (TestNotNull(TEXT("pose fragment present"), Pose))
		{
			TestEqual(TEXT("a field past the cut keeps its fail-safe default, which is no pose"),
				Pose->PoseId, static_cast<uint8>(0));
		}
	}

	// Case C: a forged pose id far outside any authored range. Ingestion stores it verbatim (no clamp;
	// every uint8 is a valid id or the 0 sentinel), so the safety net is entirely downstream, at
	// FCrowdyMassResolvedAnimSet::FindResolvedPose, covered directly by CrowdySDK.Mass.PoseIdBounds in
	// CrowdyMassAnimSetTests.cpp. Clamping here instead would silently reinterpret a forged id as a
	// real pose, which is a worse answer than one the consumer rejects.
	{
		Fixture.PoisonEntity(Entity);

		FInstancedStruct Decoded;
		USerializationFunctionLibrary::DeserializeActorState(BuildFullPayload(1, 0, 255, 250), Decoded);
		Fixture.Subsystem->ApplyViewState(UUID, Decoded);

		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (TestNotNull(TEXT("pose fragment present"), Pose))
		{
			TestEqual(TEXT("a forged pose id is stored as-is, not clamped at ingestion"),
				Pose->PoseId, static_cast<uint8>(250));
		}
	}

	// Case D: a version byte higher than this build decodes. Nothing gates decode on it, so a peer on a
	// newer build still lands every field this build does recognise. Gating instead would make every
	// update from that peer vanish. The mismatch is reported at Warning level, which does not fail a
	// running test.
	{
		Fixture.PoisonEntity(Entity);

		FInstancedStruct Decoded;
		const uint8 FutureVersion = static_cast<uint8>(CrowdyMassViewStateWireVersion + 100);
		USerializationFunctionLibrary::DeserializeActorState(BuildFullPayload(FutureVersion, 0, 255, 9), Decoded);
		Fixture.Subsystem->ApplyViewState(UUID, Decoded);

		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (TestNotNull(TEXT("pose fragment present"), Pose))
		{
			TestEqual(TEXT("a version this build does not know about does not stop the other fields landing"),
				Pose->PoseId, static_cast<uint8>(9));
		}
	}

	// Case E: trailing junk small enough that the leftover-bytes policy treats it as ordinary version
	// skew (tail no larger than what was decoded). Accepted, and the fields ahead of the tail apply.
	{
		Fixture.PoisonEntity(Entity);

		FInstancedStruct Decoded;
		const bool bDecoded = USerializationFunctionLibrary::DeserializeActorState(
			BuildFullPayload(1, 0, 255, 4, /*TrailingJunkByteCount=*/10), Decoded);
		TestTrue(TEXT("a small trailing tail is accepted"), bDecoded);

		Fixture.Subsystem->ApplyViewState(UUID, Decoded);
		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (TestNotNull(TEXT("pose fragment present"), Pose))
		{
			TestEqual(TEXT("fields ahead of a small tail still apply"), Pose->PoseId, static_cast<uint8>(4));
		}
	}

	// Case F: trailing junk larger than what was decoded. The policy still accepts the frame (the bytes
	// ahead of the tail are complete) but logs a Warning; this test does not need to whitelist it, since
	// only Error-level logs fail a running test here.
	{
		Fixture.PoisonEntity(Entity);

		FInstancedStruct Decoded;
		const bool bDecoded = USerializationFunctionLibrary::DeserializeActorState(
			BuildFullPayload(1, 0, 255, 5, /*TrailingJunkByteCount=*/60), Decoded);
		TestTrue(TEXT("a large trailing tail is still accepted, per the documented policy"), bDecoded);

		Fixture.Subsystem->ApplyViewState(UUID, Decoded);
		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (TestNotNull(TEXT("pose fragment present"), Pose))
		{
			TestEqual(TEXT("fields ahead of a large tail still apply"), Pose->PoseId, static_cast<uint8>(5));
		}
	}

	return true;
}

// A wire struct carries whatever fields its game wants, and CrowdyMass registers an ingestor for two
// names only. A field nothing claims has to be left alone: not read through a byte pointer, and not
// landed on whatever fragment happens to be to hand. Absent that, adding a field to a game's wire
// struct would quietly rewrite state on entities in every other game that shares the plugin.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateUnregisteredFieldIgnoredTest,
	"CrowdySDK.Mass.ViewStateUnregisteredFieldIgnored", CrowdyMassViewStateTestFlags)
bool FCrowdyMassViewStateUnregisteredFieldIgnoredTest::RunTest(const FString& Parameters)
{
	RegisterViewStateTestWireStructs();
	FCrowdyMassViewStateTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity(UUID);

	// The whole test is vacuous if the wire struct has stopped carrying the two unclaimed fields, and a
	// struct with no such field would satisfy every assertion below.
	const UScriptStruct* WireStruct = FCrowdyMassViewStateTestActorState::StaticStruct();
	TestNotNull(TEXT("the wire struct carries a field no ingestor is registered for"),
		WireStruct->FindPropertyByName(TEXT("StanceFlags")));
	TestNotNull(TEXT("and a second one"), WireStruct->FindPropertyByName(TEXT("HealthPercent")));

	constexpr uint8 UnclaimedStanceFlags = 0xA5;
	constexpr uint8 UnclaimedHealthPercent = 0x5A;
	constexpr uint8 EchoedPoseId = 9;

	Fixture.PoisonEntity(Entity);

	const FCrowdyMassTraceCollisionFragment* TraceCollisionPtr = Fixture.GetTraceCollision(Entity);
	if (!TestNotNull(TEXT("trace collision fragment present"), TraceCollisionPtr))
	{
		return false;
	}
	const FCrowdyMassTraceCollisionFragment TraceCollisionBefore = *TraceCollisionPtr;

	FInstancedStruct Decoded;
	const bool bDecoded = USerializationFunctionLibrary::DeserializeActorState(
		BuildFullPayload(1, UnclaimedStanceFlags, UnclaimedHealthPercent, EchoedPoseId), Decoded);
	TestTrue(TEXT("the payload decodes"), bDecoded);

	// The unclaimed bytes really did arrive and really did decode. Without this the test could not tell
	// "the ingestion path ignored them" from "they never reached the ingestion path at all", and only
	// the first of those is the property being asserted.
	const FCrowdyMassViewStateTestActorState* DecodedState = Decoded.GetPtr<FCrowdyMassViewStateTestActorState>();
	if (!TestNotNull(TEXT("the payload decodes as the test wire struct"), DecodedState))
	{
		return false;
	}
	TestEqual(TEXT("the unclaimed stance byte survived decode"), DecodedState->StanceFlags, UnclaimedStanceFlags);
	TestEqual(TEXT("the unclaimed health byte survived decode"), DecodedState->HealthPercent, UnclaimedHealthPercent);

	Fixture.Subsystem->ApplyViewState(UUID, Decoded);

	// The claimed field landing is what proves this payload was processed at all, so the untouched
	// fragments below mean "ignored" rather than "nothing ran".
	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (TestNotNull(TEXT("pose fragment present"), Pose))
	{
		TestEqual(TEXT("the claimed field lands, so this payload was genuinely ingested"),
			Pose->PoseId, EchoedPoseId);
	}

	FCrowdyMassAnimStateFragment* AnimState = Fixture.GetAnimState(Entity);
	if (TestNotNull(TEXT("anim state fragment present"), AnimState))
	{
		TestEqual(TEXT("an unclaimed field lands on no other fragment"),
			AnimState->SequenceIndex, PoisonedSequenceIndex);
	}

	FCrowdyMassActionFragment* Action = Fixture.GetAction(Entity);
	if (TestNotNull(TEXT("action fragment present"), Action))
	{
		TestEqual(TEXT("nor on the action fragment"), Action->ActionId, PoisonedActionId);
	}

	const FCrowdyMassTraceCollisionFragment* TraceCollisionAfter = Fixture.GetTraceCollision(Entity);
	if (TestNotNull(TEXT("trace collision fragment still resolvable"), TraceCollisionAfter))
	{
		TestTrue(TEXT("and trace collision is untouched, byte for byte"),
			FMemory::Memcmp(&TraceCollisionBefore, TraceCollisionAfter, sizeof(FCrowdyMassTraceCollisionFragment)) == 0);
	}

	return true;
}

// A registered name is claimed across every wire struct in the process, including structs from games
// this build has never seen. Matching on the name alone would read an unrelated float of that name
// through a byte pointer, which is a value nobody wrote and which changes as the float does. The field
// is refused instead, and refusing leaves the target exactly as an absent field does.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateWideFieldRefusedTest,
	"CrowdySDK.Mass.ViewStateWideFieldRefused", CrowdyMassViewStateTestFlags)
bool FCrowdyMassViewStateWideFieldRefusedTest::RunTest(const FString& Parameters)
{
	RegisterViewStateTestWireStructs();
	FCrowdyMassViewStateTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity(UUID);

	Fixture.PoisonEntity(Entity);

	FInstancedStruct WideDecoded;
	const bool bWideDecoded = USerializationFunctionLibrary::DeserializeActorState(BuildWideFieldPayload(), WideDecoded);
	TestTrue(TEXT("the wide-field payload decodes as its own wire struct"), bWideDecoded);

	Fixture.Subsystem->ApplyViewState(UUID, WideDecoded);

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose))
	{
		return false;
	}
	TestEqual(TEXT("a same-named field that is not a single byte is refused, leaving the pose untouched"),
		Pose->PoseId, PoisonedPoseId);

	// The control, on the same entity through the same subsystem: the identical leading byte, carried by
	// a field that genuinely is a single byte, does land. Without it, a refusal is indistinguishable
	// from a mechanism that stopped working for every struct.
	FInstancedStruct ByteDecoded;
	USerializationFunctionLibrary::DeserializeActorState(
		BuildFullPayload(1, 0, 255, WideFieldLeadingByte), ByteDecoded);
	Fixture.Subsystem->ApplyViewState(UUID, ByteDecoded);

	Pose = Fixture.GetPose(Entity);
	if (!TestNotNull(TEXT("pose fragment still present"), Pose))
	{
		return false;
	}
	TestEqual(TEXT("the same byte value carried by a byte-typed field of the same name does land"),
		Pose->PoseId, WideFieldLeadingByte);

	return true;
}

// An entity that carries nothing for a registered field to land on still takes the update, and the
// field is dropped rather than landing somewhere else or bringing the fragment into being.
//
// The archetype assertion is the point rather than a formality. An ingestor that added its target
// fragment on demand would move the entity to another archetype mid-update, which leaves every later
// field of the same update writing through a pointer into the archetype the entity has just left.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateWithoutTargetFragmentTest,
	"CrowdySDK.Mass.ViewStateWithoutTargetFragment", CrowdyMassViewStateTestFlags)
bool FCrowdyMassViewStateWithoutTargetFragmentTest::RunTest(const FString& Parameters)
{
	RegisterViewStateTestWireStructs();
	FCrowdyMassViewStateTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakePoselessEntity(UUID);

	if (!TestNull(TEXT("this entity genuinely has no pose fragment, which is the whole case"),
		Fixture.GetPose(Entity)))
	{
		return false;
	}

	Fixture.PoisonEntity(Entity);

	const FMassArchetypeHandle ArchetypeBefore = Fixture.EntityManager->GetArchetypeForEntity(Entity);

	const FCrowdyMassTraceCollisionFragment* TraceCollisionPtr = Fixture.GetTraceCollision(Entity);
	if (!TestNotNull(TEXT("trace collision fragment present"), TraceCollisionPtr))
	{
		return false;
	}
	const FCrowdyMassTraceCollisionFragment TraceCollisionBefore = *TraceCollisionPtr;

	FInstancedStruct Decoded;
	USerializationFunctionLibrary::DeserializeActorState(BuildFullPayload(1, 0, 255, 3), Decoded);

	TestTrue(TEXT("the update is accepted for a tracked entity even though one field has nowhere to land"),
		Fixture.Subsystem->ApplyViewState(UUID, Decoded));

	TestNull(TEXT("the dropped field did not bring its fragment into being"), Fixture.GetPose(Entity));
	TestTrue(TEXT("and the entity did not move archetype"),
		Fixture.EntityManager->GetArchetypeForEntity(Entity) == ArchetypeBefore);

	FCrowdyMassAnimStateFragment* AnimState = Fixture.GetAnimState(Entity);
	if (TestNotNull(TEXT("anim state fragment present"), AnimState))
	{
		TestEqual(TEXT("the dropped field landed on no other fragment"),
			AnimState->SequenceIndex, PoisonedSequenceIndex);
	}

	FCrowdyMassActionFragment* Action = Fixture.GetAction(Entity);
	if (TestNotNull(TEXT("action fragment present"), Action))
	{
		TestEqual(TEXT("nor on the action fragment"), Action->ActionId, PoisonedActionId);
	}

	const FCrowdyMassTraceCollisionFragment* TraceCollisionAfter = Fixture.GetTraceCollision(Entity);
	if (TestNotNull(TEXT("trace collision fragment still resolvable"), TraceCollisionAfter))
	{
		TestTrue(TEXT("nor on trace collision"),
			FMemory::Memcmp(&TraceCollisionBefore, TraceCollisionAfter, sizeof(FCrowdyMassTraceCollisionFragment)) == 0);
	}

	TestTrue(TEXT("and the entity is still valid"), Fixture.EntityManager->IsEntityValid(Entity));

	return true;
}

// Drives one entity through a run of updates. A registered field is last-writer-wins in both
// directions: an ingestor that only ever sets a value and never writes the resting one back would
// leave the entity stuck on whatever the last non-resting update said, and no single update can show
// that. Across the whole run the entity keeps its identity and its archetype, which is the standing
// constraint on every ingestor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateSuccessiveEchoesTest,
	"CrowdySDK.Mass.ViewStateSuccessiveEchoes", CrowdyMassViewStateTestFlags)
bool FCrowdyMassViewStateSuccessiveEchoesTest::RunTest(const FString& Parameters)
{
	RegisterViewStateTestWireStructs();
	FCrowdyMassViewStateTestFixture Fixture;
	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity(UUID);

	const FMassArchetypeHandle ArchetypeBefore = Fixture.EntityManager->GetArchetypeForEntity(Entity);

	// Authored literals rather than a generated sequence: the run has to repeat one value, return to
	// the resting value, and leave the resting value again, in that order.
	const uint8 EchoedPoseIds[] = {3, 3, 0, 250, 0};
	const int32 EchoCount = static_cast<int32>(UE_ARRAY_COUNT(EchoedPoseIds));

	for (int32 Index = 0; Index < EchoCount; ++Index)
	{
		FInstancedStruct Decoded;
		USerializationFunctionLibrary::DeserializeActorState(BuildFullPayload(1, 0, 255, EchoedPoseIds[Index]), Decoded);
		Fixture.Subsystem->ApplyViewState(UUID, Decoded);

		FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
		if (!TestNotNull(TEXT("pose fragment present"), Pose))
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("update %d lands the value it carried"), Index),
			Pose->PoseId, EchoedPoseIds[Index]);
	}

	TestTrue(TEXT("the run of updates did not destroy the entity"), Fixture.EntityManager->IsEntityValid(Entity));
	TestTrue(TEXT("nor move it to another archetype"),
		Fixture.EntityManager->GetArchetypeForEntity(Entity) == ArchetypeBefore);

	FMassEntityHandle StillTracked;
	TestTrue(TEXT("the tracker still resolves the entity's UUID"),
		Fixture.Subsystem->FindEntityByUUID(UUID, StillTracked));
	TestTrue(TEXT("and it is still the same entity, not a re-minted one"), StillTracked == Entity);

	return true;
}

// One received payload costs ONE UUID-to-handle resolution, not one per half of it. This runs for every
// entity on every update, which is the path the population ceiling is bound by, so the count is the
// assertion rather than a timing. Calling ApplyViewState and UpdateRemoteEntityTransform in turn, which is
// what ExtractUpdate used to do, makes this two.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateUpdateResolvesOnceTest,
	"CrowdySDK.Mass.ViewStateUpdateResolvesTheEntityOnce", CrowdyMassViewStateTestFlags)
bool FCrowdyMassViewStateUpdateResolvesOnceTest::RunTest(const FString& Parameters)
{
	FCrowdyMassViewStateTestFixture Fixture;

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeTransformTrackedEntity(UUID);

	// A FULL payload, so the transform half is genuinely reached. A frame carrying no transform would cost
	// one lookup either way and would prove nothing.
	FInstancedStruct Decoded;
	if (!TestTrue(TEXT("the full payload decodes"),
		USerializationFunctionLibrary::DeserializeActorState(BuildFullPayload(1, 0, 200, 7), Decoded)))
	{
		return false;
	}

	FVector Location;
	FQuat Rotation;
	float SmoothingDuration = -1.0f;
	TestTrue(TEXT("and it carries a transform, so both halves of the update run"),
		UCrowdyMassEntitySubsystem::TryExtractTransformState(Decoded, Location, Rotation, SmoothingDuration));

	// The real seam. ExtractUpdate is the function that used to make two calls, so measuring the door below
	// it would leave the change itself ungated.
	const TStrongObjectPtr<UCrowdyMassRenderingBackend> Backend(
		NewObject<UCrowdyMassRenderingBackend>(GetTransientPackage()));
	Backend->BindSlotForTest(0, UUID, Fixture.Subsystem);

	Fixture.Subsystem->ResetEntityLookupCountForTest();
	Backend->ExtractUpdate(Decoded, 1234, 0);

	TestEqual(TEXT("one received payload resolves the entity exactly once for both halves"),
		Fixture.Subsystem->GetEntityLookupCountForTest(), 1);

	FCrowdyMassViewPoseFragment* Pose = Fixture.GetPose(Entity);
	if (TestNotNull(TEXT("pose fragment present"), Pose))
	{
		TestEqual(TEXT("and the view-state field landed"), Pose->PoseId, static_cast<uint8>(7));
	}

	// The transform half actually did something, which is what stops the count above being one because the
	// second half quietly returned at a missing-fragment guard.
	FCrowdyRemoteInterpolationFragment* Interpolation =
		Fixture.EntityManager->GetFragmentDataPtr<FCrowdyRemoteInterpolationFragment>(Entity);
	if (TestNotNull(TEXT("interpolation fragment present"), Interpolation))
	{
		TestEqual(TEXT("and the transform half wrote the timestamp it was handed"),
			Interpolation->ServerTimestampMs, static_cast<int64>(1234));
	}

	// An entity nothing tracks costs one lookup and no more: the miss must not be retried by a second half.
	Fixture.Subsystem->ResetEntityLookupCountForTest();
	TestFalse(TEXT("an untracked entity refuses the update"),
		Fixture.Subsystem->ApplyRemoteEntityUpdate(FGuid::NewGuid(), Decoded, 1234));
	TestEqual(TEXT("and cost one lookup, not two"),
		Fixture.Subsystem->GetEntityLookupCountForTest(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
