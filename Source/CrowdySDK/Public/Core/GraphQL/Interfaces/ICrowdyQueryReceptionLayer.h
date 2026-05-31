#pragma once
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
class ICrowdyQueryResponse;


class CROWDYSDK_API ICrowdyQueryReceptionLayer
{
public:
	
	virtual ~ICrowdyQueryReceptionLayer() = default;
	
	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) = 0;
	
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const = 0;
	
};
