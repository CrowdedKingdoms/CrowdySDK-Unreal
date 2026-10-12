#include "Target/CrowdyMassServerValuesLibrary.h"

#include "CrowdyMassLog.h"
#include "CrowdyServerObjectLibrary.h"
#include "Derivation/CrowdyMassServerRecordsDerivation.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Subsystems/CrowdyMassServerRecordsSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/ScriptMacros.h"
#include "UObject/UnrealType.h"

namespace CrowdyMassServerValues
{
	UCrowdyMassServerRecordsSubsystem* FindServerRecords(const UWorld* World)
	{
		return World ? World->GetSubsystem<UCrowdyMassServerRecordsSubsystem>() : nullptr;
	}

	UCrowdyMassServerRecordsSubsystem* FindServerRecordsFor(const UObject* HandlerObject)
	{
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(HandlerObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		if (!World)
		{
			UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassServerRecords] %s is in no game world, so it hears no crowd server values."), *GetNameSafe(HandlerObject));
			return nullptr;
		}

		UCrowdyMassServerRecordsSubsystem* Records = FindServerRecords(World);
		if (Records)
		{
			return Records;
		}

		static TSet<FObjectKey> WorldsToldNoServerRecords;
		bool bAlreadyTold = false;
		WorldsToldNoServerRecords.Add(FObjectKey(World), &bAlreadyTold);
		UE_CLOG(!bAlreadyTold, LogCrowdyMass, Log,
			TEXT("[CrowdyMassServerRecords] World '%s' has no crowd server records, which exist only on maps whose profile selects the Mass backend, so it hears no crowd server values."),
			*World->GetName());

		return nullptr;
	}
}

bool UCrowdyMassServerValuesLibrary::GetServerValueInto(const FCrowdyMassEntityTarget& Target, FName Name, const FProperty* DestProperty, void* Dest)
{
	const UCrowdyMassServerRecordsSubsystem* Records = CrowdyMassServerValues::FindServerRecords(Target.World.Get());
	if (Records)
	{
		return Records->ReadServerValueInto(Target.Entity, Name, DestProperty, Dest);
	}
	if (DestProperty && Dest)
	{
		DestProperty->ClearValue(Dest);
	}
	return false;
}

bool UCrowdyMassServerValuesLibrary::GetServerBoolValue(const FCrowdyMassEntityTarget& Target, FName Name, const FBoolProperty& Source, bool& Out)
{
	// Read into storage as wide as the property's own field, so its byte offset and mask land inside it.
	alignas(8) uint8 Storage[8] = {};
	check(Source.GetElementSize() <= static_cast<int32>(sizeof(Storage)));
	const bool bRead = GetServerValueInto(Target, Name, &Source, Storage);
	Out = Source.GetPropertyValue(Storage);
	return bRead;
}

const FProperty* UCrowdyMassServerValuesLibrary::FindServerValueProperty(const FCrowdyMassEntityTarget& Target, FName Name)
{
	const FInstancedStruct* Values = FindServerValues(Target);
	return Values ? UCrowdyServerObjectLibrary::FindServerValue(*Values, Name) : nullptr;
}

const FInstancedStruct* UCrowdyMassServerValuesLibrary::FindServerValues(const FCrowdyMassEntityTarget& Target)
{
	const UCrowdyMassServerRecordsSubsystem* Records = CrowdyMassServerValues::FindServerRecords(Target.World.Get());
	return Records ? Records->FindServerValues(Target.Entity) : nullptr;
}

bool UCrowdyMassServerValuesLibrary::ClassGetsServerValues(const UClass* ActorClass)
{
	return FCrowdyMassServerRecordsDerivation::FromClass(ActorClass).bSupported;
}

bool UCrowdyMassServerValuesLibrary::CrowdyMassBindServerValuesChangedOnEntity(UObject* HandlerObject, FName HandlerFunctionName, UClass* EntityClass)
{
	UCrowdyMassServerRecordsSubsystem* Records = CrowdyMassServerValues::FindServerRecordsFor(HandlerObject);
	return Records && Records->BindObjectHandler(EntityClass, HandlerObject, HandlerFunctionName);
}

bool UCrowdyMassServerValuesLibrary::CrowdyMassUnbindServerValuesChangedOnEntity(UObject* HandlerObject, FName HandlerFunctionName, UClass* EntityClass)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(HandlerObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	UCrowdyMassServerRecordsSubsystem* Records = CrowdyMassServerValues::FindServerRecords(World);
	return Records && Records->UnbindObjectHandler(EntityClass, HandlerObject, HandlerFunctionName);
}

DEFINE_FUNCTION(UCrowdyMassServerValuesLibrary::execGetServerValue)
{
	P_GET_STRUCT_REF(FCrowdyMassEntityTarget, Target);
	P_GET_PROPERTY(FNameProperty, Name);

	// Value is a wildcard: step over it by hand to learn its type and address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	void* Value = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bRead = false;
	P_NATIVE_BEGIN;
	bRead = GetServerValueInto(Target, Name, ValueProperty, Value);
	P_NATIVE_END;
	*(bool*)RESULT_PARAM = bRead;
}
