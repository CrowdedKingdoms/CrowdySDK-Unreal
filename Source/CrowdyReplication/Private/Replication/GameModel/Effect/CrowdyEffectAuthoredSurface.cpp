// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Replication/GameModel/Effect/CrowdyEffectGraphCompileHook.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#if WITH_EDITORONLY_DATA
#include "EdGraph/EdGraph.h"
#endif

namespace
{
	using FSurfaceWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
	using FSurfaceWriterFactory = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

	// Backs the lowering context's lazy fn: callee lookup. The context is copied by value and can outlive the call
	// that built it, so the cache is reached through a shared pointer captured by value, never a local by reference.
	struct FCrowdyEffectFnCalleeCache
	{
		bool bLoaded = false;
		TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction> Functions;
	};

	// Reads an enum that was stored as its integer value, refusing anything outside the declared range. A payload
	// travels on disk and can arrive corrupted or hand-edited, and an out-of-range value cast straight to an enum is
	// a value no switch has a case for, so it falls back to the field's own default instead.
	template <typename TEnum>
	TEnum ReadEnum(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32 NumValues, TEnum Fallback)
	{
		int32 Value = 0;
		if (!Object.IsValid() || !Object->TryGetNumberField(Key, Value))
		{
			return Fallback;
		}
		return (Value >= 0 && Value < NumValues) ? static_cast<TEnum>(Value) : Fallback;
	}

	FString ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Key, Value);
		}
		return Value;
	}

	int32 ReadInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32 Fallback)
	{
		int32 Value = 0;
		if (Object.IsValid() && Object->TryGetNumberField(Key, Value))
		{
			return Value;
		}
		return Fallback;
	}

	bool ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, bool bFallback)
	{
		bool bValue = false;
		if (Object.IsValid() && Object->TryGetBoolField(Key, bValue))
		{
			return bValue;
		}
		return bFallback;
	}

	// Every field is written unconditionally and in one fixed order, so the text is a pure function of the value.
	// An "omit it when it is empty" shortcut would make two different surfaces able to produce one text, and would
	// make the bytes depend on which fields happened to be filled in.
	void WriteOperand(const TSharedRef<FSurfaceWriter>& Writer, const TCHAR* Key, const FCrowdyEffectOperand& Operand)
	{
		Writer->WriteObjectStart(Key);
		Writer->WriteValue(TEXT("k"), static_cast<int32>(Operand.Kind));
		Writer->WriteValue(TEXT("lit"), Operand.Literal);
		Writer->WriteValue(TEXT("role"), static_cast<int32>(Operand.Role));
		Writer->WriteValue(TEXT("name"), Operand.Name);
		Writer->WriteValue(TEXT("ifc"), Operand.If.Condition);
		Writer->WriteValue(TEXT("ift"), Operand.If.Then);
		Writer->WriteValue(TEXT("ife"), Operand.If.Else);
		Writer->WriteValue(TEXT("callee"), Operand.Call.Callee);
		Writer->WriteArrayStart(TEXT("args"));
		for (const FString& Arg : Operand.Call.Args)
		{
			Writer->WriteValue(Arg);
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
	}

	FCrowdyEffectOperand ReadOperand(const TSharedPtr<FJsonObject>& Object)
	{
		FCrowdyEffectOperand Operand;
		if (!Object.IsValid())
		{
			return Operand;
		}
		Operand.Kind = ReadEnum(Object, TEXT("k"), 7, ECrowdyEffectOperandKind::Number);
		Operand.Literal = ReadString(Object, TEXT("lit"));
		Operand.Role = ReadEnum(Object, TEXT("role"), 2, ECrowdyEffectRole::Source);
		Operand.Name = ReadString(Object, TEXT("name"));
		Operand.If.Condition = ReadString(Object, TEXT("ifc"));
		Operand.If.Then = ReadString(Object, TEXT("ift"));
		Operand.If.Else = ReadString(Object, TEXT("ife"));
		Operand.Call.Callee = ReadString(Object, TEXT("callee"));

		const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
		if (Object->TryGetArrayField(TEXT("args"), Args) && Args)
		{
			Operand.Call.Args.Reserve(Args->Num());
			for (const TSharedPtr<FJsonValue>& Arg : *Args)
			{
				FString Text;
				if (Arg.IsValid() && Arg->TryGetString(Text))
				{
					Operand.Call.Args.Add(MoveTemp(Text));
				}
			}
		}
		return Operand;
	}

	const TSharedPtr<FJsonObject>* GetObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Object.IsValid() && Object->TryGetObjectField(Key, Found))
		{
			return Found;
		}
		return nullptr;
	}

	void WriteSpec(const TSharedRef<FSurfaceWriter>& Writer, const FCrowdyEffectSpec& Spec)
	{
		Writer->WriteObjectStart(TEXT("spec"));

		// Assignment order is semantic (a later write sees the earlier ones), so this list keeps its authored order
		// rather than being sorted into a canonical one.
		Writer->WriteArrayStart(TEXT("asg"));
		for (const FCrowdyEffectAssignmentSpec& Assignment : Spec.Assignments)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("role"), static_cast<int32>(Assignment.TargetRole));
			Writer->WriteValue(TEXT("attr"), Assignment.Attribute);
			Writer->WriteValue(TEXT("op"), static_cast<int32>(Assignment.Operator));
			Writer->WriteArrayStart(TEXT("val"));
			for (const FCrowdyEffectTerm& Term : Assignment.Value)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("op"), static_cast<int32>(Term.Op));
				WriteOperand(Writer, TEXT("od"), Term.Operand);
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();

		Writer->WriteArrayStart(TEXT("req"));
		for (const FCrowdyEffectRequireSpec& Require : Spec.Requires)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("kind"), static_cast<int32>(Require.Kind));
			Writer->WriteValue(TEXT("kw"), static_cast<int32>(Require.Keyword));
			WriteOperand(Writer, TEXT("l"), Require.Left);
			Writer->WriteValue(TEXT("cmp"), static_cast<int32>(Require.Comparator));
			WriteOperand(Writer, TEXT("r"), Require.Right);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();

		Writer->WriteValue(TEXT("hasRet"), Spec.bHasReturn);
		WriteOperand(Writer, TEXT("ret"), Spec.Return);
		Writer->WriteObjectEnd();
	}

	FCrowdyEffectSpec ReadSpec(const TSharedPtr<FJsonObject>& Object)
	{
		FCrowdyEffectSpec Spec;
		if (!Object.IsValid())
		{
			return Spec;
		}

		const TArray<TSharedPtr<FJsonValue>>* Assignments = nullptr;
		if (Object->TryGetArrayField(TEXT("asg"), Assignments) && Assignments)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Assignments)
			{
				const TSharedPtr<FJsonObject>* EntryObject = nullptr;
				if (!Entry.IsValid() || !Entry->TryGetObject(EntryObject) || !EntryObject)
				{
					continue;
				}
				FCrowdyEffectAssignmentSpec Assignment;
				Assignment.TargetRole = ReadEnum(*EntryObject, TEXT("role"), 2, ECrowdyEffectRole::Target);
				Assignment.Attribute = ReadString(*EntryObject, TEXT("attr"));
				Assignment.Operator = ReadEnum(*EntryObject, TEXT("op"), 5, ECrowdyEffectAssignmentOp::Subtract);

				const TArray<TSharedPtr<FJsonValue>>* Terms = nullptr;
				if ((*EntryObject)->TryGetArrayField(TEXT("val"), Terms) && Terms)
				{
					for (const TSharedPtr<FJsonValue>& TermValue : *Terms)
					{
						const TSharedPtr<FJsonObject>* TermObject = nullptr;
						if (!TermValue.IsValid() || !TermValue->TryGetObject(TermObject) || !TermObject)
						{
							continue;
						}
						FCrowdyEffectTerm Term;
						Term.Op = ReadEnum(*TermObject, TEXT("op"), 4, ECrowdyEffectBinaryOp::Add);
						if (const TSharedPtr<FJsonObject>* OperandObject = GetObjectField(*TermObject, TEXT("od")))
						{
							Term.Operand = ReadOperand(*OperandObject);
						}
						Assignment.Value.Add(MoveTemp(Term));
					}
				}
				Spec.Assignments.Add(MoveTemp(Assignment));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Requires = nullptr;
		if (Object->TryGetArrayField(TEXT("req"), Requires) && Requires)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Requires)
			{
				const TSharedPtr<FJsonObject>* EntryObject = nullptr;
				if (!Entry.IsValid() || !Entry->TryGetObject(EntryObject) || !EntryObject)
				{
					continue;
				}
				FCrowdyEffectRequireSpec Require;
				Require.Kind = ReadEnum(*EntryObject, TEXT("kind"), 2, ECrowdyEffectRequireKind::Keyword);
				Require.Keyword = ReadEnum(*EntryObject, TEXT("kw"), 5, ECrowdyEffectPolicyKeyword::Owner);
				if (const TSharedPtr<FJsonObject>* Left = GetObjectField(*EntryObject, TEXT("l")))
				{
					Require.Left = ReadOperand(*Left);
				}
				Require.Comparator = ReadEnum(*EntryObject, TEXT("cmp"), 6, ECrowdyEffectComparator::GreaterOrEqual);
				if (const TSharedPtr<FJsonObject>* Right = GetObjectField(*EntryObject, TEXT("r")))
				{
					Require.Right = ReadOperand(*Right);
				}
				Spec.Requires.Add(MoveTemp(Require));
			}
		}

		Spec.bHasReturn = ReadBool(Object, TEXT("hasRet"), false);
		if (const TSharedPtr<FJsonObject>* Return = GetObjectField(Object, TEXT("ret")))
		{
			Spec.Return = ReadOperand(*Return);
		}
		return Spec;
	}

	void WriteAutomation(const TSharedRef<FSurfaceWriter>& Writer, const FCrowdyEffectAutomationAuthoring& Automation)
	{
		Writer->WriteObjectStart(TEXT("auto"));
		Writer->WriteValue(TEXT("run"), Automation.bRunAutomatically);
		Writer->WriteValue(TEXT("en"), Automation.bEnabled);
		Writer->WriteValue(TEXT("trig"), static_cast<int32>(Automation.Trigger));
		Writer->WriteValue(TEXT("ms"), Automation.IntervalMs);
		Writer->WriteValue(TEXT("cron"), Automation.CronExpr);
		Writer->WriteValue(TEXT("propKey"), Automation.ChangePropertyKey);
		Writer->WriteValue(TEXT("ws"), static_cast<int32>(Automation.WriteSource));
		Writer->WriteValue(TEXT("watchFn"), Automation.WatchFunctionName);
		Writer->WriteValue(TEXT("changeType"), Automation.ChangeContainerType);
		Writer->WriteValue(TEXT("debounce"), Automation.DebounceMs);
		Writer->WriteValue(TEXT("tmode"), static_cast<int32>(Automation.TargetMode));
		Writer->WriteValue(TEXT("ttype"), Automation.TargetTypeOverride);
		Writer->WriteValue(TEXT("tcid"), Automation.TargetContainerId);
		Writer->WriteValue(TEXT("name"), Automation.AutomationName);
		Writer->WriteValue(TEXT("maxTargets"), Automation.MaxTargets);
		Writer->WriteValue(TEXT("gas"), Automation.GasLimit);
		Writer->WriteValue(TEXT("timeout"), Automation.RunTimeoutMs);
		Writer->WriteValue(TEXT("rpm"), Automation.MaxRunsPerMinute);
		Writer->WriteValue(TEXT("failures"), Automation.FailureThreshold);
		Writer->WriteValue(TEXT("cooldown"), Automation.CooldownMs);
		Writer->WriteObjectEnd();
	}

	FCrowdyEffectAutomationAuthoring ReadAutomation(const TSharedPtr<FJsonObject>& Object)
	{
		FCrowdyEffectAutomationAuthoring Automation;
		if (!Object.IsValid())
		{
			return Automation;
		}
		Automation.bRunAutomatically = ReadBool(Object, TEXT("run"), Automation.bRunAutomatically);
		Automation.bEnabled = ReadBool(Object, TEXT("en"), Automation.bEnabled);
		Automation.Trigger = ReadEnum(Object, TEXT("trig"), 4, Automation.Trigger);
		Automation.IntervalMs = ReadInt(Object, TEXT("ms"), Automation.IntervalMs);
		Automation.CronExpr = ReadString(Object, TEXT("cron"));
		Automation.ChangePropertyKey = ReadString(Object, TEXT("propKey"));
		Automation.WriteSource = ReadEnum(Object, TEXT("ws"), 3, Automation.WriteSource);
		Automation.WatchFunctionName = ReadString(Object, TEXT("watchFn"));
		Automation.ChangeContainerType = ReadString(Object, TEXT("changeType"));
		Automation.DebounceMs = ReadInt(Object, TEXT("debounce"), Automation.DebounceMs);
		Automation.TargetMode = ReadEnum(Object, TEXT("tmode"), 3, Automation.TargetMode);
		Automation.TargetTypeOverride = ReadString(Object, TEXT("ttype"));
		Automation.TargetContainerId = ReadString(Object, TEXT("tcid"));
		Automation.AutomationName = ReadString(Object, TEXT("name"));
		Automation.MaxTargets = ReadInt(Object, TEXT("maxTargets"), Automation.MaxTargets);
		Automation.GasLimit = ReadInt(Object, TEXT("gas"), Automation.GasLimit);
		Automation.RunTimeoutMs = ReadInt(Object, TEXT("timeout"), Automation.RunTimeoutMs);
		Automation.MaxRunsPerMinute = ReadInt(Object, TEXT("rpm"), Automation.MaxRunsPerMinute);
		Automation.FailureThreshold = ReadInt(Object, TEXT("failures"), Automation.FailureThreshold);
		Automation.CooldownMs = ReadInt(Object, TEXT("cooldown"), Automation.CooldownMs);
		return Automation;
	}

	bool DiagnosticsHaveError(const TArray<FCrowdyEffectDiagnostic>& Diagnostics)
	{
		return Diagnostics.ContainsByPredicate(
			[](const FCrowdyEffectDiagnostic& D) { return D.Severity == ECrowdyEffectSeverity::Error; });
	}
}

FCrowdyEffectAuthoredSurface CrowdyEffectAuthoredSurface::FromEffect(
	const UCrowdyEffect& Effect, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics)
{
	FCrowdyEffectAuthoredSurface Surface = FromEffectSettings(Effect);
	if (Effect.Source == ECrowdyEffectSource::Graph)
	{
#if WITH_EDITORONLY_DATA
		Surface.GraphSpec = CrowdyEffectGraphCompile::CompileGraphToSpec(Effect.EffectGraph, OutDiagnostics);
#else
		OutDiagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0,
			TEXT("this effect authors from a node graph, which is not available in this build") });
#endif
	}
	return Surface;
}

FCrowdyEffectAuthoredSurface CrowdyEffectAuthoredSurface::FromEffectSettings(const UCrowdyEffect& Effect)
{
	FCrowdyEffectAuthoredSurface Surface;
	Surface.EffectiveFunctionName = Effect.GetEffectiveFunctionName();
	Surface.Description = Effect.Description;
	Surface.ContainerClassPath = Effect.ContainerClass.ToString();
	Surface.SourceContainerType = Effect.SourceContainerType.TrimStartAndEnd();
	Surface.Source = Effect.Source;
	Surface.ScriptText = Effect.EffectScript;

	// A magnitude's value type has a typed field and a legacy string behind it, and which one is authoritative
	// depends on whether the magnitude has been through its migration. Resolving it here is what keeps a stored
	// surface saying the same thing the asset does: the legacy fields never travel.
	Surface.Magnitudes.Reserve(Effect.Magnitudes.Num());
	for (const FCrowdyEffectMagnitude& Magnitude : Effect.Magnitudes)
	{
		Surface.Magnitudes.Add({ Magnitude.Name,
			UCrowdyEffect::ValueTypeToWireString(UCrowdyEffect::ResolveMagnitudeValueType(Magnitude)),
			Magnitude.DefaultValueJson, Magnitude.Description });
	}

	Surface.Signals = Effect.Signals;
	Surface.Timers = Effect.Timers;
	Surface.ReturnType = UCrowdyEffect::ReturnTypeToWireString(Effect.ReturnType);

	// The same shape as the magnitude type above: the invoke scope has a legacy flag behind it, folded in once by
	// the load-time migration. An asset that has been loaded already carries the folded value, and this is written
	// while saving, which a load always precedes, so in practice the guard is never the branch taken. It is still
	// applied rather than assumed, because an effect built in memory and never loaded reaches here too, and the
	// answer it gets has to be the one a compile would use.
	const ECrowdyEffectCallableFrom ResolvedCallableFrom = Effect.bInvokeScopeMigrated
		? Effect.CallableFrom
		: UCrowdyEffect::MigrateCallableFrom(Effect.CallableFrom, Effect.bAutonomousOnly, Effect.bRunAutomatically);
	Surface.InvokeScope = UCrowdyEffect::CallableFromToWireString(ResolvedCallableFrom);

	Surface.NotificationCarrier = Effect.NotificationCarrier;
	Surface.Automation = Effect.GetAutomationAuthoring();
	return Surface;
}

FString CrowdyEffectAuthoredSurface::ToJson(const FCrowdyEffectAuthoredSurface& Surface)
{
	FString Json;
	const TSharedRef<FSurfaceWriter> Writer = FSurfaceWriterFactory::Create(&Json);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("fn"), Surface.EffectiveFunctionName);
	Writer->WriteValue(TEXT("desc"), Surface.Description);
	Writer->WriteValue(TEXT("class"), Surface.ContainerClassPath);
	Writer->WriteValue(TEXT("srcType"), Surface.SourceContainerType);
	Writer->WriteValue(TEXT("surf"), static_cast<int32>(Surface.Source));
	Writer->WriteValue(TEXT("script"), Surface.ScriptText);
	WriteSpec(Writer, Surface.GraphSpec);

	// The authored lists below all keep their authored order. Each one's order is part of what the effect means:
	// tuning parameters become the function's parameters in this order, timers are armed in it, and signals are
	// emitted in it. Sorting them would change the compiled function, not just the text.
	Writer->WriteArrayStart(TEXT("mags"));
	for (const FCrowdyEffectParamDecl& Magnitude : Surface.Magnitudes)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("n"), Magnitude.Name);
		Writer->WriteValue(TEXT("t"), Magnitude.ValueType);
		Writer->WriteValue(TEXT("d"), Magnitude.DefaultValueJson);
		Writer->WriteValue(TEXT("desc"), Magnitude.Description);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteArrayStart(TEXT("sigs"));
	for (const FCrowdyEffectSignal& Signal : Surface.Signals)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("n"), Signal.Name);
		Writer->WriteValue(TEXT("desc"), Signal.Description);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteArrayStart(TEXT("timers"));
	for (const FCrowdyEffectTimer& Timer : Surface.Timers)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("fn"), Timer.FunctionName);
		Writer->WriteValue(TEXT("ms"), Timer.DelayMs);
		Writer->WriteValue(TEXT("key"), Timer.DedupeKey);
		Writer->WriteValue(TEXT("tgt"), Timer.Target);
		Writer->WriteValue(TEXT("dexpr"), Timer.DelayExpression);
		Writer->WriteValue(TEXT("kexpr"), Timer.DedupeKeyExpression);
		Writer->WriteArrayStart(TEXT("params"));
		for (const FCrowdyEffectTimerParam& Param : Timer.Params)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("n"), Param.Name);
			Writer->WriteValue(TEXT("e"), Param.Expression);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteValue(TEXT("ret"), Surface.ReturnType);
	Writer->WriteValue(TEXT("scope"), Surface.InvokeScope);
	Writer->WriteValue(TEXT("carrier"), static_cast<int32>(Surface.NotificationCarrier));
	WriteAutomation(Writer, Surface.Automation);
	Writer->WriteObjectEnd();
	Writer->Close();

	return Json;
}

bool CrowdyEffectAuthoredSurface::IsCurrentTagVersion(const FString& VersionText)
{
	int32 Version = 0;
	return LexTryParseString(Version, *VersionText) && Version == FCrowdyEffectAuthoredSurface::TagVersion;
}

bool CrowdyEffectAuthoredSurface::FromJson(const FString& Json, FCrowdyEffectAuthoredSurface& OutSurface)
{
	OutSurface = FCrowdyEffectAuthoredSurface();
	if (Json.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(Json))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	OutSurface.EffectiveFunctionName = ReadString(Root, TEXT("fn"));
	OutSurface.Description = ReadString(Root, TEXT("desc"));
	OutSurface.ContainerClassPath = ReadString(Root, TEXT("class"));
	OutSurface.SourceContainerType = ReadString(Root, TEXT("srcType"));
	OutSurface.Source = ReadEnum(Root, TEXT("surf"), 2, ECrowdyEffectSource::Text);
	OutSurface.ScriptText = ReadString(Root, TEXT("script"));
	if (const TSharedPtr<FJsonObject>* Spec = GetObjectField(Root, TEXT("spec")))
	{
		OutSurface.GraphSpec = ReadSpec(*Spec);
	}

	const TArray<TSharedPtr<FJsonValue>>* Magnitudes = nullptr;
	if (Root->TryGetArrayField(TEXT("mags"), Magnitudes) && Magnitudes)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Magnitudes)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(EntryObject) || !EntryObject)
			{
				continue;
			}
			FCrowdyEffectParamDecl Magnitude;
			Magnitude.Name = ReadString(*EntryObject, TEXT("n"));
			Magnitude.ValueType = ReadString(*EntryObject, TEXT("t"));
			Magnitude.DefaultValueJson = ReadString(*EntryObject, TEXT("d"));
			Magnitude.Description = ReadString(*EntryObject, TEXT("desc"));
			OutSurface.Magnitudes.Add(MoveTemp(Magnitude));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Signals = nullptr;
	if (Root->TryGetArrayField(TEXT("sigs"), Signals) && Signals)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Signals)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(EntryObject) || !EntryObject)
			{
				continue;
			}
			FCrowdyEffectSignal Signal;
			Signal.Name = ReadString(*EntryObject, TEXT("n"));
			Signal.Description = ReadString(*EntryObject, TEXT("desc"));
			OutSurface.Signals.Add(MoveTemp(Signal));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Timers = nullptr;
	if (Root->TryGetArrayField(TEXT("timers"), Timers) && Timers)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *Timers)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(EntryObject) || !EntryObject)
			{
				continue;
			}
			FCrowdyEffectTimer Timer;
			Timer.FunctionName = ReadString(*EntryObject, TEXT("fn"));
			Timer.DelayMs = ReadInt(*EntryObject, TEXT("ms"), Timer.DelayMs);
			Timer.DedupeKey = ReadString(*EntryObject, TEXT("key"));
			Timer.Target = ReadString(*EntryObject, TEXT("tgt"));
			Timer.DelayExpression = ReadString(*EntryObject, TEXT("dexpr"));
			Timer.DedupeKeyExpression = ReadString(*EntryObject, TEXT("kexpr"));

			const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
			if ((*EntryObject)->TryGetArrayField(TEXT("params"), Params) && Params)
			{
				for (const TSharedPtr<FJsonValue>& ParamValue : *Params)
				{
					const TSharedPtr<FJsonObject>* ParamObject = nullptr;
					if (!ParamValue.IsValid() || !ParamValue->TryGetObject(ParamObject) || !ParamObject)
					{
						continue;
					}
					FCrowdyEffectTimerParam Param;
					Param.Name = ReadString(*ParamObject, TEXT("n"));
					Param.Expression = ReadString(*ParamObject, TEXT("e"));
					Timer.Params.Add(MoveTemp(Param));
				}
			}
			OutSurface.Timers.Add(MoveTemp(Timer));
		}
	}

	OutSurface.ReturnType = ReadString(Root, TEXT("ret"));
	OutSurface.InvokeScope = ReadString(Root, TEXT("scope"));
	OutSurface.NotificationCarrier = ReadEnum(Root, TEXT("carrier"), 4, ECrowdyEffectNotificationCarrier::Default);
	if (const TSharedPtr<FJsonObject>* Automation = GetObjectField(Root, TEXT("auto")))
	{
		OutSurface.Automation = ReadAutomation(*Automation);
	}
	return true;
}

bool CrowdyEffectAuthoredSurface::BuildTagPayload(const UCrowdyEffect& Effect, FString& OutPayload)
{
	OutPayload.Reset();

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	const FCrowdyEffectAuthoredSurface Surface = FromEffect(Effect, Diagnostics);
	if (DiagnosticsHaveError(Diagnostics))
	{
		// Nothing here is worth recording. A reader that finds no payload loads the asset, which is the right
		// answer, and the next clean save replaces it. Writing the failure down instead would keep the effect
		// broken after whatever it was waiting on arrived, and would do so for every process that reads the
		// package rather than only the one that saved it.
		return false;
	}

	OutPayload = ToJson(Surface);
	return !OutPayload.IsEmpty();
}

UClass* CrowdyEffectAuthoredSurface::ResolveTargetClass(const FCrowdyEffectAuthoredSurface& Surface)
{
	if (Surface.ContainerClassPath.IsEmpty())
	{
		return nullptr;
	}
	return TSoftClassPtr<UObject>(FSoftObjectPath(Surface.ContainerClassPath)).LoadSynchronous();
}

ECrowdyModelNotificationCarrier CrowdyEffectAuthoredSurface::ResolveCarrier(const FCrowdyEffectAuthoredSurface& Surface)
{
	ECrowdyEffectNotificationCarrier ProjectCarrier = ECrowdyEffectNotificationCarrier::Channel;
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		ProjectCarrier = Settings->DefaultModelNotificationCarrier;
	}
	return UCrowdyEffect::ResolveCarrier(Surface.NotificationCarrier, ProjectCarrier);
}

FCrowdyEffectVocabulary CrowdyEffectAuthoredSurface::ResolveVocabularyFromClasses(
	const UClass* TargetClass, const FString& TargetTypeName, const FString& DeclaredSourceType)
{
	FCrowdyEffectVocabulary Vocabulary;
	Vocabulary.TargetTypeName = TargetTypeName;
	Vocabulary.TargetAttributes = FCrowdyAttributeRegistry::DiscoverForClass(TargetClass);

	// Declaring no source type leaves both source fields empty, which the lowering reads as "the source is another
	// container of the target's type": the only behaviour there was before source types existed, and it pays for no
	// extra lookup.
	const FString SourceType = DeclaredSourceType.TrimStartAndEnd();
	if (SourceType.IsEmpty())
	{
		return Vocabulary;
	}

	Vocabulary.SourceTypeName = SourceType;
	if (SourceType.Equals(TargetTypeName, ESearchCase::CaseSensitive))
	{
		// Naming the target's own type means what leaving it empty means; reusing the list already discovered
		// answers it without a second scan.
		Vocabulary.SourceAttributes = Vocabulary.TargetAttributes;
	}
	else if (const UClass* SourceClass = FCrowdyAttributeRegistry::FindContainerClassByTypeName(SourceType))
	{
		Vocabulary.SourceAttributes = FCrowdyAttributeRegistry::DiscoverForClass(SourceClass);
	}
	else
	{
		// A source type with no class behind it. The lowering reports it rather than checking the source's reads
		// against the target's attributes, which would accept names the source does not have. A source type that
		// exists but declares no attributes of its own is a different case: it stays resolved, and every source
		// read is checked against its empty vocabulary.
		Vocabulary.bSourceTypeUnresolved = true;
	}
	return Vocabulary;
}

FCrowdyEffectVocabulary CrowdyEffectAuthoredSurface::ResolveVocabularyFromTypes(
	const TArray<FCrowdyEffectVocabularyType>& Types, const FString& TargetTypeName,
	const FString& DeclaredSourceType)
{
	auto FindType = [&Types](const FString& Name) -> const FCrowdyEffectVocabularyType*
	{
		return Types.FindByPredicate([&Name](const FCrowdyEffectVocabularyType& Candidate)
		{
			return Candidate.TypeName.Equals(Name, ESearchCase::CaseSensitive);
		});
	};

	FCrowdyEffectVocabulary Vocabulary;
	Vocabulary.TargetTypeName = TargetTypeName;
	if (const FCrowdyEffectVocabularyType* Target = FindType(TargetTypeName))
	{
		Vocabulary.TargetAttributes = Target->Attributes;
	}

	const FString SourceType = DeclaredSourceType.TrimStartAndEnd();
	if (SourceType.IsEmpty())
	{
		return Vocabulary;
	}

	Vocabulary.SourceTypeName = SourceType;
	if (SourceType.Equals(TargetTypeName, ESearchCase::CaseSensitive))
	{
		Vocabulary.SourceAttributes = Vocabulary.TargetAttributes;
	}
	else if (const FCrowdyEffectVocabularyType* Source = FindType(SourceType))
	{
		Vocabulary.SourceAttributes = Source->Attributes;
	}
	else
	{
		Vocabulary.bSourceTypeUnresolved = true;
	}
	return Vocabulary;
}

FCrowdyEffectLoweringContext CrowdyEffectAuthoredSurface::BuildLoweringContext(
	const FCrowdyEffectAuthoredSurface& Surface, const FCrowdyEffectVocabulary& Vocabulary,
	ECrowdyModelNotificationCarrier Carrier, ECrowdyEffectFnCatalog FnCatalog)
{
	FCrowdyEffectLoweringContext Ctx;
	Ctx.FunctionName = Surface.EffectiveFunctionName;
	Ctx.ContainerTypeName = Vocabulary.TargetTypeName;
	Ctx.Description = Surface.Description;
	Ctx.NotificationCarrier = Carrier;
	Ctx.InvokeScope = Surface.InvokeScope;
	Ctx.bAutonomousInvocable = Surface.Automation.bRunAutomatically;
	Ctx.ReturnType = Surface.ReturnType;

	switch (Surface.Source)
	{
	case ECrowdyEffectSource::Graph:
		Ctx.ReturnAuthoringHint =
			TEXT("tick 'Returns a value' on the Result node and wire the value into its Return pin");
		break;
	default:
		Ctx.ReturnAuthoringHint = TEXT("write a 'return <expression>' line");
		break;
	}

	Ctx.Attributes = Vocabulary.TargetAttributes;
	Ctx.SourceContainerTypeName = Vocabulary.SourceTypeName;
	Ctx.SourceAttributes = Vocabulary.SourceAttributes;
	Ctx.bSourceContainerTypeUnresolved = Vocabulary.bSourceTypeUnresolved;
	Ctx.Magnitudes = Surface.Magnitudes;

	// Left unset when no catalog is registered, which is what tells the lowering it knows nothing about fn: callees
	// and must stay quiet about them rather than calling every one of them unknown. A caller that passes None gets
	// exactly that same silence on purpose: the catalog only ever adds advisory warnings, and answering a lookup can
	// force a project-wide asset load, so a caller that discards warnings must not be the one that triggers it.
	if (FnCatalog == ECrowdyEffectFnCatalog::Project && CrowdyEffectFunctionCatalog::IsCatalogAvailable())
	{
		// Answering can force a project-wide asset sweep, and this context is rebuilt every time the live script
		// editor repaints, so the list is fetched on the first call and cached: an effect with no fn: call at all
		// never pays for it.
		TSharedRef<FCrowdyEffectFnCalleeCache> Cache = MakeShared<FCrowdyEffectFnCalleeCache>();
		const FString TypeName = Vocabulary.TargetTypeName;
		Ctx.FnCalleeLookup = [Cache, TypeName](const FString& Name, FCrowdyEffectFnCallee& OutCallee) -> bool
		{
			if (!Cache->bLoaded)
			{
				// Scoped to this effect's own container type, because the server scopes a model function the same
				// way: the same function name on two container types is two distinct functions.
				Cache->Functions = CrowdyEffectFunctionCatalog::FindFunctionsOnContainerType(TypeName);
				Cache->bLoaded = true;
			}

			for (const CrowdyEffectFunctionCatalog::FDeclaredFunction& Declared : Cache->Functions)
			{
				if (!Declared.FunctionName.Equals(Name, ESearchCase::CaseSensitive))
				{
					continue;
				}
				OutCallee.FunctionName = Declared.FunctionName;
				OutCallee.bAuthorsReturn = Declared.bAuthorsReturn;
				OutCallee.ReturnType = Declared.ReturnType;
				OutCallee.InvokeScope = Declared.InvokeScope;
				OutCallee.bHasMutations = Declared.bHasMutations;
				return true;
			}
			return false;
		};
	}

	return Ctx;
}

FCrowdyEffectLoweringResult CrowdyEffectAuthoredSurface::Compile(
	const FCrowdyEffectAuthoredSurface& Surface, const FCrowdyEffectVocabulary& Vocabulary,
	ECrowdyModelNotificationCarrier Carrier, ECrowdyEffectFnCatalog FnCatalog,
	const TArray<FCrowdyEffectDiagnostic>& PriorDiagnostics)
{
	FCrowdyEffectLoweringResult Result;
	Result.Diagnostics = PriorDiagnostics;

	// Produce the shared AST from whichever authoring surface is selected; the text and graph forms both lower
	// identically through the one FCrowdyEffectLowering core. The graph arrives already compiled to its spec, and
	// reuses FCrowdyEffectSpecBuilder to reach the same AST the text parser would produce.
	FCrowdyEffectProgram Program;
	if (Surface.Source == ECrowdyEffectSource::Graph)
	{
		Program = FCrowdyEffectSpecBuilder::BuildProgram(Surface.GraphSpec, Result.Diagnostics);
	}
	else
	{
		FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(Surface.ScriptText);
		Result.Diagnostics.Append(Parsed.Diagnostics);
		Program = MoveTemp(Parsed.Program);
	}
	if (Result.HasErrors())
	{
		return Result;
	}

	const FCrowdyEffectLoweringContext Ctx = BuildLoweringContext(Surface, Vocabulary, Carrier, FnCatalog);

	FCrowdyEffectLoweringResult Lowered = FCrowdyEffectLowering::Lower(Program, Ctx);
	Result.Function = MoveTemp(Lowered.Function);
	Result.Diagnostics.Append(Lowered.Diagnostics);
	Result.bSourceReferenced = Lowered.bSourceReferenced;

	// Signals, like timers, are declarative and need no AST, so they are appended after lowering. They ride the same
	// notifications array as the model-changed carrier, so an effect that both writes state and signals produces one
	// function carrying both.
	TSet<FString> SeenSignalNames;
	for (const FCrowdyEffectSignal& Signal : Surface.Signals)
	{
		const FString SignalName = Signal.Name.TrimStartAndEnd();
		if (!UCrowdyEffect::IsValidSignalName(SignalName))
		{
			Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, FString::Printf(
				TEXT("the signal name '%s' is not usable; use letters, digits and underscores only, not starting with "
				"a digit, since the name picks the handler function"), *SignalName) });
			continue;
		}
		bool bAlreadySeen = false;
		SeenSignalNames.Add(SignalName, &bAlreadySeen);
		if (bAlreadySeen)
		{
			// Two identical signals would author two identical notifications, so every handler would run twice per
			// invocation.
			Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, FString::Printf(
				TEXT("the signal '%s' is declared more than once on this effect; each would fire the handler again"),
				*SignalName) });
			continue;
		}
		Result.Function.Notifications.Add(UCrowdyEffect::BuildSignalNotification(SignalName));
	}

	// Timers are mapped here rather than inside the lowering because they are declarative: they reference a function
	// by name and carry their own expressions, so nothing about them depends on the effect body's AST.
	if (Surface.Timers.Num() > UCrowdyEffect::MaxTimersPerEffect)
	{
		Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, FString::Printf(
			TEXT("this effect declares %d timers, but a Model function may declare at most %d; the server rejects the "
			"upsert above that limit"), Surface.Timers.Num(), UCrowdyEffect::MaxTimersPerEffect) });
	}
	for (const FCrowdyEffectTimer& Timer : Surface.Timers)
	{
		if (Timer.FunctionName.TrimStartAndEnd().IsEmpty())
		{
			Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0,
				TEXT("a timer has no function name; name the Model function it should run when it fires") });
			continue;
		}
		TSet<FString> SeenParamNames;
		for (const FCrowdyEffectTimerParam& Param : Timer.Params)
		{
			const FString ParamName = Param.Name.TrimStartAndEnd();
			if (ParamName.IsEmpty())
			{
				// The row is dropped rather than emitted as a nameless parameter, so an author who filled in an
				// expression and left the name blank would otherwise get silence and a value that never arrives.
				if (!Param.Expression.TrimStartAndEnd().IsEmpty())
				{
					Result.Diagnostics.Add({ ECrowdyEffectSeverity::Warning, 0, 0,
						TEXT("a timer parameter has an expression but no name, so it will not be sent; name it or remove the row") });
				}
				continue;
			}
			if (FCrowdyEffectLowering::IsReservedParamName(ParamName))
			{
				Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, FString::Printf(
					TEXT("the timer parameter name '%s' is reserved by the effect layer and cannot be declared"),
					*ParamName) });
			}
			// Two rows claiming one name send two values the fired function reads under a single $name, and the
			// schema diff matches a timer's parameters by name, so a duplicate can also make a real difference read
			// as no change at all.
			if (SeenParamNames.Contains(ParamName))
			{
				Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, FString::Printf(
					TEXT("the timer parameter name '%s' is declared twice on one timer; give each row its own name"),
					*ParamName) });
			}
			SeenParamNames.Add(ParamName);
		}
		Result.Function.Timers.Add(UCrowdyEffect::BuildTimerInput(Timer));
	}

	if (Surface.Automation.bRunAutomatically)
	{
		TOptional<FCrowdyGameModelAutomationTriggerInput> Trigger;
		Result.Automation = UCrowdyEffect::BuildAutomationInput(
			Surface.Automation, Ctx.FunctionName, Vocabulary.TargetTypeName, Trigger);
		Result.Trigger = Trigger;
	}

	return Result;
}

FCrowdyEffectLoweringResult CrowdyEffectAuthoredSurface::CompileResolved(
	const FCrowdyEffectAuthoredSurface& Surface, ECrowdyEffectFnCatalog FnCatalog,
	const TArray<FCrowdyEffectDiagnostic>& PriorDiagnostics, FString& OutTargetTypeName)
{
	OutTargetTypeName.Reset();

	// Both checks below answer before anything the surface says is reported, because without a container type there
	// is no vocabulary to judge the body against: reporting the body's own problems on top would bury the one thing
	// the author has to fix first.
	UClass* Class = ResolveTargetClass(Surface);
	if (!Class)
	{
		FCrowdyEffectLoweringResult Result;
		Result.Diagnostics.Add(
			{ ECrowdyEffectSeverity::Error, 0, 0, TEXT("the effect has no target container class set") });
		return Result;
	}

	FString TypeName;
	if (!FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName))
	{
		FCrowdyEffectLoweringResult Result;
		Result.Diagnostics.Add({ ECrowdyEffectSeverity::Error, 0, 0, FString::Printf(
			TEXT("the target class '%s' carries no CrowdyContainer tag"), *Class->GetName()) });
		return Result;
	}

	OutTargetTypeName = TypeName;
	const FCrowdyEffectVocabulary Vocabulary =
		ResolveVocabularyFromClasses(Class, TypeName, Surface.SourceContainerType);
	return Compile(Surface, Vocabulary, ResolveCarrier(Surface), FnCatalog, PriorDiagnostics);
}
