// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyEffect.h"

#include "Dom/JsonObject.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"
#include "Replication/GameModel/Effect/CrowdyEffectDslEscape.h"
#include "Replication/GameModel/Effect/CrowdyEffectGraphCompileHook.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#if WITH_EDITORONLY_DATA
#include "EdGraph/EdGraph.h"
#endif

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/Package.h"
#endif

FString UCrowdyEffect::GetEffectiveFunctionName() const
{
	return FunctionName.IsEmpty() ? GetName() : FunctionName;
}

FString UCrowdyEffect::GetContainerTypeName() const
{
	FString TypeName;
	if (UClass* Class = ContainerClass.LoadSynchronous())
	{
		FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName);
	}
	return TypeName;
}

UClass* UCrowdyEffect::ResolveSourceContainerClass() const
{
	return FCrowdyAttributeRegistry::FindContainerClassByTypeName(SourceContainerType.TrimStartAndEnd());
}

namespace
{
	CrowdyEffectDuplicateFunctionCheck::FSweepHook GDuplicateFunctionSweepHook;
	CrowdyEffectFunctionCatalog::FHook GFunctionCatalogHook;
}

void CrowdyEffectDuplicateFunctionCheck::SetSweepHook(FSweepHook Hook)
{
	GDuplicateFunctionSweepHook = MoveTemp(Hook);
}

TArray<CrowdyEffectDuplicateFunctionCheck::FConflict> CrowdyEffectDuplicateFunctionCheck::FindOtherEffectsWithFunctionName(
	const FString& AssetPath, const FString& ContainerTypeName, const FString& FunctionName)
{
	if (GDuplicateFunctionSweepHook)
	{
		return GDuplicateFunctionSweepHook(AssetPath, ContainerTypeName, FunctionName);
	}
	return {};
}

void CrowdyEffectFunctionCatalog::SetCatalogHook(FHook Hook)
{
	GFunctionCatalogHook = MoveTemp(Hook);
}

bool CrowdyEffectFunctionCatalog::IsCatalogAvailable()
{
	return static_cast<bool>(GFunctionCatalogHook);
}

TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction> CrowdyEffectFunctionCatalog::FindFunctionsOnContainerType(
	const FString& ContainerTypeName)
{
	if (GFunctionCatalogHook)
	{
		return GFunctionCatalogHook(ContainerTypeName);
	}
	return {};
}

FCrowdyEffectAutomationAuthoring UCrowdyEffect::GetAutomationAuthoring() const
{
	FCrowdyEffectAutomationAuthoring Authoring;
	Authoring.bRunAutomatically = bRunAutomatically;
	Authoring.bEnabled = bEnabled;
	Authoring.Trigger = AutomationTrigger;
	Authoring.IntervalMs = AutomationIntervalMs;
	Authoring.CronExpr = AutomationCronExpr;
	Authoring.ChangePropertyKey = AutomationChangePropertyKey;
	Authoring.WriteSource = AutomationWriteSource;
	Authoring.WatchFunctionName = AutomationWatchFunctionName;
	Authoring.ChangeContainerType = AutomationChangeContainerType;
	Authoring.DebounceMs = AutomationDebounceMs;
	Authoring.TargetMode = AutomationTargetMode;
	Authoring.TargetTypeOverride = AutomationTargetTypeOverride;
	Authoring.TargetContainerId = AutomationTargetContainerId;
	Authoring.AutomationName = AutomationName;
	Authoring.MaxTargets = AutomationMaxTargets;
	Authoring.GasLimit = AutomationGasLimit;
	Authoring.RunTimeoutMs = AutomationRunTimeoutMs;
	Authoring.MaxRunsPerMinute = AutomationMaxRunsPerMinute;
	Authoring.FailureThreshold = AutomationFailureThreshold;
	Authoring.CooldownMs = AutomationCooldownMs;
	return Authoring;
}

FCrowdyGameModelAutomationInput UCrowdyEffect::BuildAutomationInput(const FCrowdyEffectAutomationAuthoring& Authoring,
	const FString& FunctionName, const FString& ContainerTypeName,
	TOptional<FCrowdyGameModelAutomationTriggerInput>& OutTrigger)
{
	OutTrigger.Reset();

	FCrowdyGameModelAutomationInput Out;
	Out.Name = Authoring.AutomationName.IsEmpty() ? FunctionName : Authoring.AutomationName;
	Out.bEnabled = Authoring.bEnabled;
	Out.ActionKind = TEXT("model_function");
	Out.FunctionName = FunctionName;

	switch (Authoring.TargetMode)
	{
	case ECrowdyEffectAutomationTargetMode::Container:
		Out.TargetMode = TEXT("container");
		Out.SelfContainerId = Authoring.TargetContainerId;
		// The automation's model-driven notification names its container via the server-injected $self_container_id,
		// which the server evaluates against each run's target - so a Container-mode run needs no baked params, and a
		// Type-mode fan-out names each container correctly with the same authored notification.
		break;
	case ECrowdyEffectAutomationTargetMode::Global:
		Out.TargetMode = TEXT("global");
		break;
	case ECrowdyEffectAutomationTargetMode::Type:
	default:
		Out.TargetMode = TEXT("type");
		Out.TargetTypeName = Authoring.TargetTypeOverride.IsEmpty() ? ContainerTypeName : Authoring.TargetTypeOverride;
		break;
	}

	switch (Authoring.Trigger)
	{
	case ECrowdyEffectAutomationTrigger::Cron:
		Out.TriggerType = TEXT("schedule");
		Out.ScheduleKind = TEXT("cron");
		Out.CronExpr = Authoring.CronExpr;
		break;
	case ECrowdyEffectAutomationTrigger::OnPropertyChange:
	{
		Out.TriggerType = TEXT("event");
		FCrowdyGameModelAutomationTriggerInput Trigger;
		Trigger.AutomationName = Out.Name;
		Trigger.OnEvent = TEXT("property_changed");
		Trigger.PropertyKey = Authoring.ChangePropertyKey;
		Trigger.WriteSource = WriteSourceToWireString(Authoring.WriteSource);
		// An unset filter watches the effect's own container type: an effect normally reacts to a property on the
		// kind of container it is authored against.
		Trigger.ContainerTypeName =
			Authoring.ChangeContainerType.IsEmpty() ? ContainerTypeName : Authoring.ChangeContainerType;
		Trigger.DebounceMs = Authoring.DebounceMs;
		OutTrigger = Trigger;
		break;
	}
	case ECrowdyEffectAutomationTrigger::OnFunctionInvoked:
	{
		Out.TriggerType = TEXT("event");
		FCrowdyGameModelAutomationTriggerInput Trigger;
		Trigger.AutomationName = Out.Name;
		Trigger.OnEvent = TEXT("function_invoked");
		Trigger.FunctionName = Authoring.WatchFunctionName;
		// Unlike a property-change filter, this names the container the WATCHED function runs on, which is usually a
		// different type from the one this effect is authored against. Defaulting it to the effect's own type would
		// author "fires when that function runs on my own type" and silently never match the ordinary cross-type
		// case, so an unset filter is left empty and matches every type.
		Trigger.ContainerTypeName = Authoring.ChangeContainerType;
		Trigger.DebounceMs = Authoring.DebounceMs;
		OutTrigger = Trigger;
		break;
	}
	case ECrowdyEffectAutomationTrigger::EveryInterval:
	default:
		Out.TriggerType = TEXT("schedule");
		Out.ScheduleKind = TEXT("interval");
		Out.IntervalMs = Authoring.IntervalMs;
		break;
	}

	Out.MaxTargets = Authoring.MaxTargets;
	Out.GasLimit = Authoring.GasLimit;
	Out.RunTimeoutMs = Authoring.RunTimeoutMs;
	Out.MaxRunsPerMinute = Authoring.MaxRunsPerMinute;
	Out.FailureThreshold = Authoring.FailureThreshold;
	Out.CooldownMs = Authoring.CooldownMs;
	return Out;
}

FString UCrowdyEffect::ValueTypeToWireString(ECrowdyEffectValueType ValueType)
{
	switch (ValueType)
	{
	case ECrowdyEffectValueType::Float:        return TEXT("float");
	case ECrowdyEffectValueType::Bool:         return TEXT("bool");
	case ECrowdyEffectValueType::String:       return TEXT("string");
	case ECrowdyEffectValueType::ContainerRef: return TEXT("container_ref");
	case ECrowdyEffectValueType::Int:
	default:                                    return TEXT("int");
	}
}

FString UCrowdyEffect::CallableFromToWireString(ECrowdyEffectCallableFrom CallableFrom)
{
	switch (CallableFrom)
	{
	case ECrowdyEffectCallableFrom::ServerOnly:       return TEXT("server");
	case ECrowdyEffectCallableFrom::OtherEffectsOnly: return TEXT("internal");
	case ECrowdyEffectCallableFrom::Players:
	default:                                          return TEXT("player");
	}
}

FString UCrowdyEffect::ReturnTypeToWireString(ECrowdyEffectReturnType ReturnType)
{
	switch (ReturnType)
	{
	case ECrowdyEffectReturnType::Int:    return TEXT("int");
	case ECrowdyEffectReturnType::Float:  return TEXT("float");
	case ECrowdyEffectReturnType::Bool:   return TEXT("bool");
	case ECrowdyEffectReturnType::String: return TEXT("string");
	case ECrowdyEffectReturnType::None:
	default:                              return FString();
	}
}

ECrowdyEffectCallableFrom UCrowdyEffect::MigrateCallableFrom(ECrowdyEffectCallableFrom Current,
	bool bLegacyAutonomousOnly, bool bLegacyRunAutomatically)
{
	// The legacy flag only ever reached the invoke scope from inside the "runs automatically" branch, and the panel
	// hid it otherwise, so an effect with the flag set but automation off is one players can invoke today. Honouring
	// the flag on its own would close it, which is a behaviour change on an asset nobody edited.
	if (bLegacyAutonomousOnly && bLegacyRunAutomatically)
	{
		return ECrowdyEffectCallableFrom::ServerOnly;
	}
	return Current;
}

bool UCrowdyEffect::IsValidSignalName(const FString& Name)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return false;
	}
	if (FChar::IsDigit(Trimmed[0]))
	{
		return false;
	}
	for (const TCHAR Char : Trimmed)
	{
		if (!FChar::IsAlnum(Char) && Char != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

FName UCrowdyEffect::MakeSignalHandlerName(const FString& SignalName)
{
	return FName(*(FString(CrowdyGameModelMetaKeys::SignalHandlerPrefix) + SignalName.TrimStartAndEnd()));
}

FCrowdyGameModelNotification UCrowdyEffect::BuildSignalNotification(const FString& SignalName)
{
	FCrowdyGameModelNotification Notif;
	Notif.Kind = TEXT("channel");

	FCrowdyGameModelNotificationArg Payload;
	Payload.Name = TEXT("payload");
	// The name is embedded in the literal rather than passed as a parameter: a signal carries no caller-supplied
	// data, and baking it in keeps the notification self-describing for an automation or timer fire with no caller
	// at all. $self_container_id is already a string, so no cast is needed around it.
	Payload.Expression = FString::Printf(TEXT("concat(\"%s%s:\", $%s)"),
		CrowdyGameModelMetaKeys::SignalChannelPrefix,
		*SignalName.TrimStartAndEnd(),
		CrowdyGameModelMetaKeys::SelfContainerIdParam);
	Notif.Args.Add(MoveTemp(Payload));
	return Notif;
}

FCrowdyGameModelTimer UCrowdyEffect::BuildTimerInput(const FCrowdyEffectTimer& Authored)
{
	FCrowdyGameModelTimer Out;
	Out.FunctionName = Authored.FunctionName.TrimStartAndEnd();
	Out.Target = Authored.Target.TrimStartAndEnd();

	// Both fields are expression source. The plain forms are converted here rather than trusted as typed, because a
	// bare word in either position parses as an identifier server-side and fails the upsert.
	const FString DelayOverride = Authored.DelayExpression.TrimStartAndEnd();
	Out.DelayMsExpression = DelayOverride.IsEmpty() ? FString::FromInt(Authored.DelayMs) : DelayOverride;

	const FString DedupeOverride = Authored.DedupeKeyExpression.TrimStartAndEnd();
	if (!DedupeOverride.IsEmpty())
	{
		Out.DedupeKeyExpression = DedupeOverride;
	}
	else
	{
		const FString PlainKey = Authored.DedupeKey.TrimStartAndEnd();
		if (!PlainKey.IsEmpty())
		{
			Out.DedupeKeyExpression = FString(TEXT("\"")) + DslStringEscape(PlainKey) + TEXT("\"");
		}
	}

	// Each row is expression source through and through, exactly like Delay Expression and Dedupe Key Expression
	// above: no quoting is added here. A row with no name carries nothing to bind the value to, so it is dropped
	// rather than emitted as a nameless parameter.
	Out.Params.Reserve(Authored.Params.Num());
	for (const FCrowdyEffectTimerParam& Param : Authored.Params)
	{
		const FString TrimmedName = Param.Name.TrimStartAndEnd();
		if (TrimmedName.IsEmpty())
		{
			continue;
		}
		FCrowdyGameModelTimerParam OutParam;
		OutParam.Name = TrimmedName;
		OutParam.Expression = Param.Expression.TrimStartAndEnd();
		Out.Params.Add(MoveTemp(OutParam));
	}
	return Out;
}

FString UCrowdyEffect::WriteSourceToWireString(ECrowdyEffectPropertyWriteSource WriteSource)
{
	switch (WriteSource)
	{
	case ECrowdyEffectPropertyWriteSource::Direct:   return TEXT("direct");
	case ECrowdyEffectPropertyWriteSource::Function: return TEXT("function");
	case ECrowdyEffectPropertyWriteSource::Any:
	default:                                         return TEXT("any");
	}
}

ECrowdyEffectPropertyWriteSource UCrowdyEffect::WireStringToWriteSource(const FString& Wire)
{
	const FString Normalized = Wire.TrimStartAndEnd();
	if (Normalized.Equals(TEXT("direct"), ESearchCase::IgnoreCase))   return ECrowdyEffectPropertyWriteSource::Direct;
	if (Normalized.Equals(TEXT("function"), ESearchCase::IgnoreCase)) return ECrowdyEffectPropertyWriteSource::Function;
	return ECrowdyEffectPropertyWriteSource::Any;
}

ECrowdyEffectValueType UCrowdyEffect::WireStringToValueType(const FString& Wire)
{
	const FString Normalized = Wire.TrimStartAndEnd();
	if (Normalized.Equals(TEXT("float"), ESearchCase::IgnoreCase))         return ECrowdyEffectValueType::Float;
	if (Normalized.Equals(TEXT("bool"), ESearchCase::IgnoreCase))          return ECrowdyEffectValueType::Bool;
	if (Normalized.Equals(TEXT("string"), ESearchCase::IgnoreCase))        return ECrowdyEffectValueType::String;
	if (Normalized.Equals(TEXT("container_ref"), ESearchCase::IgnoreCase)) return ECrowdyEffectValueType::ContainerRef;
	return ECrowdyEffectValueType::Int;
}

void UCrowdyEffect::MigrateMagnitude(FCrowdyEffectMagnitude& Magnitude)
{
	Magnitude.Name = Magnitude.Name.TrimStartAndEnd();
	if (!Magnitude.bTypeMigrated)
	{
		Magnitude.ValueTypeEnum = WireStringToValueType(Magnitude.ValueType);
		Magnitude.bTypeMigrated = true;
	}
	// The enum is authoritative; keep the legacy string mirrored so any stale reader still sees the right type.
	Magnitude.ValueType = ValueTypeToWireString(Magnitude.ValueTypeEnum);
}

ECrowdyEffectValueType UCrowdyEffect::ResolveMagnitudeValueType(const FCrowdyEffectMagnitude& Magnitude)
{
	return Magnitude.bTypeMigrated ? Magnitude.ValueTypeEnum : WireStringToValueType(Magnitude.ValueType);
}

bool UCrowdyEffect::IsNumericMagnitude(const TArray<FCrowdyEffectMagnitude>& Magnitudes, const FString& Name,
	bool& bOutInteger)
{
	bOutInteger = false;
	const FString Wanted = Name.TrimStartAndEnd();
	if (Wanted.IsEmpty())
	{
		return false;
	}
	for (const FCrowdyEffectMagnitude& Magnitude : Magnitudes)
	{
		if (!Magnitude.Name.Equals(Wanted, ESearchCase::CaseSensitive))
		{
			continue;
		}
		const ECrowdyEffectValueType ValueType = ResolveMagnitudeValueType(Magnitude);
		if (ValueType == ECrowdyEffectValueType::Int)
		{
			bOutInteger = true;
			return true;
		}
		return ValueType == ECrowdyEffectValueType::Float;
	}
	return false;
}

bool UCrowdyEffect::TryGetCoalesceSpec(FString& OutAccumulateParam, bool& bOutInteger) const
{
	if (CachedCoalesceKind < 0)
	{
		CachedAccumulateParam.Reset();
		CachedCoalesceKind = 0;
		if (bCoalescable && CoalesceWindowSeconds > 0.0f)
		{
			bool bInteger = false;
			if (IsNumericMagnitude(Magnitudes, AccumulateParam, bInteger))
			{
				CachedAccumulateParam = AccumulateParam.TrimStartAndEnd();
				CachedCoalesceKind = bInteger ? 1 : 2;
			}
		}
	}

	if (CachedCoalesceKind == 0)
	{
		return false;
	}
	OutAccumulateParam = CachedAccumulateParam;
	bOutInteger = (CachedCoalesceKind == 1);
	return true;
}

void UCrowdyEffect::InvalidateCoalesceSpec() const
{
	CachedCoalesceKind = -1;
	CachedAccumulateParam.Reset();
}

void UCrowdyEffect::PostLoad()
{
	Super::PostLoad();
	InvalidateCoalesceSpec();
	for (FCrowdyEffectMagnitude& Magnitude : Magnitudes)
	{
		MigrateMagnitude(Magnitude);
	}
	if (!bInvokeScopeMigrated)
	{
		bInvokeScopeMigrated = true;
		CallableFrom = MigrateCallableFrom(CallableFrom, bAutonomousOnly, bRunAutomatically);
	}
}

ECrowdyModelNotificationCarrier UCrowdyEffect::ResolveCarrier(ECrowdyEffectNotificationCarrier EffectCarrier,
	ECrowdyEffectNotificationCarrier ProjectCarrier)
{
	// An effect-level Default defers to the project; a project-level Default (the field's own default, or a stale
	// value) resolves to Channel, the SDK's position-independent default.
	ECrowdyEffectNotificationCarrier Effective =
		(EffectCarrier == ECrowdyEffectNotificationCarrier::Default) ? ProjectCarrier : EffectCarrier;
	switch (Effective)
	{
	case ECrowdyEffectNotificationCarrier::None:    return ECrowdyModelNotificationCarrier::None;
	case ECrowdyEffectNotificationCarrier::Spatial: return ECrowdyModelNotificationCarrier::Spatial;
	case ECrowdyEffectNotificationCarrier::Channel: return ECrowdyModelNotificationCarrier::Channel;
	default:                                        return ECrowdyModelNotificationCarrier::Channel; // Default -> Channel
	}
}

ECrowdyModelNotificationCarrier UCrowdyEffect::ResolveNotificationCarrier() const
{
	ECrowdyEffectNotificationCarrier ProjectCarrier = ECrowdyEffectNotificationCarrier::Channel;
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		ProjectCarrier = Settings->DefaultModelNotificationCarrier;
	}
	return ResolveCarrier(NotificationCarrier, ProjectCarrier);
}

FCrowdyEffectAuthoredShape UCrowdyEffect::GetAuthoredShape() const
{
	// Produce the program exactly as Compile does for the selected surface, then read both answers off that one
	// program. Asking the surface's own fields instead lets the two drift: a graph effect whose Result node has
	// "Returns a value" ticked but whose Return pin is unwired carries the flag, yet the builder cannot build the
	// operand and drops the return, so it would be published as a callable question that answers nothing.
	//
	// Diagnostics are discarded here: they belong to a real compile, and a surface that cannot build declares
	// nothing a caller can rely on. This must stay cheap - it never loads the container class and never needs the
	// attribute vocabulary, because the editor's project-wide function index calls it once per effect asset in a
	// single sweep. Parsing and building the program need neither.
	TArray<FCrowdyEffectDiagnostic> Ignored;
	FCrowdyEffectProgram Program;
	switch (Source)
	{
	case ECrowdyEffectSource::Graph:
#if WITH_EDITORONLY_DATA
		Program = FCrowdyEffectSpecBuilder::BuildProgram(
			CrowdyEffectGraphCompile::CompileGraphToSpec(EffectGraph, Ignored), Ignored);
#endif
		break;

	default:
		Program = FCrowdyEffectParser::Parse(EffectScript).Program;
		break;
	}

	FCrowdyEffectAuthoredShape Shape;
	Shape.bAuthorsReturn = Program.ReturnExpr.IsValid();

	// Signals and timers are declarative, so they never reach the program, but they are side effects all the same:
	// a fn: call runs none of them either, so an effect that only signals is still an effect a fn: caller loses.
	Shape.bHasMutations = Signals.Num() > 0 || Timers.Num() > 0
		|| Program.Statements.ContainsByPredicate(
			[](const FCrowdyEffectStatement& S) { return S.Kind == ECrowdyEffectStmtKind::Assignment; });
	return Shape;
}

TArray<FCrowdyEffectDiagnostic> UCrowdyEffect::DiagnoseScriptBody(const FString& Body) const
{
	TArray<FCrowdyEffectDiagnostic> Diagnostics;

	UClass* Class = ContainerClass.LoadSynchronous();
	FString TypeName;
	if (!Class || !FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName))
	{
		// Without a container type there is no attribute vocabulary to check against, so the caller keeps whatever
		// the parser alone found rather than being told every attribute is unknown.
		return Diagnostics;
	}

	FCrowdyEffectParseResult Parsed = FCrowdyEffectParser::Parse(Body);
	if (Parsed.HasErrors())
	{
		// The body does not parse yet, which the caller already reports; lowering a half-built program on top would
		// bury that behind a second wave of errors about statements the author has not finished typing.
		return Diagnostics;
	}

	// The very context Compile builds, so a live diagnostic and the real compile can never disagree about the
	// vocabulary they check against. Only the context comes from this asset: the program being checked is the text
	// the author is still typing, not the body the asset has committed, so the settings-only snapshot is enough.
	const FCrowdyEffectAuthoredSurface Surface = CrowdyEffectAuthoredSurface::FromEffectSettings(*this);
	const FCrowdyEffectVocabulary Vocabulary =
		CrowdyEffectAuthoredSurface::ResolveVocabularyFromClasses(Class, TypeName, Surface.SourceContainerType);
	const FCrowdyEffectLoweringContext Ctx = CrowdyEffectAuthoredSurface::BuildLoweringContext(
		Surface, Vocabulary, ResolveNotificationCarrier(), ECrowdyEffectFnCatalog::Project);

	return FCrowdyEffectLowering::Lower(Parsed.Program, Ctx).Diagnostics;
}

FCrowdyEffectLoweringResult UCrowdyEffect::Compile(ECrowdyEffectFnCatalog FnCatalog) const
{
	// Everything the compile reads is snapshotted first, and the snapshot is what is compiled. That is what makes
	// compiling this asset and compiling a stored snapshot of it one operation rather than two implementations that
	// have to be kept in agreement.
	TArray<FCrowdyEffectDiagnostic> SurfaceDiagnostics;
	const FCrowdyEffectAuthoredSurface Surface = CrowdyEffectAuthoredSurface::FromEffect(*this, SurfaceDiagnostics);

	FString TargetTypeName;
	return CrowdyEffectAuthoredSurface::CompileResolved(Surface, FnCatalog, SurfaceDiagnostics, TargetTypeName);
}

#if WITH_EDITOR
EDataValidationResult UCrowdyEffect::IsDataValid(FDataValidationContext& ValidationContext) const
{
	const EDataValidationResult Base = Super::IsDataValid(ValidationContext);

	// The retired "players cannot invoke" flag only ever reached the invoke scope on an effect that also ran
	// automatically, so on any other effect it was set but inert. Migration keeps that behaviour rather than newly
	// closing an effect players can invoke today, which means the author's stated intent was never honoured and
	// still is not. Say so, since Callable From is now the control that would honour it.
	if (bAutonomousOnly && CallableFrom == ECrowdyEffectCallableFrom::Players)
	{
		ValidationContext.AddWarning(FText::FromString(FString::Printf(TEXT(
			"Effect '%s' carries the retired 'players cannot invoke' flag, which never took effect because the effect "
			"does not run automatically. Callable From is set to Players, matching how it has always behaved. Set "
			"Callable From to Server only if players really should not be able to invoke it."), *GetName())));
	}

	// A duplicated asset copies the FunctionName field along with everything else, so two effects can silently
	// author the same server function with asset-registry sweep order (not the designer) deciding which one a
	// later sync keeps. The identity is the (container type, function name) pair, exactly as the server scopes a
	// model function: two container types may each declare a "take_damage" without conflicting. Cross-asset, so it
	// goes through the no-cycle hook: the sweep itself is an editor-only asset-registry lookup that
	// CrowdyReplication (a runtime module) never performs directly. Checked even for a blank, freshly duplicated
	// asset, since that is exactly the moment the collision is introduced.
	bool bDuplicateFunctionNameError = false;
	{
		const FString EffectiveName = GetEffectiveFunctionName();
		const FString TypeName = GetContainerTypeName();
		const TArray<CrowdyEffectDuplicateFunctionCheck::FConflict> Conflicts =
			CrowdyEffectDuplicateFunctionCheck::FindOtherEffectsWithFunctionName(GetPathName(), TypeName, EffectiveName);
		if (!Conflicts.IsEmpty())
		{
			bDuplicateFunctionNameError = true;
			TArray<FString> OtherPaths;
			for (const CrowdyEffectDuplicateFunctionCheck::FConflict& Conflict : Conflicts)
			{
				OtherPaths.Add(Conflict.AssetPath);
			}
			const FString TypeLabel = TypeName.IsEmpty() ? TEXT("(no container type)") : TypeName;
			ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
				"Effect '%s' authors the function name '%s' on container type '%s', already authored by: %s. Rename "
				"one, or point it at a different container type, so each function name is unique per container type; "
				"a schema sync will not upsert either of them until this is fixed."),
				*GetName(), *EffectiveName, *TypeLabel, *FString::Join(OtherPaths, TEXT(", ")))));
		}
	}

	// When the effect runs itself, its trigger fields must be complete: an interval needs a positive period, a cron
	// schedule needs an expression, a property-change trigger needs a property key, a function-invoked trigger needs
	// a watched function name. Validate regardless of the effect body so an automatic effect with an incomplete
	// trigger fails even when the body is still blank.
	bool bAutomationError = false;
	if (bRunAutomatically)
	{
		switch (AutomationTrigger)
		{
		case ECrowdyEffectAutomationTrigger::EveryInterval:
			if (AutomationIntervalMs <= 0)
			{
				bAutomationError = true;
				ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' runs on an interval but Interval (ms) is not positive; set it above 0."), *GetName())));
			}
			break;
		case ECrowdyEffectAutomationTrigger::Cron:
			if (AutomationCronExpr.TrimStartAndEnd().IsEmpty())
			{
				bAutomationError = true;
				ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' uses a cron schedule but its Cron Expression is empty."), *GetName())));
			}
			break;
		case ECrowdyEffectAutomationTrigger::OnPropertyChange:
			if (AutomationChangePropertyKey.TrimStartAndEnd().IsEmpty())
			{
				bAutomationError = true;
				ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' fires on a property change but no On Property Key is set."), *GetName())));
			}
			break;
		case ECrowdyEffectAutomationTrigger::OnFunctionInvoked:
		{
			const FString WatchName = AutomationWatchFunctionName.TrimStartAndEnd();
			if (WatchName.IsEmpty())
			{
				bAutomationError = true;
				ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' fires on a function invocation but no Watch Function Name is set."), *GetName())));
			}
			else if (WatchName.Equals(GetEffectiveFunctionName(), ESearchCase::CaseSensitive))
			{
				// Not an error: the server bounds an invoke-triggers-itself chain by its cascade depth. It is almost
				// always a mistake though, so say so rather than let it burn run budget at runtime.
				ValidationContext.AddWarning(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' watches its own function '%s', so each run re-triggers the automation. The server "
					"bounds the chain by its cascade depth, but name a different function unless that is intended."),
					*GetName(), *WatchName)));
			}
			break;
		}
		}
	}

	// Signal names are checked here as well as at compile, because a signal-only effect has a blank body and the
	// blank case returns below without ever compiling. Without this, a bad signal name on the one kind of effect
	// most likely to have nothing else in it would surface only at dispatch, on a remote machine.
	bool bSignalError = false;
	{
		TSet<FString> SeenNames;
		for (const FCrowdyEffectSignal& Signal : Signals)
		{
			const FString SignalName = Signal.Name.TrimStartAndEnd();
			if (!IsValidSignalName(SignalName))
			{
				bSignalError = true;
				ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' declares the signal name '%s', which cannot be used: the name picks the handler "
					"function, so it must be letters, digits and underscores only and cannot start with a digit."),
					*GetName(), *SignalName)));
				continue;
			}
			bool bAlreadySeen = false;
			SeenNames.Add(SignalName, &bAlreadySeen);
			if (bAlreadySeen)
			{
				bSignalError = true;
				ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
					"Effect '%s' declares the signal '%s' more than once; each copy would call '%s' again on every "
					"listener."), *GetName(), *SignalName, *MakeSignalHandlerName(SignalName).ToString())));
			}
		}
	}

	// Coalescing is checked here rather than only at apply time, because a misconfigured one fails SILENTLY at
	// runtime: the apply path just stops merging and sends every call on its own, which looks identical to an effect
	// that was never made coalescable. The author would only find out from the server's refusals under load.
	bool bCoalesceError = false;
	if (bCoalescable)
	{
		// The int/float split is not consulted here; only whether the parameter can be summed at all.
		bool bIsIntegerUnused = false;
		if (AccumulateParam.TrimStartAndEnd().IsEmpty())
		{
			bCoalesceError = true;
			ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
				"Effect '%s' coalesces repeated applies but names no Accumulate Parameter. Name the int or float "
				"tuning parameter whose values should be summed across a merge window."), *GetName())));
		}
		else if (!IsNumericMagnitude(Magnitudes, AccumulateParam, bIsIntegerUnused))
		{
			bCoalesceError = true;
			ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
				"Effect '%s' coalesces on the Accumulate Parameter '%s', which is not one of its int or float tuning "
				"parameters. Only a numeric parameter can be summed across merged applies."),
				*GetName(), *AccumulateParam.TrimStartAndEnd())));
		}

		if (CoalesceWindowSeconds <= 0.0f)
		{
			bCoalesceError = true;
			ValidationContext.AddError(FText::FromString(FString::Printf(TEXT(
				"Effect '%s' coalesces repeated applies but its Coalesce Window is not positive; a window of zero "
				"merges nothing."), *GetName())));
		}
	}

	// A blank effect is a not-yet-authored asset, not an error: an empty text body, or a graph mode with no graph
	// yet (a freshly created asset, before the graph editor has placed any nodes).
	bool bBlank;
	switch (Source)
	{
	case ECrowdyEffectSource::Graph:
		bBlank = (EffectGraph == nullptr || EffectGraph->Nodes.Num() == 0);
		break;
	case ECrowdyEffectSource::Text:
	default:
		bBlank = EffectScript.TrimStartAndEnd().IsEmpty();
		break;
	}
	if (bBlank)
	{
		return (bAutomationError || bDuplicateFunctionNameError || bSignalError || bCoalesceError)
			? EDataValidationResult::Invalid : Base;
	}

	// A curve only supplies a numeric magnitude; flag a curve bound to a non-numeric one at author time rather than
	// letting the apply-time marshaller be the first to reject it.
	bool bMagnitudeError = false;
	for (const FCrowdyEffectMagnitude& M : Magnitudes)
	{
		const ECrowdyEffectValueType ValueType = ResolveMagnitudeValueType(M);
		if (M.Curve && ValueType != ECrowdyEffectValueType::Int && ValueType != ECrowdyEffectValueType::Float)
		{
			bMagnitudeError = true;
			ValidationContext.AddError(FText::FromString(FString::Printf(
				TEXT("magnitude '%s' has a curve but a non-numeric value type '%s' (curves apply to int or float only)"),
				*M.Name, *ValueTypeToWireString(ValueType))));
		}
	}

	const FCrowdyEffectLoweringResult Compiled = Compile();
	bool bAnyError = false;
	for (const FCrowdyEffectDiagnostic& D : Compiled.Diagnostics)
	{
		if (D.Severity == ECrowdyEffectSeverity::Error)
		{
			bAnyError = true;
			ValidationContext.AddError(FText::FromString(D.ToString()));
		}
		else
		{
			ValidationContext.AddWarning(FText::FromString(D.ToString()));
		}
	}

	if (bAnyError || bMagnitudeError || bAutomationError || bDuplicateFunctionNameError || bSignalError
		|| bCoalesceError)
	{
		return EDataValidationResult::Invalid;
	}
	return Base == EDataValidationResult::NotValidated ? EDataValidationResult::Valid : Base;
}

void UCrowdyEffect::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// An edit can change the coalescing fields or the magnitude a summed parameter names, so the cached answer is
	// dropped rather than reasoned about per property.
	InvalidateCoalesceSpec();

	// Normalize magnitudes on every edit: trim names, treat an edited magnitude as modern (the enum is truth), and
	// mirror the legacy string from the enum. This keeps a freshly authored magnitude coherent on reload (PostLoad
	// would otherwise re-derive the enum from a stale string). The string->enum direction only ever runs at load.
	for (FCrowdyEffectMagnitude& Magnitude : Magnitudes)
	{
		Magnitude.Name = Magnitude.Name.TrimStartAndEnd();
		Magnitude.bTypeMigrated = true;
		Magnitude.ValueType = ValueTypeToWireString(Magnitude.ValueTypeEnum);
	}

	// Recompute the persisted "needs a Source" flag on every edit, so the invoke path (which never compiles on the
	// hot path) can enforce it and the picker can show it. The value reflects the current AST (a source read/write
	// sets it even if a later attribute error halts the compile); a non-shippable effect is never applied, so a
	// stale value there is harmless and self-corrects on the next valid edit. Serialized + cooked as last authored.
	// Catalog-free, and load-bearingly so: the Super:: call above broadcast this very edit, which invalidated the
	// fn-callee catalog, so a Project compile here would rebuild it (a project-wide effect-asset force-load) on
	// EVERY property tweak, including per-frame slider drags, only to read one catalog-independent bool.
	bRequiresSource = Compile(ECrowdyEffectFnCatalog::None).bSourceReferenced;
}

void UCrowdyEffect::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	Super::GetAssetRegistryTags(Context);

	// Only a save writes the payload. The Content Browser asks every asset class for its tags while the user is
	// merely browsing, and building this parses the effect body (and compiles the node graph, for a graph-authored
	// one), so answering there would do that work for every effect in the project for nothing.
	if (!Context.IsSaving())
	{
		return;
	}

	// A cook commandlet has none of the editor tooling the graph compiler lives in, so whatever it could write here
	// would describe the tooling that was loaded rather than the effect. A diff-only save writes a throwaway package
	// and must not decide what an asset says about itself either.
	if (IsRunningCookCommandlet())
	{
		return;
	}
	if (const UPackage* Package = GetPackage())
	{
		if (Package->HasAnyPackageFlags(PKG_ForDiffing))
		{
			return;
		}
	}

	FString Payload;
	if (!CrowdyEffectAuthoredSurface::BuildTagPayload(*this, Payload) || Payload.IsEmpty())
	{
		// Nothing usable to say, so nothing is written. A reader that finds no payload falls back to loading the
		// asset, which is the right answer; a zero-length tag value is not an option at all, since the registry
		// treats it as a programming error rather than as an absent value.
		return;
	}

	Context.AddTag(FAssetRegistryTag(
		FName(CrowdyEffectTagKeys::AuthoredSurface), Payload, FAssetRegistryTag::TT_Hidden));
	Context.AddTag(FAssetRegistryTag(
		FName(CrowdyEffectTagKeys::AuthoredSurfaceVersion),
		FString::FromInt(FCrowdyEffectAuthoredSurface::TagVersion), FAssetRegistryTag::TT_Hidden));
}
#endif
