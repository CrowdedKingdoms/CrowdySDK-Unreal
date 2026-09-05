#include "Network/UDP/FCrowdyUDPEndpoint.h"

#include "CrowdyNetLog.h"
#include "Dom/JsonObject.h"

namespace CrowdyUDPEndpoint
{
	bool ParseServerAssignment(const TSharedPtr<FJsonObject>& Data, FCrowdyUDPEndpoint& OutEndpoint, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ServerObject = nullptr;
		if (!Data.IsValid()
			|| !Data->TryGetObjectField(TEXT("serverWithLeastClients"), ServerObject)
			|| !ServerObject->IsValid())
		{
			OutError = TEXT("the answer named no replication server");
			return false;
		}

		FCrowdyUDPEndpoint Parsed;

		if (!(*ServerObject)->TryGetStringField(TEXT("ip6"), Parsed.IPv6Address))
		{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("The replication server reported no IPv6 address; falling back to IPv4."));
		}

		if (!(*ServerObject)->TryGetStringField(TEXT("ip4"), Parsed.IPv4Address))
		{
			OutError = TEXT("the answer carried neither an IPv4 nor an IPv6 address");
			return false;
		}

		if (!(*ServerObject)->TryGetNumberField(TEXT("clientPort"), Parsed.Port))
		{
			OutError = TEXT("the answer carried no client port");
			return false;
		}

		OutEndpoint = MoveTemp(Parsed);
		return true;
	}
}
