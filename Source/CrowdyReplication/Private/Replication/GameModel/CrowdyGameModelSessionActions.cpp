// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelSessionActions.h"

#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelActionSupport.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

UCrowdyCreateSessionAction* UCrowdyCreateSessionAction::CreateSession(UObject* WorldContext, const FString& Name,
	const TArray<int64>& ParticipantUserIds, const FString& MetadataJson)
{
	UCrowdyCreateSessionAction* Action = NewObject<UCrowdyCreateSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->Name = Name;
	Action->ParticipantUserIds = ParticipantUserIds;
	Action->MetadataJson = MetadataJson;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyCreateSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(FCrowdyGameModelSession());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyCreateSessionAction> WeakThis(this);
	Model->CreateSession(Name, ParticipantUserIds, MetadataJson,
		[WeakThis](bool bOk, const FCrowdyGameModelSession& Session)
		{
			UCrowdyCreateSessionAction* Action = WeakThis.Get();
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
				Action->Failed.Broadcast(Session);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyJoinSessionAction* UCrowdyJoinSessionAction::JoinSession(UObject* WorldContext, const FString& SessionId, const FString& Role)
{
	UCrowdyJoinSessionAction* Action = NewObject<UCrowdyJoinSessionAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->Role = Role;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyJoinSessionAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyJoinSessionAction> WeakThis(this);
	Model->JoinSession(SessionId, Role,
		[WeakThis](bool bOk)
		{
			UCrowdyJoinSessionAction* Action = WeakThis.Get();
			if (!Action)
			{
				return;
			}
			if (bOk)
			{
				Action->Succeeded.Broadcast();
			}
			else
			{
				Action->Failed.Broadcast();
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdySetSessionTurnAction* UCrowdySetSessionTurnAction::SetSessionTurn(UObject* WorldContext, const FString& SessionId,
	int64 UserId, bool bClearTurn)
{
	UCrowdySetSessionTurnAction* Action = NewObject<UCrowdySetSessionTurnAction>();
	Action->WorldContextObject = WorldContext;
	Action->SessionId = SessionId;
	Action->UserId = UserId;
	Action->bClearTurn = bClearTurn;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdySetSessionTurnAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(FCrowdyGameModelSession());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdySetSessionTurnAction> WeakThis(this);
	const bool bHasUserId = !bClearTurn;
	Model->SetSessionTurn(SessionId, UserId, bHasUserId,
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
				Action->Failed.Broadcast(Session);
			}
			Action->SetReadyToDestroy();
		});
}

UCrowdyListSessionsAction* UCrowdyListSessionsAction::ListSessions(UObject* WorldContext, const FString& Status)
{
	UCrowdyListSessionsAction* Action = NewObject<UCrowdyListSessionsAction>();
	Action->WorldContextObject = WorldContext;
	Action->Status = Status;
	Action->RegisterWithGameInstance(WorldContext);
	return Action;
}

void UCrowdyListSessionsAction::Activate()
{
	UCrowdyGameModelSubsystem* Model = GetGameModelSubsystem(WorldContextObject);
	if (!Model)
	{
		Failed.Broadcast(TArray<FCrowdyGameModelSession>());
		SetReadyToDestroy();
		return;
	}

	TWeakObjectPtr<UCrowdyListSessionsAction> WeakThis(this);
	Model->ListSessions(Status,
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
				Action->Failed.Broadcast(Sessions);
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
		Failed.Broadcast(FCrowdyGameModelSession());
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
				Action->Failed.Broadcast(Session);
			}
			Action->SetReadyToDestroy();
		});
}
