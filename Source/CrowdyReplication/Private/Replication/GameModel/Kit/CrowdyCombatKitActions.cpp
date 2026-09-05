// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyCombatKitActions.h"

#include "CrowdyGameModelLog.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/Kit/CrowdyCombatKitNames.h"
#include "Replication/GameModel/Kit/CrowdyKitActionSupport.h"

UCrowdySpawnCombatantAction* UCrowdySpawnCombatantAction::SpawnCombatant(UObject* WorldContext, AActor* Actor,
	const FString& InTypePrefix, int32 Hp, int32 MaxHp, int32 Attack, int32 Defense, const FString& InSessionId)
{
	UCrowdySpawnCombatantAction* Action = NewObject<UCrowdySpawnCombatantAction>();
	Action->WorldContextObject = WorldContext;
	Action->Target = Actor;
	Action->TypePrefix = InTypePrefix;
	Action->StartingHp = Hp;
	Action->StartingMaxHp = MaxHp;
	Action->StartingAttack = Attack;
	Action->StartingDefense = Defense;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdySpawnCombatantAction::Activate()
{
	const UObject* Ctx = Target.IsValid() ? static_cast<const UObject*>(Target.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* ModelPtr = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!ModelPtr)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Target.IsValid())
	{
		Failed.Broadcast(FString(), TEXT("no Actor to bind the combatant to"));
		SetReadyToDestroy();
		return;
	}

	FGuid NetID;
	if (!ModelPtr->ResolveTargetNetID(Target.Get(), NetID))
	{
		Failed.Broadcast(FString(), TEXT("Actor is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	Model = ModelPtr;
	BoundNetID = NetID;

	const FString TypeName = CrowdyCombatKitNames::CombatantTypeName(TypePrefix);
	const FString DisplayName = Target->GetName();

	TWeakObjectPtr<UCrowdySpawnCombatantAction> WeakThis(this);
	ModelPtr->CreateDataContainer(TypeName, DisplayName, SessionId, FString(),
		[WeakThis](bool bOk, const FString& NewContainerId)
		{
			UCrowdySpawnCombatantAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk || NewContainerId.IsEmpty())
			{
				Action->Failed.Broadcast(FString(),
					TEXT("the server rejected the combatant container create (check sign-in and app scope)"));
				Action->SetReadyToDestroy();
				return;
			}
			// The create can land after the world was torn down (the Game API client outlives it), and binding the
			// container or writing its starting stats would then touch a subsystem whose caches are already cleared.
			// Stop the chain here, but still report it so this node's pins fire.
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Action->Model);
			if (!M)
			{
				Action->Failed.Broadcast(FString(),
					TEXT("the world was torn down before the combatant container could be bound"));
				Action->SetReadyToDestroy();
				return;
			}
			// The entity may have unregistered mid-create (its actor was destroyed), leaving BoundNetID stale.
			// Re-validate that the Actor is still live and still resolves to the same entity before binding, so a
			// dead NetID is never mapped to the new container (a permanent stale entry that a later attack/read
			// would resolve to a gone actor).
			FGuid CurrentNetID;
			if (!Action->Target.IsValid() || !M->ResolveTargetNetID(Action->Target.Get(), CurrentNetID)
				|| CurrentNetID != Action->BoundNetID)
			{
				Action->Failed.Broadcast(FString(),
					TEXT("the Actor was unregistered before its combatant container could be bound"));
				Action->SetReadyToDestroy();
				return;
			}
			Action->CreatedContainerId = NewContainerId;
			M->BindEntityContainer(Action->BoundNetID, NewContainerId);
			Action->BuildPendingWrites();
			Action->ApplyNextProperty();
		});
}

void UCrowdySpawnCombatantAction::BuildPendingWrites()
{
	PendingWrites.Reset();
	WriteIndex = 0;

	// The owner mirror is best-effort: the server already pins the record owner to the caller on create, and the
	// mirror property may be gated, so a rejection here must not fail the spawn.
	const int64 UserId = Model.IsValid() ? Model->GetLocalUserId() : 0;
	if (UserId != 0)
	{
		PendingWrites.Add({ CrowdyCombatKitNames::Keys::OwnerUserId, TEXT("int"),
			FString::Printf(TEXT("%lld"), UserId), false });
	}

	// The combat_key is the status-effect join key: an effect's target_key must equal the target combatant's
	// combat_key for the tick automation to bind them. Seed it to the bound NetID string. That value is
	// deterministic and unique per combatant and is the SAME string every client computes from the actor, so Apply
	// Status Effect derives a target's combat_key from the target actor's NetID with no extra server read. This is
	// fatal: without a combat_key no effect can ever target this combatant.
	PendingWrites.Add({ CrowdyCombatKitNames::Keys::CombatKey, TEXT("string"),
		CrowdyKitActionSupport::MakeStringValueJson(BoundNetID.ToString()), true });

	PendingWrites.Add({ CrowdyCombatKitNames::Keys::Hp, TEXT("int"), FString::FromInt(StartingHp), true });
	PendingWrites.Add({ CrowdyCombatKitNames::Keys::MaxHp, TEXT("int"), FString::FromInt(StartingMaxHp), true });
	PendingWrites.Add({ CrowdyCombatKitNames::Keys::Attack, TEXT("int"), FString::FromInt(StartingAttack), true });
	PendingWrites.Add({ CrowdyCombatKitNames::Keys::Defense, TEXT("int"), FString::FromInt(StartingDefense), true });
}

void UCrowdySpawnCombatantAction::ApplyNextProperty()
{
	// Each write is issued from the previous write's completion, which can land after the world was torn down: the
	// remaining writes belong to a world that no longer exists, so stop and report rather than issuing them.
	UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
	if (!M)
	{
		Failed.Broadcast(FString(), TEXT("the Game Model subsystem went away mid-spawn"));
		SetReadyToDestroy();
		return;
	}
	if (WriteIndex >= PendingWrites.Num())
	{
		Succeeded.Broadcast(CreatedContainerId, FString());
		SetReadyToDestroy();
		return;
	}

	const FPendingWrite Write = PendingWrites[WriteIndex];
	TWeakObjectPtr<UCrowdySpawnCombatantAction> WeakThis(this);
	M->SetDataProperty(CreatedContainerId, Write.Key, Write.ValueType, Write.ValueJson,
		[WeakThis, Key = Write.Key, bFatal = Write.bFatal](bool bWriteOk)
		{
			UCrowdySpawnCombatantAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bWriteOk && bFatal)
			{
				Action->Failed.Broadcast(FString(),
					FString::Printf(TEXT("the server rejected setting combatant property '%s'"), *Key));
				Action->SetReadyToDestroy();
				return;
			}
			if (!bWriteOk)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("Spawn Combatant: optional property '%s' was not set; continuing"), *Key);
			}
			++Action->WriteIndex;
			Action->ApplyNextProperty();
		});
}

UCrowdyCombatAttackAction* UCrowdyCombatAttackAction::CombatAttack(UObject* WorldContext, AActor* InAttacker,
	AActor* InTarget, const FString& InTypePrefix, const FString& InSessionId)
{
	UCrowdyCombatAttackAction* Action = NewObject<UCrowdyCombatAttackAction>();
	Action->WorldContextObject = WorldContext;
	Action->Attacker = InAttacker;
	Action->Target = InTarget;
	Action->TypePrefix = InTypePrefix;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

TSharedPtr<FJsonObject> UCrowdyCombatAttackAction::BuildAttackParams(const FString& TargetContainerId)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	// target_id is a container_ref, which on the wire is the bare container-id string.
	Out->SetStringField(TEXT("target_id"), TargetContainerId);
	return Out;
}

void UCrowdyCombatAttackAction::Activate()
{
	const UObject* Ctx = Attacker.IsValid() ? static_cast<const UObject*>(Attacker.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!Model)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Attacker.IsValid() || !Target.IsValid())
	{
		Failed.Broadcast(FString(), TEXT("Combat Attack needs both an Attacker and a Target actor"));
		SetReadyToDestroy();
		return;
	}

	FGuid AttackerNetID;
	if (!Model->ResolveTargetNetID(Attacker.Get(), AttackerNetID))
	{
		Failed.Broadcast(FString(), TEXT("Attacker is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FGuid TargetNetID;
	if (!Model->ResolveTargetNetID(Target.Get(), TargetNetID))
	{
		Failed.Broadcast(FString(), TEXT("Target is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FString TargetContainerId;
	if (!Model->TryGetContainerId(TargetNetID, TargetContainerId) || TargetContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Target has no bound combatant container (spawn the combatant first)"));
		SetReadyToDestroy();
		return;
	}

	// The attack function's self is the ATTACKER (the owner_of_self policy authorizes against the caller's own
	// container), but it mutates the TARGET's hp/alive, not self. The returned mutationsApplied carry only key +
	// newValue with no container id, so applying them locally would corrupt whichever container they were applied
	// to. Route through the plain non-applying Invoke against the attacker's container, and on success refresh the
	// TARGET (the real container the mutations describe) so its hp/alive update locally and fire OnRep.
	FString AttackerContainerId;
	if (!Model->TryGetContainerId(AttackerNetID, AttackerContainerId) || AttackerContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Attacker has no bound combatant container (spawn the combatant first)"));
		SetReadyToDestroy();
		return;
	}

	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyCombatKitNames::AttackFunctionName(TypePrefix);
	Req.SelfContainerId = AttackerContainerId;
	Req.SessionId = SessionId;
	Req.Params = BuildAttackParams(TargetContainerId);

	TWeakObjectPtr<UCrowdyCombatAttackAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	const FGuid CapturedTargetNetID = TargetNetID;
	Model->Invoke(Req, [WeakThis, WeakModel, CapturedTargetNetID](FCrowdyInvokeResult Result)
		{
			UCrowdyCombatAttackAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				// Re-pull the target's container and apply it, firing the target's OnRep for the changed hp/alive.
				// The attack already committed on the server, so a world torn down during the round-trip only means
				// there is nothing left to refresh locally; the caller is still told the outcome either way.
				if (UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel))
				{
					M->HandleModelChanged(CapturedTargetNetID);
				}
				Action->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				Action->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyGetCombatantStateAction* UCrowdyGetCombatantStateAction::GetCombatantState(UObject* WorldContext,
	AActor* Actor, const FString& InSessionId)
{
	UCrowdyGetCombatantStateAction* Action = NewObject<UCrowdyGetCombatantStateAction>();
	Action->WorldContextObject = WorldContext;
	Action->Target = Actor;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

FCrowdyCombatantState UCrowdyGetCombatantStateAction::ParseCombatantState(const TSharedPtr<FJsonObject>& State,
	const FString& ContainerId)
{
	FCrowdyCombatantState Out;
	Out.ContainerId = ContainerId;
	if (!State.IsValid())
	{
		return Out;
	}

	double Value = 0.0;
	if (State->TryGetNumberField(CrowdyCombatKitNames::Keys::Hp, Value))
	{
		Out.Hp = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyCombatKitNames::Keys::MaxHp, Value))
	{
		Out.MaxHp = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyCombatKitNames::Keys::Attack, Value))
	{
		Out.Attack = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyCombatKitNames::Keys::Defense, Value))
	{
		Out.Defense = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	bool bAliveValue = false;
	if (State->TryGetBoolField(CrowdyCombatKitNames::Keys::Alive, bAliveValue))
	{
		Out.bAlive = bAliveValue;
	}
	return Out;
}

void UCrowdyGetCombatantStateAction::Activate()
{
	const UObject* Ctx = Target.IsValid() ? static_cast<const UObject*>(Target.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!Model)
	{
		Failed.Broadcast(FCrowdyCombatantState(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Target.IsValid())
	{
		Failed.Broadcast(FCrowdyCombatantState(), TEXT("no Actor to read combatant state from"));
		SetReadyToDestroy();
		return;
	}

	FGuid NetID;
	if (!Model->ResolveTargetNetID(Target.Get(), NetID))
	{
		Failed.Broadcast(FCrowdyCombatantState(), TEXT("Actor is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FString ContainerId;
	if (!Model->TryGetContainerId(NetID, ContainerId) || ContainerId.IsEmpty())
	{
		Failed.Broadcast(FCrowdyCombatantState(),
			TEXT("Actor has no bound combatant container (spawn the combatant first)"));
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyGetCombatantStateAction> WeakThis(this);
	Model->PullContainerState(ContainerId, [WeakThis, ContainerId](bool bOk, TSharedPtr<FJsonObject> State)
	{
		UCrowdyGetCombatantStateAction* Action = WeakThis.Get();
		if (!Action)
		{
			return;
		}
		if (!bOk || !State.IsValid())
		{
			Action->Failed.Broadcast(FCrowdyCombatantState(),
				TEXT("failed to read combatant state from the server"));
			Action->SetReadyToDestroy();
			return;
		}
		Action->Succeeded.Broadcast(ParseCombatantState(State, ContainerId), FString());
		Action->SetReadyToDestroy();
	});
}

UCrowdyRespawnCombatantAction* UCrowdyRespawnCombatantAction::RespawnCombatant(UObject* WorldContext, AActor* Actor,
	const FString& InTypePrefix, const FString& InSessionId)
{
	UCrowdyRespawnCombatantAction* Action = NewObject<UCrowdyRespawnCombatantAction>();
	Action->WorldContextObject = WorldContext;
	Action->Target = Actor;
	Action->TypePrefix = InTypePrefix;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyRespawnCombatantAction::Activate()
{
	const UObject* Ctx = Target.IsValid() ? static_cast<const UObject*>(Target.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!Model)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Target.IsValid())
	{
		Failed.Broadcast(FString(), TEXT("no Actor to respawn"));
		SetReadyToDestroy();
		return;
	}

	FGuid NetID;
	if (!Model->ResolveTargetNetID(Target.Get(), NetID))
	{
		Failed.Broadcast(FString(), TEXT("Actor is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FString ContainerId;
	if (!Model->TryGetContainerId(NetID, ContainerId) || ContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Actor has no bound combatant container (spawn the combatant first)"));
		SetReadyToDestroy();
		return;
	}

	// respawn mutates the combatant itself (hp -> max_hp, alive -> true), so the invoke self IS the actor's own
	// container. The confirmed mutations describe that same container but carry no container id, so route through
	// the plain non-applying Invoke and re-pull this actor on success to fire its OnRep.
	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyCombatKitNames::RespawnFunctionName(TypePrefix);
	Req.SelfContainerId = ContainerId;
	Req.SessionId = SessionId;

	TWeakObjectPtr<UCrowdyRespawnCombatantAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	const FGuid CapturedNetID = NetID;
	Model->Invoke(Req, [WeakThis, WeakModel, CapturedNetID](FCrowdyInvokeResult Result)
		{
			UCrowdyRespawnCombatantAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				// The server call already committed, so a world torn down during the round-trip only means there is
					// nothing left to refresh locally; the caller is still told the outcome either way.
				if (UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel))
				{
					M->HandleModelChanged(CapturedNetID);
				}
				Action->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				// A denial (the combatant is still alive) reads back verbatim from the server here.
				Action->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyReviveCombatantAction* UCrowdyReviveCombatantAction::ReviveCombatant(UObject* WorldContext, AActor* InTarget,
	const FString& InTypePrefix, const FString& InSessionId)
{
	UCrowdyReviveCombatantAction* Action = NewObject<UCrowdyReviveCombatantAction>();
	Action->WorldContextObject = WorldContext;
	Action->Target = InTarget;
	Action->TypePrefix = InTypePrefix;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyReviveCombatantAction::Activate()
{
	const UObject* Ctx = Target.IsValid() ? static_cast<const UObject*>(Target.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!Model)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Target.IsValid())
	{
		Failed.Broadcast(FString(), TEXT("no Target Actor to revive"));
		SetReadyToDestroy();
		return;
	}

	FGuid NetID;
	if (!Model->ResolveTargetNetID(Target.Get(), NetID))
	{
		Failed.Broadcast(FString(), TEXT("Target is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FString ContainerId;
	if (!Model->TryGetContainerId(NetID, ContainerId) || ContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Target has no bound combatant container (spawn the combatant first)"));
		SetReadyToDestroy();
		return;
	}

	// revive mutates the TARGET combatant (hp -> max_hp, alive -> true) but is authorized by a team/group
	// permission the CALLER holds, so the invoke self is the target's own container and the caller's permission
	// gates it. revive only exists when the kit was deployed with a revive group; a kit without one returns a
	// server error on Failed. Re-pull the target on success so its OnRep fires.
	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyCombatKitNames::ReviveFunctionName(TypePrefix);
	Req.SelfContainerId = ContainerId;
	Req.SessionId = SessionId;

	TWeakObjectPtr<UCrowdyReviveCombatantAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	const FGuid CapturedNetID = NetID;
	Model->Invoke(Req, [WeakThis, WeakModel, CapturedNetID](FCrowdyInvokeResult Result)
		{
			UCrowdyReviveCombatantAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				// The server call already committed, so a world torn down during the round-trip only means there is
					// nothing left to refresh locally; the caller is still told the outcome either way.
				if (UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel))
				{
					M->HandleModelChanged(CapturedNetID);
				}
				Action->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				Action->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdySyncCombatantAction* UCrowdySyncCombatantAction::SyncCombatant(UObject* WorldContext, AActor* Actor,
	const FString& InTypePrefix, int32 Hp, const FString& InSessionId)
{
	UCrowdySyncCombatantAction* Action = NewObject<UCrowdySyncCombatantAction>();
	Action->WorldContextObject = WorldContext;
	Action->Target = Actor;
	Action->TypePrefix = InTypePrefix;
	Action->SyncHp = Hp;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

TSharedPtr<FJsonObject> UCrowdySyncCombatantAction::BuildSyncParams(int32 Hp)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	// hp is a plain int param; the server clamps it to 0..max_hp.
	Out->SetNumberField(TEXT("hp"), Hp);
	return Out;
}

void UCrowdySyncCombatantAction::Activate()
{
	const UObject* Ctx = Target.IsValid() ? static_cast<const UObject*>(Target.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!Model)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Target.IsValid())
	{
		Failed.Broadcast(FString(), TEXT("no Actor to sync"));
		SetReadyToDestroy();
		return;
	}

	FGuid NetID;
	if (!Model->ResolveTargetNetID(Target.Get(), NetID))
	{
		Failed.Broadcast(FString(), TEXT("Actor is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FString ContainerId;
	if (!Model->TryGetContainerId(NetID, ContainerId) || ContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Actor has no bound combatant container (spawn the combatant first)"));
		SetReadyToDestroy();
		return;
	}

	// sync_combatant persists host-simulated hp (clamped 0..max_hp, alive re-derived) on the combatant itself. It
	// only exists when the kit was deployed hostSynced and is enforced is_host server-side, so a non-host caller
	// reads back the server's denial on Failed. Re-pull this actor on success to fire its OnRep.
	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyCombatKitNames::SyncFunctionName(TypePrefix);
	Req.SelfContainerId = ContainerId;
	Req.SessionId = SessionId;
	Req.Params = BuildSyncParams(SyncHp);

	TWeakObjectPtr<UCrowdySyncCombatantAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	const FGuid CapturedNetID = NetID;
	Model->Invoke(Req, [WeakThis, WeakModel, CapturedNetID](FCrowdyInvokeResult Result)
		{
			UCrowdySyncCombatantAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				// The server call already committed, so a world torn down during the round-trip only means there is
					// nothing left to refresh locally; the caller is still told the outcome either way.
				if (UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel))
				{
					M->HandleModelChanged(CapturedNetID);
				}
				Action->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				Action->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyApplyStatusEffectAction* UCrowdyApplyStatusEffectAction::ApplyStatusEffect(UObject* WorldContext,
	AActor* InCaster, AActor* InTarget, const FString& InTypePrefix, const FString& InEffectId, int32 InMagnitude,
	int32 InTicks, const FString& InSessionId)
{
	UCrowdyApplyStatusEffectAction* Action = NewObject<UCrowdyApplyStatusEffectAction>();
	Action->WorldContextObject = WorldContext;
	Action->Caster = InCaster;
	Action->Target = InTarget;
	Action->TypePrefix = InTypePrefix;
	Action->EffectId = InEffectId;
	Action->Magnitude = InMagnitude;
	Action->Ticks = InTicks;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

TSharedPtr<FJsonObject> UCrowdyApplyStatusEffectAction::BuildApplyEffectParams(const FString& TargetKey,
	const FString& EffectId, int32 Magnitude, int32 Ticks)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	// target_key and effect_id are string params; magnitude and ticks are int params. target_key is the target
	// combatant's combat_key, which the selector automation matches to join the effect to the combatant.
	Out->SetStringField(TEXT("target_key"), TargetKey);
	Out->SetStringField(TEXT("effect_id"), EffectId);
	Out->SetNumberField(TEXT("magnitude"), Magnitude);
	Out->SetNumberField(TEXT("ticks"), Ticks);
	return Out;
}

void UCrowdyApplyStatusEffectAction::Activate()
{
	const UObject* Ctx = Caster.IsValid() ? static_cast<const UObject*>(Caster.Get()) : WorldContextObject.Get();
	UCrowdyGameModelSubsystem* ModelPtr = CrowdyKitActionSupport::ResolveModel(Ctx);
	if (!ModelPtr)
	{
		Failed.Broadcast(FString(), FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (!Caster.IsValid() || !Target.IsValid())
	{
		Failed.Broadcast(FString(), FString(),
			TEXT("Apply Status Effect needs both a Caster and a Target actor"));
		SetReadyToDestroy();
		return;
	}

	FGuid CasterNetID;
	if (!ModelPtr->ResolveTargetNetID(Caster.Get(), CasterNetID))
	{
		Failed.Broadcast(FString(), FString(), TEXT("Caster is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	FGuid TargetNetID;
	if (!ModelPtr->ResolveTargetNetID(Target.Get(), TargetNetID))
	{
		Failed.Broadcast(FString(), FString(), TEXT("Target is not a registered Game Model entity"));
		SetReadyToDestroy();
		return;
	}

	Model = ModelPtr;
	// The target combat_key is the target's NetID string, the same value Spawn Combatant seeded as the target
	// combatant's combat_key. Deriving it here avoids an extra server read to look the target combatant up.
	TargetKey = TargetNetID.ToString();

	const FString EffectTypeName = CrowdyCombatKitNames::EffectTypeName(TypePrefix);
	const FString DisplayName = FString::Printf(TEXT("Effect %s"), *EffectId);

	TWeakObjectPtr<UCrowdyApplyStatusEffectAction> WeakThis(this);
	ModelPtr->CreateDataContainer(EffectTypeName, DisplayName, SessionId, FString(),
		[WeakThis](bool bOk, const FString& NewContainerId)
		{
			UCrowdyApplyStatusEffectAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk || NewContainerId.IsEmpty() || !Action->Model.IsValid())
			{
				Action->Failed.Broadcast(FString(), FString(),
					TEXT("the server rejected the status-effect container create (check sign-in and app scope)"));
				Action->SetReadyToDestroy();
				return;
			}
			Action->EffectContainerId = NewContainerId;
			Action->SeedOwnerThenApply();
		});
}

void UCrowdyApplyStatusEffectAction::SeedOwnerThenApply()
{
	// Reached from the create completion, which can land after the world was torn down; the mirror write and the
	// apply below belong to that world, so stop and report instead of issuing them.
	UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
	if (!M)
	{
		Failed.Broadcast(FString(), FString(), TEXT("the Game Model subsystem went away mid-apply"));
		SetReadyToDestroy();
		return;
	}

	// The owner mirror is best-effort: the server already pins the record owner to the caller on create, and the
	// apply policy checks owner_of_self against that pinned owner, so a rejected mirror write must not fail the
	// apply. Invoke apply_effect whether or not the mirror write lands.
	const int64 UserId = M->GetLocalUserId();
	if (UserId == 0)
	{
		InvokeApply();
		return;
	}

	TWeakObjectPtr<UCrowdyApplyStatusEffectAction> WeakThis(this);
	M->SetDataProperty(EffectContainerId, CrowdyCombatKitNames::Keys::OwnerUserId, TEXT("int"),
		FString::Printf(TEXT("%lld"), UserId), [WeakThis](bool bWriteOk)
		{
			UCrowdyApplyStatusEffectAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bWriteOk)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("Apply Status Effect: optional owner_user_id mirror was not set; continuing"));
			}
			Action->InvokeApply();
		});
}

void UCrowdyApplyStatusEffectAction::InvokeApply()
{
	// Also reached from a completion (the owner-mirror write), so the same rule applies: no world session, no invoke.
	UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
	if (!M)
	{
		Failed.Broadcast(EffectContainerId, FString(), TEXT("the Game Model subsystem went away mid-apply"));
		SetReadyToDestroy();
		return;
	}

	// apply_effect's self is the caller-owned effect container; it records target_key/effect_id/magnitude/ticks so
	// the server's interval automation applies the damage-over-time tick by tick. The target is mutated server-side
	// by that automation, not here, so nothing is applied locally on the target; cross-client convergence for the
	// affected combatant lands on the next state pull. A live notification carrier for effect application is a
	// deferred follow-up (mirroring the Combat Attack cross-client note).
	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyCombatKitNames::ApplyEffectFunctionName(TypePrefix);
	Req.SelfContainerId = EffectContainerId;
	Req.SessionId = SessionId;
	Req.Params = BuildApplyEffectParams(TargetKey, EffectId, Magnitude, Ticks);

	TWeakObjectPtr<UCrowdyApplyStatusEffectAction> WeakThis(this);
	M->Invoke(Req, [WeakThis](FCrowdyInvokeResult Result)
		{
			UCrowdyApplyStatusEffectAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				Action->Succeeded.Broadcast(Action->EffectContainerId, Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				// The effect container exists but the arm was refused; report its id so the caller can clean it up.
				Action->Failed.Broadcast(Action->EffectContainerId, Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}
