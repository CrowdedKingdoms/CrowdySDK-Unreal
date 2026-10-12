#include "Dev/CrowdyMassGateHarness.h"

#include "Animation/AnimSequenceTransformProviderData.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "CrowdyMassLog.h"
#include "Dev/CrowdyMassGateStateEntity.h"
#include "Dev/CrowdyMassGateWireState.h"
#include "Dev/CrowdyMassStateDeltaBuilder.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "MassEntityManager.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "MassVisualizationComponent.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassTargetLibrary.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "View/CrowdyMassViewState.h"

namespace CrowdyMassGate
{
	// The action fired by the automated run. Id 1 is the first authored action in any anim set, so a
	// project that has authored one at all resolves it; one that has not still exercises every gate
	// except the "was it shown" reading, which reports itself as unshown rather than failing.
	constexpr int32 GateActionId = 1;

	constexpr int32 GateEntityCount = 3;
	constexpr int32 FiredEntityIndex = 1;
	constexpr int32 ForeignOwnerIndex = 2;

	// How long a step may take before it is failed. Generous against a frame rate a crowd might drag
	// down, and finite so a run always ends with a verdict rather than sitting in a state.
	constexpr float ShortStepSeconds = 2.0f;

	// The action-scoped defer budget is a fraction of a second, so a stale event must be gone well
	// inside this. Sized to be unambiguous: still queued after this means it got the ordinary
	// six-hundred-retry budget instead, which is the failure this gate exists to catch.
	constexpr float StaleStepSeconds = 3.0f;

	constexpr float EntitySpacingCentimetres = 200.0f;

	// How far along the camera's aim to look for something to fire at manually.
	constexpr float AimRangeCentimetres = 100000.0f;

	// How many bytes the view-state echo appends to a position update: the four trailing single-byte
	// fields of the harness's wire struct. Dropping exactly this many off the end of a current payload
	// is how the harness builds the payload an older producer sends.
	constexpr int32 ViewStateAppendedByteCount = 4;

	// Half the length of the ray the trace-grid gate fires through the echo entity. It only has to reach
	// the entity, not model a weapon: the query answers "which entities are near this line".
	constexpr float TraceGridHalfLengthCentimetres = 500.0f;

	// How many entities the animation-track gate spawns and destroys, and the range that count is held
	// to. The default is high enough that a per-entity leak shows as a clear step in the track count and
	// low enough to spawn, render and destroy inside the windows below on an ordinary session.
	constexpr int32 DefaultChurnEntityCount = 64;
	constexpr int32 MinChurnEntityCount = 1;
	constexpr int32 MaxChurnEntityCount = 1024;

	// How long the churn entities are left alive before being destroyed. They have to survive long
	// enough for the renderer to be handed them, which is several frames of visualization work rather
	// than the one frame it takes to spawn them.
	constexpr float ChurnSettleSeconds = 2.0f;

	// How long the track count is watched for its return after the last entity is destroyed. Longer than
	// the settle window because removal drains over the frames that follow, and a count that has still
	// not come back by the end of it is the leak this gate exists to catch.
	constexpr float ChurnDrainSeconds = 5.0f;

	// Gate names, written once so a skip and a verdict can never disagree about what was measured.
	constexpr const TCHAR* GateE1 = TEXT("E1 a decoded pose lands on the entity");
	constexpr const TCHAR* GateE2 = TEXT("E2 a second echo changes the pose it landed on");
	constexpr const TCHAR* GateE3 = TEXT("E3 a pose echo sets a pose and nothing else");
	constexpr const TCHAR* GateE4 = TEXT("E4 an echo carrying no pose clears the pose");
	constexpr const TCHAR* GateE5 = TEXT("E5 no view-state echo can unmake an entity");
	constexpr const TCHAR* GateE6 = TEXT("E6 a truncated payload keeps its fail-safe defaults");
	constexpr const TCHAR* GateE7 = TEXT("E7 a forged payload is harmless");
	constexpr const TCHAR* GateL1 = TEXT("L1 animation tracks return to their baseline after churn");
	constexpr const TCHAR* GateD1 = TEXT("D1 every authored pose has a bank slot of its own");
	constexpr const TCHAR* GateS1 = TEXT("S1 a crowdystate delta reaches an entity that owns no participant");
	constexpr const TCHAR* GateS2 = TEXT("S2 nothing on the crowdystate path was dropped");
	constexpr const TCHAR* GateS3 = TEXT("S3 the pose came from the state plane and not the echo");
	constexpr const TCHAR* GateS4 = TEXT("S4 a field registered from outside this module receives its bytes");

	// The two pose ids the echo gates send. Any two distinct non-zero ids would do: these gates are about
	// a pose id reaching the entity and changing nothing else, never about which animation it resolves
	// to, so they must not depend on what a game happens to have authored. 1 is the first authored row.
	constexpr uint8 FirstEchoPoseId = 1;
	constexpr uint8 SecondEchoPoseId = 2;

	// A pose id no gate ever sends, written onto the entity before a gate that asserts a decoded value.
	// It has to stay out of the set above, out of the forged and truncated payloads below, and out of the
	// ids the CrowdyState gates send, or the poisoning would be indistinguishable from a value that
	// arrived. The CrowdyState gates poison with the same value, on their own entity.
	constexpr uint8 PoisonPoseId = 77;

	// The pose id the "cannot unmake an entity" gate forges. Distinct from every other id sent here and
	// from the poison value above, so that gate can tell an echo that was ingested from one that never
	// reached the entity at all.
	constexpr uint8 UnmakeEchoPoseId = 200;

	// How many frames that gate lets pass between pushing its echo and asserting. One is the minimum the
	// property needs: registered ingestors write their fragments inside the push itself, but a game's
	// processors and any listener bound to them do not run until the next processing phase, so a reading
	// taken in the same tick cannot see one of them destroy the entity. Two rather than one, so a
	// listener that defers its own work by a tick is still inside the window.
	constexpr uint64 UnmakeEchoFrameSpan = 2;

	// The pose ids the CrowdyState gates send, in order. The second is deliberately a repeat of the
	// first, and that repeat is part of what these gates measure.
	//
	// One decode buffer is kept per class and reused, so by the time the second delta is decoded the
	// buffer already holds the first one's value. The codec calls a slot CHANGED only where the decoded
	// value differs from what the buffer held, so the second delta's pose is decoded as present and
	// unchanged. A receiver that delivered the changed set rather than the present set would report that
	// delta as having delivered nothing, and the same suppression would silence every re-send after it.
	// The buffer is shared between entities as well as between deltas, which is what makes this the
	// ordinary case in a crowd converging on one value rather than an edge case; one entity sending the
	// same value twice is the smallest way to reach it.
	// STANCE VALUES, not pose ids. What travels is the game's own enum value, and the row it selects is
	// decided on this side by the animation set below.
	constexpr uint8 StateStances[] = {
		static_cast<uint8>(ECrowdyMassGateStance::Wounded),
		static_cast<uint8>(ECrowdyMassGateStance::Wounded),
		static_cast<uint8>(ECrowdyMassGateStance::Downed)};

	// The pose rows the harness authors for the gate class, IN THIS ORDER, and the order is the whole
	// point. Row N answers pose id N+1, so Downed (stance value 2) is id 1 and Wounded (stance value 1) is
	// id 2. Every stance the gates send therefore maps to an id that is NOT its own value, which is what
	// makes these gates able to tell a value-to-row mapping from an arriving byte written straight through.
	// Idle is deliberately unauthored: a stance no row names clears the pose.
	const FName StatePoseRowNames[] = {TEXT("Downed"), TEXT("Wounded")};

	// What the last stance sent above must read as: Downed is row 0, so id 1.
	constexpr uint8 ExpectedPoseIdForLastStance = 1;

	// The stance the field-registration gate sends, and the id it must produce. Distinct from the last
	// stance above so that gate cannot pass on a pose the deltas before it left behind: Wounded is row 1,
	// so id 2.
	constexpr uint8 StateGenericStance = static_cast<uint8>(ECrowdyMassGateStance::Wounded);
	constexpr uint8 ExpectedPoseIdForGenericStance = 2;

	// The byte the field this module claims no ingestor for carries. Distinct from every pose id and every
	// stance value here, so a reading of it cannot be one of those that leaked into the wrong field.
	constexpr uint8 GateOnlyStateByteValue = 222;

	// The two CrowdyState property names those deltas write.
	//
	// They are found in two different ways, and that difference is deliberate. The pose property is bound
	// by its declared TYPE, so this name is used only to ADDRESS it when building a delta, exactly as a
	// real sender addresses its own member; renaming the property means changing this line and nothing
	// else. GateOnlyStateByte is bound by NAME through the public registration call, so there the name IS
	// the contract on both sides.
	const FName StatePoseFieldName(TEXT("Stance"));
	const FName GateOnlyStateFieldName(TEXT("GateOnlyStateByte"));

	// The field on the view-state ECHO plane the E gates drive, matching the byte FCrowdyMassGateWireState
	// declares. Registered by this harness rather than by CrowdyMass: the plugin's own standard pose field
	// on this plane was deleted in Phase A.4, so the name is the harness's to claim now. A name claimed on
	// this plane is claimed process-wide across every wire struct, which is exactly why the plugin stopped
	// claiming this one.
	const FName EchoPoseFieldName(TEXT("PoseId"));

	// Salts for the CrowdyState entity and for the peer its deltas name. Both are clear of every other
	// salt here, so no run can address one set of entities while believing it is addressing another, and
	// the sender is a distinct id because a delta whose sender is this client is dropped before anything
	// else runs.
	constexpr int32 StateGuidSalt = 93;
	constexpr int32 StateSenderGuidSalt = 94;

	// Names the trace-grid leg of that gate in words, including the ways it can be not attempted. Those
	// must never read as a pass: a grid that cannot answer hands back every tracked entity, so the echo
	// entity would be in the list whether the grid still held it or not.
	const TCHAR* DescribeUnmakeGridLeg(const bool bAnsweredBefore, const bool bHeldBefore,
		const bool bAnsweredAfter, const bool bHeldAfter)
	{
		if (!bAnsweredBefore)
		{
			return TEXT("not attempted, the spatial grid did not answer this query before the echo (it is switched off in this world, or holds no cells), so the candidate list is simply every tracked entity and would contain the echo entity either way");
		}

		if (!bHeldBefore)
		{
			return TEXT("not attempted, the grid did not return the entity before the echo either, so there is no 'still' here to observe");
		}

		if (!bAnsweredAfter)
		{
			return TEXT("NO, the grid answered before the echo and stopped answering after it, so it can no longer say the entity is there");
		}

		return bHeldAfter
			? TEXT("yes")
			: TEXT("NO, the echo took the entity out of the grid and it is now untraceable and untargetable for everyone else");
	}

	// Names the trace-collision leg of the same gate. An entity carrying no trace collision fragment is
	// not a difference between two readings, it is the absence of anything to compare, and it is said so
	// rather than folded into the verdict.
	FString DescribeUnmakeCollisionLeg(const bool bMeasured, const bool bCopied, const bool bUnchanged, const int32 ByteCount)
	{
		if (!bMeasured)
		{
			return TEXT("not attempted, the echo entity carries no trace collision fragment, so there are no bytes here for an echo to be shown leaving alone");
		}

		if (!bCopied)
		{
			return FString::Printf(TEXT("NO, the %d bytes could not be read back at all, so the fragment or the entity under it is gone"), ByteCount);
		}

		return bUnchanged
			? FString::Printf(TEXT("yes, all %d bytes"), ByteCount)
			: FString::Printf(TEXT("NO, the %d bytes differ"), ByteCount);
	}

	// Named rather than printed as a number, because the whole verdict of the animation-track gate turns
	// on which of these the destroy landed in.
	const TCHAR* DescribeTickGroup(const ETickingGroup Group)
	{
		switch (Group)
		{
		case TG_PrePhysics: return TEXT("TG_PrePhysics");
		case TG_StartPhysics: return TEXT("TG_StartPhysics");
		case TG_DuringPhysics: return TEXT("TG_DuringPhysics");
		case TG_EndPhysics: return TEXT("TG_EndPhysics");
		case TG_PostPhysics: return TEXT("TG_PostPhysics");
		case TG_PostUpdateWork: return TEXT("TG_PostUpdateWork");
		case TG_LastDemotable: return TEXT("TG_LastDemotable");
		case TG_NewlySpawned: return TEXT("TG_NewlySpawned");
		default: return TEXT("no tick group at all");
		}
	}

	// Salt for the entity the view-state echoes are pushed at, and the base of the range the churn
	// entities' ids are derived from. Both are clear of the salts the action gates use, so no run can
	// address one set of entities while believing it is addressing the other.
	constexpr int32 EchoGuidSalt = 92;
	constexpr int32 ChurnGuidSaltBase = 1000;

	FGuid MakeGateGuid(const int32 Salt)
	{
		// Derived rather than random, so a failing run names an id that can be searched for in a log and
		// a re-run addresses the same entities.
		return FGuid(0x0C0DE000 + Salt, 0x4A7E0001, 0x4A7E0002, 0x4A7E0003);
	}

	// One gate contributed by a consuming game.
	struct FExtraGate
	{
		FName Name;
		FCrowdyMassGateProbe Probe;
	};

	// Held inside a function rather than as a file-scope object, because the natural place to register a
	// gate is a game module's startup, which can run before this module's own static initialisers do.
	TArray<FExtraGate>& GetExtraGates()
	{
		static TArray<FExtraGate> Gates;
		return Gates;
	}

	// Picks a Mass entity for a console-driven action send: one along the camera's aim if World offers a
	// player controller and the spatial grid answers there, otherwise any entity the subsystem is
	// tracking. False, with every output left untouched, when neither offers one.
	//
	// Takes the subsystem and world as plain arguments rather than reading them off a harness instance so
	// this stays callable from a headless test with no live world at all: passing World as null simply
	// skips the aim leg and goes straight to the tracked fallback.
	bool ResolveSendActionTarget(UCrowdyMassEntitySubsystem& Mass, UWorld* World, FMassEntityHandle& OutEntity,
		FGuid& OutUUID, bool& bOutFromAimTrace)
	{
		bOutFromAimTrace = false;

		if (const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr)
		{
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);

			TArray<FMassEntityHandle> Candidates;
			int32 VisitedCells = 0;
			Mass.GetTraceCandidateEntities(ViewLocation,
				ViewLocation + ViewRotation.Vector() * AimRangeCentimetres, Candidates, VisitedCells);

			for (const FMassEntityHandle& Handle : Candidates)
			{
				if (Mass.FindUUIDByEntity(Handle, OutUUID))
				{
					OutEntity = Handle;
					bOutFromAimTrace = true;
					return true;
				}
			}
		}

		TArray<FMassEntityHandle> Tracked;
		Mass.GetTrackedRemoteEntities(Tracked);
		for (const FMassEntityHandle& Handle : Tracked)
		{
			if (Mass.FindUUIDByEntity(Handle, OutUUID))
			{
				OutEntity = Handle;
				return true;
			}
		}

		return false;
	}
}

void FCrowdyMassGateChurnDestroyTick::ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread,
	const FGraphEventRef& MyCompletionGraphEvent)
{
	if (!Harness)
	{
		return;
	}

	Harness->DestroyChurnEntitiesInsideFrame();
}

FString FCrowdyMassGateChurnDestroyTick::DiagnosticMessage()
{
	return TEXT("CrowdyMassGateHarness churn destroy");
}

bool UCrowdyMassGateHarness::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	return Super::ShouldCreateSubsystem(Outer);
#endif
}

void UCrowdyMassGateHarness::Deinitialize()
{
	StopGates();
	Super::Deinitialize();
}

UCrowdyMassGateHarness* UCrowdyMassGateHarness::Get(UWorld* World)
{
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyMassGateHarness>() : nullptr;
}

UCrowdyMassEntitySubsystem* UCrowdyMassGateHarness::GetMassSubsystem() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
}

UCrowdyEventRouter* UCrowdyMassGateHarness::GetRouter() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UCrowdyEventRouter>() : nullptr;
}

void UCrowdyMassGateHarness::FireOneShot(const int32 ActionId, const int32 Variant, const float PlayRate)
{
	UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	if (!Mass)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.oneshot: no Crowdy Mass entity subsystem in this world, so there is nothing to fire at. Run this from a play session."));
		return;
	}

	// Candidates along the aim ray from the subsystem's own spatial grid, rather than a full collision
	// trace. This is a dev aim, not a hit test: the grid answers "which entities are near this line",
	// which is enough to pick one to look at and avoids depending on a collision profile being set up.
	FGuid TargetUUID;
	if (const APlayerController* PlayerController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);

		TArray<FMassEntityHandle> Candidates;
		int32 VisitedCells = 0;
		Mass->GetTraceCandidateEntities(ViewLocation,
			ViewLocation + ViewRotation.Vector() * CrowdyMassGate::AimRangeCentimetres, Candidates, VisitedCells);

		for (const FMassEntityHandle& Handle : Candidates)
		{
			if (Mass->FindUUIDByEntity(Handle, TargetUUID))
			{
				break;
			}
		}
	}

	if (!TargetUUID.IsValid())
	{
		// Nothing along the aim: fall back to any tracked entity so the command still does something on a
		// map where the crowd is behind the camera. Named in the log, because "it fired at something else"
		// is otherwise indistinguishable from "the aim did not work".
		TArray<FMassEntityHandle> Tracked;
		Mass->GetTrackedRemoteEntities(Tracked);
		for (const FMassEntityHandle& Handle : Tracked)
		{
			if (Mass->FindUUIDByEntity(Handle, TargetUUID))
			{
				UE_LOG(LogCrowdyMass, Log,
					TEXT("crowdy.mass.oneshot: nothing along the aim, so firing at tracked entity %s instead."),
					*TargetUUID.ToString());
				break;
			}
		}
	}

	if (!TargetUUID.IsValid())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.oneshot: no Mass entity is tracked in this world, so there is nothing to fire at. Start a crowd first."));
		return;
	}

	UCrowdyMassCharacterAvatar* Avatar = Cast<UCrowdyMassCharacterAvatar>(
		Mass->GetOrCreateAvatar(TargetUUID, UCrowdyMassCharacterAvatar::StaticClass()));
	if (!Avatar)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.oneshot: entity %s resolved no character avatar, so the event has no receiver. Avatars are minted only for ids the tracker knows."),
			*TargetUUID.ToString());
		return;
	}

	// Asked before firing rather than reported after it. A client may only originate an event for an entity
	// it is the authority for, so aiming this at somebody else's player is refused deeper down, and saying
	// "fired" afterwards would describe a send that never happened: the reader then goes looking for the
	// reason nothing animated in the receiving half, which is the one place it cannot be.
	if (!Avatar->IsEventLocallyOwned())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.oneshot: entity %s belongs to another client, so this one may not fire for it and nothing was sent. This command can only drive an entity this client owns: turn on Render Local Owner in the Mass backend config to get one of your own, or trigger the action from the client that owns this character."),
			*TargetUUID.ToString());
		return;
	}

	Avatar->PlayOneShot(ActionId, Variant, PlayRate);

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.oneshot: fired action %d (variant %d, rate %.2f) at entity %s. This is a spatial multicast, so it plays on this machine as part of the send and on every other machine drawing the entity; crowdy.rpc.loopback only decides whether this machine's own run goes through the receive path."),
		ActionId, Variant, PlayRate, *TargetUUID.ToString());
}

void UCrowdyMassGateHarness::SendGenericAction(const FName ActionName, const int32 Variant, const float PlayRate)
{
	if (ActionName.IsNone())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.sendaction: no action name given. Usage: crowdy.mass.sendaction <actionName> [variant] [playRate]."));
		return;
	}

	UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	if (!Mass)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.sendaction: no Crowdy Mass entity subsystem in this world, so there is nothing to send '%s' to. Run this from a play session."),
			*ActionName.ToString());
		return;
	}

	// A refusal is reported once per class, name and verdict for the life of the process, which is right for
	// a send called every frame and wrong here: an operator running the same command twice would be told the
	// reason once and met with silence afterwards, with nothing to distinguish that from a send that worked.
	// Clearing the record first makes every run of this command answer for itself.
	CrowdyMassResetActionSendRefusalLog();

	FMassEntityHandle TargetEntity;
	FGuid TargetUUID;
	bool bFromAimTrace = false;
	if (!CrowdyMassGate::ResolveSendActionTarget(*Mass, GetWorld(), TargetEntity, TargetUUID, bFromAimTrace))
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.sendaction: no Mass entity is tracked in this world, so there is nothing to send '%s' to. Start a crowd first."),
			*ActionName.ToString());
		return;
	}

	if (!bFromAimTrace)
	{
		UE_LOG(LogCrowdyMass, Log,
			TEXT("crowdy.mass.sendaction: nothing along the aim, so sending '%s' at tracked entity %s instead."),
			*ActionName.ToString(), *TargetUUID.ToString());
	}

	FCrowdyMassEntityTarget Target;
	Target.Entity = TargetEntity;
	Target.World = GetWorld();

	// Send Crowdy Action has no ownership check, deliberately: any client may play an action on any
	// entity, so unlike crowdy.mass.oneshot this never refuses on whose entity it is.
	if (!UCrowdyMassTargetLibrary::SendEntityAction(Target, ActionName, Variant, PlayRate))
	{
		// Three of the four ways this can fail name their own reason above (no class learned yet, no
		// animation set, no row by this name), reported for this run because the record was cleared before
		// the send. The one that never speaks is the target resolving to nothing, which the library leaves
		// silent on purpose because it is hit every frame by a caller holding a target across a despawn.
		// This command fires once, so that silence would read as a command that did nothing rather than one
		// that was refused, hence the line below.
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.sendaction: '%s' was not sent to entity %s. If nothing above named a reason, the entity stopped being live between being picked and being sent to."),
			*ActionName.ToString(), *TargetUUID.ToString());
		return;
	}

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.sendaction: sent '%s' (variant %d, rate %.2f) to entity %s. This is a spatial multicast, so it plays on this machine as part of the send and on every other machine drawing the entity; crowdy.rpc.loopback only decides whether this machine's own run goes through the receive path."),
		*ActionName.ToString(), Variant, PlayRate, *TargetUUID.ToString());
}

void UCrowdyMassGateHarness::RegisterExtraGate(const FName Name, FCrowdyMassGateProbe Probe)
{
	if (Name.IsNone() || !Probe)
	{
		return;
	}

	TArray<CrowdyMassGate::FExtraGate>& Gates = CrowdyMassGate::GetExtraGates();
	for (CrowdyMassGate::FExtraGate& Gate : Gates)
	{
		if (Gate.Name == Name)
		{
			Gate.Probe = MoveTemp(Probe);
			return;
		}
	}

	CrowdyMassGate::FExtraGate& Added = Gates.AddDefaulted_GetRef();
	Added.Name = Name;
	Added.Probe = MoveTemp(Probe);
}

void UCrowdyMassGateHarness::UnregisterExtraGate(const FName Name)
{
	CrowdyMassGate::GetExtraGates().RemoveAll([Name](const CrowdyMassGate::FExtraGate& Gate)
	{
		return Gate.Name == Name;
	});
}

void UCrowdyMassGateHarness::RunGates(const int32 ChurnEntityCount)
{
	if (Step != ECrowdyMassGateStep::Idle)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("crowdy.mass.gates: a run is already in progress."));
		return;
	}

	if (!GetMassSubsystem() || !GetRouter())
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("crowdy.mass.gates: this world has no Crowdy Mass entity subsystem or no event router, so no gate can run. Start a play session first."));
		return;
	}

	RunWorld = GetWorld();
	Results.Reset();
	EntityUUIDs.Reset();
	EntityHandles.Reset();
	ChurnUUIDs.Reset();
	bActionWasApplied = false;
	DeferUUID = CrowdyMassGate::MakeGateGuid(90);
	StaleUUID = CrowdyMassGate::MakeGateGuid(91);
	EchoUUID = CrowdyMassGate::MakeGateGuid(CrowdyMassGate::EchoGuidSalt);
	EchoEntity.Reset();
	EchoTraceCollisionBefore.Reset();
	EchoPoseAfterApply = 0;
	EchoUnmakeFrameCounter = 0;
	bEchoUnmakeIngested = false;
	bEchoUnmakeGridAnswered = false;
	bEchoUnmakeInGridBefore = false;
	EchoUnmakeCollisionBefore.Reset();
	StateUUID = CrowdyMassGate::MakeGateGuid(CrowdyMassGate::StateGuidSalt);
	StateSenderID = CrowdyMassGate::MakeGateGuid(CrowdyMassGate::StateSenderGuidSalt);
	StateEntity.Reset();
	StateLayoutSlotCount = 0;
	StateCountersBefore = FCrowdyMassGateStateCounters();
	bGateFieldIngestorRan = false;
	GateFieldValueSeen = 0;
	bGateFieldReachedAFragment = false;
	ChurnEntityCountRequested = FMath::Clamp(ChurnEntityCount,
		CrowdyMassGate::MinChurnEntityCount, CrowdyMassGate::MaxChurnEntityCount);
	ChurnEntitiesSpawned = 0;
	TracksAtBaseline = 0;
	InstancesAtBaseline = 0;
	TracksAtPeak = 0;
	InstancesAtPeak = 0;
	bChurnDestroyRan = false;
	ChurnDestroyTickGroup = TG_MAX;
	ChurnDestroyFrameCounter = 0;

	// Forced on for the run and restored afterwards. Without it a sent event goes to the wire and the
	// send path applies it locally itself, which would make every observation below a test of a direct
	// function call rather than of the receive path.
	if (IConsoleVariable* Loopback = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.rpc.loopback")))
	{
		LoopbackBeforeRun = Loopback->GetInt();
		Loopback->Set(1, ECVF_SetByConsole);
		bChangedLoopback = true;
	}

	UE_LOG(LogCrowdyMass, Log, TEXT("crowdy.mass.gates: starting, churning %d entities for the animation-track reading."),
		ChurnEntityCountRequested);

	if (!TickHandle.IsValid())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UCrowdyMassGateHarness::Tick));
	}

	Advance(ECrowdyMassGateStep::Spawn, CrowdyMassGate::ShortStepSeconds);
}

void UCrowdyMassGateHarness::StopGates()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	DisarmChurnDestroyTick();
	DestroyGateEntities();
	DestroyChurnEntities();

	if (bChangedLoopback)
	{
		if (IConsoleVariable* Loopback = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.rpc.loopback")))
		{
			Loopback->Set(LoopbackBeforeRun, ECVF_SetByConsole);
		}
		bChangedLoopback = false;
	}

	Step = ECrowdyMassGateStep::Idle;
}

void UCrowdyMassGateHarness::Advance(const ECrowdyMassGateStep Next, const float TimeoutSeconds)
{
	Step = Next;
	StepStartedSeconds = FPlatformTime::Seconds();
	StepTimeoutSeconds = TimeoutSeconds;
}

bool UCrowdyMassGateHarness::HasStepTimedOut() const
{
	return FPlatformTime::Seconds() - StepStartedSeconds >= StepTimeoutSeconds;
}

void UCrowdyMassGateHarness::Record(const FString& Name, const bool bPassed, const FString& Observed)
{
	FCrowdyMassGateResult& Result = Results.AddDefaulted_GetRef();
	Result.Name = Name;
	Result.bPassed = bPassed;
	Result.Observed = Observed;
}

void UCrowdyMassGateHarness::Skip(const FString& Name, const FString& Why)
{
	FCrowdyMassGateResult& Result = Results.AddDefaulted_GetRef();
	Result.Name = Name;
	Result.bSkipped = true;
	Result.Observed = Why;
}

bool UCrowdyMassGateHarness::SpawnGateEntity(const int32 Index, const FGuid& UUID)
{
	UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	UWorld* World = GetWorld();
	if (!Mass || !World)
	{
		return false;
	}

	FVector Origin = FVector::ZeroVector;
	if (const APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			Origin = Pawn->GetActorLocation();
		}
	}

	const FTransform Transform(FRotator::ZeroRotator,
		Origin + FVector(Index * CrowdyMassGate::EntitySpacingCentimetres, 0.0f, 0.0f));

	const FMassEntityHandle Handle = Mass->SpawnRemoteEntity(UUID, Transform);
	if (!Handle.IsSet())
	{
		return false;
	}

	EntityHandles.Add(Handle);

	// Stamped so this client is the authority for the entity. Without it the send path refuses, which is
	// correct behaviour and is gated separately below; leaving it unstamped here would mean every other
	// gate silently measured that refusal instead of what it claims to measure.
	FMassEntityManager* EntityManager = Mass->GetMassEntityManager();
	UCrowdyEntitySubsystem* Entities = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!EntityManager || !Entities)
	{
		return false;
	}

	FCrowdyRemoteIdentityFragment* Identity = EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Handle);
	if (!Identity)
	{
		return false;
	}

	Identity->OwnerID = Entities->GetLocalPlayerID();
	Identity->Role = ECrowdyRole::Owner;
	return true;
}

void UCrowdyMassGateHarness::DestroyGateEntities()
{
	if (UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem())
	{
		for (const FGuid& UUID : EntityUUIDs)
		{
			Mass->DestroyRemoteEntity(UUID);
		}

		// Kept out of the array above so the indices the action gates address stay meaning what they
		// meant when they were spawned.
		if (EchoUUID.IsValid())
		{
			Mass->DestroyRemoteEntity(EchoUUID);
		}

		// Out of the array for the same reason, and destroyed here rather than left behind because it is
		// the only entity in the session spawned from this plugin's own template: one left alive would
		// sit in a crowd it does not belong to and be drawn by nothing.
		if (StateUUID.IsValid())
		{
			Mass->DestroyRemoteEntity(StateUUID);
		}
	}

	EntityUUIDs.Reset();
	EntityHandles.Reset();
	EchoUUID.Invalidate();
	EchoEntity.Reset();
	StateUUID.Invalidate();
	StateEntity.Reset();
}

void UCrowdyMassGateHarness::DestroyChurnEntities()
{
	if (UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem())
	{
		for (const FGuid& UUID : ChurnUUIDs)
		{
			Mass->DestroyRemoteEntity(UUID);
		}
	}

	ChurnUUIDs.Reset();
}

bool UCrowdyMassGateHarness::ArmChurnDestroyTick()
{
	UWorld* World = GetWorld();
	if (!World || !World->PersistentLevel)
	{
		return false;
	}

	if (ChurnDestroyTick.IsTickFunctionRegistered())
	{
		return true;
	}

	ChurnDestroyTick.Harness = this;
	ChurnDestroyTick.bCanEverTick = true;
	ChurnDestroyTick.bStartWithTickEnabled = false;

	// After the pre-physics group, where the pass that batches animation data for the renderer runs,
	// and before the post-physics group, at the end of which the renderer drains its removals and then
	// re-adds anything this frame batched. That span is where a destroy leaks an animation track, and
	// putting the destroy anywhere else would leave the gate reporting a release that was never at risk.
	ChurnDestroyTick.TickGroup = TG_DuringPhysics;

	ChurnDestroyTick.RegisterTickFunction(World->PersistentLevel);
	if (!ChurnDestroyTick.IsTickFunctionRegistered())
	{
		ChurnDestroyTick.Harness = nullptr;
		return false;
	}

	ChurnDestroyTick.SetTickFunctionEnable(true);
	return true;
}

void UCrowdyMassGateHarness::DisarmChurnDestroyTick()
{
	if (ChurnDestroyTick.IsTickFunctionRegistered())
	{
		ChurnDestroyTick.UnRegisterTickFunction();
	}

	ChurnDestroyTick.Harness = nullptr;
}

void UCrowdyMassGateHarness::DestroyChurnEntitiesInsideFrame()
{
	// Once only. The tick function is unregistered from the harness's own ticker rather than from in
	// here, because unregistering a tick function from inside its own tick edits the list being walked.
	if (bChurnDestroyRan)
	{
		return;
	}

	// Read before anything is destroyed, and read off the world rather than off this tick function's
	// own request. A tick function can be demoted to a later group by a prerequisite, and a run that
	// was demoted out of the window has to say so instead of reporting the group it asked for.
	const UWorld* World = GetWorld();
	ChurnDestroyTickGroup = World ? World->TickGroup : TEnumAsByte<ETickingGroup>(TG_MAX);
	ChurnDestroyFrameCounter = GFrameCounter;
	bChurnDestroyRan = true;

	DestroyChurnEntities();
}

FString UCrowdyMassGateHarness::DescribeChurnDestroyPosition() const
{
	if (!bChurnDestroyRan)
	{
		return TEXT("the destroy never ran from its tick function, so nothing was destroyed inside the frame window this gate measures");
	}

	// Judged against the groups strictly between pre-physics and post-physics rather than against the
	// group that was asked for. A destroy landing in either of those two outer groups may fall on
	// either side of the work it has to sit between, and a maybe has to read as a no here: claiming
	// the window was exercised when it might not have been is the exact fault this gate had.
	const bool bInsideWindow = ChurnDestroyTickGroup >= TG_StartPhysics && ChurnDestroyTickGroup <= TG_EndPhysics;

	return FString::Printf(
		TEXT("the destroy ran on frame %llu with the world executing %s, which is %s the window a destroy has to land in to leak an animation track: that window opens once the pre-physics group has handed the renderer this frame's animation batch, and closes at the end of the post-physics group, where the renderer drains its removals and then re-adds anything still batched"),
		ChurnDestroyFrameCounter, CrowdyMassGate::DescribeTickGroup(ChurnDestroyTickGroup),
		bInsideWindow ? TEXT("INSIDE") : TEXT("NOT provably inside"));
}

bool UCrowdyMassGateHarness::ValidatePoseBankSlots(FString& OutObserved, bool& bOutSkipped) const
{
	bOutSkipped = true;

	const UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
	if (!EntityManager)
	{
		OutObserved = TEXT("the world lost its Crowdy Mass entity subsystem mid-run");
		return false;
	}

	// Any live gate entity will do: the anim set and the bank are shared by every entity of the same
	// archetype, so this is a question about the archetype's authored content, not about one entity.
	FMassEntityHandle Entity;
	if (EntityManager->IsEntityValid(EchoEntity))
	{
		Entity = EchoEntity;
	}

	for (const FMassEntityHandle& Candidate : EntityHandles)
	{
		if (Entity.IsSet())
		{
			break;
		}

		if (EntityManager->IsEntityValid(Candidate))
		{
			Entity = Candidate;
		}
	}

	if (!Entity.IsSet())
	{
		OutObserved = TEXT("no gate entity was still alive to read an animation set off");
		return false;
	}

	const FCrowdyMassAnimSetSharedFragment* AnimSetFragment =
		EntityManager->GetConstSharedFragmentDataPtr<FCrowdyMassAnimSetSharedFragment>(Entity);
	if (!AnimSetFragment)
	{
		OutObserved = TEXT("the remote entity config carries no animation set shared fragment, so nothing here selects animations at all. Add the Crowdy Mass animation trait to it.");
		return false;
	}

	const UCrowdyMassAnimSet* AnimSet = AnimSetFragment->AnimSet.Get();
	if (!AnimSet)
	{
		OutObserved = TEXT("the animation trait has no animation set assigned, so there is no asset for a pose to be authored in");
		return false;
	}

	const FMassRepresentationSubsystemSharedFragment* RepresentationFragment =
		EntityManager->GetSharedFragmentDataPtr<FMassRepresentationSubsystemSharedFragment>(Entity);
	UMassRepresentationSubsystem* RepresentationSubsystem =
		RepresentationFragment ? RepresentationFragment->RepresentationSubsystem : nullptr;
	const FMassRepresentationFragment* Representation = EntityManager->GetFragmentDataPtr<FMassRepresentationFragment>(Entity);
	if (!RepresentationSubsystem || !Representation)
	{
		OutObserved = TEXT("the entity has no representation subsystem or no representation fragment, so there is no animation bank to resolve a pose against");
		return false;
	}

	FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();
	const int32 MeshInfoIndex = Representation->SkinnedMeshDescHandle.ToIndex();
	if (!MeshInfos.IsValidIndex(MeshInfoIndex))
	{
		OutObserved = TEXT("the entity names no registered skinned mesh description, so it is not drawn through the path that owns an animation bank and there is no bank to resolve against");
		return false;
	}

	// The first provider on the description, the same one the animation pass resolves against. A
	// description carrying more than one bank already warns from there; reading a different one here
	// would answer about a bank no entity animates from.
	const UAnimSequenceTransformProviderData* Provider = nullptr;
	for (const FMassSkinnedMeshInstanceVisualizationMeshDesc& MeshDesc : MeshInfos[MeshInfoIndex].GetDesc().Meshes)
	{
		Provider = Cast<UAnimSequenceTransformProviderData>(MeshDesc.TransformProvider.Get());
		if (Provider)
		{
			break;
		}
	}

	if (!Provider)
	{
		OutObserved = TEXT("the mesh description this entity renders through carries no animation sequence transform provider, so there is no bank of sequences for any animation to resolve into");
		return false;
	}

	TArray<UAnimSequence*> BankSequences;
	BankSequences.Reserve(Provider->GetSequences().Num());
	for (const FAnimSequenceTransformProviderSequence& BankEntry : Provider->GetSequences())
	{
		BankSequences.Add(BankEntry.Sequence);
	}

	// Built here rather than read out of the animation pass's cache, so this gate reports what the
	// authored content resolves to today and cannot be answered by a table built before it changed.
	FCrowdyMassResolvedAnimSet Resolved;
	Resolved.Build(AnimSet, BankSequences);

	// Which poses actually resolve. A pose that names nothing in this bank cannot freeze anything, so
	// it is not what this gate is about; an anim set with no resolvable pose at all has nothing to
	// check and says so rather than passing on an empty walk.
	int32 ResolvedPoseCount = 0;
	for (const FCrowdyMassResolvedPose& Pose : Resolved.ResolvedPoses)
	{
		ResolvedPoseCount += Pose.BankIndex != INDEX_NONE ? 1 : 0;
	}

	if (ResolvedPoseCount == 0)
	{
		// An authoring gap, not a code defect, so this is reported as not attempted. Named plainly
		// because nothing else in the run says it: a pose id still reaches the entity either way, so
		// every other pose gate passes while the entity keeps running its locomotion.
		OutObserved = FString::Printf(
			TEXT("animation set '%s' authors no pose that resolves against bank '%s' (%d sequences), so an entity put into a pose keeps playing whichever source is below it. Add a Poses row whose Sequence is in this bank; rank it Above Actions and check Hold Last Frame for a state the entity cannot act out of."),
			*AnimSet->GetName(), *Provider->GetName(), BankSequences.Num());
		return false;
	}

	bOutSkipped = false;

	// Every resolved pose is checked against every gait slot, against every other resolved pose, and
	// against both halves of every action slot pair. The renderer only reacts to the pushed index
	// CHANGING, so a pose sharing a slot with anything an entity can arrive from or leave to pushes an
	// index that never changes: the push is dropped as a no-op and the entity stays frozen on the pose
	// with its pose id correctly cleared. No amount of validation of either row alone can see this,
	// because neither is wrong on its own; it is a property of the pair against one bank.
	// Walked off the resolved table, which is parallel to the authored rows and never longer than them,
	// so a gait is named by whatever the author called it rather than by a fixed list this code knows.
	FString GaitIndices;
	int32 ResolvedGaitCount = 0;
	for (int32 GaitIndex = 0; GaitIndex < Resolved.ResolvedGaits.Num(); ++GaitIndex)
	{
		// INDEX_NONE is a gait whose sequence is absent from this bank. It can never be shown, so it is
		// nothing a pose can be trapped behind and is not a collision. Still printed below, because a
		// gait that resolves to nothing is worth seeing in a passing verdict.
		const int32 GaitSlot = Resolved.ResolvedGaits[GaitIndex].BankIndex;
		ResolvedGaitCount += GaitSlot != INDEX_NONE ? 1 : 0;

		if (!GaitIndices.IsEmpty())
		{
			GaitIndices += TEXT(", ");
		}
		GaitIndices += FString::Printf(TEXT("'%s' %d"), *AnimSet->Gaits[GaitIndex].GaitName.ToString(), GaitSlot);
	}

	// An action whose slot pair does not resolve keeps both indices at INDEX_NONE and is never
	// selected, so like an unresolved gait it cannot be what an entity comes back to.
	int32 ResolvedActionCount = 0;
	for (const FCrowdyMassResolvedAction& Action : Resolved.ResolvedActions)
	{
		ResolvedActionCount += Action.PrimaryBankIndex != INDEX_NONE ? 1 : 0;
	}

	// Every unordered pair of resolved poses, which is what the pose-against-pose walk below covers.
	const int32 PosePairingCount = ResolvedPoseCount * (ResolvedPoseCount - 1) / 2;

	FString Collisions;
	auto AppendCollision = [&Collisions](FString&& Line)
	{
		if (!Collisions.IsEmpty())
		{
			Collisions += TEXT("; ");
		}
		Collisions += MoveTemp(Line);
	};

	for (int32 PoseIndex = 0; PoseIndex < Resolved.ResolvedPoses.Num(); ++PoseIndex)
	{
		const int32 PoseSlot = Resolved.ResolvedPoses[PoseIndex].BankIndex;
		if (PoseSlot == INDEX_NONE)
		{
			continue;
		}

		// Named by both id and authored name, because whoever wired the pose up is working in ids and
		// whoever authored it is working in names, and these lines have to reach both.
		const FString PoseLabel = FString::Printf(TEXT("pose id %d ('%s') on slot %d"),
			PoseIndex + 1, *AnimSet->Poses[PoseIndex].PoseName.ToString(), PoseSlot);

		for (int32 GaitIndex = 0; GaitIndex < Resolved.ResolvedGaits.Num(); ++GaitIndex)
		{
			if (Resolved.ResolvedGaits[GaitIndex].BankIndex != PoseSlot)
			{
				continue;
			}

			AppendCollision(FString::Printf(TEXT("%s collides with gait '%s'"),
				*PoseLabel, *AnimSet->Gaits[GaitIndex].GaitName.ToString()));
		}

		// Only poses after this one, so a colliding pair is named once rather than from both ends. An
		// unresolved pose carries INDEX_NONE and so can never match a slot that resolved.
		for (int32 OtherIndex = PoseIndex + 1; OtherIndex < Resolved.ResolvedPoses.Num(); ++OtherIndex)
		{
			if (Resolved.ResolvedPoses[OtherIndex].BankIndex != PoseSlot)
			{
				continue;
			}

			AppendCollision(FString::Printf(TEXT("%s collides with pose id %d ('%s')"),
				*PoseLabel, OtherIndex + 1, *AnimSet->Poses[OtherIndex].PoseName.ToString()));
		}

		// Walked off the resolved table rather than looked up by name. The two differ exactly when an
		// action name is duplicated, and there a name search would report this row against the first
		// matching row's slots, naming a collision that is not the one in front of it.
		for (int32 ActionIndex = 0; ActionIndex < Resolved.ResolvedActions.Num(); ++ActionIndex)
		{
			const FCrowdyMassResolvedAction& Action = Resolved.ResolvedActions[ActionIndex];
			if (Action.PrimaryBankIndex == INDEX_NONE)
			{
				continue;
			}

			const bool bOnPrimary = Action.PrimaryBankIndex == PoseSlot;
			if (!bOnPrimary && Action.AlternateBankIndex != PoseSlot)
			{
				continue;
			}

			// Which half of the pair, because an action alternates between its two slots so the pushed
			// index always changes, and only one half is ever the one the pose sits on. The fix is a new
			// sequence for the pose, but whoever reads this has to be able to find the slot in the bank.
			AppendCollision(FString::Printf(TEXT("%s collides with the %s half of action '%s', whose slot pair is (%d, %d)"),
				*PoseLabel, bOnPrimary ? TEXT("primary") : TEXT("alternate"),
				*AnimSet->Actions[ActionIndex].ActionName.ToString(),
				Action.PrimaryBankIndex, Action.AlternateBankIndex));
		}
	}

	const FString Coverage = FString::Printf(
		TEXT("%d of %d authored poses resolved and were checked against %d resolved gait slots, %d pose-against-pose pairings and %d action slots (the primary and the alternate of every resolvable action, %d in all). Gait slots: %s"),
		ResolvedPoseCount, Resolved.ResolvedPoses.Num(), ResolvedGaitCount,
		PosePairingCount, ResolvedActionCount * 2, ResolvedActionCount, *GaitIndices);

	if (!Collisions.IsEmpty())
	{
		OutObserved = FString::Printf(
			TEXT("in animation set '%s' against bank '%s': %s. The renderer only reacts to the pushed index changing, so an entity leaving a pose that shares its slot composes the index it is already showing, the push is dropped, and it stays frozen on the pose with its pose id correctly cleared; where the slot is shared with an action, the action landing on it also leaves the pose unable to show itself again. Give each pose a sequence of its own in the bank. %s."),
			*AnimSet->GetName(), *Provider->GetName(), *Collisions, *Coverage);
		return false;
	}

	OutObserved = FString::Printf(
		TEXT("in animation set '%s' against bank '%s' (%d sequences), no resolved pose shares a bank slot with a gait, with another pose, or with either half of an action's slot pair, so leaving a pose always changes the pushed index and no action can take the slot a pose has to come back to. %s."),
		*AnimSet->GetName(), *Provider->GetName(), BankSequences.Num(), *Coverage);
	return true;
}

bool UCrowdyMassGateHarness::EnsureEchoPayloadTypeRegistered() const
{
	UActorUpdatePayloadRegistry* Registry = UActorUpdatePayloadRegistry::Get();
	if (!Registry)
	{
		return false;
	}

	// The same call the auto registry makes for a game's executor-declared state struct, so this payload
	// gets its wire tag the way every other one does, honouring any id override in developer settings. It
	// is a no-op once the type is registered, and it is made from here rather than at module startup so a
	// session that never runs the gates never carries the type at all.
	Registry->RegisterStructAuto(FCrowdyMassGateWireState::StaticStruct());

	FCrowdyTypeID TypeID = CROWDY_INVALID_TYPE_ID;
	return Registry->GetID(FCrowdyMassGateWireState::StaticStruct(), TypeID);
}

bool UCrowdyMassGateHarness::EncodeEcho(const FCrowdyMassGateWireState& State, const int32 TruncateTrailingByteCount, TArray<uint8>& OutBytes) const
{
	// The gates fabricate frames with no sending actor behind them, so the class id is synthetic. It only
	// has to be non-zero: zero is refused by the framing, because a frame that names no class leaves the
	// receiver deriving one from the wire struct, which is what class identity on the wire removes.
	static constexpr FCrowdyClassID GateEchoClassID = 0x6A7E0000u;

	if (!USerializationFunctionLibrary::SerializeActorState(FInstancedStruct::Make(State), GateEchoClassID, OutBytes))
	{
		return false;
	}

	if (TruncateTrailingByteCount <= 0)
	{
		return true;
	}

	if (OutBytes.Num() <= TruncateTrailingByteCount)
	{
		return false;
	}

	// Cutting the tail off a current payload rather than encoding a second, shorter struct. The two are
	// the same bytes, and this way the trailing fields carry values the assertions would notice if the
	// cut ever stopped removing them.
	OutBytes.SetNum(OutBytes.Num() - TruncateTrailingByteCount);
	return true;
}

bool UCrowdyMassGateHarness::PushEcho(const TArray<uint8>& Bytes, FInstancedStruct& OutDecoded, FString& OutWhy)
{
	UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	if (!Mass)
	{
		OutWhy = TEXT("the world lost its Crowdy Mass entity subsystem mid-run");
		return false;
	}

	if (!USerializationFunctionLibrary::DeserializeActorState(Bytes, OutDecoded))
	{
		OutWhy = FString::Printf(TEXT("%d bytes went in and the live decode path rejected them outright"), Bytes.Num());
		return false;
	}

	if (!Mass->ApplyViewState(EchoUUID, OutDecoded))
	{
		OutWhy = TEXT("the bytes decoded but the tracker no longer resolves the echo entity's id, so nothing was ingested");
		return false;
	}

	return true;
}

FCrowdyMassViewPoseFragment* UCrowdyMassGateHarness::GetEchoPose() const
{
	const UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
	if (!EntityManager || !EntityManager->IsEntityValid(EchoEntity))
	{
		return nullptr;
	}

	return EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(EchoEntity);
}

bool UCrowdyMassGateHarness::CopyEchoTraceCollisionBytes(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();

	const UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
	if (!EntityManager || !EntityManager->IsEntityValid(EchoEntity))
	{
		return false;
	}

	const FCrowdyMassTraceCollisionFragment* TraceCollision =
		EntityManager->GetFragmentDataPtr<FCrowdyMassTraceCollisionFragment>(EchoEntity);
	if (!TraceCollision)
	{
		return false;
	}

	// The whole fragment, not a chosen field or two. The claim being gated is that a pose echo touches
	// none of it, and a field-by-field comparison only ever covers the fields somebody remembered.
	OutBytes.Append(reinterpret_cast<const uint8*>(TraceCollision), sizeof(FCrowdyMassTraceCollisionFragment));
	return true;
}

void UCrowdyMassGateHarness::PoisonEchoPose()
{
	FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
	if (!Pose)
	{
		return;
	}

	// A value no echo below ever sends, so the next gate's assertion cannot be satisfied by an entity
	// nothing had ever written to. Without this a gate asserting a decoded pose would keep passing with
	// the whole decode path deleted.
	Pose->PoseId = CrowdyMassGate::PoisonPoseId;
}

bool UCrowdyMassGateHarness::ReadRendererTrackCounts(int32& OutTrackCount, int32& OutLiveInstanceCount) const
{
	OutTrackCount = 0;
	OutLiveInstanceCount = 0;

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	bool bFoundSharedData = false;
	for (const UMassRepresentationSubsystem* RepresentationSubsystem : World->GetSubsystemArrayCopy<UMassRepresentationSubsystem>())
	{
		const UMassVisualizationComponent* VisualizationComponent =
			RepresentationSubsystem ? RepresentationSubsystem->GetVisualizationComponent() : nullptr;
		if (!VisualizationComponent)
		{
			continue;
		}

		// The shared data entries are stored contiguously, so the first index that resolves to nothing is
		// the end of the list.
		for (int32 DescriptionIndex = 0; ; ++DescriptionIndex)
		{
			const FMassInstancedSkinnedMeshComponentSharedData* SharedData =
				VisualizationComponent->GetSharedDataForDescriptionIndex(DescriptionIndex);
			if (!SharedData)
			{
				break;
			}

			bFoundSharedData = true;
			OutTrackCount += SharedData->GetEntityToTrackMap().Num();
			OutLiveInstanceCount += SharedData->GetEntityToPrimitiveIdMap().Num();
		}
	}

	// False is not "zero tracks". It means nothing in this world renders through the instanced skinned
	// mesh path at all, so there is no count to watch and no reading to report.
	return bFoundSharedData;
}

void UCrowdyMassGateHarness::SkipEveryEchoGate(const FString& Why)
{
	Skip(CrowdyMassGate::GateE1, Why);
	Skip(CrowdyMassGate::GateE2, Why);
	Skip(CrowdyMassGate::GateE3, Why);
	Skip(CrowdyMassGate::GateE4, Why);
	Skip(CrowdyMassGate::GateE5, Why);
	Skip(CrowdyMassGate::GateE6, Why);
	Skip(CrowdyMassGate::GateE7, Why);
}

void UCrowdyMassGateHarness::SkipEveryStateGate(const FString& Why)
{
	Skip(CrowdyMassGate::GateS1, Why);
	Skip(CrowdyMassGate::GateS2, Why);
	Skip(CrowdyMassGate::GateS3, Why);
	Skip(CrowdyMassGate::GateS4, Why);
}

bool UCrowdyMassGateHarness::SpawnStateEntity(FString& OutWhy)
{
#if UE_BUILD_SHIPPING
	// These gates pose through an animation set authored here rather than by a project, and the only way
	// to hand a class one is the development-only bind below, which a Shipping build does not carry.
	// Refused up front rather than left to fail further down, so the reason is the one a reader needs.
	OutWhy = TEXT("the state gates author their own animation set and bind it through a development seam a Shipping build does not carry");
	return false;
#else
	UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	UWorld* World = GetWorld();
	if (!Mass || !World)
	{
		OutWhy = TEXT("the world lost its Crowdy Mass entity subsystem mid-run");
		return false;
	}

	// Checked rather than assumed, because the receive path resolves a participant BEFORE it ever asks
	// the crowd, and it does so silently: an id that has one takes the object road, which is a different
	// road that works, so these gates would report a healthy number for a path they never touched. The id
	// is derived and used nowhere else, so this can only fire if something started claiming it.
	const UCrowdyEntitySubsystem* Entities = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (Entities && Entities->FindParticipant(StateUUID))
	{
		OutWhy = TEXT("something in this session holds a participant for the id these gates use, and a delta addressed to an id with a participant is applied to that object instead, so these gates would be measuring the object road while claiming the crowd one");
		return false;
	}

	// A delta this client sent is dropped before anything else runs, which is correct and is why the
	// sender has to be a peer. An id equal to this client's own would make every gate below report a
	// clean zero, which reads exactly like the path being broken.
	if (Entities && Entities->GetLocalPlayerID().IsValid() && Entities->GetLocalPlayerID() == StateSenderID)
	{
		OutWhy = TEXT("the synthetic peer these deltas name collided with this client's own player id, so every one of them would be dropped as an echo of ourselves");
		return false;
	}

	FVector Origin = FVector::ZeroVector;
	if (const APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			Origin = Pawn->GetActorLocation();
		}
	}

	// Offset upward, on the one axis neither the action entities nor the echo entity use, so no query any
	// other gate makes can return this one.
	const FVector Location = Origin + FVector(0.0f, 0.0f, CrowdyMassGate::EntitySpacingCentimetres);

	// Spawned from this plugin's own template rather than from whatever Mass entity config the project
	// configured, and told which class its state arrives as. Every other gate here spawns from that
	// config, so in a real project they measure a game's traits and assets as much as this code; this one
	// depends on nothing a project authored, which is what lets its verdict be a statement about the
	// plugin.
	// The set the gate class poses through, authored here rather than read from the project. The gates
	// below assert which ROW a stance selects, so they need rows whose order they chose: a set a project
	// authored could put the rows in the order that makes a row id equal its stance value, and the gates
	// would then pass just as well against a receive path that copied the arriving byte through.
	//
	// Named through PoseStateEnumName rather than assigned to PoseStateEnum, because ECrowdyMassGateStance
	// is declared in C++ and is therefore not an asset the object property could ever hold.
	UCrowdyMassAnimSet* GateAnimSet = NewObject<UCrowdyMassAnimSet>(this, NAME_None, RF_Transient);
	GateAnimSet->PoseStateEnumName = TEXT("ECrowdyMassGateStance");
	for (const FName& PoseRowName : CrowdyMassGate::StatePoseRowNames)
	{
		FCrowdyMassPoseEntry Pose;
		Pose.PoseName = PoseRowName;
		GateAnimSet->Poses.Add(Pose);
	}

	// Bound before the spawn, because spawning resolves the class's set and would otherwise cache the
	// "this class names nothing" answer that every delta below then reads.
	Mass->BindDevAnimSetForClass(UCrowdyMassGateStateEntity::StaticClass(), GateAnimSet);
	GateAnimSetKeepAlive = GateAnimSet;

	StateEntity = Mass->SpawnRemoteEntity(StateUUID, FTransform(FRotator::ZeroRotator, Location),
		FInstancedStruct(), UCrowdyMassGateStateEntity::StaticClass(),
		ECrowdyMassRemoteEntitySource::PluginDefaults);
	if (!StateEntity.IsSet())
	{
		OutWhy = TEXT("the entity failed to spawn from this plugin's own template, so there was nothing for a decoded delta to land on");
		return false;
	}

	if (!GetStatePose())
	{
		OutWhy = TEXT("the entity spawned from this plugin's own template carries no view pose fragment, so an ingested pose has nowhere to land");
		return false;
	}

	if (Mass->GetEntityClass(StateUUID) != UCrowdyMassGateStateEntity::StaticClass())
	{
		OutWhy = TEXT("the spawn did not record the class these deltas are encoded as, so the receive path has nothing to read their bytes against and refuses every one of them");
		return false;
	}

	return true;
#endif
}

FCrowdyMassViewPoseFragment* UCrowdyMassGateHarness::GetStatePose() const
{
	const UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
	if (!EntityManager || !EntityManager->IsEntityValid(StateEntity))
	{
		return nullptr;
	}

	return EntityManager->GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(StateEntity);
}

void UCrowdyMassGateHarness::PoisonStatePose()
{
	FCrowdyMassViewPoseFragment* Pose = GetStatePose();
	if (!Pose)
	{
		return;
	}

	// The same reasoning as the echo entity's poisoning, and it matters more here: both roads write this
	// one byte with the same kind of value, so a gate that read a plausible pose off an entity nothing
	// had written to could not tell an arriving delta from an untouched fragment.
	Pose->PoseId = CrowdyMassGate::PoisonPoseId;
}

bool UCrowdyMassGateHarness::ReadStateCounters(FCrowdyMassGateStateCounters& OutCounters) const
{
	const UCrowdyEventRouter* Router = GetRouter();
	const UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	if (!Router || !Mass)
	{
		return false;
	}

	const UCrowdyEventRouter::FStateSubscriberStats& Stats = Router->GetStateSubscriberStats();
	OutCounters.Claimed = Stats.Claimed;
	OutCounters.Applied = Stats.Applied;
	OutCounters.DeliveredSlots = Stats.DeliveredSlots;
	OutCounters.NotClaimed = Stats.NotClaimed;
	OutCounters.DroppedNoEntityClass = Stats.DroppedNoEntityClass;
	OutCounters.DroppedClassIdMismatch = Stats.DroppedClassIdMismatch;
	OutCounters.DroppedNoLayout = Stats.DroppedNoLayout;
	OutCounters.DroppedNoScratch = Stats.DroppedNoScratch;
	OutCounters.DroppedDecodeFailed = Stats.DroppedDecodeFailed;
	OutCounters.PoseWritesFromState = Mass->PoseWritesFromState;
	return true;
}

bool UCrowdyMassGateHarness::DispatchStateDelta(TConstArrayView<FCrowdyMassStateDeltaByteField> Fields, FString& OutWhy)
{
	UCrowdyEventRouter* Router = GetRouter();
	UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	const UGameInstance* GameInstance = GetGameInstance();
	UCrowdyAutoRegistry* Registry = GameInstance ? GameInstance->GetSubsystem<UCrowdyAutoRegistry>() : nullptr;
	if (!Router || !Mass || !Registry)
	{
		OutWhy = TEXT("this world has no event router, no Crowdy Mass entity subsystem or no registry to resolve a layout from");
		return false;
	}

	// Asked of the same source the receive path's own guard reads, rather than named here, so a delta
	// this builds and the entity it is addressed to cannot disagree about the class. Encoding as some
	// class the harness picked instead is the mistake that makes every delta refused for a reason that
	// looks like the guard being broken.
	const UClass* EntityClass = Mass->GetEntityClass(StateUUID);
	if (!EntityClass)
	{
		OutWhy = TEXT("the subsystem records no class for the state entity, so the receive path has nothing to read a delta's bytes against and refuses every one of them");
		return false;
	}

	// The same registry the receive path resolves its layout from, for the same reason.
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(EntityClass);
	if (!Layout)
	{
		// Worth naming the cooked case, because it is the likely one and it is not a defect. A layout is
		// built live from reflection only where property metadata survives; a packaged build reads the
		// baked table instead, and a class added since that table was last generated is simply absent
		// from it.
		OutWhy = FString::Printf(TEXT("'%s' resolves to no CrowdyState layout, so it carries no replicated property for a delta to be built from. In a packaged build this is what a stale baked registry looks like, and regenerating it picks the class up"), *EntityClass->GetName());
		return false;
	}

	StateLayoutSlotCount = Layout->Properties.Num();

	FCrowdyStateDelta Delta;
	if (!FCrowdyMassStateDeltaBuilder::Build(*EntityClass, *Layout, StateUUID, StateSenderID, Fields, Delta, OutWhy))
	{
		return false;
	}

	// Handed to the router as an inbound event rather than written anywhere directly, so the claim, the
	// class guard, the shared decode buffer, the decode itself and the delivery all run. A rig that wrote
	// the fragment would prove only that a fragment can be written.
	// The event borrows its payload, so the owner is a local that outlives the dispatch and no longer.
	const FInstancedStruct Payload = FInstancedStruct::Make(Delta);

	FCrowdyInboundEvent Inbound;
	Inbound.Payload = &Payload;
	Inbound.SenderID = Delta.SenderID;
	Router->DispatchEvent(Inbound);
	return true;
}

bool UCrowdyMassGateHarness::ReadAction(const int32 Index, int32& OutActionId, int32& OutSerial, int32& OutAppliedSerial) const
{
	const UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
	if (!Mass || !EntityHandles.IsValidIndex(Index))
	{
		return false;
	}

	FMassEntityManager* EntityManager = Mass->GetMassEntityManager();
	if (!EntityManager || !EntityManager->IsEntityValid(EntityHandles[Index]))
	{
		return false;
	}

	const FCrowdyMassActionFragment* Action =
		EntityManager->GetFragmentDataPtr<FCrowdyMassActionFragment>(EntityHandles[Index]);
	if (!Action)
	{
		return false;
	}

	OutActionId = Action->ActionId;
	OutSerial = Action->Serial;
	OutAppliedSerial = Action->AppliedSerial;
	return true;
}

bool UCrowdyMassGateHarness::InjectInboundForUnknownEntity(const FGuid& UUID)
{
	UCrowdyEventRouter* Router = GetRouter();
	UFunction* Fn = UCrowdyMassCharacterAvatar::StaticClass()->FindFunctionByName(TEXT("PlayOneShot_Implementation"));
	if (!Router || !Fn)
	{
		return false;
	}

	// Built with the real routing identity rather than placeholder numbers, because that identity decides
	// the deferral budget: an event whose function cannot be resolved is not classified as an action and
	// keeps the ordinary six-hundred-retry wait, which is precisely the case the stale gate must not
	// accidentally measure instead.
	FCrowdyRpcCall Call;
	Call.ClassID = UCrowdyClassRegistry::Get()->GetID(UCrowdyMassCharacterAvatar::StaticClass());
	Call.FunctionID = FCrowdyRPC::ComputeFunctionID(Fn);
	Call.EntityID = UUID;

	// The event borrows its payload, so the owner is a local that outlives the dispatch and no longer. The
	// wait queue takes its own copy, which is what lets the retry run frames later against a dead local.
	const FInstancedStruct Payload = FInstancedStruct::Make(Call);

	FCrowdyInboundEvent Event;
	Event.Payload = &Payload;
	Event.SenderID = FGuid(0x0C0DEFFF, 1, 2, 3);

	Router->DispatchEvent(Event);
	return true;
}

bool UCrowdyMassGateHarness::Tick(float DeltaSeconds)
{
	// Checked before any step runs, because a Mass entity handle is not unique across worlds: serial
	// numbers restart at zero in each world's entity manager, so every handle this run is holding stays
	// valid in the next world and resolves to whatever entity happens to occupy it. The steps below write
	// through those handles, stamping ownership and pose values, so a run that carried on would be
	// modifying live entities in a level it never measured and reporting the result as a verdict about
	// this one. Abandoned rather than adapted: the readings taken before the change describe a world that
	// is gone, and there is nothing to compare them against.
	if (Step != ECrowdyMassGateStep::Idle && GetWorld() != RunWorld.Get())
	{
		Record(TEXT("Setup: the run stayed in one world"), false,
			TEXT("the world changed while this run was in progress, so every reading taken before that point describes a world that no longer exists and every entity handle still held now names an unrelated entity. The run was abandoned rather than finished against the new world."));
		PrintReport();
		StopGates();
		return true;
	}

	switch (Step)
	{
	case ECrowdyMassGateStep::Idle:
		return true;

	case ECrowdyMassGateStep::Spawn:
	{
		bool bAllSpawned = true;
		for (int32 Index = 0; Index < CrowdyMassGate::GateEntityCount; ++Index)
		{
			const FGuid UUID = CrowdyMassGate::MakeGateGuid(Index);
			EntityUUIDs.Add(UUID);
			bAllSpawned &= SpawnGateEntity(Index, UUID);
		}

		if (!bAllSpawned)
		{
			const FString Why = TEXT("the run's three owned entities could not all be spawned from the configured Mass entity config, or one carried no identity or action fragment");
			Record(TEXT("Setup: three owned Mass entities"), false,
				FString::Printf(TEXT("%s. Every gate that reads those fragments is reported as not attempted below rather than left out of this report."), *Why));

			// Named one by one rather than jumped over. A gate missing from a report reads as a gate that
			// was not needed, and a reader counting passes and failures has no way to notice the
			// difference; the whole point of a report is that its silence means something.
			SkipEveryEchoGate(Why);
			Skip(CrowdyMassGate::GateL1, Why);
			Skip(CrowdyMassGate::GateD1, Why);

			// The CrowdyState gates carry on, because none of this is a precondition of theirs: they spawn
			// their own entity from this plugin's own template, under their own id and their own class, so
			// a project whose configured entity config is wrong or missing is exactly the case they are
			// still able to answer.
			Advance(ECrowdyMassGateStep::StateSpawn, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Record(TEXT("Setup: three owned Mass entities"), true,
			FString::Printf(TEXT("spawned %d, stamped Role=Owner"), CrowdyMassGate::GateEntityCount));
		Advance(ECrowdyMassGateStep::FireOwned, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::FireOwned:
	{
		int32 ActionId = 0;
		int32 Serial = 0;
		int32 AppliedSerial = 0;
		if (!ReadAction(CrowdyMassGate::FiredEntityIndex, ActionId, Serial, AppliedSerial))
		{
			Record(TEXT("G1 loopback round trip"), false, TEXT("the fired-at entity has no action fragment to read."));
			Advance(ECrowdyMassGateStep::DeferInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		SerialAtFire = Serial;

		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		UCrowdyMassCharacterAvatar* Avatar = Cast<UCrowdyMassCharacterAvatar>(
			Mass->GetOrCreateAvatar(EntityUUIDs[CrowdyMassGate::FiredEntityIndex], UCrowdyMassCharacterAvatar::StaticClass()));
		if (!Avatar)
		{
			Record(TEXT("G1 loopback round trip"), false,
				TEXT("no avatar resolved for a live, tracked entity, so the event had no receiver to send from."));
			Advance(ECrowdyMassGateStep::DeferInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Avatar->PlayOneShot(CrowdyMassGate::GateActionId, 0, 1.0f);
		Advance(ECrowdyMassGateStep::ObserveWrite, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::ObserveWrite:
	{
		int32 ActionId = 0;
		int32 Serial = 0;
		int32 AppliedSerial = 0;
		const bool bRead = ReadAction(CrowdyMassGate::FiredEntityIndex, ActionId, Serial, AppliedSerial);

		if (bRead && ActionId == CrowdyMassGate::GateActionId && Serial == SerialAtFire + 1)
		{
			Record(TEXT("G1 loopback round trip"), true,
				FString::Printf(TEXT("ActionId %d written and Serial advanced %d -> %d through the receive path, so serialize, route, avatar resolve and dispatch all ran"),
					ActionId, SerialAtFire, Serial));

			// Read now rather than later: the neighbours must be untouched at the moment the fired entity
			// has its action, not merely by the time everything has expired.
			int32 NeighbourActions = 0;
			for (int32 Index = 0; Index < CrowdyMassGate::GateEntityCount; ++Index)
			{
				if (Index == CrowdyMassGate::FiredEntityIndex)
				{
					continue;
				}

				int32 OtherAction = 0;
				int32 OtherSerial = 0;
				int32 OtherApplied = 0;
				if (ReadAction(Index, OtherAction, OtherSerial, OtherApplied) && OtherAction != 0)
				{
					++NeighbourActions;
				}
			}

			Record(TEXT("G3a one entity, not the archetype"), NeighbourActions == 0,
				NeighbourActions == 0
					? TEXT("both neighbouring entities still read ActionId 0")
					: FString::Printf(TEXT("%d neighbouring entities also took the action"), NeighbourActions));

			Record(TEXT("G3b no double run"), Serial == SerialAtFire + 1,
				FString::Printf(TEXT("one send advanced Serial by %d"), Serial - SerialAtFire));

			Advance(ECrowdyMassGateStep::ObserveApplied, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			Record(TEXT("G1 loopback round trip"), false, bRead
				? FString::Printf(TEXT("after %.0f s the fragment still reads ActionId %d, Serial %d (expected %d and %d)"),
					StepTimeoutSeconds, ActionId, Serial, CrowdyMassGate::GateActionId, SerialAtFire + 1)
				: TEXT("the entity's action fragment could not be read at all"));
			Skip(TEXT("G3a one entity, not the archetype"), TEXT("nothing was written, so there was no write to localise"));
			Skip(TEXT("G3b no double run"), TEXT("nothing was written, so there was no run to count"));
			Advance(ECrowdyMassGateStep::DeferInject, CrowdyMassGate::ShortStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::ObserveApplied:
	{
		int32 ActionId = 0;
		int32 Serial = 0;
		int32 AppliedSerial = 0;
		const bool bRead = ReadAction(CrowdyMassGate::FiredEntityIndex, ActionId, Serial, AppliedSerial);

		if (bRead && ActionId != 0 && AppliedSerial == Serial)
		{
			bActionWasApplied = true;
			Record(TEXT("G1b handed to the renderer"), true,
				FString::Printf(TEXT("AppliedSerial reached %d, so the animation pass composed the action and pushed a sequence index change"), AppliedSerial));
			Advance(ECrowdyMassGateStep::ObserveRevert, CrowdyMassGate::StaleStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			// Deliberately not a failure. An entity with no anim set, or no authored action for this id,
			// legitimately never shows one, and saying so is more useful than a red mark that sends the
			// reader looking at the event path the gate above already proved works.
			Skip(TEXT("G1b handed to the renderer"),
				FString::Printf(TEXT("written but never applied within %.0f s (AppliedSerial %d against Serial %d). Expected when no anim set is assigned or action id %d is unauthored; the event path itself is already proven by G1."),
					StepTimeoutSeconds, AppliedSerial, Serial, CrowdyMassGate::GateActionId));
			Advance(ECrowdyMassGateStep::ObserveRevert, CrowdyMassGate::StaleStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::ObserveRevert:
	{
		int32 ActionId = 0;
		int32 Serial = 0;
		int32 AppliedSerial = 0;
		const bool bRead = ReadAction(CrowdyMassGate::FiredEntityIndex, ActionId, Serial, AppliedSerial);

		if (bRead && ActionId == 0)
		{
			Record(TEXT("G1c reverts to locomotion"), true,
				bActionWasApplied
					? TEXT("ActionId returned to 0 after the action's own duration")
					: TEXT("ActionId returned to 0 on the unshown-action grace, which is the correct path for an action that never resolved"));
			Advance(ECrowdyMassGateStep::DeferInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			Record(TEXT("G1c reverts to locomotion"), false,
				FString::Printf(TEXT("ActionId was still %d after %.0f s, so the action never expired and the entity is stuck on it"),
					ActionId, StepTimeoutSeconds));
			Advance(ECrowdyMassGateStep::DeferInject, CrowdyMassGate::ShortStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::DeferInject:
	{
		UCrowdyEventRouter* Router = GetRouter();
		if (!Router)
		{
			Skip(TEXT("G2a defers before spawn"), TEXT("no event router in this world"));
			Skip(TEXT("G2b stale action drops"), TEXT("no event router in this world"));
			Advance(ECrowdyMassGateStep::AuthorityRefuse, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		DeferredAtInject = Router->NumDeferredForTest();
		if (!InjectInboundForUnknownEntity(DeferUUID))
		{
			Skip(TEXT("G2a defers before spawn"), TEXT("the inbound event could not be built (PlayOneShot_Implementation did not resolve)"));
			Advance(ECrowdyMassGateStep::StaleInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const int32 Now = Router->NumDeferredForTest();
		if (Now <= DeferredAtInject)
		{
			Record(TEXT("G2a defers before spawn"), false,
				FString::Printf(TEXT("an event for an unspawned entity was DROPPED rather than queued (deferred %d -> %d), so it could never arrive once the entity spawns"),
					DeferredAtInject, Now));
			Advance(ECrowdyMassGateStep::StaleInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Advance(ECrowdyMassGateStep::DeferSpawn, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::DeferSpawn:
	{
		UCrowdyEventRouter* Router = GetRouter();
		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		if (!Router || !Mass)
		{
			Skip(TEXT("G2a defers before spawn"), TEXT("the world lost its subsystems mid-run"));
			Advance(ECrowdyMassGateStep::StaleInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Spawning the id the queued event names is what should let the retry pass resolve it. The gate is
		// that the queue DRAINS, which is the difference between an event that waited and one that was
		// quietly discarded; whether the body then decoded its parameters is covered headlessly.
		const int32 SpawnIndex = EntityUUIDs.Num();
		EntityUUIDs.Add(DeferUUID);
		SpawnGateEntity(SpawnIndex, DeferUUID);
		Router->RetryDeferredForTest();

		const int32 Now = Router->NumDeferredForTest();
		if (Now <= DeferredAtInject)
		{
			Record(TEXT("G2a defers before spawn"), true,
				FString::Printf(TEXT("the event waited while the entity did not exist, then drained on the retry once it spawned (deferred %d -> %d)"),
					DeferredAtInject + 1, Now));
			Advance(ECrowdyMassGateStep::StaleInject, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			Record(TEXT("G2a defers before spawn"), false,
				FString::Printf(TEXT("the entity spawned but the event was still queued %.0f s later (deferred count %d), so it waited and then did not fire"),
					StepTimeoutSeconds, Now));
			Advance(ECrowdyMassGateStep::StaleInject, CrowdyMassGate::ShortStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::StaleInject:
	{
		UCrowdyEventRouter* Router = GetRouter();
		if (!Router)
		{
			Skip(TEXT("G2b stale action drops"), TEXT("no event router in this world"));
			Advance(ECrowdyMassGateStep::AuthorityRefuse, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		EvictionsAtInject = Router->NumDeferredEvictionsForTest();
		if (!InjectInboundForUnknownEntity(StaleUUID))
		{
			Skip(TEXT("G2b stale action drops"), TEXT("the inbound event could not be built"));
			Advance(ECrowdyMassGateStep::AuthorityRefuse, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Advance(ECrowdyMassGateStep::StaleObserve, CrowdyMassGate::StaleStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::StaleObserve:
	{
		UCrowdyEventRouter* Router = GetRouter();
		if (!Router)
		{
			Skip(TEXT("G2b stale action drops"), TEXT("no event router in this world"));
			Advance(ECrowdyMassGateStep::AuthorityRefuse, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// The retry pass is what walks deadlines, and it runs on the router's own tick; nudging it here
		// keeps the gate from depending on that tick rate.
		Router->RetryDeferredForTest();

		if (Router->NumDeferredEvictionsForTest() > EvictionsAtInject)
		{
			Record(TEXT("G2b stale action drops"), true,
				FString::Printf(TEXT("an action for an entity that never spawned was evicted on its deadline after %.1f s, rather than waiting out the ordinary retry budget"),
					FPlatformTime::Seconds() - StepStartedSeconds));
			Advance(ECrowdyMassGateStep::AuthorityRefuse, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			Record(TEXT("G2b stale action drops"), false,
				FString::Printf(TEXT("after %.0f s the action was still queued and no eviction was counted, so it is on the ordinary retry budget and a stale swing can still fire seconds late on a respawned entity"),
					StepTimeoutSeconds));
			Advance(ECrowdyMassGateStep::AuthorityRefuse, CrowdyMassGate::ShortStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::AuthorityRefuse:
	{
		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
		constexpr int32 Index = CrowdyMassGate::ForeignOwnerIndex;
		if (!EntityManager || !EntityHandles.IsValidIndex(Index))
		{
			Skip(TEXT("G4 refuses to speak for another client's entity"), TEXT("no spare entity to re-stamp"));
			Advance(ECrowdyMassGateStep::EchoSpawn, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		FCrowdyRemoteIdentityFragment* Identity =
			EntityManager->GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(EntityHandles[Index]);
		if (!Identity)
		{
			Skip(TEXT("G4 refuses to speak for another client's entity"), TEXT("the spare entity has no identity fragment"));
			Advance(ECrowdyMassGateStep::EchoSpawn, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// RemoteProxy with a foreign owner: an entity somebody else simulates and sends for, which is what
		// every crowd member is from this client's point of view, and the case the send path must refuse.
		//
		// Role is what carries that meaning, not OwnerID. The enum defines Owner as "this client simulates
		// and sends" and RemoteProxy as "receives state and events from the network", so stamping Owner
		// with another player's id would be a self-contradictory state rather than a hostile one, and a
		// gate built on it would be testing something production cannot produce.
		Identity->OwnerID = FGuid(0x0BADC0DE, 1, 2, 3);
		Identity->Role = ECrowdyRole::RemoteProxy;

		int32 BeforeAction = 0;
		int32 BeforeSerial = 0;
		int32 BeforeApplied = 0;
		ReadAction(Index, BeforeAction, BeforeSerial, BeforeApplied);

		if (UCrowdyMassCharacterAvatar* Avatar = Cast<UCrowdyMassCharacterAvatar>(
			Mass->GetOrCreateAvatar(EntityUUIDs[Index], UCrowdyMassCharacterAvatar::StaticClass())))
		{
			Avatar->PlayOneShot(CrowdyMassGate::GateActionId, 0, 1.0f);
		}

		int32 AfterAction = 0;
		int32 AfterSerial = 0;
		int32 AfterApplied = 0;
		ReadAction(Index, AfterAction, AfterSerial, AfterApplied);

		Record(TEXT("G4 refuses to speak for another client's entity"), AfterSerial == BeforeSerial,
			AfterSerial == BeforeSerial
				? TEXT("the send was refused and nothing was written, so a client cannot originate events for an entity it does not own")
				: FString::Printf(TEXT("the send went through for an entity owned by another client (Serial %d -> %d)"), BeforeSerial, AfterSerial));

		Advance(ECrowdyMassGateStep::EchoSpawn, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoSpawn:
	{
		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		UWorld* World = GetWorld();
		if (!Mass || !World)
		{
			SkipEveryEchoGate(TEXT("the world lost its Crowdy Mass entity subsystem mid-run"));
			Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (!EnsureEchoPayloadTypeRegistered())
		{
			SkipEveryEchoGate(TEXT("the harness wire payload has no type tag (another struct already holds the id it derives), so nothing can be serialised and there is no decode to measure"));
			Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// The pose ingestor these gates measure is registered HERE, by the harness, through the same public
		// call a consuming game uses for its own fields. CrowdyMass itself registers nothing but Version on
		// this plane: its standard pose field was deleted in Phase A.4, because a byte on the echo is a row
		// id the SENDER chose and the pose now travels as the game's own enum value on the CrowdyState
		// plane instead.
		//
		// So what E1 to E7 measure is the echo MECHANISM (decode, ingest, fail-safe defaults, forged input,
		// clearing) exercised through the public surface, which is what that mechanism is now for. Guarded
		// on the name because registrations cannot be removed and a second one would be silently dead.
		if (!Mass->IsViewStateFieldRegistered(CrowdyMassGate::EchoPoseFieldName))
		{
			Mass->RegisterViewStateField(CrowdyMassGate::EchoPoseFieldName,
				[](const void* PropertyValuePtr, const FCrowdyMassViewStateContext& Context)
				{
					FCrowdyMassViewPoseFragment* PoseFragment = Context.GetFragment<FCrowdyMassViewPoseFragment>();
					if (!PoseFragment)
					{
						return;
					}

					// Stored exactly as sent, with no clamp. An id past the anim set's rows is caught where
					// it is consumed, which is what E7 asserts: clamping here would turn a forged id into a
					// plausible wrong pose instead of no pose.
					PoseFragment->PoseId = *static_cast<const uint8*>(PropertyValuePtr);
				});
		}

		FVector Origin = FVector::ZeroVector;
		if (const APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			if (const APawn* Pawn = PlayerController->GetPawn())
			{
				Origin = Pawn->GetActorLocation();
			}
		}

		// Offset along a different axis from the action entities, so the ray the trace-grid gate fires
		// cannot pick one of those up and read as this entity still being there.
		EchoLocation = Origin + FVector(0.0f, CrowdyMassGate::EntitySpacingCentimetres, 0.0f);

		EchoEntity = Mass->SpawnRemoteEntity(EchoUUID, FTransform(FRotator::ZeroRotator, EchoLocation));
		if (!EchoEntity.IsSet())
		{
			SkipEveryEchoGate(TEXT("the echo entity failed to spawn, so there was nothing for a decoded payload to land on"));
			Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (!GetEchoPose())
		{
			SkipEveryEchoGate(TEXT("the remote entity config carries no view pose fragment, so an ingested echo has nowhere to land. Add the Crowdy Remote Entity trait to it."));
			Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Read here only to establish that the fragment exists at all, which is a precondition of the gate
		// below rather than part of what it measures. That gate takes its own reading immediately before it
		// sends, and that is the one it compares against.
		CopyEchoTraceCollisionBytes(EchoTraceCollisionBefore);

		Record(TEXT("Setup: one entity to push view-state echoes at"), true,
			FString::Printf(TEXT("spawned entity %s carrying a view pose fragment"), *EchoUUID.ToString()));
		Advance(ECrowdyMassGateStep::EchoPoseApply, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoPoseApply:
	{
		PoisonEchoPose();

		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;
		State.PoseId = CrowdyMassGate::FirstEchoPoseId;

		TArray<uint8> Bytes;
		if (!EncodeEcho(State, 0, Bytes))
		{
			Skip(CrowdyMassGate::GateE1, TEXT("the payload could not be encoded, so there was nothing to decode"));
			Advance(ECrowdyMassGateStep::EchoPoseChange, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		FInstancedStruct Decoded;
		FString Why;
		if (!PushEcho(Bytes, Decoded, Why))
		{
			Record(CrowdyMassGate::GateE1, false, Why);
			Advance(ECrowdyMassGateStep::EchoPoseChange, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		if (!Pose)
		{
			Skip(CrowdyMassGate::GateE1, TEXT("the echo entity's view pose fragment went away between the push and the read"));
			Advance(ECrowdyMassGateStep::EchoPoseChange, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		EchoPoseAfterApply = Pose->PoseId;
		const bool bPassed = EchoPoseAfterApply == CrowdyMassGate::FirstEchoPoseId;

		// The byte count is reported because it is the only thing separating this from a direct fragment
		// write: these bytes went through the encoder and back through the decoder the receive path uses,
		// which is what proves the live field-registration table resolved the field by its name.
		Record(CrowdyMassGate::GateE1, bPassed,
			FString::Printf(TEXT("%d bytes through serialise, decode and ingest left pose id %d on an entity poisoned to %d beforehand (expected %d)"),
				Bytes.Num(), EchoPoseAfterApply, CrowdyMassGate::PoisonPoseId, CrowdyMassGate::FirstEchoPoseId));

		Advance(ECrowdyMassGateStep::EchoPoseChange, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoPoseChange:
	{
		if (EchoPoseAfterApply == 0)
		{
			Skip(CrowdyMassGate::GateE2, TEXT("the first pose echo never landed, so there is no pose for this one to have moved away from"));
			Advance(ECrowdyMassGateStep::EchoPoseSet, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Deliberately not poisoned first. This gate's claim is that a second echo MOVES the value the
		// first one left, so it has to run against exactly what the first one left; overwriting that would
		// make this a repeat of the gate above rather than the half that catches a write-once ingestor.
		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;
		State.PoseId = CrowdyMassGate::SecondEchoPoseId;

		TArray<uint8> Bytes;
		FInstancedStruct Decoded;
		FString Why;
		if (!EncodeEcho(State, 0, Bytes) || !PushEcho(Bytes, Decoded, Why))
		{
			Record(CrowdyMassGate::GateE2, false, Why.IsEmpty() ? FString(TEXT("the payload could not be encoded")) : Why);
			Advance(ECrowdyMassGateStep::EchoPoseSet, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		if (!Pose)
		{
			Skip(CrowdyMassGate::GateE2, TEXT("the echo entity's view pose fragment went away between the push and the read"));
			Advance(ECrowdyMassGateStep::EchoPoseSet, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const bool bPassed = Pose->PoseId != EchoPoseAfterApply && Pose->PoseId == CrowdyMassGate::SecondEchoPoseId;

		Record(CrowdyMassGate::GateE2, bPassed,
			FString::Printf(TEXT("pose byte %d decoded to pose id %d, from the %d the previous echo left (expected a move to %d)"),
				State.PoseId, Pose->PoseId, EchoPoseAfterApply, CrowdyMassGate::SecondEchoPoseId));

		Advance(ECrowdyMassGateStep::EchoPoseSet, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoPoseSet:
	{
		if (EchoTraceCollisionBefore.IsEmpty())
		{
			Skip(CrowdyMassGate::GateE3, TEXT("the echo entity carries no trace collision fragment, so there is nothing here for a pose echo to be shown leaving alone. The rest of this gate is not worth reporting without it."));
			Advance(ECrowdyMassGateStep::EchoPoseCleared, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Re-read here rather than reusing the reading taken at spawn, so this gate compares across this
		// echo and nothing else. Comparing against a reading from before the earlier echoes would let one
		// of those break trace collision and have this gate report it as this echo having done it.
		CopyEchoTraceCollisionBytes(EchoTraceCollisionBefore);

		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;
		State.PoseId = CrowdyMassGate::FirstEchoPoseId;

		TArray<uint8> Bytes;
		FInstancedStruct Decoded;
		FString Why;
		if (!EncodeEcho(State, 0, Bytes) || !PushEcho(Bytes, Decoded, Why))
		{
			Record(CrowdyMassGate::GateE3, false, Why.IsEmpty() ? FString(TEXT("the payload could not be encoded")) : Why);
			Advance(ECrowdyMassGateStep::EchoPoseCleared, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		if (!Pose)
		{
			Skip(CrowdyMassGate::GateE3, TEXT("the echo entity's view pose fragment went away between the push and the read"));
			Advance(ECrowdyMassGateStep::EchoPoseCleared, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
		const bool bStillValid = EntityManager && EntityManager->IsEntityValid(EchoEntity);

		FMassEntityHandle StillTracked;
		const bool bStillTracked = Mass && Mass->FindEntityByUUID(EchoUUID, StillTracked) && StillTracked == EchoEntity;

		TArray<uint8> TraceCollisionAfter;
		const bool bCopied = CopyEchoTraceCollisionBytes(TraceCollisionAfter);
		const bool bCollisionUnchanged = bCopied && TraceCollisionAfter == EchoTraceCollisionBefore;

		const bool bPassed = Pose->PoseId == CrowdyMassGate::FirstEchoPoseId && bStillValid && bStillTracked && bCollisionUnchanged;

		// All four readings are reported, passing or failing. The last three are the security posture and
		// the reason this gate exists: a forgeable byte may choose a pose, and it may not decide whether an
		// entity exists, whether its id still resolves, or whether it can be hit.
		Record(CrowdyMassGate::GateE3, bPassed,
			FString::Printf(TEXT("pose id landed: %d (expected %d). Entity still valid: %s. Tracker still resolves the same entity: %s. Trace collision unchanged across %d bytes: %s."),
				Pose->PoseId, CrowdyMassGate::FirstEchoPoseId,
				bStillValid ? TEXT("yes") : TEXT("no"),
				bStillTracked ? TEXT("yes") : TEXT("no"),
				EchoTraceCollisionBefore.Num(),
				bCollisionUnchanged ? TEXT("yes") : (bCopied ? TEXT("NO, the bytes differ") : TEXT("could not be read back"))));

		Advance(ECrowdyMassGateStep::EchoPoseCleared, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoPoseCleared:
	{
		const FCrowdyMassViewPoseFragment* PoseBefore = GetEchoPose();
		if (!PoseBefore || PoseBefore->PoseId == 0)
		{
			Skip(CrowdyMassGate::GateE4, TEXT("the entity was not in a pose going into this gate, so clearing the pose is not a transition and would pass without anything having been cleared"));
			Advance(ECrowdyMassGateStep::EchoUnmakePush, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const uint8 PoseIdBefore = PoseBefore->PoseId;

		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;

		TArray<uint8> Bytes;
		FInstancedStruct Decoded;
		FString Why;
		if (!EncodeEcho(State, 0, Bytes) || !PushEcho(Bytes, Decoded, Why))
		{
			Record(CrowdyMassGate::GateE4, false, Why.IsEmpty() ? FString(TEXT("the payload could not be encoded")) : Why);
			Advance(ECrowdyMassGateStep::EchoUnmakePush, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		if (!Pose)
		{
			Skip(CrowdyMassGate::GateE4, TEXT("the echo entity's view pose fragment went away between the push and the read"));
			Advance(ECrowdyMassGateStep::EchoUnmakePush, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Record(CrowdyMassGate::GateE4, Pose->PoseId == 0,
			FString::Printf(TEXT("the pose went from id %d to %d on an echo carrying no pose. This direction is gated on its own because an ingestor that only ever SETS a pose looks correct in every check that only puts entities into one."),
				PoseIdBefore, Pose->PoseId));

		Advance(ECrowdyMassGateStep::EchoUnmakePush, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoUnmakePush:
	{
		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		if (!Mass)
		{
			Skip(CrowdyMassGate::GateE5, TEXT("the world lost its Crowdy Mass entity subsystem mid-run"));
			Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Poisoned first, so nothing below can be satisfied by an entity nothing ever wrote to: an echo
		// that never reaches the entity leaves this value behind, and the assertion step reports itself as
		// not attempted rather than as a pass.
		PoisonEchoPose();

		// What the grid says BEFORE the echo. Without it a gate claiming the entity is "still" a candidate
		// would be answering about an entity the grid may never have held, and would fail for a reason
		// that has nothing to do with the echo.
		const FVector RayOffset(CrowdyMassGate::TraceGridHalfLengthCentimetres, 0.0f, 0.0f);
		TArray<FMassEntityHandle> CandidatesBefore;
		int32 VisitedCellsBefore = 0;
		bEchoUnmakeGridAnswered = Mass->GetTraceCandidateEntities(
			EchoLocation - RayOffset, EchoLocation + RayOffset, CandidatesBefore, VisitedCellsBefore);
		bEchoUnmakeInGridBefore = bEchoUnmakeGridAnswered && CandidatesBefore.Contains(EchoEntity);

		if (!CopyEchoTraceCollisionBytes(EchoUnmakeCollisionBefore))
		{
			EchoUnmakeCollisionBefore.Reset();
		}

		// Every appended byte carries a value a well-behaved producer would not send alongside the others:
		// a full stance byte, an empty health byte, and a pose id no project authors, all at the current
		// wire version so the payload cannot be dismissed as an older peer's. Two of those names have no
		// ingestor in this module and may well have one in the consuming game, and that is the point: this
		// gate does not need to know what any byte means, only that no meaning a game gives one can
		// destroy an entity, untrack it, take it out of the trace grid or move its trace collision.
		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;
		State.StanceFlags = MAX_uint8;
		State.HealthPercent = 0;
		State.PoseId = CrowdyMassGate::UnmakeEchoPoseId;

		TArray<uint8> Bytes;
		if (!EncodeEcho(State, 0, Bytes))
		{
			Skip(CrowdyMassGate::GateE5, TEXT("the hostile payload could not be encoded, so nothing was ever pushed at the entity"));
			Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		FInstancedStruct Decoded;
		FString Why;
		if (!PushEcho(Bytes, Decoded, Why))
		{
			Skip(CrowdyMassGate::GateE5,
				FString::Printf(TEXT("the hostile echo never reached the entity, so nothing structural was put at risk by it: %s"), *Why));
			Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Read immediately, because ingestion happens inside the push. Whether it landed decides only
		// whether this gate has anything to say; the structural readings a frame from now are taken either
		// way, and a pose fragment that has gone missing here is itself one of the things they catch.
		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		bEchoUnmakeIngested = Pose && Pose->PoseId == CrowdyMassGate::UnmakeEchoPoseId;

		EchoUnmakeFrameCounter = GFrameCounter;
		Advance(ECrowdyMassGateStep::EchoUnmakeAssert, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoUnmakeAssert:
	{
		// The frame counter, not a wall clock. Registered ingestors wrote their fragments inside the push
		// itself, but a game's processors and anything they notify do not run until the next processing
		// phase, so a reading taken in the same tick as the push cannot see one of them destroy the
		// entity. That is the whole failure this gate exists to catch, so a position in the frame sequence
		// is the measurement rather than a hope that enough time has gone by.
		const uint64 FramesCrossed = GFrameCounter - EchoUnmakeFrameCounter;
		if (FramesCrossed < CrowdyMassGate::UnmakeEchoFrameSpan)
		{
			if (HasStepTimedOut())
			{
				Skip(CrowdyMassGate::GateE5,
					FString::Printf(TEXT("the echo was pushed on frame %llu and %.0f s later only %llu frame(s) had passed, short of the %llu this gate waits for, so nothing here would have seen a processor or a listener act on it"),
						EchoUnmakeFrameCounter, StepTimeoutSeconds, FramesCrossed, CrowdyMassGate::UnmakeEchoFrameSpan));
				Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			}
			return true;
		}

		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
		if (!EntityManager)
		{
			Skip(CrowdyMassGate::GateE5,
				TEXT("the world lost its Crowdy Mass entity subsystem across the frames this gate waits out, so the readings before and after do not describe the same world"));
			Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const bool bStillValid = EntityManager->IsEntityValid(EchoEntity);

		FMassEntityHandle StillTracked;
		const bool bStillTracked = Mass->FindEntityByUUID(EchoUUID, StillTracked) && StillTracked == EchoEntity;

		const FVector RayOffset(CrowdyMassGate::TraceGridHalfLengthCentimetres, 0.0f, 0.0f);
		TArray<FMassEntityHandle> Candidates;
		int32 VisitedCells = 0;
		const bool bGridAnswered = Mass->GetTraceCandidateEntities(
			EchoLocation - RayOffset, EchoLocation + RayOffset, Candidates, VisitedCells);
		const bool bInGrid = bGridAnswered && Candidates.Contains(EchoEntity);

		TArray<uint8> CollisionAfter;
		const bool bCollisionCopied = CopyEchoTraceCollisionBytes(CollisionAfter);
		const bool bCollisionUnchanged = bCollisionCopied && CollisionAfter == EchoUnmakeCollisionBefore;

		// A leg nothing could measure is neither passed nor failed and must not move the verdict either
		// way. Which legs those were is spelled out in the text below, so a pass can never be read as
		// covering a question that was never asked.
		const bool bGridLegMeasured = bEchoUnmakeGridAnswered && bEchoUnmakeInGridBefore;
		const bool bCollisionLegMeasured = !EchoUnmakeCollisionBefore.IsEmpty();
		const bool bStructurallyIntact = bStillValid
			&& bStillTracked
			&& (!bGridLegMeasured || bInGrid)
			&& (!bCollisionLegMeasured || bCollisionUnchanged);

		const FString CollisionLeg = CrowdyMassGate::DescribeUnmakeCollisionLeg(
			bCollisionLegMeasured, bCollisionCopied, bCollisionUnchanged, EchoUnmakeCollisionBefore.Num());

		// All four readings are reported, passing or failing, alongside where in the frame sequence they
		// were taken. This is the hardest line in the architecture: view state comes from the observed
		// player's own client and is trivially forged, so a byte of it may choose what an entity looks
		// like and may never decide whether it exists or whether anyone can hit it.
		const FString Observed = FString::Printf(
			TEXT("a hostile echo (stance byte %d, health byte %d, forged pose id %d, at wire version %d so it cannot be dismissed as an older peer's) was pushed on frame %llu and read back %llu frames later, after a processing phase in which a game's processors and anything they notify had their chance to act on it. Still valid in the entity manager: %s. Tracker still resolves %s to the same handle: %s. Trace grid still returns it along a %.0f cm ray through its location: %s. Trace collision bytes unchanged: %s. The echo itself was ingested: %s."),
			static_cast<int32>(MAX_uint8), 0, static_cast<int32>(CrowdyMassGate::UnmakeEchoPoseId),
			static_cast<int32>(CrowdyMassViewStateWireVersion),
			EchoUnmakeFrameCounter, FramesCrossed,
			bStillValid ? TEXT("yes") : TEXT("NO, the entity was destroyed"),
			*EchoUUID.ToString(),
			bStillTracked ? TEXT("yes") : TEXT("NO, the id no longer resolves to this entity"),
			CrowdyMassGate::TraceGridHalfLengthCentimetres * 2.0f,
			CrowdyMassGate::DescribeUnmakeGridLeg(bEchoUnmakeGridAnswered, bEchoUnmakeInGridBefore, bGridAnswered, bInGrid),
			*CollisionLeg,
			bEchoUnmakeIngested ? TEXT("yes, its pose byte was read straight back off the entity") : TEXT("no, nothing was seen to land"));

		if (!bStructurallyIntact)
		{
			Record(CrowdyMassGate::GateE5, false, Observed);
			Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (!bEchoUnmakeIngested)
		{
			// Intact, but nothing was shown to have reached the entity, so nothing was ever demonstrably
			// at risk. Reported as not attempted, because these are the same readings an entity nobody
			// echoed at would give and a pass on them would prove nothing.
			Skip(CrowdyMassGate::GateE5, Observed
				+ TEXT(" Nothing landed, so these readings are the ones an entity nobody echoed at would give, and they say nothing about a hostile echo. E1 says whether ingestion works at all."));
			Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Record(CrowdyMassGate::GateE5, true, Observed);
		Advance(ECrowdyMassGateStep::EchoLegacy, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoLegacy:
	{
		PoisonEchoPose();

		// Every appended field is set to a value no fail-safe default holds: a stamped version, a full
		// stance byte, an empty health byte, a nonsense pose. The truncation below removes them, so if it
		// ever stopped removing them this gate would fail rather than pass on values it never meant to
		// send.
		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;
		State.StanceFlags = MAX_uint8;
		State.HealthPercent = 0;
		State.PoseId = 250;

		TArray<uint8> FullBytes;
		TArray<uint8> LegacyBytes;
		if (!EncodeEcho(State, 0, FullBytes)
			|| !EncodeEcho(State, CrowdyMassGate::ViewStateAppendedByteCount, LegacyBytes))
		{
			Skip(CrowdyMassGate::GateE6, TEXT("the payload could not be encoded, so a legacy-shaped one could not be cut from it"));
			Advance(ECrowdyMassGateStep::EchoForged, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		FInstancedStruct Decoded;
		FString Why;
		if (!PushEcho(LegacyBytes, Decoded, Why))
		{
			// The loudest failure in the set: this is what an older client's update looks like, so a
			// rejection here means every player on an older build is invisible to everyone on a newer one.
			Record(CrowdyMassGate::GateE6, false,
				FString::Printf(TEXT("%d bytes (a %d byte payload with its %d appended bytes cut off, which is exactly what an older client sends) and %s"),
					LegacyBytes.Num(), FullBytes.Num(), CrowdyMassGate::ViewStateAppendedByteCount, *Why));
			Advance(ECrowdyMassGateStep::EchoForged, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassGateWireState* DecodedState = Decoded.GetPtr<FCrowdyMassGateWireState>();
		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		if (!DecodedState || !Pose)
		{
			Skip(CrowdyMassGate::GateE6, TEXT("the decoded payload or the view pose fragment could not be read back"));
			Advance(ECrowdyMassGateStep::EchoForged, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const bool bPassed = Pose->PoseId == 0
			&& DecodedState->Version == CrowdyMassViewStateWireVersionUnspecified;

		Record(CrowdyMassGate::GateE6, bPassed,
			FString::Printf(TEXT("%d bytes instead of %d (the %d appended bytes absent, as an older client sends it) decoded to version byte %d, expected %d for a sender that never wrote one, and left pose id %d on an entity poisoned to %d beforehand (expected 0)"),
				LegacyBytes.Num(), FullBytes.Num(), CrowdyMassGate::ViewStateAppendedByteCount,
				DecodedState->Version, CrowdyMassViewStateWireVersionUnspecified,
				Pose->PoseId, CrowdyMassGate::PoisonPoseId));

		Advance(ECrowdyMassGateStep::EchoForged, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::EchoForged:
	{
		PoisonEchoPose();

		// A pose id far outside anything a project authors, which is what a modified client sends. The
		// expected outcome is that it is STORED verbatim and acted on nowhere: ingestion has no authored
		// range to check against, so the bound belongs where the id is resolved to an animation, and an
		// ingestor that quietly rewrote the byte would hide a forged value from whoever looks at the
		// fragment. What must not happen is the entity being unmade or losing its identity over it.
		FCrowdyMassGateWireState State;
		State.Location = EchoLocation;
		State.Version = CrowdyMassViewStateWireVersion;
		State.StanceFlags = MAX_uint8;
		State.HealthPercent = 0;
		State.PoseId = MAX_uint8;

		TArray<uint8> Bytes;
		FInstancedStruct Decoded;
		FString Why;
		if (!EncodeEcho(State, 0, Bytes) || !PushEcho(Bytes, Decoded, Why))
		{
			Record(CrowdyMassGate::GateE7, false, Why.IsEmpty() ? FString(TEXT("the payload could not be encoded")) : Why);
			Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassViewPoseFragment* Pose = GetEchoPose();
		if (!Pose)
		{
			Skip(CrowdyMassGate::GateE7, TEXT("the echo entity's view pose fragment went away between the push and the read"));
			Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		FMassEntityManager* EntityManager = Mass ? Mass->GetMassEntityManager() : nullptr;
		const bool bStillValid = EntityManager && EntityManager->IsEntityValid(EchoEntity);

		FMassEntityHandle StillTracked;
		const bool bStillTracked = Mass && Mass->FindEntityByUUID(EchoUUID, StillTracked) && StillTracked == EchoEntity;

		const bool bPassed = Pose->PoseId == State.PoseId && bStillValid && bStillTracked;

		// The stored byte is reported rather than only a pass or a fail. A gate that says only "it did no
		// harm" reads the same whether the byte was stored, clamped or dropped, and those need different
		// answers from whoever resolves the id later.
		Record(CrowdyMassGate::GateE7, bPassed,
			FString::Printf(TEXT("a forged pose id %d, stance byte %d and health byte %d in, over an entity poisoned to pose %d: the stored pose id is %d (expected it stored as sent, to be bounds-checked where it is resolved), the entity is still valid: %s, still tracked: %s"),
				State.PoseId, State.StanceFlags, State.HealthPercent, CrowdyMassGate::PoisonPoseId,
				Pose->PoseId,
				bStillValid ? TEXT("yes") : TEXT("no"),
				bStillTracked ? TEXT("yes") : TEXT("no")));

		Advance(ECrowdyMassGateStep::ChurnBaseline, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::ChurnBaseline:
	{
		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		if (!Mass)
		{
			Skip(CrowdyMassGate::GateL1, TEXT("the world lost its Crowdy Mass entity subsystem mid-run"));
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (!ReadRendererTrackCounts(TracksAtBaseline, InstancesAtBaseline))
		{
			// Not a pass. Nothing in this world draws through the instanced skinned mesh path, so there is
			// no track count at all, and reporting zero equals zero as a pass would turn this gate into one
			// that can never fail.
			Skip(CrowdyMassGate::GateL1, TEXT("this world has no instanced-skinned-mesh shared data, so the crowd here is not rendered through the path that owns animation tracks and there is no count to watch"));
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		TracksAtPeak = TracksAtBaseline;
		InstancesAtPeak = InstancesAtBaseline;

		ChurnEntitiesSpawned = 0;
		for (int32 Index = 0; Index < ChurnEntityCountRequested; ++Index)
		{
			const FGuid UUID = CrowdyMassGate::MakeGateGuid(CrowdyMassGate::ChurnGuidSaltBase + Index);
			const FTransform Transform(FRotator::ZeroRotator,
				EchoLocation + FVector(0.0f, 0.0f, static_cast<float>(Index)));

			ChurnUUIDs.Add(UUID);
			if (Mass->SpawnRemoteEntity(UUID, Transform).IsSet())
			{
				++ChurnEntitiesSpawned;
			}
		}

		if (ChurnEntitiesSpawned == 0)
		{
			Skip(CrowdyMassGate::GateL1, TEXT("not one churn entity spawned, so nothing was ever handed to the renderer for it to hold a track for"));
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Advance(ECrowdyMassGateStep::ChurnSettle, CrowdyMassGate::ChurnSettleSeconds);
		return true;
	}

	case ECrowdyMassGateStep::ChurnSettle:
	{
		int32 Tracks = 0;
		int32 Instances = 0;
		if (ReadRendererTrackCounts(Tracks, Instances))
		{
			TracksAtPeak = FMath::Max(TracksAtPeak, Tracks);
			InstancesAtPeak = FMath::Max(InstancesAtPeak, Instances);
		}

		// This step's deadline is a dwell rather than a failure: the entities have to live long enough for
		// the renderer to be handed them, and the peak above is sampled on every frame of it. Reading only
		// at the end of the window would miss tracks that came and went inside it, which is the reading
		// that decides whether this gate has anything to say at all.
		if (!HasStepTimedOut())
		{
			return true;
		}

		Advance(ECrowdyMassGateStep::ChurnDestroy, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::ChurnDestroy:
	{
		if (bChurnDestroyRan)
		{
			// Unregistered here, on the harness's own ticker, rather than from inside the tick function.
			DisarmChurnDestroyTick();
			Advance(ECrowdyMassGateStep::ChurnDrain, CrowdyMassGate::ChurnDrainSeconds);
			return true;
		}

		if (!ChurnDestroyTick.IsTickFunctionRegistered() && !ArmChurnDestroyTick())
		{
			// Deliberately not destroyed from here instead. A destroy from this ticker lands outside the
			// span in which a destroy can leak a track, so the reading it produced would say the tracks
			// came back without ever having put the release path under the condition it exists for.
			Skip(CrowdyMassGate::GateL1,
				TEXT("this world has no level to register a tick function against, so the destroy could only have landed outside the frame span where an animation track leaks, and any count taken across it would be measuring a release that was never at risk"));
			DestroyChurnEntities();
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			Skip(CrowdyMassGate::GateL1,
				FString::Printf(TEXT("the churn destroy tick function was registered in %s but had not run %.0f s later, so the entities were never destroyed at the point in the frame this gate measures"),
					CrowdyMassGate::DescribeTickGroup(ChurnDestroyTick.TickGroup), StepTimeoutSeconds));
			DisarmChurnDestroyTick();
			DestroyChurnEntities();
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::ChurnDrain:
	{
		// Said first in every outcome below, passing, failing or skipped. A track count that returned is
		// only evidence about the release path if the destroy that produced it happened inside the frame
		// span where a destroy leaks a track, and nothing else in the report says where that was.
		const FString FramePosition = DescribeChurnDestroyPosition();

		int32 Tracks = 0;
		int32 Instances = 0;
		if (!ReadRendererTrackCounts(Tracks, Instances))
		{
			Skip(CrowdyMassGate::GateL1,
				FString::Printf(TEXT("%s. The instanced-skinned-mesh shared data then went away part way through, so the count before and the count after do not describe the same thing"),
					*FramePosition));
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (Tracks <= TracksAtBaseline)
		{
			if (TracksAtPeak <= TracksAtBaseline)
			{
				// A count that never went up cannot demonstrate coming back down. Without this branch the
				// gate would pass on every world where the crowd is drawn some other way, which is exactly
				// the permanent false pass it is meant to avoid.
				Skip(CrowdyMassGate::GateL1,
					FString::Printf(TEXT("%s. But the track count never rose above its starting %d while %d entities were alive (peak %d, live instances %d against a starting %d), so there is no rise for it to have returned from and this reading proves nothing"),
						*FramePosition, TracksAtBaseline, ChurnEntitiesSpawned, TracksAtPeak, InstancesAtPeak, InstancesAtBaseline));
			}
			else
			{
				Record(CrowdyMassGate::GateL1, true,
					FString::Printf(TEXT("%s. %d entities spawned and destroyed there: animation tracks %d -> %d (peak) -> %d against a starting %d, live instances %d -> %d against a starting %d"),
						*FramePosition, ChurnEntitiesSpawned, TracksAtBaseline, TracksAtPeak, Tracks, TracksAtBaseline,
						InstancesAtPeak, Instances, InstancesAtBaseline));
			}

			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (HasStepTimedOut())
		{
			Record(CrowdyMassGate::GateL1, false,
				FString::Printf(TEXT("%s. %.0f s after the last of %d entities was destroyed the animation track count is %d against a starting %d (peak %d), so %d tracks were never released; live instances are %d against a starting %d"),
					*FramePosition, StepTimeoutSeconds, ChurnEntitiesSpawned, Tracks, TracksAtBaseline, TracksAtPeak,
					Tracks - TracksAtBaseline, Instances, InstancesAtBaseline));
			Advance(ECrowdyMassGateStep::PoseBankSlots, CrowdyMassGate::ShortStepSeconds);
		}
		return true;
	}

	case ECrowdyMassGateStep::PoseBankSlots:
	{
		FString Observed;
		bool bSkipped = false;
		const bool bPassed = ValidatePoseBankSlots(Observed, bSkipped);

		if (bSkipped)
		{
			Skip(CrowdyMassGate::GateD1, Observed);
		}
		else
		{
			Record(CrowdyMassGate::GateD1, bPassed, Observed);
		}

		// Every early exit and skip in the steps above funnels through this one, so this is the only
		// forward edge into the CrowdyState gates that the ordinary flow needs. The single exception is
		// the setup failure right at the start of the run, which reaches them directly. A step inserted
		// anywhere earlier would instead have to be threaded through every branch of its predecessor, and
		// the branch that got missed would drop the new gates silently on exactly the paths that took an
		// early exit.
		Advance(ECrowdyMassGateStep::StateSpawn, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::StateSpawn:
	{
		FString Why;
		if (!SpawnStateEntity(Why))
		{
			SkipEveryStateGate(Why);
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		if (!ReadStateCounters(StateCountersBefore))
		{
			SkipEveryStateGate(TEXT("the counters every one of these gates is a difference over could not be read, so there is nothing to compare against"));
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		Record(TEXT("Setup: one entity to drive CrowdyState deltas at"), true,
			FString::Printf(TEXT("spawned entity %s from this plugin's own template, carrying a view pose fragment, recorded as class %s, with no participant anywhere for its id"),
				*StateUUID.ToString(), *UCrowdyMassGateStateEntity::StaticClass()->GetName()));

		Advance(ECrowdyMassGateStep::StateDeliver, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::StateDeliver:
	{
		PoisonStatePose();

		// Read here rather than at spawn, so the window every verdict is a difference over starts as late
		// as it can and contains as little of the session's other traffic as possible.
		if (!ReadStateCounters(StateCountersBefore))
		{
			SkipEveryStateGate(TEXT("the counters every one of these gates is a difference over could not be read, so there is nothing to compare against"));
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		int32 Dispatched = 0;
		FString FirstFailure;
		for (const uint8 Stance : CrowdyMassGate::StateStances)
		{
			const FCrowdyMassStateDeltaByteField DeltaFields[] = {{CrowdyMassGate::StatePoseFieldName, Stance}};

			FString Why;
			if (!DispatchStateDelta(DeltaFields, Why))
			{
				FirstFailure = Why;
				break;
			}

			++Dispatched;
		}

		FCrowdyMassGateStateCounters After;
		if (!ReadStateCounters(After))
		{
			SkipEveryStateGate(TEXT("the counters could not be read back after the deltas were sent, so no difference can be taken"));
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const int32 Expected = UE_ARRAY_COUNT(CrowdyMassGate::StateStances);
		const uint8 ExpectedPoseId = CrowdyMassGate::ExpectedPoseIdForLastStance;

		const int64 Claimed = After.Claimed - StateCountersBefore.Claimed;
		const int64 Applied = After.Applied - StateCountersBefore.Applied;
		const int64 Delivered = After.DeliveredSlots - StateCountersBefore.DeliveredSlots;
		const int64 NotClaimed = After.NotClaimed - StateCountersBefore.NotClaimed;
		const int64 NoEntityClass = After.DroppedNoEntityClass - StateCountersBefore.DroppedNoEntityClass;
		const int64 ClassIdMismatch = After.DroppedClassIdMismatch - StateCountersBefore.DroppedClassIdMismatch;
		const int64 NoLayout = After.DroppedNoLayout - StateCountersBefore.DroppedNoLayout;
		const int64 NoBuffer = After.DroppedNoScratch - StateCountersBefore.DroppedNoScratch;
		const int64 DecodeFailed = After.DroppedDecodeFailed - StateCountersBefore.DroppedDecodeFailed;
		const int64 StateWrites = After.PoseWritesFromState - StateCountersBefore.PoseWritesFromState;

		const FCrowdyMassViewPoseFragment* Pose = GetStatePose();
		const int32 PoseRead = Pose ? static_cast<int32>(Pose->PoseId) : -1;

		if (Dispatched < Expected)
		{
			// A delta this harness could not even build is a failure and not a skip: every input to it is
			// this module's own, so nothing a project did can be what stopped it.
			const FString Why = FString::Printf(
				TEXT("only %d of %d deltas could be built and sent, and the first that could not said: %s"),
				Dispatched, Expected, *FirstFailure);
			Record(CrowdyMassGate::GateS1, false, Why);
			Record(CrowdyMassGate::GateS2, false, Why);
			Record(CrowdyMassGate::GateS3, false, Why);
			Advance(ECrowdyMassGateStep::StateGenericField, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Delivered slots are counted per SLOT rather than per delta, and every delta here is a keyframe,
		// so each one carries every CrowdyState property the class has. Quoting the product rather than a
		// bare "it went up" is what makes this arithmetic and not an impression.
		const int64 ExpectedDelivered = static_cast<int64>(Expected) * StateLayoutSlotCount;

		// One cause turns all of these red at once and is not what any of them is named for: the router
		// keeps a single entity-subscriber slot per world and the newest registrant takes it, so anything
		// else registering after this subsystem leaves every delta offered to a subscriber that does not
		// hold this id. The counters say so precisely, claiming nothing while everything goes on waiting,
		// and naming it here saves a reader diagnosing four failures that share one answer.
		const FString SubscriberNote = (Claimed == 0 && NotClaimed == Expected)
			? TEXT(". Nothing was claimed while everything went on waiting, which is what it looks like when the router's entity-subscriber slot is held by something other than this world's Crowdy Mass entity subsystem rather than when this path is broken")
			: FString();

		const bool bS1 = Claimed == Expected && Applied == Expected && Delivered == ExpectedDelivered
			&& PoseRead == static_cast<int32>(ExpectedPoseId);
		Record(CrowdyMassGate::GateS1, bS1, FString::Printf(
			TEXT("%d deltas sent to an id with no participant: claimed=%lld applied=%lld delivered slots=%lld (expected %d, %d and %d, being %d slots on each of %d deltas), and the entity now reads pose id %d after being poisoned to %d (expected %d). That expected id is NOT the stance value that was sent: the last delta carries Downed, which this run's animation set authors at row 0, so a receive path writing the arriving byte through would leave 2 here rather than 1. The second delta repeats the first's stance on purpose: the decode buffer is reused and still holds that value, so a receiver counting only the slots whose value moved would report that delta as having delivered nothing"),
			Expected, Claimed, Applied, Delivered, Expected, Expected, static_cast<int32>(ExpectedDelivered),
			StateLayoutSlotCount, Expected, PoseRead, static_cast<int32>(CrowdyMassGate::PoisonPoseId),
			static_cast<int32>(ExpectedPoseId)) + SubscriberNote);

		// Every refusal reason the receive path has, including the one for an entity held with no recorded
		// class. That one is listed because a gate that checks a subset of the reasons reads green while a
		// delta is refused for the reason it left out: a crowd spawned without a class would claim every
		// delta, apply none, and show zeros on all the others.
		const bool bS2 = NotClaimed == 0 && NoEntityClass == 0 && ClassIdMismatch == 0 && NoLayout == 0
			&& NoBuffer == 0 && DecodeFailed == 0;
		Record(CrowdyMassGate::GateS2, bS2, FString::Printf(
			TEXT("over the window those %d deltas were sent in: not claimed (went on waiting)=%lld, no recorded class=%lld, classid mismatch=%lld, no layout=%lld, no buffer=%lld, decode failed=%lld. Every one of these being zero is only worth reading alongside S1: nothing is rejected when nothing arrives either"),
			Expected, NotClaimed, NoEntityClass, ClassIdMismatch, NoLayout, NoBuffer, DecodeFailed) + SubscriberNote);

		// The gate the whole set exists for, and its subject NARROWED in Phase A.4 rather than going away.
		//
		// It used to assert that the view-state echo wrote nothing over the window, because the pose had
		// two transports writing one byte and a correct-looking pose was evidence for neither. That road
		// is deleted: CrowdyMass registers no pose field on the echo plane at all now. So the echo term
		// was REMOVED rather than kept reading zero. A term that no longer has a way to fail makes a
		// verdict read as more thorough than it is, which is the trap this file warns about elsewhere.
		//
		// What is left is still worth asserting and is not implied by S1: that the state plane wrote the
		// pose exactly as many times as deltas were sent. A path that wrote once and left the right value
		// standing would satisfy the fragment reading while dropping two deliveries.
		const bool bPoseIsTheSentOne = PoseRead == static_cast<int32>(ExpectedPoseId);
		const bool bS3 = StateWrites == Expected && bPoseIsTheSentOne;

		FString S3Observed = FString::Printf(
			TEXT("pose writes over the window from CrowdyState=%lld (expected %d, one per delta); the entity reads pose id %d after being poisoned to %d (expected %d). The echo is no longer measured here because it no longer carries a pose: this plugin registers no pose field on that plane"),
			StateWrites, Expected, PoseRead,
			static_cast<int32>(CrowdyMassGate::PoisonPoseId), static_cast<int32>(ExpectedPoseId));

		if (StateWrites == 0 && bPoseIsTheSentOne)
		{
			S3Observed += TEXT(". The pose is the one that was sent and the state plane wrote none of it, which means something else put it there: exactly the failure this gate exists to catch, and exactly what a picture of the entity would have hidden");
		}

		Record(CrowdyMassGate::GateS3, bS3, S3Observed);

		Advance(ECrowdyMassGateStep::StateGenericField, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::StateGenericField:
	{
		UCrowdyMassEntitySubsystem* Mass = GetMassSubsystem();
		if (!Mass)
		{
			Skip(CrowdyMassGate::GateS4, TEXT("the world lost its Crowdy Mass entity subsystem mid-run"));
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		// Registered here, through the same public call a consuming game uses and with no help from
		// anything private to this module, which is the whole claim: a field this module knows nothing
		// about, added without editing it. Guarded on the name because registrations cannot be removed,
		// so a second run of the gates would otherwise leave a second entry that nothing ever calls.
		//
		// Registering it AFTER the deltas above have already been decoded is deliberate too: a class
		// resolved before a field was registered has to be resolved again, and a registration that only
		// worked when it happened before any traffic would not be one a game could rely on.
		//
		// The ingestor holds this harness weakly, and that is what makes it safe rather than a nicety.
		// The registration is stored on a WORLD subsystem while this harness belongs to the game
		// instance, and on ending a play session the game instance is shut down BEFORE the world is
		// cleaned up, so the registration outlives the harness rather than the other way round. A
		// subsystem being deinitialized is also not marked as garbage, so a raw pointer would still look
		// live and would call into a torn-down object.
		// Whether THIS run did the registering, and it is reported rather than assumed. A second run in
		// the same world finds the field already registered and skips the registration, so it measures
		// that an earlier run's field still receives bytes, which is a weaker statement than the one this
		// gate is named for. Two runs printing the same green line while measuring different things is
		// how a report stops meaning what it says.
		const bool bRegisteredThisRun = !Mass->IsStateFragmentFieldRegistered(CrowdyMassGate::GateOnlyStateFieldName);

		if (bRegisteredThisRun)
		{
			TWeakObjectPtr<UCrowdyMassGateHarness> WeakThis(this);
			Mass->RegisterStateFragmentField(CrowdyMassGate::GateOnlyStateFieldName, ECrowdyMassStateFieldType::UInt8,
				[WeakThis](const void* PropertyValuePtr, const FCrowdyMassViewStateContext& Context)
				{
					UCrowdyMassGateHarness* Harness = WeakThis.Get();
					if (!Harness || !PropertyValuePtr)
					{
						return;
					}

					Harness->bGateFieldIngestorRan = true;
					Harness->GateFieldValueSeen = *static_cast<const uint8*>(PropertyValuePtr);

					// Resolved but not written. A game's own ingestor exists to write a fragment, so an
					// ingestor that receives its bytes and cannot reach one has a registration it cannot
					// use; writing here would put this gate's own value where a later gate reads.
					Harness->bGateFieldReachedAFragment = Context.GetFragment<FCrowdyMassViewPoseFragment>() != nullptr;
				});
		}

		if (!Mass->IsStateFragmentFieldRegistered(CrowdyMassGate::GateOnlyStateFieldName))
		{
			Record(CrowdyMassGate::GateS4, false,
				FString::Printf(TEXT("registering '%s' through the public call left no registration behind, so nothing a game registers this way could ever land"),
					*CrowdyMassGate::GateOnlyStateFieldName.ToString()));
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		bGateFieldIngestorRan = false;
		GateFieldValueSeen = 0;
		bGateFieldReachedAFragment = false;
		PoisonStatePose();

		const FCrowdyMassStateDeltaByteField DeltaFields[] = {
			{CrowdyMassGate::StatePoseFieldName, CrowdyMassGate::StateGenericStance},
			{CrowdyMassGate::GateOnlyStateFieldName, CrowdyMassGate::GateOnlyStateByteValue}};

		FString Why;
		if (!DispatchStateDelta(DeltaFields, Why))
		{
			Record(CrowdyMassGate::GateS4, false, Why);
			Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
			return true;
		}

		const FCrowdyMassViewPoseFragment* Pose = GetStatePose();
		const int32 PoseRead = Pose ? static_cast<int32>(Pose->PoseId) : -1;

		// The value is read back off the ingestor rather than off a fragment. What is being gated is that
		// the bytes reached the code that registered for them, and a fragment some other field wrote
		// would answer a different question.
		//
		// Whether the ingestor could reach a fragment is reported but deliberately NOT part of the
		// verdict: the spawn step already refused to go on without that same fragment on that same
		// entity, so asserting it again here would add a term that cannot independently fail. A verdict
		// built from terms that cannot fail reads as more thorough than it is.
		const bool bPassed = bGateFieldIngestorRan
			&& GateFieldValueSeen == CrowdyMassGate::GateOnlyStateByteValue
			&& PoseRead == static_cast<int32>(CrowdyMassGate::ExpectedPoseIdForGenericStance);

		Record(CrowdyMassGate::GateS4, bPassed, FString::Printf(
			TEXT("'%s' is a CrowdyState property this module registers nothing for: %s, one delta carrying %d in it was sent, and the ingestor %s and was handed %d. The standard field in the same delta left pose id %d on an entity poisoned to %d (expected %d), so this delta went the same way the ones before it did. The ingestor %s reach a fragment through the context it was given, which the spawn had already established and which is reported here rather than asserted"),
			*CrowdyMassGate::GateOnlyStateFieldName.ToString(),
			bRegisteredThisRun
				? TEXT("this run registered an ingestor for it through the public call, after the deltas above had already resolved the class once, so the registration had to invalidate that resolution")
				: TEXT("an earlier run in this world had already registered an ingestor for it and registrations cannot be removed, so this run measured that it still receives bytes rather than that registering works"),
			static_cast<int32>(CrowdyMassGate::GateOnlyStateByteValue),
			bGateFieldIngestorRan ? TEXT("ran") : TEXT("NEVER RAN"),
			static_cast<int32>(GateFieldValueSeen),
			PoseRead, static_cast<int32>(CrowdyMassGate::PoisonPoseId),
			static_cast<int32>(CrowdyMassGate::ExpectedPoseIdForGenericStance),
			bGateFieldReachedAFragment ? TEXT("could") : TEXT("could NOT")));

		Advance(ECrowdyMassGateStep::ExtraGates, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::ExtraGates:
	{
		UWorld* World = GetWorld();

		// Copied before running any of them, because a probe is free to register a gate of its own and
		// that would reallocate the array being walked.
		const TArray<CrowdyMassGate::FExtraGate> Gates = CrowdyMassGate::GetExtraGates();
		for (const CrowdyMassGate::FExtraGate& Gate : Gates)
		{
			if (!Gate.Probe)
			{
				Skip(Gate.Name.ToString(), TEXT("the gate was registered without a probe to run"));
				continue;
			}

			FString Observed;
			const bool bPassed = Gate.Probe(World, Observed);
			Record(Gate.Name.ToString(), bPassed, Observed.IsEmpty()
				? FString(TEXT("the gate reported no reading, so there is no number to tell a right answer from a lucky one"))
				: Observed);
		}

		Advance(ECrowdyMassGateStep::Report, CrowdyMassGate::ShortStepSeconds);
		return true;
	}

	case ECrowdyMassGateStep::Report:
		PrintReport();
		StopGates();
		return true;
	}

	return true;
}

void UCrowdyMassGateHarness::PrintReport()
{
	int32 Passed = 0;
	int32 Failed = 0;
	int32 Skipped = 0;

	UE_LOG(LogCrowdyMass, Log, TEXT("===== crowdy.mass.gates ====="));

	for (const FCrowdyMassGateResult& Result : Results)
	{
		if (Result.bSkipped)
		{
			++Skipped;
			UE_LOG(LogCrowdyMass, Warning, TEXT("  SKIP %s: %s"), *Result.Name, *Result.Observed);
			continue;
		}

		if (Result.bPassed)
		{
			++Passed;
			UE_LOG(LogCrowdyMass, Log, TEXT("  PASS %s: %s"), *Result.Name, *Result.Observed);
			continue;
		}

		++Failed;
		UE_LOG(LogCrowdyMass, Error, TEXT("  FAIL %s: %s"), *Result.Name, *Result.Observed);
	}

	UE_LOG(LogCrowdyMass, Log, TEXT("  %d passed, %d failed, %d skipped."), Passed, Failed, Skipped);

	// Said every run, because it is the half no single-process run can cover, and a report that listed
	// only what it checked would read as a complete answer.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("  STILL MANUAL, not covered here: two-client delivery, that the OTHER machine runs the body exactly once on the same entity. This ran in one process. The actor RPC path being unchanged is covered headlessly by CrowdySDK.EventSeam.ActorSourcedRpcNeverConsultsExternalProvider."));
	UE_LOG(LogCrowdyMass, Log,
		TEXT("  STILL MANUAL, not covered here: that the OTHER machine SHOWS it. Every view-state gate above reads a fragment in the same process that wrote the bytes, so it can say the values landed and cannot say a second client's screen changed. Watch a pose start and clear on the other machine."));
	UE_LOG(LogCrowdyMass, Log,
		TEXT("  STILL MANUAL, not covered here: what a game's OWN fields mean once they land. The pose gates measure the pose byte, which proves registration, encode, decode and ingestion end to end for any field registered the same way, and E5 sends hostile values in every field the wire struct carries precisely because a game may have claimed the ones this module has not. Neither can say a game's field reached that game's own fragment with the right value: that needs a gate of the game's own, registered here, reading its own fragments."));
	UE_LOG(LogCrowdyMass, Log,
		TEXT("  STILL MANUAL, not covered here: that a pose plays the RIGHT animation. The pose gate resolves each authored pose name to a bank slot and checks no gait shares it; it cannot tell one clip from another sitting in that slot."));
	UE_LOG(LogCrowdyMass, Log,
		TEXT("  STILL MANUAL, not covered here: that a CrowdyState delta CROSSES A NETWORK. The S gates synthesize a delta and hand it to the router the way one arriving from another machine is handed over, so they cover claim, class guard, decode, delivery and the fragment write; they skip the transport, because a process is correctly forbidden from applying its own deltas and cannot make one arrive from outside itself. Two clients, one setting a pose and the other with no Actor for that entity, is still the gate that decides this."));
	UE_LOG(LogCrowdyMass, Log, TEXT("============================="));
}

#if !UE_BUILD_SHIPPING
namespace
{
	UCrowdyMassGateHarness* ResolveGateHarness(UWorld* World)
	{
		UCrowdyMassGateHarness* Harness = UCrowdyMassGateHarness::Get(World);
		UE_CLOG(!Harness, LogCrowdyMass, Warning,
			TEXT("crowdy.mass: no gate harness for this world, so run this from a play session."));
		return Harness;
	}
}

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassOneShot(
	TEXT("crowdy.mass.oneshot"),
	TEXT("Fires one PlayOneShot at a Mass entity along the camera's aim, or at any tracked one. Args: <actionId> [variant] [playRate]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassGateHarness* Harness = ResolveGateHarness(World))
			{
				Harness->FireOneShot(
					Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1,
					Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 0,
					Args.Num() > 2 ? FCString::Atof(*Args[2]) : 1.0f);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassGates(
	TEXT("crowdy.mass.gates"),
	TEXT("Runs every automated gate and prints one PASS/FAIL report to copy in a single selection: the one-shot action path, the view-state echo (a pose landing, a second echo changing it, a pose touching nothing else, a pose clearing, a hostile echo in every field being unable to unmake an entity, a truncated payload, a forged payload), the renderer's animation track count across a spawn and destroy churn, whether every authored pose resolves to a bank slot of its own, the CrowdyState delta path into an entity that owns no participant (which road actually wrote the pose, what was dropped, and whether a field registered from outside this module receives its bytes), and any gate a game has registered. Optional arg: how many entities the churn spawns (default 64, clamped 1 to 1024). Around fifteen seconds at the default; forces crowdy.rpc.loopback on for the run and restores it afterwards."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassGateHarness* Harness = ResolveGateHarness(World))
			{
				Harness->RunGates(Args.Num() > 0
					? FCString::Atoi(*Args[0])
					: CrowdyMassGate::DefaultChurnEntityCount);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassGatesStop(
	TEXT("crowdy.mass.gates.stop"),
	TEXT("Abandons a gate run in progress, destroying its entities and restoring crowdy.rpc.loopback."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassGateHarness* Harness = ResolveGateHarness(World))
			{
				Harness->StopGates();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassSendAction(
	TEXT("crowdy.mass.sendaction"),
	TEXT("Sends the SDK's generic Crowdy action at a Mass entity along the camera's aim, or at any tracked one, through the real Send Crowdy Action path: it reaches every machine drawing the entity, this one included, and unlike crowdy.mass.oneshot it carries no ownership check. Args: <actionName> [variant] [playRate]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassGateHarness* Harness = ResolveGateHarness(World))
			{
				Harness->SendGenericAction(
					Args.Num() > 0 ? FName(*Args[0]) : NAME_None,
					Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 0,
					Args.Num() > 2 ? FCString::Atof(*Args[2]) : 1.0f);
			}
		}));
#endif

// Coverage for CrowdyMassGate::ResolveSendActionTarget, the entity pick behind crowdy.mass.sendaction.
// Everything else the command does (logging, and the call into UCrowdyMassTargetLibrary::SendEntityAction)
// needs a live play session to exercise for real, same as every other command in this file; this is the
// one piece of it that is plain C++ over a Mass entity manager and needs neither a World nor a console.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyMassSendActionResolveTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A standalone Mass entity manager and subsystem, with no world: entity resolution reads the
	// subsystem's tracked-entity map directly and does not need one.
	struct FCrowdyMassSendActionResolveFixture
	{
		TSharedPtr<FMassEntityManager> EntityManager;
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassSendActionResolveFixture()
		{
			// Outered to the transient package rather than left ownerless: PostInitialize constructs
			// relation observers outered to the manager's owner, and one with none takes the process down.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));

			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();

			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetMassEntityManagerForTest(EntityManager.Get());
		}

		~FCrowdyMassSendActionResolveFixture()
		{
			EntityManager->Deinitialize();
		}

		FMassEntityHandle MakeTrackedEntity(const FGuid& UUID)
		{
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(
				MakeArrayView<const UScriptStruct*>({FCrowdyMassActionFragment::StaticStruct()}));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Subsystem->RegisterTrackedEntityForTest(UUID, Entity);
			return Entity;
		}
	};
}

// Mutating ResolveSendActionTarget's tracked-fallback loop to return true unconditionally (skipping the
// FindUUIDByEntity check, or returning true past an empty Tracked array) turns this red: with nothing
// registered there is no entity to find, and a command that reported one anyway would be lying to whoever
// typed it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSendActionResolveFindsNothingWhenNothingIsTrackedTest,
	"CrowdySDK.Mass.SendActionResolveFindsNothingWhenNothingIsTracked", CrowdyMassSendActionResolveTestFlags)
bool FCrowdyMassSendActionResolveFindsNothingWhenNothingIsTrackedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSendActionResolveFixture Fixture;

	FMassEntityHandle OutEntity;
	FGuid OutUUID;
	bool bOutFromAimTrace = true; // Deliberately wrong going in, so a false return proves it was cleared.

	// World is null, exactly as it is for a headless run with no player controller to aim from: this
	// exercises the tracked-fallback leg on its own, with nothing tracked to fall back to.
	const bool bFound = CrowdyMassGate::ResolveSendActionTarget(*Fixture.Subsystem, nullptr, OutEntity, OutUUID,
		bOutFromAimTrace);

	TestFalse(TEXT("nothing is tracked, so nothing is found"), bFound);
	TestFalse(TEXT("and the aim-trace flag reads as not taken, not left at whatever it started as"),
		bOutFromAimTrace);

	return true;
}

// The paired case: one entity registered exactly as the tracker's own bookkeeping would, and nothing along
// the aim because World is null. Mutating the tracked-fallback loop to always report bFromAimTrace = true,
// or to hand back the wrong entity's UUID, turns this red without touching the case above: the two gates
// share no state, and this one alone catches a resolve that finds something but misreports which one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassSendActionResolveFallsBackToATrackedEntityTest,
	"CrowdySDK.Mass.SendActionResolveFallsBackToATrackedEntity", CrowdyMassSendActionResolveTestFlags)
bool FCrowdyMassSendActionResolveFallsBackToATrackedEntityTest::RunTest(const FString& Parameters)
{
	FCrowdyMassSendActionResolveFixture Fixture;

	const FGuid UUID = FGuid::NewGuid();
	const FMassEntityHandle Entity = Fixture.MakeTrackedEntity(UUID);

	FMassEntityHandle OutEntity;
	FGuid OutUUID;
	bool bOutFromAimTrace = true;

	const bool bFound = CrowdyMassGate::ResolveSendActionTarget(*Fixture.Subsystem, nullptr, OutEntity, OutUUID,
		bOutFromAimTrace);

	TestTrue(TEXT("the one tracked entity is found"), bFound);
	TestEqual(TEXT("its handle is the one returned"), OutEntity, Entity);
	TestEqual(TEXT("and its UUID is the one returned"), OutUUID, UUID);
	TestFalse(TEXT("reached through the tracked fallback, not the aim trace"), bOutFromAimTrace);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
