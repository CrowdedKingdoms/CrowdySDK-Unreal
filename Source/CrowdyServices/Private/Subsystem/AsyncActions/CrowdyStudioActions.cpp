#include "Subsystem/AsyncActions/CrowdyStudioActions.h"
#include "CrowdyServicesActionSupport.h"

namespace
{
	const TCHAR* const StudioAgentMissing = TEXT("UCrowdyStudioAgent subsystem not found");
	const TCHAR* const StudioProjectsMissing = TEXT("UCrowdyStudioProjects subsystem not found");

	template <typename AnswerType>
	AnswerType StudioMissing(const TCHAR* Message)
	{
		AnswerType Missing;
		Missing.Message = Message;
		return Missing;
	}
}

UCrowdyGetAgentBoot* UCrowdyGetAgentBoot::GetAgentBoot(UObject* WorldContextObject)
{
	UCrowdyGetAgentBoot* Action = NewObject<UCrowdyGetAgentBoot>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetAgentBoot::Activate()
{
	UCrowdyStudioAgent* Agent = GetServiceSubsystem<UCrowdyStudioAgent>(WorldContextObject);
	if (!Agent)
	{
		Finish(StudioMissing<FCrowdyStudioAgentBoot>(StudioAgentMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyGetAgentBoot> WeakThis(this);
	Agent->GetAgentBoot([WeakThis](const FCrowdyStudioAgentBoot& Boot)
	{
		if (UCrowdyGetAgentBoot* Self = WeakThis.Get())
		{
			Self->Finish(Boot);
		}
	});
}

void UCrowdyGetAgentBoot::Finish(const FCrowdyStudioAgentBoot& Boot)
{
	FCrowdyStudioBootAsyncResult& Pin = Boot.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Boot, Boot.Result, Boot.Message);
	SetReadyToDestroy();
}

UCrowdyGetProviderConsent* UCrowdyGetProviderConsent::GetProviderConsent(UObject* WorldContextObject)
{
	UCrowdyGetProviderConsent* Action = NewObject<UCrowdyGetProviderConsent>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetProviderConsent::Activate()
{
	UCrowdyStudioAgent* Agent = GetServiceSubsystem<UCrowdyStudioAgent>(WorldContextObject);
	if (!Agent)
	{
		Finish(StudioMissing<FCrowdyStudioConsent>(StudioAgentMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyGetProviderConsent> WeakThis(this);
	Agent->GetProviderConsent([WeakThis](const FCrowdyStudioConsent& Consent)
	{
		if (UCrowdyGetProviderConsent* Self = WeakThis.Get())
		{
			Self->Finish(Consent);
		}
	});
}

void UCrowdyGetProviderConsent::Finish(const FCrowdyStudioConsent& Consent)
{
	FCrowdyStudioConsentAsyncResult& Pin = Consent.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Consent.bConsented, Consent.ConsentedAtMs, Consent.Result, Consent.Message);
	SetReadyToDestroy();
}

UCrowdyRecordProviderConsent* UCrowdyRecordProviderConsent::RecordProviderConsent(UObject* WorldContextObject, bool bConsented)
{
	UCrowdyRecordProviderConsent* Action = NewObject<UCrowdyRecordProviderConsent>();
	Action->WorldContextObject = WorldContextObject;
	Action->bConsented = bConsented;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyRecordProviderConsent::Activate()
{
	UCrowdyStudioAgent* Agent = GetServiceSubsystem<UCrowdyStudioAgent>(WorldContextObject);
	if (!Agent)
	{
		Finish(StudioMissing<FCrowdyStudioConsent>(StudioAgentMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyRecordProviderConsent> WeakThis(this);
	Agent->RecordProviderConsent([WeakThis](const FCrowdyStudioConsent& Consent)
	{
		if (UCrowdyRecordProviderConsent* Self = WeakThis.Get())
		{
			Self->Finish(Consent);
		}
	}, bConsented);
}

void UCrowdyRecordProviderConsent::Finish(const FCrowdyStudioConsent& Consent)
{
	FCrowdyStudioConsentAsyncResult& Pin = Consent.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Consent.bConsented, Consent.ConsentedAtMs, Consent.Result, Consent.Message);
	SetReadyToDestroy();
}

UCrowdyGetModelUsage* UCrowdyGetModelUsage::GetModelUsage(UObject* WorldContextObject)
{
	UCrowdyGetModelUsage* Action = NewObject<UCrowdyGetModelUsage>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetModelUsage::Activate()
{
	UCrowdyStudioAgent* Agent = GetServiceSubsystem<UCrowdyStudioAgent>(WorldContextObject);
	if (!Agent)
	{
		Finish(StudioMissing<FCrowdyStudioModelUsage>(StudioAgentMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyGetModelUsage> WeakThis(this);
	Agent->GetModelUsage([WeakThis](const FCrowdyStudioModelUsage& Usage)
	{
		if (UCrowdyGetModelUsage* Self = WeakThis.Get())
		{
			Self->Finish(Usage);
		}
	});
}

void UCrowdyGetModelUsage::Finish(const FCrowdyStudioModelUsage& Usage)
{
	FCrowdyStudioUsageAsyncResult& Pin = Usage.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Usage, Usage.Result, Usage.Message);
	SetReadyToDestroy();
}

UCrowdyAuthorizeLiveModding* UCrowdyAuthorizeLiveModding::AuthorizeLiveModding(UObject* WorldContextObject, ECrowdyStudioRunMode Action)
{
	UCrowdyAuthorizeLiveModding* Node = NewObject<UCrowdyAuthorizeLiveModding>();
	Node->WorldContextObject = WorldContextObject;
	Node->Action = Action;
	Node->RegisterWithGameInstance(WorldContextObject);
	return Node;
}

void UCrowdyAuthorizeLiveModding::Activate()
{
	UCrowdyStudioAgent* Agent = GetServiceSubsystem<UCrowdyStudioAgent>(WorldContextObject);
	if (!Agent)
	{
		Finish(StudioMissing<FCrowdyStudioLiveModding>(StudioAgentMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyAuthorizeLiveModding> WeakThis(this);
	Agent->AuthorizeLiveModding(Action, [WeakThis](const FCrowdyStudioLiveModding& LiveModding)
	{
		if (UCrowdyAuthorizeLiveModding* Self = WeakThis.Get())
		{
			Self->Finish(LiveModding);
		}
	});
}

void UCrowdyAuthorizeLiveModding::Finish(const FCrowdyStudioLiveModding& LiveModding)
{
	FCrowdyStudioLiveModdingAsyncResult& Pin = LiveModding.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(LiveModding.bAllowed, LiveModding.Result, LiveModding.Message);
	SetReadyToDestroy();
}

UCrowdyListStudioProjects* UCrowdyListStudioProjects::ListProjects(UObject* WorldContextObject)
{
	UCrowdyListStudioProjects* Action = NewObject<UCrowdyListStudioProjects>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyListStudioProjects::Activate()
{
	UCrowdyStudioProjects* Projects = GetServiceSubsystem<UCrowdyStudioProjects>(WorldContextObject);
	if (!Projects)
	{
		Finish(StudioMissing<FCrowdyStudioProjectList>(StudioProjectsMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyListStudioProjects> WeakThis(this);
	Projects->ListProjects([WeakThis](const FCrowdyStudioProjectList& List)
	{
		if (UCrowdyListStudioProjects* Self = WeakThis.Get())
		{
			Self->Finish(List);
		}
	});
}

void UCrowdyListStudioProjects::Finish(const FCrowdyStudioProjectList& List)
{
	FCrowdyStudioProjectListAsyncResult& Pin = List.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(List.Projects, List.bMore, List.Result, List.Message);
	SetReadyToDestroy();
}

UCrowdyGetStudioProject* UCrowdyGetStudioProject::GetProject(UObject* WorldContextObject, const FString& ProjectId)
{
	UCrowdyGetStudioProject* Action = NewObject<UCrowdyGetStudioProject>();
	Action->WorldContextObject = WorldContextObject;
	Action->ProjectId = ProjectId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetStudioProject::Activate()
{
	UCrowdyStudioProjects* Projects = GetServiceSubsystem<UCrowdyStudioProjects>(WorldContextObject);
	if (!Projects)
	{
		Finish(StudioMissing<FCrowdyStudioProjectInfo>(StudioProjectsMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyGetStudioProject> WeakThis(this);
	Projects->GetProject(ProjectId, [WeakThis](const FCrowdyStudioProjectInfo& Info)
	{
		if (UCrowdyGetStudioProject* Self = WeakThis.Get())
		{
			Self->Finish(Info);
		}
	});
}

void UCrowdyGetStudioProject::Finish(const FCrowdyStudioProjectInfo& Info)
{
	FCrowdyStudioProjectAsyncResult& Pin = Info.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Info.Project, Info.Result, Info.Message);
	SetReadyToDestroy();
}

UCrowdyCreateStudioProject* UCrowdyCreateStudioProject::CreateProject(UObject* WorldContextObject, const FString& Name, ECrowdyStudioProjectKind Kind)
{
	UCrowdyCreateStudioProject* Action = NewObject<UCrowdyCreateStudioProject>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->Kind = Kind;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyCreateStudioProject::Activate()
{
	UCrowdyStudioProjects* Projects = GetServiceSubsystem<UCrowdyStudioProjects>(WorldContextObject);
	if (!Projects)
	{
		Finish(StudioMissing<FCrowdyStudioProjectInfo>(StudioProjectsMissing));
		return;
	}
	TWeakObjectPtr<UCrowdyCreateStudioProject> WeakThis(this);
	Projects->CreateProject(Name, Kind, [WeakThis](const FCrowdyStudioProjectInfo& Info)
	{
		if (UCrowdyCreateStudioProject* Self = WeakThis.Get())
		{
			Self->Finish(Info);
		}
	});
}

void UCrowdyCreateStudioProject::Finish(const FCrowdyStudioProjectInfo& Info)
{
	FCrowdyStudioProjectAsyncResult& Pin = Info.Result == ECrowdyStudioResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Info.Project, Info.Result, Info.Message);
	SetReadyToDestroy();
}
