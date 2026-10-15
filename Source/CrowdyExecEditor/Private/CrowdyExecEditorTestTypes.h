#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecEditorTestTypes.generated.h"

/** Kinds the client checks inside containers: names in a list and in a set, an optional GUID, and dates keyed by name. */
USTRUCT()
struct FCrowdyExecTestKinds
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FName> Names;

	UPROPERTY()
	TOptional<FGuid> MaybeId;

	UPROPERTY()
	TMap<FName, FDateTime> SeenAt;

	UPROPERTY()
	TSet<FName> Tags;
};

/** A row of a record's grid, so a record can hold many elements. */
USTRUCT()
struct FCrowdyExecTestRecordRow
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<int32> Cells;
};

/** One entity's record: Health, Label and Rows for everyone, Gold for its owner, Ammo for its team, Secret for the server alone. */
USTRUCT()
struct FCrowdyExecTestRecord
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Health = 100;

	UPROPERTY()
	int32 Gold = 0;

	UPROPERTY()
	int32 Ammo = 0;

	UPROPERTY()
	int32 Secret = 0;

	UPROPERTY()
	FString Label;

	UPROPERTY()
	TArray<FCrowdyExecTestRecordRow> Rows;
};

/** Hit's inputs: the record to damage and by how much. */
USTRUCT()
struct FCrowdyExecTestRecordHit
{
	GENERATED_BODY()

	UPROPERTY()
	FString Target;

	UPROPERTY()
	int32 Amount = 0;
};

/** A struct whose server code name is Library, the name a Server Function Library's own struct takes. */
USTRUCT()
struct FLibrary
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Count = 0;
};
