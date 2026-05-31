#pragma once
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class UCrowdyGameSession;
class FCrowdyServiceRegistry;
class UCrowdyUDPSubsystem;
class ICrowdyMessage;
class UCrowdySDKSubsystem;

class CROWDYSDK_API FCrowdyMessageParser
{
	
public:
	
	FCrowdyMessageParser(FCrowdyServiceRegistry* InServiceRegistry, 
		UCrowdyUDPSubsystem* InUDPSubsystem, 
		UCrowdySDKSubsystem* InSDK,
		UCrowdyGameSession* InGameSession);
	~FCrowdyMessageParser() = default;
	
	[[nodiscard]] TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> ParseMessage(const TArray<uint8>& Data);
	void SetExpectedActorStateSize(const int32 NewSize);
private:
	UCrowdyGameSession* GameSession;
	UCrowdySDKSubsystem* SDK;
	FCrowdyServiceRegistry* ServiceRegistry;
	UCrowdyUDPSubsystem* UDPSubsystem;
	int32 ExpectedActorStateSize = 300;
};
