#include "Target/CrowdyMassTargetLibrary.h"

#include "Assets/CrowdyMassAnimSet.h"
#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "Events/CrowdyMassEventLibrary.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "MassEntityManager.h"
#include "Reflection/CrowdyMassByteProperty.h"
#include "Replication/RPC/FCrowdyRpcTarget.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "Target/CrowdyMassFieldAccess.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
	// A vector field is a struct property whose struct is FVector itself, never anything that merely
	// contains one: reading three doubles out of an FTransform's memory would be reading its rotation.
	const FStructProperty* AsVectorProperty(const FProperty& Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		return (StructProperty && StructProperty->Struct == TBaseStructure<FVector>::Get()) ? StructProperty : nullptr;
	}

	// The Blueprint real type is double, and a fragment may declare either width, so both are accepted and
	// the narrow one widens. A game that stores a float and reads it back here gets the float's value, not
	// a different number.
	bool ReadReal(const FProperty& Property, const void* ValuePtr, double& OutValue)
	{
		if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(&Property))
		{
			OutValue = DoubleProperty->GetPropertyValue(ValuePtr);
			return true;
		}

		const FFloatProperty* FloatProperty = CastField<FFloatProperty>(&Property);
		if (!FloatProperty)
		{
			return false;
		}

		OutValue = FloatProperty->GetPropertyValue(ValuePtr);
		return true;
	}

	bool WriteReal(const FProperty& Property, void* ValuePtr, const double Value)
	{
		if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(&Property))
		{
			DoubleProperty->SetPropertyValue(ValuePtr, Value);
			return true;
		}

		const FFloatProperty* FloatProperty = CastField<FFloatProperty>(&Property);
		if (!FloatProperty)
		{
			return false;
		}

		FloatProperty->SetPropertyValue(ValuePtr, static_cast<float>(Value));
		return true;
	}

	bool RunAccess(const FCrowdyMassEntityTarget& Target, const FName FieldName, const bool bWrite,
		CrowdyMassFields::FFieldVisitor Visitor)
	{
		return CrowdyMassFields::AccessField(Target, FieldName, bWrite, Visitor) == CrowdyMassFields::EAccessResult::Ok;
	}

	// The subsystem for the world the target itself names, which is what makes every entry point here take
	// the target and nothing else. Null once that world has gone.
	//
	// Every caller here only reads, and each binds the result to a const pointer at its own call site,
	// which is where that intent belongs.
	UCrowdyMassEntitySubsystem* ResolveSubsystem(const FCrowdyMassEntityTarget& Target)
	{
		UWorld* World = Target.World.Get();
		return World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	}

	// A send refused because Mass is running is a situation rather than an authoring fact: the same graph
	// works from a phase-end delegate, and the caller that hits it hits it every frame it fires. So this
	// one is spaced out rather than said once, matching what the field writes on this library already do
	// for the identical refusal. The message names its own throttle so a reader is not left counting.
	constexpr double ActionSendDuringProcessingWarnIntervalSeconds = 5.0;

	double& LastActionSendDuringProcessingReport()
	{
		static double LastReportedSeconds = -ActionSendDuringProcessingWarnIntervalSeconds;
		return LastReportedSeconds;
	}

	bool ShouldReportActionSendDuringProcessing()
	{
		double& LastReportedSeconds = LastActionSendDuringProcessingReport();

		const double Now = FPlatformTime::Seconds();
		if (Now - LastReportedSeconds < ActionSendDuringProcessingWarnIntervalSeconds)
		{
			return false;
		}

		LastReportedSeconds = Now;
		return true;
	}

	// Writes one value into a parameter frame through the function's own reflected property, so the frame
	// cannot silently disagree with the signature the transport serializes from. A parameter that is not
	// there, or is not the expected type, writes nothing and reads back as its zeroed default, which is
	// what an event declaring no variant or no rate already means.
	void WriteIntParam(const UFunction* Function, void* Frame, const FName ParamName, const int32 Value)
	{
		if (const FIntProperty* Property = CastField<FIntProperty>(Function->FindPropertyByName(ParamName)))
		{
			Property->SetPropertyValue_InContainer(Frame, Value);
		}
	}

	void WriteFloatParam(const UFunction* Function, void* Frame, const FName ParamName, const float Value)
	{
		if (const FFloatProperty* Property = CastField<FFloatProperty>(Function->FindPropertyByName(ParamName)))
		{
			Property->SetPropertyValue_InContainer(Frame, Value);
		}
	}

	// Names which of the three refusals it was, and what to do about that one. They are separated because
	// only one of them is anything an author can act on immediately and each is fixed somewhere else; one
	// shared sentence would be unfollowable from two of the three states that reach it.
	void ReportActionSendRefusal(const ECrowdyMassActionSendVerdict Verdict, const UClass* EntityClass,
		const FName ActionName)
	{
		// The verdict is part of what "already said this" means, not only the class and the name: two of
		// the three refusals name a fix that turns the send into the OTHER refusal, and a record that
		// could not tell them apart would swallow the sentence naming what is still left to do.
		if (!CrowdyMassShouldReportActionSendRefusal(EntityClass, ActionName, Verdict))
		{
			return;
		}

		if (Verdict == ECrowdyMassActionSendVerdict::ClassUnknown)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassTargetLibrary] '%s' was not sent: this client has not learned the entity's class yet, so there is no animation set to resolve the name against. Ordinary and temporary; it resolves once the entity's own client announces what it is. Said once for this name and reason."),
				*ActionName.ToString());
			return;
		}

		if (Verdict == ECrowdyMassActionSendVerdict::NoAnimSet)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassTargetLibrary] '%s' was not sent: '%s' names no animation set, so it authors no actions at all. Give the class a Crowdy Mass Anim Set component, or bind it to a Mass entity config carrying an animation trait. Said once per name and reason for this class."),
				*ActionName.ToString(), *GetNameSafe(EntityClass));
			return;
		}

		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassTargetLibrary] '%s' was not sent: '%s' renders through an animation set that authors no action row of that name. Add a row named '%s' to that set's Actions array. Action ids start at 1 and follow row order, and 0 means no action. Said once per name and reason for this class."),
			*ActionName.ToString(), *GetNameSafe(EntityClass), *ActionName.ToString());
	}
}

bool UCrowdyMassTargetLibrary::IsTargetLive(const FCrowdyMassEntityTarget& Target)
{
	if (!IsInGameThread())
	{
		return false;
	}

	const FMassEntityManager* EntityManager = CrowdyMassFields::ResolveEntityManager(Target);
	if (!EntityManager)
	{
		return false;
	}

	// Built as well as valid: a reserved entity that has not been built yet has no archetype, so nothing
	// can be read off it even though its slot and serial check out.
	return EntityManager->IsEntityValid(Target.Entity) && EntityManager->IsEntityBuilt(Target.Entity);
}

bool UCrowdyMassTargetLibrary::IsFieldRegistered(const FName FieldName)
{
	return CrowdyMassFields::IsFieldRegistered(FieldName);
}

bool UCrowdyMassTargetLibrary::GetFieldByte(const FCrowdyMassEntityTarget& Target, const FName FieldName, uint8& OutValue)
{
	OutValue = 0;

	return RunAccess(Target, FieldName, /*bWrite=*/false, [&OutValue](const FProperty& Property, void* ValuePtr)
	{
		const FByteProperty* ByteProperty = CrowdyMassAsByteProperty(Property);
		if (!ByteProperty)
		{
			return false;
		}

		OutValue = ByteProperty->GetPropertyValue(ValuePtr);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::GetFieldInt(const FCrowdyMassEntityTarget& Target, const FName FieldName, int32& OutValue)
{
	OutValue = 0;

	return RunAccess(Target, FieldName, /*bWrite=*/false, [&OutValue](const FProperty& Property, void* ValuePtr)
	{
		const FIntProperty* IntProperty = CastField<FIntProperty>(&Property);
		if (!IntProperty)
		{
			return false;
		}

		OutValue = IntProperty->GetPropertyValue(ValuePtr);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::GetFieldFloat(const FCrowdyMassEntityTarget& Target, const FName FieldName, double& OutValue)
{
	OutValue = 0.0;

	return RunAccess(Target, FieldName, /*bWrite=*/false, [&OutValue](const FProperty& Property, void* ValuePtr)
	{
		return ReadReal(Property, ValuePtr, OutValue);
	});
}

bool UCrowdyMassTargetLibrary::GetFieldBool(const FCrowdyMassEntityTarget& Target, const FName FieldName, bool& bOutValue)
{
	bOutValue = false;

	return RunAccess(Target, FieldName, /*bWrite=*/false, [&bOutValue](const FProperty& Property, void* ValuePtr)
	{
		// FBoolProperty, not a byte read: a bool declared as a bitfield lives in a shared byte, and only
		// the property knows which bit of it is this field.
		const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property);
		if (!BoolProperty)
		{
			return false;
		}

		bOutValue = BoolProperty->GetPropertyValue(ValuePtr);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::GetFieldVector(const FCrowdyMassEntityTarget& Target, const FName FieldName, FVector& OutValue)
{
	OutValue = FVector::ZeroVector;

	return RunAccess(Target, FieldName, /*bWrite=*/false, [&OutValue](const FProperty& Property, void* ValuePtr)
	{
		if (!AsVectorProperty(Property))
		{
			return false;
		}

		OutValue = *static_cast<const FVector*>(ValuePtr);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::SetFieldByte(const FCrowdyMassEntityTarget& Target, const FName FieldName, const uint8 Value)
{
	return RunAccess(Target, FieldName, /*bWrite=*/true, [Value](const FProperty& Property, void* ValuePtr)
	{
		const FByteProperty* ByteProperty = CrowdyMassAsByteProperty(Property);
		if (!ByteProperty)
		{
			return false;
		}

		ByteProperty->SetPropertyValue(ValuePtr, Value);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::SetFieldInt(const FCrowdyMassEntityTarget& Target, const FName FieldName, const int32 Value)
{
	return RunAccess(Target, FieldName, /*bWrite=*/true, [Value](const FProperty& Property, void* ValuePtr)
	{
		const FIntProperty* IntProperty = CastField<FIntProperty>(&Property);
		if (!IntProperty)
		{
			return false;
		}

		IntProperty->SetPropertyValue(ValuePtr, Value);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::SetFieldFloat(const FCrowdyMassEntityTarget& Target, const FName FieldName, const double Value)
{
	return RunAccess(Target, FieldName, /*bWrite=*/true, [Value](const FProperty& Property, void* ValuePtr)
	{
		return WriteReal(Property, ValuePtr, Value);
	});
}

bool UCrowdyMassTargetLibrary::SetFieldBool(const FCrowdyMassEntityTarget& Target, const FName FieldName, const bool bValue)
{
	return RunAccess(Target, FieldName, /*bWrite=*/true, [bValue](const FProperty& Property, void* ValuePtr)
	{
		const FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property);
		if (!BoolProperty)
		{
			return false;
		}

		BoolProperty->SetPropertyValue(ValuePtr, bValue);
		return true;
	});
}

bool UCrowdyMassTargetLibrary::SetFieldVector(const FCrowdyMassEntityTarget& Target, const FName FieldName, const FVector Value)
{
	return RunAccess(Target, FieldName, /*bWrite=*/true, [&Value](const FProperty& Property, void* ValuePtr)
	{
		if (!AsVectorProperty(Property))
		{
			return false;
		}

		*static_cast<FVector*>(ValuePtr) = Value;
		return true;
	});
}

bool UCrowdyMassTargetLibrary::SetEntityPose(const FCrowdyMassEntityTarget& Target, const FName PoseName)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassTargetLibrary] Set Entity Pose was called off the game thread and has been refused."));
		return false;
	}

	const UCrowdyMassEntitySubsystem* MassSubsystem = ResolveSubsystem(Target);
	FMassEntityManager* EntityManager = CrowdyMassFields::ResolveEntityManager(Target);
	if (!MassSubsystem || !EntityManager)
	{
		return false;
	}

	// The class comes from the target, not from the caller, which is what keeps this node's pins the same
	// now that a pose name is a row in one class's own animation set. Resolving the same name against
	// another class's set would put the entity into whatever that class authored under it, which is a
	// wrong pose rather than no pose and nothing would report it.
	FCrowdyRpcTarget Resolved;
	const UClass* EntityClass = CrowdyMassTarget::ResolveRpcTarget(Target, Resolved) == CrowdyMassTarget::EResolveResult::Resolved
		? MassSubsystem->GetEntityClass(Resolved.NetID)
		: nullptr;

	// Resolved before the write rather than inside it. Turning a name into an id reads that class's
	// animation set, which has nothing to do with this entity's fragments, and doing it with a fragment
	// pointer in hand would be holding one across work that does not need it.
	const int32 PoseId = MassSubsystem->FindPoseId(EntityClass, PoseName);

	// With no class there is no animation set to look the name up in, so the lookup answered 0, and 0 is
	// the value that CLEARS the pose. Writing it would do the opposite of what was asked and report
	// success; clearing still works, because clearing needs no set.
	if (!EntityClass && !PoseName.IsNone())
	{
		return false;
	}

	return CrowdyMassFields::SetPoseIdIn(*EntityManager, Target, static_cast<uint8>(PoseId))
		== CrowdyMassFields::EAccessResult::Ok;
}

void UCrowdyMassTargetLibrary::ResetSendThrottleForTest()
{
	LastActionSendDuringProcessingReport() = -ActionSendDuringProcessingWarnIntervalSeconds;
}

bool UCrowdyMassTargetLibrary::SendEntityAction(const FCrowdyMassEntityTarget& Target, const FName ActionName,
	const int32 Variant, const float PlayRate)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassTargetLibrary] Send Crowdy Action was called off the game thread and has been refused."));
		return false;
	}

	// The same refusal every other write on this library makes, and it belongs here for the same reason:
	// sending an action runs the event's own body on this machine as part of the send, and that body
	// writes the entity's action fragment straight into chunk storage, racing every processor declared to
	// read it. Two things put a caller here, a graph run from a delegate broadcast inside a chunk loop and
	// an ordinary tick that overlaps a Mass phase. The WHOLE send is dropped rather than only its local
	// half, so this machine and every other one keep showing the same thing.
	const FMassEntityManager* EntityManager = CrowdyMassFields::ResolveEntityManager(Target);
	if (EntityManager && EntityManager->IsProcessing())
	{
		UE_CLOG(ShouldReportActionSendDuringProcessing(), LogCrowdyMass, Error,
			TEXT("[CrowdyMassTargetLibrary] '%s' was sent while Mass processors are running, and the send has been refused. It would have written the entity's action straight into chunk storage alongside the processors reading it. Move the send to a phase-end delegate, or defer the broadcast that reached it to the end of the frame, and the same graph works. Repeats are reported at most once every few seconds."),
			*ActionName.ToString());
		return false;
	}

	const UCrowdyMassEntitySubsystem* MassSubsystem = ResolveSubsystem(Target);
	if (!MassSubsystem)
	{
		return false;
	}

	FCrowdyRpcTarget Resolved;
	if (CrowdyMassTarget::ResolveRpcTarget(Target, Resolved) != CrowdyMassTarget::EResolveResult::Resolved)
	{
		// The target names no live, addressable entity right now. Left to the resolve step to describe
		// rather than restated here, and not reported per call: a target held across a despawn hits this
		// every frame something keeps firing at it.
		return false;
	}

	// The class comes from the TARGET, never from the caller. An action id is a row position in one
	// class's set, so resolving the name against another class's set would play whatever that class
	// authored under it, which is a wrong action rather than no action and nothing would report it.
	const UClass* EntityClass = MassSubsystem->GetEntityClass(Resolved.NetID);
	const UCrowdyMassAnimSet* AnimSet = MassSubsystem->GetRemoteEntityAnimSet(EntityClass);

	int32 ActionId = 0;
	const ECrowdyMassActionSendVerdict Verdict =
		CrowdyMassDecideActionSend(EntityClass, AnimSet, ActionName, ActionId);
	if (Verdict != ECrowdyMassActionSendVerdict::Send)
	{
		ReportActionSendRefusal(Verdict, EntityClass, ActionName);
		return false;
	}

	UFunction* Function = CrowdyMassFindPlayActionEvent();
	if (!Function)
	{
		UE_LOG(LogCrowdyMass, Error,
			TEXT("[CrowdyMassTargetLibrary] The built-in crowd action event could not be resolved, so no action can be sent through it."));
		return false;
	}

	const int32 FrameSize = FMath::Max<int32>(Function->ParmsSize, 1);
	void* Frame = FMemory_Alloca(FrameSize);
	FMemory::Memzero(Frame, FrameSize);

	// Every parameter of this event is a plain integer or float, so zeroing is the whole of constructing
	// the frame and there is nothing in it to destroy afterwards.
	WriteIntParam(Function, Frame, CrowdyMassActionParamNames::ActionId, ActionId);
	WriteIntParam(Function, Frame, CrowdyMassActionParamNames::Variant, Variant);
	WriteFloatParam(Function, Frame, CrowdyMassActionParamNames::PlayRate, PlayRate);

	// The send path's own false means only that there was nothing to route through at all, which is
	// already covered by the resolve above; every refusal it makes logs its own reason.
	UCrowdyMassEventLibrary::SendEventToMassTarget(Target, Function, Frame);
	return true;
}
