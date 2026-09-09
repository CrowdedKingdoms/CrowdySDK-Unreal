// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyCanonicalNumber.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"
#include "Replication/GameModel/Effect/CrowdyEffectGraphCompileHook.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

namespace
{
	constexpr EAutomationTestFlags CrowdySurfaceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Installs a graph compiler for the length of a scope and puts back whatever was there. The editor module
	// registers the real one at startup, so a test that simply cleared it would leave every graph effect in the
	// session uncompilable.
	struct FScopedGraphCompiler
	{
		explicit FScopedGraphCompiler(CrowdyEffectGraphCompile::FCompileHook Hook)
			: Previous(CrowdyEffectGraphCompile::GetCompileHook())
		{
			CrowdyEffectGraphCompile::SetCompileHook(MoveTemp(Hook));
		}

		~FScopedGraphCompiler()
		{
			CrowdyEffectGraphCompile::SetCompileHook(MoveTemp(Previous));
		}

		CrowdyEffectGraphCompile::FCompileHook Previous;
	};

	FCrowdyAttributeDef MakeAttr(const TCHAR* Name, const TCHAR* Key, const TCHAR* ValueType,
		bool bHasClamp = false, double ClampMin = 0.0, double ClampMax = 0.0)
	{
		FCrowdyAttributeDef Def;
		Def.PropertyName = FName(Name);
		Def.Key = Key;
		Def.ValueType = ValueType;
		Def.bHasClamp = bHasClamp;
		Def.ClampMin = ClampMin;
		Def.ClampMax = ClampMax;
		return Def;
	}

	// A Hero target and a Minion source, as a caller that already knows the project's container types would hand
	// them over. No class is named anywhere, which is the point: this is the shape a compile has to accept.
	TArray<FCrowdyEffectVocabularyType> MakeProjectTypes()
	{
		FCrowdyEffectVocabularyType Hero;
		Hero.TypeName = TEXT("Hero");
		Hero.Attributes.Add(MakeAttr(TEXT("Hp"), TEXT("hp"), TEXT("int"), true, 0.0, 100.0));
		Hero.Attributes.Add(MakeAttr(TEXT("Mana"), TEXT("mana"), TEXT("int")));

		FCrowdyEffectVocabularyType Minion;
		Minion.TypeName = TEXT("Minion");
		Minion.Attributes.Add(MakeAttr(TEXT("Power"), TEXT("power"), TEXT("int")));

		return { Hero, Minion };
	}

	FCrowdyEffectOperand AttrOperand(ECrowdyEffectRole Role, const TCHAR* Name)
	{
		FCrowdyEffectOperand Operand;
		Operand.Kind = ECrowdyEffectOperandKind::Attribute;
		Operand.Role = Role;
		Operand.Name = Name;
		return Operand;
	}

	// A surface with something in every list, so a serializer that quietly drops one is caught rather than passing
	// on the strength of the fields that happened to be filled in.
	FCrowdyEffectAuthoredSurface MakePopulatedTextSurface()
	{
		FCrowdyEffectAuthoredSurface Surface;
		Surface.EffectiveFunctionName = TEXT("strike");
		Surface.Description = TEXT("a hit that costs mana");
		Surface.ContainerClassPath = TEXT("/Game/Model/BP_Hero.BP_Hero_C");
		Surface.SourceContainerType = TEXT("Minion");
		Surface.Source = ECrowdyEffectSource::Text;
		Surface.ScriptText = TEXT("self.hp -= source.power\nself.mana -= $cost");

		Surface.Magnitudes.Add({ TEXT("cost"), TEXT("int"), TEXT("5"), TEXT("mana spent") });
		Surface.Magnitudes.Add({ TEXT("label"), TEXT("string"), TEXT("\"hit\""), FString() });

		FCrowdyEffectSignal Signal;
		Signal.Name = TEXT("BossWave");
		Signal.Description = TEXT("play the roar");
		Surface.Signals.Add(Signal);

		FCrowdyEffectTimer Timer;
		Timer.FunctionName = TEXT("strike");
		Timer.DelayMs = 250;
		Timer.DedupeKey = TEXT("strike-again");
		Timer.Target = TEXT("self");
		Timer.DelayExpression = TEXT("self.mana");
		Timer.DedupeKeyExpression = TEXT("\"k\"");

		FCrowdyEffectTimerParam TimerParam;
		TimerParam.Name = TEXT("snapshot");
		TimerParam.Expression = TEXT("self.hp");
		Timer.Params.Add(TimerParam);
		Surface.Timers.Add(Timer);

		Surface.ReturnType = TEXT("int");
		Surface.InvokeScope = TEXT("server");
		Surface.NotificationCarrier = ECrowdyEffectNotificationCarrier::Spatial;

		Surface.Automation.bRunAutomatically = true;
		Surface.Automation.bEnabled = false;
		Surface.Automation.Trigger = ECrowdyEffectAutomationTrigger::OnPropertyChange;
		Surface.Automation.IntervalMs = 777;
		Surface.Automation.CronExpr = TEXT("* * * * *");
		Surface.Automation.ChangePropertyKey = TEXT("hp");
		Surface.Automation.WriteSource = ECrowdyEffectPropertyWriteSource::Function;
		Surface.Automation.WatchFunctionName = TEXT("other");
		Surface.Automation.ChangeContainerType = TEXT("Minion");
		Surface.Automation.DebounceMs = 40;
		Surface.Automation.TargetMode = ECrowdyEffectAutomationTargetMode::Container;
		Surface.Automation.TargetTypeOverride = TEXT("Hero");
		Surface.Automation.TargetContainerId = TEXT("abc");
		Surface.Automation.AutomationName = TEXT("strike_auto");
		Surface.Automation.MaxTargets = 11;
		Surface.Automation.GasLimit = 12;
		Surface.Automation.RunTimeoutMs = 13;
		Surface.Automation.MaxRunsPerMinute = 14;
		Surface.Automation.FailureThreshold = 15;
		Surface.Automation.CooldownMs = 16;
		return Surface;
	}

	// A graph spec exercising the nested shapes: an assignment with two terms, a keyword require, a comparison
	// require, and a return.
	FCrowdyEffectSpec MakeGraphSpec()
	{
		FCrowdyEffectSpec Spec;

		FCrowdyEffectAssignmentSpec Assignment;
		Assignment.TargetRole = ECrowdyEffectRole::Target;
		Assignment.Attribute = TEXT("hp");
		Assignment.Operator = ECrowdyEffectAssignmentOp::Subtract;

		FCrowdyEffectTerm First;
		First.Op = ECrowdyEffectBinaryOp::Add;
		First.Operand = AttrOperand(ECrowdyEffectRole::Source, TEXT("power"));
		Assignment.Value.Add(First);

		FCrowdyEffectTerm Second;
		Second.Op = ECrowdyEffectBinaryOp::Multiply;
		Second.Operand.Kind = ECrowdyEffectOperandKind::Number;
		Second.Operand.Literal = TEXT("2");
		Assignment.Value.Add(Second);
		Spec.Assignments.Add(Assignment);

		FCrowdyEffectRequireSpec Keyword;
		Keyword.Kind = ECrowdyEffectRequireKind::Keyword;
		Keyword.Keyword = ECrowdyEffectPolicyKeyword::Participant;
		Spec.Requires.Add(Keyword);

		FCrowdyEffectRequireSpec Comparison;
		Comparison.Kind = ECrowdyEffectRequireKind::Comparison;
		Comparison.Left = AttrOperand(ECrowdyEffectRole::Target, TEXT("mana"));
		Comparison.Comparator = ECrowdyEffectComparator::Greater;
		Comparison.Right.Kind = ECrowdyEffectOperandKind::Call;
		Comparison.Right.Call.Callee = TEXT("max");
		Comparison.Right.Call.Args = { TEXT("1"), TEXT("2") };
		Spec.Requires.Add(Comparison);

		Spec.bHasReturn = true;
		Spec.Return = AttrOperand(ECrowdyEffectRole::Target, TEXT("hp"));
		return Spec;
	}

	// Field-for-field equality of the two compiled functions, so "the records agree" is a claim about every field
	// the sync sends rather than about the handful a spot check would look at.
	bool FunctionsMatch(FAutomationTestBase& Test, const FCrowdyGameModelFunctionInput& A,
		const FCrowdyGameModelFunctionInput& B)
	{
		bool bMatch = true;
		bMatch &= Test.TestEqual(TEXT("name"), A.Name, B.Name);
		bMatch &= Test.TestEqual(TEXT("container type"), A.ContainerTypeName, B.ContainerTypeName);
		bMatch &= Test.TestEqual(TEXT("description"), A.Description, B.Description);
		bMatch &= Test.TestEqual(TEXT("return type"), A.ReturnType, B.ReturnType);
		bMatch &= Test.TestEqual(TEXT("invoke scope"), A.InvokeScope, B.InvokeScope);
		bMatch &= Test.TestEqual(TEXT("autonomous invocable"),
			A.bAutonomousInvocable ? 1 : 0, B.bAutonomousInvocable ? 1 : 0);
		bMatch &= Test.TestEqual(TEXT("return expression"), A.ReturnExpression, B.ReturnExpression);
		bMatch &= Test.TestEqual(TEXT("invoke policy"), A.InvokePolicyJson, B.InvokePolicyJson);

		if (!Test.TestEqual(TEXT("parameter count"), A.Parameters.Num(), B.Parameters.Num()))
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Parameters.Num(); ++Index)
		{
			bMatch &= Test.TestEqual(TEXT("parameter name"), A.Parameters[Index].Name, B.Parameters[Index].Name);
			bMatch &= Test.TestEqual(TEXT("parameter type"), A.Parameters[Index].ValueType, B.Parameters[Index].ValueType);
			bMatch &= Test.TestEqual(TEXT("parameter required"),
				A.Parameters[Index].bRequired ? 1 : 0, B.Parameters[Index].bRequired ? 1 : 0);
			bMatch &= Test.TestEqual(TEXT("parameter default"),
				A.Parameters[Index].DefaultValueJson, B.Parameters[Index].DefaultValueJson);
			bMatch &= Test.TestEqual(TEXT("parameter description"),
				A.Parameters[Index].Description, B.Parameters[Index].Description);
		}

		if (!Test.TestEqual(TEXT("mutation count"), A.Mutations.Num(), B.Mutations.Num()))
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Mutations.Num(); ++Index)
		{
			bMatch &= Test.TestEqual(TEXT("mutation target"), A.Mutations[Index].Target, B.Mutations[Index].Target);
			bMatch &= Test.TestEqual(TEXT("mutation property"), A.Mutations[Index].Property, B.Mutations[Index].Property);
			bMatch &= Test.TestEqual(TEXT("mutation expression"),
				A.Mutations[Index].Expression, B.Mutations[Index].Expression);
		}

		if (!Test.TestEqual(TEXT("notification count"), A.Notifications.Num(), B.Notifications.Num()))
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Notifications.Num(); ++Index)
		{
			bMatch &= Test.TestEqual(TEXT("notification kind"), A.Notifications[Index].Kind, B.Notifications[Index].Kind);
			bMatch &= Test.TestEqual(TEXT("notification arg count"),
				A.Notifications[Index].Args.Num(), B.Notifications[Index].Args.Num());
			for (int32 ArgIndex = 0;
				ArgIndex < FMath::Min(A.Notifications[Index].Args.Num(), B.Notifications[Index].Args.Num());
				++ArgIndex)
			{
				bMatch &= Test.TestEqual(TEXT("notification arg expression"),
					A.Notifications[Index].Args[ArgIndex].Expression,
					B.Notifications[Index].Args[ArgIndex].Expression);
			}
		}

		if (!Test.TestEqual(TEXT("timer count"), A.Timers.Num(), B.Timers.Num()))
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Timers.Num(); ++Index)
		{
			bMatch &= Test.TestEqual(TEXT("timer function"), A.Timers[Index].FunctionName, B.Timers[Index].FunctionName);
			bMatch &= Test.TestEqual(TEXT("timer target"), A.Timers[Index].Target, B.Timers[Index].Target);
			bMatch &= Test.TestEqual(TEXT("timer delay"),
				A.Timers[Index].DelayMsExpression, B.Timers[Index].DelayMsExpression);
			bMatch &= Test.TestEqual(TEXT("timer dedupe"),
				A.Timers[Index].DedupeKeyExpression, B.Timers[Index].DedupeKeyExpression);
		}
		return bMatch;
	}
}

// A surface is only useful if it survives the trip to disk and back unchanged. Serializing the parsed copy and
// comparing the two texts covers every field at once, so a field the writer forgets or the reader drops shows up
// here rather than as a plan that quietly disagrees with the asset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceTextRoundTripTest,
	"CrowdySDK.Effect.SurfaceTextRoundTrips", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceTextRoundTripTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Original = MakePopulatedTextSurface();
	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Original);
	TestFalse(TEXT("the payload is never empty"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("the payload parses"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("the round trip changes nothing"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);

	// A handful of spot checks so a failure names the field rather than only reporting that two long texts differ.
	TestEqual(TEXT("function name"), Parsed.EffectiveFunctionName, Original.EffectiveFunctionName);
	TestEqual(TEXT("script"), Parsed.ScriptText, Original.ScriptText);
	TestEqual(TEXT("invoke scope"), Parsed.InvokeScope, Original.InvokeScope);
	TestEqual(TEXT("magnitude count"), Parsed.Magnitudes.Num(), 2);
	TestEqual(TEXT("magnitude order is the authored one"), Parsed.Magnitudes[0].Name, FString(TEXT("cost")));
	TestEqual(TEXT("timer parameter"), Parsed.Timers[0].Params[0].Expression, FString(TEXT("self.hp")));
	TestTrue(TEXT("carrier"), Parsed.NotificationCarrier == ECrowdyEffectNotificationCarrier::Spatial);
	TestTrue(TEXT("automation trigger"),
		Parsed.Automation.Trigger == ECrowdyEffectAutomationTrigger::OnPropertyChange);
	TestEqual(TEXT("automation cooldown"), Parsed.Automation.CooldownMs, 16);
	return true;
}

// The graph half of the same claim. A graph effect's program is a nested tree of operands, terms and requires, and
// none of it is text the author could re-type: if the payload loses part of it, the effect quietly compiles to
// something smaller than what was drawn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceGraphRoundTripTest,
	"CrowdySDK.Effect.SurfaceGraphRoundTrips", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceGraphRoundTripTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Original = MakePopulatedTextSurface();
	Original.Source = ECrowdyEffectSource::Graph;
	Original.GraphSpec = MakeGraphSpec();

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Original);
	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("the payload parses"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("the round trip changes nothing"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);
	TestTrue(TEXT("the surface is still the graph one"), Parsed.Source == ECrowdyEffectSource::Graph);
	if (TestEqual(TEXT("one assignment"), Parsed.GraphSpec.Assignments.Num(), 1))
	{
		TestEqual(TEXT("two terms"), Parsed.GraphSpec.Assignments[0].Value.Num(), 2);
		TestTrue(TEXT("the second term keeps its operator"),
			Parsed.GraphSpec.Assignments[0].Value[1].Op == ECrowdyEffectBinaryOp::Multiply);
	}
	if (TestEqual(TEXT("two requires"), Parsed.GraphSpec.Requires.Num(), 2))
	{
		TestTrue(TEXT("the call operand keeps its arguments"),
			Parsed.GraphSpec.Requires[1].Right.Call.Args.Num() == 2);
	}
	TestTrue(TEXT("the return survives"), Parsed.GraphSpec.bHasReturn);
	return true;
}

// Comparing a re-serialized copy against the original text cannot see a field the WRITER never writes: both sides
// come from the same writer, so the omission cancels out and the comparison passes. This checks the payload against
// the authored strings instead, which is the only direction that catches a dropped field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceCarriesEveryAuthoredStringTest,
	"CrowdySDK.Effect.SurfaceCarriesEveryAuthoredString", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceCarriesEveryAuthoredStringTest::RunTest(const FString& Parameters)
{
	// Every string the surface can carry, each a value nothing else in the payload would produce, so finding it in
	// the text means that field specifically reached it.
	TArray<FString> Sentinels;
	auto Mark = [&Sentinels](const TCHAR* Token) -> FString
	{
		Sentinels.Add(Token);
		return Token;
	};

	FCrowdyEffectAuthoredSurface Surface;
	Surface.EffectiveFunctionName = Mark(TEXT("zq_fn"));
	Surface.Description = Mark(TEXT("zq_desc"));
	Surface.ContainerClassPath = Mark(TEXT("zq_class"));
	Surface.SourceContainerType = Mark(TEXT("zq_srctype"));
	Surface.Source = ECrowdyEffectSource::Graph;
	Surface.ScriptText = Mark(TEXT("zq_script"));
	Surface.ReturnType = Mark(TEXT("zq_ret"));
	Surface.InvokeScope = Mark(TEXT("zq_scope"));

	Surface.Magnitudes.Add({ Mark(TEXT("zq_magname")), Mark(TEXT("zq_magtype")),
		Mark(TEXT("zq_magdefault")), Mark(TEXT("zq_magdesc")) });

	FCrowdyEffectSignal Signal;
	Signal.Name = Mark(TEXT("zq_signame"));
	Signal.Description = Mark(TEXT("zq_sigdesc"));
	Surface.Signals.Add(Signal);

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = Mark(TEXT("zq_timerfn"));
	Timer.DedupeKey = Mark(TEXT("zq_dedupe"));
	Timer.Target = Mark(TEXT("zq_target"));
	Timer.DelayExpression = Mark(TEXT("zq_delayexpr"));
	Timer.DedupeKeyExpression = Mark(TEXT("zq_dedupeexpr"));
	FCrowdyEffectTimerParam TimerParam;
	TimerParam.Name = Mark(TEXT("zq_paramname"));
	TimerParam.Expression = Mark(TEXT("zq_paramexpr"));
	Timer.Params.Add(TimerParam);
	Surface.Timers.Add(Timer);

	Surface.Automation.CronExpr = Mark(TEXT("zq_cron"));
	Surface.Automation.ChangePropertyKey = Mark(TEXT("zq_changekey"));
	Surface.Automation.WatchFunctionName = Mark(TEXT("zq_watchfn"));
	Surface.Automation.ChangeContainerType = Mark(TEXT("zq_changetype"));
	Surface.Automation.TargetTypeOverride = Mark(TEXT("zq_targettype"));
	Surface.Automation.TargetContainerId = Mark(TEXT("zq_targetid"));
	Surface.Automation.AutomationName = Mark(TEXT("zq_autoname"));

	// One operand carrying a distinct value in each of its string fields. The writer emits all of them for every
	// operand regardless of kind, so a single operand covers the whole shape.
	FCrowdyEffectOperand Operand;
	Operand.Kind = ECrowdyEffectOperandKind::Attribute;
	Operand.Literal = Mark(TEXT("zq_lit"));
	Operand.Name = Mark(TEXT("zq_operandname"));
	Operand.If.Condition = Mark(TEXT("zq_ifc"));
	Operand.If.Then = Mark(TEXT("zq_ift"));
	Operand.If.Else = Mark(TEXT("zq_ife"));
	Operand.Call.Callee = Mark(TEXT("zq_callee"));
	Operand.Call.Args = { Mark(TEXT("zq_arg0")), Mark(TEXT("zq_arg1")) };

	FCrowdyEffectAssignmentSpec Assignment;
	Assignment.Attribute = Mark(TEXT("zq_assignattr"));
	FCrowdyEffectTerm Term;
	Term.Operand = Operand;
	Assignment.Value.Add(Term);
	Surface.GraphSpec.Assignments.Add(Assignment);

	FCrowdyEffectRequireSpec Comparison;
	Comparison.Kind = ECrowdyEffectRequireKind::Comparison;
	Comparison.Left = Operand;
	Comparison.Right = Operand;
	Surface.GraphSpec.Requires.Add(Comparison);

	Surface.GraphSpec.bHasReturn = true;
	Surface.GraphSpec.Return = Operand;

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("the payload is never empty"), Json.IsEmpty());

	for (const FString& Sentinel : Sentinels)
	{
		// Case-SENSITIVE: FString::Contains folds case by default, which would let a writer that lowercased a value
		// still read as having carried it.
		TestTrue(FString::Printf(TEXT("the payload carries %s"), *Sentinel),
			Json.Contains(Sentinel, ESearchCase::CaseSensitive));
	}
	return true;
}

// The surface records WHICH surface authored it. Without that, a text effect and a graph effect whose bodies happen
// to read the same are one payload, and each would describe the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceKeepsItsAuthoringSurfaceTest,
	"CrowdySDK.Effect.SurfaceKeepsItsAuthoringSurface", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceKeepsItsAuthoringSurfaceTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface AsText = MakePopulatedTextSurface();
	AsText.ScriptText.Reset();

	FCrowdyEffectAuthoredSurface AsGraph = AsText;
	AsGraph.Source = ECrowdyEffectSource::Graph;

	TestNotEqual(TEXT("two surfaces with identical bodies do not share a payload"),
		CrowdyEffectAuthoredSurface::ToJson(AsText), CrowdyEffectAuthoredSurface::ToJson(AsGraph));
	return true;
}

// A payload written by a different version of the plugin means fields this build would read as something else, so
// only the exact current version is accepted. Everything else has to fall through to loading the asset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceVersionGateTest,
	"CrowdySDK.Effect.SurfaceVersionGate", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceVersionGateTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("this build's own version is accepted"), CrowdyEffectAuthoredSurface::IsCurrentTagVersion(
		FString::FromInt(FCrowdyEffectAuthoredSurface::TagVersion)));
	TestFalse(TEXT("an older version is refused"), CrowdyEffectAuthoredSurface::IsCurrentTagVersion(
		FString::FromInt(FCrowdyEffectAuthoredSurface::TagVersion - 1)));
	TestFalse(TEXT("a newer version is refused"), CrowdyEffectAuthoredSurface::IsCurrentTagVersion(
		FString::FromInt(FCrowdyEffectAuthoredSurface::TagVersion + 1)));
	TestFalse(TEXT("text that is not a version at all is refused"),
		CrowdyEffectAuthoredSurface::IsCurrentTagVersion(TEXT("latest")));
	TestFalse(TEXT("an absent version is refused"), CrowdyEffectAuthoredSurface::IsCurrentTagVersion(FString()));
	return true;
}

// The parity claim, and the reason the snapshot exists at all: a record built from a stored payload and a record
// built by reading the live asset are the same record. Both sides go through one compile, so this fails the moment
// the snapshot stops carrying something the compile reads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceTagAndLoadAgreeTest,
	"CrowdySDK.Effect.SurfaceTagAndLoadAgree", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceTagAndLoadAgreeTest::RunTest(const FString& Parameters)
{
	// The discovery fixture deliberately carries two rejected properties and re-logs them on every discovery pass,
	// so the count is "one or more" rather than a fixed number.
	AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, 0);

	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelDiscoveryTarget::StaticClass();
	Effect->Source = ECrowdyEffectSource::Text;
	// The function name is left blank on purpose: the effective name falls back to the ASSET name, and a snapshot
	// that stored the raw field would carry an empty one that no reader could recover.
	Effect->FunctionName.Reset();
	Effect->Description = TEXT("drains health");
	Effect->EffectScript = TEXT("self.Health -= $amount\nreturn self.Health");
	Effect->ReturnType = ECrowdyEffectReturnType::Int;
	Effect->CallableFrom = ECrowdyEffectCallableFrom::ServerOnly;
	Effect->bInvokeScopeMigrated = true;
	Effect->NotificationCarrier = ECrowdyEffectNotificationCarrier::Channel;

	FCrowdyEffectMagnitude Magnitude;
	Magnitude.Name = TEXT("amount");
	Magnitude.ValueTypeEnum = ECrowdyEffectValueType::Int;
	Magnitude.bTypeMigrated = true;
	Magnitude.DefaultValueJson = TEXT("3");
	Effect->Magnitudes.Add(Magnitude);

	FCrowdyEffectSignal Signal;
	Signal.Name = TEXT("Drained");
	Effect->Signals.Add(Signal);

	FCrowdyEffectTimer Timer;
	Timer.FunctionName = TEXT("tick");
	Timer.DelayMs = 500;
	Effect->Timers.Add(Timer);

	TArray<FCrowdyEffectDiagnostic> FromLoadDiagnostics;
	const FCrowdyEffectAuthoredSurface FromLoad =
		CrowdyEffectAuthoredSurface::FromEffect(*Effect, FromLoadDiagnostics);
	TestEqual(TEXT("snapshotting a text effect raises nothing"), FromLoadDiagnostics.Num(), 0);
	TestEqual(TEXT("the blank function name resolved to the asset name"),
		FromLoad.EffectiveFunctionName, Effect->GetName());
	TestEqual(TEXT("the invoke scope is stored in wire form"), FromLoad.InvokeScope, FString(TEXT("server")));

	FCrowdyEffectAuthoredSurface FromTag;
	if (!TestTrue(TEXT("the payload parses"),
		CrowdyEffectAuthoredSurface::FromJson(CrowdyEffectAuthoredSurface::ToJson(FromLoad), FromTag)))
	{
		return false;
	}

	FString LoadedTypeName;
	FString TaggedTypeName;
	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult LoadedResult = CrowdyEffectAuthoredSurface::CompileResolved(
		FromLoad, ECrowdyEffectFnCatalog::None, None, LoadedTypeName);
	const FCrowdyEffectLoweringResult TaggedResult = CrowdyEffectAuthoredSurface::CompileResolved(
		FromTag, ECrowdyEffectFnCatalog::None, None, TaggedTypeName);

	TestFalse(TEXT("the loaded path compiles"), LoadedResult.HasErrors());
	TestFalse(TEXT("the tagged path compiles"), TaggedResult.HasErrors());
	TestEqual(TEXT("both resolve the same container type"), TaggedTypeName, LoadedTypeName);
	TestEqual(TEXT("both agree on whether a Source is needed"),
		TaggedResult.bSourceReferenced ? 1 : 0, LoadedResult.bSourceReferenced ? 1 : 0);
	TestEqual(TEXT("both agree on whether an automation was authored"),
		TaggedResult.Automation.IsSet() ? 1 : 0, LoadedResult.Automation.IsSet() ? 1 : 0);
	FunctionsMatch(*this, TaggedResult.Function, LoadedResult.Function);
	return true;
}

// The regression this whole refactor exists to prevent: a cross-type effect used to resolve its source container by
// scanning the loaded classes, so the answer depended on which packages happened to be in memory. Nothing is loaded
// here at all, and the source's own attributes still have to be the ones the body is checked against.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectCrossTypeLowersWithNothingLoadedTest,
	"CrowdySDK.Effect.CrossTypeLowersWithNothingLoaded", CrowdySurfaceTestFlags)
bool FCrowdyEffectCrossTypeLowersWithNothingLoadedTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyEffectVocabularyType> Types = MakeProjectTypes();

	FCrowdyEffectAuthoredSurface Surface;
	Surface.EffectiveFunctionName = TEXT("strike");
	Surface.Source = ECrowdyEffectSource::Text;
	Surface.SourceContainerType = TEXT("Minion");
	Surface.ScriptText = TEXT("self.hp -= source.power");
	Surface.InvokeScope = TEXT("player");

	const FCrowdyEffectVocabulary Vocabulary =
		CrowdyEffectAuthoredSurface::ResolveVocabularyFromTypes(Types, TEXT("Hero"), Surface.SourceContainerType);
	TestFalse(TEXT("the source type resolved"), Vocabulary.bSourceTypeUnresolved);
	TestEqual(TEXT("the source vocabulary is the source type's"), Vocabulary.SourceAttributes.Num(), 1);

	const TArray<FCrowdyEffectDiagnostic> None;
	const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::Compile(
		Surface, Vocabulary, ECrowdyModelNotificationCarrier::None, ECrowdyEffectFnCatalog::None, None);

	TestFalse(TEXT("a cross-type effect lowers with no container class anywhere"), Result.HasErrors());
	if (TestEqual(TEXT("one mutation"), Result.Function.Mutations.Num(), 1))
	{
		TestEqual(TEXT("the source attribute lowered through the injected ref"), Result.Function.Mutations[0].Expression,
			FString(TEXT("max(0, min(100, self.hp - (ref($source_id).power)))")));
	}
	TestTrue(TEXT("the effect is marked as needing a Source"), Result.bSourceReferenced);

	// The other half of the same rule: a source type the caller does not know is REPORTED, never quietly checked
	// against the target's attributes, which would accept names the source does not have.
	FCrowdyEffectAuthoredSurface Unknown = Surface;
	Unknown.SourceContainerType = TEXT("Ghost");
	const FCrowdyEffectVocabulary Missing =
		CrowdyEffectAuthoredSurface::ResolveVocabularyFromTypes(Types, TEXT("Hero"), Unknown.SourceContainerType);
	TestTrue(TEXT("an unknown source type is flagged rather than substituted"), Missing.bSourceTypeUnresolved);
	return true;
}

// Never persist a compile failure. A failure is waiting on something outside the asset (a compiler that lives in the
// editor module, a class not created yet), so recording it would keep the effect broken after that arrived, and any
// process without the editor tooling would stamp the same verdict onto every graph effect it saved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceWritesNoPayloadOnCompileFailureTest,
	"CrowdySDK.Effect.SurfaceWritesNoPayloadOnCompileFailure", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceWritesNoPayloadOnCompileFailureTest::RunTest(const FString& Parameters)
{
	// A node graph is editor-only data. Where it does not exist, a graph-authored effect has no surface to build at
	// all, which the ladder already treats as "load it", so there is nothing here to assert.
#if !WITH_EDITORONLY_DATA
	return true;
#else
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyGameModelDiscoveryTarget::StaticClass();
	Effect->Source = ECrowdyEffectSource::Graph;

	{
		const FScopedGraphCompiler Failing([](const UEdGraph*, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
		{
			OutDiagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, TEXT("the graph is not finished") });
			return FCrowdyEffectSpec();
		});

		FString Payload = TEXT("stale");
		TestFalse(TEXT("a graph that does not compile produces no payload"),
			CrowdyEffectAuthoredSurface::BuildTagPayload(*Effect, Payload));
		TestTrue(TEXT("and leaves nothing behind to be written"), Payload.IsEmpty());
	}

	{
		const FScopedGraphCompiler Working([](const UEdGraph*, TArray<FCrowdyEffectDiagnostic>&)
		{
			return MakeGraphSpec();
		});

		FString Payload;
		TestTrue(TEXT("a graph that compiles produces a payload"),
			CrowdyEffectAuthoredSurface::BuildTagPayload(*Effect, Payload));
		TestFalse(TEXT("and it is never empty, which the registry treats as a programming error"), Payload.IsEmpty());

		FCrowdyEffectAuthoredSurface Parsed;
		TestTrue(TEXT("the payload it produced is readable"),
			CrowdyEffectAuthoredSurface::FromJson(Payload, Parsed));
		TestEqual(TEXT("the compiled graph travelled with it"), Parsed.GraphSpec.Assignments.Num(), 1);
	}

	// A warning is not a failure: an effect the author can still ship must not lose its payload over one.
	{
		const FScopedGraphCompiler Warning([](const UEdGraph*, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
		{
			OutDiagnostics.Add({ ECrowdyEffectSeverity::Warning, 0, 0, TEXT("an unused input") });
			return MakeGraphSpec();
		});

		FString Payload;
		TestTrue(TEXT("a warning still produces a payload"),
			CrowdyEffectAuthoredSurface::BuildTagPayload(*Effect, Payload));
	}
	return true;
#endif
}

// The payload is written once and read on other machines, so the same value has to produce the same bytes however
// the surface was assembled. A writer that leaned on the order fields were filled in, or on a map's iteration order,
// would make every save a spurious change in version control.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceSerializationIsDeterministicTest,
	"CrowdySDK.Effect.SurfaceSerializationIsDeterministic", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceSerializationIsDeterministicTest::RunTest(const FString& Parameters)
{
	const FCrowdyEffectAuthoredSurface Forwards = MakePopulatedTextSurface();

	// The same value, assembled in the opposite order and with its lists built back to front then corrected, so the
	// only thing the two share is what they mean.
	FCrowdyEffectAuthoredSurface Backwards;
	Backwards.Automation = Forwards.Automation;
	Backwards.NotificationCarrier = Forwards.NotificationCarrier;
	Backwards.InvokeScope = Forwards.InvokeScope;
	Backwards.ReturnType = Forwards.ReturnType;
	for (int32 Index = Forwards.Timers.Num() - 1; Index >= 0; --Index)
	{
		Backwards.Timers.Insert(Forwards.Timers[Index], 0);
	}
	for (int32 Index = Forwards.Signals.Num() - 1; Index >= 0; --Index)
	{
		Backwards.Signals.Insert(Forwards.Signals[Index], 0);
	}
	for (int32 Index = Forwards.Magnitudes.Num() - 1; Index >= 0; --Index)
	{
		Backwards.Magnitudes.Insert(Forwards.Magnitudes[Index], 0);
	}
	Backwards.ScriptText = Forwards.ScriptText;
	Backwards.Source = Forwards.Source;
	Backwards.SourceContainerType = Forwards.SourceContainerType;
	Backwards.ContainerClassPath = Forwards.ContainerClassPath;
	Backwards.Description = Forwards.Description;
	Backwards.EffectiveFunctionName = Forwards.EffectiveFunctionName;

	TestEqual(TEXT("assembly order does not reach the bytes"),
		CrowdyEffectAuthoredSurface::ToJson(Backwards), CrowdyEffectAuthoredSurface::ToJson(Forwards));

	// Serializing the same value twice in a row has to agree too, which is what a hidden map iteration would break.
	TestEqual(TEXT("two writes of one value agree"),
		CrowdyEffectAuthoredSurface::ToJson(Forwards), CrowdyEffectAuthoredSurface::ToJson(Forwards));
	return true;
}

// A number that is persisted and compared across machines needs exactly one spelling. Left to an ordinary float
// print, a whole number and the same whole number typed as a float read as two different values, and a key built
// from them reports a change that never happened.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCanonicalNumberHasOneSpellingTest,
	"CrowdySDK.Effect.CanonicalNumberHasOneSpelling", CrowdySurfaceTestFlags)
bool FCrowdyCanonicalNumberHasOneSpellingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a whole number prints as an integer"), CrowdyCanonicalNumber::ToText(10.0), FString(TEXT("10")));
	TestEqual(TEXT("and reaches that spelling from either side"),
		CrowdyCanonicalNumber::ToText(10.0), CrowdyCanonicalNumber::ToText(static_cast<double>(10)));
	TestEqual(TEXT("zero and negative zero are one value"),
		CrowdyCanonicalNumber::ToText(-0.0), CrowdyCanonicalNumber::ToText(0.0));
	TestEqual(TEXT("a negative whole number keeps its sign"),
		CrowdyCanonicalNumber::ToText(-7.0), FString(TEXT("-7")));
	TestNotEqual(TEXT("a fractional value is still distinct from its truncation"),
		CrowdyCanonicalNumber::ToText(10.5), CrowdyCanonicalNumber::ToText(10.0));

	// A value with no integer spelling must not take the integer branch: the cast there is only defined inside
	// int64 range, so a value beyond it has to come back through the float path rather than as a wrapped integer.
	TestFalse(TEXT("a value outside integer range still produces text"),
		CrowdyCanonicalNumber::ToText(1.0e30).IsEmpty());
	TestNotEqual(TEXT("and it is not the smallest integer, which is what a wrapped cast would give"),
		CrowdyCanonicalNumber::ToText(1.0e30), FString(TEXT("-9223372036854775808")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceEmptyOptionalFieldsTest,
	"CrowdySDK.Effect.SurfaceEmptyOptionalFields", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceEmptyOptionalFieldsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();
	Surface.Description.Reset();
	Surface.SourceContainerType.Reset();
	Surface.ScriptText.Reset();
	Surface.ReturnType.Reset();
	Surface.Magnitudes.Reset();
	Surface.Signals.Reset();
	Surface.Timers.Reset();
	Surface.Automation = FCrowdyEffectAutomationAuthoring();

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("the payload is never empty even with empty fields"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("empty fields round-trip"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("round trip preserves the payload"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);
	TestTrue(TEXT("description is empty"), Parsed.Description.IsEmpty());
	TestTrue(TEXT("source type is empty"), Parsed.SourceContainerType.IsEmpty());
	TestTrue(TEXT("script text is empty"), Parsed.ScriptText.IsEmpty());
	TestTrue(TEXT("return type is empty"), Parsed.ReturnType.IsEmpty());
	TestEqual(TEXT("magnitudes array is empty"), Parsed.Magnitudes.Num(), 0);
	TestEqual(TEXT("signals array is empty"), Parsed.Signals.Num(), 0);
	TestEqual(TEXT("timers array is empty"), Parsed.Timers.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceSpecialCharactersTest,
	"CrowdySDK.Effect.SurfaceSpecialCharacters", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceSpecialCharactersTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();

	Surface.Description = TEXT("A \"quoted\" effect\nwith newlines\nand backslashes\\ inside");
	Surface.EffectiveFunctionName = TEXT("strike_with_\"quotes\"");
	Surface.ScriptText = TEXT("self.hp -= $cost\n\tself.mana -= 1");

	FCrowdyEffectParamDecl SpecialParam;
	SpecialParam.Name = TEXT("key");
	SpecialParam.ValueType = TEXT("string");
	SpecialParam.DefaultValueJson = TEXT("\"\\\"quoted\\\" \\\\backslash\\\\\"");
	SpecialParam.Description = TEXT("Contains \"quotes\" and \\backslashes\\");
	Surface.Magnitudes.Reset();
	Surface.Magnitudes.Add(SpecialParam);

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("special characters produce a valid payload"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("special characters round-trip"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("quoted text survives"), Parsed.Description, Surface.Description);
	TestEqual(TEXT("newlines survive"), Parsed.ScriptText, Surface.ScriptText);

	// Gated rather than compared, because the read below indexes it: a parse that dropped the magnitude
	// would otherwise be an engine bounds assert that takes the whole run down instead of failing here.
	if (!TestEqual(TEXT("the parsed surface kept its one magnitude"), Parsed.Magnitudes.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("magnitude with special chars survives"),
		Parsed.Magnitudes[0].DefaultValueJson, SpecialParam.DefaultValueJson);
	TestEqual(TEXT("round trip unchanged"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceNonAsciiTest,
	"CrowdySDK.Effect.SurfaceNonAscii", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceNonAsciiTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();

	Surface.Description = TEXT("Effect with emoji: helix (αβγ) and Chinese 一二三");
	Surface.EffectiveFunctionName = TEXT("strike_é");

	FCrowdyEffectSignal Signal;
	Signal.Name = TEXT("Signal");
	Signal.Description = TEXT("Non-ASCII chars: üöä");
	Surface.Signals.Reset();
	Surface.Signals.Add(Signal);

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("non-ASCII produces a valid payload"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("non-ASCII round-trips"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("non-ASCII in description preserved"), Parsed.Description, Surface.Description);
	TestEqual(TEXT("non-ASCII in function name preserved"), Parsed.EffectiveFunctionName, Surface.EffectiveFunctionName);

	// Gated for the same reason as the magnitude above: the read below indexes this array.
	if (!TestEqual(TEXT("the parsed surface kept its one signal"), Parsed.Signals.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("non-ASCII in signal description preserved"),
		Parsed.Signals[0].Description, Signal.Description);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceNumericExtremeTest,
	"CrowdySDK.Effect.SurfaceNumericExtremes", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceNumericExtremeTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();

	Surface.Automation.IntervalMs = 0;
	Surface.Automation.DebounceMs = INT32_MAX;
	Surface.Automation.MaxTargets = 1;
	Surface.Automation.GasLimit = INT32_MAX;
	Surface.Automation.RunTimeoutMs = 0;
	Surface.Automation.MaxRunsPerMinute = INT32_MIN;
	Surface.Automation.FailureThreshold = 0;
	Surface.Automation.CooldownMs = INT32_MAX;

	FCrowdyEffectTimer ExtremTimer;
	ExtremTimer.FunctionName = TEXT("fire");
	ExtremTimer.DelayMs = 1;
	ExtremTimer.DedupeKey = TEXT("k");
	Surface.Timers.Reset();
	Surface.Timers.Add(ExtremTimer);

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("extreme numeric values produce a valid payload"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("extreme values round-trip"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("zero interval preserved"), Parsed.Automation.IntervalMs, 0);
	TestEqual(TEXT("max int debounce preserved"), Parsed.Automation.DebounceMs, INT32_MAX);
	TestEqual(TEXT("single target preserved"), Parsed.Automation.MaxTargets, 1);
	TestEqual(TEXT("min int runs per minute preserved"), Parsed.Automation.MaxRunsPerMinute, INT32_MIN);
	TestEqual(TEXT("zero timeout preserved"), Parsed.Automation.RunTimeoutMs, 0);
	TestEqual(TEXT("timer delay 1 preserved"), Parsed.Timers[0].DelayMs, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceArrayEmptyVsAbsentTest,
	"CrowdySDK.Effect.SurfaceArrayEmptyVsAbsent", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceArrayEmptyVsAbsentTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface WithEmptyArrays;
	WithEmptyArrays.EffectiveFunctionName = TEXT("test");
	WithEmptyArrays.ContainerClassPath = TEXT("/Game/Test");
	WithEmptyArrays.Source = ECrowdyEffectSource::Text;
	WithEmptyArrays.ScriptText = TEXT("return 1");
	WithEmptyArrays.InvokeScope = TEXT("player");

	const FString EmptyJson = CrowdyEffectAuthoredSurface::ToJson(WithEmptyArrays);
	TestFalse(TEXT("empty arrays produce a valid payload"), EmptyJson.IsEmpty());

	FCrowdyEffectAuthoredSurface ParsedEmpty;
	if (!TestTrue(TEXT("empty arrays parse"), CrowdyEffectAuthoredSurface::FromJson(EmptyJson, ParsedEmpty)))
	{
		return false;
	}

	TestEqual(TEXT("magnitudes stay empty"), ParsedEmpty.Magnitudes.Num(), 0);
	TestEqual(TEXT("signals stay empty"), ParsedEmpty.Signals.Num(), 0);
	TestEqual(TEXT("timers stay empty"), ParsedEmpty.Timers.Num(), 0);
	TestEqual(TEXT("round trip unchanged"), CrowdyEffectAuthoredSurface::ToJson(ParsedEmpty), EmptyJson);

	FCrowdyEffectAuthoredSurface WithArrays = WithEmptyArrays;
	{
		FCrowdyEffectParamDecl Mag;
		Mag.Name = TEXT("m");
		Mag.ValueType = TEXT("int");
		Mag.DefaultValueJson = TEXT("1");
		WithArrays.Magnitudes.Add(Mag);
	}
	{
		FCrowdyEffectSignal Sig;
		Sig.Name = TEXT("s");
		WithArrays.Signals.Add(Sig);
	}
	{
		FCrowdyEffectTimer Tim;
		Tim.FunctionName = TEXT("t");
		Tim.DelayMs = 100;
		WithArrays.Timers.Add(Tim);
	}

	const FString WithJson = CrowdyEffectAuthoredSurface::ToJson(WithArrays);
	TestNotEqual(TEXT("a populated array changes the payload"), WithJson, EmptyJson);

	FCrowdyEffectAuthoredSurface ParsedWith;
	if (!TestTrue(TEXT("populated arrays parse"), CrowdyEffectAuthoredSurface::FromJson(WithJson, ParsedWith)))
	{
		return false;
	}

	TestEqual(TEXT("magnitudes count is 1"), ParsedWith.Magnitudes.Num(), 1);
	TestEqual(TEXT("signals count is 1"), ParsedWith.Signals.Num(), 1);
	TestEqual(TEXT("timers count is 1"), ParsedWith.Timers.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceArrayManyElementsTest,
	"CrowdySDK.Effect.SurfaceArrayManyElements", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceArrayManyElementsTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();
	Surface.Magnitudes.Reset();
	Surface.Signals.Reset();
	Surface.Timers.Reset();

	for (int32 Index = 0; Index < 50; ++Index)
	{
		FCrowdyEffectParamDecl Mag;
		Mag.Name = FString::Printf(TEXT("param_%d"), Index);
		Mag.ValueType = (Index % 2 == 0) ? TEXT("int") : TEXT("string");
		Mag.DefaultValueJson = FString::Printf(TEXT("%d"), Index);
		Mag.Description = FString::Printf(TEXT("Parameter number %d"), Index);
		Surface.Magnitudes.Add(Mag);
	}

	for (int32 Index = 0; Index < 30; ++Index)
	{
		FCrowdyEffectSignal Sig;
		Sig.Name = FString::Printf(TEXT("Signal_%d"), Index);
		Sig.Description = FString::Printf(TEXT("Signal %d fired"), Index);
		Surface.Signals.Add(Sig);
	}

	for (int32 Index = 0; Index < 20; ++Index)
	{
		FCrowdyEffectTimer Tim;
		Tim.FunctionName = TEXT("tick");
		Tim.DelayMs = 100 * (Index + 1);
		Tim.DedupeKey = FString::Printf(TEXT("timer_%d"), Index);
		Surface.Timers.Add(Tim);
	}

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("many elements produce a valid payload"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("many elements round-trip"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("50 magnitudes preserved"), Parsed.Magnitudes.Num(), 50);
	TestEqual(TEXT("30 signals preserved"), Parsed.Signals.Num(), 30);
	TestEqual(TEXT("20 timers preserved"), Parsed.Timers.Num(), 20);

	TestEqual(TEXT("magnitude order preserved"), Parsed.Magnitudes[25].Name, FString(TEXT("param_25")));
	TestEqual(TEXT("signal order preserved"), Parsed.Signals[15].Name, FString(TEXT("Signal_15")));
	TestEqual(TEXT("timer order preserved"), Parsed.Timers[10].DelayMs, 1100);

	TestEqual(TEXT("round trip unchanged"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceFloatPrecisionTest,
	"CrowdySDK.Effect.SurfaceFloatPrecision", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceFloatPrecisionTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();
	Surface.Magnitudes.Reset();

	const FString ProblematicFloats[] = {
		TEXT("0.1"),
		TEXT("0.3"),
		TEXT("1e-7"),
		TEXT("-0.0"),
		TEXT("1e10"),
		TEXT("9999999999.99"),
		TEXT("-1e-10")
	};

	for (const FString& FloatStr : ProblematicFloats)
	{
		FCrowdyEffectParamDecl Mag;
		Mag.Name = FString::Printf(TEXT("val_%s"), *FloatStr.Replace(TEXT("."), TEXT("_")).Replace(TEXT("-"), TEXT("m")).Replace(TEXT("e"), TEXT("e")));
		Mag.ValueType = TEXT("float");
		Mag.DefaultValueJson = FloatStr;
		Mag.Description = FString::Printf(TEXT("Float %s"), *FloatStr);
		Surface.Magnitudes.Add(Mag);
	}

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("problematic floats produce a valid payload"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("problematic floats round-trip"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	for (int32 Index = 0; Index < Surface.Magnitudes.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("float %s preserved"), *Surface.Magnitudes[Index].DefaultValueJson),
			Parsed.Magnitudes[Index].DefaultValueJson,
			Surface.Magnitudes[Index].DefaultValueJson);
	}

	TestEqual(TEXT("round trip unchanged"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceSourceIdentityTest,
	"CrowdySDK.Effect.SurfaceSourceIdentity", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceSourceIdentityTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface TextEffect;
	TextEffect.EffectiveFunctionName = TEXT("strike");
	TextEffect.Description = TEXT("a hit");
	TextEffect.ContainerClassPath = TEXT("/Game/Hero");
	TextEffect.Source = ECrowdyEffectSource::Text;
	TextEffect.ScriptText = TEXT("self.hp -= 1");
	TextEffect.InvokeScope = TEXT("player");

	FCrowdyEffectAuthoredSurface GraphEffect;
	GraphEffect.EffectiveFunctionName = TEXT("strike");
	GraphEffect.Description = TEXT("a hit");
	GraphEffect.ContainerClassPath = TEXT("/Game/Hero");
	GraphEffect.Source = ECrowdyEffectSource::Graph;
	GraphEffect.GraphSpec = MakeGraphSpec();
	GraphEffect.InvokeScope = TEXT("player");

	const FString TextJson = CrowdyEffectAuthoredSurface::ToJson(TextEffect);
	const FString GraphJson = CrowdyEffectAuthoredSurface::ToJson(GraphEffect);

	TestNotEqual(TEXT("text and graph sources produce different payloads"),
		TextJson, GraphJson);

	FCrowdyEffectAuthoredSurface ParsedText;
	if (!TestTrue(TEXT("text payload parses"), CrowdyEffectAuthoredSurface::FromJson(TextJson, ParsedText)))
	{
		return false;
	}

	FCrowdyEffectAuthoredSurface ParsedGraph;
	if (!TestTrue(TEXT("graph payload parses"), CrowdyEffectAuthoredSurface::FromJson(GraphJson, ParsedGraph)))
	{
		return false;
	}

	TestTrue(TEXT("parsed text stays text"), ParsedText.Source == ECrowdyEffectSource::Text);
	TestTrue(TEXT("parsed graph stays graph"), ParsedGraph.Source == ECrowdyEffectSource::Graph);

	TestTrue(TEXT("text has script"), !ParsedText.ScriptText.IsEmpty());
	TestTrue(TEXT("text has no spec"), ParsedText.GraphSpec.Assignments.Num() == 0);

	TestTrue(TEXT("graph has no script"), ParsedGraph.ScriptText.IsEmpty());
	TestTrue(TEXT("graph has spec"), ParsedGraph.GraphSpec.Assignments.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceComplexGraphTest,
	"CrowdySDK.Effect.SurfaceComplexGraph", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceComplexGraphTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();
	Surface.Source = ECrowdyEffectSource::Graph;
	Surface.ScriptText.Reset();

	FCrowdyEffectSpec Spec;

	for (int32 AssignIdx = 0; AssignIdx < 5; ++AssignIdx)
	{
		FCrowdyEffectAssignmentSpec Assignment;
		Assignment.TargetRole = ECrowdyEffectRole::Target;
		Assignment.Attribute = (AssignIdx % 2 == 0) ? TEXT("hp") : TEXT("mana");
		Assignment.Operator = (AssignIdx % 3 == 0) ? ECrowdyEffectAssignmentOp::Add : ECrowdyEffectAssignmentOp::Subtract;

		for (int32 TermIdx = 0; TermIdx < 3; ++TermIdx)
		{
			FCrowdyEffectTerm Term;
			// The first term's operator is ignored, so every term can carry a real one.
			Term.Op = (TermIdx % 2 == 0) ? ECrowdyEffectBinaryOp::Add : ECrowdyEffectBinaryOp::Multiply;

			if (TermIdx % 2 == 0)
			{
				Term.Operand = AttrOperand(ECrowdyEffectRole::Source, TEXT("power"));
			}
			else
			{
				Term.Operand.Kind = ECrowdyEffectOperandKind::Number;
				Term.Operand.Literal = FString::Printf(TEXT("%d"), TermIdx);
			}

			Assignment.Value.Add(Term);
		}

		Spec.Assignments.Add(Assignment);
	}

	for (int32 ReqIdx = 0; ReqIdx < 3; ++ReqIdx)
	{
		if (ReqIdx % 2 == 0)
		{
			FCrowdyEffectRequireSpec Keyword;
			Keyword.Kind = ECrowdyEffectRequireKind::Keyword;
			Keyword.Keyword = (ReqIdx == 0) ? ECrowdyEffectPolicyKeyword::Participant : ECrowdyEffectPolicyKeyword::Host;
			Spec.Requires.Add(Keyword);
		}
		else
		{
			FCrowdyEffectRequireSpec Comparison;
			Comparison.Kind = ECrowdyEffectRequireKind::Comparison;
			Comparison.Left = AttrOperand(ECrowdyEffectRole::Target, TEXT("mana"));
			Comparison.Comparator = ECrowdyEffectComparator::Greater;
			Comparison.Right.Kind = ECrowdyEffectOperandKind::Number;
			Comparison.Right.Literal = TEXT("10");
			Spec.Requires.Add(Comparison);
		}
	}

	Spec.bHasReturn = true;
	Spec.Return = AttrOperand(ECrowdyEffectRole::Target, TEXT("hp"));

	Surface.GraphSpec = Spec;

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);
	TestFalse(TEXT("complex graph produces a valid payload"), Json.IsEmpty());

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("complex graph round-trips"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}

	TestEqual(TEXT("5 assignments preserved"), Parsed.GraphSpec.Assignments.Num(), 5);
	TestEqual(TEXT("3 requires preserved"), Parsed.GraphSpec.Requires.Num(), 3);
	TestTrue(TEXT("return preserved"), Parsed.GraphSpec.bHasReturn);

	for (int32 Index = 0; Index < Parsed.GraphSpec.Assignments.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("assignment %d has 3 terms"), Index),
			Parsed.GraphSpec.Assignments[Index].Value.Num(),
			3);
	}

	TestEqual(TEXT("round trip unchanged"), CrowdyEffectAuthoredSurface::ToJson(Parsed), Json);
	return true;
}

// Required-ness is carried in the payload, not re-derived from an empty default on the way back. The other
// round-trip tests compare one serialization against another, which a writer and a reader that both drop the
// field satisfy, so this asserts the parsed value directly. The two magnitudes differ only in required-ness and
// one of them keeps a default while required, which is the pair the old empty-default rule could not express.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceCarriesRequiredTest,
	"CrowdySDK.Effect.SurfaceCarriesRequired", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceCarriesRequiredTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();
	Surface.Magnitudes.Reset();

	FCrowdyEffectParamDecl RequiredParam;
	RequiredParam.Name = TEXT("target");
	RequiredParam.ValueType = TEXT("string");
	RequiredParam.DefaultValueJson = TEXT("\"ignored\"");
	RequiredParam.bRequired = true;
	Surface.Magnitudes.Add(RequiredParam);

	FCrowdyEffectParamDecl OptionalParam;
	OptionalParam.Name = TEXT("amount");
	OptionalParam.ValueType = TEXT("int");
	OptionalParam.DefaultValueJson = TEXT("3");
	OptionalParam.bRequired = false;
	Surface.Magnitudes.Add(OptionalParam);

	const FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("the payload parses"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}
	if (!TestEqual(TEXT("both magnitudes survive"), Parsed.Magnitudes.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("the required magnitude comes back required"), Parsed.Magnitudes[0].bRequired);
	TestFalse(TEXT("the optional magnitude comes back optional"), Parsed.Magnitudes[1].bRequired);

	// The control that makes the first assertion mean something: a required parameter keeps a non-empty default,
	// so required-ness cannot have been re-derived from emptiness on the way back in.
	TestEqual(TEXT("the required magnitude kept its default text"),
		Parsed.Magnitudes[0].DefaultValueJson, FString(TEXT("\"ignored\"")));
	return true;
}

// A payload written before required-ness existed carries no flag, and must read back exactly as the old rule
// meant it: required when the default is empty, optional otherwise, and never required for a bool.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectSurfaceLegacyRequiredFallbackTest,
	"CrowdySDK.Effect.SurfaceLegacyRequiredFallback", CrowdySurfaceTestFlags)
bool FCrowdyEffectSurfaceLegacyRequiredFallbackTest::RunTest(const FString& Parameters)
{
	FCrowdyEffectAuthoredSurface Surface = MakePopulatedTextSurface();
	Surface.Magnitudes.Reset();
	Surface.Magnitudes.Add({ TEXT("empty"), TEXT("int"), FString(), FString() });
	Surface.Magnitudes.Add({ TEXT("defaulted"), TEXT("int"), TEXT("7"), FString() });
	Surface.Magnitudes.Add({ TEXT("flag"), TEXT("bool"), FString(), FString() });

	FString Json = CrowdyEffectAuthoredSurface::ToJson(Surface);

	// Strip every required key, which is what a payload stamped before the field existed looks like. Only the
	// boolean form is touched: a graph require's right-hand operand uses the same short key for an object.
	TestTrue(TEXT("the payload carried a required key to begin with"),
		Json.Contains(TEXT("\"r\":true")) || Json.Contains(TEXT("\"r\":false")));
	Json = Json.Replace(TEXT("\"r\":true,"), TEXT(""));
	Json = Json.Replace(TEXT("\"r\":false,"), TEXT(""));
	Json = Json.Replace(TEXT(",\"r\":true"), TEXT(""));
	Json = Json.Replace(TEXT(",\"r\":false"), TEXT(""));
	TestFalse(TEXT("no boolean required key survives the strip"),
		Json.Contains(TEXT("\"r\":true")) || Json.Contains(TEXT("\"r\":false")));

	FCrowdyEffectAuthoredSurface Parsed;
	if (!TestTrue(TEXT("the stripped payload still parses"), CrowdyEffectAuthoredSurface::FromJson(Json, Parsed)))
	{
		return false;
	}
	if (!TestEqual(TEXT("all three magnitudes survive"), Parsed.Magnitudes.Num(), 3))
	{
		return false;
	}

	TestTrue(TEXT("an empty default reads as required"), Parsed.Magnitudes[0].bRequired);
	TestFalse(TEXT("an authored default reads as optional"), Parsed.Magnitudes[1].bRequired);
	TestFalse(TEXT("a bool is never required by the fallback"), Parsed.Magnitudes[2].bRequired);

	// The bool also comes back with the explicit false a load writes onto the asset. Without that this payload and
	// the asset it came from describe two different schemas for one parameter.
	TestEqual(TEXT("an optional bool is normalized to the explicit false"), Parsed.Magnitudes[2].DefaultValueJson,
		FString(TEXT("false")));

	// The controls: normalization touches the bool alone, and invents nothing for a type whose default really is
	// absent.
	TestTrue(TEXT("a required int keeps no default"), Parsed.Magnitudes[0].DefaultValueJson.IsEmpty());
	TestEqual(TEXT("an authored default is left as authored"), Parsed.Magnitudes[1].DefaultValueJson,
		FString(TEXT("7")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
