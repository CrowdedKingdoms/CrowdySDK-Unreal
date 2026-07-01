#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"

/**
 * Which Crowded Kingdoms API endpoint handles this query.
 * Management: login, registration, user/org operations.
 * Game:       gameplay — voxels, replication, UDP proxy.
 */
enum class ECrowdyApiTarget : uint8
{
	Management,
	Game,
};

/**
 * Which bearer token a query authenticates with.
 *   Auto    — Session for Management queries, App for Game queries (the rule).
 *   Session — identity session token (management-plane: mint, identities, me).
 *   App     — app-scoped gameplay token (Game API, and refreshAppToken which is a
 *             Management mutation but re-presents the current app token).
 *   None    — no bearer (public sign-in mutations).
 */
enum class ECrowdyTokenScope : uint8
{
	Auto,
	Session,
	App,
	None,
};

/**
 * Compile-time metadata for a single GraphQL query.
 * Every query the SDK can send has exactly one row in FCrowdyQueryDescriptor.cpp.
 */
struct FCrowdyQueryDescriptor
{
	EGraphQLQuery      QueryID;
	EQueryResponseType ResponseType;    // Corresponding response enum value
	ECrowdyApiTarget   ApiTarget;       // Which API handles this query
	bool               bRequiresAuth  = true;
	float              TimeoutSeconds = 10.f;
	int32              MaxRetries     = 0;   // For automatic retry on transient failures
	ECrowdyTokenScope  TokenScope     = ECrowdyTokenScope::Auto;  // Which bearer token to attach
};

/**
 * Static lookup table — the single authoritative source for query metadata.
 *
 * Adding a new query only requires:
 *   1. One new value in EGraphQLQuery
 *   2. One new value in EQueryResponseType
 *   3. One new row in FCrowdyQueryDescriptor.cpp
 *
 * Nothing else needs to change for routing, timeouts, or auth.
 */
class CROWDYNET_API FCrowdyQueryDescriptors
{
public:
	/** Returns nullptr if QueryID is not registered. */
	static const FCrowdyQueryDescriptor* Find(EGraphQLQuery QueryID);

	static bool               IsManagementQuery(EGraphQLQuery QueryID);
	static float              GetTimeout(EGraphQLQuery QueryID);
	static int32              GetMaxRetries(EGraphQLQuery QueryID);
	static EQueryResponseType GetResponseType(EGraphQLQuery QueryID);

	/** Resolves Auto to Session (Management) or App (Game); returns explicit scopes as-is. */
	static ECrowdyTokenScope  GetTokenScope(EGraphQLQuery QueryID);

private:
	static const TArray<FCrowdyQueryDescriptor>& GetAll();
};
