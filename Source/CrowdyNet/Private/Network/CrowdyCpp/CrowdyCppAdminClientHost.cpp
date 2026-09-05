#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"

TSharedPtr<FCrowdyCppAdminClientHost> FCrowdyCppAdminClientHost::Create(const FCrowdyCppClientConfig& Config,
	const FString& AdminToken)
{
	TSharedPtr<FCrowdyCppClient> NewClient = FCrowdyCppClient::Make(Config);
	if (!NewClient.IsValid())
	{
		return nullptr;
	}
	// Admin authoring operations are gameplay-root operations issued with a manage_apps bearer, so the admin token
	// is the game-plane token here rather than an identity one.
	NewClient->SetGameToken(AdminToken);

	TSharedPtr<FCrowdyCppAdminClientHost> Host = MakeShareable(new FCrowdyCppAdminClientHost());
	Host->Client = MoveTemp(NewClient);

	// The pump holds the host weakly and unregisters itself once the host is gone, so the host's own destruction
	// never has to reach back into a ticker that may be running. Pinning the host for the whole tick means a
	// completion that releases the caller's last reference cannot free the host underneath the drain; the host
	// dies when this tick releases its pin, and the next tick unregisters.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakHost = TWeakPtr<FCrowdyCppAdminClientHost>(Host)](float DeltaTime) -> bool
		{
			const TSharedPtr<FCrowdyCppAdminClientHost> Pinned = WeakHost.Pin();
			if (!Pinned.IsValid())
			{
				return false;
			}
			if (const TSharedPtr<FCrowdyCppClient> PinnedClient = Pinned->Client)
			{
				PinnedClient->Poll();
			}
			return true;
		}));

	return Host;
}

FCrowdyCppAdminClientHost::~FCrowdyCppAdminClientHost()
{
	if (Client.IsValid())
	{
		// Dispose before releasing so a request still in flight completes as canceled rather than staying queued on
		// a client nobody will pump again.
		Client->Close();
		Client.Reset();
	}
}

TSharedRef<FCrowdyCppClient> FCrowdyCppAdminClientHost::GetClient() const
{
	return Client.ToSharedRef();
}
