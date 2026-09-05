// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectPickerOptions.h"

#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectPickerOptions"

namespace CrowdyEffectPickerOptions
{
	TArray<ECrowdyEffectAssignmentOp> AllowedAssignmentOps(const FString& ValueType)
	{
		if (ValueType == TEXT("int") || ValueType == TEXT("float"))
		{
			return { ECrowdyEffectAssignmentOp::Set, ECrowdyEffectAssignmentOp::Add,
					 ECrowdyEffectAssignmentOp::Subtract, ECrowdyEffectAssignmentOp::Multiply,
					 ECrowdyEffectAssignmentOp::Divide };
		}

		// bool / string / empty / anything else: only a full overwrite is meaningful, and it is the only
		// operator the lowering accepts for a non-numeric attribute.
		return { ECrowdyEffectAssignmentOp::Set };
	}

	TArray<ECrowdyEffectAssignmentOp> AllowedAssignmentOps(ECrowdyEffectValueType ValueType)
	{
		return AllowedAssignmentOps(UCrowdyEffect::ValueTypeToWireString(ValueType));
	}

	TArray<FCrowdyAttributeDef> AttributesForClass(const UClass* Class)
	{
		if (!Class)
		{
			return {};
		}
		return FCrowdyAttributeRegistry::DiscoverForClass(Class);
	}

	FString ValueTypeForAttribute(const UClass* Class, const FString& AttributeKeyOrName)
	{
		if (!Class || AttributeKeyOrName.IsEmpty())
		{
			return FString();
		}

		const TArray<FCrowdyAttributeDef> Defs = FCrowdyAttributeRegistry::DiscoverForClass(Class);
		const FCrowdyAttributeDef* Found = Defs.FindByPredicate([&AttributeKeyOrName](const FCrowdyAttributeDef& D)
		{
			return D.PropertyName.ToString().Equals(AttributeKeyOrName, ESearchCase::IgnoreCase)
				|| D.Key.Equals(AttributeKeyOrName, ESearchCase::IgnoreCase);
		});
		return Found ? Found->ValueType : FString();
	}

	FText VerbLabel(ECrowdyEffectAssignmentOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectAssignmentOp::Add:      return LOCTEXT("VerbIncrease", "Increase");
		case ECrowdyEffectAssignmentOp::Subtract: return LOCTEXT("VerbDecrease", "Decrease");
		case ECrowdyEffectAssignmentOp::Multiply: return LOCTEXT("VerbMultiply", "Multiply");
		case ECrowdyEffectAssignmentOp::Divide:   return LOCTEXT("VerbDivide", "Divide");
		case ECrowdyEffectAssignmentOp::Set:
		default:                                  return LOCTEXT("VerbSet", "Set");
		}
	}

	bool OpForVerbLabel(const FString& VerbText, ECrowdyEffectAssignmentOp& OutOp)
	{
		for (const ECrowdyEffectAssignmentOp Op : { ECrowdyEffectAssignmentOp::Set, ECrowdyEffectAssignmentOp::Add,
			ECrowdyEffectAssignmentOp::Subtract, ECrowdyEffectAssignmentOp::Multiply, ECrowdyEffectAssignmentOp::Divide })
		{
			if (VerbLabel(Op).ToString().Equals(VerbText, ESearchCase::IgnoreCase))
			{
				OutOp = Op;
				return true;
			}
		}
		return false;
	}

	FString RoleWord(ECrowdyEffectRole Role, const FString& SourceRoleLabel)
	{
		if (Role == ECrowdyEffectRole::Target)
		{
			return TEXT("Target");
		}
		const FString Trimmed = SourceRoleLabel.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? TEXT("Source") : Trimmed;
	}

	FString TargetPhrase(ECrowdyEffectRole Role, const FString& Attribute, const FString& SourceRoleLabel)
	{
		return FString::Printf(TEXT("%s's %s"), *RoleWord(Role, SourceRoleLabel), *Attribute);
	}

	FString PropertyKeyForAutomationTrigger(const FCrowdyAttributeDef& Def)
	{
		return Def.Key;
	}

	FString AttributePickerWrittenValue(const FCrowdyAttributeDef& Def)
	{
		return Def.Key.IsEmpty() ? Def.PropertyName.ToString() : Def.Key;
	}

	FString AttributeNameForScriptCompletion(const FCrowdyAttributeDef& Def)
	{
		return Def.PropertyName.IsNone() ? Def.Key : Def.PropertyName.ToString();
	}

	bool IsTransientReflectionClassName(const FString& ClassName)
	{
		return ClassName.StartsWith(TEXT("SKEL_"))
			|| ClassName.StartsWith(TEXT("REINST_"))
			|| ClassName.StartsWith(TEXT("TRASHCLASS_"))
			|| ClassName.StartsWith(TEXT("PLACEHOLDER-"));
	}

	bool IsOfferableContainerType(const UClass* Class, FString& OutTypeName)
	{
		if (!Class || FCrowdyAttributeRegistry::IsTestContainer(Class))
		{
			return false;
		}
		return FCrowdyAttributeRegistry::GetContainerTypeName(Class, OutTypeName);
	}

	TArray<FString> KnownContainerTypeNames()
	{
		TSet<FString> Unique;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			const UClass* Class = *It;
			if (!IsValid(Class))
			{
				continue;
			}
			if (IsTransientReflectionClassName(Class->GetName()))
			{
				continue;
			}
			FString TypeName;
			if (IsOfferableContainerType(Class, TypeName))
			{
				Unique.Add(TypeName);
			}
		}
		TArray<FString> Out = Unique.Array();
		Out.Sort();
		return Out;
	}
}

#undef LOCTEXT_NAMESPACE
