// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyModelRef.generated.h"

/**
 * A Server Owned attribute that points at another Game Model container by its server id. Maps to the server
 * container_ref value type; the JSON value carried on the wire is the referenced container's id string (the
 * same id an effect expression consumes as ref("<id>")). An empty ModelId means "no reference" and sends no
 * default. Use this as a variable type on a container class to model relationships (equipped weapon, target,
 * owning party) that the server resolves.
 */
USTRUCT(BlueprintType)
struct FCrowdyModelRef
{
	GENERATED_BODY()

	// The referenced container's server id, or empty for no reference.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy SDK|Game Model")
	FString ModelId;
};
