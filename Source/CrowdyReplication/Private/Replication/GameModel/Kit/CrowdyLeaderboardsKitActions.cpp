// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyLeaderboardsKitActions.h"

#include "CrowdyGameModelLog.h"
#include "Dom/JsonObject.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/Kit/CrowdyKitActionSupport.h"
#include "Replication/GameModel/Kit/CrowdyLeaderboardsKitNames.h"

namespace
{
	// Read a listed container row's ownerUserId as a string. The subsystem row parsers read it as a string field
	// (a BigInt id arrives as a JSON string), but a numeric encoding is accepted too so a differently-typed row is
	// not dropped. Empty when the row carries no owner.
	FString CrowdyLeaderboardsReadRowOwner(const TSharedPtr<FJsonObject>& Row)
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
			return FString::Printf(TEXT("%lld"), static_cast<int64>(OwnerNum));
		}
		return FString();
	}

	// The find-or-create walk for one player's entry on one board, shared by Submit Score and Ensure Leaderboard
	// Entry. It lists the entry type, considers only the rows the local user owns, pulls each such row to match its
	// board_id, and creates + seeds a new entry when none matches. The walk keeps itself alive through its async
	// captures (each pending callback holds a strong self-ref) and releases once it completes, so a node need not
	// outlive its own broadcast. OnComplete runs on the game thread exactly once: (bOk, ContainerId, ErrorMessage).
	// Every step after the first is issued from the previous step's completion, which can land after the world was
	// torn down, so each one re-resolves the subsystem through ResolveLive: no live world session means the walk
	// stops and completes as failed rather than issuing more server work for a world that is gone.
	class FCrowdyLeaderboardsEntryResolver : public TSharedFromThis<FCrowdyLeaderboardsEntryResolver>
	{
	public:
		FCrowdyLeaderboardsEntryResolver(UCrowdyGameModelSubsystem* InModel, const FString& InTypeName,
			const FString& InBoardId, const FString& InDisplayName, const FString& InSessionId, int64 InLocalUserId)
			: Model(InModel)
			, TypeName(InTypeName)
			, BoardId(InBoardId)
			, DisplayName(InDisplayName)
			, SessionId(InSessionId)
			, LocalUserId(InLocalUserId)
		{
		}

		void Start(TFunction<void(bool bOk, const FString& ContainerId, const FString& ErrorMessage)> InOnComplete)
		{
			OnComplete = MoveTemp(InOnComplete);

			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
			if (!M)
			{
				Complete(false, FString(), TEXT("the Game Model subsystem went away"));
				return;
			}

			TSharedRef<FCrowdyLeaderboardsEntryResolver> Self = AsShared();
			M->ListContainers(TypeName, SessionId,
				[Self](bool bOk, TArray<TSharedPtr<FJsonObject>> Rows)
				{
					Self->OnListed(bOk, MoveTemp(Rows));
				});
		}

	private:
		void OnListed(bool bOk, TArray<TSharedPtr<FJsonObject>> Rows)
		{
			if (!bOk)
			{
				Complete(false, FString(), TEXT("failed to list leaderboard entries"));
				return;
			}

			if (Rows.Num() > CrowdyKitActionSupport::MaxKitListRows)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("Leaderboard entry lookup: server returned %d rows; capping the scan to %d"),
					Rows.Num(), CrowdyKitActionSupport::MaxKitListRows);
				Rows.SetNum(CrowdyKitActionSupport::MaxKitListRows);
			}

			// Only the caller's own rows can be this player's entry. With no signed-in user id there is nothing to
			// match on, so the walk falls through to create.
			OwnedContainerIds.Reset();
			if (LocalUserId != 0)
			{
				for (const TSharedPtr<FJsonObject>& Row : Rows)
				{
					if (!Row.IsValid())
					{
						continue;
					}
					int64 Owner = 0;
					LexFromString(Owner, *CrowdyLeaderboardsReadRowOwner(Row));
					if (Owner != LocalUserId)
					{
						continue;
					}
					FString ContainerId;
					if (Row->TryGetStringField(TEXT("containerId"), ContainerId) && !ContainerId.IsEmpty())
					{
						OwnedContainerIds.Add(ContainerId);
					}
				}
			}

			RowIndex = 0;
			PullNext();
		}

		void PullNext()
		{
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
			if (!M)
			{
				Complete(false, FString(), TEXT("the Game Model subsystem went away"));
				return;
			}
			if (RowIndex >= OwnedContainerIds.Num())
			{
				CreateEntry();
				return;
			}

			const FString ContainerId = OwnedContainerIds[RowIndex];
			TSharedRef<FCrowdyLeaderboardsEntryResolver> Self = AsShared();
			M->PullContainerState(ContainerId,
				[Self, ContainerId](bool bPullOk, TSharedPtr<FJsonObject> State)
				{
					Self->OnPulled(bPullOk, State, ContainerId);
				});
		}

		void OnPulled(bool bOk, const TSharedPtr<FJsonObject>& State, const FString& ContainerId)
		{
			if (bOk && State.IsValid())
			{
				FString Board;
				if (State->TryGetStringField(CrowdyLeaderboardsKitNames::Keys::BoardId, Board) && Board == BoardId)
				{
					Complete(true, ContainerId, FString());
					return;
				}
			}
			++RowIndex;
			PullNext();
		}

		void CreateEntry()
		{
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
			if (!M)
			{
				Complete(false, FString(), TEXT("the Game Model subsystem went away"));
				return;
			}

			const FString NewDisplayName = DisplayName.IsEmpty() ? BoardId : DisplayName;
			TSharedRef<FCrowdyLeaderboardsEntryResolver> Self = AsShared();
			M->CreateDataContainer(TypeName, NewDisplayName, SessionId, FString(),
				[Self](bool bCreateOk, const FString& NewContainerId)
				{
					Self->OnCreated(bCreateOk, NewContainerId);
				});
		}

		void OnCreated(bool bOk, const FString& NewContainerId)
		{
			if (!bOk || NewContainerId.IsEmpty())
			{
				Complete(false, FString(),
					TEXT("the server rejected the leaderboard entry create (check sign-in and app scope)"));
				return;
			}
			CreatedContainerId = NewContainerId;
			SeedOwner();
		}

		void SeedOwner()
		{
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
			if (!M)
			{
				Complete(false, FString(), TEXT("the Game Model subsystem went away"));
				return;
			}

			// The owner mirror is best-effort: the server already pins the record owner to the caller on create, and
			// the mirror property may be gated, so a rejection here must not fail the ensure. Skip it entirely when
			// signed out (no id to write).
			if (LocalUserId == 0)
			{
				SeedBoard();
				return;
			}

			TSharedRef<FCrowdyLeaderboardsEntryResolver> Self = AsShared();
			M->SetDataProperty(CreatedContainerId, CrowdyLeaderboardsKitNames::Keys::OwnerUserId, TEXT("int"),
				FString::Printf(TEXT("%lld"), LocalUserId),
				[Self](bool bWriteOk)
				{
					if (!bWriteOk)
					{
						UE_LOG(LogCrowdyGameModel, Warning,
							TEXT("Ensure Leaderboard Entry: optional owner_user_id mirror was not set; continuing"));
					}
					Self->SeedBoard();
				});
		}

		void SeedBoard()
		{
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
			if (!M)
			{
				Complete(false, FString(), TEXT("the Game Model subsystem went away"));
				return;
			}

			// board_id is the key the entry is looked up by, so a rejected write leaves an unusable orphan entry:
			// fail the ensure rather than return a container that no later read can match.
			TSharedRef<FCrowdyLeaderboardsEntryResolver> Self = AsShared();
			M->SetDataProperty(CreatedContainerId, CrowdyLeaderboardsKitNames::Keys::BoardId, TEXT("string"),
				CrowdyKitActionSupport::MakeStringValueJson(BoardId),
				[Self](bool bWriteOk)
				{
					if (!bWriteOk)
					{
						Self->Complete(false, FString(),
							TEXT("the server rejected the board_id write on the new leaderboard entry"));
						return;
					}
					Self->Complete(true, Self->CreatedContainerId, FString());
				});
		}

		void Complete(bool bOk, const FString& ContainerId, const FString& ErrorMessage)
		{
			if (bDone)
			{
				return;
			}
			bDone = true;
			if (OnComplete)
			{
				OnComplete(bOk, ContainerId, ErrorMessage);
			}
		}

		TWeakObjectPtr<UCrowdyGameModelSubsystem> Model;
		FString TypeName;
		FString BoardId;
		FString DisplayName;
		FString SessionId;
		int64 LocalUserId = 0;

		TFunction<void(bool, const FString&, const FString&)> OnComplete;
		TArray<FString> OwnedContainerIds;
		int32 RowIndex = 0;
		FString CreatedContainerId;
		bool bDone = false;
	};
}

UCrowdySubmitScoreAction* UCrowdySubmitScoreAction::SubmitScore(UObject* WorldContext, const FString& InTypePrefix,
	const FString& InBoardId, int32 InPoints, const FString& InDisplayName, const FString& InSessionId)
{
	UCrowdySubmitScoreAction* Action = NewObject<UCrowdySubmitScoreAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->BoardId = InBoardId;
	Action->Points = InPoints;
	Action->DisplayName = InDisplayName;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

TSharedPtr<FJsonObject> UCrowdySubmitScoreAction::BuildSubmitScoreParams(int32 Points)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	// points is an int parameter, which on the wire is a bare JSON number.
	Out->SetNumberField(TEXT("points"), Points);
	return Out;
}

void UCrowdySubmitScoreAction::Activate()
{
	UCrowdyGameModelSubsystem* ModelPtr = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!ModelPtr)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (BoardId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Submit Score needs a Board Id"));
		SetReadyToDestroy();
		return;
	}

	const FString TypeName = CrowdyLeaderboardsKitNames::EntryTypeName(TypePrefix);
	const TSharedRef<FCrowdyLeaderboardsEntryResolver> Resolver = MakeShared<FCrowdyLeaderboardsEntryResolver>(
		ModelPtr, TypeName, BoardId, DisplayName, SessionId, ModelPtr->GetLocalUserId());

	TWeakObjectPtr<UCrowdySubmitScoreAction> WeakThis(this);
	TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakModel(ModelPtr);
	Resolver->Start(
		[WeakThis, WeakModel](bool bOk, const FString& ContainerId, const FString& ErrorMessage)
		{
			UCrowdySubmitScoreAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk)
			{
				Action->Failed.Broadcast(FString(), ErrorMessage);
				Action->SetReadyToDestroy();
				return;
			}
			// The entry resolved, but the submit is issued from that walk's completion: without a live world session
			// there is nothing to submit on behalf of, so fail rather than invoke.
			UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(WeakModel);
			if (!M)
			{
				Action->Failed.Broadcast(FString(), TEXT("the Game Model subsystem went away before submit"));
				Action->SetReadyToDestroy();
				return;
			}

			FCrowdyInvokeRequest Req;
			Req.FunctionName = CrowdyLeaderboardsKitNames::SubmitScoreFunctionName(Action->TypePrefix);
			Req.SelfContainerId = ContainerId;
			Req.SessionId = Action->SessionId;
			Req.Params = BuildSubmitScoreParams(Action->Points);

			TWeakObjectPtr<UCrowdySubmitScoreAction> WeakThisInner(Action);
			M->Invoke(Req, [WeakThisInner](FCrowdyInvokeResult Result)
				{
					UCrowdySubmitScoreAction* Inner = WeakThisInner.Get();
					if (!Inner)
					{
						return;
					}
					if (Result.bTransportOk && Result.bSuccess)
					{
						Inner->Succeeded.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
					}
					else
					{
						Inner->Failed.Broadcast(Result.ReturnValueJson, Result.ErrorMessage);
					}
					Inner->SetReadyToDestroy();
				});
		});
}

UCrowdyEnsureLeaderboardEntryAction* UCrowdyEnsureLeaderboardEntryAction::EnsureLeaderboardEntry(UObject* WorldContext,
	const FString& InTypePrefix, const FString& InBoardId, const FString& InDisplayName, const FString& InSessionId)
{
	UCrowdyEnsureLeaderboardEntryAction* Action = NewObject<UCrowdyEnsureLeaderboardEntryAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->BoardId = InBoardId;
	Action->DisplayName = InDisplayName;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyEnsureLeaderboardEntryAction::Activate()
{
	UCrowdyGameModelSubsystem* ModelPtr = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!ModelPtr)
	{
		Failed.Broadcast(FString(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (BoardId.IsEmpty())
	{
		Failed.Broadcast(FString(), TEXT("Ensure Leaderboard Entry needs a Board Id"));
		SetReadyToDestroy();
		return;
	}

	const FString TypeName = CrowdyLeaderboardsKitNames::EntryTypeName(TypePrefix);
	const TSharedRef<FCrowdyLeaderboardsEntryResolver> Resolver = MakeShared<FCrowdyLeaderboardsEntryResolver>(
		ModelPtr, TypeName, BoardId, DisplayName, SessionId, ModelPtr->GetLocalUserId());

	TWeakObjectPtr<UCrowdyEnsureLeaderboardEntryAction> WeakThis(this);
	Resolver->Start(
		[WeakThis](bool bOk, const FString& ContainerId, const FString& ErrorMessage)
		{
			UCrowdyEnsureLeaderboardEntryAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(ContainerId, FString());
			}
			else
			{
				Action->Failed.Broadcast(FString(), ErrorMessage);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyGetLeaderboardAction* UCrowdyGetLeaderboardAction::GetLeaderboard(UObject* WorldContext,
	const FString& InTypePrefix, const FString& InBoardId, int32 InTopN, const FString& InSessionId)
{
	UCrowdyGetLeaderboardAction* Action = NewObject<UCrowdyGetLeaderboardAction>();
	Action->WorldContextObject = WorldContext;
	Action->TypePrefix = InTypePrefix;
	Action->BoardId = InBoardId;
	Action->TopN = InTopN;
	Action->SessionId = InSessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

FCrowdyLeaderboardEntry UCrowdyGetLeaderboardAction::ParseLeaderboardEntry(const TSharedPtr<FJsonObject>& State,
	const FString& ContainerId, const FString& DisplayName, const FString& OwnerUserId)
{
	FCrowdyLeaderboardEntry Out;
	Out.ContainerId = ContainerId;
	Out.DisplayName = DisplayName;
	Out.OwnerUserId = OwnerUserId;
	if (!State.IsValid())
	{
		return Out;
	}

	FString Board;
	if (State->TryGetStringField(CrowdyLeaderboardsKitNames::Keys::BoardId, Board))
	{
		Out.BoardId = Board;
	}
	double Value = 0.0;
	if (State->TryGetNumberField(CrowdyLeaderboardsKitNames::Keys::Score, Value))
	{
		Out.Score = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyLeaderboardsKitNames::Keys::Season, Value))
	{
		Out.Season = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	if (State->TryGetNumberField(CrowdyLeaderboardsKitNames::Keys::Rank, Value))
	{
		Out.Rank = CrowdyKitActionSupport::ClampDoubleToInt32(Value);
	}
	return Out;
}

void UCrowdyGetLeaderboardAction::RankEntries(TArray<FCrowdyLeaderboardEntry>& Entries)
{
	// A stable sort keeps the listed order among equal scores, so a tie reads back in a deterministic (insertion)
	// order rather than shuffling between reads.
	Entries.StableSort([](const FCrowdyLeaderboardEntry& A, const FCrowdyLeaderboardEntry& B)
		{
			return A.Score > B.Score;
		});
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		Entries[Index].Position = Index + 1;
	}
}

void UCrowdyGetLeaderboardAction::Activate()
{
	UCrowdyGameModelSubsystem* ModelPtr = CrowdyKitActionSupport::ResolveModel(WorldContextObject.Get());
	if (!ModelPtr)
	{
		Failed.Broadcast(TArray<FCrowdyLeaderboardEntry>(), TEXT("no Game Model subsystem (not a play world?)"));
		SetReadyToDestroy();
		return;
	}
	if (BoardId.IsEmpty())
	{
		Failed.Broadcast(TArray<FCrowdyLeaderboardEntry>(), TEXT("Get Leaderboard needs a Board Id"));
		SetReadyToDestroy();
		return;
	}

	Model = ModelPtr;
	const FString TypeName = CrowdyLeaderboardsKitNames::EntryTypeName(TypePrefix);

	TWeakObjectPtr<UCrowdyGetLeaderboardAction> WeakThis(this);
	ModelPtr->ListContainers(TypeName, SessionId,
		[WeakThis](bool bOk, TArray<TSharedPtr<FJsonObject>> Rows)
		{
			UCrowdyGetLeaderboardAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (!bOk)
			{
				Action->Failed.Broadcast(TArray<FCrowdyLeaderboardEntry>(),
					TEXT("failed to list leaderboard entries"));
				Action->SetReadyToDestroy();
				return;
			}
			Action->ListedRows = MoveTemp(Rows);
			if (Action->ListedRows.Num() > CrowdyKitActionSupport::MaxKitListRows)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("Get Leaderboard: server returned %d entries; capping the read to %d"),
					Action->ListedRows.Num(), CrowdyKitActionSupport::MaxKitListRows);
				Action->ListedRows.SetNum(CrowdyKitActionSupport::MaxKitListRows);
			}
			Action->RowIndex = 0;
			Action->Collected.Reset();
			Action->PullNextRow();
		});
}

void UCrowdyGetLeaderboardAction::PullNextRow()
{
	// Each row is read from the previous row's completion, which can land after the world was torn down: the
	// remaining reads belong to a world that no longer exists, so stop and report rather than issuing them.
	UCrowdyGameModelSubsystem* M = UCrowdyGameModelSubsystem::ResolveLive(Model);
	if (!M)
	{
		Failed.Broadcast(TArray<FCrowdyLeaderboardEntry>(), TEXT("the Game Model subsystem went away mid-read"));
		SetReadyToDestroy();
		return;
	}
	if (RowIndex >= ListedRows.Num())
	{
		FinishAndBroadcast();
		return;
	}

	const TSharedPtr<FJsonObject>& Row = ListedRows[RowIndex];
	FString ContainerId;
	if (!Row.IsValid() || !Row->TryGetStringField(TEXT("containerId"), ContainerId) || ContainerId.IsEmpty())
	{
		++RowIndex;
		PullNextRow();
		return;
	}
	FString RowDisplayName;
	Row->TryGetStringField(TEXT("displayName"), RowDisplayName);
	const FString RowOwner = CrowdyLeaderboardsReadRowOwner(Row);

	TWeakObjectPtr<UCrowdyGetLeaderboardAction> WeakThis(this);
	M->PullContainerState(ContainerId,
		[WeakThis, ContainerId, RowDisplayName, RowOwner](bool bPullOk, TSharedPtr<FJsonObject> State)
		{
			UCrowdyGetLeaderboardAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bPullOk && State.IsValid())
			{
				const FCrowdyLeaderboardEntry Entry =
					ParseLeaderboardEntry(State, ContainerId, RowDisplayName, RowOwner);
				if (Entry.BoardId == Action->BoardId)
				{
					Action->Collected.Add(Entry);
				}
			}
			++Action->RowIndex;
			Action->PullNextRow();
		});
}

void UCrowdyGetLeaderboardAction::FinishAndBroadcast()
{
	RankEntries(Collected);
	// TopN caps the returned page; a negative value means no cap (return the whole board).
	if (TopN >= 0 && Collected.Num() > TopN)
	{
		Collected.SetNum(TopN);
	}
	Succeeded.Broadcast(Collected, FString());
	SetReadyToDestroy();
}
