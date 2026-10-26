#include "Subsystem/CrowdyAuthDevCommands.h"

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/CrowdyGameSession.h"

bool CrowdyAuthDev::ReadCredentialsFile(const FString& Path, FString& OutEmail, FString& OutPassword, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("No readable file at '%s'."), *Path);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		OutError = TEXT("The file is not a JSON object.");
		return false;
	}

	FString Email;
	FString Password;
	if (!Root->TryGetStringField(TEXT("email"), Email) || Email.IsEmpty())
	{
		OutError = TEXT("The file has no \"email\" string.");
		return false;
	}
	if (!Root->TryGetStringField(TEXT("password"), Password) || Password.IsEmpty())
	{
		OutError = TEXT("The file has no \"password\" string.");
		return false;
	}

	OutEmail = MoveTemp(Email);
	OutPassword = MoveTemp(Password);
	return true;
}

void CrowdyAuthDev::SignIn(UCrowdyAuthentication& Auth, const FString& Email, const FString& Password)
{
	UCrowdyAuthDevCommandSink* Sink = NewObject<UCrowdyAuthDevCommandSink>();
	Sink->AddToRoot();

	FOnAuthSuccess OnSuccess;
	OnSuccess.BindDynamic(Sink, &UCrowdyAuthDevCommandSink::HandleSuccess);
	FOnAuthError OnError;
	OnError.BindDynamic(Sink, &UCrowdyAuthDevCommandSink::HandleError);
	Auth.Login(Email, Password, OnSuccess, OnError);
}

FString CrowdyAuthDev::DescribeStatus(const UCrowdyAuthentication& Auth, const UCrowdyGameSession& Session)
{
	const FString Expiry = Session.GetAppTokenExpiresAt();
	FString Paused = Session.IsAppPaused() ? TEXT("true") : TEXT("false");
	if (Session.IsAppPaused() && !Session.GetAppPausedReason().IsEmpty())
	{
		Paused += FString::Printf(TEXT(" (%s)"), *Session.GetAppPausedReason());
	}

	return FString::Printf(TEXT("crowdy.auth.status: SignedIn=%s UserID=%lld AppToken=%s ExpiresAt=%s AppPaused=%s"),
		Auth.IsSignedIn() ? TEXT("true") : TEXT("false"), Session.GetUserID(),
		Session.GetGameToken().IsEmpty() ? TEXT("unset") : TEXT("set"),
		Expiry.IsEmpty() ? TEXT("none") : *Expiry, *Paused);
}

namespace
{
	UGameInstance* FindGameInstance(const UWorld* World)
	{
		if (World)
		{
			return World->GetGameInstance();
		}
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.OwningGameInstance)
			{
				return Context.OwningGameInstance;
			}
		}
		return nullptr;
	}

	UCrowdyAuthentication* FindAuthentication(const TCHAR* Command, const UWorld* World, UGameInstance*& OutInstance)
	{
		OutInstance = FindGameInstance(World);
		UCrowdyAuthentication* Auth = OutInstance ? OutInstance->GetSubsystem<UCrowdyAuthentication>() : nullptr;
		if (!Auth)
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("%s needs a running game instance."), Command);
		}
		return Auth;
	}

	void SignInFromFile(const TArray<FString>& Args, UWorld* World)
	{
		const FString Path = FString::Join(Args, TEXT(" "));
		if (Path.IsEmpty())
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("Usage: crowdy.auth.signin.file <path to a JSON file with \"email\" and \"password\">"));
			return;
		}

		UGameInstance* Instance = nullptr;
		UCrowdyAuthentication* Auth = FindAuthentication(TEXT("crowdy.auth.signin.file"), World, Instance);
		if (!Auth)
		{
			return;
		}

		FString Email;
		FString Password;
		FString Error;
		if (!CrowdyAuthDev::ReadCredentialsFile(Path, Email, Password, Error))
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("crowdy.auth.signin.file: %s"), *Error);
			return;
		}

		UE_LOG(LogCrowdyServices, Log, TEXT("crowdy.auth.signin.file: signing in from the file."));
		CrowdyAuthDev::SignIn(*Auth, Email, Password);
	}

	void LogStatus(UWorld* World)
	{
		UGameInstance* Instance = nullptr;
		UCrowdyAuthentication* Auth = FindAuthentication(TEXT("crowdy.auth.status"), World, Instance);
		const UCrowdyGameSession* Session = Instance ? Instance->GetSubsystem<UCrowdyGameSession>() : nullptr;
		if (!Auth || !Session)
		{
			return;
		}

		UE_LOG(LogCrowdyServices, Log, TEXT("%s"), *CrowdyAuthDev::DescribeStatus(*Auth, *Session));
	}

	void SignOut(UWorld* World)
	{
		UGameInstance* Instance = nullptr;
		UCrowdyAuthentication* Auth = FindAuthentication(TEXT("crowdy.auth.signout"), World, Instance);
		if (!Auth)
		{
			return;
		}

		// The SDK subsystem runs its full logout when the session ends, so announcing that is the normal sign-out.
		if (!Auth->OnServerSessionEnded.IsBound())
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("crowdy.auth.signout: the SDK subsystem is not listening, so nothing was signed out."));
			return;
		}

		Auth->OnServerSessionEnded.Broadcast();
		UE_LOG(LogCrowdyServices, Log, TEXT("crowdy.auth.signout: signed out."));
	}

	FAutoConsoleCommandWithWorldAndArgs GCrowdyAuthSignInFile(
		TEXT("crowdy.auth.signin.file"),
		TEXT("Signs in with the password account in a JSON file, {\"email\": \"...\", \"password\": \"...\"}, for headless smoke tests. ")
		TEXT("For test accounts and development builds only: the password sits in that file, so keep it out of source control. ")
		TEXT("Never type a password as a console argument, which the console logs. Args: <path to the file>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SignInFromFile));

	FAutoConsoleCommandWithWorld GCrowdyAuthStatus(
		TEXT("crowdy.auth.status"),
		TEXT("Prints whether this game instance is signed in, the user id, whether the app token is set and when it expires, ")
		TEXT("and whether the app is paused. Never prints a token."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&LogStatus));

	FAutoConsoleCommandWithWorld GCrowdyAuthSignOut(
		TEXT("crowdy.auth.signout"),
		TEXT("Signs the player out through the SDK's normal logout."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&SignOut));
}

#endif
