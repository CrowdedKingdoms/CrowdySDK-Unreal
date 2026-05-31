#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "FGraphQLQueryDef.generated.h"

/**
 * FGraphQLQueryDef is a structure representing the definition of a GraphQL query.
 * It includes the query identifier, the query string, and a map of default variables.
 */
USTRUCT(BlueprintType)
struct FGraphQLQueryDef
{
	GENERATED_BODY()

	/**
	 * QueryID represents the type of GraphQL query to be executed.
	 * It is an enumeration of possible GraphQL queries defined by the EGraphQLQuery enum.
	 * This property can be edited within the editor, accessed in Blueprints,
	 * and is categorized under "GraphQL Query".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GraphQL Query")
	EGraphQLQuery QueryID;

	/**
	 * Represents the body of a GraphQL query in string format.
	 * This string is intended to define the structure of a query that will be sent to a GraphQL API.
	 * Can be edited in the Unreal Engine editor and is accessible within Blueprints.
	 * The string property is marked as multiline to allow for easier editing of longer queries.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GraphQL Query", meta=(MultiLine = true))
	FString QueryBody;

	/**
	 * A map that defines default variables to be included in the GraphQL query execution.
	 * This property allows for specifying key-value pairs, where the keys and values
	 * are strings representing the variable names and their corresponding default values.
	 *
	 * These default variables are used as a base set of variables for the query and can be
	 * merged with or overridden by runtime variables provided during query execution.
	 *
	 * Editable in the editor and accessible in Blueprints.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GraphQL Query")
	TMap<FString, FString> DefaultVariables;
};
