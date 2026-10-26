#include "Components/CrowdyMassNearTierComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/CrowdyMassNearTierActor.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "MassCommandBuffer.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "Misc/AutomationTest.h"
#include "Representation/CrowdyMassRepresentationActorManagement.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// What the near tier's view and its pooled shell can be asked with no world.
//
// Everything the view does per frame reads the Mass entity manager off a world subsystem that only exists
// in a PIE or Game world, so binding to an entity, following its transform, pushing its trace collision and
// starting its action are all out of reach here and are covered by the PIE harness instead. What IS
// reachable is the shape both classes are built with, the pooled shell's handover, the one conversion from
// an entity transform to a root transform, what promoting and demoting an actor does to it, and the
// playback convention the view shares with the crowd.
static constexpr EAutomationTestFlags CrowdyMassNearTierViewTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassNearTierViewTestFixture
{
	// A shell built but never spawned. Its native subobjects are the ones the constructor made, which is
	// what every question below is about; nothing here needs it placed in a world.
	static ACrowdyMassNearTierActor* MakeShell()
	{
		return NewObject<ACrowdyMassNearTierActor>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	/**
	 * An asset for a mesh component to point at.
	 *
	 * Nothing here draws it and nothing asks it anything: what is being asked is only whether a component
	 * has resolved an asset, so an empty one answers that as well as a built one and costs no build.
	 */
	static USkeletalMesh* MakeStandInAsset(UObject* Outer)
	{
		return NewObject<USkeletalMesh>(Outer, NAME_None, RF_Transient);
	}

	/**
	 * Places an unspawned actor's root without moving it.
	 *
	 * The root has no attach parent, so its relative transform IS its world transform, and writing the
	 * fields and then recomputing avoids a move, a sweep and an overlap update on a component that was never
	 * registered. What is being set up is only the answer AActor::GetTransform gives.
	 */
	static void PlaceUnspawnedActor(AActor& Actor, const FTransform& Transform)
	{
		USceneComponent* Root = Actor.GetRootComponent();
		if (!Root)
		{
			return;
		}

		Root->SetRelativeTransform_Direct(Transform);
		Root->UpdateComponentToWorld();
	}
};

/**
 * The view ticks after physics, and that is a correctness requirement rather than an ordering preference.
 *
 * Every processor that writes the fragments it reads runs in the PrePhysics Mass phase and one of them is
 * allowed off the game thread, so reading them from a PrePhysics tick is a data race: the values would be
 * torn rather than merely one frame old, and it would present as an occasional actor snapping to somewhere
 * it never was.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierViewTicksAfterPhysicsTest,
	"CrowdySDK.Mass.NearTierViewTicksAfterPhysics", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierViewTicksAfterPhysicsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FCrowdyMassNearTierViewTestFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view to ask"), View))
	{
		return false;
	}

	TestTrue(TEXT("the view ticks after physics, where the Mass writers have finished"),
		View->PrimaryComponentTick.TickGroup == TG_PostPhysics);
	TestTrue(TEXT("and it can tick at all, since nothing else moves a promoted actor"),
		View->PrimaryComponentTick.bCanEverTick);
	TestTrue(TEXT("and is willing to from the start, so an owner that never switches it still follows its entity"),
		View->PrimaryComponentTick.bStartWithTickEnabled);

	return true;
}

/**
 * What an unbound view reports.
 *
 * A gait or pose of 0 means no gait and no pose were resolved, which is a real answer that has to be passed
 * on as it stands: a graph handed 1 instead would show the first authored row for an entity whose animation
 * set resolved nothing, which reads as a working crowd standing oddly still rather than as the
 * configuration fault it is. The play rate is the one input that defaults to something rather than nothing,
 * because a rate of 0 freezes a clip.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierUnboundViewReportsHonestZerosTest,
	"CrowdySDK.Mass.NearTierUnboundViewReportsHonestZeros", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierUnboundViewReportsHonestZerosTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FCrowdyMassNearTierViewTestFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view to ask"), View))
	{
		return false;
	}

	TestFalse(TEXT("a view nobody has bound stands for no entity"), View->IsBoundToEntity());
	TestFalse(TEXT("and names none"), View->GetEntityUUID().IsValid());

	TestEqual(TEXT("no gait is reported as no gait, not as the first authored one"), View->GetGaitId(), 0);
	TestEqual(TEXT("and carries no name"), View->GetGaitName(), FName(NAME_None));
	TestEqual(TEXT("no pose is reported as no pose"), View->GetPoseId(), 0);
	TestEqual(TEXT("and carries no name"), View->GetPoseName(), FName(NAME_None));
	TestFalse(TEXT("so nothing claims a pose is held"), View->HasPose());

	TestEqual(TEXT("no action is active"), View->GetActionId(), 0);
	TestEqual(TEXT("with no variant"), View->GetActionVariant(), 0);
	TestEqual(TEXT("the play rate defaults to the ordinary one rather than to zero, which would freeze a clip"),
		View->GetActionPlayRate(), 1.0f);
	TestEqual(TEXT("and no time has elapsed into one"), View->GetActionElapsedSeconds(), 0.0f);

	TestEqual(TEXT("no speed"), View->GetSmoothedSpeed(), 0.0f);
	TestEqual(TEXT("no world velocity"), View->GetWorldVelocity(), FVector::ZeroVector);
	TestEqual(TEXT("no local velocity"), View->GetLocalVelocity(), FVector::ZeroVector);

	TestTrue(TEXT("actions are playable until something is found to refuse them"), View->AreActionsPlayable());
	TestNull(TEXT("and no animation set is resolved for an entity there is none of"), View->GetResolvedAnimSet());

	// Unbinding something that was never bound leaves it in the same state rather than half-clearing it.
	View->UnbindFromEntity();
	TestFalse(TEXT("unbinding an unbound view is a no-op"), View->IsBoundToEntity());
	TestEqual(TEXT("and leaves the honest zeros alone"), View->GetGaitId(), 0);

	return true;
}

/**
 * The shape the pooled shell is built with.
 *
 * The mesh hangs under a bare pivot rather than being the pivot itself, because a root component's own
 * location and rotation cannot be edited and a skeletal mesh nearly always needs both. The entity's
 * transform is written onto the pivot, so a mesh left at its own origin stands where the entity stands and
 * a zero entity offset is right for this class. The
 * ACTOR starts hidden because it is spawned some frames before the promotion decides to show it, and
 * anything visible in the meantime is a second copy of a character the crowd is still drawing as a row.
 *
 * Hidden at the actor and not by switching each component off: actor hiding composes with the visibility a
 * subclass authored instead of replacing it, and these actors are pooled rather than rebuilt, so a
 * visibility overwritten once stays overwritten for every later occupant of that actor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierShellShapeTest,
	"CrowdySDK.Mass.NearTierShellShape", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierShellShapeTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FCrowdyMassNearTierViewTestFixture::MakeShell());

	if (!TestNotNull(TEXT("the shell carries a mesh to draw with"), Shell->Mesh.Get()))
	{
		return false;
	}
	TestNotNull(TEXT("and a near tier view to drive it"), Shell->NearTier.Get());
	// The mesh hangs under a bare pivot rather than being the pivot. A root component's own location and
	// rotation cannot be edited, so a mesh used as the root could never be turned to face the way its
	// entity faces, and the standard skeleton needs exactly that.
	TestSamePtr(TEXT("the root is the bare pivot the entity's transform is written onto"),
		Shell->GetRootComponent(), static_cast<USceneComponent*>(Shell->Root.Get()));
	TestNotSamePtr(TEXT("so the mesh is not the root, and its own placement stays authorable"),
		Shell->GetRootComponent(), static_cast<USceneComponent*>(Shell->Mesh.Get()));
	TestSamePtr(TEXT("and the mesh hangs beneath that pivot, so placing the pivot places the mesh"),
		Shell->Mesh->GetAttachParent(), static_cast<USceneComponent*>(Shell->Root.Get()));

	TestTrue(TEXT("the actor starts hidden, so a spawned but not yet shown shell cannot double-render against the row it is replacing"),
		Shell->IsHidden());
	TestTrue(TEXT("and the mesh's own visibility is left as authored, which is what actor hiding composes with"),
		Shell->Mesh->GetVisibleFlag());

	TestEqual(TEXT("a shell whose mesh stands at its pivot needs no entity offset"),
		Shell->GetEntityTransformOffset(), FVector::ZeroVector);
	TestTrue(TEXT("so its conversion of an entity transform leaves that transform alone"),
		Shell->MakeRootTransformFromEntity(FTransform(FRotator(0.0, 30.0, 0.0), FVector(10.0, 20.0, 30.0)))
			.Equals(FTransform(FRotator(0.0, 30.0, 0.0), FVector(10.0, 20.0, 30.0))));

	TestFalse(TEXT("the actor itself does not tick; a picture has nothing to do per frame and there can be a great many"),
		Shell->PrimaryActorTick.bCanEverTick);

	TestTrue(TEXT("the shell can be pooled, without which every promotion is a real spawn and a real destroy"),
		Shell->CanBePooled_Implementation());

	return true;
}

/**
 * The pool handover, from the shell's side.
 *
 * Nothing in the engine's actor pool says whether an appearance was built or reused, so the shell records
 * it and hands it over exactly once: read twice, the second answer would report a spawn that never
 * happened. Both halves of the handover also re-hide the actor, because the engine unhides a pooled actor
 * just before handing it back and because a Blueprint subclass edits these defaults and can turn it on.
 *
 * The release also takes the collision off. An entity destroyed while it is promoted goes straight to the
 * pool without the demotion step that disables it, so without this the shell sits in the pool, hidden, at
 * the dead entity's last position, blocking every trace and camera sweep aimed through a point where
 * nothing is drawn, until the pool happens to hand it out again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierShellPoolHandoverTest,
	"CrowdySDK.Mass.NearTierShellPoolHandover", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierShellPoolHandoverTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FCrowdyMassNearTierViewTestFixture::MakeShell());
	if (!TestNotNull(TEXT("the shell carries a mesh"), Shell->Mesh.Get()))
	{
		return false;
	}

	TestFalse(TEXT("a shell that was just built did not come from the pool"), Shell->ConsumeCameFromPool());

	Shell->PrepareForGame_Implementation();
	TestTrue(TEXT("a shell handed out by the pool says so"), Shell->ConsumeCameFromPool());
	TestFalse(TEXT("and says so once, so one appearance cannot be reported as two"), Shell->ConsumeCameFromPool());

	// Shown first, so what is asserted afterwards is that the handover hid it rather than that it was never
	// shown. This is also what the engine itself does: it unhides a pooled actor immediately before handing
	// it back, which is why the handover has to hide it again.
	Shell->SetActorHiddenInGame(false);
	if (!TestFalse(TEXT("the fixture really did show the actor"), Shell->IsHidden()))
	{
		return false;
	}
	Shell->PrepareForGame_Implementation();
	TestTrue(TEXT("a shell taken out of the pool comes back hidden"), Shell->IsHidden());
	TestTrue(TEXT("and the mesh's authored visibility was not overwritten to do it"), Shell->Mesh->GetVisibleFlag());

	Shell->SetActorHiddenInGame(false);
	Shell->SetActorEnableCollision(true);

	Shell->PrepareForPooling_Implementation();
	TestTrue(TEXT("and is hidden again on the way back in"), Shell->IsHidden());
	TestFalse(TEXT("a shell released to the pool stops colliding, so it cannot block traces through a point where nothing is drawn"),
		Shell->GetActorEnableCollision());
	TestFalse(TEXT("a shell going back to the pool stands for nothing"), Shell->NearTier->IsBoundToEntity());

	return true;
}

/**
 * One conversion from an entity transform to a root transform, used by everything that places the actor.
 *
 * A shell's root and its entity's transform need not sit at the same point: an entity is placed where it
 * stands, while a shell may be rooted at the middle of a body or at a socket. Three sites need that
 * conversion, the spawn, the teleport and the per-frame follow, and each of them applying the offset by
 * hand is how they come to disagree about its sign or its frame. A disagreement between the placement and
 * the follow shows up as the actor jumping on its first tick after appearing and then staying where the
 * follow put it; a disagreement about the frame swings the actor around its entity as that entity turns.
 * The picture never says which of the two it was.
 *
 * The offset used here leans to one side as well as upwards on purpose. A purely vertical offset is
 * unchanged by the yaw a crowd entity turns through, so it gives the same answer whether the offset is
 * rotated or not, and the shipped default of zero gives the same answer for every convention there is:
 * those are exactly the values that hide this.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierEntityOffsetRoundTripTest,
	"CrowdySDK.Mass.NearTierEntityOffsetRoundTrip", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierEntityOffsetRoundTripTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());

	const FVector Offset(20.0, -8.0, 88.0);
	Shell->EntityTransformOffset = Offset;
	if (!TestEqual(TEXT("the shell reports the offset it was given"), Shell->GetEntityTransformOffset(), Offset))
	{
		return false;
	}

	// Facing somewhere other than along an axis, which is the ordinary state of a crowd entity and the only
	// state in which the frame the offset is measured in makes any difference.
	const FQuat EntityRotation = FRotator(0.0, 55.0, 0.0).Quaternion();
	const FVector EntityLocation(400.0, -120.0, 15.0);
	const FTransform EntityTransform(EntityRotation, EntityLocation);

	const FTransform RootTransform = Shell->MakeRootTransformFromEntity(EntityTransform);
	const FVector RotatedOffset = EntityRotation.RotateVector(Offset);

	TestTrue(TEXT("the root stands at the entity's location plus the offset measured in the entity's own space"),
		RootTransform.GetLocation().Equals(EntityLocation + RotatedOffset));
	TestTrue(TEXT("facing the way the entity faces"), RootTransform.GetRotation().Equals(EntityRotation));
	TestTrue(TEXT("at the entity's scale, since an offset is not a resize"),
		RootTransform.GetScale3D().Equals(EntityTransform.GetScale3D()));

	// The three ways of getting it wrong, each of which produces a perfectly plausible picture.
	TestFalse(TEXT("the offset is not added in world space, which would swing the actor around its entity as it turns"),
		RootTransform.GetLocation().Equals(EntityLocation + Offset));
	TestFalse(TEXT("nor subtracted, which puts the actor as far the wrong side as it should have been the right one"),
		RootTransform.GetLocation().Equals(EntityLocation - RotatedOffset));
	TestFalse(TEXT("nor dropped, which leaves a shell rooted at its middle sunk into the ground"),
		RootTransform.GetLocation().Equals(EntityLocation));

	// The promotion's own placement, driven through the class that does it rather than restated here. The
	// engine only asks for a move when the actor is not already where it belongs, so an actor standing at
	// the shell's converted transform gives it nothing to do, and one standing anywhere else does not.
	const UCrowdyMassRepresentationActorManagement* Management =
		GetDefault<UCrowdyMassRepresentationActorManagement>();

	FFixture::PlaceUnspawnedActor(*Shell, RootTransform);
	FMassCommandBuffer AlreadyAtRoot;
	Management->TeleportActor(EntityTransform, *Shell, AlreadyAtRoot);
	TestFalse(TEXT("the placement puts a shell at exactly what the shell's own conversion produces, so an actor already there is left alone"),
		AlreadyAtRoot.HasPendingCommands());

	FFixture::PlaceUnspawnedActor(*Shell, EntityTransform);
	FMassCommandBuffer AtEntityTransform;
	Management->TeleportActor(EntityTransform, *Shell, AtEntityTransform);
	TestTrue(TEXT("and not at the entity's own transform, which for a shell with an offset is somewhere else"),
		AtEntityTransform.HasPendingCommands());
	AtEntityTransform.CancelCommands();

	// The shipped default at the same site: no offset means the conversion is the identity and the actor
	// belongs at the entity's own transform, which is why every disagreement above is invisible until a real
	// shell sets an offset.
	const TStrongObjectPtr<ACrowdyMassNearTierActor> UnoffsetShell(FFixture::MakeShell());
	FFixture::PlaceUnspawnedActor(*UnoffsetShell, EntityTransform);
	FMassCommandBuffer UnoffsetBuffer;
	Management->TeleportActor(EntityTransform, *UnoffsetShell, UnoffsetBuffer);
	TestFalse(TEXT("a shell with no offset belongs at the entity's transform exactly"),
		UnoffsetBuffer.HasPendingCommands());

	return true;
}

/**
 * What promoting and demoting an actor does to it.
 *
 * Demoting is the half that has to be complete. The entity goes back to being one row of the crowd, and
 * anything the actor is still doing is a second copy of that character standing somewhere else: drawn, it
 * is visibly two of them; ticking its view, it goes on following an entity nobody is drawing it for.
 *
 * Showing is done at the ACTOR and not by writing a visibility onto each component, because these actors
 * are pooled rather than rebuilt. Writing true onto a part its author deliberately left hidden reveals it
 * for that occupant and for every later occupant of the same actor for the rest of the session, and the
 * only evidence is a character wearing something it should not be.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierPromotionShowsAndDrivesTheActorTest,
	"CrowdySDK.Mass.NearTierPromotionShowsAndDrivesTheActor", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierPromotionShowsAndDrivesTheActorTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FCrowdyMassNearTierViewTestFixture::MakeShell());
	if (!TestNotNull(TEXT("the shell carries a mesh and a view to switch"), Shell->NearTier.Get()))
	{
		return false;
	}

	// The inherited part a subclass turned off, having added a mesh of its own. Switched off here so that
	// what is asserted after the promotion is that it was left alone, not that it was never on.
	Shell->Mesh->SetVisibility(false);

	// Both ticks switched off first, which is the state an actor sitting in the pool is in, so that the
	// promotion below is what turns them on rather than them having been on all along.
	Shell->NearTier->SetComponentTickEnabled(false);
	Shell->Mesh->SetComponentTickEnabled(false);
	if (!TestFalse(TEXT("the fixture really did stop the view ticking"), Shell->NearTier->IsComponentTickEnabled())
		|| !TestFalse(TEXT("and the mesh"), Shell->Mesh->IsComponentTickEnabled()))
	{
		return false;
	}

	const UCrowdyMassRepresentationActorManagement* Management =
		GetDefault<UCrowdyMassRepresentationActorManagement>();

	FMassCommandBuffer PromoteCommands;
	Management->SetActorEnabled(EMassActorEnabledType::HighRes, *Shell, /*EntityIdx=*/0, PromoteCommands);

	TestFalse(TEXT("a promoted actor is shown, since the crowd has stopped drawing its row"), Shell->IsHidden());
	TestFalse(TEXT("and a part its author left hidden is still hidden, which is what showing the actor rather than each component preserves"),
		Shell->Mesh->GetVisibleFlag());
	TestTrue(TEXT("its view ticks, without which nothing moves it and it stands where it was born"),
		Shell->NearTier->IsComponentTickEnabled());
	TestTrue(TEXT("and its mesh ticks, without which the animation graph is never evaluated"),
		Shell->Mesh->IsComponentTickEnabled());
	PromoteCommands.CancelCommands();

	FMassCommandBuffer DemoteCommands;
	Management->SetActorEnabled(EMassActorEnabledType::Disabled, *Shell, /*EntityIdx=*/0, DemoteCommands);

	TestTrue(TEXT("a demoted actor is hidden, since its entity is being drawn as a crowd row again"),
		Shell->IsHidden());
	TestFalse(TEXT("its view stops ticking, so it cannot go on following an entity nobody is drawing it for"),
		Shell->NearTier->IsComponentTickEnabled());
	TestFalse(TEXT("and its mesh stops evaluating an animation graph for a character it is no longer drawing"),
		Shell->Mesh->IsComponentTickEnabled());
	DemoteCommands.CancelCommands();

	return true;
}

/**
 * Whether an actor has anything to draw is asked of the actor, not of one inherited pointer.
 *
 * A shell is meant to be subclassed, and a subclass commonly adds its own mesh rather than filling in the
 * inherited one. Answering from a single pointer leaves such a subclass never ready, and never ready is not
 * a delay: the crowd stops drawing an entity's row the moment its promotion is decided, so the entity is
 * drawn by nothing at all until the engine gives up waiting and shows the actor anyway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierDrawableMeshIsSearchedForTest,
	"CrowdySDK.Mass.NearTierDrawableMeshIsSearchedFor", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierDrawableMeshIsSearchedForTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());

	TestFalse(TEXT("a shell whose mesh has resolved no asset has nothing to draw"),
		CrowdyMassNearTier::HasDrawableSkinnedMesh(*Shell));

	Shell->Mesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(Shell.Get()));
	TestTrue(TEXT("and has something to draw once it has"),
		CrowdyMassNearTier::HasDrawableSkinnedMesh(*Shell));

	// The subclass shape: the inherited mesh is left empty and a mesh of the subclass's own carries the
	// asset. This is the cell a single-pointer answer fails, and it fails it silently.
	const TStrongObjectPtr<ACrowdyMassNearTierActor> SubclassShapedShell(FFixture::MakeShell());
	USkeletalMeshComponent* OwnMesh =
		NewObject<USkeletalMeshComponent>(SubclassShapedShell.Get(), TEXT("SubclassMesh"));
	OwnMesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(SubclassShapedShell.Get()));

	if (!TestNull(TEXT("the inherited mesh really is empty, so the answer below cannot come from it"),
		SubclassShapedShell->Mesh->GetSkinnedAsset()))
	{
		return false;
	}
	TestTrue(TEXT("an actor whose own mesh carries the asset has something to draw"),
		CrowdyMassNearTier::HasDrawableSkinnedMesh(*SubclassShapedShell));

	// Having something to draw is only half of being ready. Until the actor knows which entity it stands
	// for, the first frame it is shown is a bind pose at whatever transform the spawn was asked for.
	TestFalse(TEXT("a shell with a mesh but no entity is not ready to stand in for one"),
		Shell->IsReadyForRepresentation());

	// And an actor that carries no near tier view at all is not this plugin's to answer for: a game may
	// promote its own classes through the engine's ordinary path, and refusing them would stall it forever.
	const UCrowdyMassRepresentationActorManagement* Management =
		GetDefault<UCrowdyMassRepresentationActorManagement>();
	const TStrongObjectPtr<AActor> PlainActor(NewObject<AActor>(GetTransientPackage(), NAME_None, RF_Transient));

	TestTrue(TEXT("an actor with no near tier view is left to answer for itself"),
		Management->IsActorReadyForRepresentation(*PlainActor));
	TestFalse(TEXT("while a shell that has not been bound is held back"),
		Management->IsActorReadyForRepresentation(*Shell));

	return true;
}

/**
 * Where in a clip a promoted actor starts an action that was already running.
 *
 * The crowd's renderer starts its playback clock when the pushed sequence index changes, never when the
 * action was triggered, so both representations seek forward by however long the action has already been
 * running and clamp that into the clip. The seek is deliberately NOT divided by the play rate: the crowd
 * seeks and expires on unscaled seconds in both directions, so dividing on one side would leave a promoted
 * actor at a different frame from the crowd behind it for every rate other than 1, which is exactly the
 * kind of change that looks like a correction.
 *
 * This pins the convention, not the near tier's call site: that call happens inside a per-frame method that
 * needs a world, a mesh and an animation instance, none of which exist here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierActionSeekIsRateNaiveTest,
	"CrowdySDK.Mass.NearTierActionSeekIsRateNaive", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierActionSeekIsRateNaiveTest::RunTest(const FString& Parameters)
{
	// An action written by an event and then left running for a while, as the action processor leaves it:
	// the duration filled in by the animation pass that pushed it, and the elapsed time still climbing.
	FCrowdyMassActionFragment Action;
	if (!TestTrue(TEXT("the fixture action was written"), CrowdyMassWriteAction(Action, 4, 0, 2.0f)))
	{
		return false;
	}
	Action.DurationSeconds = 2.0f;
	Action.ElapsedSecondsSinceTrigger = 0.75f;

	const float ClampedRate = CrowdyMassClampActionPlayRate(Action.PlayRate);
	const float Seek = GetCrowdyMassActionCorrectedPosition(Action.ElapsedSecondsSinceTrigger, Action.DurationSeconds);

	TestEqual(TEXT("the seek is the unscaled time the action has been running"), Seek, 0.75f);
	TestEqual(TEXT("the play rate reached the fragment clamped"), ClampedRate, 2.0f);
	TestNotEqual(TEXT("and the seek is not scaled by it, which at any rate other than 1 is a different frame"),
		Seek, Action.ElapsedSecondsSinceTrigger / ClampedRate);
	TestNotEqual(TEXT("nor scaled the other way"), Seek, Action.ElapsedSecondsSinceTrigger * ClampedRate);

	// Clamped into the clip. An action promoted so late that it should already be over starts at its last
	// frame, which the crowd's Clamp loop mode holds, rather than seeking past the end.
	Action.ElapsedSecondsSinceTrigger = 5.0f;
	TestEqual(TEXT("an action already over starts at its last frame"),
		GetCrowdyMassActionCorrectedPosition(Action.ElapsedSecondsSinceTrigger, Action.DurationSeconds), 2.0f);

	// A duration of zero is what a fragment carries while the animation pass has not resolved the clip yet.
	// There is nothing to seek into, so the only safe answer is the start.
	TestEqual(TEXT("an action with no resolved duration starts at the beginning rather than seeking blind"),
		GetCrowdyMassActionCorrectedPosition(Action.ElapsedSecondsSinceTrigger, 0.0f), 0.0f);

	// A forged rate off the wire is replaced rather than carried, and it never reaches the seek at all.
	FCrowdyMassActionFragment ForgedAction;
	CrowdyMassWriteAction(ForgedAction, 4, 0, FMath::Sqrt(-1.0f));
	ForgedAction.DurationSeconds = 2.0f;
	ForgedAction.ElapsedSecondsSinceTrigger = 0.75f;

	TestTrue(TEXT("a non-finite play rate does not survive into the fragment"), FMath::IsFinite(ForgedAction.PlayRate));
	TestEqual(TEXT("and the seek is the same as it was for an ordinary rate"),
		GetCrowdyMassActionCorrectedPosition(ForgedAction.ElapsedSecondsSinceTrigger, ForgedAction.DurationSeconds),
		0.75f);

	return true;
}

/**
 * An actor blends into an action it starts and never into one it joins partway through.
 *
 * A promoted actor's animation graph is in its default pose, so easing from that into a clip's first pose
 * is right for an action beginning now and wrong for one already half played: the character passes through
 * a pose it never held, which is visible as a flinch and only ever on promotion, since the other direction
 * hands an uninterrupted crowd row back.
 *
 * The boundary is the blend length rather than zero because an action is never zero seconds old by the time
 * a view reads it. The simulation advances the elapsed time first, so testing for an exact zero would take
 * the blend off every action instead of off the ones this is about.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierActionBlendSkipsAMidClipSeekTest,
	"CrowdySDK.Mass.NearTierActionBlendSkipsAMidClipSeek", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierActionBlendSkipsAMidClipSeekTest::RunTest(const FString& Parameters)
{
	const float AuthoredBlendSeconds = 0.1f;

	// An action triggered and read on the same frame, which is one simulation step old rather than new.
	FCrowdyMassActionFragment Action;
	if (!TestTrue(TEXT("the fixture action was written"), CrowdyMassWriteAction(Action, 4, 0, 1.0f)))
	{
		return false;
	}
	Action.DurationSeconds = 2.0f;
	Action.ElapsedSecondsSinceTrigger = 1.0f / 60.0f;

	const float FreshSeek = GetCrowdyMassActionCorrectedPosition(Action.ElapsedSecondsSinceTrigger,
		Action.DurationSeconds);
	TestEqual(TEXT("an action a frame old is still one starting at its beginning and keeps its blend"),
		CrowdyMassNearTier::ActionBlendInForSeek(AuthoredBlendSeconds, FreshSeek), AuthoredBlendSeconds);

	// The same action joined by an actor promoted while it was running.
	Action.ElapsedSecondsSinceTrigger = 0.75f;
	const float MidClipSeek = GetCrowdyMassActionCorrectedPosition(Action.ElapsedSecondsSinceTrigger,
		Action.DurationSeconds);
	TestEqual(TEXT("while one already under way is shown at its offset with no blend into it"),
		CrowdyMassNearTier::ActionBlendInForSeek(AuthoredBlendSeconds, MidClipSeek), 0.0f);

	// The boundary itself belongs to the blend: a seek the blend would have covered anyway is not a jump
	// into the middle of anything.
	TestEqual(TEXT("a seek of exactly one blend length still blends"),
		CrowdyMassNearTier::ActionBlendInForSeek(AuthoredBlendSeconds, AuthoredBlendSeconds), AuthoredBlendSeconds);
	TestEqual(TEXT("and one just past it does not"),
		CrowdyMassNearTier::ActionBlendInForSeek(AuthoredBlendSeconds, AuthoredBlendSeconds + 0.01f), 0.0f);
	TestEqual(TEXT("an action starting at the very first frame of the clip blends"),
		CrowdyMassNearTier::ActionBlendInForSeek(AuthoredBlendSeconds, 0.0f), AuthoredBlendSeconds);

	// A blend length reaches this from a property a graph can write, and a seek from a fragment, so neither
	// is trusted to be a sane number any more than a play rate off the wire is.
	TestEqual(TEXT("a negative blend length is no blend rather than a negative one"),
		CrowdyMassNearTier::ActionBlendInForSeek(-1.0f, 0.0f), 0.0f);
	TestEqual(TEXT("nor does a non-finite one reach the montage"),
		CrowdyMassNearTier::ActionBlendInForSeek(FMath::Sqrt(-1.0f), 0.0f), 0.0f);
	TestEqual(TEXT("and a non-finite seek blends into nothing rather than into wherever it landed"),
		CrowdyMassNearTier::ActionBlendInForSeek(AuthoredBlendSeconds, FMath::Sqrt(-1.0f)), 0.0f);

	return true;
}

/**
 * An actor takes its entity's action again on the frame it starts being drawn, whatever the serial says.
 *
 * While an action runs its serial does not change, which is exactly what stops a running clip being
 * restarted every frame. It also means the serial cannot tell a montage that is playing correctly from one
 * frozen where it started, and both happen: a mesh that is not being drawn does not tick, so an action
 * started when the view bound has not advanced by the time the actor is shown some frames later, and a mesh
 * out of the actor pool is holding whoever it was drawing last.
 *
 * So being shown is its own reason to start, and the two callers differ by nothing else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierBeingShownRestartsTheHeldActionTest,
	"CrowdySDK.Mass.NearTierBeingShownRestartsTheHeldAction", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierBeingShownRestartsTheHeldActionTest::RunTest(const FString& Parameters)
{
	// The ordinary per-frame answer for an action this view has already started: leave it playing.
	TestFalse(TEXT("a serial already started is not started again on an ordinary frame"),
		CrowdyMassNearTier::ShouldStartHeldAction(/*bIsBeingShownNow=*/false, /*bHasStartedAnAction=*/true,
			/*StartedSerial=*/7, /*IncomingSerial=*/7));

	// The same action, on the frame the actor starts being drawn.
	TestTrue(TEXT("while the frame it starts being drawn on takes that same action again"),
		CrowdyMassNearTier::ShouldStartHeldAction(/*bIsBeingShownNow=*/true, /*bHasStartedAnAction=*/true,
			/*StartedSerial=*/7, /*IncomingSerial=*/7));

	TestTrue(TEXT("an action this view has never started is started"),
		CrowdyMassNearTier::ShouldStartHeldAction(false, /*bHasStartedAnAction=*/false, 0, 0));
	TestTrue(TEXT("and so is a different serial"),
		CrowdyMassNearTier::ShouldStartHeldAction(false, true, 7, 8));

	// Freshness is inequality, not ordering. A serial that has wrapped is a new action like any other, and
	// a "newer than" test would refuse it and every action after it for the rest of that entity's life.
	TestTrue(TEXT("a serial that has wrapped past the top of its range is still a new action"),
		CrowdyMassNearTier::ShouldStartHeldAction(false, true, MAX_int32, MIN_int32));

	return true;
}

/**
 * A promoted actor stands where the crowd drew the row it replaced, including the way it faces.
 *
 * The crowd does not draw a mesh at the entity's transform. It draws it at the mesh transform authored
 * beside the mesh, applied to the entity's transform, and the standard skeleton needs a quarter turn there
 * before it faces the way it walks. An actor that ignored that number would be turned 90 degrees away from
 * the row it just replaced, and since both representations track the entity correctly, nothing about the
 * picture says which of the two is wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierMeshTransformMirrorsTheCrowdTest,
	"CrowdySDK.Mass.NearTierMeshTransformMirrorsTheCrowd", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierMeshTransformMirrorsTheCrowdTest::RunTest(const FString& Parameters)
{
	// The correction a character mesh usually carries: a quarter turn, and dropped to stand on the ground.
	const FTransform AuthoredMeshTransform(FRotator(0.0, -90.0, 0.0), FVector(0.0, 0.0, -90.0));

	FSkinnedMeshInstanceVisualizationDesc Desc;
	TestTrue(TEXT("a description naming no mesh leaves whatever the shell authored standing"),
		CrowdyMassNearTier::MeshTransformFromVisualizationDesc(Desc).Equals(FTransform::Identity));

	FMassSkinnedMeshInstanceVisualizationMeshDesc MeshDesc;
	MeshDesc.LocalTransform = AuthoredMeshTransform;
	Desc.Meshes.Add(MeshDesc);

	const FTransform Mirrored = CrowdyMassNearTier::MeshTransformFromVisualizationDesc(Desc);
	TestTrue(TEXT("and a description that names one hands back exactly the transform the crowd draws it at"),
		Mirrored.Equals(AuthoredMeshTransform));

	// Stated as the thing that actually goes wrong, not just as an equality: the yaw is the half a reader
	// sees on screen, and identity is what the shell had before this was carried across.
	TestFalse(TEXT("which is not identity, since identity is the 90 degrees a promotion used to turn a character by"),
		Mirrored.Equals(FTransform::Identity));
	TestTrue(TEXT("and it is the crowd's own number rather than a correction invented here"),
		FMath::IsNearlyEqual(Mirrored.GetRotation().Rotator().Yaw, -90.0, 0.01));

	// The half that matters most, because getting it wrong erases the only remedy an author has left. A
	// description with no placement of its own must leave the shell's mesh where its author put it: taking
	// identity as an instruction overwrites a correction typed in by hand, every time the actor binds, and
	// the character then keeps facing the wrong way with nothing to blame it on.
	const FTransform AuthoredByHand(FRotator(0.0, -90.0, 0.0), FVector::ZeroVector);
	FSkinnedMeshInstanceVisualizationDesc SilentDesc;
	TestTrue(TEXT("a description with no mesh leaves an authored correction exactly as its author left it"),
		CrowdyMassNearTier::ResolveShellMeshTransform(SilentDesc, AuthoredByHand).Equals(AuthoredByHand));

	FMassSkinnedMeshInstanceVisualizationMeshDesc SilentMeshDesc;
	SilentDesc.Meshes.Add(SilentMeshDesc);
	TestTrue(TEXT("and so does a description whose mesh carries no placement, which is not an instruction to stand at identity"),
		CrowdyMassNearTier::ResolveShellMeshTransform(SilentDesc, AuthoredByHand).Equals(AuthoredByHand));
	TestFalse(TEXT("so an authored correction is never quietly replaced by identity"),
		CrowdyMassNearTier::ResolveShellMeshTransform(SilentDesc, AuthoredByHand).Equals(FTransform::Identity));

	TestTrue(TEXT("and an unauthored shell simply takes the crowd's placement"),
		CrowdyMassNearTier::ResolveShellMeshTransform(Desc, FTransform::Identity).Equals(AuthoredMeshTransform));

	// The shape a real character config has, and the one that made a promoted actor face the wrong way: the
	// crowd's description drops the mesh to stand it on the ground and says nothing about rotation, because
	// the instanced path and a skeletal mesh component do not need the same turn to face the same way. Both
	// halves have to survive. Keeping only the description leaves the mesh facing wrong; keeping only the
	// shell's correction leaves it floating.
	FSkinnedMeshInstanceVisualizationDesc DropOnlyDesc;
	FMassSkinnedMeshInstanceVisualizationMeshDesc DropOnlyMeshDesc;
	DropOnlyMeshDesc.LocalTransform = FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, -89.0));
	DropOnlyDesc.Meshes.Add(DropOnlyMeshDesc);

	const FTransform Composed = CrowdyMassNearTier::ResolveShellMeshTransform(DropOnlyDesc, AuthoredByHand);
	TestTrue(TEXT("the shell's turn survives a description that carries only a drop"),
		FMath::IsNearlyEqual(Composed.GetRotation().Rotator().Yaw, -90.0, 0.01));
	TestTrue(TEXT("and the crowd's drop survives the shell's turn, so the mesh neither floats nor sinks"),
		FMath::IsNearlyEqual(Composed.GetLocation().Z, -89.0, 0.01));
	TestFalse(TEXT("so the answer is neither half on its own"),
		Composed.Equals(AuthoredByHand) || Composed.Equals(DropOnlyMeshDesc.LocalTransform));

	// A second mesh must not change the answer for the one the shell draws.
	FMassSkinnedMeshInstanceVisualizationMeshDesc SecondMeshDesc;
	SecondMeshDesc.LocalTransform = FTransform(FRotator(0.0, 45.0, 0.0), FVector(5.0, 5.0, 5.0));
	Desc.Meshes.Add(SecondMeshDesc);
	TestTrue(TEXT("and a description built from several meshes still places the shell's one by the first"),
		CrowdyMassNearTier::MeshTransformFromVisualizationDesc(Desc).Equals(AuthoredMeshTransform));

	return true;
}

/**
 * The shell's authored mesh placement is taken once and never re-read, because this component writes over
 * the very value it would be reading.
 *
 * Composing the crowd's placement into the mesh's CURRENT transform looks right for exactly one promotion.
 * On the second, the value read back already contains the crowd's placement, so it goes in again, and a
 * pooled actor crossing the promotion boundary sinks by that offset every single time until it is under
 * the map. It presents as a character that vanishes while crossing the boundary, which sounds like a
 * visibility bug and is not one, so the arithmetic is pinned here rather than left to be recognised.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredMeshPlacementIsTakenOnceTest,
	"CrowdySDK.Mass.NearTierAuthoredMeshPlacementIsTakenOnce", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierAuthoredMeshPlacementIsTakenOnceTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view to ask"), View))
	{
		return false;
	}

	const FTransform AuthoredByHand(FRotator(0.0, -90.0, 0.0), FVector::ZeroVector);
	Shell->Mesh->SetRelativeTransform(AuthoredByHand);

	TestTrue(TEXT("the placement taken first is the one the shell's author left"),
		View->CaptureAuthoredMeshTransform(*Shell->Mesh).Equals(AuthoredByHand));

	// What the component itself does next: it writes its composed answer onto the mesh. Asking again after
	// that must still answer with what the AUTHOR left, not with what was just written.
	const FTransform ComposedOnFirstBind(FRotator(0.0, -180.0, 0.0), FVector(0.0, 0.0, -89.0));
	Shell->Mesh->SetRelativeTransform(ComposedOnFirstBind);

	TestTrue(TEXT("and asking again after this component has moved the mesh still answers with the author's placement"),
		View->CaptureAuthoredMeshTransform(*Shell->Mesh).Equals(AuthoredByHand));
	TestFalse(TEXT("so a second promotion cannot compose the crowd's placement in a second time"),
		View->CaptureAuthoredMeshTransform(*Shell->Mesh).Equals(ComposedOnFirstBind));

	// The accumulation this prevents, stated as the number a reader would see: the crowd's drop going in
	// once per promotion rather than once.
	const FTransform CrowdPlacement(FRotator::ZeroRotator, FVector(0.0, 0.0, -89.0));
	FTransform Settled = CrowdyMassNearTier::ResolveShellMeshTransform(
		FSkinnedMeshInstanceVisualizationDesc(), View->CaptureAuthoredMeshTransform(*Shell->Mesh));
	for (int32 Promotion = 0; Promotion < 8; ++Promotion)
	{
		Settled = View->CaptureAuthoredMeshTransform(*Shell->Mesh) * CrowdPlacement;
		Shell->Mesh->SetRelativeTransform(Settled);
	}

	TestTrue(TEXT("so eight promotions leave the mesh at the same depth as one, rather than eight times it"),
		FMath::IsNearlyEqual(Settled.GetLocation().Z, -89.0, 0.01));

	return true;
}

/**
 * An action is played on a mesh that has something to draw, never merely on the first one found.
 *
 * A skeletal mesh with no asset accepts an animation and reports success, having shown nothing, and an
 * actor's components are held in a set with no authored order. So an actor carrying an empty mesh beside a
 * real one, which is exactly the shape of a shell whose own mesh is left empty for a subclass to replace,
 * would swallow every action on some runs and play them on others.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierActionMeshMustHaveAnAssetTest,
	"CrowdySDK.Mass.NearTierActionMeshMustHaveAnAsset", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierActionMeshMustHaveAnAssetTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());

	TestNull(TEXT("an actor whose only mesh has resolved no asset has nothing to play an action on"),
		CrowdyMassNearTier::ChooseActionMesh(*Shell));

	// The subclass shape: the inherited mesh left empty, a mesh of the subclass's own carrying the asset.
	USkeletalMeshComponent* OwnMesh = NewObject<USkeletalMeshComponent>(Shell.Get(), TEXT("SubclassMesh"));
	OwnMesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(Shell.Get()));

	if (!TestNull(TEXT("the inherited mesh really is empty, so the answer below cannot come from it"),
		Shell->Mesh->GetSkinnedAsset()))
	{
		return false;
	}
	TestSamePtr(TEXT("so the mesh that has something to draw is the one an action is played on"),
		CrowdyMassNearTier::ChooseActionMesh(*Shell), OwnMesh);

	// And once the shell's own mesh can draw, it wins: that is the one the crowd's mesh transform is
	// mirrored onto, and therefore the one standing where the entity stands.
	Shell->Mesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(Shell.Get()));
	TestSamePtr(TEXT("and the shell's own mesh wins once it has an asset of its own"),
		CrowdyMassNearTier::ChooseActionMesh(*Shell), static_cast<USkeletalMeshComponent*>(Shell->Mesh.Get()));

	return true;
}

struct FCrowdyMassNearTierTraceCollisionTestAccess
{
	static void Apply(UCrowdyMassNearTierComponent& View, const bool bEnable)
	{
		View.ApplyTraceCollisionFromFragment(bEnable, ECC_Pawn, FCollisionResponseContainer::GetDefaultResponseContainer());
	}

	static void Clear(UCrowdyMassNearTierComponent& View) { View.ClearAppliedTraceCollision(); }

	static bool HasApplied(const UCrowdyMassNearTierComponent& View) { return View.bHasAppliedTraceCollision; }
};

/**
 * Only the mesh that draws the character, and a component tagged CrowdyTraceCollision, answer traces as the entity.
 *
 * A held weapon or a hat is part of the picture and not part of the body, so giving it query collision would let
 * a shot at the decoration count as a hit on the entity.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierTraceCollisionStaysOffDecorationTest,
	"CrowdySDK.Mass.NearTierTraceCollisionStaysOffDecoration", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierTraceCollisionStaysOffDecorationTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;
	using FAccess = FCrowdyMassNearTierTraceCollisionTestAccess;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view"), View))
	{
		return false;
	}

	USkeletalMeshComponent* Mesh = Shell->Mesh;
	UStaticMeshComponent* Decoration = NewObject<UStaticMeshComponent>(Shell.Get(), TEXT("HeldWeapon"));
	UBoxComponent* HitBox = NewObject<UBoxComponent>(Shell.Get(), TEXT("HitBox"));
	HitBox->ComponentTags.Add(TEXT("CrowdyTraceCollision"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Decoration->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(Shell.Get()));

	FAccess::Apply(*View, true);
	TestTrue(TEXT("the mesh that draws the character answers traces"),
		CollisionEnabledHasQuery(Mesh->BodyInstance.GetCollisionEnabled(false)));
	TestTrue(TEXT("a decoration is left as authored"), Decoration->BodyInstance.GetCollisionEnabled(false) == ECollisionEnabled::NoCollision);
	TestTrue(TEXT("a component tagged to opt in answers traces"),
		CollisionEnabledHasQuery(HitBox->BodyInstance.GetCollisionEnabled(false)));

	FAccess::Clear(*View);
	TestFalse(TEXT("clearing takes the mesh's query collision back off"),
		CollisionEnabledHasQuery(Mesh->BodyInstance.GetCollisionEnabled(false)));
	TestFalse(TEXT("and the tagged component's"),
		CollisionEnabledHasQuery(HitBox->BodyInstance.GetCollisionEnabled(false)));
	TestTrue(TEXT("and still leaves the decoration alone"), Decoration->BodyInstance.GetCollisionEnabled(false) == ECollisionEnabled::NoCollision);
	return true;
}

/**
 * A shell whose mesh has no asset yet has nothing to take trace collision, and must not remember having pushed it.
 *
 * Latching the push would leave the mesh without query collision for the rest of the promotion once its asset loaded.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierTraceCollisionRetriesUntilTheMeshHasAnAssetTest,
	"CrowdySDK.Mass.NearTierTraceCollisionRetriesUntilTheMeshHasAnAsset", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierTraceCollisionRetriesUntilTheMeshHasAnAssetTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;
	using FAccess = FCrowdyMassNearTierTraceCollisionTestAccess;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view"), View))
	{
		return false;
	}

	USkeletalMeshComponent* Mesh = Shell->Mesh;
	UBoxComponent* HitBox = NewObject<UBoxComponent>(Shell.Get(), TEXT("HitBox"));
	HitBox->ComponentTags.Add(TEXT("CrowdyTraceCollision"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	FAccess::Apply(*View, true);
	TestFalse(TEXT("with no asset on the mesh nothing is remembered as applied"), FAccess::HasApplied(*View));
	TestFalse(TEXT("and the mesh is not pushed"), CollisionEnabledHasQuery(Mesh->BodyInstance.GetCollisionEnabled(false)));
	TestTrue(TEXT("though a tagged component does not wait for it"), CollisionEnabledHasQuery(HitBox->BodyInstance.GetCollisionEnabled(false)));

	Mesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(Shell.Get()));
	FAccess::Apply(*View, true);
	TestTrue(TEXT("a later push with the same values still lands"), FAccess::HasApplied(*View));
	TestTrue(TEXT("on the mesh"), CollisionEnabledHasQuery(Mesh->BodyInstance.GetCollisionEnabled(false)));
	return true;
}

/**
 * A component tagged CrowdyTraceCollision answers traces whether or not the shell has a skinned mesh to draw.
 *
 * A shell whose only hit target is a tagged box, or one with no skeletal mesh at all, would otherwise never be hittable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierTraceCollisionReachesTaggedComponentsWithoutAMeshTest,
	"CrowdySDK.Mass.NearTierTraceCollisionReachesTaggedComponentsWithoutAMesh", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierTraceCollisionReachesTaggedComponentsWithoutAMeshTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;
	using FAccess = FCrowdyMassNearTierTraceCollisionTestAccess;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view"), View))
	{
		return false;
	}

	UBoxComponent* HitBox = NewObject<UBoxComponent>(Shell.Get(), TEXT("HitBox"));
	HitBox->ComponentTags.Add(TEXT("CrowdyTraceCollision"));
	HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Shell->Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	FAccess::Apply(*View, true);
	TestTrue(TEXT("a shell whose mesh has no skinned asset still has its tagged box hittable"),
		CollisionEnabledHasQuery(HitBox->BodyInstance.GetCollisionEnabled(false)));
	TestFalse(TEXT("while the mesh with nothing to draw stays out of it"),
		CollisionEnabledHasQuery(Shell->Mesh->BodyInstance.GetCollisionEnabled(false)));

	FAccess::Clear(*View);
	TestFalse(TEXT("and clearing takes the box's query collision back off even though no mesh ever resolved"),
		CollisionEnabledHasQuery(HitBox->BodyInstance.GetCollisionEnabled(false)));

	AActor* Bare = NewObject<AActor>(GetTransientPackage(), NAME_None, RF_Transient);
	const TStrongObjectPtr<AActor> BareHolder(Bare);
	UCrowdyMassNearTierComponent* BareView = NewObject<UCrowdyMassNearTierComponent>(Bare);
	UBoxComponent* BareBox = NewObject<UBoxComponent>(Bare, TEXT("BareBox"));
	BareBox->ComponentTags.Add(TEXT("CrowdyTraceCollision"));
	BareBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	FAccess::Apply(*BareView, true);
	TestTrue(TEXT("an actor with no skeletal mesh at all gets its tagged box hittable"),
		CollisionEnabledHasQuery(BareBox->BodyInstance.GetCollisionEnabled(false)));
	TestTrue(TEXT("and remembers it, since no mesh is coming to retry for"), FAccess::HasApplied(*BareView));
	return true;
}

/**
 * Clearing undoes what the push wrote to, not what would be chosen again now.
 *
 * A mesh that lost its asset after the push is no longer the action mesh, so a clear that re-chose would leave it hittable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierTraceCollisionClearUndoesWhatWasPushedTest,
	"CrowdySDK.Mass.NearTierTraceCollisionClearUndoesWhatWasPushed", CrowdyMassNearTierViewTestFlags)
bool FCrowdyMassNearTierTraceCollisionClearUndoesWhatWasPushedTest::RunTest(const FString& Parameters)
{
	using FFixture = FCrowdyMassNearTierViewTestFixture;
	using FAccess = FCrowdyMassNearTierTraceCollisionTestAccess;

	const TStrongObjectPtr<ACrowdyMassNearTierActor> Shell(FFixture::MakeShell());
	UCrowdyMassNearTierComponent* View = Shell->NearTier;
	if (!TestNotNull(TEXT("the shell carries a near tier view"), View))
	{
		return false;
	}

	USkeletalMeshComponent* Mesh = Shell->Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetSkinnedAssetAndUpdate(FFixture::MakeStandInAsset(Shell.Get()));

	FAccess::Apply(*View, true);
	TestTrue(TEXT("the mesh answers traces"), CollisionEnabledHasQuery(Mesh->BodyInstance.GetCollisionEnabled(false)));

	Mesh->SetSkinnedAssetAndUpdate(nullptr);
	FAccess::Clear(*View);
	TestFalse(TEXT("clearing after the mesh lost its asset still takes its query collision back off"),
		CollisionEnabledHasQuery(Mesh->BodyInstance.GetCollisionEnabled(false)));
	TestFalse(TEXT("and forgets the push, so the next one is not skipped as unchanged"), FAccess::HasApplied(*View));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
