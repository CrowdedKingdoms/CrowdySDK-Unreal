// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyWorldsimKitActions.h"

#include "CrowdyGameModelLog.h"
#include "Dom/JsonObject.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/Kit/CrowdyKitActionSupport.h"
#include "Replication/GameModel/Kit/CrowdyWorldsimKitNames.h"

namespace
{
	// Reads a list-row's ownerUserId as its canonical string form. The server types ownerUserId as a BigInt and
	// returns it as a JSON string, but a forged/alternate producer could emit a JSON number; accept either so the
	// owner filter and the crop's owner mirror never silently drop a row. Empty when the field is absent. The
	// unity build co-batches the kit translation units, so this file-local name is Worldsim-unique on purpose.
	FString CrowdyWorldsimReadRowOwnerString(const TSharedPtr<FJsonObject>& Row)
	{
		if (!Row.IsValid())
		{
			return FString();
		}
		FString OwnerStr;
		if (Row->TryGetStringField(TEXT("ownerUserId"), OwnerStr) && !OwnerStr.IsEmpty())
		{
			return OwnerStr;
		}
		double OwnerNum = 0.0;
		if (Row->TryGetNumberField(TEXT("ownerUserId"), OwnerNum) && FMath::IsFinite(OwnerNum))
		{
			return LexToString(static_cast<int64>(OwnerNum));
		}
		return FString();
	}

	// Reads a float coordinate out of a pulled property map: a non-finite (forged NaN/Inf) value is coerced to 0
	// so a downstream position math never sees a garbage float. Missing keys leave the caller's default.
	float CrowdyWorldsimReadCoordinate(const TSharedPtr<FJsonObject>& State, const TCHAR* Key, float Fallback)
	{
		double Value = 0.0;
		if (State.IsValid() && State->TryGetNumberField(Key, Value))
		{
			if (!FMath::IsFinite(Value))
			{
				return 0.f;
			}
			return static_cast<float>(Value);
		}
		return Fallback;
	}
}

FCrowdyWorldState UCrowdyGetWorldStateAction::ParseWorldState(const TSharedPtr<FJsonObject>& State,
	const FString& ContainerId)
{
	FCrowdyWorldState Out;
	Out.ContainerId = ContainerId;
	Out.Weather = TEXT("clear");
	if (!State.IsValid())
	{
		return Out;
	}

	double Value = 0.0;
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::TimeOfDay, Value))
	{
		Out.TimeOfDay = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::Day, Value))
	{
		Out.Day = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	FString WeatherValue;
	if (State->TryGetStringField(CrowdyWorldsimKitNames::Keys::Weather, WeatherValue))
	{
		Out.Weather = WeatherValue;
	}
	return Out;
}

UCrowdyGetWorldStateAction* UCrowdyGetWorldStateAction::GetWorldState(UObject* WorldContext,
	const FString& InTypePrefix, const FString& InSessionId)
{
	UCrowdyGetWorldStateAction* Action = NewObject<UCrowdyGetWorldStateAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGetWorldStateAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!Model)
	{
		Failed.Broadcast(FCrowdyWorldState(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}

	const FString TypeName = CrowdyWorldsimKitNames::WorldStateTypeName(TypePrefix);

	TWeakObjectPtr<UCrowdyGetWorldStateAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	Model->ListContainers(TypeName, SessionId,
		[WeakThis, WeakModel](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
		{
			UCrowdyGetWorldStateAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk)
			{
				Action->Failed.Broadcast(FCrowdyWorldState(),
					TEXT("failed to list the WorldState from the server"));
				Action->SetReadyToDestroy();
				return;
			}
			if (Containers.Num() == 0)
			{
				// The WorldState singleton is admin-created; there is no player-facing node to make one.
				Action->Failed.Broadcast(FCrowdyWorldState(),
					TEXT("no WorldState exists yet (an admin must create the WorldState singleton first)"));
				Action->SetReadyToDestroy();
				return;
			}

			// The reads below are issued from this completion, which can land after the world was torn down;
			// without a live world session there is nothing to read on behalf of, so fail instead of issuing
			// more server work. The caller is still told the outcome.
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel);
			if (!M)
			{
				Action->Failed.Broadcast(FCrowdyWorldState(), TEXT("the Game Model subsystem went away"));
				Action->SetReadyToDestroy();
				return;
			}

			const TSharedPtr<FJsonObject>& First = Containers[0];
			FString ContainerId;
			if (!First.IsValid() || !First->TryGetStringField(TEXT("containerId"), ContainerId)
				|| ContainerId.IsEmpty())
			{
				Action->Failed.Broadcast(FCrowdyWorldState(), TEXT("the WorldState row has no container id"));
				Action->SetReadyToDestroy();
				return;
			}

			M->PullContainerState(ContainerId,
				[WeakThis, ContainerId](bool bPullOk, TSharedPtr<FJsonObject> State)
				{
					UCrowdyGetWorldStateAction* Inner = WeakThis.Get();
					if (!Inner)
					{
						return;
					}
					if (!bPullOk || !State.IsValid())
					{
						Inner->Failed.Broadcast(FCrowdyWorldState(),
							TEXT("failed to read the WorldState from the server"));
						Inner->SetReadyToDestroy();
						return;
					}
					Inner->Succeeded.Broadcast(ParseWorldState(State, ContainerId), FString());
					Inner->SetReadyToDestroy();
				});
		});
}

FCrowdyResourceNode UCrowdyListResourceNodesAction::ParseResourceNode(const TSharedPtr<FJsonObject>& State,
	const FString& ContainerId, const FString& DisplayName)
{
	FCrowdyResourceNode Out;
	Out.ContainerId = ContainerId;
	Out.DisplayName = DisplayName;
	if (!State.IsValid())
	{
		return Out;
	}

	State->TryGetStringField(CrowdyWorldsimKitNames::Keys::NodeId, Out.NodeId);
	State->TryGetStringField(CrowdyWorldsimKitNames::Keys::ResourceItemId, Out.ResourceItemId);

	double Value = 0.0;
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::Amount, Value))
	{
		Out.Amount = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::MaxAmount, Value))
	{
		Out.MaxAmount = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::RegenRate, Value))
	{
		Out.RegenRate = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	Out.X = CrowdyWorldsimReadCoordinate(State, CrowdyWorldsimKitNames::Keys::X, Out.X);
	Out.Y = CrowdyWorldsimReadCoordinate(State, CrowdyWorldsimKitNames::Keys::Y, Out.Y);
	Out.Z = CrowdyWorldsimReadCoordinate(State, CrowdyWorldsimKitNames::Keys::Z, Out.Z);
	return Out;
}

UCrowdyListResourceNodesAction* UCrowdyListResourceNodesAction::ListResourceNodes(UObject* WorldContext,
	const FString& InTypePrefix, const FString& InSessionId)
{
	UCrowdyListResourceNodesAction* Action = NewObject<UCrowdyListResourceNodesAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyListResourceNodesAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyResourceNode>(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}

	const FString TypeName = CrowdyWorldsimKitNames::ResourceNodeTypeName(TypePrefix);

	TWeakObjectPtr<UCrowdyListResourceNodesAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	Model->ListContainers(TypeName, SessionId,
		[WeakThis, WeakModel](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
		{
			UCrowdyListResourceNodesAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk)
			{
				Action->Failed.Broadcast(TArray<FCrowdyResourceNode>(),
					TEXT("failed to list resource nodes from the server"));
				Action->SetReadyToDestroy();
				return;
			}
			if (Containers.Num() == 0)
			{
				Action->Succeeded.Broadcast(TArray<FCrowdyResourceNode>(), FString());
				Action->SetReadyToDestroy();
				return;
			}

			// The reads below are issued from this completion, which can land after the world was torn down;
			// without a live world session there is nothing to read on behalf of, so fail instead of issuing
			// more server work. The caller is still told the outcome.
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel);
			if (!M)
			{
				Action->Failed.Broadcast(TArray<FCrowdyResourceNode>(),
					TEXT("the Game Model subsystem went away"));
				Action->SetReadyToDestroy();
				return;
			}

			if (Containers.Num() > CrowdyKitActionSupport::MaxKitListRows)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("List Resource Nodes: server returned %d rows; capping the read to %d"),
					Containers.Num(), CrowdyKitActionSupport::MaxKitListRows);
				Containers.SetNum(CrowdyKitActionSupport::MaxKitListRows);
			}

			// Fan out one PullContainerState per row and gather the parsed nodes. The subsystem callbacks land on
			// the game thread one at a time, so the shared accumulator and the remaining counter need no locking.
			// A row whose pull fails is logged and omitted rather than failing the whole list.
			const TSharedPtr<TArray<FCrowdyResourceNode>> Accumulated = MakeShared<TArray<FCrowdyResourceNode>>();
			const TSharedPtr<int32> Remaining = MakeShared<int32>(Containers.Num());

			for (const TSharedPtr<FJsonObject>& Row : Containers)
			{
				FString ContainerId;
				FString DisplayName;
				if (Row.IsValid())
				{
					Row->TryGetStringField(TEXT("containerId"), ContainerId);
					Row->TryGetStringField(TEXT("displayName"), DisplayName);
				}
				if (ContainerId.IsEmpty())
				{
					--(*Remaining);
					if (*Remaining <= 0)
					{
						Action->Succeeded.Broadcast(*Accumulated, FString());
						Action->SetReadyToDestroy();
					}
					continue;
				}

				M->PullContainerState(ContainerId,
					[WeakThis, Accumulated, Remaining, ContainerId, DisplayName](bool bPullOk,
						TSharedPtr<FJsonObject> State)
					{
						UCrowdyListResourceNodesAction* Inner = WeakThis.Get();
						if (!Inner)
						{
							return;
						}
						if (bPullOk && State.IsValid())
						{
							Accumulated->Add(ParseResourceNode(State, ContainerId, DisplayName));
						}
						else
						{
							UE_LOG(LogCrowdyGameModel, Warning,
								TEXT("List Resource Nodes: skipping node '%s' whose state could not be read"),
								*ContainerId);
						}
						--(*Remaining);
						if (*Remaining <= 0)
						{
							Inner->Succeeded.Broadcast(*Accumulated, FString());
							Inner->SetReadyToDestroy();
						}
					});
			}
		});
}

TSharedPtr<FJsonObject> UCrowdyGatherNodeAction::BuildGatherParams(int32 Amount, const FString& ToStackId)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	// amount is an int (a JSON number); to_stack_id is a container_ref, which on the wire is the bare stack
	// container-id string.
	Out->SetNumberField(CrowdyWorldsimKitNames::Params::Amount, Amount);
	Out->SetStringField(CrowdyWorldsimKitNames::Params::ToStackId, ToStackId);
	return Out;
}

UCrowdyGatherNodeAction* UCrowdyGatherNodeAction::GatherNode(UObject* WorldContext, const FString& InTypePrefix,
	const FString& InNodeContainerId, int32 InAmount, const FString& InToStackContainerId, const FString& InSessionId)
{
	UCrowdyGatherNodeAction* Action = NewObject<UCrowdyGatherNodeAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->NodeContainerId = InNodeContainerId;
	Action->Amount = InAmount;
	Action->ToStackContainerId = InToStackContainerId;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGatherNodeAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!Model)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (NodeContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Gather Node needs a resource-node container id"));
		SetReadyToDestroy();
		return;
	}
	if (ToStackContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Gather Node needs a destination stack container id"));
		SetReadyToDestroy();
		return;
	}

	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyWorldsimKitNames::GatherNodeFunctionName(TypePrefix);
	Req.SelfContainerId = NodeContainerId;
	Req.SessionId = SessionId;
	Req.Params = BuildGatherParams(Amount, ToStackContainerId);

	TWeakObjectPtr<UCrowdyGatherNodeAction> WeakThis(this);
	Model->Invoke(Req, [WeakThis](FCrowdyInvokeResult Result)
		{
			UCrowdyGatherNodeAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				Action->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				Action->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyPlantCropAction* UCrowdyPlantCropAction::PlantCrop(UObject* WorldContext, const FString& InTypePrefix,
	const FString& InOutputItemId, int32 InOutputQty, int32 InMaxStage, const FString& InDisplayName,
	const FString& InSessionId)
{
	UCrowdyPlantCropAction* Action = NewObject<UCrowdyPlantCropAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->OutputItemId = InOutputItemId;
	Action->OutputQty = InOutputQty;
	Action->MaxStage = InMaxStage;
	Action->DisplayName = InDisplayName;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyPlantCropAction::Activate()
{
	UCrowdyGameModelSubsystem* ModelPtr = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!ModelPtr)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (OutputItemId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Plant Crop needs an output item id"));
		SetReadyToDestroy();
		return;
	}

	Model = ModelPtr;

	const FString TypeName = CrowdyWorldsimKitNames::CropTypeName(TypePrefix);
	const FString ResolvedDisplayName = DisplayName.IsEmpty()
		? FString::Printf(TEXT("Crop %s"), *OutputItemId)
		: DisplayName;

	TWeakObjectPtr<UCrowdyPlantCropAction> WeakThis(this);
	ModelPtr->CreateDataContainer(TypeName, ResolvedDisplayName, SessionId, FString(),
		[WeakThis](bool bOk, const FString& NewContainerId)
		{
			UCrowdyPlantCropAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk || NewContainerId.IsEmpty() || !Action->Model.IsValid())
			{
				Action->Failed.Broadcast(FString(),
					TEXT("the server rejected the crop container create (check sign-in and app scope)"));
				Action->SetReadyToDestroy();
				return;
			}
			Action->CreatedContainerId = NewContainerId;
			Action->BuildPendingWrites();
			Action->ApplyNextProperty();
		});
}

void UCrowdyPlantCropAction::BuildPendingWrites()
{
	PendingWrites.Reset();
	WriteIndex = 0;

	// The owner mirror is best-effort: the server already pins the record owner to the caller on create, and the
	// mirror property may be gated, so a rejection here must not fail the plant.
	const int64 UserId = Model.IsValid() ? Model->GetLocalUserId() : 0;
	if (UserId != 0)
	{
		PendingWrites.Add({ CrowdyWorldsimKitNames::Keys::OwnerUserId, TEXT("int"),
			FString::Printf(TEXT("%lld"), UserId), false });
	}

	PendingWrites.Add({ CrowdyWorldsimKitNames::Keys::OutputItemId, TEXT("string"),
		CrowdyKitActionSupport::MakeStringValueJson(OutputItemId), true });
	PendingWrites.Add({ CrowdyWorldsimKitNames::Keys::OutputQty, TEXT("int"), FString::FromInt(OutputQty), true });
	PendingWrites.Add({ CrowdyWorldsimKitNames::Keys::MaxStage, TEXT("int"), FString::FromInt(MaxStage), true });
}

void UCrowdyPlantCropAction::ApplyNextProperty()
{
	// Each write is issued from the previous write's completion, which can land after the world was torn down: the
	// remaining writes belong to a world that no longer exists, so stop and report rather than issuing them.
	UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
	if (!M)
	{
		Failed.Broadcast(FString(), TEXT("the Game Model subsystem went away mid-plant"));
		SetReadyToDestroy();
		return;
	}
	if (WriteIndex >= PendingWrites.Num())
	{
		Succeeded.Broadcast(CreatedContainerId, FString());
		SetReadyToDestroy();
		return;
	}

	const FPendingWrite Write = PendingWrites[WriteIndex];
	TWeakObjectPtr<UCrowdyPlantCropAction> WeakThis(this);
	M->SetDataProperty(CreatedContainerId, Write.Key, Write.ValueType, Write.ValueJson,
		[WeakThis, Key = Write.Key, bFatal = Write.bFatal](bool bWriteOk)
		{
			UCrowdyPlantCropAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bWriteOk && bFatal)
			{
				Action->Failed.Broadcast(FString(),
					FString::Printf(TEXT("the server rejected setting crop property '%s'"), *Key));
				Action->SetReadyToDestroy();
				return;
			}
			if (!bWriteOk)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("Plant Crop: optional property '%s' was not set; continuing"), *Key);
			}
			++Action->WriteIndex;
			Action->ApplyNextProperty();
		});
}

FCrowdyCrop UCrowdyListCropsAction::ParseCrop(const TSharedPtr<FJsonObject>& State, const FString& ContainerId,
	const FString& DisplayName, const FString& OwnerUserId)
{
	FCrowdyCrop Out;
	Out.ContainerId = ContainerId;
	Out.DisplayName = DisplayName;
	Out.OwnerUserId = OwnerUserId;
	if (!State.IsValid())
	{
		return Out;
	}

	double Value = 0.0;
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::Stage, Value))
	{
		Out.Stage = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::MaxStage, Value))
	{
		Out.MaxStage = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyWorldsimKitNames::Keys::OutputQty, Value))
	{
		Out.OutputQty = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	State->TryGetStringField(CrowdyWorldsimKitNames::Keys::OutputItemId, Out.OutputItemId);
	Out.bReady = Out.MaxStage > 0 && Out.Stage >= Out.MaxStage;
	return Out;
}

UCrowdyListCropsAction* UCrowdyListCropsAction::ListCrops(UObject* WorldContext, const FString& InTypePrefix,
	bool bInOnlyMine, const FString& InSessionId)
{
	UCrowdyListCropsAction* Action = NewObject<UCrowdyListCropsAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->bOnlyMine = bInOnlyMine;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyListCropsAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyCrop>(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}

	const FString TypeName = CrowdyWorldsimKitNames::CropTypeName(TypePrefix);
	const bool bFilterMine = bOnlyMine;
	const int64 LocalUserId = Model->GetLocalUserId();

	TWeakObjectPtr<UCrowdyListCropsAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(Model);
	Model->ListContainers(TypeName, SessionId,
		[WeakThis, WeakModel, bFilterMine, LocalUserId](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
		{
			UCrowdyListCropsAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk)
			{
				Action->Failed.Broadcast(TArray<FCrowdyCrop>(), TEXT("failed to list crops from the server"));
				Action->SetReadyToDestroy();
				return;
			}

			// Reduce to the rows to pull first, applying the owner filter on the cheap list metadata so a foreign
			// crop never triggers a needless per-row pull. When bOnlyMine has no local user, nothing matches.
			struct FCropRow
			{
				FString ContainerId;
				FString DisplayName;
				FString OwnerUserId;
			};
			TArray<FCropRow> Rows;
			for (const TSharedPtr<FJsonObject>& Row : Containers)
			{
				if (!Row.IsValid())
				{
					continue;
				}
				const FString OwnerStr = CrowdyWorldsimReadRowOwnerString(Row);
				if (bFilterMine)
				{
					if (OwnerStr.IsEmpty() || LocalUserId == 0)
					{
						continue;
					}
					int64 OwnerId = 0;
					LexFromString(OwnerId, *OwnerStr);
					if (OwnerId != LocalUserId)
					{
						continue;
					}
				}
				FString ContainerId;
				FString DisplayName;
				Row->TryGetStringField(TEXT("containerId"), ContainerId);
				Row->TryGetStringField(TEXT("displayName"), DisplayName);
				if (ContainerId.IsEmpty())
				{
					continue;
				}
				Rows.Add({ ContainerId, DisplayName, OwnerStr });
			}

			if (Rows.Num() == 0)
			{
				Action->Succeeded.Broadcast(TArray<FCrowdyCrop>(), FString());
				Action->SetReadyToDestroy();
				return;
			}

			if (Rows.Num() > CrowdyKitActionSupport::MaxKitListRows)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("List Crops: server returned %d crops; capping the read to %d"),
					Rows.Num(), CrowdyKitActionSupport::MaxKitListRows);
				Rows.SetNum(CrowdyKitActionSupport::MaxKitListRows);
			}

			// The reads below are issued from this completion, which can land after the world was torn down;
			// without a live world session there is nothing to read on behalf of, so fail instead of issuing
			// more server work. The caller is still told the outcome.
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel);
			if (!M)
			{
				Action->Failed.Broadcast(TArray<FCrowdyCrop>(), TEXT("the Game Model subsystem went away"));
				Action->SetReadyToDestroy();
				return;
			}

			// Fan out one pull per surviving row; the game-thread callbacks serialize, so the accumulator and the
			// remaining counter need no locking. A row whose pull fails is logged and omitted.
			const TSharedPtr<TArray<FCrowdyCrop>> Accumulated = MakeShared<TArray<FCrowdyCrop>>();
			const TSharedPtr<int32> Remaining = MakeShared<int32>(Rows.Num());
			for (const FCropRow& Row : Rows)
			{
				const FString ContainerId = Row.ContainerId;
				const FString RowDisplayName = Row.DisplayName;
				const FString OwnerStr = Row.OwnerUserId;
				M->PullContainerState(ContainerId,
					[WeakThis, Accumulated, Remaining, ContainerId, RowDisplayName, OwnerStr](bool bPullOk,
						TSharedPtr<FJsonObject> State)
					{
						UCrowdyListCropsAction* Inner = WeakThis.Get();
						if (!Inner)
						{
							return;
						}
						if (bPullOk && State.IsValid())
						{
							Accumulated->Add(ParseCrop(State, ContainerId, RowDisplayName, OwnerStr));
						}
						else
						{
							UE_LOG(LogCrowdyGameModel, Warning,
								TEXT("List Crops: skipping crop '%s' whose state could not be read"), *ContainerId);
						}
						--(*Remaining);
						if (*Remaining <= 0)
						{
							Inner->Succeeded.Broadcast(*Accumulated, FString());
							Inner->SetReadyToDestroy();
						}
					});
			}
		});
}

TSharedPtr<FJsonObject> UCrowdyHarvestCropAction::BuildHarvestParams(const FString& ToStackId)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	// to_stack_id is a container_ref, which on the wire is the bare stack container-id string.
	Out->SetStringField(CrowdyWorldsimKitNames::Params::ToStackId, ToStackId);
	return Out;
}

UCrowdyHarvestCropAction* UCrowdyHarvestCropAction::HarvestCrop(UObject* WorldContext, const FString& InTypePrefix,
	const FString& InCropContainerId, const FString& InToStackContainerId, const FString& InSessionId)
{
	UCrowdyHarvestCropAction* Action = NewObject<UCrowdyHarvestCropAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->CropContainerId = InCropContainerId;
	Action->ToStackContainerId = InToStackContainerId;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyHarvestCropAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!Model)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (CropContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Harvest Crop needs a crop container id"));
		SetReadyToDestroy();
		return;
	}
	if (ToStackContainerId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Harvest Crop needs a destination stack container id"));
		SetReadyToDestroy();
		return;
	}

	FCrowdyInvokeRequest Req;
	Req.FunctionName = CrowdyWorldsimKitNames::HarvestFunctionName(TypePrefix);
	Req.SelfContainerId = CropContainerId;
	Req.SessionId = SessionId;
	Req.Params = BuildHarvestParams(ToStackContainerId);

	TWeakObjectPtr<UCrowdyHarvestCropAction> WeakThis(this);
	Model->Invoke(Req, [WeakThis](FCrowdyInvokeResult Result)
		{
			UCrowdyHarvestCropAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (Result.bTransportOk && Result.bSuccess)
			{
				Action->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			else
			{
				Action->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}
