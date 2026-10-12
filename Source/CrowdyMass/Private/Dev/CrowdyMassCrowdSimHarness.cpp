#include "Dev/CrowdyMassCrowdSimHarness.h"

#include "CrowdyCppReplication.h"
#include "CrowdyMassLog.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/CrowdyEntityTypes.h"
#include "Derivation/CrowdyMassClassDerivationReport.h"
#include "Dev/CrowdyMassCrowdSimStateEntity.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Network/CrowdyCpp/CrowdyCppLoopback.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Serialization/CrowdyActorId.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/EnumProperty.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utils/HelperFunctions.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UCrowdyClassRegistry.h"

namespace CrowdyMassCrowdSim
{
	// The rate a real client emits position updates at. The generator exists to reproduce the load a
	// crowd of real clients makes, so this matches the sender rather than being tunable: a run at a
	// different rate is measuring a game that does not exist.
	constexpr float EmissionIntervalSeconds = 0.1f;

	constexpr int32 DefaultBotCount = 2000;
	constexpr float DefaultRadiusMetres = 60.0f;

	// A hard ceiling on the number of bots, so a mistyped argument cannot spend the session
	// allocating instead of failing.
	constexpr int32 MaxBotCount = 20000;

	// One unattended profiling pass per set of trace scopes; see StartProfile for why they cannot
	// share a run.
	constexpr int32 ProfilePassCount = 3;

	// Matched to the population and link conditions an earlier baseline was taken under, so pass A is
	// comparable rather than merely similar.
	constexpr int32 DefaultProfileBotCount = 10000;
	constexpr float DefaultProfileTraceSeconds = 60.0f;
	constexpr float ProfileLatencyMs = 40.0f;
	constexpr float ProfileJitterMs = 10.0f;

	// The on-change rate the last pass raises to. Ten times the ordinary interval, so CrowdyState deltas
	// are a measurable share of the mix without displacing the transform stream the run is otherwise about.
	constexpr float DefaultProfileStateInterval = 0.5f;

	// Long enough for the crowd to reach its steady emission rate, and to leave the first long frame after
	// a level load outside the sample.
	constexpr float ProfileSettleSeconds = 8.0f;

	// Between passes, so the previous pass's entities have timed out of the tracker before the next one
	// seeds a fresh crowd on top of them.
	constexpr float ProfileCooldownSeconds = 5.0f;

	// How often an unattended run writes its own status block. Often enough that a run whose
	// transport starts dropping shows when it began, rare enough not to bury the log.
	constexpr double AutoStatusIntervalSeconds = 5.0;

	// Movement speeds in centimetres per second, spanning the range a player covers. They are the
	// harness's own numbers and are deliberately NOT read from the animation set: the observer's job
	// is to judge whether the authored thresholds pick sensible gaits for these speeds, and a
	// generator that derived its speeds from those same thresholds could never disagree with them.
	constexpr float WalkSpeed = 150.0f;
	constexpr float JogSpeed = 330.0f;
	constexpr float RunSpeed = 600.0f;

	// The speed a bot pins itself to during a threshold hold. Sits between the walk and jog speeds
	// above, which is where an authored enter or exit speed is most likely to fall.
	constexpr float ThresholdHoldSpeed = 240.0f;

	// How sharply a bot converges on its target speed. Long enough that a bot passes visibly through
	// the speeds between two phases rather than stepping over them in one update.
	constexpr float SpeedBlendPerSecond = 2.5f;

	// Chunk edge length in centimetres, used only to describe how the bots are spread. The spatial
	// header's own chunk maths is done by the SDK helper, never by this value.
	constexpr double ChunkSizeCentimetres = 100.0 * 100.0;

	// The names of the reflected fields UCrowdyMassCrowdSimHarness::ResolveBotFields looks for. Matched by
	// name and by type, never by metadata: cooked builds strip reflection metadata.
	const FName PayloadPropertyName(TEXT("Payload"));
	const FName StancePropertyName(TEXT("Stance"));

	float PhaseDurationSeconds(FRandomStream& Random, ECrowdyMassCrowdSimPhase Phase)
	{
		switch (Phase)
		{
		case ECrowdyMassCrowdSimPhase::Standing:
			return Random.FRandRange(1.0f, 3.0f);
		case ECrowdyMassCrowdSimPhase::HoldingAtThreshold:
			// Held longer than a real player would, on purpose. A gait selector with no hysteresis
			// flickers for exactly as long as the speed sits on the boundary, so a short hold could
			// be missed between two observer samples.
			return Random.FRandRange(3.0f, 6.0f);
		default:
			return Random.FRandRange(2.0f, 5.0f);
		}
	}

	float PhaseSpeed(ECrowdyMassCrowdSimPhase Phase)
	{
		switch (Phase)
		{
		case ECrowdyMassCrowdSimPhase::Standing:            return 0.0f;
		case ECrowdyMassCrowdSimPhase::Walking:             return WalkSpeed;
		case ECrowdyMassCrowdSimPhase::Jogging:             return JogSpeed;
		case ECrowdyMassCrowdSimPhase::Running:             return RunSpeed;
		case ECrowdyMassCrowdSimPhase::HoldingAtThreshold:  return ThresholdHoldSpeed;
		}

		return 0.0f;
	}
}

void UCrowdyMassCrowdSimHarness::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StartFromCommandLineIfAsked();
}

void UCrowdyMassCrowdSimHarness::StartFromCommandLineIfAsked()
{
	int32 BotCount = 0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("crowdsim="), BotCount) || BotCount <= 0)
	{
		return;
	}

	float RadiusMetres = 0.0f;
	FParse::Value(FCommandLine::Get(), TEXT("crowdsimradius="), RadiusMetres);

	// Local delivery is opt-in from the command line as well as from the console, because the population
	// that needs it is exactly the population nobody can watch interactively while typing.
	if (FParse::Param(FCommandLine::Get(), TEXT("crowdsimlocal")))
	{
		float LatencyMilliseconds = 0.0f;
		float JitterMilliseconds = -1.0f;
		float LossPercent = -1.0f;
		FParse::Value(FCommandLine::Get(), TEXT("crowdsimlatency="), LatencyMilliseconds);
		FParse::Value(FCommandLine::Get(), TEXT("crowdsimjitter="), JitterMilliseconds);
		FParse::Value(FCommandLine::Get(), TEXT("crowdsimloss="), LossPercent);
		SetLocalDelivery(true, LatencyMilliseconds, JitterMilliseconds, LossPercent);
	}

	UClass* CommandLineBotClass = nullptr;
	FString ClassArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("crowdsimclass="), ClassArg) && !ClassArg.IsEmpty())
	{
		FString Why;
		CommandLineBotClass = FCrowdyMassClassDerivationReport::ResolveActorClass(ClassArg, Why);
		UE_CLOG(!CommandLineBotClass, LogCrowdyMass, Warning,
			TEXT("-crowdsimclass='%s' could not be resolved (%s), falling back to the plugin's own fixture."),
			*ClassArg, *Why);
	}

	// Deferred to the first map: at Initialize the only world is the pre-map placeholder, which LoadMap
	// destroys along with anything spawned into it.
	PendingCommandLineBotCount = BotCount;
	PendingCommandLineRadiusMetres = RadiusMetres;
	PendingCommandLineBotClass = CommandLineBotClass;
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &UCrowdyMassCrowdSimHarness::StartFromCommandLineOnMapLoaded);

	UE_LOG(LogCrowdyMass, Log,
		TEXT("-crowdsim: the generator will start for %d simulated players once the first map has loaded. This generator signs in nobody: whatever "
			"session the Crowdy SDK subsystem has by then is what these updates go out under."),
		BotCount);
}

void UCrowdyMassCrowdSimHarness::StartFromCommandLineOnMapLoaded(UWorld* LoadedWorld)
{
	if (!LoadedWorld || LoadedWorld->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	PostLoadMapHandle.Reset();

	UE_LOG(LogCrowdyMass, Log, TEXT("-crowdsim: map '%s' loaded, starting the generator for %d simulated players."),
		*LoadedWorld->GetName(), PendingCommandLineBotCount);
	Start(PendingCommandLineBotCount, PendingCommandLineRadiusMetres, PendingCommandLineBotClass);
	PendingCommandLineBotClass = nullptr;
}

bool UCrowdyMassCrowdSimHarness::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	return Super::ShouldCreateSubsystem(Outer);
#endif
}

void UCrowdyMassCrowdSimHarness::Deinitialize()
{
	if (PostLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
		PostLoadMapHandle.Reset();
	}

	// Before Stop, so a session torn down mid-pass still puts the scope CVars back and never leaves a
	// trace file open on a process that is going away.
	if (IsProfileSessionRunning())
	{
		FTraceAuxiliary::Stop();
		FinishProfileSession();
	}

	Stop();
	Super::Deinitialize();
}

UCrowdyMassCrowdSimHarness* UCrowdyMassCrowdSimHarness::Get(const UWorld* World)
{
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyMassCrowdSimHarness>() : nullptr;
}

FString UCrowdyMassCrowdSimHarness::MakeDeterministicActorId(int32 RunSeed, int32 BotIndex)
{
	// Four independent words rather than one hash repeated, so two bots cannot share an id through a
	// hash collision in a single 32-bit value. The literals are arbitrary mixing constants; what
	// matters is that they differ from each other.
	const uint32 A = GetTypeHash(FString::Printf(TEXT("crowdsim.%d.%d.a"), RunSeed, BotIndex));
	const uint32 B = GetTypeHash(FString::Printf(TEXT("crowdsim.%d.%d.b"), RunSeed, BotIndex));
	const uint32 C = GetTypeHash(FString::Printf(TEXT("crowdsim.%d.%d.c"), RunSeed, BotIndex));
	const uint32 D = GetTypeHash(FString::Printf(TEXT("crowdsim.%d.%d.d"), RunSeed, BotIndex));

	// Digits is the 32-character, dashless form, which is what a real entity's id serialises to on
	// the wire. Anything else would be a different length and would not round-trip.
	return FGuid(A, B, C, D).ToString(EGuidFormats::Digits);
}

FGuid UCrowdyMassCrowdSimHarness::MakeDeterministicStateEntityId(const FString& ActorId)
{
	// Derived FROM the bot's actor id through the SDK's own conversion, so a bot's two channels name the
	// same entity on the observer rather than two unrelated ones.
	return USerializationFunctionLibrary::ToGuid(ActorId);
}

FVector UCrowdyMassCrowdSimHarness::ResolveCrowdCentre() const
{
	// An explicit centre wins over everything. Whether an unattended instance has a pawn by the time
	// the generator starts depends on how far the game has got through its own start-up, so a run
	// that has to put the crowd somewhere specific should say so rather than hope.
	FString CentreArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("crowdsimcenter="), CentreArg))
	{
		FVector Centre;
		if (Centre.InitFromString(CentreArg) || Centre.InitFromCompactString(CentreArg))
		{
			return Centre;
		}

		UE_LOG(LogCrowdyMass, Warning,
			TEXT("-crowdsimcenter='%s' could not be read as a location, so the crowd falls back to the player or the origin. Use the form X=100,Y=200,Z=300."),
			*CentreArg);
	}

	const UWorld* World = GetWorld();
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;

	// Falling back to the origin is a real possibility on an unattended instance, not a corner case:
	// whether a pawn exists yet depends on start-up order. The origin may be nowhere near the play
	// area, in which case an observer sees nothing at all, so Start logs whichever was used.
	return Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
}

void UCrowdyMassCrowdSimHarness::SetLocalDelivery(const bool bEnable, const float LatencyMs,
	const float JitterMs, const float LossPercent)
{
	bLocalDelivery = bEnable;

	if (LatencyMs > 0.0f)
	{
		LocalLatencyMilliseconds = LatencyMs;
	}
	if (JitterMs >= 0.0f)
	{
		LocalJitterMilliseconds = JitterMs;
	}
	if (LossPercent >= 0.0f)
	{
		LocalLossPercent = LossPercent;
	}

	if (!bRunning)
	{
		UE_LOG(LogCrowdyMass, Log,
			TEXT("crowdy.mass.crowdsim.local: local delivery is %s and will apply to the next run."),
			bLocalDelivery ? TEXT("on") : TEXT("off"));
		return;
	}

	if (bLocalDelivery)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.local: the running bots were seeded with their crowd ids as their own CrowdyState ids, so the local observer will refuse their rows as its own until the crowd is restarted. Stop and start again for rows to appear."));
		ApplyLocalDelivery();
		return;
	}

	ClearLocalDelivery();
	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.local: local delivery is off, so the simulated crowd is going to the network again. "
			"Watch the offered load: this population may be more than the uplink carries."));
}

void UCrowdyMassCrowdSimHarness::SetStateChangeInterval(float Seconds)
{
	StateChangeIntervalSeconds = Seconds;

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.state: each simulated player's CrowdyState payload now changes on average every %s. This is separate from the keyframe heartbeat, which the map profile controls and which fires regardless."),
		StateChangeIntervalSeconds > 0.0f
			? *FString::Printf(TEXT("%.1f s"), StateChangeIntervalSeconds)
			: TEXT("never (0 or negative)"));
}

void UCrowdyMassCrowdSimHarness::ApplyLocalDelivery()
{
	UCrowdyCppReplicationSubsystem* Replication = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr;
	if (!Replication)
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("crowdy.mass.crowdsim: local delivery was asked for but there is no replication subsystem to intercept "
				"through, so the crowd would go to the network at a size this machine cannot send. Not starting it."));
		bLocalDelivery = false;
		return;
	}

	TArray<FCrowdyActorId> ActorIds;
	ActorIds.Reserve(Bots.Num());
	for (const FCrowdyMassCrowdSimBot& Bot : Bots)
	{
		// Refused rather than skipped. An id that will not convert is one this run would emit and never
		// see come back, which reads on the observer as a crowd smaller than the one asked for and says
		// nothing about why.
		FCrowdyActorId ActorId;
		if (!FCrowdyActorId::TryFromString(Bot.ActorId, ActorId))
		{
			UE_LOG(LogCrowdyMass, Error,
				TEXT("crowdy.mass.crowdsim: simulated player id '%s' is not the 32 octets an actor id is, so local "
					"delivery would silently miss it. Not starting local delivery."),
				*Bot.ActorId);
			bLocalDelivery = false;
			return;
		}

		ActorIds.Add(ActorId);
	}

	FCrowdyLoopbackSettings Settings;
	Settings.LatencySeconds = LocalLatencyMilliseconds / 1000.0f;
	Settings.JitterSeconds = LocalJitterMilliseconds / 1000.0f;
	Settings.LossFraction = LocalLossPercent / 100.0f;

	Replication->ArmLocalDelivery(ActorIds, Settings);

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim: the %d simulated players are being delivered inside this process, %.0f ms later give "
			"or take %.0f, with %.1f%% thrown away. Nothing of theirs reaches the network, so this measures what "
			"this client costs to RECEIVE a crowd this size. It cannot measure anything the server decides, above "
			"all which of these the relay would have judged relevant to you."),
		Bots.Num(), LocalLatencyMilliseconds, LocalJitterMilliseconds, LocalLossPercent);

	// Said once at arm time rather than beside every reading, because it qualifies the whole run and a
	// caveat repeated every five seconds stops being read. It is the one way this mode's per-message cost
	// differs from a connected client's: what surrounds each delivery here is a locked heap pop, and on the
	// wire it is the library's own ring.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim: the per-message drain cost reported below is measured through local delivery, so "
			"the part of it that is not delivery belongs to this mode rather than to the network path. Read it as "
			"the receive ceiling this mode can prove, and confirm it against a connected client before it moves a "
			"shipped default."));

	// The tracker's ceiling is deliberately NOT restated here. It comes from a serialized map profile,
	// so a constant in this file would be a third number free to disagree with the config default and
	// with the tracker's own, and a warning that fires wrongly is worse than none: it hands the
	// operator a ready-made false explanation for a shortfall that has some other cause. The tracker
	// itself logs its effective ceiling once per world and warns once when it actually reaches it,
	// which is the same information from the only place that knows it.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim: if fewer than %d players appear, check the tracker's own start-up line for the "
			"Max Tracked Actors this map profile carries. Entities past that ceiling never appear at all."),
		Bots.Num());
}

void UCrowdyMassCrowdSimHarness::ClearLocalDelivery()
{
	if (UCrowdyCppReplicationSubsystem* Replication = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr)
	{
		Replication->DisarmLocalDelivery();
	}
}

void UCrowdyMassCrowdSimHarness::ReportLocalDelivery(const double ReportSeconds)
{
	UCrowdyCppReplicationSubsystem* Replication = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr;
	if (!Replication || !Replication->IsLocalDeliveryArmed())
	{
		return;
	}

	const FCrowdyLoopbackStats Stats = Replication->GetLocalDeliveryStats();

	UE_LOG(LogCrowdyMass, Log,
		TEXT("  local delivery: %lld updates taken and %lld handed back in the last %.0f s, %lld lost to the "
			"simulated loss rate, largest single drain %d."),
		Stats.Accepted, Stats.Delivered, ReportSeconds, Stats.DroppedToLoss, Stats.LargestDrain);

	// The number this whole mode exists to produce. The receive drain has a fixed message and time
	// budget per frame, so a client can only take so many updates a second however they got there. A
	// queue that keeps growing is that budget being exceeded, and it is a client limit rather than a
	// network one: moving the generator to another machine would not change it.
	//
	// Which budget ran out is named, because the two want opposite things done about them.
	UE_CLOG(Stats.DrainsCutShortByMessageBudget > 0, LogCrowdyMass, Warning,
		TEXT("  %lld drains spent their whole message allowance and the queue is holding %d updates, %d at its worst. "
			"This client cannot take %d players' updates at this rate, whatever sends them. Raise "
			"crowdy.net.receive.maxmessages, or lower the population until the queue stops growing: that number is "
			"the real receive ceiling."),
		Stats.DrainsCutShortByMessageBudget, Stats.QueueDepth, Stats.LargestQueueDepth, Bots.Num());

	// Said separately and worded away from the message count on purpose. Raising the message allowance
	// against a time-bound drain spends more of the frame on the same backlog and clears none of it.
	UE_CLOG(Stats.DrainsCutShortByTimeBudget > 0, LogCrowdyMass, Warning,
		TEXT("  %lld drains ran out of TIME rather than message allowance, holding %d updates. Delivering one update "
			"is costing more than the frame can afford, so the message count is not the lever here: raise "
			"crowdy.net.receive.maxdrainms only if the frame can spare it, and otherwise the cost per update is what "
			"has to come down."),
		Stats.DrainsCutShortByTimeBudget, Stats.QueueDepth);

	UE_CLOG(Stats.DroppedToQueueFull > 0, LogCrowdyMass, Warning,
		TEXT("  %lld updates were refused because local delivery's own queue was full, so the crowd the observer "
			"sees is smaller than the one being generated."),
		Stats.DroppedToQueueFull);

	// What the message allowance above is derived from, taken over the whole drain rather than over the
	// traced scopes inside delivery, which are about two thirds of it.
	UE_LOG(LogCrowdyMass, Log, TEXT("  %s"), *Replication->DescribeInboundDrainCost());

	// Rolled here so every figure above describes one interval, matching how the transport counters
	// beside them are reported.
	Replication->ResetLocalDeliveryCounters();
	Replication->ResetInboundDrainCost();
}

FCrowdyMassCrowdSimBotFields UCrowdyMassCrowdSimHarness::ResolveBotFields(const UClass* BotClassToResolve)
{
	FCrowdyMassCrowdSimBotFields Fields;
	if (!BotClassToResolve)
	{
		return Fields;
	}

	for (TFieldIterator<FProperty> It(BotClassToResolve); It; ++It)
	{
		FProperty* Property = *It;

		if (Property->GetFName() == CrowdyMassCrowdSim::PayloadPropertyName)
		{
			Fields.PayloadProperty = CastField<FIntProperty>(Property);
			continue;
		}

		if (Property->GetFName() == CrowdyMassCrowdSim::StancePropertyName)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Fields.StanceProperty = EnumProperty;
				Fields.StanceEnum = EnumProperty->GetEnum();
				Fields.StanceUnderlyingProperty = EnumProperty->GetUnderlyingProperty();
			}
			else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				if (ByteProperty->Enum)
				{
					Fields.StanceProperty = ByteProperty;
					Fields.StanceEnum = ByteProperty->Enum;
				}
			}
			continue;
		}
	}

	if (Fields.StanceEnum)
	{
		Fields.StanceMaxValue = Fields.StanceEnum->GetMaxEnumValue();
	}

	return Fields;
}

void UCrowdyMassCrowdSimHarness::WriteStanceValue(const FCrowdyMassCrowdSimBotFields& Fields, void* Container, int64 Value)
{
	// The underlying property is owned by the enum property, so it is addressed at the value, never in-container.
	if (Fields.StanceUnderlyingProperty)
	{
		Fields.StanceUnderlyingProperty->SetIntPropertyValue(
			Fields.StanceProperty->ContainerPtrToValuePtr<void>(Container), Value);
		return;
	}

	static_cast<const FByteProperty*>(Fields.StanceProperty)->SetPropertyValue_InContainer(Container, static_cast<uint8>(Value));
}

void UCrowdyMassCrowdSimHarness::SpawnStateEntities()
{
	UWorld* World = GetWorld();
	UCrowdyEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
	if (!EntitySubsystem)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim: no entity subsystem, so the simulated players cannot carry a CrowdyState identity this run. The transform channel is unaffected; only CrowdyState traffic is missing."));
		return;
	}

	// This client owns every simulated player, exactly like a real locally-controlled entity: the harness
	// process stands in for a whole population of clients, but from the SDK's point of view each of these
	// is Role::Owner under the one local player that is actually signed in here.
	const FGuid LocalPlayerID = EntitySubsystem->GetLocalPlayerID();

	int32 SpawnFailures = 0;
	for (FCrowdyMassCrowdSimBot& Bot : Bots)
	{
		const FTransform SpawnTransform(FRotator::ZeroRotator, Bot.Location);

		// Deferred, exactly like UCrowdyStateReplicator's own loopback mirror: InitIdentity has to run
		// before BeginPlay, which is where the component self-registers and this entity becomes something
		// the replicator will walk.
		AActor* Entity = World->SpawnActorDeferred<AActor>(
			BotClass, SpawnTransform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Entity)
		{
			++SpawnFailures;
			continue;
		}

		if (UCrowdyEntityComponent* Component = Entity->FindComponentByClass<UCrowdyEntityComponent>())
		{
			Component->InitIdentity(Bot.StateEntityID, LocalPlayerID, ECrowdyRole::Owner, CROWDY_INVALID_CLASS_ID);
		}

		Entity->FinishSpawning(SpawnTransform);
		Entity->SetActorHiddenInGame(true);
		Entity->SetActorTickEnabled(false);
		Bot.StateEntity = Entity;
	}

	UE_CLOG(SpawnFailures > 0, LogCrowdyMass, Warning,
		TEXT("crowdy.mass.crowdsim: %d of %d simulated players could not spawn a CrowdyState entity and will carry no CrowdyState traffic this run."),
		SpawnFailures, Bots.Num());
}

void UCrowdyMassCrowdSimHarness::DestroyStateEntities()
{
	// Unlike the transform channel's simulated players, which Stop deliberately abandons to the tracker's
	// own timeout, a CrowdyState entity is a real UObject this process spawned and owns, so it has to be
	// torn down explicitly rather than left for the world to eventually garbage-collect.
	for (FCrowdyMassCrowdSimBot& Bot : Bots)
	{
		if (AActor* Entity = Bot.StateEntity.Get())
		{
			Entity->Destroy();
		}
		Bot.StateEntity.Reset();
	}
}

void UCrowdyMassCrowdSimHarness::SeedBots(int32 BotCount, float RadiusMetres)
{
	const FVector& Centre = CrowdCentre;
	RadiusCentimetres = RadiusMetres * 100.0f;

	Bots.Reset(BotCount);
	for (int32 BotIndex = 0; BotIndex < BotCount; ++BotIndex)
	{
		FCrowdyMassCrowdSimBot& Bot = Bots.AddDefaulted_GetRef();
		Bot.ActorId = MakeDeterministicActorId(RunSeed, BotIndex);
		// The participant's own id, and under local delivery it must NOT be the crowd id. On a real observer
		// no state entity exists, so naming the crowd id is what makes a bot's two channels agree. In one
		// process the entity is right here, so naming the crowd id would let participant resolution find it
		// and take the delta down the object path, quietly testing the opposite of what this run is for.
		Bot.StateEntityID = bLocalDelivery
			? FGuid::NewGuid()
			: MakeDeterministicStateEntityId(Bot.ActorId);
		Bot.Random.Initialize(RunSeed * 7919 + BotIndex);

		// Staggered from the start, like SecondsUntilNextPhase below: without this every bot's first
		// CrowdyState change would land on the same tick, which is exactly the shared-clock burst this
		// harness exists to avoid producing.
		Bot.SecondsUntilNextStateChange = Bot.Random.FRandRange(0.0f, FMath::Max(StateChangeIntervalSeconds, 0.1f));

		// Square-rooted radius, so the bots are spread evenly over the disc rather than bunched at
		// its centre. An uneven spread would make chunk occupancy a property of this line rather
		// than of the radius asked for, and chunk occupancy is one of the numbers being measured.
		const float Angle = Bot.Random.FRandRange(0.0f, 2.0f * PI);
		const float Distance = RadiusCentimetres * FMath::Sqrt(Bot.Random.FRand());
		Bot.Location = Centre + FVector(Distance * FMath::Cos(Angle), Distance * FMath::Sin(Angle), 0.0f);

		Bot.HeadingRadians = Bot.Random.FRandRange(0.0f, 2.0f * PI);
		ChooseNextPhase(Bot);

		// Started at the phase's speed rather than at zero, so the crowd is already in motion on the
		// first frame instead of every bot accelerating from a standstill together.
		Bot.SpeedCentimetresPerSecond = Bot.TargetSpeedCentimetresPerSecond;
		Bot.SecondsUntilNextPhase = Bot.Random.FRandRange(0.0f, Bot.SecondsUntilNextPhase);
	}
}

void UCrowdyMassCrowdSimHarness::ChooseNextPhase(FCrowdyMassCrowdSimBot& Bot) const
{
	// Weighted so that most of the crowd is moving at any moment, with the threshold hold rare enough
	// to be a spice rather than the whole test but common enough that a few dozen of a large crowd are
	// sitting on a boundary at all times.
	const float Roll = Bot.Random.FRand();
	if (Roll < 0.20f)       { Bot.Phase = ECrowdyMassCrowdSimPhase::Standing; }
	else if (Roll < 0.45f)  { Bot.Phase = ECrowdyMassCrowdSimPhase::Walking; }
	else if (Roll < 0.70f)  { Bot.Phase = ECrowdyMassCrowdSimPhase::Jogging; }
	else if (Roll < 0.90f)  { Bot.Phase = ECrowdyMassCrowdSimPhase::Running; }
	else                    { Bot.Phase = ECrowdyMassCrowdSimPhase::HoldingAtThreshold; }

	Bot.TargetSpeedCentimetresPerSecond = CrowdyMassCrowdSim::PhaseSpeed(Bot.Phase);
	Bot.SecondsUntilNextPhase = CrowdyMassCrowdSim::PhaseDurationSeconds(Bot.Random, Bot.Phase);
	Bot.TurnRateRadiansPerSecond = Bot.Random.FRandRange(-0.8f, 0.8f);
}

void UCrowdyMassCrowdSimHarness::AdvanceBot(FCrowdyMassCrowdSimBot& Bot, const FVector& Centre, float DeltaSeconds) const
{
	Bot.SecondsUntilNextPhase -= DeltaSeconds;
	if (Bot.SecondsUntilNextPhase <= 0.0f)
	{
		ChooseNextPhase(Bot);
	}

	// Eased rather than snapped, so a bot crosses the speeds between two phases instead of jumping
	// over them. Those crossings are what exercise gait selection at all.
	Bot.SpeedCentimetresPerSecond = FMath::FInterpTo(
		Bot.SpeedCentimetresPerSecond, Bot.TargetSpeedCentimetresPerSecond, DeltaSeconds, CrowdyMassCrowdSim::SpeedBlendPerSecond);

	// A threshold hold is the one phase that must not be eased into and then drifted off: the point
	// is to sit exactly on a boundary.
	if (Bot.Phase == ECrowdyMassCrowdSimPhase::HoldingAtThreshold
		&& FMath::IsNearlyEqual(Bot.SpeedCentimetresPerSecond, Bot.TargetSpeedCentimetresPerSecond, 5.0f))
	{
		Bot.SpeedCentimetresPerSecond = Bot.TargetSpeedCentimetresPerSecond;
	}

	Bot.HeadingRadians += Bot.TurnRateRadiansPerSecond * DeltaSeconds;
	const FVector Forward(FMath::Cos(Bot.HeadingRadians), FMath::Sin(Bot.HeadingRadians), 0.0f);
	Bot.Location += Forward * Bot.SpeedCentimetresPerSecond * DeltaSeconds;

	// Turned back at the edge rather than clamped. Clamping would pile every bot that reached the
	// boundary onto it, which would quietly concentrate the crowd into the outermost chunks and make
	// the occupancy number describe the containment rule instead of the radius.
	const FVector FromCentre = Bot.Location - Centre;
	if (FromCentre.SizeSquared2D() > FMath::Square(RadiusCentimetres))
	{
		Bot.HeadingRadians = FMath::Atan2(-FromCentre.Y, -FromCentre.X) + Bot.Random.FRandRange(-0.5f, 0.5f);
		Bot.Location = Centre + FromCentre.GetSafeNormal2D() * RadiusCentimetres;
	}
}

void UCrowdyMassCrowdSimHarness::EmitBot(FCrowdyMassCrowdSimBot& Bot, UCrowdySDKSubsystem& SDK)
{
	FCrowdyActorState State;
	State.Location = Bot.Location;
	State.Rotation = FRotator(0.0f, FMath::RadiansToDegrees(Bot.HeadingRadians), 0.0f);

	// The chunk comes from the SDK's own helper rather than from arithmetic here, so a change to how
	// the world is divided moves the bots with it instead of silently addressing them to chunks that
	// no longer exist.
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;
	UHelperFunctions::GetChunkCoordinateAtLocation(this, Bot.Location, ChunkX, ChunkY, ChunkZ);

	// The same call the automatic replicator makes for a real player, with the same decay and
	// distance arguments. Going through anything else would make this a test of a second sender.
	//
	// The class id is what stops an observer resolving these bots as whatever other class last claimed
	// FCrowdyActorState: a class guard on the receiving end refuses a delta whose claimed class does not
	// match what the sender is actually spawned as.
	SDK.DispatchActorUpdate(ChunkX, ChunkY, ChunkZ,
		ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance::Eight_Chunks,
		Bot.ActorId,
		FInstancedStruct::Make(State),
		BotClassID,
		/*bAsync=*/true);

	++UpdatesEmitted;

	// A second, independent channel: CrowdyState (typed gameplay properties), riding the real
	// UCrowdyStateReplicator send path rather than DispatchActorUpdate. See EmitBotState.
	EmitBotState(Bot);
}

void UCrowdyMassCrowdSimHarness::EmitBotState(FCrowdyMassCrowdSimBot& Bot)
{
	AActor* Entity = Bot.StateEntity.Get();
	if (!Entity)
	{
		return;
	}

	// Keeps the CrowdyState entity's own location in step with the position the transform channel is
	// reporting for this bot. Location carries no CrowdyState property on this entity, so this alone puts
	// nothing on the wire; it exists only so the entity's spatial relevance (which the SDK reads off the
	// actor's own transform) does not go stale relative to where the bot actually is.
	Entity->SetActorLocation(Bot.Location);

	// Zero or negative turns on-change CrowdyState traffic off for this run; the keyframe heartbeat still
	// fires on the entity's own schedule regardless, since it does not depend on this timer.
	if (StateChangeIntervalSeconds <= 0.0f)
	{
		return;
	}

	// Ticked in emission-interval units, not true wall-clock seconds: EmitBot for a given bot fires roughly
	// once every CrowdyMassCrowdSim::EmissionIntervalSeconds (the phase sweep spaces the population across
	// that interval, never together), so counting down by that fixed step reuses the same clock the rest of
	// this bot's traffic already runs on instead of plumbing a separate delta-time through the emission sweep.
	Bot.SecondsUntilNextStateChange -= CrowdyMassCrowdSim::EmissionIntervalSeconds;
	if (Bot.SecondsUntilNextStateChange > 0.0f)
	{
		return;
	}

	// Redrawn per bot from its own stream, half to one and a half times the configured average, so a
	// crowd's worth of bots' payload changes land spread across time instead of bunching on one interval
	// boundary together, the same reasoning EmissionPhaseSeconds uses for the transform channel, applied
	// to this one.
	Bot.SecondsUntilNextStateChange = Bot.Random.FRandRange(
		StateChangeIntervalSeconds * 0.5f, StateChangeIntervalSeconds * 1.5f);

	// The value itself is arbitrary; what matters is that it changed; the real replicator's own diff is
	// what decides whether that is worth a delta, and the real codec is what puts it on the wire.
	if (BotFields.PayloadProperty)
	{
		BotFields.PayloadProperty->SetPropertyValue_InContainer(Entity, static_cast<int32>(Bot.Random.RandRange(0, MAX_int32)));
	}

	// A stance the observer can actually see, drawn from the whole enum. The first entry is included on
	// purpose: it normally names no authored row and therefore CLEARS the pose, and a run that only ever
	// sets one cannot show that clearing works, which is the direction an ingestor is most likely to get
	// wrong.
	//
	// The bound comes from the enum rather than naming its last entry, so a value added later is emitted
	// here without an edit.
	const int32 LastStanceValue = FMath::Max(static_cast<int32>(BotFields.StanceMaxValue) - 1, 0);
	const int64 NewStance = Bot.Random.RandRange(0, LastStanceValue);
	if (BotFields.StanceProperty)
	{
		WriteStanceValue(BotFields, Entity, NewStance);
	}

	// Delivered whenever the payload changed; a class with no Stance still gets its keyframe.
	DeliverStateLocally(Bot, NewStance);
}

void UCrowdyMassCrowdSimHarness::WriteStanceIntoTemplate(const FCrowdyRepLayout& Layout, UObject* Template, int64 StanceValue) const
{
	const UEnum* const StanceEnum = BotFields.StanceEnum;
	if (!StanceEnum)
	{
		return;
	}

	// Found through the layout by the TYPE it is declared as, which is the same question the receiving side
	// asks, and which keeps this rig honest: it can only set what a real sender of that class could
	// replicate, and it fails to find the field in exactly the case a real sender would fail to send it.
	for (const FCrowdyRepProperty& RepProperty : Layout.Properties)
	{
		const FProperty* Property = RepProperty.Property;
		if (!Property)
		{
			continue;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (EnumProperty->GetEnum() != StanceEnum)
			{
				continue;
			}

			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
				EnumProperty->ContainerPtrToValuePtr<void>(Template), StanceValue);
			return;
		}

		const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
		if (ByteProperty && ByteProperty->Enum == StanceEnum)
		{
			ByteProperty->SetPropertyValue_InContainer(Template, static_cast<uint8>(StanceValue));
			return;
		}
	}
}

void UCrowdyMassCrowdSimHarness::DeliverStateLocally(const FCrowdyMassCrowdSimBot& Bot, int64 StanceValue)
{
	if (!bLocalDelivery)
	{
		return;
	}

	// Local delivery exists because one machine cannot otherwise observe its own crowd, and CrowdyState is
	// harder to loop back than the transform channel is. THREE separate guards stand between a delta this
	// process sent and this process applying it, and every one of them is correct: the router drops a delta
	// whose SenderID is our own (we already hold what we sent), it drops a foreign non-host delta for an
	// entity we own, and it resolves a participant first, which for a locally spawned entity always
	// succeeds and takes the delta down the object path instead of the one being tested. So a loopback that
	// merely re-sent the bytes would be silently testing nothing.
	//
	// What this does instead: encode through the REAL codec, then hand the router a delta addressed to the
	// bot's CROWD entity id (the one the Mass row is keyed by, which owns no participant here) carrying a
	// synthetic sender. That is the same shape the delta would have arriving from another machine, and it
	// reaches the participant-less path for the same reason. It skips the transport only, which the
	// networked run already measures on its own.
	UWorld* World = GetWorld();
	UCrowdyEventRouter* Router = World ? World->GetSubsystem<UCrowdyEventRouter>() : nullptr;
	UCrowdyMassEntitySubsystem* MassSubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	UGameInstance* GameInstance = GetGameInstance();
	UCrowdyAutoRegistry* Registry = GameInstance ? GameInstance->GetSubsystem<UCrowdyAutoRegistry>() : nullptr;
	if (!Router || !Registry || !MassSubsystem)
	{
		return;
	}

	const FGuid CrowdEntityID = USerializationFunctionLibrary::ToGuid(Bot.ActorId);

	// The class the OBSERVER recorded for this crowd row, asked of the same source the receive path's guard
	// reads, so the two cannot disagree. A delta may only carry the class this client already decided the
	// entity has.
	const UClass* EntityClass = MassSubsystem->GetEntityClass(CrowdEntityID);
	if (!EntityClass)
	{
		return;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(EntityClass);
	if (!Layout)
	{
		return;
	}

	// One property container per class, shaped like the class the delta claims to be, because the codec
	// encodes by reading a container's own property offsets. Never spawned and never ticked: it is a bag of
	// properties standing in for the sender this process is pretending to be.
	TObjectPtr<UObject>& TemplateSlot = LocalDeliveryTemplates.FindOrAdd(EntityClass);
	if (!TemplateSlot)
	{
		TemplateSlot = NewObject<UObject>(this, const_cast<UClass*>(EntityClass));
	}

	UObject* Template = TemplateSlot;
	if (!Template)
	{
		return;
	}

	WriteStanceIntoTemplate(*Layout, Template, StanceValue);

	// Memoised for the same reason the transform channel caches its own: the lookup builds the class
	// path into a string and hashes it, and this runs once per bot per emit.
	FCrowdyClassID& CachedStateClassID = StateClassIDCache.FindOrAdd(EntityClass, CROWDY_INVALID_CLASS_ID);
	if (CachedStateClassID == CROWDY_INVALID_CLASS_ID)
	{
		CachedStateClassID = UCrowdyClassRegistry::Get()->GetID(EntityClass);
	}

	FCrowdyStateDelta Delta;
	Delta.EntityID = CrowdEntityID;
	Delta.ClassID = static_cast<int64>(CachedStateClassID);
	Delta.LayoutHash = Layout->LayoutHash;

	// A sender that is emphatically not us, because our own id is the first thing the router drops on. It is
	// stable across a run so the receive side sees one peer rather than a new one per delta.
	Delta.SenderID = LocalDeliverySenderID;

	// Keyframe, so every property is present rather than only what moved against a shadow this path does not
	// keep. That is also the honest shape for a test of the receive side: it is what a late observer gets.
	FCrowdyStateCodec::Encode(*Layout, Template, TBitArray<>(true, Layout->Properties.Num()),
		/*bKeyframe=*/true, Delta.Blob);

	// The event borrows its payload, so the owner is a local that outlives the dispatch and no longer.
	const FInstancedStruct Payload = FInstancedStruct::Make(Delta);

	FCrowdyInboundEvent Inbound;
	Inbound.Payload = &Payload;
	Inbound.SenderID = Delta.SenderID;
	Router->DispatchEvent(Inbound);
}

void UCrowdyMassCrowdSimHarness::EmitRange(int32 FirstIndex, int32 LastIndex, UCrowdySDKSubsystem& SDK)
{
	// Accumulated into the frame's total rather than compared against it. The sweep calls this twice on
	// the frame it wraps the end of the interval, and what a send buffer has to absorb is both ranges
	// together; taking the larger half would under-report exactly the frame this number exists to catch.
	BurstThisFrame += LastIndex - FirstIndex;
	LargestBurst = FMath::Max(LargestBurst, BurstThisFrame);

	for (int32 Index = FirstIndex; Index < LastIndex; ++Index)
	{
		EmitBot(Bots[Index], SDK);
	}
}

bool UCrowdyMassCrowdSimHarness::Tick(float DeltaSeconds)
{
	if (!bRunning || Bots.IsEmpty())
	{
		return true;
	}

	// Movement integrates the real frame delta so a bot's path is the same shape whatever the
	// generator's frame rate, while emission is paced separately below.
	for (FCrowdyMassCrowdSimBot& Bot : Bots)
	{
		AdvanceBot(Bot, CrowdCentre, DeltaSeconds);
	}

	UGameInstance* GameInstance = GetGameInstance();
	UCrowdySDKSubsystem* SDK = GameInstance ? GameInstance->GetSubsystem<UCrowdySDKSubsystem>() : nullptr;
	if (!SDK)
	{
		return true;
	}

	++FramesSinceLastReport;

	// Zeroed at the top of every frame, so EmitRange can accumulate both halves of a wrapping sweep into
	// one figure.
	BurstThisFrame = 0;

	if (bAutoStatus && FPlatformTime::Seconds() >= NextAutoStatusTimeSeconds)
	{
		NextAutoStatusTimeSeconds = FPlatformTime::Seconds() + CrowdyMassCrowdSim::AutoStatusIntervalSeconds;
		Status();
	}

	const float Interval = CrowdyMassCrowdSim::EmissionIntervalSeconds;
	const int32 Count = Bots.Num();

	// A frame longer than a whole interval means every bot came due during it. The phase resynchronises
	// and that interval is skipped rather than emitted: a real crowd is N independent clients, so its
	// sends stay scattered no matter how long THIS process's frame was, and emitting the whole crowd
	// at once is the very burst the sweep below exists to avoid. It is also self-inflicted, because the
	// first frame after a level load is always long, so every run used to open by handing the socket
	// the entire population and losing whatever overran the outbound queue.
	//
	// Dropping one interval after a hitch is the faithful choice: those updates are stale by the time
	// the frame ends, and the next sweep sends fresher ones.
	if (DeltaSeconds >= Interval)
	{
		EmissionPhaseSeconds = 0.0f;
		return true;
	}

	const float NewPhase = EmissionPhaseSeconds + DeltaSeconds;
	const int32 FirstDue = FMath::Clamp(FMath::FloorToInt(EmissionPhaseSeconds / Interval * Count), 0, Count);
	const int32 LastDue = FMath::FloorToInt(NewPhase / Interval * Count);

	if (LastDue < Count)
	{
		EmitRange(FirstDue, LastDue, *SDK);
		EmissionPhaseSeconds = NewPhase;
		return true;
	}

	// The sweep crossed the end of the interval, so it covers the tail of the crowd and then wraps
	// onto the head.
	EmitRange(FirstDue, Count, *SDK);
	EmitRange(0, FMath::Clamp(LastDue - Count, 0, Count), *SDK);
	EmissionPhaseSeconds = NewPhase - Interval;

	return true;
}

bool UCrowdyMassCrowdSimHarness::Start(int32 BotCount, float RadiusMetres, UClass* InBotClass)
{
	Stop();

	UClass* ResolvedClass = InBotClass ? InBotClass : ACrowdyMassCrowdSimStateEntity::StaticClass();
	if (!ResolvedClass->IsChildOf(AActor::StaticClass()))
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.start: '%s' is not an Actor class, so it cannot be spawned as a simulated player."),
			*ResolvedClass->GetName());
		return false;
	}

	const AActor* ClassDefaultActor = ResolvedClass->GetDefaultObject<AActor>();
	if (!ClassDefaultActor || !ClassDefaultActor->FindComponentByClass<UCrowdyEntityComponent>())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.start: '%s' carries no UCrowdyEntityComponent, so it has no CrowdyState identity to spawn simulated players as."),
			*ResolvedClass->GetName());
		return false;
	}

	const int32 ResolvedCount = FMath::Clamp(
		BotCount > 0 ? BotCount : CrowdyMassCrowdSim::DefaultBotCount, 1, CrowdyMassCrowdSim::MaxBotCount);
	const float ResolvedRadius = RadiusMetres > 0.0f ? RadiusMetres : CrowdyMassCrowdSim::DefaultRadiusMetres;

	UE_CLOG(BotCount > CrowdyMassCrowdSim::MaxBotCount, LogCrowdyMass, Warning,
		TEXT("crowdy.mass.crowdsim.start: %d bots asked for, clamped to %d."), BotCount, CrowdyMassCrowdSim::MaxBotCount);

	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance || !GameInstance->GetSubsystem<UCrowdySDKSubsystem>())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.start: no Crowdy SDK subsystem, so nothing could be sent. Run this from a play session."));
		return false;
	}

	// Resolved once per run, and it must be the class the bots actually ARE, not the class whose state
	// struct they borrow.
	BotClass = ResolvedClass;
	BotClassID = UCrowdyClassRegistry::Get()->GetID(BotClass);
	StateClassIDCache.Reset();

	if (BotClassID == CROWDY_INVALID_CLASS_ID)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.start: '%s' has no wire class id, so every simulated update will be refused by the serializer. A class registration refused over an id clash is the usual cause."),
			*BotClass->GetName());
		return false;
	}

	BotFields = ResolveBotFields(BotClass);
	UE_CLOG(!BotFields.PayloadProperty, LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.start: '%s' carries no int32 property named Payload, so simulated players will send no payload change traffic."),
		*BotClass->GetName());
	UE_CLOG(!BotFields.StanceProperty, LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.start: '%s' carries no CrowdyState-shaped Stance property, so simulated players will send no stance traffic."),
		*BotClass->GetName());

	// Fixed rather than time-derived, so two runs of the same command produce the same crowd and can
	// be compared. Change it deliberately when a different crowd is wanted.
	RunSeed = 1;

	CrowdCentre = ResolveCrowdCentre();
	SeedBots(ResolvedCount, ResolvedRadius);

	// After seeding, because a bot's CrowdyState entity spawns at its seeded location. Before the first
	// tick, so every bot already has an entity to write into once EmitBot starts running.
	SpawnStateEntities();

	// After seeding, because it hands the SDK the ids to intercept and those do not exist until the bots
	// do. Before the first tick, because an update that escaped to the network in between would produce
	// a real entity this run then delivers a second copy of.
	if (bLocalDelivery)
	{
		ApplyLocalDelivery();
	}

	UpdatesEmitted = 0;
	LargestBurst = 0;
	UpdatesAtLastReport = 0;
	DropsAtLastReport = 0;
	DeferredAtLastReport = 0;
	BytesAtLastReport = 0;
	SentAtLastReport = 0;
	FramesSinceLastReport = 0;
	EmissionPhaseSeconds = 0.0f;
	StartTimeSeconds = FPlatformTime::Seconds();
	LastReportTimeSeconds = StartTimeSeconds;
	// Minted once per run, so a locally delivered delta claims one stable peer rather than a new one each
	// time. It must never be this client's own id, which the receive path drops before anything else.
	LocalDeliverySenderID = FGuid::NewGuid();

	bRunning = true;

	// Every run reports on a timer, not just an unattended one. Delivery being behind is silent by
	// nature, while the generator's own counters look healthy either way, so an unasked-for report is
	// the only thing that can say so.
	bAutoStatus = true;
	NextAutoStatusTimeSeconds = StartTimeSeconds + CrowdyMassCrowdSim::AutoStatusIntervalSeconds;

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UCrowdyMassCrowdSimHarness::Tick));

	// The centre is logged because it decides whether the observer can see any of this. An unattended
	// instance may have no pawn yet and fall back to the origin, which can be nowhere near the play
	// area; the crowd would then be emitted correctly and be invisible, which reads exactly like a
	// broken harness.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.start: %d simulated players wandering within %.0f m of %s, emitting every %.0f ms through the real send path. An observer has to be near that point to receive them. Measure on the OBSERVING client, not here."),
		ResolvedCount, ResolvedRadius, *CrowdCentre.ToCompactString(), CrowdyMassCrowdSim::EmissionIntervalSeconds * 1000.0f);

	UE_CLOG(CrowdCentre.IsNearlyZero(), LogCrowdyMass, Warning,
		TEXT("crowdy.mass.crowdsim.start: the crowd is centred on the world origin, which is the fallback used when no player pawn exists yet. If the play area is elsewhere, pass -crowdsimcenter=X=..,Y=..,Z=.. or the observer will receive nothing."));

	return true;
}

void UCrowdyMassCrowdSimHarness::Stop()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (bRunning)
	{
		UE_LOG(LogCrowdyMass, Log, TEXT("crowdy.mass.crowdsim.stop: stopped after emitting %lld updates for %d simulated players."),
			UpdatesEmitted, Bots.Num());
	}

	// Released whether or not this run armed it, so a stopped run can never leave a later session's
	// traffic being quietly intercepted for ids nothing is emitting.
	ClearLocalDelivery();

	// Unlike the transform-channel bots below, a CrowdyState entity is a real spawned UObject and has to be
	// torn down explicitly; see DestroyStateEntities.
	DestroyStateEntities();

	// The simulated players are simply abandoned. Nothing here removes them from the receiver, on
	// purpose: entity lifetime belongs to the tracker and the backend, and a generator that deleted
	// its own entities would be testing a teardown path the game does not have.
	bRunning = false;
	bAutoStatus = false;
	Bots.Reset();
	UpdatesEmitted = 0;
}

namespace
{
	// One trace file per pass, named by what the pass is for rather than by its index.
	const TCHAR* ProfilePassTag(int32 PassIndex)
	{
		switch (PassIndex)
		{
		case 0:  return TEXT("A-baseline");
		case 1:  return TEXT("B-actorwalk");
		default: return TEXT("C-statedecode");
		}
	}

	// What the pass measures, printed with its file so the reading does not have to be looked up.
	const TCHAR* ProfilePassPurpose(int32 PassIndex)
	{
		switch (PassIndex)
		{
		case 0:  return TEXT("no extra scopes, the only pass comparable against an earlier baseline. Read Crowdy_DeliverFrame, Crowdy_DeserializeActorState, Crowdy_DecodePayload_ActorUpdate and Crowdy_DispatchMessage");
		case 1:  return TEXT("crowdy.serialize.scopes on. Read Crowdy_ActorStateSerializeBin, nested inside Crowdy_DeserializeActorState");
		default: return TEXT("crowdy.state.scopes on and the state-change rate raised. Read Crowdy_DecodeStateDelta");
		}
	}

	IConsoleVariable* FindProfileScopeCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}
}

bool UCrowdyMassCrowdSimHarness::StartProfile(int32 BotCount, float TracedSecondsPerPass, float LastPassStateInterval, UClass* InBotClass)
{
	if (IsProfileSessionRunning())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.profile: a session is already in flight (pass %d of %d); ignoring this request."),
			ProfilePassIndex + 1, CrowdyMassCrowdSim::ProfilePassCount);
		return false;
	}

	ProfileBotCount = BotCount > 0 ? BotCount : CrowdyMassCrowdSim::DefaultProfileBotCount;
	ProfileBotClass = InBotClass;
	ProfileTraceSeconds = TracedSecondsPerPass > 0.0f ? TracedSecondsPerPass : CrowdyMassCrowdSim::DefaultProfileTraceSeconds;
	ProfileStateInterval = LastPassStateInterval > 0.0f
		? LastPassStateInterval
		: CrowdyMassCrowdSim::DefaultProfileStateInterval;

	// Captured before the first pass overwrites them, so the session can put everything back even if a
	// pass leaves a scope on.
	const IConsoleVariable* const SerializeScopes = FindProfileScopeCVar(TEXT("crowdy.serialize.scopes"));
	const IConsoleVariable* const StateScopes = FindProfileScopeCVar(TEXT("crowdy.state.scopes"));
	ProfileSavedSerializeScopes = SerializeScopes ? SerializeScopes->GetInt() : 0;
	ProfileSavedStateScopes = StateScopes ? StateScopes->GetInt() : 0;
	ProfileSavedStateInterval = StateChangeIntervalSeconds;

	ProfileSessionStamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	ProfileTraceFiles.Reset();
	ProfilePassIndex = 0;

	ProfileTickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UCrowdyMassCrowdSimHarness::ProfileTick));

	const float TotalSeconds = CrowdyMassCrowdSim::ProfilePassCount
		* (CrowdyMassCrowdSim::ProfileSettleSeconds + ProfileTraceSeconds + CrowdyMassCrowdSim::ProfileCooldownSeconds);
	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.profile: %d passes of %.0f traced seconds at %d simulated players, about %.0f s in total. Leave the session alone until the 'session complete' line."),
		CrowdyMassCrowdSim::ProfilePassCount, ProfileTraceSeconds, ProfileBotCount, TotalSeconds);

	BeginProfilePass();
	return true;
}

void UCrowdyMassCrowdSimHarness::ApplyProfilePass(int32 PassIndex)
{
	IConsoleVariable* const SerializeScopes = FindProfileScopeCVar(TEXT("crowdy.serialize.scopes"));
	IConsoleVariable* const StateScopes = FindProfileScopeCVar(TEXT("crowdy.state.scopes"));

	// Exactly one extra scope per pass, because each nests inside a scope that is itself measured and two
	// at once would leave neither figure attributable.
	if (SerializeScopes)
	{
		SerializeScopes->Set(PassIndex == 1 ? 1 : 0, ECVF_SetByConsole);
	}
	if (StateScopes)
	{
		StateScopes->Set(PassIndex == 2 ? 1 : 0, ECVF_SetByConsole);
	}

	// The last pass raises the on-change rate so CrowdyState deltas are a real share of the delivered mix.
	// At the default interval they are a rounding error next to the transform stream, and a scope over a
	// rounding error sizes nothing.
	SetStateChangeInterval(PassIndex == 2 ? ProfileStateInterval : ProfileSavedStateInterval);
}

void UCrowdyMassCrowdSimHarness::BeginProfilePass()
{
	ApplyProfilePass(ProfilePassIndex);

	SetLocalDelivery(true, CrowdyMassCrowdSim::ProfileLatencyMs, CrowdyMassCrowdSim::ProfileJitterMs, 0.0f);
	const bool bPassStarted = Start(ProfileBotCount, 0.0f, ProfileBotClass);
	UE_CLOG(!bPassStarted, LogCrowdyMass, Warning,
		TEXT("crowdy.mass.crowdsim.profile: pass %d of %d could not start the generator; this pass will trace an empty crowd."),
		ProfilePassIndex + 1, CrowdyMassCrowdSim::ProfilePassCount);

	ProfileStep = EProfileStep::Settle;
	ProfileStepDeadlineSeconds = FPlatformTime::Seconds() + CrowdyMassCrowdSim::ProfileSettleSeconds;

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.profile: pass %d of %d (%s) settling for %.0f s before the trace opens."),
		ProfilePassIndex + 1, CrowdyMassCrowdSim::ProfilePassCount, ProfilePassTag(ProfilePassIndex),
		CrowdyMassCrowdSim::ProfileSettleSeconds);
}

bool UCrowdyMassCrowdSimHarness::ProfileTick(float /*DeltaSeconds*/)
{
	if (ProfileStep == EProfileStep::Idle)
	{
		return true;
	}

	// Wall clock rather than accumulated frame deltas, so a hitching frame cannot shorten a trace window.
	if (FPlatformTime::Seconds() < ProfileStepDeadlineSeconds)
	{
		return true;
	}

	if (ProfileStep == EProfileStep::Settle)
	{
		// A trace already open, from a -trace command line or a previous pass, would make Start a no-op and
		// put every pass in one file. Closing first is what keeps the passes separable.
		FTraceAuxiliary::Stop();

		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProfilingDir()
			/ FString::Printf(TEXT("HotPath-%s-%s.utrace"), *ProfileSessionStamp, ProfilePassTag(ProfilePassIndex)));

		FTraceAuxiliary::FOptions Options;
		Options.bTruncateFile = true;
		const bool bStarted = FTraceAuxiliary::Start(
			FTraceAuxiliary::EConnectionType::File, *Path, TEXT("cpu,frame,bookmark"), &Options);

		if (bStarted)
		{
			ProfileTraceFiles.Add(Path);
		}

		// Warning rather than an abort: the remaining passes are still worth running, and a pass with no
		// file is far less confusing than a session that stops without saying why.
		UE_CLOG(!bStarted, LogCrowdyMass, Warning,
			TEXT("crowdy.mass.crowdsim.profile: pass %d could not open a trace at '%s'; it will run but produce no file."),
			ProfilePassIndex + 1, *Path);

		ProfileStep = EProfileStep::Tracing;
		ProfileStepDeadlineSeconds = FPlatformTime::Seconds() + ProfileTraceSeconds;
		return true;
	}

	if (ProfileStep == EProfileStep::Tracing)
	{
		// Inside the trace window on purpose: the report is what carries the delivery budgets, and reading
		// them against a window they do not cover is how a run gets misread.
		Status();

		FTraceAuxiliary::Stop();
		Stop();

		ProfileStep = EProfileStep::Cooldown;
		ProfileStepDeadlineSeconds = FPlatformTime::Seconds() + CrowdyMassCrowdSim::ProfileCooldownSeconds;
		return true;
	}

	++ProfilePassIndex;
	if (ProfilePassIndex >= CrowdyMassCrowdSim::ProfilePassCount)
	{
		FinishProfileSession();
		return true;
	}

	BeginProfilePass();
	return true;
}

void UCrowdyMassCrowdSimHarness::FinishProfileSession()
{
	RestoreProfileSettings();

	if (ProfileTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ProfileTickHandle);
		ProfileTickHandle.Reset();
	}

	ProfileStep = EProfileStep::Idle;

	UE_LOG(LogCrowdyMass, Log, TEXT("crowdy.mass.crowdsim.profile: session complete, %d trace file(s) written."),
		ProfileTraceFiles.Num());

	for (int32 Index = 0; Index < ProfileTraceFiles.Num(); ++Index)
	{
		UE_LOG(LogCrowdyMass, Log, TEXT("  %s  -  %s"), *ProfileTraceFiles[Index], ProfilePassPurpose(Index));
	}

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.profile: read the delivery budgets from the 'local delivery:' lines above, never from the inbound-pressure report."));
}

void UCrowdyMassCrowdSimHarness::RestoreProfileSettings()
{
	if (IConsoleVariable* const SerializeScopes = FindProfileScopeCVar(TEXT("crowdy.serialize.scopes")))
	{
		SerializeScopes->Set(ProfileSavedSerializeScopes, ECVF_SetByConsole);
	}
	if (IConsoleVariable* const StateScopes = FindProfileScopeCVar(TEXT("crowdy.state.scopes")))
	{
		StateScopes->Set(ProfileSavedStateScopes, ECVF_SetByConsole);
	}

	SetStateChangeInterval(ProfileSavedStateInterval);
}

void UCrowdyMassCrowdSimHarness::Status()
{
	if (!bRunning)
	{
		UE_LOG(LogCrowdyMass, Log, TEXT("crowdy.mass.crowdsim.status: not running."));
		return;
	}

	const double ElapsedSeconds = FMath::Max(FPlatformTime::Seconds() - StartTimeSeconds, UE_KINDA_SMALL_NUMBER);

	// The rate actually achieved, not the rate asked for. A generator that has fallen behind its
	// interval is emitting a smaller crowd than the one the observer's numbers will be attributed
	// to, and nothing else in the session would say so.
	const double UpdatesPerSecond = UpdatesEmitted / ElapsedSeconds;
	const double UpdatesPerBotPerSecond = UpdatesPerSecond / FMath::Max(Bots.Num(), 1);

	int32 PhaseCounts[5] = {0, 0, 0, 0, 0};
	TMap<FIntVector, int32> Occupancy;
	Occupancy.Reserve(Bots.Num());

	for (const FCrowdyMassCrowdSimBot& Bot : Bots)
	{
		++PhaseCounts[static_cast<int32>(Bot.Phase)];

		const FIntVector Chunk(
			FMath::FloorToInt(Bot.Location.X / CrowdyMassCrowdSim::ChunkSizeCentimetres),
			FMath::FloorToInt(Bot.Location.Y / CrowdyMassCrowdSim::ChunkSizeCentimetres),
			0);
		++Occupancy.FindOrAdd(Chunk);
	}

	int32 BusiestChunk = 0;
	for (const TPair<FIntVector, int32>& Entry : Occupancy)
	{
		BusiestChunk = FMath::Max(BusiestChunk, Entry.Value);
	}

	UE_LOG(LogCrowdyMass, Log,
		TEXT("crowdy.mass.crowdsim.status: %d simulated players, %lld updates in %.1f s (%.0f/s overall, %.1f/s per player against a target of %.0f)."),
		Bots.Num(), UpdatesEmitted, ElapsedSeconds, UpdatesPerSecond, UpdatesPerBotPerSecond,
		1.0f / CrowdyMassCrowdSim::EmissionIntervalSeconds);

	UE_LOG(LogCrowdyMass, Log,
		TEXT("  movement: %d standing, %d walking, %d jogging, %d running, %d holding at a gait threshold"),
		PhaseCounts[0], PhaseCounts[1], PhaseCounts[2], PhaseCounts[3], PhaseCounts[4]);

	// The generator's own frame rate, which decides whether emissions can be spread at all. A tick
	// slower than the emission interval means every bot comes due every frame, so the whole crowd is
	// handed to the socket at once no matter how the spreading is written. Reported first because it
	// explains the burst below rather than being explained by it.
	const double ReportSeconds = FMath::Max(FPlatformTime::Seconds() - LastReportTimeSeconds, UE_KINDA_SMALL_NUMBER);
	const double FramesPerSecond = FramesSinceLastReport / ReportSeconds;

	UE_LOG(LogCrowdyMass, Log,
		TEXT("  generator running at %.0f frames a second, handing the socket at most %d updates in one frame."),
		FramesPerSecond, LargestBurst);

	UE_CLOG(FramesPerSecond < 1.0f / CrowdyMassCrowdSim::EmissionIntervalSeconds * 2.0f, LogCrowdyMass, Warning,
		TEXT("  the generator ticks too slowly to spread its sends: at %.0f frames a second against an update every %.0f ms, every player comes due every frame and the whole crowd reaches the socket at once. Run the generator on an empty map rather than the game's own level, which it does not need."),
		FramesPerSecond, CrowdyMassCrowdSim::EmissionIntervalSeconds * 1000.0f);

	// Local delivery replaces the transport entirely, so reporting the transport counters beside it
	// would print a healthy-looking zero for every one of them. Reported instead is what local delivery
	// did, which answers a different question and says so.
	const bool bDeliveringLocally = GetGameInstance()
		&& GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>()
		&& GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>()->IsLocalDeliveryArmed();

	if (bDeliveringLocally)
	{
		ReportLocalDelivery(ReportSeconds);
	}

	// What the transport did with all that, which is the half this harness cannot see from its own
	// counters. Emitting and delivering are different numbers, and only their difference explains an
	// observer watching entities update late.
	if (const UCrowdyCppReplicationSubsystem* Replication = !bDeliveringLocally && GetGameInstance() ? GetGameInstance()->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr)
	{
		if (const TSharedPtr<FCrowdyCppReplication> Connection = Replication->GetConnection())
		{
			const FCrowdyCppReplicationStats Stats = Connection->GetStats();

			// Reported as a delta since the last report, not as a running total. A cumulative
			// percentage falls steadily however the run is going, so a problem that stopped a minute
			// ago and one that is happening right now read almost identically in it. That is not
			// hypothetical: a run whose drops all happened in its first five seconds went on
			// reporting a drop percentage for the rest of its life, which invited blaming those
			// drops for a symptom that outlasted them by half a minute.
			const int64 DropsThisInterval = Stats.SendsDropped - DropsAtLastReport;
			DropsAtLastReport = Stats.SendsDropped;

			// A full kernel send buffer is not counted as a drop, so without this line a saturating
			// socket reports as perfectly healthy. The two need different answers: a drop is a fault or
			// a full outbound queue, a deferral is backpressure and the lever is SocketSendBufferBytes.
			const int64 DeferredThisInterval = Stats.SendsDeferred - DeferredAtLastReport;
			DeferredAtLastReport = Stats.SendsDeferred;

			UE_LOG(LogCrowdyMass, Log,
				TEXT("  transport: %lld sent in total, %lld dropped in total, %lld dropped in the last %.0f s."),
				Stats.MessagesSent, Stats.SendsDropped, DropsThisInterval, ReportSeconds);

			// Datagrams against messages is what says whether bundling is on and packing anything: equal
			// counts mean one message per datagram, and the wrapper count is the number of datagrams that
			// carried two or more.
			UE_LOG(LogCrowdyMass, Log,
				TEXT("  datagrams: %lld sent in total carrying %lld messages, %lld of them bundles, %lld messages lost with a failed bundle."),
				Stats.DatagramsSent, Stats.MessagesSent, Stats.BundlesSent, Stats.MessagesDropped);

			UE_LOG(LogCrowdyMass, Log,
				TEXT("  send buffer: %lld deferred in total, %lld in the last %.0f s, %lld genuine socket faults."),
				Stats.SendsDeferred, DeferredThisInterval, ReportSeconds, Stats.SendsFailed);

			// The load this process is actually offering the network. A crowd emulated from one machine
			// puts a whole population's upstream through a single link, which a real crowd never does:
			// this many players is that many separate uplinks, not one carrying all of them. Reported in
			// the units an uplink is sold in, because that is the number to compare it against.
			const int64 BytesThisInterval = Stats.BytesSent - BytesAtLastReport;
			const int64 SentThisInterval = Stats.MessagesSent - SentAtLastReport;
			BytesAtLastReport = Stats.BytesSent;
			SentAtLastReport = Stats.MessagesSent;

			// Offered is what was ASKED of the link, so it counts the deferred datagrams too. Deriving it
			// from BytesSent alone reports what got through, which falls as saturation worsens and so
			// reads backwards exactly when the number is wanted. Delivered is reported beside it, and the
			// gap between the two is the shortfall.
			const int64 OfferedThisInterval = SentThisInterval + DeferredThisInterval;
			const double BytesPerDatagram = SentThisInterval > 0
				? static_cast<double>(BytesThisInterval) / SentThisInterval : 0.0;
			const double OfferedMegabitsPerSecond =
				OfferedThisInterval * BytesPerDatagram * 8.0 / ReportSeconds / 1000000.0;
			const double DeliveredMegabitsPerSecond = BytesThisInterval * 8.0 / ReportSeconds / 1000000.0;

			UE_LOG(LogCrowdyMass, Log,
				TEXT("  offered load: %.1f Mbit/s upstream (%.1f delivered), %.0f datagrams a second, %.0f bytes each. This is one machine carrying what %d separate clients would each carry a fraction of."),
				OfferedMegabitsPerSecond, DeliveredMegabitsPerSecond,
				OfferedThisInterval / ReportSeconds, BytesPerDatagram, Bots.Num());

			// Deferred datagrams never left, so this is real outbound loss even though nothing failed.
			// Said separately from the drop warning because the cause is different: the link cannot
			// carry what is being offered, and no SDK setting changes how much fits down a wire. The
			// population the link does carry is stated outright, because that is the actionable number.
			if (DeferredThisInterval > 0)
			{
				const double DeliveredFraction = OfferedThisInterval > 0
					? static_cast<double>(SentThisInterval) / OfferedThisInterval : 1.0;

				UE_LOG(LogCrowdyMass, Warning,
					TEXT("  the uplink is saturated, not the SDK: %lld of %lld updates (%.1f%%) could not be handed to the network in the last %.0f s. The link is carrying %.1f Mbit/s of the %.1f asked of it, so it holds about %d of the %d simulated players. Lower the population, or run the generator somewhere with more upstream (a VM in the server's region)."),
					DeferredThisInterval, OfferedThisInterval, (1.0 - DeliveredFraction) * 100.0, ReportSeconds,
					DeliveredMegabitsPerSecond, OfferedMegabitsPerSecond,
					FMath::FloorToInt(Bots.Num() * DeliveredFraction), Bots.Num());
			}

			// Only claimed while it is still happening. The effect on an observer is not "fewer
			// entities" but "entities that move wrongly", so a stale warning here sends the next
			// person hunting in interpolation for a cause that stopped long ago.
			UE_CLOG(DropsThisInterval > 0, LogCrowdyMass, Warning,
				TEXT("  %lld outbound updates are being lost right now, so jitter the observer sees is those gaps rather than the receiving client."),
				DropsThisInterval);

			UE_CLOG(DropsThisInterval == 0 && Stats.SendsDropped > 0, LogCrowdyMass, Log,
				TEXT("  no sends were lost in the last %.0f s, so the %lld earlier losses are start-up only and do NOT explain anything the observer sees now."),
				ReportSeconds, Stats.SendsDropped);
		}
	}

	// The number the scale budget's "relevant entities" assumption was always standing in for. An
	// observer's cost is driven by how many players share its neighbourhood, not by how many exist,
	// so a crowd's entity count means nothing without this alongside it.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("  spread over %d chunks, busiest holding %d players, %.1f per chunk on average. This is the number an observer's frame cost should be read against, not the total."),
		Occupancy.Num(), BusiestChunk, static_cast<float>(Bots.Num()) / FMath::Max(Occupancy.Num(), 1));

	// Rolled at the end so every figure above describes the same interval.
	LargestBurst = 0;
	FramesSinceLastReport = 0;
	UpdatesAtLastReport = UpdatesEmitted;
	LastReportTimeSeconds = FPlatformTime::Seconds();
}

#if !UE_BUILD_SHIPPING
namespace
{
	UCrowdyMassCrowdSimHarness* ResolveCrowdyMassCrowdSimHarness(UWorld* World)
	{
		UCrowdyMassCrowdSimHarness* Harness = UCrowdyMassCrowdSimHarness::Get(World);
		if (!Harness)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("crowdy.mass.crowdsim: no harness for this world, so run this from a play session."));
		}
		return Harness;
	}

	// Resolves the optional trailing class argument a console command takes: a short name, an asset
	// path or a full object path, same as any other class-naming console argument in this module. Empty
	// or absent means the caller keeps whatever null means to it (the plugin's own fixture).
	UClass* ResolveCrowdSimConsoleBotClass(const TArray<FString>& Args, int32 ArgIndex, const TCHAR* CommandName)
	{
		if (Args.Num() <= ArgIndex || Args[ArgIndex].IsEmpty())
		{
			return nullptr;
		}

		FString Why;
		UClass* Resolved = FCrowdyMassClassDerivationReport::ResolveActorClass(Args[ArgIndex], Why);
		UE_CLOG(!Resolved, LogCrowdyMass, Warning, TEXT("%s: '%s' could not be resolved (%s)."),
			CommandName, *Args[ArgIndex], *Why);
		return Resolved;
	}
}

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassCrowdSimStart(
	TEXT("crowdy.mass.crowdsim.start"),
	TEXT("Crowd simulation: emit position updates for N simulated players through the real send path. Args: [count] [radiusMetres] [class]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassCrowdSimHarness* H = ResolveCrowdyMassCrowdSimHarness(World))
			{
				UClass* BotClass = ResolveCrowdSimConsoleBotClass(Args, 2, TEXT("crowdy.mass.crowdsim.start"));
				H->Start(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0,
					Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f,
					BotClass);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassCrowdSimStop(
	TEXT("crowdy.mass.crowdsim.stop"),
	TEXT("Crowd simulation: stop emitting. The simulated players are left to the tracker's normal timeout."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassCrowdSimHarness* H = ResolveCrowdyMassCrowdSimHarness(World)) { H->Stop(); }
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassCrowdSimLocal(
	TEXT("crowdy.mass.crowdsim.local"),
	TEXT("Crowd simulation: deliver the simulated crowd inside this process after a delay instead of sending it, so a "
		"population past what this machine's uplink carries can still be observed here. Args: <0|1> [latencyMs] "
		"[jitterMs] [lossPercent]. Measures what this client costs to receive a crowd; measures nothing the server "
		"decides."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UCrowdyMassCrowdSimHarness* Harness = ResolveCrowdyMassCrowdSimHarness(World);
			if (!Harness)
			{
				return;
			}

			// Negatives leave the setting alone, so a caller passing only the first argument keeps
			// whatever latency was already configured rather than silently resetting it.
			Harness->SetLocalDelivery(
				Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : true,
				Args.Num() > 1 ? FCString::Atof(*Args[1]) : -1.0f,
				Args.Num() > 2 ? FCString::Atof(*Args[2]) : -1.0f,
				Args.Num() > 3 ? FCString::Atof(*Args[3]) : -1.0f);
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassCrowdSimState(
	TEXT("crowdy.mass.crowdsim.state"),
	TEXT("Crowd simulation: how often, on average in seconds, one simulated player's CrowdyState payload changes. "
		"Args: <intervalSeconds>, where 0 or negative stops the changes and leaves only the keyframe heartbeat. "
		"Change rate and heartbeat rate are separate costs: this sets the first, the map profile sets the second."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UCrowdyMassCrowdSimHarness* Harness = ResolveCrowdyMassCrowdSimHarness(World);
			if (!Harness)
			{
				return;
			}

			// No argument reports the interval in force rather than choosing one, so asking what the
			// setting is cannot change it.
			Harness->SetStateChangeInterval(Args.Num() > 0 ? FCString::Atof(*Args[0]) : Harness->GetStateChangeInterval());
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassCrowdSimProfile(
	TEXT("crowdy.mass.crowdsim.profile"),
	TEXT("Crowd simulation: run the whole receive-cost profiling session unattended and write one .utrace "
		"per pass into Saved/Profiling. Starts and stops the tracing itself, so launch WITHOUT -trace or the "
		"first connection wins and every pass lands in one file. Args: [botCount] [tracedSecondsPerPass] "
		"[stateChangeIntervalForTheLastPass] [class]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UCrowdyMassCrowdSimHarness* Harness = ResolveCrowdyMassCrowdSimHarness(World);
			if (!Harness)
			{
				return;
			}

			UClass* BotClass = ResolveCrowdSimConsoleBotClass(Args, 3, TEXT("crowdy.mass.crowdsim.profile"));
			Harness->StartProfile(
				Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0,
				Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f,
				Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.0f,
				BotClass);
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassDerive(
	TEXT("crowdy.mass.derive"),
	TEXT("Says what a Mass entity config derived from an actor class would contain, without spawning anything. Args: <class>, a short name, an asset path or a full object path."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() == 0 || Args[0].IsEmpty())
			{
				UE_LOG(LogCrowdyMass, Warning, TEXT("Usage: crowdy.mass.derive <class>, a short name, an asset path or a full object path."));
				return;
			}

			FString Why;
			const UClass* ActorClass = FCrowdyMassClassDerivationReport::ResolveActorClass(Args[0], Why);
			if (!ActorClass)
			{
				UE_LOG(LogCrowdyMass, Warning, TEXT("crowdy.mass.derive: %s"), *Why);
				return;
			}

			const FCrowdyMassClassDerivationReport Report = FCrowdyMassClassDerivationReport::ForClass(ActorClass, World);
			const bool bWarn = !Report.bMeshResolved || !Report.Materials.IsClear() || Report.bAuthoredConfigWins;
			UE_CLOG(!bWarn, LogCrowdyMass, Log, TEXT("crowdy.mass.derive: %s"), *Report.ToLine());
			UE_CLOG(bWarn, LogCrowdyMass, Warning, TEXT("crowdy.mass.derive: %s"), *Report.ToLine());
		}));

static FAutoConsoleCommandWithWorldAndArgs GCrowdyMassCrowdSimStatus(
	TEXT("crowdy.mass.crowdsim.status"),
	TEXT("Crowd simulation: report the rate actually achieved, what the simulated players are doing, and how they are spread over chunks."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UCrowdyMassCrowdSimHarness* H = ResolveCrowdyMassCrowdSimHarness(World)) { H->Status(); }
		}));
#endif
