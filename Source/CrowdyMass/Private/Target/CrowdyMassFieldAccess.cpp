#include "Target/CrowdyMassFieldAccess.h"

#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "MassEntityManager.h"
#include "StructUtils/StructView.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates, EReloadCompleteReason, FindObject
#include "UObject/UnrealType.h"

namespace CrowdyMassFields
{

namespace
{
	// A refusal repeats for as long as its cause lasts (a name nobody registered, a field read as the wrong
	// type, a graph being run from inside the Mass schedule), so each field name gets at most one line per
	// interval rather than one per call. Same granularity and interval as the event library's own send-side
	// refusal throttle.
	constexpr double CrowdyMassFieldRefuseWarnIntervalSeconds = 5.0;

	// Registered bindings, keyed by the name a graph asks for.
	//
	// Process-wide, where the view-state ingestor registry it shares its key with is world-scoped, and the
	// difference is deliberate. An ingestor decides what an arriving byte DOES, so one world's ingestors
	// answering for another world's decode would be wrong. A binding is a fact about a C++ type: which
	// property of which fragment a name means, fixed when the fragment was compiled and identical in every
	// world the process runs. Making it world-scoped would only mean registering it twice.
	TMap<FName, FFieldBinding>& Registry()
	{
		static TMap<FName, FFieldBinding> Fields;
		return Fields;
	}

	TMap<FName, double>& RefusalLogTimes()
	{
		static TMap<FName, double> LastLoggedSeconds;
		return LastLoggedSeconds;
	}

#if WITH_EDITOR
	/**
	 * Keeps the registry's bindings pointing at the reflection data that exists now.
	 *
	 * Live Coding and hot reload replace a fragment's UScriptStruct and destroy the properties the old one
	 * owned. The engine broadcasts reload-complete once that rebuild is finished and before the collection
	 * that frees what it replaced, which is the one moment at which every binding can be re-resolved while
	 * the names it was registered under still mean something.
	 *
	 * The binding is held by an object rather than by a bare handle so that it is removed again when this
	 * module's DLL unloads. A delegate left bound into unloaded code is the mirror image of the dangling
	 * property this exists to prevent, so trading one for the other would be no improvement. The editor is
	 * always a modular build, so CoreUObject is unloaded after this module and its delegate is still alive
	 * when this destructor runs; a monolithic build compiles none of this, because nothing in one can reload.
	 */
	FDelegateHandle GReloadRebindHandle;

	void OnReloadComplete(EReloadCompleteReason)
	{
		RebindRegisteredFields();
	}

	class FReloadRebindInstaller
	{
	public:
		FReloadRebindInstaller()
		{
			GReloadRebindHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddStatic(&OnReloadComplete);
		}

		~FReloadRebindInstaller()
		{
			if (!GReloadRebindHandle.IsValid())
			{
				return;
			}

			FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(GReloadRebindHandle);
			GReloadRebindHandle.Reset();
		}
	};
#endif

	void EnsureReloadRebindInstalled()
	{
#if WITH_EDITOR
		// Registry() first, so the map is constructed before the installer and therefore destroyed after it.
		// Function-local statics are destroyed in reverse order of construction, and the delegate has to be
		// unbound while the map its handler walks is still alive.
		Registry();

		static FReloadRebindInstaller Installer;
#endif
	}

#if WITH_DEV_AUTOMATION_TESTS
	// Set only by SetEntityManagerForTest; ResolveEntityManager prefers it over the target's own world.
	FMassEntityManager* GEntityManagerForTest = nullptr;
#endif

	bool ShouldLogRefusal(const FName FieldName)
	{
		TMap<FName, double>& LastLoggedSeconds = RefusalLogTimes();

		const double Now = FPlatformTime::Seconds();
		if (const double* Last = LastLoggedSeconds.Find(FieldName))
		{
			if (Now - *Last < CrowdyMassFieldRefuseWarnIntervalSeconds)
			{
				return false;
			}
		}

		LastLoggedSeconds.Add(FieldName, Now);
		return true;
	}

	/**
	 * Everything a read and a write both have to be true of before either touches chunk storage.
	 *
	 * The Mass-processing refusal is the one that matters, and it is reported at Error for a write and at
	 * Warning for a read because they fail differently. A write during the schedule races every processor
	 * declared to read that fragment and can leave an entity holding half of two values. A read during the
	 * schedule only returns a value that may be a frame's work out of date. Neither is performed.
	 */
	EAccessResult CheckAccessible(const FMassEntityManager& EntityManager, const FCrowdyMassEntityTarget& Target,
		const bool bWrite, const FName FieldName)
	{
		if (!IsInGameThread())
		{
			// Not throttled, because the throttle's own storage is game-thread-only and touching it from
			// here would be a second instance of the bug being reported.
			UE_LOG(LogCrowdyMass, Error,
				TEXT("[CrowdyMassFields] '%s' was %s off the game thread and has been refused. Mass entity data is game thread only."),
				*FieldName.ToString(), bWrite ? TEXT("written") : TEXT("read"));
			return EAccessResult::OffGameThread;
		}

		if (EntityManager.IsProcessing())
		{
			if (ShouldLogRefusal(FieldName))
			{
				UE_CLOG(bWrite, LogCrowdyMass, Error,
					TEXT("[CrowdyMassFields] '%s' was written while Mass processors are running, and the write has been refused. ")
					TEXT("It would have raced every processor declared to read that fragment. Two things put a caller here: a graph ")
					TEXT("run from a delegate broadcast inside a chunk loop, and an ordinary tick that overlaps a Mass phase, since ")
					TEXT("phases run alongside the game thread rather than blocking it. Move the write to a phase-end delegate, or ")
					TEXT("defer the broadcast to the end of the frame, and the same graph works."),
					*FieldName.ToString());

				UE_CLOG(!bWrite, LogCrowdyMass, Warning,
					TEXT("[CrowdyMassFields] '%s' was read while Mass processors are running, and the read has been refused. ")
					TEXT("The schedule may be writing that value right now, so nothing here could answer with it. Read from a ")
					TEXT("phase-end delegate, or from a deferred broadcast, instead."),
					*FieldName.ToString());
			}

			return EAccessResult::DuringMassProcessing;
		}

		// Built, not merely valid. A reserved entity that has not been built yet passes IsEntityValid (its
		// slot is in range and its serial matches) but has no archetype, so reading a fragment off one is a
		// null dereference rather than a miss.
		if (!EntityManager.IsEntityValid(Target.Entity) || !EntityManager.IsEntityBuilt(Target.Entity))
		{
			return EAccessResult::InvalidEntity;
		}

		return EAccessResult::Ok;
	}
}

bool RegisterField(const FName FieldName, const UScriptStruct* FragmentType, const FName FragmentPropertyName)
{
	check(IsInGameThread());

	// On the first registration of the process, so that a binding is never held across a code reload without
	// something re-resolving it afterwards.
	EnsureReloadRebindInstalled();

	if (FieldName.IsNone() || !FragmentType || FragmentPropertyName.IsNone())
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassFields] A field registration needs a name, a fragment type and a property name; got '%s' on '%s::%s'."),
			*FieldName.ToString(), *GetNameSafe(FragmentType), *FragmentPropertyName.ToString());
		return false;
	}

	if (!FragmentType->IsChildOf(FMassFragment::StaticStruct()))
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassFields] '%s' cannot be registered: '%s' does not derive from FMassFragment, so no entity carries one."),
			*FieldName.ToString(), *FragmentType->GetName());
		return false;
	}

	// The identity fragment is where every send learns who an entity is. It is the one
	// fragment a graph must not be able to rewrite: changing the id here points the entity's network
	// traffic at somebody else, and changing the role decides whether this client may originate events for
	// it. Neither is a visual.
	if (FragmentType == FCrowdyRemoteIdentityFragment::StaticStruct())
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassFields] '%s' cannot be registered: the remote identity fragment carries the entity's id, owner and role, ")
			TEXT("which is what routing and authority are decided from. Anything that has to be decided from somewhere trustworthy ")
			TEXT("has to be decided by the server instead."),
			*FieldName.ToString());
		return false;
	}

	const FProperty* Property = FragmentType->FindPropertyByName(FragmentPropertyName);
	if (!Property)
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassFields] '%s' cannot be registered: '%s' has no UPROPERTY named '%s'. A plain C++ member has no ")
			TEXT("reflection entry at all and can never be found by name, so mark it UPROPERTY() to make it addressable."),
			*FieldName.ToString(), *FragmentType->GetName(), *FragmentPropertyName.ToString());
		return false;
	}

	if (const FFieldBinding* Existing = Registry().Find(FieldName))
	{
		if (Existing->FragmentType == FragmentType && Existing->Property == Property)
		{
			// Registering the same binding again is what a per-world startup path does on the second world,
			// so it succeeds rather than reporting a conflict with itself.
			return true;
		}

		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassFields] '%s' is already registered on '%s::%s'; keeping it and refusing '%s::%s'. A field name is ")
			TEXT("claimed process-wide, so a second binding under the same name would mean one of the two silently answering for the other."),
			*FieldName.ToString(),
			*GetNameSafe(Existing->FragmentType), *Existing->PropertyName.ToString(),
			*FragmentType->GetName(), *Property->GetName());
		return false;
	}

	FFieldBinding Binding;
	Binding.FragmentType = FragmentType;
	Binding.Property = Property;
	// Kept alongside the resolved pointers, and the only part of a binding that survives a code reload
	// intact. Everything a rebind needs comes from here.
	Binding.FragmentTypePath = FragmentType->GetStructPathName();
	Binding.PropertyName = FragmentPropertyName;
	Registry().Add(FieldName, Binding);

	return true;
}

int32 RebindRegisteredFields()
{
	check(IsInGameThread());

	TMap<FName, FFieldBinding>& Fields = Registry();

	int32 NumMoved = 0;
	TArray<FName, TInlineAllocator<4>> Unresolved;

	for (TPair<FName, FFieldBinding>& Pair : Fields)
	{
		FFieldBinding& Binding = Pair.Value;

		// Resolved from the stored names alone. The pointers the binding already holds are exactly what a
		// reload may have invalidated, so nothing here reads either of them, not even to name the fragment in
		// a message.
		const UScriptStruct* Fresh = Binding.FragmentTypePath.IsValid()
			? FindObject<UScriptStruct>(Binding.FragmentTypePath)
			: nullptr;
		const FProperty* Property = Fresh ? Fresh->FindPropertyByName(Binding.PropertyName) : nullptr;

		const TCHAR* Lost = nullptr;
		if (!Fresh)
		{
			Lost = TEXT("no struct of that name exists any more");
		}
		else if (!Fresh->IsChildOf(FMassFragment::StaticStruct()))
		{
			Lost = TEXT("that struct no longer derives from FMassFragment, so no entity carries one");
		}
		else if (!Property)
		{
			Lost = TEXT("that struct no longer declares a UPROPERTY of that name");
		}

		if (Lost)
		{
			UE_LOG(LogCrowdyMass, Error,
				TEXT("[CrowdyMassFields] '%s' was registered on '%s::%s' and stopped resolving when native code was reloaded, so it ")
				TEXT("has been unregistered: %s. Reads and writes of that name now answer as though it was never registered. Restore ")
				TEXT("the member, or register the field against the fragment that carries it now."),
				*Pair.Key.ToString(), *Binding.FragmentTypePath.ToString(), *Binding.PropertyName.ToString(), Lost);

			Unresolved.Add(Pair.Key);
			continue;
		}

		NumMoved += (Binding.FragmentType != Fresh || Binding.Property != Property) ? 1 : 0;
		Binding.FragmentType = Fresh;
		Binding.Property = Property;
	}

	// Dropped after the walk rather than during it: removing from a map while iterating it is not defined.
	for (const FName FieldName : Unresolved)
	{
		Fields.Remove(FieldName);
	}

	UE_CLOG(NumMoved > 0 || Unresolved.Num() > 0, LogCrowdyMass, Log,
		TEXT("[CrowdyMassFields] %d field binding(s) moved with a code reload and were re-resolved; %d stopped resolving and were unregistered."),
		NumMoved, Unresolved.Num());

	return Unresolved.Num();
}

const FFieldBinding* FindField(const FName FieldName)
{
	return Registry().Find(FieldName);
}

bool IsFieldRegistered(const FName FieldName)
{
	return Registry().Contains(FieldName);
}

EAccessResult AccessFieldIn(FMassEntityManager& EntityManager, const FCrowdyMassEntityTarget& Target,
	const FName FieldName, const bool bWrite, FFieldVisitor Visitor)
{
	const EAccessResult Accessible = CheckAccessible(EntityManager, Target, bWrite, FieldName);
	if (Accessible != EAccessResult::Ok)
	{
		return Accessible;
	}

	const FFieldBinding* Binding = Registry().Find(FieldName);
	if (!Binding || !Binding->FragmentType || !Binding->Property)
	{
		if (ShouldLogRefusal(FieldName))
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassFields] Nothing is registered as '%s', so there is no fragment field to reach. Register it with ")
				TEXT("CrowdyMassFields::RegisterField, under the same name its view-state ingestor is registered with."),
				*FieldName.ToString());
		}
		return EAccessResult::UnknownField;
	}

	// The pointer below points into Mass chunk storage and is valid for this call only. It is passed to the
	// visitor and never returned, stored, or derived from: Mass moves an entity's data whenever its
	// archetype changes, so a pointer that outlived this call would address a different entity's memory.
	const FStructView Fragment = EntityManager.GetFragmentDataStruct(Target.Entity, Binding->FragmentType);
	if (!Fragment.IsValid())
	{
		// Ordinary and not worth a line: an entity built from a different config legitimately carries none.
		return EAccessResult::NoFragment;
	}

	void* ValuePtr = Binding->Property->ContainerPtrToValuePtr<void>(Fragment.GetMemory());
	if (!Visitor(*Binding->Property, ValuePtr))
	{
		if (ShouldLogRefusal(FieldName))
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassFields] '%s' is registered on '%s::%s', which is a '%s', and cannot be %s as the type asked for here."),
				*FieldName.ToString(), *GetNameSafe(Binding->FragmentType), *Binding->Property->GetName(),
				*Binding->Property->GetCPPType(), bWrite ? TEXT("written") : TEXT("read"));
		}
		return EAccessResult::TypeMismatch;
	}

	return EAccessResult::Ok;
}

EAccessResult AccessField(const FCrowdyMassEntityTarget& Target, const FName FieldName, const bool bWrite,
	FFieldVisitor Visitor)
{
	FMassEntityManager* EntityManager = ResolveEntityManager(Target);
	if (!EntityManager)
	{
		return EAccessResult::NoWorld;
	}

	return AccessFieldIn(*EntityManager, Target, FieldName, bWrite, Visitor);
}

FMassEntityManager* ResolveEntityManager(const FCrowdyMassEntityTarget& Target)
{
#if WITH_DEV_AUTOMATION_TESTS
	if (GEntityManagerForTest)
	{
		return GEntityManagerForTest;
	}
#endif

	UWorld* World = Target.World.Get();
	if (!World)
	{
		return nullptr;
	}

	const UCrowdyMassEntitySubsystem* MassSubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	return MassSubsystem ? MassSubsystem->GetMassEntityManager() : nullptr;
}

EAccessResult SetPoseIdIn(FMassEntityManager& EntityManager, const FCrowdyMassEntityTarget& Target, const uint8 PoseId)
{
	// Named so a refusal reads the same way a registered field's refusal does, and so the two share one
	// throttle entry rather than the pose escaping it.
	static const FName PoseFieldName(TEXT("PoseId"));

	const EAccessResult Accessible = CheckAccessible(EntityManager, Target, /*bWrite=*/true, PoseFieldName);
	if (Accessible != EAccessResult::Ok)
	{
		return Accessible;
	}

	FCrowdyMassViewPoseFragment* PoseFragment = EntityManager.GetFragmentDataPtr<FCrowdyMassViewPoseFragment>(Target.Entity);
	if (!PoseFragment)
	{
		// An archetype with no animation trait has nothing to pose. Ordinary, like any other missing fragment.
		return EAccessResult::NoFragment;
	}

	PoseFragment->PoseId = PoseId;
	return EAccessResult::Ok;
}

const TCHAR* DescribeAccessResult(const EAccessResult Result)
{
	switch (Result)
	{
	case EAccessResult::Ok:
		return TEXT("the field was read or written.");
	case EAccessResult::NoWorld:
		return TEXT("the target's world has gone, or this world holds no Mass entity manager.");
	case EAccessResult::InvalidEntity:
		return TEXT("the entity handle does not name a live, built entity right now (despawned, or stale).");
	case EAccessResult::UnknownField:
		return TEXT("no field is registered under that name.");
	case EAccessResult::NoFragment:
		return TEXT("the entity carries no fragment of the registered type.");
	case EAccessResult::TypeMismatch:
		return TEXT("the registered field is not of the type being read or written as.");
	case EAccessResult::DuringMassProcessing:
		return TEXT("Mass processors are running, so the access would have raced them.");
	case EAccessResult::OffGameThread:
		return TEXT("it was called off the game thread.");
	default:
		return TEXT("the access could not be made.");
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void SetEntityManagerForTest(FMassEntityManager* InEntityManager)
{
	GEntityManagerForTest = InEntityManager;
}

void UnregisterFieldForTest(const FName FieldName)
{
	Registry().Remove(FieldName);
}

bool OverwriteFieldBindingForTest(const FName FieldName, const FFieldBinding& Binding)
{
	FFieldBinding* Existing = Registry().Find(FieldName);
	if (!Existing)
	{
		return false;
	}

	*Existing = Binding;
	return true;
}

bool IsReloadRebindInstalledForTest()
{
#if WITH_EDITOR
	// The handle the engine's delegate handed back, not a flag set beside it: it is valid only because the
	// binding was really made.
	return GReloadRebindHandle.IsValid();
#else
	return false;
#endif
}

void ResetRefusalLogThrottleForTest()
{
	RefusalLogTimes().Reset();
}
#endif

}
