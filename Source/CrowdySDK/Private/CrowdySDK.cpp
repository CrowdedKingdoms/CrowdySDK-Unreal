// Copyright Epic Games, Inc. All Rights Reserved.

#include "CrowdySDK.h"

#include "GameplayTagsManager.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FCrowdySDKModule"

void FCrowdySDKModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin("CrowdySDK");
	if (!Plugin.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load CrowdySDK plugin"));
		return;
	}
	
	const FString TagsDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("Tags"));
	
	if (FPaths::DirectoryExists(TagsDir))
	{
		UGameplayTagsManager::Get().AddTagIniSearchPath(TagsDir);
	
	
#if WITH_EDITOR
	UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
#endif
		
		UE_LOG(LogTemp, Log, TEXT("Registered gameplay tag ini search path: %s"), *TagsDir);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load gameplay tag ini search path: %s"), *TagsDir);
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
		UE_LOG(LogTemp, Log, TEXT("Unregistered gameplay tag ini search path: %s"), *TagsDir);
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCrowdySDKModule, CrowdySDK)