// Fill out your copyright notice in the Description page of Project Settings.

#include "Nodes/CrowdyApplyEffectNodeShared.h"

#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UnrealType.h"

namespace CrowdyApplyEffectNodeShared
{
	const FName PN_Effect(TEXT("Effect"));
	const FName PN_Target(TEXT("Target"));
	const FName PN_Source(TEXT("Source"));
	const FName PN_Overrides(TEXT("Overrides"));
	const FName PN_Level(TEXT("Level"));
	const FName PN_ReturnValueJson(TEXT("ReturnValueJson"));
	const FName PN_ReturnValue(TEXT("CrowdyReturnValue"));

	bool TargetResolvesToSelf(bool bHasLinks, bool bHasDefaultObject)
	{
		return !bHasLinks && !bHasDefaultObject;
	}

	bool TargetResolvesToSelf(const UEdGraphPin* TargetPin)
	{
		// A node with no Target pin at all (the by-id node) has nothing to judge, which is not the same as a pin
		// that is present and empty.
		if (!TargetPin)
		{
			return false;
		}
		return TargetResolvesToSelf(TargetPin->LinkedTo.Num() > 0, TargetPin->DefaultObject != nullptr);
	}

	bool IsGameModelContainerClass(const UClass* Class)
	{
		if (!Class)
		{
			return false;
		}
		FString TypeName;
		return FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName) && !TypeName.IsEmpty();
	}

	namespace
	{
		TFunction<bool(const UBlueprint*)> GContainerBlueprintResolver;
	}

	void SetContainerBlueprintResolver(TFunction<bool(const UBlueprint*)> Resolver)
	{
		GContainerBlueprintResolver = MoveTemp(Resolver);
	}

	TFunction<bool(const UBlueprint*)> GetContainerBlueprintResolver()
	{
		return GContainerBlueprintResolver;
	}

	bool IsGameModelContainerBlueprint(const UBlueprint* Blueprint)
	{
		return Blueprint && GContainerBlueprintResolver && GContainerBlueprintResolver(Blueprint);
	}

	UClass* ResolveSelfClassForNode(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return nullptr;
		}
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		if (!Blueprint)
		{
			return nullptr;
		}
		// The container tag is stamped onto the generated class, so that is the representation to ask. The skeleton
		// class is rebuilt from the graph on every compile and never carries the tag, so preferring it reported a
		// marked container as not one. Fall back through it and the parent only so a class is still named.
		if (Blueprint->GeneratedClass)
		{
			return Blueprint->GeneratedClass;
		}
		if (Blueprint->SkeletonGeneratedClass)
		{
			return Blueprint->SkeletonGeneratedClass;
		}
		return Blueprint->ParentClass;
	}

	void ValidateSelfTargetIsContainer(const UEdGraphNode* Node, const UEdGraphPin* TargetPin,
		FCompilerResultsLog& MessageLog)
	{
		if (!TargetResolvesToSelf(TargetPin))
		{
			return;
		}
		// The persisted marker is the authority and is unaffected by compile state, so ask it before looking at any
		// class. A Blueprint marked as a container answers true on the very first compile after marking, before the
		// tag has been stamped anywhere.
		if (const UBlueprint* Blueprint = Node ? FBlueprintEditorUtils::FindBlueprintForNode(Node) : nullptr)
		{
			if (IsGameModelContainerBlueprint(Blueprint))
			{
				return;
			}
		}
		UClass* SelfClass = ResolveSelfClassForNode(Node);
		if (!SelfClass)
		{
			return;
		}
		if (IsGameModelContainerClass(SelfClass))
		{
			return;
		}
		MessageLog.Error(*FText::Format(
			NSLOCTEXT("CrowdyApplyEffectNodeShared", "SelfTargetNotAContainer",
				"@@ leaves Target unwired, so it applies to self, but '{0}' is not a Game Model container and has no "
				"server state to apply to. Mark this Blueprint as a Crowdy container, or wire Target to something "
				"that is one."),
			FText::FromString(SelfClass->GetName())).ToString(), Node);
	}

	EParameterPinAction ActionForParameterPin(FName PinName, const FCrowdyApplyEffectPinPlan& Plan,
		bool bHasLiteralEffect)
	{
		if (!bHasLiteralEffect)
		{
			return EParameterPinAction::Keep;
		}
		if (PinName == PN_Overrides)
		{
			return EParameterPinAction::HideAndReset;
		}
		if (PinName == PN_Level)
		{
			return Plan.bIncludeLevel ? EParameterPinAction::Keep : EParameterPinAction::HideAndReset;
		}
		if (PinName == PN_Source)
		{
			return Plan.bIncludeSource ? EParameterPinAction::Keep : EParameterPinAction::HideAndReset;
		}
		return EParameterPinAction::Keep;
	}

	bool ShouldRemovePinBeforeCall(FName PinName)
	{
		return CrowdyApplyEffectNodePins::IsMagnitudePinName(PinName);
	}

	FName EncoderFunctionName(ECrowdyEffectValueType ValueType, FName& OutInputParam)
	{
		switch (ValueType)
		{
		case ECrowdyEffectValueType::Float:
			OutInputParam = TEXT("Value");
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonFromFloat);
		case ECrowdyEffectValueType::Bool:
			OutInputParam = TEXT("Value");
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonFromBool);
		case ECrowdyEffectValueType::String:
			OutInputParam = TEXT("Value");
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonFromString);
		case ECrowdyEffectValueType::ContainerRef:
			OutInputParam = TEXT("Object");
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, GetContainerIdFor);
		case ECrowdyEffectValueType::Int:
		default:
			OutInputParam = TEXT("Value");
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonFromInt);
		}
	}

	FEdGraphPinType MagnitudePinType(ECrowdyEffectValueType ValueType)
	{
		FEdGraphPinType PinType;
		switch (ValueType)
		{
		case ECrowdyEffectValueType::Float:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			break;
		case ECrowdyEffectValueType::Bool:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			break;
		case ECrowdyEffectValueType::String:
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			break;
		case ECrowdyEffectValueType::ContainerRef:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			PinType.PinSubCategoryObject = UObject::StaticClass();
			break;
		case ECrowdyEffectValueType::Int:
		default:
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			break;
		}
		return PinType;
	}

	bool ReturnValueType(ECrowdyEffectReturnType ReturnType, ECrowdyEffectValueType& OutValueType)
	{
		switch (ReturnType)
		{
		case ECrowdyEffectReturnType::Int:
			OutValueType = ECrowdyEffectValueType::Int;
			return true;
		case ECrowdyEffectReturnType::Float:
			OutValueType = ECrowdyEffectValueType::Float;
			return true;
		case ECrowdyEffectReturnType::Bool:
			OutValueType = ECrowdyEffectValueType::Bool;
			return true;
		case ECrowdyEffectReturnType::String:
			OutValueType = ECrowdyEffectValueType::String;
			return true;
		case ECrowdyEffectReturnType::None:
		default:
			return false;
		}
	}

	FEdGraphPinType ReturnPinType(ECrowdyEffectReturnType ReturnType)
	{
		ECrowdyEffectValueType ValueType = ECrowdyEffectValueType::Int;
		if (!ReturnValueType(ReturnType, ValueType))
		{
			// No declared type means no pin, so there is nothing meaningful to answer. A wildcard says so rather
			// than handing back a plausible-looking int pin a caller might create.
			FEdGraphPinType Wildcard;
			Wildcard.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
			return Wildcard;
		}
		return MagnitudePinType(ValueType);
	}

	FName DecoderFunctionName(ECrowdyEffectReturnType ReturnType, FName& OutInputParam)
	{
		OutInputParam = TEXT("ValueJson");
		switch (ReturnType)
		{
		case ECrowdyEffectReturnType::Int:
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonToInt);
		case ECrowdyEffectReturnType::Float:
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonToFloat);
		case ECrowdyEffectReturnType::Bool:
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonToBool);
		case ECrowdyEffectReturnType::String:
			return GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, JsonToString);
		case ECrowdyEffectReturnType::None:
		default:
			OutInputParam = NAME_None;
			return NAME_None;
		}
	}

	bool ReturnPinLacksBacking(ECrowdyEffectReturnType DeclaredType, const FString& LoweredReturnExpression)
	{
		return DeclaredType != ECrowdyEffectReturnType::None && LoweredReturnExpression.IsEmpty();
	}

	void PreloadEffectAsset(UObject* Effect)
	{
		// RF_NeedLoad is the linker's own "this export has not been serialized yet" flag, so this costs a bit test on
		// every already-loaded asset and does real work only inside the load window.
		if (!Effect || !Effect->HasAnyFlags(RF_NeedLoad))
		{
			return;
		}
		if (FLinkerLoad* Linker = Effect->GetLinker())
		{
			Linker->Preload(Effect);
		}
	}

	FString FirstCompileError(const TArray<FCrowdyEffectDiagnostic>& Diagnostics)
	{
		for (const FCrowdyEffectDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity != ECrowdyEffectSeverity::Error)
			{
				continue;
			}
			return Diagnostic.Line > 0
				? FString::Printf(TEXT("line %d: %s"), Diagnostic.Line, *Diagnostic.Message)
				: Diagnostic.Message;
		}
		return FString();
	}

	bool ChangeAffectsPinPlan(const FPropertyChangedEvent& PropertyChangedEvent)
	{
		static const TSet<FName> RelevantNames = {
			GET_MEMBER_NAME_CHECKED(UCrowdyEffect, Magnitudes),
			GET_MEMBER_NAME_CHECKED(UCrowdyEffect, bRequiresSource),
			GET_MEMBER_NAME_CHECKED(UCrowdyEffect, ReturnType),
			GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, Name),
			GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, ValueTypeEnum),
			GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, ValueType),
			GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, DefaultValueJson),
			GET_MEMBER_NAME_CHECKED(FCrowdyEffectMagnitude, Curve),
		};

		const FName MemberName = PropertyChangedEvent.MemberProperty
			? PropertyChangedEvent.MemberProperty->GetFName()
			: NAME_None;
		return RelevantNames.Contains(MemberName) || RelevantNames.Contains(PropertyChangedEvent.GetPropertyName());
	}
}
