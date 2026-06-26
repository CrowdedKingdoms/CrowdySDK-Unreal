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

TArray<UScriptStruct*> FCrowdyBPLayerBridge::GetSupportedEvents() const
{
	if (UCrowdyBlueprintReceptionLayer* Owner = WeakOwner.Get())
	{
		TArray<UScriptStruct*> Structs;
		Structs.Reserve(Owner->SupportedEvents.Num());
		for (const TObjectPtr<UScriptStruct>& Struct : Owner->SupportedEvents)
		{
			if (Struct) Structs.Add(Struct);
		}
		return Structs;
	}
	return {};
}

TArray<ECrowdyMessageType> FCrowdyBPLayerBridge::GetSupportedResponseTypes() const
{
	if (UCrowdyBlueprintReceptionLayer* Owner = WeakOwner.Get())
		return Owner->SupportedResponseTypes;
	return {};
}
