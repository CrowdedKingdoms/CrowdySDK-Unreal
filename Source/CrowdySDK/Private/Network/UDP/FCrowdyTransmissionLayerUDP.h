#pragma once
#include "Core/UDP/Interfaces/ICrowdyTransmissionLayer.h"
#include "CoreMinimal.h"
class UCrowdyGameSession;

class FCrowdyTransmissionLayerUDP : public ICrowdyTransmissionLayer
{
public:
	
	explicit FCrowdyTransmissionLayerUDP(UCrowdyGameSession* InGameSession): GameSession(InGameSession)
	{
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: UDP Transmission Layer Initialized."))
	};
	
	virtual void SendBytes(TArray<uint8>&& Data, bool bRequiresAuth, uint8 SequenceNumber) override;

private:
	
	UCrowdyGameSession* GameSession = nullptr; 
};
