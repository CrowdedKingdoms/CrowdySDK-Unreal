// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyAutoRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"
#include "Subsystem/CrowdyPersistenceSubsystem.h"

void UCrowdyAutoRegistry::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Ensure singleton registries exist before we feed them
	UEventPayloadRegistry::Get();
	UActorUpdatePayloadRegistry::Get();

	ScanAndRegisterAllStructs();
	MergeDataAssetOverrides();

	// Lock registries worker threads may read from this point forward
	UEventPayloadRegistry::Get()->Seal();
	UActorUpdatePayloadRegistry::Get()->Seal();

	// The persistence subsystem does its own scan on Initialize, but we trigger
	// it via the collection dependency, so it's ready before gameplay starts.
	Collection.InitializeDependency(UCrowdyPersistenceSubsystem::StaticClass());
}

void UCrowdyAutoRegistry::Deinitialize()
{
	UEventPayloadRegistry::Shutdown();
	UActorUpdatePayloadRegistry::Shutdown();
	Super::Deinitialize();
}

TArray<UScriptStruct*> UCrowdyAutoRegistry::GetAllRegisteredGameEvents()
{
	return AllRegisteredGameEvents;
}

void UCrowdyAutoRegistry::ScanAndRegisterAllStructs()
{
	// Pass 1: C++ structs already in memory via TObjectIterator.
	// This covers all USTRUCT types from loaded modules.
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		ProcessStruct(*It);
	}

	// Pass 2: Blueprint user-defined structs that may not be loaded yet.
	// We check asset registry tags before loading only structs that
	// have the CrowdyCategory tag are loaded into memory.
	const IAssetRegistry& AR =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> StructAssets;
	AR.GetAssetsByClass(
		FTopLevelAssetPath(UUserDefinedStruct::StaticClass()),
		StructAssets);

	for (const FAssetData& Asset : StructAssets)
	{
		// skip the asset if already loaded
		if (Asset.IsAssetLoaded()) continue;

		UScriptStruct* Struct = Cast<UScriptStruct>(Asset.GetAsset());
		if (Struct) ProcessStruct(Struct);
	}
}

void UCrowdyAutoRegistry::MergeDataAssetOverrides()
{
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();

	// Event payload data asset — still respected if assigned
	if (const UEventPayloadType* Asset = Settings->EventPayloadDataAsset.LoadSynchronous())
	{
		for (const FEventPayloadTypeEntry& Entry : Asset->Entries)
		{
			if (!Entry.EventType) continue;

			// Data asset entries take priority they can override an
			// auto-registered ID if the author explicitly wants to.
			UEventPayloadRegistry::Get()->RegisterStruct(
				Entry.EventType,
				static_cast<FCrowdyTypeID>(Entry.TypeID));
		}
	}

	// Actor update payload data asset
	if (const UActorUpdatePayloadType* Asset = Settings->ActorUpdatePayloadDataAsset.LoadSynchronous())
	{
		for (const FActorUpdatePayloadTypeEntry& Entry : Asset->Entries)
		{
			if (!Entry.ActorUpdateType) continue;

			UActorUpdatePayloadRegistry::Get()->RegisterStruct(
				Entry.ActorUpdateType,
				static_cast<FCrowdyTypeID>(Entry.TypeID));
		}
	}
}

void UCrowdyAutoRegistry::ProcessStruct(UScriptStruct* Struct)
{
	if (!IsValid(Struct)) return;

	const FString Category = Struct->GetMetaData(FName("CrowdyRep"));
	if (Category.IsEmpty()) return; // not a Crowdy struct skip

	const FCrowdyTypeID TypeID = ResolveTypeID(Struct);
	if (TypeID == CROWDY_INVALID_TYPE_ID)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CrowdyAutoRegistry] Could not resolve TypeID for '%s' skipped."),
			*Struct->GetPathName());
		return;
	}

	if (Category == TEXT("Event"))
	{
		UEventPayloadRegistry::Get()->RegisterStruct(Struct, TypeID);
		UE_LOG(LogTemp, Log, TEXT("[CrowdyAutoRegistry] Registered '%s' as CrowdyRep"), *Struct->GetName());
		AllRegisteredGameEvents.AddUnique(Struct);
	}
	else if (Category == TEXT("ActorUpdate"))
	{
		UActorUpdatePayloadRegistry::Get()->RegisterStruct(Struct, TypeID);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[CrowdyAutoRegistry] Unknown CrowdyRep '%s' on '%s' skipped."),
			*Category, *Struct->GetName());
	}
}

FCrowdyTypeID UCrowdyAutoRegistry::ResolveTypeID(const UScriptStruct* Struct) const
{
	// Check developer settings for a manual override first.
	// This is only needed if the log reports a hash collision.
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	
	for (const FCrowdyIDOverride& Override : Settings->IDOverrides)
	{
		if (Override.Struct.Get() == Struct)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[CrowdyAutoRegistry] Using override ID=%d for '%s'"),
				Override.OverrideID, *Struct->GetName());
			return static_cast<FCrowdyTypeID>(Override.OverrideID);
		}
	}

	return FCrowdyTypeIDGenerator::GenerateFromStruct(Struct);
}
