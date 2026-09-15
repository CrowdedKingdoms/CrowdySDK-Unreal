// Copyright Epic Games, Inc. All Rights Reserved.

#include "Replication/GameModel/CrowdyGameModelSessionActions.h"

#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelActionSupport.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

namespace
{
	// The failure a node reports when it could not even reach the subsystem (no play world, or the world is gone).
	FCrowdyModelFailure NoSubsystemFailure()
	{
		FCrowdyModelFailure Failure;
		Failure.Error = ECrowdySessionError::Other;
		Failure.Message = TEXT("No Game Model subsystem: the node was called outside a play world.");
		return Failure;
	}

	int32 HostTermFor(bool bRefuseIfHostChanged)
	{
		return bRefuseIfHostChanged ? UCrowdyGameModelSubsystem::UseKnownHostTerm : UCrowdyGameModelSubsystem::SkipHostTermCheck;
	}
}

UCrowdyCreateSessionAction* UCrowdyCreateSessionAction::CreateSession(UObject* WorldContext, const FString& Name,
	const FCrowdyGameModelCreateSessionOptions& Options, const TArray<int64>& ParticipantUserIds,
	bool bMakeActive, bool bWatchForChanges, const FString& MetadataJson)
{
	UCrowdyCreateSessionAction* Action = NewObject<UCrowdyCreateSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->Name = Name;
	Action->ParticipantUserIds = ParticipantUserIds;
	Action->MetadataJson = MetadataJson;
	Action->Options = Options;
	Action->bMakeActive = bMakeActive;
	Action->bWatchForChanges = bWatchForChanges;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyCreateSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyCreateSessionAction> WeakThis(this);
	Model->CreateSession(Name, ParticipantUserIds, MetadataJson, Options,
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdyCreateSessionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
			if (!bOk)
			{
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
				Action->SetReadyToDestroy();
				return;
			}
			if (Model && Action->bMakeActive)
			{
				Model->SetActiveSession(Session.SessionId);
			}
			if (Model && Action->bWatchForChanges)
			{
				Model->WatchSession(Session.SessionId);
			}
			Action->Succeeded.Broadcast(Session);
			Action->SetReadyToDestroy();
		});
}

UCrowdyJoinSessionAction* UCrowdyJoinSessionAction::JoinSession(UObject* WorldContext, const FString& SessionId,
	bool bMakeActive, bool bWatchForChanges, const FString& Role, bool bBindPresenceToMyActor)
{
	UCrowdyJoinSessionAction* Action = NewObject<UCrowdyJoinSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->Role = Role;
	Action->bMakeActive = bMakeActive;
	Action->bWatchForChanges = bWatchForChanges;
	Action->bBindPresenceToMyActor = bBindPresenceToMyActor;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyJoinSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyJoinSessionAction> WeakThis(this);
	Model->JoinSession(SessionId, Role, bBindPresenceToMyActor,
		[WeakThis](bool bOk, const FCrowdyGameModelSessionParticipant& Participant)
		{
			UCrowdyJoinSessionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
			if (!bOk)
			{
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
				Action->SetReadyToDestroy();
				return;
			}
			// The row names the session even when the node was given an empty id and joined the active one.
			const FString Joined = Participant.SessionId.IsEmpty() ? Action->SessionId : Participant.SessionId;
			if (Model && Action->bMakeActive && !Joined.IsEmpty())
			{
				Model->SetActiveSession(Joined);
			}
			if (Model && Action->bWatchForChanges)
			{
				Model->WatchSession(Joined);
			}
			Action->Succeeded.Broadcast(Participant);
			Action->SetReadyToDestroy();
		});
}

UCrowdyLeaveSessionAction* UCrowdyLeaveSessionAction::LeaveSession(UObject* WorldContext, const FString& SessionId, int32 Incarnation)
{
	UCrowdyLeaveSessionAction* Action = NewObject<UCrowdyLeaveSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->Incarnation = Incarnation;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyLeaveSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyLeaveSessionAction> WeakThis(this);
	Model->LeaveSession(SessionId, Incarnation,
		[WeakThis](bool bOk, const FCrowdyGameModelSessionParticipant& Participant)
		{
			UCrowdyLeaveSessionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
			if (!bOk)
			{
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
				Action->SetReadyToDestroy();
				return;
			}
			if (Model)
			{
				Model->UnwatchSession(Action->SessionId);
			}
			Action->Succeeded.Broadcast(Participant);
			Action->SetReadyToDestroy();
		});
}

UCrowdySetSessionAdmissionAction* UCrowdySetSessionAdmissionAction::SetSessionAdmission(UObject* WorldContext,
	ECrowdySessionAdmission Admission, const FString& SessionId, bool bRefuseIfHostChanged)
{
	UCrowdySetSessionAdmissionAction* Action = NewObject<UCrowdySetSessionAdmissionAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->Admission = Admission;
	Action->bRefuseIfHostChanged = bRefuseIfHostChanged;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdySetSessionAdmissionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdySetSessionAdmissionAction> WeakThis(this);
	Model->SetSessionAdmission(SessionId, Admission, HostTermFor(bRefuseIfHostChanged),
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdySetSessionAdmissionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Session);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyTransferSessionHostAction* UCrowdyTransferSessionHostAction::TransferSessionHost(UObject* WorldContext,
	int64 ToUserId, const FString& SessionId, bool bRefuseIfHostChanged)
{
	UCrowdyTransferSessionHostAction* Action = NewObject<UCrowdyTransferSessionHostAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->ToUserId = ToUserId;
	Action->bRefuseIfHostChanged = bRefuseIfHostChanged;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyTransferSessionHostAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyTransferSessionHostAction> WeakThis(this);
	Model->TransferSessionHost(SessionId, ToUserId, HostTermFor(bRefuseIfHostChanged),
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdyTransferSessionHostAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Session);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyEndSessionAction* UCrowdyEndSessionAction::EndSession(UObject* WorldContext, const FString& SessionId,
	ECrowdySessionEndReason Reason, bool bRefuseIfHostChanged)
{
	UCrowdyEndSessionAction* Action = NewObject<UCrowdyEndSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->Reason = Reason;
	Action->bRefuseIfHostChanged = bRefuseIfHostChanged;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyEndSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyEndSessionAction> WeakThis(this);
	Model->EndSession(SessionId, Reason, HostTermFor(bRefuseIfHostChanged),
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdyEndSessionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
			if (!bOk)
			{
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
				Action->SetReadyToDestroy();
				return;
			}
			if (Model)
			{
				Model->UnwatchSession(Session.SessionId.IsEmpty() ? Action->SessionId : Session.SessionId);
			}
			Action->Succeeded.Broadcast(Session);
			Action->SetReadyToDestroy();
		});
}

UCrowdySetSessionTurnAction* UCrowdySetSessionTurnAction::SetSessionTurn(UObject* WorldContext, int64 UserId,
	const FString& SessionId, bool bClearTurn, bool bRefuseIfHostChanged)
{
	UCrowdySetSessionTurnAction* Action = NewObject<UCrowdySetSessionTurnAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->UserId = UserId;
	Action->bClearTurn = bClearTurn;
	Action->bRefuseIfHostChanged = bRefuseIfHostChanged;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdySetSessionTurnAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdySetSessionTurnAction> WeakThis(this);
	Model->SetSessionTurn(SessionId, UserId, !bClearTurn,
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdySetSessionTurnAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Session);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		}, HostTermFor(bRefuseIfHostChanged));
}

UCrowdyListSessionsAction* UCrowdyListSessionsAction::ListSessions(UObject* WorldContext,
	ECrowdySessionStatusFilter Status, ECrowdySessionAdmissionFilter Admission, int64 HostUserId, int32 Limit)
{
	UCrowdyListSessionsAction* Action = NewObject<UCrowdyListSessionsAction>();
	Action->WorldContextObject = WorldContext;
	Action->Status = Status;
	Action->Admission = Admission;
	Action->HostUserId = HostUserId;
	Action->Limit = Limit;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyListSessionsAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyListSessionsAction> WeakThis(this);
	Model->ListSessions(Status, Admission, HostUserId, Limit,
		[WeakThis](bool bOk, const TArray<FCrowdyGameModelSession>& Sessions)
		{
			UCrowdyListSessionsAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Sessions);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyGetSessionAction* UCrowdyGetSessionAction::GetSession(UObject* WorldContext, const FString& SessionId)
{
	UCrowdyGetSessionAction* Action = NewObject<UCrowdyGetSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGetSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyGetSessionAction> WeakThis(this);
	Model->GetSession(SessionId,
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdyGetSessionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Session);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyGetSessionSnapshotAction* UCrowdyGetSessionSnapshotAction::GetSessionSnapshot(UObject* WorldContext, const FString& SessionId)
{
	UCrowdyGetSessionSnapshotAction* Action = NewObject<UCrowdyGetSessionSnapshotAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGetSessionSnapshotAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyGetSessionSnapshotAction> WeakThis(this);
	Model->GetSessionSnapshot(SessionId,
		[WeakThis](bool bOk, const FCrowdyGameModelSessionSnapshot& Snapshot)
		{
			UCrowdyGetSessionSnapshotAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Snapshot);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyGetSessionEventsAction* UCrowdyGetSessionEventsAction::GetSessionEvents(UObject* WorldContext,
	const FString& SessionId, int64 AfterRevision, int32 Limit)
{
	UCrowdyGetSessionEventsAction* Action = NewObject<UCrowdyGetSessionEventsAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->AfterRevision = AfterRevision;
	Action->Limit = Limit;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyGetSessionEventsAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(NoSubsystemFailure());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyGetSessionEventsAction> WeakThis(this);
	Model->GetSessionEvents(SessionId, AfterRevision, Limit,
		[WeakThis](bool bOk, const TArray<FCrowdyGameModelSessionEvent>& Events)
		{
			UCrowdyGetSessionEventsAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast(Events);
			}
			else
			{
				UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(Action->WorldContextObject);
				Action->Failed.Broadcast(Model ? Model->GetLastFailure() : NoSubsystemFailure());
			}
			Action->SetReadyToDestroy();
		});
}
