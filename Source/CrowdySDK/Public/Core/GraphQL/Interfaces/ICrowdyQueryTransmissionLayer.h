#pragma once

class ICrowdyQueryRequest;

class CROWDYSDK_API ICrowdyQueryTransmissionLayer
{
public:
	virtual ~ICrowdyQueryTransmissionLayer() = default;
	
	virtual void ExecuteQuery(ICrowdyQueryRequest& QueryRequest) = 0;
	
};
