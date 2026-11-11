#include "Subsystem/AsyncActions/CrowdyModsActions.h"
#include "CrowdyServicesActionSupport.h"

namespace
{
	const TCHAR* const ModsMissingSubsystem = TEXT("UCrowdyMods subsystem not found");

	template <typename AnswerType>
	AnswerType ModsMissing()
	{
		AnswerType Missing;
		Missing.Message = ModsMissingSubsystem;
		return Missing;
	}
}

UCrowdyBuildMod* UCrowdyBuildMod::BuildMod(UObject* WorldContextObject, const FString& CrateName, const TArray<FCrowdyModFile>& Files)
{
	UCrowdyBuildMod* Action = NewObject<UCrowdyBuildMod>();
	Action->WorldContextObject = WorldContextObject;
	Action->CrateName = CrateName;
	Action->Files = Files;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyBuildMod::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModBuild>());
		return;
	}
	TWeakObjectPtr<UCrowdyBuildMod> WeakThis(this);
	Mods->BuildMod(CrateName, Files,
		[WeakThis](const FCrowdyModBuild& Build)
		{
			if (UCrowdyBuildMod* Self = WeakThis.Get())
			{
				Self->Broadcast(Self->OnProgress, Build);
			}
		},
		[WeakThis](const FCrowdyModBuild& Build)
		{
			if (UCrowdyBuildMod* Self = WeakThis.Get())
			{
				Self->Finish(Build);
			}
		});
}

void UCrowdyBuildMod::Broadcast(FCrowdyModBuildAsyncResult& Pin, const FCrowdyModBuild& Build)
{
	Pin.Broadcast(Build.BuildId, Build.Status, Build.Log, Build.SdkVersion, Build.Result, Build.Message);
}

void UCrowdyBuildMod::Finish(const FCrowdyModBuild& Build)
{
	Broadcast(Build.Result == ECrowdyModResult::Success ? OnDone : OnFailed, Build);
	SetReadyToDestroy();
}

UCrowdyDeployMod* UCrowdyDeployMod::DeployMod(UObject* WorldContextObject, int64 GridId, const FString& Name, const FString& BuildId)
{
	UCrowdyDeployMod* Action = NewObject<UCrowdyDeployMod>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->Name = Name;
	Action->BuildId = BuildId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyDeployMod::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModInfo>());
		return;
	}
	TWeakObjectPtr<UCrowdyDeployMod> WeakThis(this);
	Mods->DeployMod(GridId, Name, BuildId, [WeakThis](const FCrowdyModInfo& Info)
	{
		if (UCrowdyDeployMod* Self = WeakThis.Get())
		{
			Self->Finish(Info);
		}
	});
}

void UCrowdyDeployMod::Finish(const FCrowdyModInfo& Info)
{
	FCrowdyModInfoAsyncResult& Pin = Info.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Info.Mod, Info.Result, Info.Message);
	SetReadyToDestroy();
}

UCrowdySetModEnabled* UCrowdySetModEnabled::SetModEnabled(UObject* WorldContextObject, int64 GridId, const FString& Name, bool bEnabled)
{
	UCrowdySetModEnabled* Action = NewObject<UCrowdySetModEnabled>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->Name = Name;
	Action->bEnabled = bEnabled;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdySetModEnabled::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModInfo>());
		return;
	}
	TWeakObjectPtr<UCrowdySetModEnabled> WeakThis(this);
	Mods->SetModEnabled(GridId, Name, bEnabled, [WeakThis](const FCrowdyModInfo& Info)
	{
		if (UCrowdySetModEnabled* Self = WeakThis.Get())
		{
			Self->Finish(Info);
		}
	});
}

void UCrowdySetModEnabled::Finish(const FCrowdyModInfo& Info)
{
	FCrowdyModInfoAsyncResult& Pin = Info.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Info.Mod, Info.Result, Info.Message);
	SetReadyToDestroy();
}

UCrowdyListMods* UCrowdyListMods::ListMods(UObject* WorldContextObject, int64 GridId)
{
	UCrowdyListMods* Action = NewObject<UCrowdyListMods>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyListMods::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModList>());
		return;
	}
	TWeakObjectPtr<UCrowdyListMods> WeakThis(this);
	Mods->ListMods(GridId, [WeakThis](const FCrowdyModList& List)
	{
		if (UCrowdyListMods* Self = WeakThis.Get())
		{
			Self->Finish(List);
		}
	});
}

void UCrowdyListMods::Finish(const FCrowdyModList& List)
{
	FCrowdyModListAsyncResult& Pin = List.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(List.Mods, List.Result, List.Message);
	SetReadyToDestroy();
}

UCrowdyListMyMods* UCrowdyListMyMods::ListMyMods(UObject* WorldContextObject)
{
	UCrowdyListMyMods* Action = NewObject<UCrowdyListMyMods>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyListMyMods::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModList>());
		return;
	}
	TWeakObjectPtr<UCrowdyListMyMods> WeakThis(this);
	Mods->ListMyMods([WeakThis](const FCrowdyModList& List)
	{
		if (UCrowdyListMyMods* Self = WeakThis.Get())
		{
			Self->Finish(List);
		}
	});
}

void UCrowdyListMyMods::Finish(const FCrowdyModList& List)
{
	FCrowdyModListAsyncResult& Pin = List.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(List.Mods, List.Result, List.Message);
	SetReadyToDestroy();
}

UCrowdyGetModLogs* UCrowdyGetModLogs::GetModLogs(UObject* WorldContextObject, int64 GridId, const FString& Name, int32 Limit)
{
	UCrowdyGetModLogs* Action = NewObject<UCrowdyGetModLogs>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->Name = Name;
	Action->Limit = Limit;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetModLogs::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModLogs>());
		return;
	}
	TWeakObjectPtr<UCrowdyGetModLogs> WeakThis(this);
	Mods->GetModLogs(GridId, Name, Limit, [WeakThis](const FCrowdyModLogs& Logs)
	{
		if (UCrowdyGetModLogs* Self = WeakThis.Get())
		{
			Self->Finish(Logs);
		}
	});
}

void UCrowdyGetModLogs::Finish(const FCrowdyModLogs& Logs)
{
	FCrowdyModLogsAsyncResult& Pin = Logs.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Logs.Lines, Logs.Result, Logs.Message);
	SetReadyToDestroy();
}

UCrowdyDeleteMod* UCrowdyDeleteMod::DeleteMod(UObject* WorldContextObject, int64 GridId, const FString& Name)
{
	UCrowdyDeleteMod* Action = NewObject<UCrowdyDeleteMod>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyDeleteMod::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModDelete>());
		return;
	}
	TWeakObjectPtr<UCrowdyDeleteMod> WeakThis(this);
	Mods->DeleteMod(GridId, Name, [WeakThis](const FCrowdyModDelete& Delete)
	{
		if (UCrowdyDeleteMod* Self = WeakThis.Get())
		{
			Self->Finish(Delete);
		}
	});
}

void UCrowdyDeleteMod::Finish(const FCrowdyModDelete& Delete)
{
	FCrowdyModDeleteAsyncResult& Pin = Delete.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Delete.bDeleted, Delete.Result, Delete.Message);
	SetReadyToDestroy();
}

UCrowdyGetModStarter* UCrowdyGetModStarter::GetStarter(UObject* WorldContextObject)
{
	UCrowdyGetModStarter* Action = NewObject<UCrowdyGetModStarter>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetModStarter::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModStarter>());
		return;
	}
	TWeakObjectPtr<UCrowdyGetModStarter> WeakThis(this);
	Mods->GetStarter([WeakThis](const FCrowdyModStarter& Starter)
	{
		if (UCrowdyGetModStarter* Self = WeakThis.Get())
		{
			Self->Finish(Starter);
		}
	});
}

void UCrowdyGetModStarter::Finish(const FCrowdyModStarter& Starter)
{
	FCrowdyModStarterAsyncResult& Pin = Starter.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Starter.Name, Starter.Files, Starter.Result, Starter.Message);
	SetReadyToDestroy();
}

UCrowdyCallMod* UCrowdyCallMod::CallMod(UObject* WorldContextObject, int64 GridId, const FString& Name, const FString& Method, const FString& ParamsJson)
{
	UCrowdyCallMod* Action = NewObject<UCrowdyCallMod>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->Name = Name;
	Action->Method = Method;
	Action->ParamsJson = ParamsJson;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyCallMod::Activate()
{
	UCrowdyMods* Mods = GetServiceSubsystem<UCrowdyMods>(WorldContextObject);
	if (!Mods)
	{
		Finish(ModsMissing<FCrowdyModCall>());
		return;
	}
	TWeakObjectPtr<UCrowdyCallMod> WeakThis(this);
	Mods->CallMod(GridId, Name, Method, ParamsJson, [WeakThis](const FCrowdyModCall& Call)
	{
		if (UCrowdyCallMod* Self = WeakThis.Get())
		{
			Self->Finish(Call);
		}
	});
}

void UCrowdyCallMod::Finish(const FCrowdyModCall& Call)
{
	FCrowdyModCallAsyncResult& Pin = Call.Result == ECrowdyModResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Call.ReplyJson, Call.Result, Call.Message);
	SetReadyToDestroy();
}
