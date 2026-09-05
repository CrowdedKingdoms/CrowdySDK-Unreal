// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyGameSession.h"
#include "CrowdyNetLog.h"
#include "Async/TaskGraphInterfaces.h"

void UCrowdyGameSession::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("Game Session Initialized."))
}

void UCrowdyGameSession::Deinitialize()
{
	Super::Deinitialize();
}
