// Copyright Epic Games, Inc. All Rights Reserved.

#include "CrowdySDK.h"

#include "CrowdySDKLog.h"
#include "CrowdyLog.h"
#include "GameplayTagsManager.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FCrowdySDKModule"

DEFINE_LOG_CATEGORY(LogCrowdySDK);

namespace
{
	CROWDY_DEFINE_TRACE_CVAR(CVarCrowdySdkTrace, TEXT("crowdy.sdk.trace"),
		TEXT("When non-zero, logs top-level CrowdySDK activity: subsystem lifecycle and ")
		TEXT("configuration / gameplay-tag path registration. Off by default."));
}

bool CrowdySDKTrace::Sdk() { return CVarCrowdySdkTrace.GetValueOnAnyThread() != 0; }

void FCrowdySDKModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin("CrowdySDK");
	if (!Plugin.IsValid())
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Failed to load CrowdySDK plugin"));
		return;
	}

	const FString TagsDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("Tags"));

	if (FPaths::DirectoryExists(TagsDir))
	{
		UGameplayTagsManager::Get().AddTagIniSearchPath(TagsDir);


#if WITH_EDITOR
	UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
#endif

		UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Registered gameplay tag ini search path: %s"), *TagsDir);
	}
	else
	{
		UE_LOG(LogCrowdySDK, Error, TEXT("Failed to load gameplay tag ini search path: %s"), *TagsDir);
	}

}

void FCrowdySDKModule::ShutdownModule()
{
	if (IsEngineExitRequested())
	{
		return;
	}

	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK")); Plugin.IsValid())
	{
		const FString TagsDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("Tags"));
		UGameplayTagsManager::Get().RemoveTagIniSearchPath(TagsDir);
		UE_CLOG(CrowdySDKTrace::Sdk(), LogCrowdySDK, Log, TEXT("Unregistered gameplay tag ini search path: %s"), *TagsDir);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCrowdySDKModule, CrowdySDK)
