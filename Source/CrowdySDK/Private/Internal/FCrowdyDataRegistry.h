#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"

class ICrowdyQueryReceptionLayer;
class ICrowdyQueryResponse;

class CROWDYSDK_API FCrowdyDataRegistry
{
public:
	FCrowdyDataRegistry() = default;
	~FCrowdyDataRegistry() = default;
	
	void RegisterLayer(ICrowdyQueryReceptionLayer* LayerToRegister);
	void DispatchResponse(const TSharedPtr<ICrowdyQueryResponse>& Response);

private:
	
	TMap<EQueryResponseType, ICrowdyQueryReceptionLayer*> ReceptionLayersByType;
};

