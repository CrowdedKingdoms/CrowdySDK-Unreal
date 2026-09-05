// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Curves/CurveFloat.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Knot.h"
#include "Nodes/CrowdyApplyEffectNodePins.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyApplyEffectPinTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FCrowdyEffectMagnitude Mag(const FString& Name, ECrowdyEffectValueType Type, const FString& DefaultJson,
		UCurveFloat* Curve = nullptr)
	{
		FCrowdyEffectMagnitude M;
		M.Name = Name;
		M.ValueTypeEnum = Type;
		M.ValueType = UCrowdyEffect::ValueTypeToWireString(Type);
		M.DefaultValueJson = DefaultJson;
		M.Curve = Curve;
		return M;
	}
}

// BuildPinPlan turns an effect's magnitudes into typed pin entries: one per magnitude with the matching value type
// and stable prefixed name, the raw default prefilled (a string's quotes stripped, a container_ref carrying none),
// a required flag when the magnitude has no default and no curve, plus the Level flag (any curve magnitude) and the
// Source flag (the effect reads a Source).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectPinPlanTest,
	"CrowdySDK.Editor.ApplyEffectPinPlan", CrowdyApplyEffectPinTestFlags)
bool FCrowdyApplyEffectPinPlanTest::RunTest(const FString& Parameters)
{
	using EVT = ECrowdyEffectValueType;

	// A null effect degrades to an empty plan (the node keeps the plain async pins).
	{
		const FCrowdyApplyEffectPinPlan Empty = CrowdyApplyEffectNodePins::BuildPinPlan(nullptr);
		TestEqual(TEXT("null effect -> no magnitudes"), Empty.Magnitudes.Num(), 0);
		TestFalse(TEXT("null effect -> no Level"), Empty.bIncludeLevel);
		TestFalse(TEXT("null effect -> no Source"), Empty.bIncludeSource);
	}

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	UCurveFloat* Curve = NewObject<UCurveFloat>(GetTransientPackage());

	Effect->Magnitudes.Add(Mag(TEXT("power"), EVT::Int, TEXT("5")));
	Effect->Magnitudes.Add(Mag(TEXT("rate"), EVT::Float, TEXT("2.5")));
	Effect->Magnitudes.Add(Mag(TEXT("flag"), EVT::Bool, TEXT("true")));
	Effect->Magnitudes.Add(Mag(TEXT("label"), EVT::String, TEXT("\"hi\"")));
	Effect->Magnitudes.Add(Mag(TEXT("target_ref"), EVT::ContainerRef, TEXT("")));
	Effect->Magnitudes.Add(Mag(TEXT("required_amount"), EVT::Int, TEXT("")));
	Effect->Magnitudes.Add(Mag(TEXT("scaled"), EVT::Float, TEXT(""), Curve));

	Effect->bRequiresSource = true;

	const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);

	if (!TestEqual(TEXT("one entry per magnitude"), Plan.Magnitudes.Num(), 7))
	{
		return false;
	}

	// Level is included because "scaled" is curve-driven; Source because the effect reads a Source.
	TestTrue(TEXT("Level included (a curve magnitude exists)"), Plan.bIncludeLevel);
	TestTrue(TEXT("Source included (bRequiresSource)"), Plan.bIncludeSource);

	auto Find = [&Plan](const FString& Name) -> const FCrowdyApplyEffectPinEntry*
	{
		return Plan.Magnitudes.FindByPredicate(
			[&Name](const FCrowdyApplyEffectPinEntry& E) { return E.MagnitudeName == Name; });
	};

	if (const FCrowdyApplyEffectPinEntry* Power = Find(TEXT("power")))
	{
		TestEqual(TEXT("power is int"), static_cast<int32>(Power->ValueType), static_cast<int32>(EVT::Int));
		TestEqual(TEXT("power default prefilled"), Power->DefaultValue, FString(TEXT("5")));
		TestFalse(TEXT("power not required"), Power->bRequired);
		TestEqual(TEXT("power pin name is prefixed"), Power->PinName,
			CrowdyApplyEffectNodePins::MagnitudePinName(TEXT("power")));
		TestTrue(TEXT("power pin name recognised"),
			CrowdyApplyEffectNodePins::IsMagnitudePinName(Power->PinName));
	}
	else
	{
		AddError(TEXT("power entry missing"));
	}

	if (const FCrowdyApplyEffectPinEntry* Rate = Find(TEXT("rate")))
	{
		TestEqual(TEXT("rate is float"), static_cast<int32>(Rate->ValueType), static_cast<int32>(EVT::Float));
		TestEqual(TEXT("rate default prefilled"), Rate->DefaultValue, FString(TEXT("2.5")));
	}

	if (const FCrowdyApplyEffectPinEntry* Flag = Find(TEXT("flag")))
	{
		TestEqual(TEXT("flag is bool"), static_cast<int32>(Flag->ValueType), static_cast<int32>(EVT::Bool));
		TestFalse(TEXT("bool never required"), Flag->bRequired);
	}

	if (const FCrowdyApplyEffectPinEntry* Label = Find(TEXT("label")))
	{
		TestEqual(TEXT("label is string"), static_cast<int32>(Label->ValueType), static_cast<int32>(EVT::String));
		// The JSON-quoted default is shown unquoted on the pin.
		TestEqual(TEXT("label default unquoted"), Label->DefaultValue, FString(TEXT("hi")));
	}

	if (const FCrowdyApplyEffectPinEntry* Ref = Find(TEXT("target_ref")))
	{
		TestEqual(TEXT("target_ref is container_ref"),
			static_cast<int32>(Ref->ValueType), static_cast<int32>(EVT::ContainerRef));
		TestTrue(TEXT("target_ref reports as object"), Ref->IsContainerRef());
		TestTrue(TEXT("container_ref carries no string default"), Ref->DefaultValue.IsEmpty());
	}

	if (const FCrowdyApplyEffectPinEntry* Req = Find(TEXT("required_amount")))
	{
		// No default and no curve -> the designer must supply it.
		TestTrue(TEXT("empty-default numeric magnitude is required"), Req->bRequired);
	}

	if (const FCrowdyApplyEffectPinEntry* Scaled = Find(TEXT("scaled")))
	{
		// A curve always yields a value, so a curve magnitude is never required.
		TestFalse(TEXT("curve magnitude not required"), Scaled->bRequired);
	}

	// The inclusion rule: an untouched pin (unconnected, still at its prefill) is omitted; a wired or changed pin
	// contributes an override.
	TestFalse(TEXT("untouched pin omitted"),
		CrowdyApplyEffectNodePins::ShouldIncludeOverride(false, false, nullptr, TEXT("5"), TEXT("5")));
	TestTrue(TEXT("connected pin included"),
		CrowdyApplyEffectNodePins::ShouldIncludeOverride(true, false, nullptr, TEXT("5"), TEXT("5")));
	TestTrue(TEXT("changed pin included"),
		CrowdyApplyEffectNodePins::ShouldIncludeOverride(false, false, nullptr, TEXT("7"), TEXT("5")));

	// A container_ref magnitude has no string default: DefaultValue and AutogeneratedDefaultValue are both empty
	// even when the designer set an object literal, so the container-ref arm must key off the object instead.
	UCrowdyEffect* ObjectLiteral = Effect;
	TestFalse(TEXT("untouched container_ref pin omitted"),
		CrowdyApplyEffectNodePins::ShouldIncludeOverride(false, true, nullptr, FString(), FString()));
	TestTrue(TEXT("connected container_ref pin included"),
		CrowdyApplyEffectNodePins::ShouldIncludeOverride(true, true, nullptr, FString(), FString()));
	TestTrue(TEXT("container_ref pin with a literal object included"),
		CrowdyApplyEffectNodePins::ShouldIncludeOverride(false, true, ObjectLiteral, FString(), FString()));

	return true;
}

// Two magnitudes sharing a name would otherwise emit two identically-named pins that collapse to one Overrides
// key, silently dropping the second value. BuildPinPlan keeps a single entry per name and reports the duplicate
// so the node can fail compilation with a clear message instead of losing a magnitude at runtime.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectPinPlanDuplicateTest,
	"CrowdySDK.Editor.ApplyEffectPinPlanDuplicate", CrowdyApplyEffectPinTestFlags)
bool FCrowdyApplyEffectPinPlanDuplicateTest::RunTest(const FString& Parameters)
{
	using EVT = ECrowdyEffectValueType;

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes.Add(Mag(TEXT("power"), EVT::Int, TEXT("5")));
	Effect->Magnitudes.Add(Mag(TEXT("power"), EVT::Float, TEXT("2.5")));
	Effect->Magnitudes.Add(Mag(TEXT("rate"), EVT::Float, TEXT("1.0")));

	const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);

	int32 PowerEntries = 0;
	for (const FCrowdyApplyEffectPinEntry& Entry : Plan.Magnitudes)
	{
		if (Entry.MagnitudeName == TEXT("power"))
		{
			++PowerEntries;
		}
	}
	TestEqual(TEXT("one pin per name despite the duplicate"), PowerEntries, 1);
	TestEqual(TEXT("three magnitudes collapse to two entries"), Plan.Magnitudes.Num(), 2);
	TestTrue(TEXT("the duplicated name is reported"), Plan.DuplicateMagnitudeNames.Contains(TEXT("power")));
	TestEqual(TEXT("the duplicate is reported once"), Plan.DuplicateMagnitudeNames.Num(), 1);

	return true;
}

// The type an effect declares it answers with is a fact about the whole effect, like whether it reads a Source, not
// a magnitude: it decides whether the latent node grows one typed output pin, and it must never turn into an extra
// input pin. HasReturnPin keys off the declared type alone, so a plan built from an effect that declares nothing has
// no return pin and every concrete declaration has one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectReturnPinPlanTest,
	"CrowdySDK.Editor.ApplyEffectReturnPinPlan", CrowdyApplyEffectPinTestFlags)
bool FCrowdyApplyEffectReturnPinPlanTest::RunTest(const FString& Parameters)
{
	using ERT = ECrowdyEffectReturnType;
	using EVT = ECrowdyEffectValueType;

	// A null effect declares nothing, so the node keeps the plain async pin set with no typed result.
	const FCrowdyApplyEffectPinPlan Empty = CrowdyApplyEffectNodePins::BuildPinPlan(nullptr);
	TestEqual(TEXT("null effect declares no return type"),
		static_cast<int32>(Empty.ReturnType), static_cast<int32>(ERT::None));
	TestFalse(TEXT("null effect gets no return pin"), Empty.HasReturnPin());

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes.Add(Mag(TEXT("power"), EVT::Int, TEXT("5")));

	Effect->ReturnType = ERT::None;
	{
		const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);
		TestEqual(TEXT("None is carried onto the plan"),
			static_cast<int32>(Plan.ReturnType), static_cast<int32>(ERT::None));
		TestFalse(TEXT("None gets no return pin"), Plan.HasReturnPin());
	}

	const ERT Concrete[] = { ERT::Int, ERT::Float, ERT::Bool, ERT::String };
	for (const ERT Declared : Concrete)
	{
		Effect->ReturnType = Declared;
		const FCrowdyApplyEffectPinPlan Plan = CrowdyApplyEffectNodePins::BuildPinPlan(Effect);

		TestEqual(*FString::Printf(TEXT("return type %d is carried onto the plan"), static_cast<int32>(Declared)),
			static_cast<int32>(Plan.ReturnType), static_cast<int32>(Declared));
		TestTrue(*FString::Printf(TEXT("return type %d gets a return pin"), static_cast<int32>(Declared)),
			Plan.HasReturnPin());

		// The declared return is a whole-effect flag: it must not add an input pin alongside the magnitudes.
		TestEqual(*FString::Printf(TEXT("return type %d adds no magnitude entry"), static_cast<int32>(Declared)),
			Plan.Magnitudes.Num(), 1);
	}

	return true;
}

// A required magnitude (no authored default) once prefilled its pin with an empty string. That is not a valid
// default for a numeric pin: the editor's numeric field discards any value typed against that invalid baseline, so
// a required float read back empty at compile and its "requires a value" error never cleared no matter what the
// designer entered. ApplyMagnitudeDefaultToPin (the single code path SynchronizeEffectPins uses) must instead give a
// required numeric pin a valid, non-empty default that a supplied value overrides, and the required check keys off
// the pin's own change-detection. This drives real pins to guard both halves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectRequiredFloatPinTest,
	"CrowdySDK.Editor.ApplyEffectRequiredFloatPin", CrowdyApplyEffectPinTestFlags)
bool FCrowdyApplyEffectRequiredFloatPinTest::RunTest(const FString& Parameters)
{
	using EVT = ECrowdyEffectValueType;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	// A lightweight owner just to host real pins; the pin behaviour, not the node, is under test.
	UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
	Graph->Schema = UEdGraphSchema_K2::StaticClass();
	UK2Node_Knot* Owner = NewObject<UK2Node_Knot>(Graph);

	auto MakeRealPin = [&](const FName PinName) -> UEdGraphPin*
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Real;
		Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return Owner->CreatePin(EGPD_Input, Type, PinName);
	};

	// Required float magnitude (empty authored default).
	{
		FCrowdyApplyEffectPinEntry Entry;
		Entry.MagnitudeName = TEXT("Damage");
		Entry.PinName = CrowdyApplyEffectNodePins::MagnitudePinName(Entry.MagnitudeName);
		Entry.ValueType = EVT::Float;
		Entry.DefaultValue = FString();
		Entry.bRequired = true;

		UEdGraphPin* Pin = MakeRealPin(Entry.PinName);
		CrowdyApplyEffectNodePins::ApplyMagnitudeDefaultToPin(Pin, Entry, Schema);

		// Root-cause guard: a required float pin must carry a valid non-empty default, not the invalid empty string.
		TestFalse(TEXT("required float pin has a valid non-empty default"), Pin->DefaultValue.IsEmpty());
		TestTrue(TEXT("an untouched required pin reads as its autogenerated default"),
			Pin->DoesDefaultValueMatchAutogenerated());

		// A supplied value is recognised as a change - exactly what the required check keys off. (Set the default
		// directly rather than through the schema, whose notify path assumes the node lives in a real Blueprint.)
		Pin->DefaultValue = TEXT("10");
		TestFalse(TEXT("a supplied value no longer matches the autogenerated default"),
			Pin->DoesDefaultValueMatchAutogenerated());
	}

	// A magnitude with an authored default keeps that value as both the current and autogenerated default, so an
	// untouched pin still reads as unchanged (the server default stays authoritative).
	{
		FCrowdyApplyEffectPinEntry Entry;
		Entry.MagnitudeName = TEXT("rate");
		Entry.PinName = CrowdyApplyEffectNodePins::MagnitudePinName(Entry.MagnitudeName);
		Entry.ValueType = EVT::Float;
		Entry.DefaultValue = TEXT("2.5");
		Entry.bRequired = false;

		UEdGraphPin* Pin = MakeRealPin(Entry.PinName);
		CrowdyApplyEffectNodePins::ApplyMagnitudeDefaultToPin(Pin, Entry, Schema);

		TestEqual(TEXT("authored default is prefilled"), Pin->DefaultValue, FString(TEXT("2.5")));
		TestTrue(TEXT("an untouched authored-default pin reads as unchanged"),
			Pin->DoesDefaultValueMatchAutogenerated());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
