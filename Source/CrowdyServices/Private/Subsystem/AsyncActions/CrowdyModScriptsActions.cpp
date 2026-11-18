#include "Subsystem/AsyncActions/CrowdyModScriptsActions.h"
#include "Engine/World.h"
#include "Subsystem/CrowdyModScripts.h"

namespace
{
	const TCHAR* const ScriptsMissingSubsystem = TEXT("UCrowdyModScripts subsystem not found");

	UCrowdyModScripts* GetModScripts(const TWeakObjectPtr<UObject>& Context)
	{
		UWorld* World = Context.IsValid() ? Context->GetWorld() : nullptr;
		return World ? World->GetSubsystem<UCrowdyModScripts>() : nullptr;
	}
}

UCrowdyGetScriptStatus* UCrowdyGetScriptStatus::GetScriptStatus(UObject* WorldContextObject, int64 GridId)
{
	UCrowdyGetScriptStatus* Action = NewObject<UCrowdyGetScriptStatus>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetScriptStatus::Activate()
{
	UCrowdyModScripts* Scripts = GetModScripts(WorldContextObject);
	if (!Scripts)
	{
		FCrowdyModScriptStatusList Missing;
		Missing.GridId = GridId;
		Missing.Message = ScriptsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyGetScriptStatus> WeakThis(this);
	Scripts->GetScriptStatus(GridId, [WeakThis](const FCrowdyModScriptStatusList& Status)
	{
		if (UCrowdyGetScriptStatus* Self = WeakThis.Get())
		{
			Self->Finish(Status);
		}
	});
}

void UCrowdyGetScriptStatus::Finish(const FCrowdyModScriptStatusList& Status)
{
	FCrowdyModScriptStatusAsyncResult& Pin = Status.Result == ECrowdyModScriptReadResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Status.Scripts, Status.Result, Status.Message);
	SetReadyToDestroy();
}

UCrowdyGetScriptLog* UCrowdyGetScriptLog::GetScriptLog(UObject* WorldContextObject, int64 GridId, const FString& ModId)
{
	UCrowdyGetScriptLog* Action = NewObject<UCrowdyGetScriptLog>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->ModId = ModId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetScriptLog::Activate()
{
	UCrowdyModScripts* Scripts = GetModScripts(WorldContextObject);
	if (!Scripts)
	{
		FCrowdyModScriptLog Missing;
		Missing.ModId = ModId;
		Missing.Message = ScriptsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyGetScriptLog> WeakThis(this);
	Scripts->GetScriptLog(GridId, ModId, [WeakThis](const FCrowdyModScriptLog& Log)
	{
		if (UCrowdyGetScriptLog* Self = WeakThis.Get())
		{
			Self->Finish(Log);
		}
	});
}

void UCrowdyGetScriptLog::Finish(const FCrowdyModScriptLog& Log)
{
	FCrowdyModScriptLogAsyncResult& Pin = Log.Result == ECrowdyModScriptReadResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Log.Lines, Log.Result, Log.Message);
	SetReadyToDestroy();
}
