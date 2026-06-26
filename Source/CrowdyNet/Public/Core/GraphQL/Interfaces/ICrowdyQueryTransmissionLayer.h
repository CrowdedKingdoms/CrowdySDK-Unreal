#pragma once

class ICrowdyQueryRequest;

class CROWDYNET_API ICrowdyQueryTransmissionLayer
{
public:
	virtual ~ICrowdyQueryTransmissionLayer() = default;
	
	virtual void ExecuteQuery(ICrowdyQueryRequest& QueryRequest) = 0;
	
};
