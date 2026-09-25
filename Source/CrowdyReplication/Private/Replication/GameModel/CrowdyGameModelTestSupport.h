#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// Parses a JSON object literal for a test, returning null on malformed input.
inline TSharedPtr<FJsonObject> ParseObject(const FString& Json)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TSharedPtr<FJsonObject> Obj;
	FJsonSerializer::Deserialize(Reader, Obj);
	return Obj;
}

// A client host holding a canned API client, installed on Model so its Game Model calls get as far as being sent.
// Endpoint must be the one the model's API context names. Nothing is delivered until the test polls Client, and
// whatever is still pending when this goes out of scope completes as canceled.
struct FCrowdyGameModelTestClientHost
{
	TStrongObjectPtr<UGameInstance> Instance;
	TStrongObjectPtr<UCrowdyCppClientSubsystem> Host;
	TSharedPtr<FCrowdyCppClient> Client;

	FCrowdyGameModelTestClientHost(UCrowdyGameModelSubsystem* Model, const FString& Endpoint,
		const FString& CannedBody = TEXT("{\"data\":{}}"))
	{
		Instance.Reset(NewObject<UGameInstance>(GetTransientPackage()));
		Host.Reset(NewObject<UCrowdyCppClientSubsystem>(Instance.Get()));
		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		FCrowdyCppClientConfig Config;
		Config.DiscoveryUrl = Settings ? Settings->GetDiscoveryUrl() : FString();
		Config.ApiUrl = Endpoint;
		Client = FCrowdyCppClient::MakeForTest(CannedBody, 200, Config);
		Host->SetClientForTest(Client, Config);
		Model->SetClientHostForTest(Host.Get());
	}

	// Closed while still installed, so a canceled completion that reissues finds this closed client rather than
	// building a real one.
	~FCrowdyGameModelTestClientHost()
	{
		if (Client.IsValid())
		{
			Client->Close();
		}
		Host->SetClientForTest(nullptr, FCrowdyCppClientConfig());
	}

	// Polls until Done reads true or MaxPolls pass; a paged read answers one page per poll at most.
	template <typename FDonePredicate>
	bool PollUntil(FDonePredicate Done, int32 MaxPolls = 200)
	{
		for (int32 Poll = 0; Poll < MaxPolls && !Done(); ++Poll)
		{
			Client->Poll();
		}
		return Done();
	}
};

#endif
