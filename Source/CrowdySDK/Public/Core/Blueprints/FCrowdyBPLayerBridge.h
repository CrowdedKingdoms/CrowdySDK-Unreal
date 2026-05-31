#pragma once
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"

class UCrowdyBlueprintReceptionLayer;

class FCrowdyBPLayerBridge : public ICrowdyReceptionLayer
{
public:
	
	explicit FCrowdyBPLayerBridge(UCrowdyBlueprintReceptionLayer* InOwner)
		: WeakOwner(InOwner) {}
	
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<FName> GetSupportedActorUpdateTypes() const override;
	virtual TArray<FName> GetSupportedEventTypes() const override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
private:
	TWeakObjectPtr<UCrowdyBlueprintReceptionLayer> WeakOwner;
};
