#include "Auth/FCrowdyLoopbackAuthServer.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HttpServerModule.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace CrowdyLoopbackAuthTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The request a browser sends, over a real socket, so the engine's router decides what is reachable. Returns the
	// HTTP status, or 0 when no answer came.
	int32 Get(const uint32 Port, const FString& PathAndQuery)
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
		bool bValid = false;
		Address->SetIp(TEXT("127.0.0.1"), bValid);
		Address->SetPort(Port);

		FSocket* Socket = Sockets->CreateSocket(NAME_Stream, TEXT("CrowdyLoopbackAuthTest"), Address->GetProtocolType());
		if (!Socket)
		{
			return 0;
		}
		ON_SCOPE_EXIT
		{
			Socket->Close();
			Sockets->DestroySocket(Socket);
		};

		Socket->SetNonBlocking(false);
		if (!bValid || !Socket->Connect(*Address))
		{
			return 0;
		}
		Socket->SetNonBlocking(true);

		const FTCHARToUTF8 Request(*FString::Printf(TEXT("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"), *PathAndQuery));
		int32 Sent = 0;
		Socket->Send(reinterpret_cast<const uint8*>(Request.Get()), Request.Length(), Sent);

		FString Answer;
		const double Until = FPlatformTime::Seconds() + 3.0;
		while (FPlatformTime::Seconds() < Until)
		{
			// Through the base: the module's own override is not exported.
			static_cast<FTSTickerObjectBase&>(FHttpServerModule::Get()).Tick(0.01f);
			uint8 Buffer[256];
			int32 Read = 0;
			if (Socket->Recv(Buffer, sizeof(Buffer) - 1, Read) && Read > 0)
			{
				Buffer[Read] = 0;
				Answer += ANSI_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Buffer));
				if (Answer.Contains(TEXT("\r\n")))
				{
					break;
				}
			}
			FPlatformProcess::Sleep(0.005f);
		}

		TArray<FString> Parts;
		Answer.ParseIntoArray(Parts, TEXT(" "));
		return Parts.Num() > 1 ? FCString::Atoi(*Parts[1]) : 0;
	}

	bool SplitUri(const FString& Uri, uint32& OutPort, FString& OutPath)
	{
		const FString Prefix = TEXT("http://127.0.0.1:");
		if (!Uri.StartsWith(Prefix))
		{
			return false;
		}
		const FString Rest = Uri.RightChop(Prefix.Len());
		int32 Slash = INDEX_NONE;
		if (!Rest.FindChar(TEXT('/'), Slash))
		{
			return false;
		}
		OutPort = FCString::Atoi(*Rest.Left(Slash));
		OutPath = Rest.RightChop(Slash);
		return true;
	}
}

// With no state to check, a web page can reach the fixed address but cannot guess the one in the email.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackUnguessablePathTest,
	"CrowdySDK.Auth.LoopbackRefusesACallbackOutsideItsPath", CrowdyLoopbackAuthTests::TestFlags)
bool FCrowdyLoopbackUnguessablePathTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyLoopbackAuthTests;

	FString Captured;
	FOnLoopbackToken OnToken;
	OnToken.BindLambda([&Captured](const FString& Token) { Captured = Token; });

	FCrowdyLoopbackAuthServer Server;
	const FString First = Server.Start(FString(), 30.0, OnToken, FOnLoopbackError());
	Server.Stop();
	const FString Second = Server.Start(FString(), 30.0, OnToken, FOnLoopbackError());
	TestNotEqual(TEXT("every flow gets its own address"), First, Second);

	uint32 Port = 0;
	FString Path;
	if (!TestTrue(TEXT("the listener gives an address"), SplitUri(Second, Port, Path)))
	{
		return false;
	}

	TestEqual(TEXT("the fixed address is not served"), Get(Port, TEXT("/callback?token=attacker")), 404);
	TestEqual(TEXT("nor is the first flow's address"), Get(Port, *FString::Printf(TEXT("%s?token=attacker"), *First.RightChop(First.Find(TEXT("/callback"))))), 404);
	TestTrue(TEXT("and no token was taken"), Captured.IsEmpty());
	TestTrue(TEXT("the flow is still waiting for the real link"), Server.IsActive());

	TestEqual(TEXT("the real address answers"), Get(Port, *FString::Printf(TEXT("%s?token=real"), *Path)), 200);
	TestEqual(TEXT("and its token is taken"), Captured, FString(TEXT("real")));

	Server.Stop();
	return true;
}

// Social sign-in reserves its address before it knows the state, and the state then guards that fixed path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLoopbackFixedPathTest,
	"CrowdySDK.Auth.LoopbackKeepsTheFixedPathByDefault", CrowdyLoopbackAuthTests::TestFlags)
bool FCrowdyLoopbackFixedPathTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyLoopbackAuthTests;

	FCrowdyLoopbackAuthServer Server;
	const FString Reserved = Server.ReserveRedirectUri();
	const FString Armed = Server.Start(TEXT("state"), 30.0, FOnLoopbackToken(), FOnLoopbackError());
	TestEqual(TEXT("the address armed is the address reserved"), Armed, Reserved);
	Server.Stop();
	return true;
}

#endif
