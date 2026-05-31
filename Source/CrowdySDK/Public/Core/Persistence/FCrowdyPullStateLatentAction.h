#pragma once
#include "CoreMinimal.h"
#include "LatentActions.h"
#include "Core/Persistence/EPullStateResult.h"
#include "Core/Persistence/ICrowdyPersistentOwner.h"

/**
 * FPendingLatentAction for UCrowdyPersistenceSubsystem::K2_PullState.
 *
 * Lifecycle:
 *   1. Created by InternalPullState on the game thread.
 *   2. Added to the world's FLatentActionManager.
 *   3. The subsystem's OnResponseReceived sets bCompleted + ResultBytes.
 *   4. UpdateOperation fires on the next tick, deserializes bytes into
 *      OutAddr, calls OnPersistentStateRestored if the owner implements
 *      ICrowdyPersistentOwner, then resumes the Blueprint exec graph.
 */
class FCrowdyPullStateLatentAction : public FPendingLatentAction
{
public:

	// ── Blueprint graph wiring ─────────────────────────────────────────────

	FName           ExecutionFunction;
	int32           OutputLink      = 0;
	FWeakObjectPtr  CallbackTarget;

	// ── Set during construction ────────────────────────────────────────────

	/** Struct type to deserialize into. */
	UScriptStruct* StructType = nullptr;

	/** Pointer to the Blueprint local variable that receives the result.
	 *  Valid for the entire lifetime of this latent action because the
	 *  Blueprint execution frame is kept alive by the LatentActionManager. */
	void* OutAddr = nullptr;

	/** Written before the exec graph is resumed; drives OnSuccess/OnFailure. */
	EPullStateResult* ResultPtr = nullptr;

	/** The owner that triggered the pull — used for the auto-apply callback. */
	TWeakObjectPtr<UObject> OwnerObject;

	/** Instance key that was used to locate the voxel slot. */
	FString InstanceKey;

	// ── Set by the subsystem when the response arrives ─────────────────────

	bool           bCompleted   = false;
	bool           bSuccess     = false;
	TArray<uint8>  ResultBytes;

	// ── Construction ──────────────────────────────────────────────────────

	FCrowdyPullStateLatentAction(
		const FLatentActionInfo& LatentInfo,
		UScriptStruct*           InStructType,
		void*                    InOutAddr,
		EPullStateResult&        InResult,
		UObject*                 InOwner,
		const FString&           InInstanceKey)
		: ExecutionFunction(LatentInfo.ExecutionFunction)
		, OutputLink(LatentInfo.Linkage)
		, CallbackTarget(LatentInfo.CallbackTarget)
		, StructType(InStructType)
		, OutAddr(InOutAddr)
		, ResultPtr(&InResult)
		, OwnerObject(InOwner)
		, InstanceKey(InInstanceKey)
	{}

	// ── FPendingLatentAction ───────────────────────────────────────────────

	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		if (!bCompleted)
			return;

		// ── Deserialize result into the Blueprint local variable ───────────
		if (bSuccess && OutAddr && StructType && ResultBytes.Num() > 0)
		{
			// Initialize the destination to struct defaults before filling it.
			StructType->InitializeStruct(OutAddr);

			FMemoryReader Reader(ResultBytes, /*bIsPersistent=*/true);
			StructType->SerializeTaggedProperties(Reader, static_cast<uint8*>(OutAddr),
			                                      /*DefaultsStruct=*/nullptr,
			                                      /*Defaults=*/nullptr);
		}

		// ── Write result enum (drives exec routing) ────────────────────────
		if (ResultPtr)
			*ResultPtr = bSuccess ? EPullStateResult::OnSuccess : EPullStateResult::OnFailure;

		// ── Auto-apply callback ────────────────────────────────────────────
		if (bSuccess)
		{
			if (UObject* Owner = OwnerObject.Get())
			{
				if (Owner->GetClass()->ImplementsInterface(UCrowdyPersistentOwner::StaticClass()))
				{
					ICrowdyPersistentOwner::Execute_OnPersistentStateRestored(
						Owner, StructType, InstanceKey);
				}
			}
		}

		Response.FinishAndTriggerIf(true, ExecutionFunction, OutputLink, CallbackTarget);
	}

	virtual FString GetDescription() const override
	{
		return FString::Printf(TEXT("CrowdyPersistence: pulling %s..."),
			StructType ? *StructType->GetName() : TEXT("?"));
	}
};
