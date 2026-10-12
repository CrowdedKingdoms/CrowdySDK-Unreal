#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "MassEntityManager.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

// Covers the fragment memo in FCrowdyMassViewStateContext, which every registered view-state field
// resolves its target through, so that several fields landing on one fragment cost one archetype and
// chunk lookup between them instead of one each.
//
// A memo can only be seen from outside by making the underlying answer change and observing that the
// remembered one is handed back regardless. These tests do that by moving the entity between
// archetypes mid-context, which is exactly what an ingestor is forbidden to do and is the reason the
// prohibition exists: a stale remembered pointer is what an ingestor would leave every later field of
// the same update writing through. A pointer memoised before such a move is compared here and never
// dereferenced.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassViewStateContextTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Values no code under test writes, so a fragment identified by one of them cannot be a fragment
	// something else happened to leave at a default.
	constexpr uint8 MarkedPoseId = 11;
	constexpr int32 MarkedSequenceIndex = 4201;
	constexpr int32 MarkedActionId = 4202;
	constexpr int64 MarkedServerTimestampMs = 4205;

	// A standalone Mass entity manager with no World, mirroring the fixture in
	// Mass/CrowdyMassAvatarEventTests.cpp: PostInitialize constructs relation observers outered to
	// whatever owner the manager was given, and an owner-less manager takes the process down with
	// "Object is not packaged".
	struct FCrowdyMassViewStateContextTestFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;

		FCrowdyMassViewStateContextTestFixture()
		{
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();
		}

		~FCrowdyMassViewStateContextTestFixture()
		{
			EntityManager->Deinitialize();
		}

		FMassEntityHandle MakeEntity(TConstArrayView<const UScriptStruct*> Fragments)
		{
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(Fragments);
			return EntityManager->CreateEntity(Archetype);
		}
	};
}

// The same fragment type asked for twice hands back the answer found the first time, rather than
// resolving again. The pointer alone cannot show that (two identical answers look the same either
// way), so the entity loses the fragment between the two asks: a second resolution would now find
// nothing, and only a remembered answer can still be non-null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateContextMemoisesHitTest,
	"CrowdySDK.Mass.ViewStateContextMemoisesHit", CrowdyMassViewStateContextTestFlags)
bool FCrowdyMassViewStateContextMemoisesHitTest::RunTest(const FString& Parameters)
{
	FCrowdyMassViewStateContextTestFixture Fixture;

	const UScriptStruct* Fragments[] = {
		FCrowdyMassViewPoseFragment::StaticStruct(), FCrowdyMassTraceCollisionFragment::StaticStruct()};
	const FMassEntityHandle Entity = Fixture.MakeEntity(MakeArrayView(Fragments, 2));

	FCrowdyMassViewStateContext Context{*Fixture.EntityManager, Entity};

	FCrowdyMassViewPoseFragment* First = Context.GetFragment<FCrowdyMassViewPoseFragment>();
	if (!TestNotNull(TEXT("a fragment the entity carries resolves"), First))
	{
		return false;
	}

	FCrowdyMassViewPoseFragment* Second = Context.GetFragment<FCrowdyMassViewPoseFragment>();
	TestTrue(TEXT("asking twice hands back the same pointer"), Second == First);

	// Compared against the manager's own answer rather than only against itself, so the memo is being
	// held to an independent source before that source is taken away below.
	TestTrue(TEXT("and that pointer is the one the entity manager resolves"),
		First == Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Entity));

	Fixture.EntityManager->RemoveFragmentFromEntity(Entity, FCrowdyMassViewPoseFragment::StaticStruct());

	TestNull(TEXT("the entity no longer carries the fragment, so a fresh resolution would find nothing"),
		Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Entity));

	FCrowdyMassViewPoseFragment* AfterRemoval = Context.GetFragment<FCrowdyMassViewPoseFragment>();
	TestTrue(TEXT("the context still hands back the remembered pointer, so it did not resolve again"),
		AfterRemoval == First);

	return true;
}

// A miss is remembered exactly like a hit. An entity carrying no such fragment is ordinary, and
// re-resolving a null for every field that targets it is the same waste the memo exists to remove.
// The entity gains the fragment between the two asks, so a second resolution would now find one and
// only a remembered miss can still be null.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateContextMemoisesMissTest,
	"CrowdySDK.Mass.ViewStateContextMemoisesMiss", CrowdyMassViewStateContextTestFlags)
bool FCrowdyMassViewStateContextMemoisesMissTest::RunTest(const FString& Parameters)
{
	FCrowdyMassViewStateContextTestFixture Fixture;

	const UScriptStruct* Fragments[] = {FCrowdyMassViewPoseFragment::StaticStruct()};
	const FMassEntityHandle Entity = Fixture.MakeEntity(MakeArrayView(Fragments, 1));

	FCrowdyMassViewStateContext Context{*Fixture.EntityManager, Entity};

	TestNull(TEXT("a fragment the entity does not carry resolves to nothing"),
		Context.GetFragment<FCrowdyMassActionFragment>());
	TestNull(TEXT("and asking again is still nothing"),
		Context.GetFragment<FCrowdyMassActionFragment>());

	Fixture.EntityManager->AddFragmentToEntity(Entity, FCrowdyMassActionFragment::StaticStruct());

	// Without this, the assertion below would be satisfied just as well by an add that never happened,
	// which is the whole gate: the underlying answer has to have genuinely changed.
	TestNotNull(TEXT("the entity now carries the fragment, so a fresh resolution would find one"),
		Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassActionFragment>(Entity));

	TestNull(TEXT("the context still hands back the remembered miss, so it did not resolve again"),
		Context.GetFragment<FCrowdyMassActionFragment>());

	return true;
}

// The memo is a fixed-size inline table, and an update whose fields target more distinct fragment
// types than it holds is allowed to pay the lookup again for the extra ones. What it may never do is
// answer with the wrong fragment or with nothing: a field silently landing on another field's
// fragment is the failure that would be invisible in everything except the value it corrupts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassViewStateContextPastCapacityTest,
	"CrowdySDK.Mass.ViewStateContextPastCapacity", CrowdyMassViewStateContextTestFlags)
bool FCrowdyMassViewStateContextPastCapacityTest::RunTest(const FString& Parameters)
{
	FCrowdyMassViewStateContextTestFixture Fixture;

	// Five distinct fragment types on one entity, one more than the memo holds.
	const UScriptStruct* Fragments[] = {
		FCrowdyMassViewPoseFragment::StaticStruct(), FCrowdyMassAnimStateFragment::StaticStruct(),
		FCrowdyMassActionFragment::StaticStruct(), FCrowdyMassTraceCollisionFragment::StaticStruct(),
		FCrowdyRemoteInterpolationFragment::StaticStruct()};
	const FMassEntityHandle Entity = Fixture.MakeEntity(MakeArrayView(Fragments, 5));

	FCrowdyMassViewPoseFragment* Pose = Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Entity);
	FCrowdyMassAnimStateFragment* AnimState = Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassAnimStateFragment>(Entity);
	FCrowdyMassActionFragment* Action = Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassActionFragment>(Entity);
	FCrowdyMassTraceCollisionFragment* TraceCollision = Fixture.EntityManager->GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>(Entity);
	FCrowdyRemoteInterpolationFragment* Interpolation = Fixture.EntityManager->GetFragmentDataPtr<FCrowdyRemoteInterpolationFragment>(Entity);
	if (!TestNotNull(TEXT("pose fragment present"), Pose) || !TestNotNull(TEXT("anim state fragment present"), AnimState)
		|| !TestNotNull(TEXT("action fragment present"), Action)
		|| !TestNotNull(TEXT("trace collision fragment present"), TraceCollision)
		|| !TestNotNull(TEXT("interpolation fragment present"), Interpolation))
	{
		return false;
	}

	// Marked so that a wrong answer is caught by the value it carries, not only by its address. An
	// address comparison alone cannot say which fragment a pointer names once several are live.
	Pose->PoseId = MarkedPoseId;
	AnimState->SequenceIndex = MarkedSequenceIndex;
	Action->ActionId = MarkedActionId;
	TraceCollision->bEnableTraceCollision = false;
	Interpolation->ServerTimestampMs = MarkedServerTimestampMs;

	FCrowdyMassViewStateContext Context{*Fixture.EntityManager, Entity};

	// The first four fill the table.
	TestTrue(TEXT("the first type resolves to its own fragment"),
		Context.GetFragment<FCrowdyMassViewPoseFragment>() == Pose);
	TestTrue(TEXT("the second type resolves to its own fragment"),
		Context.GetFragment<FCrowdyMassAnimStateFragment>() == AnimState);
	TestTrue(TEXT("the third type resolves to its own fragment"),
		Context.GetFragment<FCrowdyMassActionFragment>() == Action);
	TestTrue(TEXT("the fourth type resolves to its own fragment"),
		Context.GetFragment<FCrowdyMassTraceCollisionFragment>() == TraceCollision);

	FCrowdyRemoteInterpolationFragment* Fifth = Context.GetFragment<FCrowdyRemoteInterpolationFragment>();
	if (!TestNotNull(TEXT("a fifth distinct type still resolves to something"), Fifth))
	{
		return false;
	}
	TestTrue(TEXT("and it is that type's own fragment"), Fifth == Interpolation);
	TestEqual(TEXT("carrying the value written to that fragment and no other"),
		Fifth->ServerTimestampMs, MarkedServerTimestampMs);

	// Asking again is where re-resolution happens, if it happens at all, so the answer has to survive
	// the repeat.
	FCrowdyRemoteInterpolationFragment* FifthAgain = Context.GetFragment<FCrowdyRemoteInterpolationFragment>();
	TestTrue(TEXT("asking the fifth type again still answers with its own fragment"), FifthAgain == Interpolation);

	// The overflow must not have displaced or corrupted what the table already held.
	TestTrue(TEXT("the first type still answers with its own fragment after the overflow"),
		Context.GetFragment<FCrowdyMassViewPoseFragment>() == Pose);
	TestEqual(TEXT("carrying the value written to it"),
		Context.GetFragment<FCrowdyMassViewPoseFragment>()->PoseId, MarkedPoseId);
	TestTrue(TEXT("and the fourth still answers with its own fragment"),
		Context.GetFragment<FCrowdyMassTraceCollisionFragment>() == TraceCollision);
	TestFalse(TEXT("carrying the value written to it"),
		Context.GetFragment<FCrowdyMassTraceCollisionFragment>()->bEnableTraceCollision);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
