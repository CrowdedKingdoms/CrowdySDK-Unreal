#include "FCrowdyQueryTransmissionLayerGQL.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"

void FCrowdyQueryTransmissionLayerGQL::ExecuteQuery(ICrowdyQueryRequest& QueryRequest)
{
	// Fail-safe if the query is not prepared by the requesting service
	if (!QueryRequest.IsValid())
		QueryRequest.PrepareQuery();

	// If the request embeds its own query body, bypass the data-asset lookup.
	if (!QueryRequest.InlineQueryBody.IsEmpty())
	{
		QuerySubsystem->ExecuteQueryWithBody(
			QueryRequest.GetQueryType(),
			QueryRequest.InlineQueryBody,
			QueryRequest.RuntimeVariables,
			QueryRequest.bIncludeAuthToken);
		return;
	}

	QuerySubsystem->ExecuteQueryByID(QueryRequest.GetQueryType(), QueryRequest.RuntimeVariables,
	                                 QueryRequest.bIncludeAuthToken, QueryRequest.bUseNestedJson);
}
