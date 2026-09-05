#include "Replication/GameModel/Kit/CrowdyGameKitEmit.h"

#include "Replication/GameModel/Kit/CrowdyGameKitPreview.h"
#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	ECrowdyKitOwnerIdKind ToBridgeOwnerId(ECrowdyKitOwnerId Kind)
	{
		return Kind == ECrowdyKitOwnerId::String ? ECrowdyKitOwnerIdKind::String : ECrowdyKitOwnerIdKind::Int;
	}

	// The preset (reflection) enums and the bridge (plain) enums are distinct types with the same shape;
	// map by case so a future divergence in either is a compile error rather than a silent misvalue.
	ECrowdyKitInstantiableBy ToBridgeInstantiableBy(ECrowdyKitCreator Kind)
	{
		return Kind == ECrowdyKitCreator::Admin ? ECrowdyKitInstantiableBy::Admin
												: ECrowdyKitInstantiableBy::Member;
	}

	ECrowdyKitSubmitAuthority ToBridgeSubmitAuthority(ECrowdyKitAuthority Authority)
	{
		switch (Authority)
		{
		case ECrowdyKitAuthority::Server:
			return ECrowdyKitSubmitAuthority::Server;
		case ECrowdyKitAuthority::Automation:
			return ECrowdyKitSubmitAuthority::Automation;
		case ECrowdyKitAuthority::Owner:
			return ECrowdyKitSubmitAuthority::Owner;
		case ECrowdyKitAuthority::Host:
		default:
			return ECrowdyKitSubmitAuthority::Host;
		}
	}

	FCrowdyKitLayerSpec CombatSpec(const UCrowdyCombatPreset& P)
	{
		FCrowdyKitLayerSpec Spec;
		Spec.Genre = ECrowdyKitGenre::Combat;
		Spec.Combat.TypePrefix = P.TypePrefix;
		Spec.Combat.bTurnBased = P.bTurnBased;
		Spec.Combat.bHostSynced = P.bHostSynced;
		Spec.Combat.EffectTickIntervalMs = P.EffectTickIntervalMs;
		Spec.Combat.CombatantInstantiableBy = ToBridgeInstantiableBy(P.CombatantInstantiableBy);
		Spec.Combat.bEnableRevive = P.bEnableRevive;
		Spec.Combat.ReviveGroupId = P.ReviveGroupId;
		Spec.Combat.RevivePermission = P.RevivePermission;
		Spec.Combat.OwnerIdKind = ToBridgeOwnerId(P.OwnerIdKind);
		return Spec;
	}

	FCrowdyKitLayerSpec LeaderboardsSpec(const UCrowdyLeaderboardsPreset& P)
	{
		FCrowdyKitLayerSpec Spec;
		Spec.Genre = ECrowdyKitGenre::Leaderboards;
		Spec.Leaderboards.TypePrefix = P.TypePrefix;
		Spec.Leaderboards.SubmitAuthority = ToBridgeSubmitAuthority(P.SubmitAuthority);
		Spec.Leaderboards.bKeepBest = P.bKeepBest;
		Spec.Leaderboards.SeasonCron = P.SeasonCron;
		Spec.Leaderboards.OwnerIdKind = ToBridgeOwnerId(P.OwnerIdKind);
		return Spec;
	}

	FCrowdyKitLayerSpec GuildSpec(const UCrowdyGuildPreset& P)
	{
		FCrowdyKitLayerSpec Spec;
		Spec.Genre = ECrowdyKitGenre::Guild;
		Spec.Guild.TypePrefix = P.TypePrefix;
		Spec.Guild.GuildGroupId = P.GuildGroupId;
		Spec.Guild.HallPermission = P.HallPermission;
		Spec.Guild.bBank = P.bBank;
		return Spec;
	}

	FCrowdyKitLayerSpec LivingWorldSpec(const UCrowdyLivingWorldPreset& P)
	{
		FCrowdyKitLayerSpec Spec;
		Spec.Genre = ECrowdyKitGenre::LivingWorld;
		Spec.LivingWorld.TypePrefix = P.TypePrefix;

		Spec.LivingWorld.bEnableTime = P.bEnableTime;
		Spec.LivingWorld.Time.IntervalMs = P.TimeIntervalMs;
		Spec.LivingWorld.Time.HoursPerDay = P.HoursPerDay;
		Spec.LivingWorld.Time.bWeather = P.bWeather;
		Spec.LivingWorld.Time.NotifyDistance = P.NotifyDistance;

		Spec.LivingWorld.bEnableNodes = P.bEnableNodes;
		Spec.LivingWorld.Nodes.IntervalMs = P.NodesIntervalMs;

		Spec.LivingWorld.bEnableCrops = P.bEnableCrops;
		Spec.LivingWorld.Crops.IntervalMs = P.CropsIntervalMs;

		Spec.LivingWorld.bEnableWaves = P.bEnableWaves;
		Spec.LivingWorld.Waves.IntervalMs = P.WavesIntervalMs;
		Spec.LivingWorld.Waves.Growth = P.WaveGrowth;

		Spec.LivingWorld.OwnerIdKind = ToBridgeOwnerId(P.OwnerIdKind);
		return Spec;
	}
}

FCrowdyKitBundle CrowdyKitEmitFromPresets(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers,
	int64 AppId, const FString& SessionId)
{
	TArray<FCrowdyKitLayerSpec> Specs;
	Specs.Reserve(Layers.Num());

	for (const TObjectPtr<UCrowdyKitLayerPreset>& Layer : Layers)
	{
		const UCrowdyKitLayerPreset* Preset = Layer.Get();
		if (!Preset)
		{
			continue;
		}

		if (const UCrowdyCombatPreset* Combat = Cast<UCrowdyCombatPreset>(Preset))
		{
			Specs.Add(CombatSpec(*Combat));
		}
		else if (const UCrowdyLeaderboardsPreset* Boards = Cast<UCrowdyLeaderboardsPreset>(Preset))
		{
			Specs.Add(LeaderboardsSpec(*Boards));
		}
		else if (const UCrowdyGuildPreset* Guild = Cast<UCrowdyGuildPreset>(Preset))
		{
			Specs.Add(GuildSpec(*Guild));
		}
		else if (const UCrowdyLivingWorldPreset* World = Cast<UCrowdyLivingWorldPreset>(Preset))
		{
			Specs.Add(LivingWorldSpec(*World));
		}
		// An unknown preset subclass is skipped; the emit still runs on the recognized layers.
	}

	return FCrowdyKitBridge::EmitBundle(AppId, Specs, SessionId);
}

FCrowdyKitPreview CrowdyKitPreviewLayers(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers,
	int64 AppId, const FString& SessionId)
{
	const FCrowdyKitBundle Bundle = CrowdyKitEmitFromPresets(Layers, AppId, SessionId);

	FCrowdyKitPreview Preview;
	Preview.bOk = Bundle.bOk;
	Preview.Error = Bundle.ErrorMessage;
	Preview.NumContainerTypes = Bundle.NumContainerTypes;
	Preview.NumPropertyDefs = Bundle.NumPropertyDefs;
	Preview.NumFunctions = Bundle.NumFunctions;
	Preview.NumAutomations = Bundle.NumAutomations;
	return Preview;
}

namespace
{
	// Append the string field Field of each object in the seed's Array member into Out (unique, non-empty).
	void CollectNamesFromSeedArray(const TSharedPtr<FJsonObject>& Seed, const TCHAR* ArrayField,
		const TCHAR* NameField, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Seed->TryGetArrayField(ArrayField, Array) || !Array)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Element : *Array)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Element.IsValid() || !Element->TryGetObject(Object) || !Object)
			{
				continue;
			}
			FString Name;
			if ((*Object)->TryGetStringField(NameField, Name) && !Name.IsEmpty())
			{
				Out.AddUnique(Name);
			}
		}
	}
}

bool CrowdyKitDeployedNames(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers,
	int64 AppId, const FString& SessionId, TArray<FString>& OutTypeNames, TArray<FString>& OutFunctionNames)
{
	OutTypeNames.Reset();
	OutFunctionNames.Reset();

	const FCrowdyKitBundle Bundle = CrowdyKitEmitFromPresets(Layers, AppId, SessionId);
	if (!Bundle.bOk || Bundle.SeedInputJson.IsEmpty())
	{
		return false;
	}

	// The seed is the gameModelSeed variables object: its containerTypes[].typeName and functions[].name are the
	// exact names the deploy seeds, so they are the names a later schema sync protects from prune.
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Bundle.SeedInputJson);
	TSharedPtr<FJsonObject> Seed;
	if (!FJsonSerializer::Deserialize(Reader, Seed) || !Seed.IsValid())
	{
		return false;
	}

	CollectNamesFromSeedArray(Seed, TEXT("containerTypes"), TEXT("typeName"), OutTypeNames);
	CollectNamesFromSeedArray(Seed, TEXT("functions"), TEXT("name"), OutFunctionNames);
	return true;
}
