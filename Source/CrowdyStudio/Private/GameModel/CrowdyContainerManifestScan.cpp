#include "GameModel/CrowdyContainerManifestScan.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyBindingKeyProvider.h"
#include "Replication/GameModel/CrowdyEntityClassContainer.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/Script.h"
#include "WorldPartition/ActorInstanceGuids.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogCrowdyManifestScan, Log, All);

namespace
{
	// An editor-world actor's ProcessEvent drops the call unless script execution is allowed, so without the guard
	// a Blueprint or native key reads back empty and the scan would key the actor by placement instead.
	FString ManifestScanKeyOverride(const UObject* Object)
	{
		if (!Object || !Object->GetClass()->ImplementsInterface(UCrowdyBindingKeyProvider::StaticClass()))
		{
			return FString();
		}
		FEditorScriptExecutionGuard ScriptGuard;
		return ICrowdyBindingKeyProvider::Execute_GetCrowdyBindingKey(Object);
	}

	// A desc is worth loading when its native class already carries an entity component, or when it is a Blueprint,
	// whose components a desc cannot see (a native placement has no base class in its desc). Everything else, a
	// placed static mesh or a light, stays unloaded.
	bool ManifestScanDescMayBeEntity(const FWorldPartitionActorDescInstance* Desc)
	{
		const UClass* NativeClass = Desc->GetActorNativeClass();
		if (!NativeClass)
		{
			return false;
		}
		const AActor* Default = NativeClass->GetDefaultObject<AActor>();
		if (Default && Default->FindComponentByClass<UCrowdyEntityComponent>())
		{
			return true;
		}
		return Desc->GetBaseClass().IsValid();
	}
}

void FCrowdyContainerManifestScan::ScanActor(const AActor* Actor, FCrowdyContainerManifestScanResult& Out)
{
	if (!IsValid(Actor))
	{
		return;
	}
	const UCrowdyEntityComponent* Entity = Actor->FindComponentByClass<UCrowdyEntityComponent>();
	if (!Entity)
	{
		return;
	}
	++Out.EntityActors;

	const FString Source = Actor->GetPathName();
	auto Skip = [&Out, &Source](const FString& Reason)
	{
		Out.Skips.Add({Source, Reason});
	};

	if (Actor->IsEditorOnly() || Actor->HasAnyFlags(RF_Transient))
	{
		Skip(TEXT("editor-only or transient, so it is not in the cooked level"));
		return;
	}
	if (Actor->GetPackage()->IsDirty())
	{
		Skip(TEXT("has unsaved changes; save the map so its placement guid is the one a build cooks"));
		return;
	}
	if (!Actor->bNetLoadOnClient)
	{
		Skip(TEXT("Net Load On Client is off, so at runtime it is not a level-startup actor and takes no placement identity"));
		return;
	}
	if (Entity->Ownership != ECrowdyOwnership::Host)
	{
		Skip(TEXT("owned by a player, whose identity is unknown until it plays"));
		return;
	}
	if (Entity->IdentityPolicy == ECrowdyIdentityPolicy::PlayerDerived)
	{
		Skip(TEXT("player-derived identity"));
		return;
	}

	// The same precedence ResolveIdentity applies: an authored binding key names the actor; otherwise only Stable
	// identity has a shared id before play.
	const FString ActorKeyOverride = ManifestScanKeyOverride(Actor);
	if (ActorKeyOverride.IsEmpty() && Entity->IdentityPolicy != ECrowdyIdentityPolicy::Stable)
	{
		Skip(TEXT("identity is Random, so no shared id exists before play"));
		return;
	}
	const FGuid PlacementGuid = FActorInstanceGuid::GetActorInstanceGuid(*Actor);
	if (ActorKeyOverride.IsEmpty() && !PlacementGuid.IsValid())
	{
		Skip(TEXT("has no engine placement guid"));
		return;
	}
	const FGuid AnchorNetID = FCrowdyContainerManifestKeys::ActorNetID(PlacementGuid, ActorKeyOverride);
	const FString Label = Actor->GetActorNameOrLabel();

	FString TypeName;
	if (CrowdyEntityClassContainer::TryGetContainerTypeName(Actor->GetClass(), TypeName))
	{
		FCrowdyContainerManifestRow& Row = Out.Rows.AddDefaulted_GetRef();
		Row.TypeName = TypeName;
		Row.BindingKey = FCrowdyContainerManifestKeys::KeyForNetID(AnchorNetID);
		Row.DisplayName = Label;
		Row.SourceActor = Source;
	}

	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (UActorComponent* Component : Components)
	{
		FString ComponentType;
		if (!CrowdyEntityClassContainer::TryGetContainerTypeName(Component->GetClass(), ComponentType))
		{
			continue;
		}
		const FString ComponentKeyOverride = ManifestScanKeyOverride(Component);
		if (Component->CreationMethod == EComponentCreationMethod::Instance && ComponentKeyOverride.IsEmpty())
		{
			Skip(FString::Printf(TEXT("component '%s' was added to this placement, so its name differs per client; give it a binding key"),
				*Component->GetName()));
			continue;
		}
		const FString InstanceTerm = ComponentKeyOverride.IsEmpty() ? Component->GetName() : ComponentKeyOverride;
		FCrowdyContainerManifestRow& Row = Out.Rows.AddDefaulted_GetRef();
		Row.TypeName = ComponentType;
		Row.BindingKey = FCrowdyContainerManifestKeys::ComponentKey(AnchorNetID, Component->GetClass(), InstanceTerm);
		Row.DisplayName = Label + TEXT(".") + Component->GetName();
		Row.SourceActor = Source;
		Row.SourceComponent = Component->GetName();
	}
}

void FCrowdyContainerManifestScan::ScanWorld(UWorld* World, FCrowdyContainerManifestScanResult& Out)
{
	if (!World)
	{
		return;
	}

	TSet<FObjectKey> Seen;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		Seen.Add(FObjectKey(*It));
		ScanActor(*It, Out);
	}

	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		return;
	}
	FWorldPartitionHelpers::FForEachActorWithLoadingParams Params;
	Params.FilterActorDescInstance = [&Seen](const FWorldPartitionActorDescInstance* Desc)
	{
		const AActor* Loaded = Desc->GetActor();
		return !(Loaded && Seen.Contains(FObjectKey(Loaded))) && ManifestScanDescMayBeEntity(Desc);
	};
	FWorldPartitionHelpers::ForEachActorWithLoading(WorldPartition, [&Out](const FWorldPartitionActorDescInstance* Desc)
	{
		ScanActor(Desc->GetActor(), Out);
		return true;
	}, Params);
}

FString FCrowdyContainerManifestScan::ManifestPackageNameForMap(const FString& MapPackageName)
{
	return MapPackageName + TEXT("_ContainerManifest");
}

bool FCrowdyContainerManifestScan::RowsMatch(const TArray<FCrowdyContainerManifestRow>& A, const TArray<FCrowdyContainerManifestRow>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	auto Lines = [](const TArray<FCrowdyContainerManifestRow>& Rows)
	{
		TArray<FString> Out;
		Out.Reserve(Rows.Num());
		for (const FCrowdyContainerManifestRow& Row : Rows)
		{
			Out.Add(Row.TypeName + TEXT("|") + Row.BindingKey + TEXT("|") + Row.DisplayName + TEXT("|") + Row.SourceActor + TEXT("|") + Row.SourceComponent);
		}
		Out.Sort([](const FString& L, const FString& R) { return L.Compare(R, ESearchCase::CaseSensitive) < 0; });
		return Out;
	};
	const TArray<FString> LinesA = Lines(A);
	const TArray<FString> LinesB = Lines(B);
	for (int32 Index = 0; Index < LinesA.Num(); ++Index)
	{
		if (!LinesA[Index].Equals(LinesB[Index], ESearchCase::CaseSensitive))
		{
			return false;
		}
	}
	return true;
}

UCrowdyContainerManifest* FCrowdyContainerManifestScan::SaveManifest(UWorld* World,
	const FCrowdyContainerManifestScanResult& Result, const FString& PackageNameOverride, bool* bOutWritten)
{
	if (bOutWritten) { *bOutWritten = false; }
	if (!World || !World->GetPackage() || World->GetPackage()->GetName().StartsWith(TEXT("/Temp/")))
	{
		UE_LOG(LogCrowdyManifestScan, Warning, TEXT("[ManifestScan] the map is unsaved, so there is nowhere to put its manifest."));
		return nullptr;
	}
	if (World->WorldType != EWorldType::Editor)
	{
		UE_LOG(LogCrowdyManifestScan, Warning, TEXT("[ManifestScan] scan the map in the editor, not a play session: a play world's package is a copy."));
		return nullptr;
	}

	const FString MapPackageName = World->GetPackage()->GetName();
	const FString PackageName = PackageNameOverride.IsEmpty() ? ManifestPackageNameForMap(MapPackageName) : PackageNameOverride;
	const FString AssetName = FPackageName::GetShortName(PackageName);

	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	UCrowdyContainerManifest* Manifest = FindObject<UCrowdyContainerManifest>(Package, *AssetName);
	if (Manifest && Manifest->MapPackage == MapPackageName && RowsMatch(Manifest->Rows, Result.Rows))
	{
		// The level did not change since the last scan: no write, so nothing for version control to pick up.
		UE_LOG(LogCrowdyManifestScan, Log, TEXT("[ManifestScan] %s is unchanged: %d row(s), %d skip(s), %d entity actor(s)."),
			*PackageName, Result.Rows.Num(), Result.Skips.Num(), Result.EntityActors);
		return Manifest;
	}
	if (!Manifest)
	{
		Manifest = NewObject<UCrowdyContainerManifest>(Package, *AssetName, RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Manifest);
	}
	Manifest->Modify();
	Manifest->MapPackage = MapPackageName;
	Manifest->ScannedAt = FDateTime::UtcNow().ToIso8601();
	Manifest->Rows = Result.Rows;
	Package->MarkPackageDirty();

	const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Manifest, *FileName, SaveArgs))
	{
		UE_LOG(LogCrowdyManifestScan, Warning, TEXT("[ManifestScan] could not save %s."), *FileName);
		return nullptr;
	}
	if (bOutWritten) { *bOutWritten = true; }
	UE_LOG(LogCrowdyManifestScan, Log, TEXT("[ManifestScan] saved %s: %d row(s), %d skip(s), %d entity actor(s)."),
		*PackageName, Result.Rows.Num(), Result.Skips.Num(), Result.EntityActors);
	return Manifest;
}
