#pragma once

#include "CoreMinimal.h"
#include "Auth/CrowdyAuthPayloadReaders.h"
#include "CrowdyCppClient.h"
#include "Subsystem/CrowdyAuthentication.h"

/**
 * The runtime half of the pure authentication layer: reducing an app-token payload to the fields the sign-in
 * pipeline adopts. The shape readers live in Auth/CrowdyAuthPayloadReaders.h, which the editor console also uses;
 * this one stays here because it names a bridge type, which no public header may.
 */
namespace CrowdyAuthPayloads
{
	// The app-token payload reduces to the fields the pipeline adopts, so the tail that adopts a token is written once.
	inline FCrowdyAppTokenFields MakeAppTokenFields(const FCrowdyCppAppTokenResult& Result)
	{
		FCrowdyAppTokenFields Fields;
		Fields.AppToken       = Result.AppToken;
		Fields.AppGameTokenID = Result.AppGameTokenID;
		Fields.ExpiresAt      = Result.ExpiresAt;
		Fields.GameApiUrl     = Result.GameApiUrl;
		Fields.GameApiWsUrl   = Result.GameApiWsUrl;
		Fields.LaunchUrl      = Result.LaunchUrl;
		Fields.AuthorizedServerIp4        = Result.AuthorizedServerIp4;
		Fields.AuthorizedServerClientPort = Result.AuthorizedServerClientPort;
		return Fields;
	}
}
