// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyEffects.h"

#include "CrowdyGameModelLog.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyModelValue.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Whether Literal is a well-formed JSON number and nothing else: optional minus, an integer part, an optional
	// fraction, an optional exponent, no leading or trailing anything. This is the JSON grammar exactly, so a literal
	// it accepts parses to the same value the full reader would produce, and one it rejects still goes to the reader.
	// It exists so the common case (a magnitude whose value is a number) does not build a wrapper string and a whole
	// JSON reader per magnitude per apply.
	bool IsJsonNumberLiteral(const FString& Literal)
	{
		const TCHAR* Cursor = *Literal;
		if (*Cursor == TEXT('-'))
		{
			++Cursor;
		}
		if (!FChar::IsDigit(*Cursor))
		{
			return false;
		}
		while (FChar::IsDigit(*Cursor))
		{
			++Cursor;
		}
		if (*Cursor == TEXT('.'))
		{
			++Cursor;
			if (!FChar::IsDigit(*Cursor))
			{
				return false;
			}
			while (FChar::IsDigit(*Cursor))
			{
				++Cursor;
			}
		}
		if (*Cursor == TEXT('e') || *Cursor == TEXT('E'))
		{
			++Cursor;
			if (*Cursor == TEXT('+') || *Cursor == TEXT('-'))
			{
				++Cursor;
			}
			if (!FChar::IsDigit(*Cursor))
			{
				return false;
			}
			while (FChar::IsDigit(*Cursor))
			{
				++Cursor;
			}
		}
		return *Cursor == TEXT('\0');
	}

	// Parse a JSON-encoded value literal ("5", "12.5", "true", "\"text\"") into an FJsonValue. The JSON reader
	// rejects a bare top-level scalar, so wrap it (mirrors CrowdyModel::ParseCachedValue and the apply path).
	// Returns null on malformed input.
	//
	// Numbers and the two boolean literals are answered directly, because they are what almost every magnitude
	// carries and each one otherwise costs a formatted wrapper string plus a reader. Everything else - a quoted
	// string, which needs real unescaping, null, or anything malformed - still goes through the reader, so the set of
	// literals accepted and the values produced are unchanged.
	TSharedPtr<FJsonValue> ParseValueLiteral(const FString& Literal)
	{
		if (IsJsonNumberLiteral(Literal))
		{
			return MakeShared<FJsonValueNumber>(FCString::Atod(*Literal));
		}
		if (Literal.Equals(TEXT("true"), ESearchCase::CaseSensitive))
		{
			return MakeShared<FJsonValueBoolean>(true);
		}
		if (Literal.Equals(TEXT("false"), ESearchCase::CaseSensitive))
		{
			return MakeShared<FJsonValueBoolean>(false);
		}

		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *Literal);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
		TSharedPtr<FJsonObject> Object;
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			return Object->TryGetField(TEXT("v"));
		}
		return nullptr;
	}

	// Fill Request's coalescing fields from the effect, and answer whether this apply is offered for merging at all.
	// The effect has to opt in with a positive window and name one of its own int or float tuning parameters to sum.
	// The subsystem checks the marshalled value itself and falls back to an unmerged send if it is not a number, so
	// this only decides whether merging is asked for, never whether it is possible.
	//
	// The decision itself is cached on the asset, because it depends only on fields the asset serializes and this runs
	// on every apply. Reading it back is one copy of the parameter name instead of a magnitude scan and two trims.
	//
	// A misconfigured Accumulate Parameter is reported by asset validation, which is where it belongs: at runtime the
	// only symptom is that merging quietly stops happening, and an effect applied ten times a second cannot afford a
	// log line per apply.
	bool TryFillCoalesceSpec(const UCrowdyEffect* Effect, FCrowdyCoalesceRequest& Request)
	{
		bool bInteger = false;
		if (!Effect || !Effect->TryGetCoalesceSpec(Request.AccumulateParam, bInteger))
		{
			return false;
		}

		Request.bAccumulateIsInteger = bInteger;
		Request.WindowSeconds = Effect->CoalesceWindowSeconds;
		return true;
	}
}

uint64 UCrowdyEffects::BuildMergeDiscriminator(const UCrowdyEffect* Effect, const TMap<FName, FString>& Overrides,
	float Level, const FString& SourceContainerId, const FString& AccumulateParam)
{
	// The asset identity stands for everything the effect itself contributes: its magnitude set, their authored
	// defaults and their curves. Two assets can share a function name on different container types, and their
	// parameters are not interchangeable. It is taken as the asset's own name plus its package's, both already FNames,
	// so identifying the asset costs two table lookups rather than the string an object path has to build; and unlike
	// a cached path it cannot go stale when the asset is renamed.
	uint64 Discriminator = 0;
	if (Effect)
	{
		Discriminator = CrowdyCoalesce::MixHash(Discriminator, GetTypeHash(Effect->GetFName()));
		Discriminator = CrowdyCoalesce::MixHash(Discriminator, GetTypeHash(Effect->GetOutermost()->GetFName()));
	}

	// The level's exact bits, not a printed form: two levels that print the same must not merge if they would sample a
	// curve differently.
	uint32 LevelBits = 0;
	FMemory::Memcpy(&LevelBits, &Level, sizeof(LevelBits));
	Discriminator = CrowdyCoalesce::MixHash(Discriminator, LevelBits);
	Discriminator = CrowdyCoalesce::MixHash(Discriminator, CrowdyCoalesce::HashString(SourceContainerId));

	// Overrides are looked up by FName in the marshaller, so they are compared the same way here: the accumulated one
	// is dropped by FName, and the rest are folded in by SUMMING their per-entry hashes, which is independent of map
	// iteration order without needing a sort. Two entries can never cancel, because a map's keys are unique. No
	// per-override string copy either: an FName's hash is already case-insensitive, which is the normalization the
	// string form spelled out by lowercasing each key.
	const FName AccumulateName(*AccumulateParam);
	uint64 OverrideSum = 0;
	for (const TPair<FName, FString>& Override : Overrides)
	{
		if (!AccumulateParam.IsEmpty() && Override.Key == AccumulateName)
		{
			continue;
		}
		OverrideSum += CrowdyCoalesce::MixHash(GetTypeHash(Override.Key),
			CrowdyCoalesce::HashString(Override.Value));
	}
	return CrowdyCoalesce::MixHash(Discriminator, OverrideSum);
}

FString UCrowdyEffects::JsonFromInt(int32 Value)
{
	return FString::FromInt(Value);
}

FString UCrowdyEffects::JsonFromFloat(double Value)
{
	// A finite double sanitizes to a valid JSON number; a non-finite one has no JSON form, so emit 0 rather than
	// "nan"/"inf" (which the marshaller would reject).
	if (!FMath::IsFinite(Value))
	{
		return TEXT("0");
	}
	return FString::SanitizeFloat(Value);
}

FString UCrowdyEffects::JsonFromBool(bool Value)
{
	return Value ? TEXT("true") : TEXT("false");
}

FString UCrowdyEffects::JsonFromString(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}

int32 UCrowdyEffects::JsonToInt(const FString& ValueJson)
{
	return UCrowdyModelValue::AsInt(ValueJson, 0);
}

double UCrowdyEffects::JsonToFloat(const FString& ValueJson)
{
	return UCrowdyModelValue::AsDouble(ValueJson, 0.0);
}

bool UCrowdyEffects::JsonToBool(const FString& ValueJson)
{
	return UCrowdyModelValue::AsBool(ValueJson, false);
}

FString UCrowdyEffects::JsonToString(const FString& ValueJson)
{
	return UCrowdyModelValue::AsString(ValueJson, FString());
}

FString UCrowdyEffects::GetContainerIdFor(UObject* Object)
{
	if (!Object)
	{
		return FString();
	}
	const UWorld* World = Object->GetWorld();
	if (!World)
	{
		return FString();
	}
	UCrowdyGameModelSubsystem* Model = World->GetSubsystem<UCrowdyGameModelSubsystem>();
	if (!Model)
	{
		return FString();
	}
	FGuid NetID;
	FString ContainerId;
	if (Model->ResolveTargetNetID(Object, NetID) && Model->TryGetContainerId(NetID, ContainerId))
	{
		return ContainerId;
	}
	return FString();
}

TSharedPtr<FJsonObject> UCrowdyEffects::BuildInvokeParams(const UCrowdyEffect* Effect,
	const TMap<FName, FString>& Overrides, float Level, bool bHasSource, const FString& SourceContainerId, FString& OutError)
{
	OutError.Reset();
	if (!Effect)
	{
		OutError = TEXT("no effect asset");
		return nullptr;
	}

	// An effect that reads or writes source.<attr> cannot run without a Source: the lowering makes source_id a
	// required param. Reject that here (a clear client-side error) rather than dispatching an invoke the server is
	// bound to fail. bRequiresSource is the persisted lowering result; when stale-false the check is simply skipped,
	// which is the pre-guardrail behavior, so this can never produce a false rejection.
	if (Effect->bRequiresSource && !bHasSource)
	{
		OutError = FString::Printf(
			TEXT("effect '%s' reads a Source but was applied without one; pass the instigator as the Source"),
			*Effect->GetName());
		return nullptr;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const FCrowdyEffectMagnitude& Magnitude : Effect->Magnitudes)
	{
		if (Magnitude.Name.IsEmpty())
		{
			OutError = TEXT("an effect magnitude has an empty name");
			return nullptr;
		}

		const ECrowdyEffectValueType ValueType = UCrowdyEffect::ResolveMagnitudeValueType(Magnitude);

		// Resolve the value literal: an explicit Override (a JSON-encoded literal) wins; else a bound curve is
		// sampled at Level (numeric magnitudes only, an int rounds); else the authored default. An empty result
		// means a required magnitude with no curve was left unsupplied.
		//
		// The magnitude's name is taken as an FName it caches rather than built from its string here, so a repeated
		// apply does not re-intern every parameter name it has already interned.
		const FString* Override = Overrides.IsEmpty() ? nullptr : Overrides.Find(Magnitude.GetNameKey());
		FString Value;
		if (Override)
		{
			Value = *Override;
		}
		else if (Magnitude.Curve)
		{
			if (ValueType != ECrowdyEffectValueType::Int && ValueType != ECrowdyEffectValueType::Float)
			{
				OutError = FString::Printf(TEXT("magnitude '%s' has a curve but a non-numeric value type '%s'"),
					*Magnitude.Name, *UCrowdyEffect::ValueTypeToWireString(ValueType));
				return nullptr;
			}
			const float Sampled = Magnitude.Curve->GetFloatValue(Level);
			if (!FMath::IsFinite(Sampled))
			{
				OutError = FString::Printf(
					TEXT("magnitude '%s' curve sampled a non-finite value at level %g"), *Magnitude.Name, Level);
				return nullptr;
			}
			if (ValueType == ECrowdyEffectValueType::Int)
			{
				// Round half-up in double, then range-check before narrowing, so an out-of-range sample fails loudly
				// instead of wrapping to a garbage int32 that would still be valid JSON.
				const double Rounded = FMath::FloorToDouble(static_cast<double>(Sampled) + 0.5);
				if (Rounded < static_cast<double>(MIN_int32) || Rounded > static_cast<double>(MAX_int32))
				{
					OutError = FString::Printf(TEXT("magnitude '%s' curve sampled %g at level %g, outside the int range"),
						*Magnitude.Name, Sampled, Level);
					return nullptr;
				}
				Value = FString::FromInt(static_cast<int32>(Rounded));
			}
			else
			{
				Value = FString::SanitizeFloat(Sampled);
			}
		}
		else
		{
			Value = Magnitude.DefaultValueJson;
		}
		if (Value.IsEmpty())
		{
			OutError = FString::Printf(TEXT("magnitude '%s' is required but no value was supplied"), *Magnitude.Name);
			return nullptr;
		}

		// A string / container_ref magnitude is ALWAYS sent as a JSON string, so a designer can type sword or a bare
		// uuid without quotes and an all-digit id is not mis-sent as a number: use the parsed value only when it
		// already parsed AS a string, otherwise stringify the raw text. A numeric / bool magnitude must be valid JSON
		// of its own shape -- a non-JSON value there is a typo, rejected here with a clear client-side error rather
		// than shipped as a wrong-typed string the server rejects far from the edit. The value is never empty here (a
		// required-missing magnitude was already rejected above).
		const bool bStringShaped =
			(ValueType == ECrowdyEffectValueType::String || ValueType == ECrowdyEffectValueType::ContainerRef);

		// A string-shaped value that does not start with a quote cannot parse as a JSON string whatever the parser
		// does with it, so it is stringified without one being run: that is the designer-typed case (a bare word, an
		// unquoted uuid) and the same value the parse-then-fall-back path produced.
		if (bStringShaped && !Value.StartsWith(TEXT("\""), ESearchCase::CaseSensitive))
		{
			Params->SetField(Magnitude.Name, MakeShared<FJsonValueString>(Value));
			continue;
		}

		TSharedPtr<FJsonValue> Parsed = ParseValueLiteral(Value);
		if (bStringShaped)
		{
			if (!Parsed.IsValid() || Parsed->Type != EJson::String)
			{
				Parsed = MakeShared<FJsonValueString>(Value);
			}
		}
		else if (!Parsed.IsValid())
		{
			OutError = FString::Printf(TEXT("magnitude '%s' value is not valid JSON: %s"), *Magnitude.Name, *Value);
			return nullptr;
		}
		Params->SetField(Magnitude.Name, Parsed);
	}

	// source_id is injected last so the resolved container id is authoritative even if a magnitude were named
	// source_id (the lowering rejects that at author time; this is defense in depth).
	if (bHasSource)
	{
		if (SourceContainerId.IsEmpty())
		{
			OutError = TEXT("effect has a Source but the Source has no bound container");
			return nullptr;
		}
		Params->SetStringField(TEXT("source_id"), SourceContainerId);
	}

	return Params;
}

bool UCrowdyEffects::ApplyInternal(UObject* WorldContext, UObject* Target, const FString& ContainerId,
	UCrowdyEffect* Effect, UObject* Source, const TMap<FName, FString>& Overrides, float Level, const FString& SessionId,
	TFunction<void(FCrowdyInvokeResult)> OnDone, FString& OutError)
{
	OutError.Reset();
	if (!Effect)
	{
		OutError = TEXT("no effect asset");
		return false;
	}

	// Resolve the world from whichever context we hold: a target/source participant, or the explicit WorldContext.
	const UObject* Ctx = Target ? Target : (Source ? Source : WorldContext);
	const UWorld* World = Ctx ? Ctx->GetWorld() : nullptr;
	if (!World)
	{
		OutError = TEXT("no world (not a play world?)");
		return false;
	}

	UCrowdyGameModelSubsystem* Model = World->GetSubsystem<UCrowdyGameModelSubsystem>();
	if (!Model)
	{
		OutError = TEXT("UCrowdyGameModelSubsystem not found");
		return false;
	}

	// Source (optional) -> its bound container id for the source_id param.
	FString SourceContainerId;
	const bool bHasSource = (Source != nullptr);
	if (bHasSource)
	{
		FGuid SourceNetID;
		if (!Model->ResolveTargetNetID(Source, SourceNetID) || !Model->TryGetContainerId(SourceNetID, SourceContainerId))
		{
			OutError = TEXT("Source is not a bound Game Model container");
			return false;
		}
	}

	TSharedPtr<FJsonObject> Params = BuildInvokeParams(Effect, Overrides, Level, bHasSource, SourceContainerId, OutError);
	if (!Params.IsValid())
	{
		return false; // OutError already set by the marshaller
	}

	const FString FunctionName = Effect->GetEffectiveFunctionName();
	if (FunctionName.IsEmpty())
	{
		OutError = TEXT("effect has no function name");
		return false;
	}

	// A model-driven notification names its container via the server-injected $self_container_id, resolved from the
	// selfContainerId the invoke path already sends (InvokeAndApply from the target's bound container, InvokeOnContainer
	// from the container id). So no notify param is filled here: an effect that also writes the SOURCE container still
	// notifies only the target/self, and observers bound to the source refresh on their next pull of it.

	// Both routes are described the same way from here, so the coalescing decision is made once for either. Every
	// synchronous failure is still reported before anything is queued or sent.
	FCrowdyCoalesceRequest Request;
	Request.FunctionName = FunctionName;
	Request.SessionId = SessionId;
	Request.Params = Params;

	// Entity-bound: the Target rides as SelfNetID (resolved to selfContainerId server-side).
	if (Target)
	{
		FGuid TargetNetID;
		if (!Model->ResolveTargetNetID(Target, TargetNetID))
		{
			OutError = TEXT("Target is not a registered entity");
			return false;
		}
		Request.bEntityBound = true;
		Request.SelfNetID = TargetNetID;
	}
	else
	{
		// Free/data container path (no actor).
		if (ContainerId.IsEmpty())
		{
			OutError = TEXT("no Target actor and no container id");
			return false;
		}
		Request.ContainerId = ContainerId;
	}

	// A coalescable effect goes through the subsystem's merge window; everything else takes the direct route it
	// always has. The two branches differ only in when the call is sent, never in what OnDone eventually receives.
	//
	// The discriminator is built only when it can actually key a window: when the coalescer cannot merge anything
	// right now (the world is tearing down), nothing would ever compare it. The request still goes to the same seam,
	// which then dispatches it on its own - sending it down the direct route instead would put a coalescable effect's
	// per-apply arithmetic in two places that have to agree.
	if (TryFillCoalesceSpec(Effect, Request))
	{
		if (Model->IsCoalescingAvailable())
		{
			Request.MergeDiscriminator =
				BuildMergeDiscriminator(Effect, Overrides, Level, SourceContainerId, Request.AccumulateParam);
		}
		Model->EnqueueCoalescedInvoke(Request, MoveTemp(OnDone));
		return true;
	}

	if (Request.bEntityBound)
	{
		Model->InvokeAndApply(Request.SelfNetID, FunctionName, Params, SessionId, MoveTemp(OnDone));
		return true;
	}
	Model->InvokeOnContainer(ContainerId, FunctionName, Params, SessionId, MoveTemp(OnDone));
	return true;
}

void UCrowdyEffects::Apply(UCrowdyEffect* Effect, UObject* Target, UObject* Source,
	const TMap<FName, FString>& Overrides, float Level, const FString& SessionId)
{
	FString Error;
	if (!ApplyInternal(nullptr, Target, FString(), Effect, Source, Overrides, Level, SessionId, nullptr, Error))
	{
		UE_LOG(LogCrowdyGameModel, Warning, TEXT("[GameModel] Apply failed: %s"), *Error);
	}
}

void UCrowdyEffects::ApplyToContainer(UObject* WorldContext, const FString& ContainerId, UCrowdyEffect* Effect,
	UObject* Source, const TMap<FName, FString>& Overrides, float Level, const FString& SessionId)
{
	FString Error;
	if (!ApplyInternal(WorldContext, nullptr, ContainerId, Effect, Source, Overrides, Level, SessionId, nullptr, Error))
	{
		UE_LOG(LogCrowdyGameModel, Warning, TEXT("[GameModel] ApplyToContainer failed: %s"), *Error);
	}
}
