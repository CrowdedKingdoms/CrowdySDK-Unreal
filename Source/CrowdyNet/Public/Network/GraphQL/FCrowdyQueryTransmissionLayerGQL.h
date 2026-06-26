#pragma once
#include "CrowdyNetLog.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryTransmissionLayer.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"

class CROWDYNET_API FCrowdyQueryTransmissionLayerGQL : public ICrowdyQueryTransmissionLayer
{
public:
	
	explicit FCrowdyQueryTransmissionLayerGQL(UCrowdyQuerySubsystem* InSubsystem): QuerySubsystem(InSubsystem)
	{
		UE_CLOG(CrowdyNetTrace::Query(), LogCrowdyNet, Log, TEXT("Query Transmission Layer Initialized."))
	}
	
	virtual void ExecuteQuery(ICrowdyQueryRequest& QueryRequest) override;
	

private:
	UCrowdyQuerySubsystem* QuerySubsystem = nullptr;
};
