// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Nodes/CrowdyApplyEffectNodePins.h"
#include "Nodes/CrowdyApplyEffectNodeShared.h"
#include "Nodes/CrowdyK2Node_ApplyEffectFireAndForget.h"
#include "Nodes/CrowdyK2Node_ApplyEffectToContainer.h"
#include "Replication/GameModel/CrowdyEffectActions.h"
#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyApplyEffectCallTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	int32 AsInt(CrowdyApplyEffectNodeShared::EParameterPinAction Action)
	{
		return static_cast<int32>(Action);
	}
}

// A node that lowers to a plain library call cannot drop a parameter pin: the call fails to compile without one.
// ActionForParameterPin is the single rule the nodes apply, and it only ever says keep or hide. The raw Overrides map
// is always hidden once an effect is known (the typed magnitude pins replace it), Level survives only when a
// magnitude samples a curve, Source only when the effect reads a Source, and with no effect known nothing is hidden
// at all so the designer can still fill the map by hand.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectCallPinActionTest,
	"CrowdySDK.Editor.ApplyEffectCallPinAction", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectCallPinActionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	FCrowdyApplyEffectPinPlan Plain;
	FCrowdyApplyEffectPinPlan Curved;
	Curved.bIncludeLevel = true;
	FCrowdyApplyEffectPinPlan Sourced;
	Sourced.bIncludeSource = true;

	const int32 Keep = AsInt(EParameterPinAction::Keep);
	const int32 Hide = AsInt(EParameterPinAction::HideAndReset);

	// No literal effect: the layout cannot be derived, so every pin stays as authored.
	TestEqual(TEXT("no effect keeps Overrides"), AsInt(ActionForParameterPin(PN_Overrides, Plain, false)), Keep);
	TestEqual(TEXT("no effect keeps Level"), AsInt(ActionForParameterPin(PN_Level, Plain, false)), Keep);
	TestEqual(TEXT("no effect keeps Source"), AsInt(ActionForParameterPin(PN_Source, Plain, false)), Keep);
	TestEqual(TEXT("no effect keeps Level even with a curve plan"),
		AsInt(ActionForParameterPin(PN_Level, Curved, false)), Keep);

	// A literal effect: the typed magnitude pins take over from the raw map.
	TestEqual(TEXT("Overrides is always hidden once an effect is known"),
		AsInt(ActionForParameterPin(PN_Overrides, Plain, true)), Hide);
	TestEqual(TEXT("Overrides stays hidden for a curve effect"),
		AsInt(ActionForParameterPin(PN_Overrides, Curved, true)), Hide);

	// Level follows the curve flag, Source follows the source flag, and neither leaks into the other.
	TestEqual(TEXT("Level hidden with no curve magnitude"), AsInt(ActionForParameterPin(PN_Level, Plain, true)), Hide);
	TestEqual(TEXT("Level kept with a curve magnitude"), AsInt(ActionForParameterPin(PN_Level, Curved, true)), Keep);
	TestEqual(TEXT("Level hidden when only Source is used"),
		AsInt(ActionForParameterPin(PN_Level, Sourced, true)), Hide);

	TestEqual(TEXT("Source hidden when the effect reads no source"),
		AsInt(ActionForParameterPin(PN_Source, Plain, true)), Hide);
	TestEqual(TEXT("Source kept when the effect reads a source"),
		AsInt(ActionForParameterPin(PN_Source, Sourced, true)), Keep);
	TestEqual(TEXT("Source hidden when only a curve is used"),
		AsInt(ActionForParameterPin(PN_Source, Curved, true)), Hide);

	// Every other pin of the call is left alone whatever the effect declares.
	const FName Untouched[] = {
		PN_Effect,
		FName(TEXT("Target")),
		FName(TEXT("WorldContext")),
		FName(TEXT("ContainerId")),
		FName(TEXT("SessionId")),
	};
	for (const FName PinName : Untouched)
	{
		TestEqual(*FString::Printf(TEXT("%s is left alone"), *PinName.ToString()),
			AsInt(ActionForParameterPin(PinName, Curved, true)), Keep);
	}

	return true;
}

// The magnitude pins are the node's own authoring surface, not arguments of the function it calls, so they are the
// only pins dropped before the call is emitted. Dropping anything else would remove a parameter the call needs, and
// keeping a magnitude pin would leave a pin that matches no parameter; both fail to compile. This drives the rule
// against the real parameter lists of both wrapped functions, so a rename there is caught here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectCallPinRemovalTest,
	"CrowdySDK.Editor.ApplyEffectCallPinRemoval", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectCallPinRemovalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	TestTrue(TEXT("a synthesized magnitude pin is dropped"),
		ShouldRemovePinBeforeCall(CrowdyApplyEffectNodePins::MagnitudePinName(TEXT("power"))));
	TestTrue(TEXT("a magnitude pin whose name matches a parameter is still dropped"),
		ShouldRemovePinBeforeCall(CrowdyApplyEffectNodePins::MagnitudePinName(TEXT("Level"))));

	const FName SharedPins[] = { PN_Effect, PN_Source, PN_Overrides, PN_Level };
	for (const FName PinName : SharedPins)
	{
		TestFalse(*FString::Printf(TEXT("%s is not dropped"), *PinName.ToString()),
			ShouldRemovePinBeforeCall(PinName));
	}

	UFunction* Functions[] = {
		UCrowdyEffects::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, Apply)),
		UCrowdyEffects::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, ApplyToContainer)),
	};

	int32 ParametersChecked = 0;
	for (UFunction* Function : Functions)
	{
		if (!TestNotNull(TEXT("the wrapped library function resolves"), Function))
		{
			continue;
		}
		for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			++ParametersChecked;
			TestFalse(*FString::Printf(TEXT("parameter %s of %s survives to the call"),
				*It->GetName(), *Function->GetName()), ShouldRemovePinBeforeCall(It->GetFName()));
		}
	}

	// Guard against a vacuous pass if reflection ever hands back an empty parameter list.
	TestTrue(TEXT("parameters were actually inspected"), ParametersChecked >= 12);

	return true;
}

// Every pin the reshape touches is addressed by name, so the whole node quietly degrades to a plain call if a
// parameter of the wrapped function is renamed and the name here is not. This pins both node classes to the function
// they call and each shared pin name to a real parameter of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectCallTargetSignatureTest,
	"CrowdySDK.Editor.ApplyEffectCallTargetSignature", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectCallTargetSignatureTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	struct FCase
	{
		const UCrowdyK2Node_ApplyEffectCallBase* Node = nullptr;
		const TCHAR* ExpectedFunctionName = nullptr;
	};

	const FCase Cases[] = {
		{ GetDefault<UCrowdyK2Node_ApplyEffectFireAndForget>(), TEXT("Apply") },
		{ GetDefault<UCrowdyK2Node_ApplyEffectToContainer>(), TEXT("ApplyToContainer") },
	};

	const FName SharedPins[] = { PN_Effect, PN_Source, PN_Overrides, PN_Level };

	for (const FCase& Case : Cases)
	{
		if (!TestNotNull(TEXT("the node class default object exists"), Case.Node))
		{
			continue;
		}

		UFunction* Function = Case.Node->GetWrappedFunction();
		if (!TestNotNull(*FString::Printf(TEXT("%s resolves its wrapped function"), Case.ExpectedFunctionName),
			Function))
		{
			continue;
		}

		TestEqual(TEXT("the node wraps the expected function"), Function->GetName(),
			FString(Case.ExpectedFunctionName));

		for (const FName PinName : SharedPins)
		{
			TestNotNull(*FString::Printf(TEXT("%s has a parameter named %s"),
				*Function->GetName(), *PinName.ToString()), Function->FindPropertyByName(PinName));
		}
	}

	return true;
}

// An unwired Target has to fall back to self, and that behaviour is entirely carried by the DefaultToSelf metadata
// on the wrapped functions. Nothing about it is visible at compile time, so a dropped or misspelled meta value would
// silently restore the old "applies to nothing" behaviour. This pins the parameter and the metadata together, on
// both functions that take a Target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectTargetDefaultsToSelfTest,
	"CrowdySDK.Editor.ApplyEffectTargetDefaultsToSelf", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectTargetDefaultsToSelfTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	UFunction* const Targeted[] = {
		UCrowdyEffects::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UCrowdyEffects, Apply)),
		UCrowdyApplyEffectAction::StaticClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(UCrowdyApplyEffectAction, ApplyEffect)),
	};

	for (UFunction* Function : Targeted)
	{
		if (!TestNotNull(TEXT("the targeted function resolves"), Function))
		{
			continue;
		}
		TestNotNull(*FString::Printf(TEXT("%s takes a Target"), *Function->GetName()),
			Function->FindPropertyByName(PN_Target));
		TestEqual(*FString::Printf(TEXT("%s names Target as its DefaultToSelf parameter"), *Function->GetName()),
			Function->GetMetaData(TEXT("DefaultToSelf")), PN_Target.ToString());
	}

	// The by-id node addresses a container directly and has no Target at all, which is why Target is not in the
	// shared pin list above. The validation helper treats a missing pin as nothing to judge.
	UFunction* ByIdFunction = GetDefault<UCrowdyK2Node_ApplyEffectToContainer>()->GetWrappedFunction();
	if (TestNotNull(TEXT("the by-id function resolves"), ByIdFunction))
	{
		TestNull(TEXT("the by-id function takes no Target"), ByIdFunction->FindPropertyByName(PN_Target));
	}
	return true;
}

// The self-target check must fire only when Target really will fall back to self. A wired pin, or one holding an
// explicit object, means the author chose the target and its type is not this node's business.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectTargetResolvesToSelfTest,
	"CrowdySDK.Editor.ApplyEffectTargetResolvesToSelf", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectTargetResolvesToSelfTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	TestFalse(TEXT("a node with no Target pin is not judged"),
		TargetResolvesToSelf(static_cast<const UEdGraphPin*>(nullptr)));

	TestTrue(TEXT("an empty, unlinked Target falls back to self"),
		TargetResolvesToSelf(/*bHasLinks*/ false, /*bHasDefaultObject*/ false));
	TestFalse(TEXT("a wired Target is the author's explicit choice"),
		TargetResolvesToSelf(/*bHasLinks*/ true, /*bHasDefaultObject*/ false));
	TestFalse(TEXT("an explicit default object is not self"),
		TargetResolvesToSelf(/*bHasLinks*/ false, /*bHasDefaultObject*/ true));

	// A class with no CrowdyContainer tag is not a usable self target; a null class is simply unknown.
	TestFalse(TEXT("an untagged class is not a container"), IsGameModelContainerClass(UCrowdyEffects::StaticClass()));
	TestFalse(TEXT("a null class is not a container"), IsGameModelContainerClass(nullptr));
	return true;
}

// The container marker lives in an editor module that installs itself as a resolver here. The contract that matters
// is what an absent resolver means: it must say "I don't know", never "not a container", or a target without that
// module would report every unwired Target as an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectContainerResolverTest,
	"CrowdySDK.Editor.ApplyEffectContainerResolver", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectContainerResolverTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	// The editor installs the real resolver at startup; put it back whatever this test does.
	TFunction<bool(const UBlueprint*)> Original = GetContainerBlueprintResolver();

	UBlueprint* AnyBlueprint = NewObject<UBlueprint>(GetTransientPackage());

	SetContainerBlueprintResolver(nullptr);
	TestFalse(TEXT("with no resolver installed nothing is recognized as a container"),
		IsGameModelContainerBlueprint(AnyBlueprint));

	SetContainerBlueprintResolver([](const UBlueprint*) { return true; });
	TestTrue(TEXT("an installed resolver's answer is used"), IsGameModelContainerBlueprint(AnyBlueprint));
	TestFalse(TEXT("a null Blueprint is never a container, whatever the resolver says"),
		IsGameModelContainerBlueprint(nullptr));

	SetContainerBlueprintResolver([](const UBlueprint*) { return false; });
	TestFalse(TEXT("a resolver that declines is respected"), IsGameModelContainerBlueprint(AnyBlueprint));

	SetContainerBlueprintResolver(MoveTemp(Original));
	return true;
}

// A magnitude pin's value reaches the Overrides map through one of the library's encoders, picked by value type. The
// wrong encoder would silently mistype a value on the wire (a bool sent as a quoted string, for example), so this
// pins both the encoder chosen and the name of the input parameter its value is written to, and checks each one
// still resolves against the live library.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectMagnitudeEncoderTest,
	"CrowdySDK.Editor.ApplyEffectMagnitudeEncoder", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectMagnitudeEncoderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;
	using EVT = ECrowdyEffectValueType;

	struct FCase
	{
		EVT ValueType;
		const TCHAR* FunctionName;
		const TCHAR* InputParam;
	};

	const FCase Cases[] = {
		{ EVT::Int, TEXT("JsonFromInt"), TEXT("Value") },
		{ EVT::Float, TEXT("JsonFromFloat"), TEXT("Value") },
		{ EVT::Bool, TEXT("JsonFromBool"), TEXT("Value") },
		{ EVT::String, TEXT("JsonFromString"), TEXT("Value") },
		{ EVT::ContainerRef, TEXT("GetContainerIdFor"), TEXT("Object") },
	};

	for (const FCase& Case : Cases)
	{
		FName InputParam;
		const FName EncoderName = EncoderFunctionName(Case.ValueType, InputParam);

		TestEqual(*FString::Printf(TEXT("encoder for %s"), Case.FunctionName),
			EncoderName.ToString(), FString(Case.FunctionName));
		TestEqual(*FString::Printf(TEXT("input parameter of %s"), Case.FunctionName),
			InputParam.ToString(), FString(Case.InputParam));

		UFunction* Encoder = UCrowdyEffects::StaticClass()->FindFunctionByName(EncoderName);
		if (!TestNotNull(*FString::Printf(TEXT("%s resolves"), Case.FunctionName), Encoder))
		{
			continue;
		}
		TestNotNull(*FString::Printf(TEXT("%s takes a parameter named %s"), Case.FunctionName, Case.InputParam),
			Encoder->FindPropertyByName(InputParam));
	}

	return true;
}

// The pin type a magnitude is exposed as decides what a designer can wire into it and what the encoder receives, so
// each value type maps to one fixed graph type: a float is a double-precision real, a container_ref is a plain
// object reference, and so on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectMagnitudePinTypeTest,
	"CrowdySDK.Editor.ApplyEffectMagnitudePinType", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectMagnitudePinTypeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;
	using EVT = ECrowdyEffectValueType;

	TestEqual(TEXT("int is an int pin"), MagnitudePinType(EVT::Int).PinCategory.ToString(),
		UEdGraphSchema_K2::PC_Int.ToString());
	TestEqual(TEXT("bool is a bool pin"), MagnitudePinType(EVT::Bool).PinCategory.ToString(),
		UEdGraphSchema_K2::PC_Boolean.ToString());
	TestEqual(TEXT("string is a string pin"), MagnitudePinType(EVT::String).PinCategory.ToString(),
		UEdGraphSchema_K2::PC_String.ToString());

	const FEdGraphPinType FloatType = MagnitudePinType(EVT::Float);
	TestEqual(TEXT("float is a real pin"), FloatType.PinCategory.ToString(), UEdGraphSchema_K2::PC_Real.ToString());
	TestEqual(TEXT("float is double precision"), FloatType.PinSubCategory.ToString(),
		UEdGraphSchema_K2::PC_Double.ToString());

	const FEdGraphPinType RefType = MagnitudePinType(EVT::ContainerRef);
	TestEqual(TEXT("container_ref is an object pin"), RefType.PinCategory.ToString(),
		UEdGraphSchema_K2::PC_Object.ToString());
	TestTrue(TEXT("container_ref accepts any object"),
		RefType.PinSubCategoryObject.Get() == static_cast<UObject*>(UObject::StaticClass()));

	return true;
}

// A declared return type has to line up with the magnitude value types, because the typed return pin and a magnitude
// pin of the same type must be one graph type: a designer wires them into the same places, and the splice relies on
// the pin type matching what the decoder hands back. Routing the return through MagnitudePinType is what guarantees
// that, so this asserts the mapping and the routing rather than a second hand-written table.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectReturnPinTypeTest,
	"CrowdySDK.Editor.ApplyEffectReturnPinType", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectReturnPinTypeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;
	using ERT = ECrowdyEffectReturnType;
	using EVT = ECrowdyEffectValueType;

	struct FCase
	{
		ERT ReturnType;
		EVT ValueType;
		const TCHAR* Label;
	};

	const FCase Cases[] = {
		{ ERT::Int, EVT::Int, TEXT("int") },
		{ ERT::Float, EVT::Float, TEXT("float") },
		{ ERT::Bool, EVT::Bool, TEXT("bool") },
		{ ERT::String, EVT::String, TEXT("string") },
	};

	for (const FCase& Case : Cases)
	{
		EVT Mapped = EVT::ContainerRef;
		if (!TestTrue(*FString::Printf(TEXT("a declared %s return maps to a value type"), Case.Label),
			ReturnValueType(Case.ReturnType, Mapped)))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("a declared %s return maps to the same-named value type"), Case.Label),
			static_cast<int32>(Mapped), static_cast<int32>(Case.ValueType));

		// The whole point of routing through MagnitudePinType: one pin-type switch, so a return pin and a magnitude
		// pin of the same type can never drift apart.
		const FEdGraphPinType FromReturn = ReturnPinType(Case.ReturnType);
		const FEdGraphPinType FromMagnitude = MagnitudePinType(Case.ValueType);
		TestEqual(*FString::Printf(TEXT("a %s return pin has the magnitude pin's category"), Case.Label),
			FromReturn.PinCategory.ToString(), FromMagnitude.PinCategory.ToString());
		TestEqual(*FString::Printf(TEXT("a %s return pin has the magnitude pin's sub-category"), Case.Label),
			FromReturn.PinSubCategory.ToString(), FromMagnitude.PinSubCategory.ToString());
	}

	// None declares nothing, so there is no pin to type and no value type to answer with.
	EVT Unmapped = EVT::ContainerRef;
	TestFalse(TEXT("None maps to no value type"), ReturnValueType(ERT::None, Unmapped));

	return true;
}

// The typed return pin is fed by one of the library's decoders, picked by declared type. A wrong decoder would
// silently mistype the value; worse, a decoder whose return type does not match the pin type it feeds would make the
// compiler insert a conversion or refuse the connection, and neither is visible from reading the splice. So this pins
// the decoder chosen, the name of the parameter its JSON is written to, and the pin type its result presents as.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectReturnDecoderTest,
	"CrowdySDK.Editor.ApplyEffectReturnDecoder", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectReturnDecoderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;
	using ERT = ECrowdyEffectReturnType;

	struct FCase
	{
		ERT ReturnType;
		const TCHAR* FunctionName;
	};

	const FCase Cases[] = {
		{ ERT::Int, TEXT("JsonToInt") },
		{ ERT::Float, TEXT("JsonToFloat") },
		{ ERT::Bool, TEXT("JsonToBool") },
		{ ERT::String, TEXT("JsonToString") },
	};

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!TestNotNull(TEXT("the K2 schema resolves"), Schema))
	{
		return false;
	}

	for (const FCase& Case : Cases)
	{
		FName InputParam;
		const FName DecoderName = DecoderFunctionName(Case.ReturnType, InputParam);

		TestEqual(*FString::Printf(TEXT("decoder for %s"), Case.FunctionName),
			DecoderName.ToString(), FString(Case.FunctionName));
		TestEqual(*FString::Printf(TEXT("input parameter of %s"), Case.FunctionName),
			InputParam.ToString(), FString(TEXT("ValueJson")));

		UFunction* Decoder = UCrowdyEffects::StaticClass()->FindFunctionByName(DecoderName);
		if (!TestNotNull(*FString::Printf(TEXT("%s resolves"), Case.FunctionName), Decoder))
		{
			continue;
		}
		TestNotNull(*FString::Printf(TEXT("%s takes a parameter named %s"), Case.FunctionName, *InputParam.ToString()),
			Decoder->FindPropertyByName(InputParam));

		FProperty* ReturnProperty = Decoder->GetReturnProperty();
		if (!TestNotNull(*FString::Printf(TEXT("%s returns something"), Case.FunctionName), ReturnProperty))
		{
			continue;
		}

		// The splice connects this decoder's result straight to the node's typed pin. If the two graph types differ
		// at all the connection either fails or quietly grows a conversion, so they must be identical. A float-width
		// return against a double-width pin is the specific way this goes wrong.
		FEdGraphPinType DecoderReturnType;
		if (!TestTrue(*FString::Printf(TEXT("%s's return converts to a pin type"), Case.FunctionName),
			Schema->ConvertPropertyToPinType(ReturnProperty, DecoderReturnType)))
		{
			continue;
		}
		const FEdGraphPinType PinType = ReturnPinType(Case.ReturnType);
		TestEqual(*FString::Printf(TEXT("%s returns the pin's category"), Case.FunctionName),
			DecoderReturnType.PinCategory.ToString(), PinType.PinCategory.ToString());
		TestEqual(*FString::Printf(TEXT("%s returns the pin's sub-category"), Case.FunctionName),
			DecoderReturnType.PinSubCategory.ToString(), PinType.PinSubCategory.ToString());
	}

	// None is never a caller: no pin exists for it, so there is no decoder to name.
	FName NoneParam = TEXT("unset");
	TestEqual(TEXT("None names no decoder"), DecoderFunctionName(ERT::None, NoneParam), FName(NAME_None));

	return true;
}

// The splice reads the raw JSON result off the apply outcome's own pin, addressed by name. That name is a delegate
// parameter, not a function parameter, and the engine's expansion silently skips an output pin it cannot match, so a
// rename there would leave the typed pin reading its type's default with nothing failing to compile. This pins the
// name to the live delegate signature, and to a string, which is what the decoder's input expects.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectReturnDelegateSignatureTest,
	"CrowdySDK.Editor.ApplyEffectReturnDelegateSignature", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectReturnDelegateSignatureTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	FMulticastDelegateProperty* Succeeded = FindFProperty<FMulticastDelegateProperty>(
		UCrowdyApplyEffectAction::StaticClass(), GET_MEMBER_NAME_CHECKED(UCrowdyApplyEffectAction, Succeeded));
	if (!TestNotNull(TEXT("the Succeeded delegate resolves"), Succeeded))
	{
		return false;
	}

	UFunction* Signature = Succeeded->SignatureFunction;
	if (!TestNotNull(TEXT("the Succeeded delegate has a signature"), Signature))
	{
		return false;
	}

	FProperty* JsonParam = Signature->FindPropertyByName(PN_ReturnValueJson);
	if (!TestNotNull(*FString::Printf(TEXT("the outcome carries a %s parameter"), *PN_ReturnValueJson.ToString()),
		JsonParam))
	{
		return false;
	}
	TestNotNull(*FString::Printf(TEXT("%s is a string"), *PN_ReturnValueJson.ToString()),
		CastField<FStrProperty>(JsonParam));

	// The synthesized typed pin must not collide with anything the delegate already exposes.
	TestNull(TEXT("the typed return pin name is not already a delegate parameter"),
		Signature->FindPropertyByName(PN_ReturnValue));

	return true;
}

// A declared return type says what the effect answers with; whether the body actually produces a value is a separate
// question the pin deliberately does not ask, because the body is edited through paths that raise no property
// notification. The compile-time check asks it instead, off the lowering, so it can never be stale. It must fire only
// for the one combination that is a real mistake: a concrete type declared with nothing returned.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectReturnPinBackingTest,
	"CrowdySDK.Editor.ApplyEffectReturnPinBacking", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectReturnPinBackingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;
	using ERT = ECrowdyEffectReturnType;

	const ERT Concrete[] = { ERT::Int, ERT::Float, ERT::Bool, ERT::String };
	for (const ERT Declared : Concrete)
	{
		TestTrue(*FString::Printf(TEXT("return type %d with no return expression is unbacked"),
			static_cast<int32>(Declared)), ReturnPinLacksBacking(Declared, FString()));
		TestFalse(*FString::Printf(TEXT("return type %d with a return expression is backed"),
			static_cast<int32>(Declared)), ReturnPinLacksBacking(Declared, TEXT("self.health")));
	}

	// None declares nothing, so there is no promise to break either way.
	TestFalse(TEXT("None with no return expression is not a mistake"),
		ReturnPinLacksBacking(ERT::None, FString()));
	TestFalse(TEXT("None with a return expression is not a mistake"),
		ReturnPinLacksBacking(ERT::None, TEXT("self.health")));

	return true;
}

// Editing the referenced effect rebuilds the node, which is expensive and interrupts the designer, so it must happen
// only for the fields the pin layout is actually built from. An edit to the effect's script, description or carrier
// leaves the pins exactly as they are and must not trigger a rebuild.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectPinPlanChangeFilterTest,
	"CrowdySDK.Editor.ApplyEffectPinPlanChangeFilter", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectPinPlanChangeFilterTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	FProperty* MagnitudesProperty = FindFProperty<FProperty>(UCrowdyEffect::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEffect, Magnitudes));
	FProperty* RequiresSourceProperty = FindFProperty<FProperty>(UCrowdyEffect::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEffect, bRequiresSource));
	FProperty* ReturnTypeProperty = FindFProperty<FProperty>(UCrowdyEffect::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEffect, ReturnType));
	FProperty* ScriptProperty = FindFProperty<FProperty>(UCrowdyEffect::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEffect, EffectScript));
	FProperty* CarrierProperty = FindFProperty<FProperty>(UCrowdyEffect::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEffect, NotificationCarrier));

	if (!TestNotNull(TEXT("Magnitudes resolves"), MagnitudesProperty)
		|| !TestNotNull(TEXT("bRequiresSource resolves"), RequiresSourceProperty)
		|| !TestNotNull(TEXT("ReturnType resolves"), ReturnTypeProperty)
		|| !TestNotNull(TEXT("EffectScript resolves"), ScriptProperty)
		|| !TestNotNull(TEXT("NotificationCarrier resolves"), CarrierProperty))
	{
		return false;
	}

	FPropertyChangedEvent MagnitudesEvent(MagnitudesProperty);
	TestTrue(TEXT("a magnitudes edit reshapes the pins"), ChangeAffectsPinPlan(MagnitudesEvent));

	FPropertyChangedEvent RequiresSourceEvent(RequiresSourceProperty);
	TestTrue(TEXT("a source-requirement edit reshapes the pins"), ChangeAffectsPinPlan(RequiresSourceEvent));

	// The declared return type decides whether the typed return pin exists at all, so an edit to it has to reshape
	// the node. Left out of the allowlist, the pin would not appear until some unrelated edit or a reload.
	FPropertyChangedEvent ReturnTypeEvent(ReturnTypeProperty);
	TestTrue(TEXT("a return-type edit reshapes the pins"), ChangeAffectsPinPlan(ReturnTypeEvent));

	FPropertyChangedEvent ScriptEvent(ScriptProperty);
	TestFalse(TEXT("a script edit leaves the pins alone"), ChangeAffectsPinPlan(ScriptEvent));

	FPropertyChangedEvent CarrierEvent(CarrierProperty);
	TestFalse(TEXT("a carrier edit leaves the pins alone"), ChangeAffectsPinPlan(CarrierEvent));

	return true;
}

// The node reports a referenced effect that will not compile, and that report has to name the cause: "does not
// compile" on its own left an author with nowhere to go, since the effect asset opens clean whenever the reason was
// a transient one (an asset read before its own package finished loading).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectFirstCompileErrorTest,
	"CrowdySDK.Editor.ApplyEffectFirstCompileError", CrowdyApplyEffectCallTestFlags)
bool FCrowdyApplyEffectFirstCompileErrorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyApplyEffectNodeShared;

	TestEqual(TEXT("no diagnostics reads back empty"), FirstCompileError({}), FString());

	// Warnings are not what failed the compile, so the first ERROR is the one to name, not the first diagnostic.
	const TArray<FCrowdyEffectDiagnostic> WarningThenError = {
		{ ECrowdyEffectSeverity::Warning, 1, 1, TEXT("declared but never used") },
		{ ECrowdyEffectSeverity::Error, 4, 9, TEXT("unknown attribute 'healht'") },
		{ ECrowdyEffectSeverity::Error, 6, 1, TEXT("division by zero") }
	};
	TestEqual(TEXT("the first error wins over an earlier warning and a later error"),
		FirstCompileError(WarningThenError), FString(TEXT("line 4: unknown attribute 'healht'")));

	// A whole-effect diagnostic carries no position (line 0), so prefixing it with "line 0" would point the author
	// at a line that does not exist.
	const TArray<FCrowdyEffectDiagnostic> Unpositioned = {
		{ ECrowdyEffectSeverity::Error, 0, 0, TEXT("the effect has no target container class set") }
	};
	TestEqual(TEXT("an unpositioned error is reported without a line"),
		FirstCompileError(Unpositioned), FString(TEXT("the effect has no target container class set")));

	const TArray<FCrowdyEffectDiagnostic> WarningsOnly = {
		{ ECrowdyEffectSeverity::Warning, 2, 1, TEXT("declared but never used") }
	};
	TestEqual(TEXT("warnings alone name nothing"), FirstCompileError(WarningsOnly), FString());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
