#include "FCrowdyDataRegistry.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"


void FCrowdyDataRegistry::RegisterLayer(ICrowdyQueryReceptionLayer* LayerToRegister)
{
	const auto& SupportedType = LayerToRegister->GetSupportedResponseType();
	for (EQueryResponseType Type : SupportedType)
	{
		ReceptionLayersByType.Add(Type, LayerToRegister);
	}
}

void FCrowdyDataRegistry::DispatchResponse(const TSharedPtr<ICrowdyQueryResponse>& Response)
{
	EQueryResponseType ResponseType = Response->GetResponseType();
	
	ICrowdyQueryReceptionLayer* Layer = ReceptionLayersByType.FindRef(ResponseType);
	
	if (!Layer)
	{
		return;
	}
	
	Layer->OnResponseReceived(Response);
}
