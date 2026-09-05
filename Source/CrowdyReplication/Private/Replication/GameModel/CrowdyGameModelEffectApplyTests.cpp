#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectApplyTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyEffectMagnitude Mag(const FString& Name, const FString& ValueType, const FString& DefaultJson)
	{
		FCrowdyEffectMagnitude M;
		M.Name = Name;
		M.ValueType = ValueType;
		M.DefaultValueJson = DefaultJson;
		return M;
	}
}

// The marshaller turns each magnitude into one param, JSON-typed by its authored default: an int/float becomes a
// number, a bool a boolean, a string a string. With no Source, no source_id key is emitted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyBuildsParamsTest,
	"CrowdySDK.GameModel.EffectApplyBuildsParams", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectApplyBuildsParamsTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes.Add(Mag(TEXT("power"), TEXT("int"), TEXT("5")));
	Effect->Magnitudes.Add(Mag(TEXT("rate"), TEXT("float"), TEXT("2.5")));
	Effect->Magnitudes.Add(Mag(TEXT("label"), TEXT("string"), TEXT("\"hi\"")));
	Effect->Magnitudes.Add(Mag(TEXT("flag"), TEXT("bool"), TEXT("true")));

	FString Error;
	const TSharedPtr<FJsonObject> Params = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, false, FString(), Error);
	if (!TestNotNull(TEXT("params built"), Params.Get()))
	{
		return false;
	}
	TestTrue(TEXT("no error"), Error.IsEmpty());

	// Values + JSON types.
	const TSharedPtr<FJsonValue> Power = Params->TryGetField(TEXT("power"));
	if (TestTrue(TEXT("power present"), Power.IsValid()))
	{
		TestEqual(TEXT("power is a number"), static_cast<int32>(Power->Type), static_cast<int32>(EJson::Number));
		TestEqual(TEXT("power value"), static_cast<int32>(Power->AsNumber()), 5);
	}
	TestEqual(TEXT("rate value"), Params->GetNumberField(TEXT("rate")), 2.5);
	const TSharedPtr<FJsonValue> Label = Params->TryGetField(TEXT("label"));
	if (TestTrue(TEXT("label present"), Label.IsValid()))
	{
		TestEqual(TEXT("label is a string"), static_cast<int32>(Label->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("label value"), Label->AsString(), FString(TEXT("hi")));
	}
	const TSharedPtr<FJsonValue> Flag = Params->TryGetField(TEXT("flag"));
	if (TestTrue(TEXT("flag present"), Flag.IsValid()))
	{
		TestEqual(TEXT("flag is a boolean"), static_cast<int32>(Flag->Type), static_cast<int32>(EJson::Boolean));
		TestTrue(TEXT("flag value"), Flag->AsBool());
	}

	TestFalse(TEXT("no source_id without a Source"), Params->HasField(TEXT("source_id")));
	return true;
}

// A Source injects source_id (a string container id); a Source with no resolved container is an error, not a
// silent omission (source_id is a required, injected param).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplySourceInjectsIdTest,
	"CrowdySDK.GameModel.EffectApplySourceInjectsId", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectApplySourceInjectsIdTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes.Add(Mag(TEXT("amount"), TEXT("int"), TEXT("3")));

	FString Error;
	const TSharedPtr<FJsonObject> WithSource =
		UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, true, TEXT("src-container"), Error);
	if (TestNotNull(TEXT("params built with source"), WithSource.Get()))
	{
		TestEqual(TEXT("source_id string"), WithSource->GetStringField(TEXT("source_id")), FString(TEXT("src-container")));
		TestEqual(TEXT("amount still present"), static_cast<int32>(WithSource->GetNumberField(TEXT("amount"))), 3);
	}

	// A Source with no bound container id fails rather than emitting an empty/absent source_id.
	const TSharedPtr<FJsonObject> NoContainer = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, true, FString(), Error);
	TestNull(TEXT("no params when Source has no container"), NoContainer.Get());
	TestFalse(TEXT("error is set"), Error.IsEmpty());
	return true;
}

// A required magnitude (empty default) with no override is rejected; an override supplies it and also replaces a
// present default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyRequiredAndOverrideTest,
	"CrowdySDK.GameModel.EffectApplyRequiredAndOverride", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectApplyRequiredAndOverrideTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes.Add(Mag(TEXT("required"), TEXT("int"), FString())); // no default -> required
	Effect->Magnitudes.Add(Mag(TEXT("power"), TEXT("int"), TEXT("5")));    // default 5

	FString Error;
	const TSharedPtr<FJsonObject> Missing = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, false, FString(), Error);
	TestNull(TEXT("required-missing rejected"), Missing.Get());
	TestFalse(TEXT("error set for required-missing"), Error.IsEmpty());

	TMap<FName, FString> Overrides;
	Overrides.Add(FName(TEXT("required")), TEXT("42"));
	Overrides.Add(FName(TEXT("power")), TEXT("9"));
	const TSharedPtr<FJsonObject> Built = UCrowdyEffects::BuildInvokeParams(Effect, Overrides, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("params built with overrides"), Built.Get()))
	{
		TestEqual(TEXT("required supplied by override"), static_cast<int32>(Built->GetNumberField(TEXT("required"))), 42);
		TestEqual(TEXT("override replaces the default"), static_cast<int32>(Built->GetNumberField(TEXT("power"))), 9);
	}
	return true;
}

// A non-JSON value is forgiven as a raw string literal, so what leaves the client is always valid JSON: a designer
// can type sword instead of "sword" for a string / container_ref magnitude. A JSON-encoded literal is still used as
// typed, and a quoted string is not double-encoded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyForgivesNonJsonTest,
	"CrowdySDK.GameModel.EffectApplyForgivesNonJson", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectApplyForgivesNonJsonTest::RunTest(const FString& Parameters)
{
	// A non-JSON default is taken as a raw string, not rejected.
	UCrowdyEffect* RawDefault = NewObject<UCrowdyEffect>(GetTransientPackage());
	RawDefault->Magnitudes.Add(Mag(TEXT("label"), TEXT("string"), TEXT("not json")));

	FString Error;
	const TSharedPtr<FJsonObject> FromDefault = UCrowdyEffects::BuildInvokeParams(RawDefault, {}, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("non-JSON default forgiven"), FromDefault.Get()))
	{
		const TSharedPtr<FJsonValue> Label = FromDefault->TryGetField(TEXT("label"));
		if (TestTrue(TEXT("label present"), Label.IsValid()))
		{
			TestEqual(TEXT("label is a string"), static_cast<int32>(Label->Type), static_cast<int32>(EJson::String));
			TestEqual(TEXT("label raw value"), Label->AsString(), FString(TEXT("not json")));
		}
	}
	TestTrue(TEXT("no error for forgiven default"), Error.IsEmpty());

	// A raw (unquoted) container id override becomes a JSON string; the JSON-encoded form is idempotent (not
	// double-encoded).
	UCrowdyEffect* Ref = NewObject<UCrowdyEffect>(GetTransientPackage());
	Ref->Magnitudes.Add(Mag(TEXT("target"), TEXT("container_ref"), FString()));
	TMap<FName, FString> RawId;
	RawId.Add(FName(TEXT("target")), TEXT("enemy-uuid"));
	const TSharedPtr<FJsonObject> FromRaw = UCrowdyEffects::BuildInvokeParams(Ref, RawId, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("raw container id forgiven"), FromRaw.Get()))
	{
		TestEqual(TEXT("raw id is a string"), FromRaw->GetStringField(TEXT("target")), FString(TEXT("enemy-uuid")));
	}

	TMap<FName, FString> QuotedId;
	QuotedId.Add(FName(TEXT("target")), TEXT("\"enemy-uuid\""));
	const TSharedPtr<FJsonObject> FromQuoted = UCrowdyEffects::BuildInvokeParams(Ref, QuotedId, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("quoted container id accepted"), FromQuoted.Get()))
	{
		TestEqual(TEXT("quoted id is the bare id"), FromQuoted->GetStringField(TEXT("target")), FString(TEXT("enemy-uuid")));
	}

	// A JSON-encoded number override stays a number; forgiveness only kicks in when strict JSON fails.
	UCrowdyEffect* Num = NewObject<UCrowdyEffect>(GetTransientPackage());
	Num->Magnitudes.Add(Mag(TEXT("power"), TEXT("int"), TEXT("5")));
	TMap<FName, FString> NumOverride;
	NumOverride.Add(FName(TEXT("power")), TEXT("9"));
	const TSharedPtr<FJsonObject> NumParams = UCrowdyEffects::BuildInvokeParams(Num, NumOverride, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("number override built"), NumParams.Get()))
	{
		const TSharedPtr<FJsonValue> Power = NumParams->TryGetField(TEXT("power"));
		if (TestTrue(TEXT("power present"), Power.IsValid()))
		{
			TestEqual(TEXT("power stays a number"), static_cast<int32>(Power->Type), static_cast<int32>(EJson::Number));
			TestEqual(TEXT("power value"), static_cast<int32>(Power->AsNumber()), 9);
		}
	}

	// Type-aware strictness: a NON-JSON value on a numeric magnitude is a typo, rejected client-side rather than
	// shipped as a wrong-typed string (its sibling string magnitude would forgive the same text).
	UCrowdyEffect* BadNum = NewObject<UCrowdyEffect>(GetTransientPackage());
	BadNum->Magnitudes.Add(Mag(TEXT("power"), TEXT("int"), FString()));
	TMap<FName, FString> BadNumOverride;
	BadNumOverride.Add(FName(TEXT("power")), TEXT("5x"));
	TestNull(TEXT("non-JSON numeric rejected"),
		UCrowdyEffects::BuildInvokeParams(BadNum, BadNumOverride, 1.0f, false, FString(), Error).Get());
	TestFalse(TEXT("error set for non-JSON numeric"), Error.IsEmpty());

	// An all-digit container_ref id parses as a valid JSON number but must be sent as a STRING, not a number.
	UCrowdyEffect* NumericRef = NewObject<UCrowdyEffect>(GetTransientPackage());
	NumericRef->Magnitudes.Add(Mag(TEXT("target"), TEXT("container_ref"), FString()));
	TMap<FName, FString> DigitId;
	DigitId.Add(FName(TEXT("target")), TEXT("12345"));
	const TSharedPtr<FJsonObject> FromDigits = UCrowdyEffects::BuildInvokeParams(NumericRef, DigitId, 1.0f, false, FString(), Error);
	if (TestNotNull(TEXT("all-digit ref built"), FromDigits.Get()))
	{
		const TSharedPtr<FJsonValue> Target = FromDigits->TryGetField(TEXT("target"));
		if (TestTrue(TEXT("target present"), Target.IsValid()))
		{
			TestEqual(TEXT("all-digit ref is a string"), static_cast<int32>(Target->Type), static_cast<int32>(EJson::String));
			TestEqual(TEXT("all-digit ref value"), Target->AsString(), FString(TEXT("12345")));
		}
	}
	return true;
}

// The persisted bRequiresSource flag makes the marshaller reject a missing Source client-side (a clear error)
// instead of dispatching an invoke the server would fail on the absent required source_id param. A false flag (the
// pre-guardrail / not-yet-resaved state) skips the check, so a self-only effect still applies with no Source.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyRequiresSourceTest,
	"CrowdySDK.GameModel.EffectApplyRequiresSource", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectApplyRequiresSourceTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->Magnitudes.Add(Mag(TEXT("amount"), TEXT("int"), TEXT("3")));
	Effect->bRequiresSource = true;

	FString Error;
	const TSharedPtr<FJsonObject> Missing = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, false, FString(), Error);
	TestNull(TEXT("missing source rejected"), Missing.Get());
	TestFalse(TEXT("error set for missing source"), Error.IsEmpty());

	const TSharedPtr<FJsonObject> WithSource = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, true, TEXT("src"), Error);
	if (TestNotNull(TEXT("built with source"), WithSource.Get()))
	{
		TestEqual(TEXT("source_id present"), WithSource->GetStringField(TEXT("source_id")), FString(TEXT("src")));
	}

	Effect->bRequiresSource = false;
	const TSharedPtr<FJsonObject> NoGuard = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.0f, false, FString(), Error);
	TestNotNull(TEXT("no guardrail when flag false"), NoGuard.Get());
	return true;
}

// A magnitude bound to a curve is sampled at the Level input in place of its default: an int rounds the sample, a
// float keeps it, a different Level samples a different value, and an explicit Override still beats the curve. A
// curve on a non-numeric magnitude is rejected by the marshaller.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyMagnitudeCurveSampleTest,
	"CrowdySDK.GameModel.EffectApplyMagnitudeCurveSample", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectApplyMagnitudeCurveSampleTest::RunTest(const FString& Parameters)
{
	// Linear curves so a mid-segment sample is exact. power: level 1->10, 3->30 (slope 10). rate: level 1->2.5, 3->7.5.
	UCurveFloat* PowerCurve = NewObject<UCurveFloat>(GetTransientPackage());
	PowerCurve->FloatCurve.SetKeyInterpMode(PowerCurve->FloatCurve.AddKey(1.0f, 10.0f), RCIM_Linear);
	PowerCurve->FloatCurve.SetKeyInterpMode(PowerCurve->FloatCurve.AddKey(3.0f, 30.0f), RCIM_Linear);

	UCurveFloat* RateCurve = NewObject<UCurveFloat>(GetTransientPackage());
	RateCurve->FloatCurve.SetKeyInterpMode(RateCurve->FloatCurve.AddKey(1.0f, 2.5f), RCIM_Linear);
	RateCurve->FloatCurve.SetKeyInterpMode(RateCurve->FloatCurve.AddKey(3.0f, 7.5f), RCIM_Linear);

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	FCrowdyEffectMagnitude Power = Mag(TEXT("power"), TEXT("int"), FString()); // no default: the curve supplies it
	Power.Curve = PowerCurve;
	Effect->Magnitudes.Add(Power);
	FCrowdyEffectMagnitude Rate = Mag(TEXT("rate"), TEXT("float"), FString());
	Rate.Curve = RateCurve;
	Effect->Magnitudes.Add(Rate);

	FString Error;

	// Level 2: power 20 (an integer JSON number), rate 5.0 (a float). The empty defaults did not make them required.
	const TSharedPtr<FJsonObject> AtTwo = UCrowdyEffects::BuildInvokeParams(Effect, {}, 2.0f, false, FString(), Error);
	if (TestNotNull(TEXT("curve-sampled params built at level 2"), AtTwo.Get()))
	{
		const TSharedPtr<FJsonValue> PowerVal = AtTwo->TryGetField(TEXT("power"));
		if (TestTrue(TEXT("power present"), PowerVal.IsValid()))
		{
			TestEqual(TEXT("power is a number"), static_cast<int32>(PowerVal->Type), static_cast<int32>(EJson::Number));
			TestEqual(TEXT("power sampled at level 2"), static_cast<int32>(PowerVal->AsNumber()), 20);
		}
		TestEqual(TEXT("rate sampled at level 2"), AtTwo->GetNumberField(TEXT("rate")), 5.0);
	}
	TestTrue(TEXT("no error at level 2"), Error.IsEmpty());

	// A different Level samples a different value; a fractional sample rounds for an int (level 1.25 -> 12.5 -> 13).
	const TSharedPtr<FJsonObject> AtQuarter = UCrowdyEffects::BuildInvokeParams(Effect, {}, 1.25f, false, FString(), Error);
	if (TestNotNull(TEXT("params built at level 1.25"), AtQuarter.Get()))
	{
		TestEqual(TEXT("int rounds the sample"), static_cast<int32>(AtQuarter->GetNumberField(TEXT("power"))), 13);
	}

	// An explicit Override wins over the curve.
	TMap<FName, FString> Overrides;
	Overrides.Add(FName(TEXT("power")), TEXT("99"));
	const TSharedPtr<FJsonObject> Overridden =
		UCrowdyEffects::BuildInvokeParams(Effect, Overrides, 2.0f, false, FString(), Error);
	if (TestNotNull(TEXT("override params built"), Overridden.Get()))
	{
		TestEqual(TEXT("override beats the curve"), static_cast<int32>(Overridden->GetNumberField(TEXT("power"))), 99);
	}

	// A curve on a non-numeric magnitude is rejected (the author-time IsDataValid check mirrors this).
	UCrowdyEffect* BadType = NewObject<UCrowdyEffect>(GetTransientPackage());
	FCrowdyEffectMagnitude Label = Mag(TEXT("label"), TEXT("string"), FString());
	Label.Curve = PowerCurve;
	BadType->Magnitudes.Add(Label);
	TestNull(TEXT("curve on non-numeric rejected"),
		UCrowdyEffects::BuildInvokeParams(BadType, {}, 1.0f, false, FString(), Error).Get());
	TestFalse(TEXT("error set for curve on non-numeric"), Error.IsEmpty());

	// An int magnitude whose curve sample is outside the int range is rejected, not silently wrapped to a garbage
	// int; the same large value is a valid JSON number for a float magnitude.
	UCurveFloat* HugeCurve = NewObject<UCurveFloat>(GetTransientPackage());
	HugeCurve->FloatCurve.AddKey(0.0f, 5.0e9f); // one key: reads 5e9 at any level, well past MAX_int32
	UCrowdyEffect* HugeInt = NewObject<UCrowdyEffect>(GetTransientPackage());
	FCrowdyEffectMagnitude Count = Mag(TEXT("count"), TEXT("int"), FString());
	Count.Curve = HugeCurve;
	HugeInt->Magnitudes.Add(Count);
	TestNull(TEXT("out-of-int-range curve sample rejected"),
		UCrowdyEffects::BuildInvokeParams(HugeInt, {}, 0.0f, false, FString(), Error).Get());
	TestFalse(TEXT("error set for out-of-range int curve"), Error.IsEmpty());

	UCrowdyEffect* HugeFloat = NewObject<UCrowdyEffect>(GetTransientPackage());
	FCrowdyEffectMagnitude BigRate = Mag(TEXT("bigrate"), TEXT("float"), FString());
	BigRate.Curve = HugeCurve;
	HugeFloat->Magnitudes.Add(BigRate);
	const TSharedPtr<FJsonObject> BigOk = UCrowdyEffects::BuildInvokeParams(HugeFloat, {}, 0.0f, false, FString(), Error);
	TestNotNull(TEXT("large float curve sample accepted"), BigOk.Get());
	return true;
}

// The pure carrier resolution. An effect-level Default defers to the project carrier; a project-level
// Default (its own field default, or a stale value) falls back to Channel; a concrete effect value always wins.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCarrierResolvesTest,
	"CrowdySDK.GameModel.EffectCarrierResolves", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectCarrierResolvesTest::RunTest(const FString& Parameters)
{
	using EAsset = ECrowdyEffectNotificationCarrier;
	using EModel = ECrowdyModelNotificationCarrier;
	auto Resolve = [](EAsset Effect, EAsset Project) { return static_cast<int32>(UCrowdyEffect::ResolveCarrier(Effect, Project)); };
	const int32 Channel = static_cast<int32>(EModel::Channel);
	const int32 Spatial = static_cast<int32>(EModel::Spatial);
	const int32 None    = static_cast<int32>(EModel::None);

	// Effect Default defers to the project carrier.
	TestEqual(TEXT("Default+Channel -> Channel"), Resolve(EAsset::Default, EAsset::Channel), Channel);
	TestEqual(TEXT("Default+Spatial -> Spatial"), Resolve(EAsset::Default, EAsset::Spatial), Spatial);
	TestEqual(TEXT("Default+None -> None"), Resolve(EAsset::Default, EAsset::None), None);
	// A project-level Default resolves to Channel, the SDK's position-independent default.
	TestEqual(TEXT("Default+Default -> Channel"), Resolve(EAsset::Default, EAsset::Default), Channel);
	// A concrete effect-level value always wins over the project.
	TestEqual(TEXT("None+Channel -> None (effect wins)"), Resolve(EAsset::None, EAsset::Channel), None);
	TestEqual(TEXT("Spatial+None -> Spatial (effect wins)"), Resolve(EAsset::Spatial, EAsset::None), Spatial);
	TestEqual(TEXT("Channel+Spatial -> Channel (effect wins)"), Resolve(EAsset::Channel, EAsset::Spatial), Channel);
	return true;
}

// Compile() resolves the carrier off the asset + project default and feeds it to the lowering, so a
// default effect authors a channel notification (naming the container via $self_container_id, with no function
// param), and an explicit None suppresses it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCompileAuthorsNotificationTest,
	"CrowdySDK.GameModel.EffectCompileAuthorsNotification", CrowdyEffectApplyTestFlags)
bool FCrowdyEffectCompileAuthorsNotificationTest::RunTest(const FString& Parameters)
{
	// Compile() runs DiscoverForClass over the shared discovery fixture, which carries deliberately-invalid sibling
	// properties (a dual-plane field, a bad-arity notify) so its reject paths are exercised. Those emit error logs
	// that would fail this test; whitelist them (this test compiles twice, so allow one-or-more via Occurrences 0).
	AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, 0);

	// Pin the project default so this test is independent of any DefaultGame.ini override; restore after.
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const ECrowdyEffectNotificationCarrier Saved = Settings->DefaultModelNotificationCarrier;
	Settings->DefaultModelNotificationCarrier = ECrowdyEffectNotificationCarrier::Channel;
	ON_SCOPE_EXIT { Settings->DefaultModelNotificationCarrier = Saved; };

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelDiscoveryTarget::StaticClass();
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->EffectScript = TEXT("self.health -= 10");
	// NotificationCarrier defaults to Default -> resolves to the project default (Channel).

	auto HasParam = [](const FCrowdyGameModelFunctionInput& Fn, const TCHAR* Name)
	{
		return Fn.Parameters.ContainsByPredicate(
			[Name](const FCrowdyGameModelFunctionParam& P) { return P.Name == Name; });
	};

	const FCrowdyEffectLoweringResult Defaulted = Effect->Compile();
	TestFalse(TEXT("compiles without errors (default->channel)"), Defaulted.HasErrors());
	// The notification names the container via the server-injected $self_container_id, so no param is authored.
	TestFalse(TEXT("no notify_id authored by Compile"),
		HasParam(Defaulted.Function, CrowdyGameModelMetaKeys::NotifyIdParam));
	TestFalse(TEXT("no self_container_id authored as a param"),
		HasParam(Defaulted.Function, CrowdyGameModelMetaKeys::SelfContainerIdParam));
	if (TestEqual(TEXT("one notification authored"), Defaulted.Function.Notifications.Num(), 1))
	{
		TestEqual(TEXT("channel carrier"), Defaulted.Function.Notifications[0].Kind, FString(TEXT("channel")));
	}

	// An explicit None override wins over the project default and suppresses the notification.
	Effect->NotificationCarrier = ECrowdyEffectNotificationCarrier::None;
	const FCrowdyEffectLoweringResult NoneResult = Effect->Compile();
	TestFalse(TEXT("compiles without errors (none)"), NoneResult.HasErrors());
	TestEqual(TEXT("no notification when None"), NoneResult.Function.Notifications.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
