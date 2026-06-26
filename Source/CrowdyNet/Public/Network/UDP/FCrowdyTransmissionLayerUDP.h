#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyTransmissionLayer.h"
#include "CoreMinimal.h"
class UCrowdyGameSession;

class CROWDYNET_API FCrowdyTransmissionLayerUDP : public ICrowdyTransmissionLayer
{
public:
	
	explicit FCrowdyTransmissionLayerUDP(UCrowdyGameSession* InGameSession): GameSession(InGameSession)
	{
		UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("UDP Transmission Layer Initialized."))
	};
	
	virtual void SendBytes(TArray<uint8>&& Data, bool bRequiresAuth, uint8 SequenceNumber) override;

private:
	
	UCrowdyGameSession* GameSession = nullptr; 
};
