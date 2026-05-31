#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * GraphQL request used internally by UCrowdyPersistenceSubsystem to read
 * back voxel state data for a specific persistence chunk.
 *
 * Uses an inline query body so no data-asset entry is required.
 * Identical to the VoxelList query but routed to EQueryResponseType::PersistencePull
 * so responses only land in the persistence subsystem.
 */
struct FPersistencePullRequest : ICrowdyQueryRequest
{
	int64 AppID  = -1;
	int64 ChunkX = 0;   // struct TypeID
	int64 ChunkY = 0;   // CRC32(InstanceKey) — 0 for singletons
	int64 ChunkZ = 0;

	virtual FName GetOperationName() const override
	{
		return TEXT("Persistence Pull Request");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::PersistencePull;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("appId"), FString::Printf(TEXT("%lld"), AppID));
		RuntimeVariables.Add(TEXT("x"),     FString::Printf(TEXT("%lld"), ChunkX));
		RuntimeVariables.Add(TEXT("y"),     FString::Printf(TEXT("%lld"), ChunkY));
		RuntimeVariables.Add(TEXT("z"),     FString::Printf(TEXT("%lld"), ChunkZ));

		// Embed the query body directly — no data-asset entry needed.
		InlineQueryBody = GetStaticQueryBody();

		bIncludeAuthToken = true;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return AppID > -1 && RuntimeVariables.Num() == 4;
	}

private:

	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("query VoxelList($appId: BigInt!, $x: BigInt!, $y: BigInt!, $z: BigInt!) {")
			TEXT("  getVoxelList(input: {appId: $appId, coordinates: {x: $x, y: $y, z: $z}}) {")
			TEXT("    coordinates { x y z }")
			TEXT("    voxels {")
			TEXT("      location { x y z }")
			TEXT("      voxelType")
			TEXT("      state")
			TEXT("    }")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
