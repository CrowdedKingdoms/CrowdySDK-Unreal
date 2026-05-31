// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Structures/FGraphQLQueryDef.h"
#include "Engine/DataAsset.h"
#include "GraphQLQueryDatabase.generated.h"

/**
 * @class UGraphQLQueryDatabase
 * @brief Represents a database of GraphQL query definitions as a data asset.
 *
 * UGraphQLQueryDatabase is a subclass of UDataAsset designed to store and manage
 * predefined GraphQL queries. It serves as a centralized location for handling
 * GraphQL queries within the application.
 *
 * This class is primarily used to organize GraphQL queries for use in communication
 * with GraphQL servers, ensuring structured and reusable query management.
 *
 * It is part of the CROWDROCKS_API module.
 */
UCLASS(BlueprintType)
class CROWDYSDK_API UGraphQLQueryDatabase : public UDataAsset
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GraphQL Query Database")
	TArray<FGraphQLQueryDef> QueryList;

	UFUNCTION(BlueprintCallable, Category = "GraphQL Query Database")
	bool GetQueryByID(const EGraphQLQuery QueryID, FGraphQLQueryDef& OutQueryDef) const
	{
		const FGraphQLQueryDef* Found = QueryList.FindByPredicate([&](const FGraphQLQueryDef& Q) {
			return Q.QueryID == QueryID;
		});

		if (Found)
		{
			OutQueryDef = *Found;
			return true;
		}
		return false;
	}
};
