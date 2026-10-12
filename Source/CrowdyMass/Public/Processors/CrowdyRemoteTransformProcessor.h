#pragma once

#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "CrowdyRemoteTransformProcessor.generated.h"

UCLASS()
class CROWDYMASS_API UCrowdyRemoteTransformProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdyRemoteTransformProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;
};
