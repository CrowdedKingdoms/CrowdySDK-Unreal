#pragma once
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Templates/Function.h"

class UCrowdyGameSession;
class FCrowdyServiceRegistry;
class UCrowdyUDPSubsystem;
class ICrowdyMessage;

class CROWDYNET_API FCrowdyMessageParser
{

public:

	FCrowdyMessageParser(FCrowdyServiceRegistry* InServiceRegistry,
		UCrowdyUDPSubsystem* InUDPSubsystem,
		TFunction<void()> InHeartbeatCallback,
		UCrowdyGameSession* InGameSession,
		TFunction<void()> InTokenExpiredCallback = nullptr);
	~FCrowdyMessageParser() = default;

	[[nodiscard]] TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> ParseMessage(const TArray<uint8>& Data);
	void SetExpectedActorStateSize(const int32 NewSize);
private:

	void HandleGenericErrorMessage(const uint8 ErrorType, const uint8 SequenceNumber) const;

	UCrowdyGameSession* GameSession;
	TFunction<void()> HeartbeatCallback;
	TFunction<void()> TokenExpiredCallback;
	FCrowdyServiceRegistry* ServiceRegistry;
	UCrowdyUDPSubsystem* UDPSubsystem;
	int32 ExpectedActorStateSize = 300;
};
