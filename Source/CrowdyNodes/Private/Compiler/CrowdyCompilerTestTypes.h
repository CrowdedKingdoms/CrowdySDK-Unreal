#pragma once

// Declared unconditionally because UnrealHeaderTool refuses a reflected type inside a preprocessor block; only the
// tests that use it are guarded.

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "UObject/Object.h"
#include "CrowdyCompilerTestTypes.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCrowdyCompilerTestDone);

/** An async node whose factory is marked deprecated, to prove the compiler warns on any such node. */
// Hidden, with no menu entry of its own, so a fixture never reaches an author's palette.
UCLASS(Hidden, meta = (CrowdyTestFixture, HasDedicatedAsyncNode))
class UCrowdyCompilerTestDeprecatedAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:

	UPROPERTY(BlueprintAssignable)
	FCrowdyCompilerTestDone OnDone;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", DeprecatedFunction,
		DeprecationMessage = "Use Test Replacement Node."))
	static UCrowdyCompilerTestDeprecatedAsyncAction* DeprecatedTestNode()
	{
		return NewObject<UCrowdyCompilerTestDeprecatedAsyncAction>();
	}
};

/** Carries a deprecated delegate, to prove binding to one warns. */
UCLASS(Hidden, meta = (CrowdyTestFixture))
class UCrowdyCompilerTestDeprecatedDelegateOwner : public UObject
{
	GENERATED_BODY()

public:

	UE_DEPRECATED(5.8, "Bind a replacement event instead.")
	UPROPERTY(BlueprintAssignable, meta = (DeprecatedProperty, DeprecationMessage = "Bind a replacement event instead."))
	FCrowdyCompilerTestDone OnDeprecatedTest;
};
