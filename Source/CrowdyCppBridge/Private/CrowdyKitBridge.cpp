#include "CrowdyKitBridge.h"

#include "CrowdyCppClient.h"

THIRD_PARTY_INCLUDES_START
// The kit runtime helpers (pulled in via social.hpp) define a method named
// ensure(), which collides with Unreal's ensure() macro; suspend it across the
// vendored includes and restore it afterward. ensure is the only such collision
// in the current vendored set; re-check for new ones (check/verify/...) whenever
// CrowdyCPP is re-vendored.
#pragma push_macro("ensure")
#undef ensure
#include "crowdy/graphql/json.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/combat.hpp"
#include "crowdy/kit/leaderboards.hpp"
#include "crowdy/kit/social.hpp"
#include "crowdy/kit/worldsim.hpp"
#pragma pop_macro("ensure")
THIRD_PARTY_INCLUDES_END

#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace
{
	std::string Utf8(const FString& Value)
	{
		return std::string(TCHAR_TO_UTF8(*Value));
	}

	// Length-bounded UTF-8 conversion so an embedded NUL never truncates the value.
	FString FromUtf8(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(Value.data(), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	crowdy::kit::OwnerIdKind MapOwnerIdKind(ECrowdyKitOwnerIdKind Kind)
	{
		return Kind == ECrowdyKitOwnerIdKind::String ? crowdy::kit::OwnerIdKind::String
													 : crowdy::kit::OwnerIdKind::Int;
	}

	crowdy::kit::TrustedAuthority MapSubmitAuthority(ECrowdyKitSubmitAuthority Authority)
	{
		switch (Authority)
		{
		case ECrowdyKitSubmitAuthority::Server:
			return crowdy::kit::TrustedAuthority::server();
		case ECrowdyKitSubmitAuthority::Automation:
			return crowdy::kit::TrustedAuthority::automation();
		case ECrowdyKitSubmitAuthority::Owner:
			return crowdy::kit::TrustedAuthority::owner();
		case ECrowdyKitSubmitAuthority::Host:
		default:
			return crowdy::kit::TrustedAuthority::host();
		}
	}

	FCrowdyKitBundle FailBundle(const FString& Message)
	{
		FCrowdyKitBundle Bundle;
		Bundle.bOk = false;
		Bundle.ErrorMessage = Message;
		return Bundle;
	}

	crowdy::kit::KitBlueprint BuildCombat(const FCrowdyKitCombatOptions& In)
	{
		crowdy::kit::CombatBlueprintOptions Options;
		Options.typePrefix = Utf8(In.TypePrefix);
		Options.turnBased = In.bTurnBased;
		Options.hostSynced = In.bHostSynced;
		Options.effectTickIntervalMs = In.EffectTickIntervalMs;
		Options.combatantInstantiableBy =
			In.CombatantInstantiableBy == ECrowdyKitInstantiableBy::Admin ? "admin" : "member";
		if (In.bEnableRevive)
		{
			crowdy::kit::CombatReviveGroup Revive;
			Revive.groupId = Utf8(In.ReviveGroupId);
			Revive.permission = Utf8(In.RevivePermission);
			Options.reviveGroup = Revive;
		}
		Options.ownerIdKind = MapOwnerIdKind(In.OwnerIdKind);
		return crowdy::kit::combatBlueprint(Options);
	}

	crowdy::kit::KitBlueprint BuildLeaderboards(const FCrowdyKitLeaderboardsOptions& In)
	{
		crowdy::kit::LeaderboardsBlueprintOptions Options;
		Options.typePrefix = Utf8(In.TypePrefix);
		Options.submitAuthority = MapSubmitAuthority(In.SubmitAuthority);
		Options.keepBest = In.bKeepBest;
		Options.seasonCron = Utf8(In.SeasonCron);
		Options.ownerIdKind = MapOwnerIdKind(In.OwnerIdKind);
		return crowdy::kit::leaderboardsBlueprint(Options);
	}

	crowdy::kit::KitBlueprint BuildGuild(const FCrowdyKitGuildOptions& In)
	{
		crowdy::kit::GuildBlueprintOptions Options;
		// An empty prefix would name the composed types "Hall"/"BankInventory"; keep the kit default.
		Options.typePrefix = In.TypePrefix.IsEmpty() ? std::string("Guild") : Utf8(In.TypePrefix);
		Options.guildGroupId = Utf8(In.GuildGroupId);
		Options.hallPermission = Utf8(In.HallPermission);
		Options.bank = In.bBank;
		return crowdy::kit::guildBlueprint(Options);
	}

	crowdy::kit::KitBlueprint BuildLivingWorld(const FCrowdyKitLivingWorldOptions& In)
	{
		crowdy::kit::WorldsimBlueprintOptions Options;
		Options.typePrefix = Utf8(In.TypePrefix);

		if (In.bEnableTime)
		{
			crowdy::kit::WorldsimTimeOptions Time;
			Time.intervalMs = In.Time.IntervalMs;
			Time.hoursPerDay = In.Time.HoursPerDay;
			Time.weather = In.Time.bWeather;
			Time.notifyDistance = In.Time.NotifyDistance;
			Options.time = Time;
		}
		else
		{
			Options.time = std::nullopt;
		}

		if (In.bEnableNodes)
		{
			crowdy::kit::WorldsimNodesOptions Nodes;
			Nodes.intervalMs = In.Nodes.IntervalMs;
			Options.nodes = Nodes;
		}
		else
		{
			Options.nodes = std::nullopt;
		}

		if (In.bEnableCrops)
		{
			crowdy::kit::WorldsimCropsOptions Crops;
			Crops.intervalMs = In.Crops.IntervalMs;
			Options.crops = Crops;
		}
		else
		{
			Options.crops = std::nullopt;
		}

		if (In.bEnableWaves)
		{
			crowdy::kit::WorldsimWavesOptions Waves;
			Waves.intervalMs = In.Waves.IntervalMs;
			Waves.growth = In.Waves.Growth;
			Options.waves = Waves;
		}
		else
		{
			Options.waves = std::nullopt;
		}

		Options.ownerIdKind = MapOwnerIdKind(In.OwnerIdKind);
		return crowdy::kit::worldsimBlueprint(Options);
	}
}

FCrowdyKitBundle FCrowdyKitBridge::EmitBundle(int64 AppId, const TArray<FCrowdyKitLayerSpec>& Layers,
	const FString& SessionId)
{
	if (Layers.Num() == 0)
	{
		return FailBundle(TEXT("No kit layers to emit"));
	}

	try
	{
		std::vector<crowdy::kit::KitBlueprint> Blueprints;
		Blueprints.reserve(static_cast<std::size_t>(Layers.Num()));

		for (const FCrowdyKitLayerSpec& Layer : Layers)
		{
			switch (Layer.Genre)
			{
			case ECrowdyKitGenre::Combat:
				// Revive flips a downed combatant to full hp: a reward-sensitive mutation gated on a
				// group permission. An empty group id would emit a degenerate policy, so require one
				// (mirrors the Guild group-id guard).
				if (Layer.Combat.bEnableRevive && Layer.Combat.ReviveGroupId.IsEmpty())
				{
					return FailBundle(TEXT("Combat layer with Enable Revive requires a Revive Group Id"));
				}
				Blueprints.push_back(BuildCombat(Layer.Combat));
				break;
			case ECrowdyKitGenre::Leaderboards:
				Blueprints.push_back(BuildLeaderboards(Layer.Leaderboards));
				break;
			case ECrowdyKitGenre::Guild:
				if (Layer.Guild.GuildGroupId.IsEmpty())
				{
					return FailBundle(TEXT("Guild layer requires a Guild Group Id (create the guild team first)"));
				}
				Blueprints.push_back(BuildGuild(Layer.Guild));
				break;
			case ECrowdyKitGenre::LivingWorld:
				if (!Layer.LivingWorld.bEnableTime && !Layer.LivingWorld.bEnableNodes
					&& !Layer.LivingWorld.bEnableCrops && !Layer.LivingWorld.bEnableWaves)
				{
					return FailBundle(TEXT("Living World layer has every section disabled"));
				}
				Blueprints.push_back(BuildLivingWorld(Layer.LivingWorld));
				break;
			default:
				return FailBundle(TEXT("Unknown kit genre"));
			}
		}

		// appId is a BigInt scalar: mergeBlueprints stamps it as a JSON string into the seed and every automation.
		const std::string AppIdStr = Utf8(LexToString(AppId));
		const std::string SessionIdStr = Utf8(SessionId);
		crowdy::kit::MergedBlueprints Merged = crowdy::kit::mergeBlueprints(AppIdStr, Blueprints, SessionIdStr);

		FCrowdyKitBundle Bundle;
		Bundle.bOk = true;
		Bundle.SeedInputJson = FromUtf8(Merged.seedInput.dump());
		Bundle.AutomationJsons.Reserve(static_cast<int32>(Merged.automations.size()));
		for (const crowdy::graphql::JVal& Automation : Merged.automations)
		{
			Bundle.AutomationJsons.Add(FromUtf8(Automation.dump()));
		}
		Bundle.TriggerJsons.Reserve(static_cast<int32>(Merged.automationTriggers.size()));
		for (const crowdy::graphql::JVal& Trigger : Merged.automationTriggers)
		{
			Bundle.TriggerJsons.Add(FromUtf8(Trigger.dump()));
		}

		// mergeBlueprints rejects duplicate names, so a survived merge means the per-layer sums equal the
		// merged totals; sum from the blueprints (still valid - mergeBlueprints copies) for the preview counts.
		for (const crowdy::kit::KitBlueprint& Blueprint : Blueprints)
		{
			Bundle.NumContainerTypes += static_cast<int32>(Blueprint.containerTypes.size());
			Bundle.NumPropertyDefs += static_cast<int32>(Blueprint.propertyDefinitions.size());
			Bundle.NumFunctions += static_cast<int32>(Blueprint.functions.size());
		}
		Bundle.NumAutomations = static_cast<int32>(Merged.automations.size());
		return Bundle;
	}
	catch (const std::exception& Ex)
	{
		return FailBundle(FString(UTF8_TO_TCHAR(Ex.what())));
	}
	catch (...)
	{
		return FailBundle(TEXT("Kit emit threw a non-standard exception"));
	}
}

namespace
{
	// State threaded through the async deploy chain. Held alive by the pending step callback (each captures the
	// TSharedRef), so it outlives every in-flight step. bFinished makes the aggregate OnDone fire once.
	struct FKitDeployState
	{
		// Weak, not strong: a completion queued inside the client's own dispatcher captures this state, so a strong
		// ref here would close a cycle (client -> dispatcher -> queued callback -> state -> client) that leaks both
		// if a deploy is abandoned before its last completion is drained. The caller owns the client and keeps it
		// alive by polling; each step re-pins it and fails cleanly if it has been released.
		TWeakPtr<FCrowdyCppClient> Client;
		TArray<FString> AutomationJsons;
		TArray<FString> TriggerJsons;
		int32 StepsTotal = 0;
		int32 StepsCompleted = 0;
		bool bFinished = false;
		TFunction<void(FCrowdyKitDeployResult)> OnDone;
	};

	void FinishKitDeploy(const TSharedRef<FKitDeployState>& State, bool bOk, FString Error)
	{
		if (State->bFinished)
		{
			return;
		}
		State->bFinished = true;
		FCrowdyKitDeployResult Result;
		Result.bOk = bOk;
		Result.ErrorMessage = MoveTemp(Error);
		Result.StepsCompleted = State->StepsCompleted;
		Result.StepsTotal = State->StepsTotal;
		if (State->OnDone)
		{
			State->OnDone(MoveTemp(Result));
		}
	}

	void DeployTriggerAt(const TSharedRef<FKitDeployState>& State, int32 Index)
	{
		if (Index >= State->TriggerJsons.Num())
		{
			FinishKitDeploy(State, true, FString());
			return;
		}
		const TSharedPtr<FCrowdyCppClient> Client = State->Client.Pin();
		if (!Client)
		{
			FinishKitDeploy(State, false, TEXT("kit deploy client was released mid-chain"));
			return;
		}
		Client->UpsertAutomationTrigger(State->TriggerJsons[Index],
			[State, Index](FCrowdyCppStudioOpResult StepResult)
			{
				if (!StepResult.bOk)
				{
					FinishKitDeploy(State, false,
						FString::Printf(TEXT("automation trigger %d of %d failed: %s"),
							Index + 1, State->TriggerJsons.Num(), *StepResult.ErrorMessage));
					return;
				}
				State->StepsCompleted++;
				DeployTriggerAt(State, Index + 1);
			});
	}

	void DeployAutomationAt(const TSharedRef<FKitDeployState>& State, int32 Index)
	{
		if (Index >= State->AutomationJsons.Num())
		{
			DeployTriggerAt(State, 0);
			return;
		}
		const TSharedPtr<FCrowdyCppClient> Client = State->Client.Pin();
		if (!Client)
		{
			FinishKitDeploy(State, false, TEXT("kit deploy client was released mid-chain"));
			return;
		}
		Client->UpsertAutomation(State->AutomationJsons[Index],
			[State, Index](FCrowdyCppStudioOpResult StepResult)
			{
				if (!StepResult.bOk)
				{
					FinishKitDeploy(State, false,
						FString::Printf(TEXT("automation %d of %d failed: %s"),
							Index + 1, State->AutomationJsons.Num(), *StepResult.ErrorMessage));
					return;
				}
				State->StepsCompleted++;
				DeployAutomationAt(State, Index + 1);
			});
	}
}

void FCrowdyKitBridge::DeployBundle(const TSharedRef<FCrowdyCppClient>& Client, const FCrowdyKitBundle& Bundle,
	TFunction<void(FCrowdyKitDeployResult)> OnDone)
{
	const TSharedRef<FKitDeployState> State = MakeShared<FKitDeployState>();
	State->Client = Client;
	State->AutomationJsons = Bundle.AutomationJsons;
	State->TriggerJsons = Bundle.TriggerJsons;
	State->StepsTotal = 1 + Bundle.AutomationJsons.Num() + Bundle.TriggerJsons.Num();
	State->OnDone = MoveTemp(OnDone);

	if (!Bundle.bOk)
	{
		FinishKitDeploy(State, false,
			Bundle.ErrorMessage.IsEmpty() ? FString(TEXT("kit bundle is not deployable")) : Bundle.ErrorMessage);
		return;
	}
	if (Bundle.SeedInputJson.IsEmpty())
	{
		FinishKitDeploy(State, false, TEXT("kit bundle has no seed input"));
		return;
	}

	Client->SeedSchema(Bundle.SeedInputJson,
		[State](FCrowdyCppStudioOpResult StepResult)
		{
			if (!StepResult.bOk)
			{
				FinishKitDeploy(State, false, FString::Printf(TEXT("seed failed: %s"), *StepResult.ErrorMessage));
				return;
			}
			State->StepsCompleted++;
			DeployAutomationAt(State, 0);
		});
}
