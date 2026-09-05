// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyEffectActions.h"

#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h" // complete type for TWeakObjectPtr<UCrowdyEffect> assignment

UCrowdyApplyEffectAction* UCrowdyApplyEffectAction::ApplyEffect(UObject* WorldContext, UCrowdyEffect* InEffect,
	UObject* InTarget, UObject* InSource, const TMap<FName, FString>& InOverrides, float InLevel, const FString& InSessionId)
{
	UCrowdyApplyEffectAction* Action = NewObject<UCrowdyApplyEffectAction>();
	Action->WorldContextObject = WorldContext;
	Action->Effect = InEffect;
	Action->Target = InTarget;
	Action->Source = InSource;
	Action->Overrides = InOverrides;
	Action->Level = InLevel;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyApplyEffectAction::Activate()
{
	TWeakObjectPtr<UCrowdyApplyEffectAction> WeakThis(this);
	FString Error;
	// This callback runs exactly once whether the apply was sent on its own or merged into a coalesced call shared
	// with other nodes, so the pins below fire once per node either way. It may run later than the call to
	// ApplyInternal by up to the effect's coalesce window.
	const bool bDispatched = UCrowdyEffects::ApplyInternal(
		WorldContextObject.Get(), Target.Get(), FString(), Effect.Get(), Source.Get(), Overrides, Level, SessionId,
		[WeakThis](FCrowdyInvokeResult Result)
		{
			UCrowdyApplyEffectAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				Action->Succeeded.Broadcast(Result.bSuccess, Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				Action->Failed.Broadcast(Result.bSuccess, Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		},
		Error);

	// A synchronous resolve/marshal failure never reaches the server and never runs the callback above.
	if (!bDispatched)
	{
		Failed.Broadcast(false, FString(), Error);
		SetReadyToDestroy();
	}
}
