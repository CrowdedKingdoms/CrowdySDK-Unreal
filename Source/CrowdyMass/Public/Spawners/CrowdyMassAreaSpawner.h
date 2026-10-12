#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Mass/EntityHandle.h"
#include "CrowdyMassAreaSpawner.generated.h"

class UBoxComponent;

UCLASS(Blueprintable, BlueprintType, meta=(DisplayName="Crowdy Mass Area Spawner"))
class CROWDYMASS_API ACrowdyMassAreaSpawner : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyMassAreaSpawner();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Spawner")
	int32 SpawnEntities();

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Spawner")
	int32 RespawnEntities();

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Spawner")
	void DestroySpawnedEntities();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Spawner", meta=(DisplayName="Get Spawned UUIDs"))
	void GetSpawnedUUIDs(TArray<FGuid>& OutUUIDs) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Spawner")
	void GetSpawnedEntities(TArray<FMassEntityHandle>& OutEntities) const;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowdy Mass|Spawner")
	TObjectPtr<UBoxComponent> SpawnArea;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Spawner", meta=(ClampMin="0"))
	int32 EntityCount = 100;

	/**
	 * The actor class these entities stand for, recorded on each one as its class.
	 *
	 * Both per-class rendering and event sends read it, so leaving it unset produces entities that render
	 * from the fallback config and that no Crowdy event can ever be sent to: a send is refused because
	 * nothing on this client says what the entity is. Set it for anything you intend to trace and call.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Spawner")
	TSoftClassPtr<AActor> EntityClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Spawner")
	bool bSpawnOnBeginPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Spawner")
	bool bDestroySpawnedOnEndPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Spawner")
	bool bClearBeforeSpawn = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Distribution", meta=(ClampMin="0.0", ClampMax="1.0"))
	float PositionJitter = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Distribution")
	bool bUseDeterministicSeed = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Distribution", meta=(EditCondition="bUseDeterministicSeed"))
	int32 RandomSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Distribution")
	bool bRandomizeYaw = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Grounding")
	bool bProjectToGround = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Grounding", meta=(EditCondition="bProjectToGround", ClampMin="0.0"))
	float GroundTraceDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Grounding", meta=(EditCondition="bProjectToGround"))
	TEnumAsByte<ECollisionChannel> GroundTraceChannel = ECC_WorldStatic;

private:
	UPROPERTY(Transient)
	TArray<FGuid> SpawnedUUIDs;

	UPROPERTY(Transient)
	TArray<FMassEntityHandle> SpawnedEntities;

	bool bToldNoCrowdRows = false;

	void BuildSpawnTransforms(TArray<FTransform>& OutTransforms) const;
	bool ProjectSpawnLocationToGround(FVector& InOutLocation) const;
};
