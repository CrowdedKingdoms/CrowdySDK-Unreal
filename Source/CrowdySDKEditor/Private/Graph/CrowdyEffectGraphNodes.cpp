// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/CrowdyEffectGraphNodes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Graph/CrowdyEffectGraphNodeOptions.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectGraphNodes"

namespace CrowdyEffectGraphPins
{
	const FName ValueCategory(TEXT("CrowdyValue"));
	const FName BoolCategory(TEXT("CrowdyBool"));
}

namespace
{
	// Presentation glyphs for node titles. Kept local to the node widgets: they are display text, deliberately
	// separate from the compiler's emit glyphs (the graph may show a friendlier label than it emits).
	const TCHAR* ArithGlyph(ECrowdyEffectGraphArithOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectGraphArithOp::Add:      return TEXT("+");
		case ECrowdyEffectGraphArithOp::Subtract: return TEXT("-");
		case ECrowdyEffectGraphArithOp::Multiply: return TEXT("*");
		case ECrowdyEffectGraphArithOp::Divide:   return TEXT("/");
		case ECrowdyEffectGraphArithOp::Modulo:   return TEXT("%");
		default:                                  return TEXT("+");
		}
	}

	// The plain-word name of an operation, so a node reads as what it does ("Add") rather than as a symbol pattern
	// ("A + B"). The titles pair the two: "Add ( A + B )".
	const TCHAR* ArithOpName(ECrowdyEffectGraphArithOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectGraphArithOp::Add:      return TEXT("Add");
		case ECrowdyEffectGraphArithOp::Subtract: return TEXT("Subtract");
		case ECrowdyEffectGraphArithOp::Multiply: return TEXT("Multiply");
		case ECrowdyEffectGraphArithOp::Divide:   return TEXT("Divide");
		case ECrowdyEffectGraphArithOp::Modulo:   return TEXT("Modulo");
		default:                                  return TEXT("Add");
		}
	}

	const TCHAR* CompareOpName(ECrowdyEffectComparator Cmp)
	{
		switch (Cmp)
		{
		case ECrowdyEffectComparator::Equal:          return TEXT("Equal");
		case ECrowdyEffectComparator::NotEqual:       return TEXT("Not Equal");
		case ECrowdyEffectComparator::Less:           return TEXT("Less Than");
		case ECrowdyEffectComparator::Greater:        return TEXT("Greater Than");
		case ECrowdyEffectComparator::LessOrEqual:    return TEXT("Less Or Equal");
		case ECrowdyEffectComparator::GreaterOrEqual: return TEXT("Greater Or Equal");
		default:                                      return TEXT("Equal");
		}
	}

	const TCHAR* CompareDisplayGlyph(ECrowdyEffectComparator Cmp)
	{
		switch (Cmp)
		{
		case ECrowdyEffectComparator::Equal:          return TEXT("==");
		case ECrowdyEffectComparator::NotEqual:       return TEXT("!=");
		case ECrowdyEffectComparator::Less:           return TEXT("<");
		case ECrowdyEffectComparator::Greater:        return TEXT(">");
		case ECrowdyEffectComparator::LessOrEqual:    return TEXT("<=");
		case ECrowdyEffectComparator::GreaterOrEqual: return TEXT(">=");
		default:                                      return TEXT("==");
		}
	}

	// A plain, readable verb for a write pin ("Set", "Add to"), so a Result write reads as an instruction instead
	// of an operator glyph. Display only; the compiler still emits the real assignment operator.
	const TCHAR* AssignVerb(ECrowdyEffectAssignmentOp Op)
	{
		switch (Op)
		{
		case ECrowdyEffectAssignmentOp::Add:      return TEXT("Add to");
		case ECrowdyEffectAssignmentOp::Subtract: return TEXT("Subtract from");
		case ECrowdyEffectAssignmentOp::Multiply: return TEXT("Multiply");
		case ECrowdyEffectAssignmentOp::Divide:   return TEXT("Divide");
		case ECrowdyEffectAssignmentOp::Set:
		default:                                  return TEXT("Set");
		}
	}

	const TCHAR* RoleDisplayPrefix(ECrowdyEffectRole Role)
	{
		return Role == ECrowdyEffectRole::Source ? TEXT("source.") : TEXT("self.");
	}

	// The friendly label a Result write pin shows: "<verb> <attr>" for a Target write, "<verb> Source's <attr>" for a
	// Source write. Reads as a plain instruction the wired value completes ("Add to hp" by the wired amount); Target,
	// the common case, drops the possessive. An unset attribute falls back to "attribute" so the pin is never blank.
	FString WritePinLabel(ECrowdyEffectRole Role, const FString& Attribute, ECrowdyEffectAssignmentOp Op)
	{
		const FString Attr = Attribute.TrimStartAndEnd();
		const FString AttrWord = Attr.IsEmpty() ? TEXT("attribute") : Attr;
		const FString AttrPhrase = Role == ECrowdyEffectRole::Source
			? FString::Printf(TEXT("Source's %s"), *AttrWord)
			: AttrWord;
		return FString::Printf(TEXT("%s %s"), AssignVerb(Op), *AttrPhrase);
	}
}

void UCrowdyEffectGraphNode::ReconstructNode()
{
	Modify();

	// Reallocate pins, then move each old pin's links and default value onto the new pin with the same name and
	// direction. MovePersistentDataFromOldPin also repoints the pins on the other end, so the surviving connections
	// stay intact; any pin that no longer exists (a removed write / condition) simply drops its links.
	const TArray<UEdGraphPin*> OldPins = Pins;
	Pins.Reset();
	AllocateDefaultPins();

	for (UEdGraphPin* OldPin : OldPins)
	{
		if (UEdGraphPin* NewPin = FindPin(OldPin->PinName, OldPin->Direction))
		{
			NewPin->MovePersistentDataFromOldPin(*OldPin);
		}
	}

	for (UEdGraphPin* OldPin : OldPins)
	{
		OldPin->Modify();
		OldPin->BreakAllPinLinks();
		DestroyPin(OldPin);
	}
}

#if WITH_EDITOR
void UCrowdyEffectGraphNode::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Skip the mid-drag interactive events (e.g. a slider being scrubbed); the final change rebuilds the pins once,
	// so a scrub does not destroy and recreate pins and recompile the effect every frame.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	ReconstructNode();
	if (UEdGraph* Graph = GetGraph())
	{
		Graph->NotifyGraphChanged();
	}
}
#endif

FLinearColor UCrowdyEffectGraphNode::FamilyColor(ECrowdyEffectGraphNodeFamily Family)
{
	// One hue per palette category, so a graph is readable by colour before any title is read. Kept mid-saturation and
	// mid-value: a node title bar is drawn under white text in both editor themes, so a light or fully saturated
	// colour would lose the text.
	switch (Family)
	{
	case ECrowdyEffectGraphNodeFamily::Attribute:      return FLinearColor(0.10f, 0.45f, 0.50f);
	case ECrowdyEffectGraphNodeFamily::Value:          return FLinearColor(0.16f, 0.44f, 0.24f);
	case ECrowdyEffectGraphNodeFamily::Arithmetic:     return FLinearColor(0.15f, 0.33f, 0.62f);
	case ECrowdyEffectGraphNodeFamily::Comparison:     return FLinearColor(0.62f, 0.44f, 0.10f);
	case ECrowdyEffectGraphNodeFamily::Logic:          return FLinearColor(0.56f, 0.20f, 0.20f);
	case ECrowdyEffectGraphNodeFamily::Function:       return FLinearColor(0.30f, 0.26f, 0.58f);
	case ECrowdyEffectGraphNodeFamily::ServerFunction: return FLinearColor(0.48f, 0.18f, 0.48f);
	case ECrowdyEffectGraphNodeFamily::Flow:           return FLinearColor(0.32f, 0.34f, 0.38f);
	case ECrowdyEffectGraphNodeFamily::Result:         return FLinearColor(0.55f, 0.32f, 0.12f);
	}
	return FLinearColor(0.10f, 0.45f, 0.50f);
}

FLinearColor UCrowdyEffectGraphNode::GetNodeTitleColor() const
{
	return FamilyColor(GetNodeFamily());
}

UEdGraphPin* UCrowdyEffectGraphNode::CreateValueOutputPin(FName PinName)
{
	return CreatePin(EGPD_Output, CrowdyEffectGraphPins::ValueCategory, PinName);
}

UEdGraphPin* UCrowdyEffectGraphNode::CreateValueInputPin(FName PinName)
{
	return CreatePin(EGPD_Input, CrowdyEffectGraphPins::ValueCategory, PinName);
}

UEdGraphPin* UCrowdyEffectGraphNode::CreateBoolOutputPin(FName PinName)
{
	return CreatePin(EGPD_Output, CrowdyEffectGraphPins::BoolCategory, PinName);
}

UEdGraphPin* UCrowdyEffectGraphNode::CreateBoolInputPin(FName PinName)
{
	return CreatePin(EGPD_Input, CrowdyEffectGraphPins::BoolCategory, PinName);
}

FName UCrowdyEffectGraphNode_Tuning::OutputPinName()
{
	return FName(TEXT("Value"));
}

void UCrowdyEffectGraphNode_Tuning::AllocateDefaultPins()
{
	CreateValueOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Attribute::OutputPinName()
{
	return FName(TEXT("Value"));
}

void UCrowdyEffectGraphNode_Attribute::AllocateDefaultPins()
{
	CreateValueOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Constant::OutputPinName()
{
	return FName(TEXT("Value"));
}

void UCrowdyEffectGraphNode_Constant::AllocateDefaultPins()
{
	// A Bool constant is definitely boolean; a number, string, or null is a plain value.
	if (ConstantType == ECrowdyEffectGraphConstantType::Bool)
	{
		CreateBoolOutputPin(OutputPinName());
	}
	else
	{
		CreateValueOutputPin(OutputPinName());
	}
}

FName UCrowdyEffectGraphNode_BinaryOp::InputAPinName()
{
	return FName(TEXT("A"));
}

FName UCrowdyEffectGraphNode_BinaryOp::InputBPinName()
{
	return FName(TEXT("B"));
}

FName UCrowdyEffectGraphNode_BinaryOp::OutputPinName()
{
	return FName(TEXT("Result"));
}

void UCrowdyEffectGraphNode_BinaryOp::AllocateDefaultPins()
{
	// Stable order: the two operand inputs first, then the single output.
	CreateValueInputPin(InputAPinName());
	CreateValueInputPin(InputBPinName());
	CreateValueOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Compare::InputAPinName()
{
	return FName(TEXT("A"));
}

FName UCrowdyEffectGraphNode_Compare::InputBPinName()
{
	return FName(TEXT("B"));
}

FName UCrowdyEffectGraphNode_Compare::OutputPinName()
{
	return FName(TEXT("Result"));
}

void UCrowdyEffectGraphNode_Compare::AllocateDefaultPins()
{
	// The two operands are plain values (compare a number to a number); the result is definitely boolean.
	CreateValueInputPin(InputAPinName());
	CreateValueInputPin(InputBPinName());
	CreateBoolOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Logic::InputAPinName()
{
	return FName(TEXT("A"));
}

FName UCrowdyEffectGraphNode_Logic::InputBPinName()
{
	return FName(TEXT("B"));
}

FName UCrowdyEffectGraphNode_Logic::OutputPinName()
{
	return FName(TEXT("Result"));
}

void UCrowdyEffectGraphNode_Logic::AllocateDefaultPins()
{
	// A boolean connective takes two booleans and yields a boolean.
	CreateBoolInputPin(InputAPinName());
	CreateBoolInputPin(InputBPinName());
	CreateBoolOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Unary::InputPinName()
{
	return FName(TEXT("A"));
}

FName UCrowdyEffectGraphNode_Unary::OutputPinName()
{
	return FName(TEXT("Result"));
}

void UCrowdyEffectGraphNode_Unary::AllocateDefaultPins()
{
	// Logical not takes and yields a boolean; arithmetic negate takes and yields a plain value.
	if (Op == ECrowdyEffectGraphUnaryOp::Not)
	{
		CreateBoolInputPin(InputPinName());
		CreateBoolOutputPin(OutputPinName());
	}
	else
	{
		CreateValueInputPin(InputPinName());
		CreateValueOutputPin(OutputPinName());
	}
}

FName UCrowdyEffectGraphNode_If::ConditionPinName()
{
	return FName(TEXT("Cond"));
}

FName UCrowdyEffectGraphNode_If::ThenPinName()
{
	return FName(TEXT("Then"));
}

FName UCrowdyEffectGraphNode_If::ElsePinName()
{
	return FName(TEXT("Else"));
}

FName UCrowdyEffectGraphNode_If::OutputPinName()
{
	return FName(TEXT("Result"));
}

void UCrowdyEffectGraphNode_If::AllocateDefaultPins()
{
	// The condition strictly needs a boolean; Then / Else and the result are plain values (either branch may be a
	// number, a string, or itself a boolean).
	CreateBoolInputPin(ConditionPinName());
	CreateValueInputPin(ThenPinName());
	CreateValueInputPin(ElsePinName());
	CreateValueOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Call::ArgPinName(int32 Index)
{
	return FName(*FString::Printf(TEXT("Arg_%d"), Index));
}

FName UCrowdyEffectGraphNode_Call::OutputPinName()
{
	return FName(TEXT("Result"));
}

void UCrowdyEffectGraphNode_Call::AllocateDefaultPins()
{
	// A builtin's arity is fixed by the language, so a stale or hand-edited count is corrected here rather than
	// allowed to author an expression the server would reject. A server call's arity is unknown to the editor, so its
	// count passes through untouched.
	if (!bIsFnCall)
	{
		ArgCount = CrowdyEffectGraphNodeOptions::ClampArgCount(Callee, ArgCount);
	}

	// Pin NAMES stay index-based, since the compiler finds arguments by them; only the friendly label changes, so a
	// builtin's pins read as what they are ("Permission Key", "Chunk X") instead of Arg_0, Arg_1.
	for (int32 Index = 0; Index < ArgCount; ++Index)
	{
		UEdGraphPin* Pin = CreateValueInputPin(ArgPinName(Index));
		const FString Label = bIsFnCall
			? FString()
			: CrowdyEffectGraphNodeOptions::ArgDisplayName(Callee, Index);
		if (!Label.IsEmpty())
		{
			Pin->PinFriendlyName = FText::FromString(Label);
		}
	}
	CreateValueOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_ReadRef::IdPinName()
{
	return FName(TEXT("Id"));
}

FName UCrowdyEffectGraphNode_ReadRef::OutputPinName()
{
	return FName(TEXT("Value"));
}

void UCrowdyEffectGraphNode_ReadRef::AllocateDefaultPins()
{
	CreateValueInputPin(IdPinName());
	CreateValueOutputPin(OutputPinName());
}

FName UCrowdyEffectGraphNode_Result::WritePinName(int32 Index)
{
	return FName(*FString::Printf(TEXT("Write_%d"), Index));
}

FName UCrowdyEffectGraphNode_Result::RequirePinName(int32 Index)
{
	return FName(*FString::Printf(TEXT("Require_%d"), Index));
}

FName UCrowdyEffectGraphNode_Result::ReturnPinName()
{
	return FName(TEXT("Return"));
}

void UCrowdyEffectGraphNode_Result::AllocateDefaultPins()
{
	// One input pin per write, in declared order, then one boolean input pin per condition. Keyword conditions carry
	// no pins. Friendly names describe what each pin writes / gates so the node reads at a glance; the compiler still
	// finds pins by their stable index-based name, so these are display only. Each pin also carries a stable
	// PersistentGuid (the write's / condition's PinId) so ReconstructNode can preserve a wired value across a mid-array
	// edit, when the index-based name alone would remap it to the wrong entry.
	for (int32 Index = 0; Index < Writes.Num(); ++Index)
	{
		FCrowdyEffectGraphWrite& Write = Writes[Index];
		if (!Write.PinId.IsValid())
		{
			Write.PinId = FGuid::NewGuid();
		}
		UEdGraphPin* Pin = CreateValueInputPin(WritePinName(Index));
		Pin->PersistentGuid = Write.PinId;
		Pin->PinFriendlyName = FText::FromString(WritePinLabel(Write.TargetRole, Write.Attribute, Write.Op));
	}
	for (int32 Index = 0; Index < Requires.Num(); ++Index)
	{
		FCrowdyEffectGraphRequire& Require = Requires[Index];
		if (!Require.PinId.IsValid())
		{
			Require.PinId = FGuid::NewGuid();
		}
		UEdGraphPin* Pin = CreateBoolInputPin(RequirePinName(Index));
		Pin->PersistentGuid = Require.PinId;

		// One boolean value drives each condition. Label the pin with the author's note when set, else a plain prompt.
		const FString Note = Require.Note.TrimStartAndEnd();
		Pin->PinFriendlyName = FText::FromString(Note.IsEmpty()
			? FString(TEXT("Only if..."))
			: FString::Printf(TEXT("Only if: %s"), *Note));
	}

	// The Return pin comes last, after every write and condition, so a graph reads writes-then-gates-then-answer.
	if (bReturnsValue)
	{
		if (!ReturnPinId.IsValid())
		{
			ReturnPinId = FGuid::NewGuid();
		}
		UEdGraphPin* Pin = CreateValueInputPin(ReturnPinName());
		Pin->PersistentGuid = ReturnPinId;
		Pin->PinFriendlyName = FText::FromString(TEXT("Return"));
		Pin->PinToolTip = TEXT("The value the effect answers with when it is invoked.");
	}
}

void UCrowdyEffectGraphNode_Result::ReconstructNode()
{
	Modify();

	// Match old pins to new by their stable PersistentGuid, not by index-based name: a value wired into a write or a
	// condition operand then follows that write / operand across an insert, remove, or reorder in the details panel.
	// A pin whose guid no longer exists (a removed write) simply drops its links.
	const TArray<UEdGraphPin*> OldPins = Pins;
	Pins.Reset();
	AllocateDefaultPins();

	TMap<FGuid, UEdGraphPin*> NewPinsByGuid;
	NewPinsByGuid.Reserve(Pins.Num());
	for (UEdGraphPin* NewPin : Pins)
	{
		if (NewPin->PersistentGuid.IsValid())
		{
			NewPinsByGuid.Add(NewPin->PersistentGuid, NewPin);
		}
	}

	for (UEdGraphPin* OldPin : OldPins)
	{
		if (UEdGraphPin** Found = NewPinsByGuid.Find(OldPin->PersistentGuid))
		{
			(*Found)->MovePersistentDataFromOldPin(*OldPin);
		}
	}

	for (UEdGraphPin* OldPin : OldPins)
	{
		OldPin->Modify();
		OldPin->BreakAllPinLinks();
		DestroyPin(OldPin);
	}
}

FText UCrowdyEffectGraphNode_Tuning::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	// The language spells a magnitude "$name", but the node reads as a plain parameter; the sigil is emit syntax, not
	// something the author should have to see.
	const FString Name = ParamName.TrimStartAndEnd();
	return FText::FromString(Name.IsEmpty()
		? TEXT("Tuning Parameter")
		: FString::Printf(TEXT("Tuning Parameter: %s"), *Name));
}

FText UCrowdyEffectGraphNode_Tuning::GetTooltipText() const
{
	return LOCTEXT("TuningTip", "A tuning parameter. Reads a designer-tunable value exposed on the effect asset.");
}

FText UCrowdyEffectGraphNode_Attribute::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FString Attr = Attribute.TrimStartAndEnd();
	return FText::FromString(Attr.IsEmpty()
		? FString::Printf(TEXT("%sattribute"), RoleDisplayPrefix(Role))
		: FString::Printf(TEXT("%s%s"), RoleDisplayPrefix(Role), *Attr));
}

FText UCrowdyEffectGraphNode_Attribute::GetTooltipText() const
{
	return LOCTEXT("AttributeTip", "Reads an attribute of the Target (self) or Source (instigator) container.");
}

FText UCrowdyEffectGraphNode_Constant::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	switch (ConstantType)
	{
	case ECrowdyEffectGraphConstantType::Null:   return LOCTEXT("ConstNull", "Null");
	case ECrowdyEffectGraphConstantType::String: return FText::FromString(FString::Printf(TEXT("\"%s\""), *Literal));
	case ECrowdyEffectGraphConstantType::Bool:
	{
		const FString Display = CrowdyEffectGraphNodeOptions::BoolLiteralDisplayName(Literal);
		return Display.IsEmpty() ? LOCTEXT("ConstBool", "Bool") : FText::FromString(Display);
	}
	default:                                     return FText::FromString(Literal.IsEmpty() ? TEXT("Constant") : Literal);
	}
}

FText UCrowdyEffectGraphNode_Constant::GetTooltipText() const
{
	return LOCTEXT("ConstantTip", "A literal value: a number, a bool, a string, or null.");
}

FText UCrowdyEffectGraphNode_BinaryOp::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return FText::FromString(FString::Printf(TEXT("%s ( A %s B )"), ArithOpName(Op), ArithGlyph(Op)));
}

FText UCrowdyEffectGraphNode_BinaryOp::GetTooltipText() const
{
	return LOCTEXT("BinaryOpTip", "An arithmetic operation ( + - * / % ) joining two values.");
}

FText UCrowdyEffectGraphNode_Compare::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return FText::FromString(FString::Printf(TEXT("%s ( A %s B )"),
		CompareOpName(Comparator), CompareDisplayGlyph(Comparator)));
}

FText UCrowdyEffectGraphNode_Compare::GetTooltipText() const
{
	return LOCTEXT("CompareTip", "Compares two values into a boolean ( == != < > <= >= ).");
}

FText UCrowdyEffectGraphNode_Logic::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return Op == ECrowdyEffectGraphLogicOp::Or
		? LOCTEXT("LogicOrTitle", "Or ( A || B )")
		: LOCTEXT("LogicAndTitle", "And ( A && B )");
}

FText UCrowdyEffectGraphNode_Logic::GetTooltipText() const
{
	return LOCTEXT("LogicTip", "A boolean connective ( && / || ) joining two boolean values.");
}

ECrowdyEffectGraphNodeFamily UCrowdyEffectGraphNode_Unary::GetNodeFamily() const
{
	return Op == ECrowdyEffectGraphUnaryOp::Negate
		? ECrowdyEffectGraphNodeFamily::Arithmetic
		: ECrowdyEffectGraphNodeFamily::Logic;
}

FText UCrowdyEffectGraphNode_Unary::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return Op == ECrowdyEffectGraphUnaryOp::Negate
		? LOCTEXT("UnaryNegateTitle", "Negate ( -X )")
		: LOCTEXT("UnaryNotTitle", "Not ( !X )");
}

FText UCrowdyEffectGraphNode_Unary::GetTooltipText() const
{
	return LOCTEXT("UnaryTip", "A prefix operation: logical not (!) or arithmetic negate (-).");
}

FText UCrowdyEffectGraphNode_If::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("IfTitle", "Select ( If / Then / Else )");
}

FText UCrowdyEffectGraphNode_If::GetTooltipText() const
{
	return LOCTEXT("IfTip", "A ternary select: yields Then when Cond is true, otherwise Else.");
}

FText UCrowdyEffectGraphNode_Call::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FString Name = Callee.TrimStartAndEnd();
	if (Name.IsEmpty())
	{
		return LOCTEXT("CallTitleEmpty", "Function Call");
	}

	// A legacy call node may still carry the server-call flag from before the dedicated Server Function node existed,
	// so it keeps reading as what it actually compiles to.
	return bIsFnCall
		? FText::Format(LOCTEXT("CallTitleServer", "Server Function: {0}"), FText::FromString(Name))
		: FText::FromString(CrowdyEffectGraphNodeOptions::BuiltinDisplayName(Name));
}

FText UCrowdyEffectGraphNode_Call::GetTooltipText() const
{
	// A placed node explains itself with the same text its palette entry carried, so an author does not have to go
	// back to the menu to remember what a function does or when to reach for it.
	if (const FCrowdyEffectBuiltinCall* Builtin = CrowdyEffectGraphNodeOptions::FindBuiltin(Callee))
	{
		return FText::FromString(Builtin->Description);
	}
	return LOCTEXT("CallTip", "Calls one of the effect language's builtin functions (Max, Min, Clamp, Coalesce, ...).");
}

UCrowdyEffectGraphNode_ServerCall::UCrowdyEffectGraphNode_ServerCall()
{
	bIsFnCall = true;
}

FText UCrowdyEffectGraphNode_ServerCall::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FString Name = Callee.TrimStartAndEnd();
	return Name.IsEmpty()
		? LOCTEXT("ServerCallTitleEmpty", "Server Function")
		: FText::Format(LOCTEXT("ServerCallTitle", "Server Function: {0}"), FText::FromString(Name));
}

FText UCrowdyEffectGraphNode_ServerCall::GetTooltipText() const
{
	return LOCTEXT("ServerCallTip",
		"Calls a function you authored on this effect, by name. Not one of the language's builtins: the schema sync "
		"uploads it, and the server resolves it when the effect runs.");
}


FText UCrowdyEffectGraphNode_ReadRef::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	const FString Attr = Attribute.TrimStartAndEnd();
	return Attr.IsEmpty()
		? LOCTEXT("ReadRefTitleEmpty", "Read Referenced Attribute")
		: FText::Format(LOCTEXT("ReadRefTitle", "Read Referenced: {0}"), FText::FromString(Attr));
}

FText UCrowdyEffectGraphNode_ReadRef::GetTooltipText() const
{
	return LOCTEXT("ReadRefTip", "Reads an attribute from another container by its id ( ref(id).attribute ).");
}

FString UCrowdyEffectGraphNode_Result::DescribeWiredCondition(int32 RequireIndex) const
{
	if (!Requires.IsValidIndex(RequireIndex))
	{
		return FString();
	}

	// Matched on the condition's stable PinId (stamped on the pin's PersistentGuid), not the engine's own per-pin
	// PinId, so the lookup survives a reconstruct the same way the wiring does.
	const FGuid& Wanted = Requires[RequireIndex].PinId;
	for (const UEdGraphPin* Pin : Pins)
	{
		if (!Pin || Pin->PersistentGuid != Wanted || Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
		{
			continue;
		}
		if (const UEdGraphNode* Driver = Pin->LinkedTo[0]->GetOwningNodeUnchecked())
		{
			return Driver->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
	}
	return FString();
}

FText UCrowdyEffectGraphNode_Result::GetPinDisplayName(const UEdGraphPin* Pin) const
{
	if (Pin && Pin->PersistentGuid.IsValid())
	{
		for (int32 Index = 0; Index < Requires.Num(); ++Index)
		{
			if (Requires[Index].PinId != Pin->PersistentGuid)
			{
				continue;
			}
			if (!Requires[Index].Note.TrimStartAndEnd().IsEmpty())
			{
				break;
			}
			const FString Wired = DescribeWiredCondition(Index);
			if (!Wired.IsEmpty())
			{
				return FText::Format(LOCTEXT("ConditionPinWired", "Only if: {0}"), FText::FromString(Wired));
			}
			break;
		}
	}
	return Super::GetPinDisplayName(Pin);
}

FText UCrowdyEffectGraphNode_Result::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("ResultTitle", "Result");
}

FText UCrowdyEffectGraphNode_Result::GetTooltipText() const
{
	return LOCTEXT("ResultTip", "The effect's output: the writes it applies and the conditions that gate it.");
}


#undef LOCTEXT_NAMESPACE
