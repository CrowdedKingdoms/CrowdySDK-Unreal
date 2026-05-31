#include "Core/Blueprints/FCrowdyBPLayerBridge.h"
#include "Core/Blueprints/CrowdyBlueprintReceptionLayer.h"

void FCrowdyBPLayerBridge::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	TWeakObjectPtr<UCrowdyBlueprintReceptionLayer> Captured = WeakOwner;

	AsyncTask(ENamedThreads::GameThread, [Captured, Message]()
	{
		UCrowdyBlueprintReceptionLayer* Owner = Captured.Get();
		if (!Owner) return;

		Owner->DispatchToBlueprint(Message);
	});
}

TArray<FName> FCrowdyBPLayerBridge::GetSupportedActorUpdateTypes() const
{
	if (UCrowdyBlueprintReceptionLayer* Owner = WeakOwner.Get())
		return Owner->SupportedActorUpdateTypes;
	return {};
}

TArray<FName> FCrowdyBPLayerBridge::GetSupportedEventTypes() const
{
	if (UCrowdyBlueprintReceptionLayer* Owner = WeakOwner.Get())
    		return Owner->SupportedEventTypes;
    	return {};
}

TArray<ECrowdyMessageType> FCrowdyBPLayerBridge::GetSupportedResponseTypes() const
{
	if (UCrowdyBlueprintReceptionLayer* Owner = WeakOwner.Get())
		return Owner->SupportedResponseTypes;
	return {};
}
