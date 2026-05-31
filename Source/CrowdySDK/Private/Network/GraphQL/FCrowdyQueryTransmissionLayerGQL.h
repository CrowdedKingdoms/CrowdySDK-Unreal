#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryTransmissionLayer.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"

class FCrowdyQueryTransmissionLayerGQL : public ICrowdyQueryTransmissionLayer
{
public:
	
	explicit FCrowdyQueryTransmissionLayerGQL(UCrowdyQuerySubsystem* InSubsystem): QuerySubsystem(InSubsystem)
	{
		UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Query Transmission Layer Initialized."))
	}
	
	virtual void ExecuteQuery(ICrowdyQueryRequest& QueryRequest) override;
	

private:
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;
};
