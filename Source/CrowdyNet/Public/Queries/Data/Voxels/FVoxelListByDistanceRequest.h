#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FVoxelListByDistanceRequest : ICrowdyQueryRequest
{
	int64 AppID = -1;
	int64 CenterX;
	int64 CenterY;
	int64 CenterZ;
	int32 MaxDistance;
	int32 Limit;
	int32 Skip;
	
	virtual FName GetOperationName() const override
	{
		return "Voxel List By Distance Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("input.appId"), FString::Printf(TEXT("%lld"), AppID));
		RuntimeVariables.Add(TEXT("input.centerCoordinate.x"), FString::Printf(TEXT("%lld"), CenterX));
		RuntimeVariables.Add(TEXT("input.centerCoordinate.y"), FString::Printf(TEXT("%lld"), CenterY));
		RuntimeVariables.Add(TEXT("input.centerCoordinate.z"), FString::Printf(TEXT("%lld"), CenterZ));
		RuntimeVariables.Add(TEXT("input.maxDistance"), FString::Printf(TEXT("%d"), MaxDistance));
		RuntimeVariables.Add(TEXT("input.limit"), FString::Printf(TEXT("%d"), Limit));
		RuntimeVariables.Add(TEXT("input.skip"), FString::Printf(TEXT("%d"), Skip));
		RuntimeVariables.Add(TEXT("input.since"), FString::Printf(TEXT("%d"), Since));
		
		bIncludeAuthToken = true;
		bUseNestedJson = true;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::ListVoxelUpdatesByDistance;
	}
	
	virtual bool IsValid() const override
	{
		return RuntimeVariables.Num() == 8 && AppID > -1;
	}
	
private:
	int32 Since = 0;
};