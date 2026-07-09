#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "CrowdyHostQueryActions.generated.h"

class AActor;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCrowdyHostCheckPin);

/**
 * Latent node: server-validated "is this actor the host?".
 *
 * Unlike UCrowdyUtilities::IsCrowdyEntityHost (a client-side deterministic-GUID compare),
 * this asks the Game API. For the local player's own entity it uses amIGameHost; for any
 * other actor it resolves the actor's owner userId (actor(uuid)) and compares it to the
 * elected host userId. Exactly one of OnIsHost / OnIsNotHost fires on a definite answer;
 * OnFailed fires when the answer can't be determined (unresolved entity, network/GraphQL
 * error, or no host elected yet).
 *
 * C++ callers should use UCrowdyHostSubsystem::CheckEntityIsHost directly.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyIsEntityHostServer : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, meta=(DisplayName="Is Host"))
	FCrowdyHostCheckPin OnIsHost;

	UPROPERTY(BlueprintAssignable, meta=(DisplayName="Is Not Host"))
	FCrowdyHostCheckPin OnIsNotHost;

	UPROPERTY(BlueprintAssignable, meta=(DisplayName="Failed"))
	FCrowdyHostCheckPin OnFailed;

	UFUNCTION(BlueprintCallable,
		meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DefaultToSelf="Entity"),
		Category="Crowdy SDK|Session", DisplayName="Is Crowdy Entity Host (Server)")
	static UCrowdyIsEntityHostServer* IsCrowdyEntityHostServer(UObject* WorldContextObject, AActor* Entity);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<AActor>  Entity;

	void Finish(bool bSuccess, bool bIsHost);
};
