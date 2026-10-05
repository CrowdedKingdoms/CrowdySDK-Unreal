#include "CrowdyServerObjectPins.h"

#include "CrowdyExecInternal.h"
#include "CrowdyExecNodesSpawn.h"
#include "CrowdyServerObjectDefinition.h"
#include "EdGraphSchema_K2.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "CrowdyServerObjectPins"

namespace CrowdyServerObjectPinsPrivate
{
	bool IsNested(const FProperty* Property)
	{
		return Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>() || Property->IsA<FOptionalProperty>();
	}

	// The integer kinds Blueprints have no pin for, carried on the nearest pin that holds every value.
	FName WidenedCategory(const FProperty* Property)
	{
		if (Property->IsA<FInt8Property>() || Property->IsA<FInt16Property>() || Property->IsA<FUInt16Property>())
		{
			return UEdGraphSchema_K2::PC_Int;
		}
		if (Property->IsA<FUInt32Property>() || Property->IsA<FUInt64Property>())
		{
			return UEdGraphSchema_K2::PC_Int64;
		}
		return NAME_None;
	}

	bool LeafCategory(const FProperty* Leaf, FName& OutCategory, FName& OutSubCategory, TWeakObjectPtr<UObject>& OutObject)
	{
		OutCategory = WidenedCategory(Leaf);
		if (!OutCategory.IsNone())
		{
			return true;
		}
		UObject* Object = nullptr;
		bool bWeak = false;
		const bool bKnown = UEdGraphSchema_K2::GetPropertyCategoryInfo(Leaf, OutCategory, OutSubCategory, Object, bWeak);
		OutObject = Object;
		return bKnown && !bWeak;
	}

	// The schema has no pin for a container of a widened integer, so the container is rebuilt around widened leaves.
	bool MakeWidenedType(const FProperty* Property, FEdGraphPinType& Out)
	{
		const FArrayProperty* Array = CastField<FArrayProperty>(Property);
		const FSetProperty* Set = CastField<FSetProperty>(Property);
		const FMapProperty* Map = CastField<FMapProperty>(Property);
		const FProperty* Leaf = Array ? Array->Inner : Set ? Set->ElementProp : Map ? Map->KeyProp : Property;

		Out = FEdGraphPinType();
		Out.ContainerType = FEdGraphPinType::ToPinContainerType(Array != nullptr, Set != nullptr, Map != nullptr);
		TWeakObjectPtr<UObject> Object;
		if (!LeafCategory(Leaf, Out.PinCategory, Out.PinSubCategory, Object))
		{
			return false;
		}
		Out.PinSubCategoryObject = Object;
		if (!Map)
		{
			return true;
		}
		const bool bValue = LeafCategory(Map->ValueProp, Out.PinValueType.TerminalCategory, Out.PinValueType.TerminalSubCategory, Object);
		Out.PinValueType.TerminalSubCategoryObject = Object;
		return bValue;
	}

	FText CheckBlueprintType(const UObject* Object)
	{
		const UScriptStruct* Struct = Cast<UScriptStruct>(Object);
		if (Struct && !UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Struct, true))
		{
			return FText::Format(LOCTEXT("StructNotBlueprintType", "the struct {0} is not BlueprintType; mark it BlueprintType"), FText::FromString(Struct->GetName()));
		}
		const UEnum* Enum = Cast<UEnum>(Object);
		if (Enum && !UEdGraphSchema_K2::IsAllowableBlueprintVariableType(Enum))
		{
			return FText::Format(LOCTEXT("EnumNotBlueprintType", "the enum {0} is not BlueprintType; mark it BlueprintType"), FText::FromString(Enum->GetName()));
		}
		return FText::GetEmpty();
	}

	// Values that travel, as the runtime decides it: Accept answers for each property of Struct.
	void Gather(const UScriptStruct* Struct, TFunctionRef<bool(const FProperty*, FName)> Accept, TArray<FCrowdyServerValuePin>& Out)
	{
		Out.Reset();
		if (!Struct)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FName Name(*It->GetAuthoredName());
			if (!Accept(*It, Name))
			{
				continue;
			}
			FCrowdyServerValuePin& Value = Out.AddDefaulted_GetRef();
			Value.Property = *It;
			Value.Name = Name;
			Value.Id = CrowdyServerObjectPins::GetValueId(Struct, *It);
			CrowdyServerObjectPins::MakePinType(*It, Value.Type, Value.bOptional, Value.Refusal);
		}
		CrowdyServerObjectPins::RefuseClashes(Out);
	}

	void Preload(UObject* Object)
	{
		if (!Object || !Object->HasAnyFlags(RF_NeedLoad))
		{
			return;
		}
		if (FLinkerLoad* Linker = Object->GetLinker())
		{
			Linker->Preload(Object);
		}
	}
}

bool CrowdyServerObjectPins::MakePinType(const FProperty* Property, FEdGraphPinType& OutType, bool& bOutOptional, FText& OutRefusal)
{
	using namespace CrowdyServerObjectPinsPrivate;

	OutType = FEdGraphPinType();
	OutRefusal = FText::GetEmpty();
	const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property);
	bOutOptional = Optional != nullptr;
	const FProperty* Value = Optional ? Optional->GetValueProperty() : Property;

	const FArrayProperty* Array = CastField<FArrayProperty>(Value);
	const FSetProperty* Set = CastField<FSetProperty>(Value);
	const FMapProperty* Map = CastField<FMapProperty>(Value);
	const bool bNested = (Optional && Value->IsA<FOptionalProperty>()) || (Array && IsNested(Array->Inner)) || (Set && IsNested(Set->ElementProp))
		|| (Map && (IsNested(Map->KeyProp) || IsNested(Map->ValueProp)));
	if (bNested)
	{
		OutRefusal = LOCTEXT("Nested", "it is an Array, Set, Map or optional inside another, which Blueprints cannot hold");
		return false;
	}

	if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Value, OutType) && !MakeWidenedType(Value, OutType))
	{
		OutRefusal = LOCTEXT("NoPin", "its type has no Blueprint pin");
		return false;
	}
	OutType.bIsReference = false;
	OutType.bIsConst = false;

	OutRefusal = CheckBlueprintType(OutType.PinSubCategoryObject.Get());
	if (OutRefusal.IsEmpty() && OutType.IsMap())
	{
		OutRefusal = CheckBlueprintType(OutType.PinValueType.TerminalSubCategoryObject.Get());
	}
	return OutRefusal.IsEmpty();
}

void CrowdyServerObjectPins::GatherValues(const UScriptStruct* Struct, TArray<FCrowdyServerValuePin>& Out)
{
	CrowdyServerObjectPinsPrivate::Gather(Struct, [Struct](const FProperty* Property, FName Name)
	{
		return CrowdyExec::FindTravellingField(Struct, Name) == Property;
	}, Out);
}

void CrowdyServerObjectPins::GatherVariables(const UCrowdyServerObjectDefinition* Definition, TArray<FCrowdyServerValuePin>& Out)
{
	const UScriptStruct* State = Definition ? Definition->GetStateStruct() : nullptr;
	CrowdyServerObjectPinsPrivate::Gather(State, [Definition](const FProperty* Property, FName Name)
	{
		return Definition->FindVariable(Name) == Property;
	}, Out);
	for (FCrowdyServerValuePin& Variable : Out)
	{
		Variable.bVisible = Definition->WatchedFields.Contains(Variable.Name);
	}
}

const FCrowdyServerValuePin* CrowdyServerObjectPins::FindValue(TConstArrayView<FCrowdyServerValuePin> Values, const FGuid& Id, FName Name)
{
	const FCrowdyServerValuePin* ById = Id.IsValid() ? Values.FindByPredicate([&Id](const FCrowdyServerValuePin& Value) { return Value.Id == Id; }) : nullptr;
	return ById ? ById : Values.FindByPredicate([Name](const FCrowdyServerValuePin& Value) { return Value.Name == Name; });
}

FName CrowdyServerObjectPins::HasValuePinName(FName Name)
{
	return FName(*FString::Printf(TEXT("%s Has Value"), *Name.ToString()));
}

FGuid CrowdyServerObjectPins::HasValuePinId(const FGuid& Id)
{
	return Id.IsValid() ? FGuid(Id.A, Id.B, Id.C, Id.D ^ 1u) : FGuid();
}

FGuid CrowdyServerObjectPins::GetValueId(const UScriptStruct* Struct, const FProperty* Property)
{
	if (const UPropertyBag* Bag = Cast<UPropertyBag>(Struct))
	{
		const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByProperty(Property);
		return Desc ? Desc->ID : FGuid();
	}
	return Struct && Struct->ArePropertyGuidsAvailable() ? Struct->FindPropertyGuidFromName(Property->GetFName()) : FGuid();
}

void CrowdyServerObjectPins::RefuseClashes(TArray<FCrowdyServerValuePin>& Values)
{
	// Pins are found by name, ignoring case, so a value named like one of the node's own pins would be mistaken for it.
	TArray<FName> Taken = {UEdGraphSchema_K2::PN_Execute, UEdGraphSchema_K2::PN_Then, UEdGraphSchema_K2::PN_Self, CrowdyServerObjectNodePins::InstanceId,
		CrowdyServerObjectNodePins::TeamId, CrowdyServerObjectNodePins::Changed, CrowdyServerObjectNodePins::HasValues, CrowdyServerCallPins::FunctionPicker, CrowdyServerCallPins::OnSuccess,
		CrowdyServerCallPins::OnFailed, CrowdyServerCallPins::Outcome, CrowdyServerCallPins::Reason, CrowdyServerCallPins::Retryable};
	for (const FCrowdyServerValuePin& Value : Values)
	{
		if (Value.bOptional)
		{
			Taken.Add(HasValuePinName(Value.Name));
		}
	}
	for (FCrowdyServerValuePin& Value : Values)
	{
		const FName* Clash = Taken.FindByKey(Value.Name);
		if (Clash && Value.HasPin())
		{
			Value.Refusal = FText::Format(LOCTEXT("Clash", "{0} clashes with the node's {1} pin; rename it"), FText::FromName(Value.Name), FText::FromName(*Clash));
		}
	}
}

void CrowdyServerObjectPins::PreloadDefinition(UCrowdyServerObjectDefinition* Definition)
{
	if (!Definition)
	{
		return;
	}
	CrowdyServerObjectPinsPrivate::Preload(Definition);
	CrowdyServerObjectPinsPrivate::Preload(Definition->State);
	for (const FCrowdyServerFunction& Function : Definition->Functions)
	{
		CrowdyServerObjectPinsPrivate::Preload(Function.Params);
		CrowdyServerObjectPinsPrivate::Preload(Function.Reply);
	}
}

#undef LOCTEXT_NAMESPACE
