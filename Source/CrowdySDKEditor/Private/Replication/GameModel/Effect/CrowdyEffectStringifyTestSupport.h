#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"

// Stable, order-preserving string forms of a lowered function input and full lowering result, so a golden test can
// assert two compiles are byte-identical field for field (neither struct has an operator==). Shared by the effect
// picker options and effect graph compiler tests so the comparison logic lives in exactly one place.
inline FString CrowdyStringifyFunction(const FCrowdyGameModelFunctionInput& F)
{
	FString S = FString::Printf(
		TEXT("name=%s|type=%s|desc=%s|ret=%s|scope=%s|auton=%d|retExpr=%s|policy=%s|params="),
		*F.Name, *F.ContainerTypeName, *F.Description, *F.ReturnType, *F.InvokeScope,
		F.bAutonomousInvocable ? 1 : 0, *F.ReturnExpression, *F.InvokePolicyJson);
	for (const FCrowdyGameModelFunctionParam& P : F.Parameters)
	{
		S += FString::Printf(TEXT("[name=%s;type=%s;req=%d;def=%s;desc=%s;sort=%d]"),
			*P.Name, *P.ValueType, P.bRequired ? 1 : 0, *P.DefaultValueJson, *P.Description, P.SortOrder);
	}
	S += TEXT("|muts=");
	for (const FCrowdyGameModelMutation& M : F.Mutations)
	{
		S += FString::Printf(TEXT("[target=%s;prop=%s;expr=%s]"), *M.Target, *M.Property, *M.Expression);
	}
	S += TEXT("|notifs=");
	for (const FCrowdyGameModelNotification& N : F.Notifications)
	{
		S += FString::Printf(TEXT("[kind=%s;emitAs=%s;args="), *N.Kind, *N.EmitAs);
		for (const FCrowdyGameModelNotificationArg& A : N.Args)
		{
			S += FString::Printf(TEXT("(%s=%s)"), *A.Name, *A.Expression);
		}
		S += TEXT("]");
	}
	return S;
}

inline FString CrowdyStringifyResult(const FCrowdyEffectLoweringResult& R)
{
	FString S = CrowdyStringifyFunction(R.Function);
	S += FString::Printf(TEXT("|srcRef=%d|diags="), R.bSourceReferenced ? 1 : 0);
	for (const FCrowdyEffectDiagnostic& D : R.Diagnostics)
	{
		S += TEXT("<") + D.ToString() + TEXT(">");
	}
	return S;
}

#endif // WITH_DEV_AUTOMATION_TESTS
