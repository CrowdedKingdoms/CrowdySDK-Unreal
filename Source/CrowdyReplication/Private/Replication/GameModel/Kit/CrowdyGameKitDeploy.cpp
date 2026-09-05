#include "Replication/GameModel/Kit/CrowdyGameKitDeploy.h"

#include "Replication/GameModel/Kit/CrowdyGameKitEmit.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "CrowdyCppClient.h"
#include "CrowdyKitBridge.h"
#include "Containers/Ticker.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"

namespace
{
	// A wall-clock ceiling on a single deploy. Releasing the client completes the step in flight as canceled, so the
	// case this still covers is an endpoint that accepts the request and never answers without the transport timing
	// out; it bounds the run rather than letting it persist for the editor session.
	constexpr double MaxDeploySeconds = 180.0;

	// Owns one kit deploy's client host and its watchdog. The host owns the client and its completion pump, so
	// releasing it here is what stops polling and disposes the client; there is nothing else to unregister. The
	// bridge deploy holds the client only weakly, so there is no reference cycle. A pending pre-network failure (a
	// bad emit, a missing endpoint, or a client that would not construct) is delivered on the first tick so OnDone
	// is always async.
	struct FKitDeployRun
	{
		TSharedPtr<FCrowdyCppAdminClientHost> Host;
		TFunction<void(FCrowdyKitDeployOutcome)> OnDone;
		FCrowdyKitDeployOutcome Pending;
		bool bHasPending = false;
		bool bDone = false;
		double ElapsedSeconds = 0.0;
	};

	void FinishKitDeployRun(const TSharedRef<FKitDeployRun>& Run, const FCrowdyKitDeployOutcome& Outcome)
	{
		if (Run->bDone)
		{
			return;
		}
		Run->bDone = true;

		// Release the host now the deploy is over, which disposes the client and stops its pump. This can run from
		// inside that pump, which is safe: the pump pins the host for the whole tick, so the host outlives the
		// drain that is on the stack and is destroyed when the tick releases its pin.
		Run->Host.Reset();

		TFunction<void(FCrowdyKitDeployOutcome)> Callback = MoveTemp(Run->OnDone);
		Run->OnDone = nullptr;
		if (Callback)
		{
			Callback(Outcome);
		}
	}
}

void CrowdyKitDeployLayers(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers,
	int64 AppId, const FString& SessionId,
	const FString& GameApiUrl, const FString& AdminToken,
	TFunction<void(FCrowdyKitDeployOutcome)> OnDone)
{
	const TSharedRef<FKitDeployRun> Run = MakeShared<FKitDeployRun>();
	Run->OnDone = MoveTemp(OnDone);

	const FCrowdyKitBundle Bundle = CrowdyKitEmitFromPresets(Layers, AppId, SessionId);

	if (!Bundle.bOk)
	{
		Run->Pending = FCrowdyKitDeployOutcome{false, Bundle.ErrorMessage, 0, 0};
		Run->bHasPending = true;
	}
	else if (GameApiUrl.IsEmpty())
	{
		Run->Pending = FCrowdyKitDeployOutcome{false,
			TEXT("No Game API endpoint is configured for this app."), 0, 0};
		Run->bHasPending = true;
	}
	else
	{
		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		FCrowdyCppClientConfig ClientConfig;
		ClientConfig.ApiUrl = GameApiUrl;
		ClientConfig.DiscoveryUrl = Settings ? Settings->GetDiscoveryUrl() : FString();
		Run->Host = FCrowdyCppAdminClientHost::Create(ClientConfig, AdminToken);
		if (!Run->Host.IsValid())
		{
			Run->Pending = FCrowdyKitDeployOutcome{false,
				TEXT("Could not construct the Game API client for the kit deploy."), 0, 0};
			Run->bHasPending = true;
		}
	}

	// This ticker carries only the deploy's own bookkeeping: it delivers a pending pre-network failure on its first
	// call and bounds the run. The completion pump belongs to the host. It removes itself once the deploy is done.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Run](float DeltaTime) -> bool
		{
			if (Run->bHasPending)
			{
				Run->bHasPending = false;
				FinishKitDeployRun(Run, Run->Pending);
				return false;
			}
			// Bound the deploy: if the completion never arrives, finish with a timeout so the run unregisters, the
			// host is released, and the caller is unstuck rather than waiting for the rest of the editor session.
			Run->ElapsedSeconds += DeltaTime;
			if (!Run->bDone && Run->ElapsedSeconds >= MaxDeploySeconds)
			{
				FinishKitDeployRun(Run, FCrowdyKitDeployOutcome{false,
					TEXT("The Game Kit deploy timed out before the server responded."), 0, 0});
				return false;
			}
			return !Run->bDone;
		}));

	if (Run->bHasPending || !Run->Host.IsValid())
	{
		// A pre-network failure is queued (or there is no client to deploy on); the ticker delivers it.
		return;
	}

	FCrowdyKitBridge::DeployBundle(Run->Host->GetClient(), Bundle,
		[Run](FCrowdyKitDeployResult Result)
		{
			FinishKitDeployRun(Run, FCrowdyKitDeployOutcome{Result.bOk, Result.ErrorMessage,
				Result.StepsCompleted, Result.StepsTotal});
		});
}
