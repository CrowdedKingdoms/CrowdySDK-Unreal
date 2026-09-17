#include "GameModel/CrowdyContainerManifestCommandlet.h"

#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameModel/CrowdyContainerManifestScan.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogCrowdyManifestCommandlet, Log, All);

UCrowdyContainerManifestCommandlet::UCrowdyContainerManifestCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UCrowdyContainerManifestCommandlet::Main(const FString& Params)
{
	FString MapList;
	if (!FParse::Value(*Params, TEXT("map="), MapList) || MapList.IsEmpty())
	{
		UE_LOG(LogCrowdyManifestCommandlet, Error, TEXT("Usage: -run=CrowdyContainerManifest -map=/Game/Maps/L_A[+/Game/Maps/L_B]"));
		return 1;
	}
	TArray<FString> Maps;
	MapList.ParseIntoArray(Maps, TEXT("+"), true);

	int32 Failures = 0;
	for (const FString& MapPackage : Maps)
	{
		if (!FEditorFileUtils::LoadMap(MapPackage, /*bLoadAsTemplate*/ false, /*bShowProgress*/ false))
		{
			UE_LOG(LogCrowdyManifestCommandlet, Error, TEXT("%s: the map did not load."), *MapPackage);
			++Failures;
			continue;
		}
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			UE_LOG(LogCrowdyManifestCommandlet, Error, TEXT("%s: no editor world after the load."), *MapPackage);
			++Failures;
			continue;
		}

		FCrowdyContainerManifestScanResult Result;
		FCrowdyContainerManifestScan::ScanWorld(World, Result);
		for (const FCrowdyContainerManifestRow& Row : Result.Rows)
		{
			UE_LOG(LogCrowdyManifestCommandlet, Display, TEXT("  row  %s %s %s"), *Row.TypeName, *Row.BindingKey, *Row.DisplayName);
		}
		for (const FCrowdyContainerManifestSkip& Skip : Result.Skips)
		{
			UE_LOG(LogCrowdyManifestCommandlet, Display, TEXT("  skip %s: %s"), *Skip.Actor, *Skip.Reason);
		}
		bool bWritten = false;
		const UCrowdyContainerManifest* Manifest = FCrowdyContainerManifestScan::SaveManifest(World, Result, FString(), &bWritten);
		if (!Manifest)
		{
			UE_LOG(LogCrowdyManifestCommandlet, Error, TEXT("%s: the manifest could not be saved."), *MapPackage);
			++Failures;
			continue;
		}
		UE_LOG(LogCrowdyManifestCommandlet, Display, TEXT("%s: %d entity actor(s), %d row(s), %d skip(s), %s %s."),
			*MapPackage, Result.EntityActors, Result.Rows.Num(), Result.Skips.Num(), bWritten ? TEXT("saved") : TEXT("unchanged"), *Manifest->GetPathName());
	}
	return Failures == 0 ? 0 : 1;
}
