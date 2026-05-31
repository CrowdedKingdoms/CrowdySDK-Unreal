#pragma once
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"
#include <atomic>
class UCrowdyUDPSubsystem;

class CROWDYSDK_API FUDPListener : public FRunnable
{
	
public:
	
	FUDPListener(FSocket* InSocket, UCrowdyUDPSubsystem* InOwner);
	virtual ~FUDPListener() override;
	
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;
	
private:
	
	FSocket* Socket;
	UCrowdyUDPSubsystem* Owner;
	std::atomic<bool> bRun;
	TArray<uint8> Buffer;
};
