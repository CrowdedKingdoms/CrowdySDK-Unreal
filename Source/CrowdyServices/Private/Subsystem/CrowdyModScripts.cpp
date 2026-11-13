#include "Subsystem/CrowdyModScripts.h"

#include "Engine/World.h"

bool UCrowdyModScripts::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

bool UCrowdyModScripts::IsAvailable() const
{
	return false;
}

void UCrowdyModScripts::EnterGrid(int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
}

void UCrowdyModScripts::LeaveGrid()
{
}

FCrowdyModScriptGrid UCrowdyModScripts::GetGrid() const
{
	return FCrowdyModScriptGrid();
}

void UCrowdyModScripts::SetHost(TScriptInterface<ICrowdyModScriptHost> Host)
{
	HostObject = Host.GetObject();
}

void UCrowdyModScripts::ClearHost(TScriptInterface<ICrowdyModScriptHost> Host)
{
	if (HostObject.Get() == Host.GetObject())
	{
		HostObject.Reset();
	}
}

void UCrowdyModScripts::AnswerPrompt(int32 PromptId, bool bRun)
{
}

TArray<FCrowdyModScript> UCrowdyModScripts::GetRunning() const
{
	return TArray<FCrowdyModScript>();
}

void UCrowdyModScripts::Revoke(const FString& ModId)
{
}

void UCrowdyModScripts::ForgetAuthor(int64 AuthorId)
{
}
