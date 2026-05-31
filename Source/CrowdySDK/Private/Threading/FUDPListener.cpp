#include "FUDPListener.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"

FUDPListener::FUDPListener(FSocket* InSocket, UCrowdyUDPSubsystem* InOwner) : Socket(InSocket), Owner(InOwner), bRun(true)
{
	Buffer.SetNumUninitialized(1280);
}

FUDPListener::~FUDPListener()
{
}

bool FUDPListener::Init()
{
	return Socket != nullptr;
}

uint32 FUDPListener::Run()
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	
	if (!Socket || !SocketSubsystem || !Owner) return 0;

	const TSharedRef<FInternetAddr> ClientAddr = SocketSubsystem->CreateInternetAddr();
	
	while (bRun)
	{
		int32 BytesRead = 0;
		if (Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), BytesRead, *ClientAddr))
		{
			if (BytesRead > 0)
			{
				Owner->HandleUDPMessage(Buffer.GetData(), BytesRead);
			}
		}
		else
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR && Error != SE_EINTR)
			{
				return 0;
			}
		}
		
		FPlatformProcess::YieldThread();
	}
	return 0;
}

void FUDPListener::Stop()
{
	bRun = false;
}

void FUDPListener::Exit()
{
	FRunnable::Exit();
}
