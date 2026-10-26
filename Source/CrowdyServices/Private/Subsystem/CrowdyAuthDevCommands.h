#pragma once

#include "CoreMinimal.h"
#include "CrowdyServicesLog.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "UObject/Object.h"
#include "CrowdyAuthDevCommands.generated.h"

class UCrowdyGameSession;

/** Logs how a console-driven sign-in ended. Rooted while the call is in flight, since nothing else holds it. */
UCLASS(Transient)
class UCrowdyAuthDevCommandSink : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleSuccess(FCrowdyAuthResult Result)
	{
		UE_LOG(LogCrowdyServices, Log, TEXT("crowdy.auth.signin.file: signed in. UserID=%lld"), Result.UserID);
		Release();
	}

	UFUNCTION()
	void HandleError(FString Message)
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("crowdy.auth.signin.file: sign-in failed: %s"), *Message);
		Release();
	}

private:
	void Release()
	{
		if (IsRooted())
		{
			RemoveFromRoot();
		}
	}
};

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
namespace CrowdyAuthDev
{
	/** Reads the email and password a JSON file holds. OutError names the problem and never carries the password. */
	bool ReadCredentialsFile(const FString& Path, FString& OutEmail, FString& OutPassword, FString& OutError);

	/** Signs in through the same Login call the Login async action makes, and logs how it ended. */
	void SignIn(UCrowdyAuthentication& Auth, const FString& Email, const FString& Password);

	/** One line describing the session: who is signed in, whether an app token is held and when it lapses, and the pause state. Never the tokens. */
	FString DescribeStatus(const UCrowdyAuthentication& Auth, const UCrowdyGameSession& Session);
}
#endif
