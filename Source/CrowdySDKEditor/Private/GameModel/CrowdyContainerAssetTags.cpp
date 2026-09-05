// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyContainerAssetTags.h"

#include "GameModel/CrowdyContainerBlueprintExtension.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"

#include "Engine/Blueprint.h"
#include "Templates/Casts.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace
{
	FDelegateHandle GContainerAssetTagsHandle;

	// Writes the scan tags for one save. Deliberately cheap rather than memoized: the engine broadcasts this more
	// than once per save and may do so on a background thread during a concurrent save, so a process-wide cache
	// would be a data race guarding work that is two metadata reads.
	void WriteContainerScanTags(FAssetRegistryTagsContext Context)
	{
		// Only a save. The Content Browser asks every asset class's default object for tags during ordinary
		// browsing, and answering there would run this on assets nobody is writing.
		if (!Context.IsSaving())
		{
			return;
		}

		// The cook copies existing registry tags forward rather than re-deriving them, so skipping here does not
		// keep the tags out of a packaged build; the ini deny-list does that. Skipping is still right: the cook's
		// classes are the ones the editor already described, and re-deriving them buys nothing.
		if (IsRunningCookCommandlet())
		{
			return;
		}

		// The asset is the UBlueprint. A class-default object is never an asset, so SavePackage never asks one for
		// tags and anything written for it is dropped; the generated class IS a separate asset, and tagging that
		// instead would split the scan's query across two registry entries for the same Blueprint.
		const UBlueprint* Blueprint = Cast<UBlueprint>(Context.GetObject());
		if (!Blueprint)
		{
			return;
		}

		// A package saved only so a diff tool can read it is not the asset on disk, and the engine's own Blueprint
		// tags skip it for the same reason.
		const UPackage* Package = Blueprint->GetPackage();
		if (!Package || Package->HasAnyPackageFlags(PKG_ForDiffing))
		{
			return;
		}

		// The broadcast reaches this more than once for a single save with the same output. Answering again could
		// only overwrite the answer already there.
		const FName ScanTagName(CrowdyGameModelMetaKeys::ScanAssetTag);
		if (Context.ContainsTag(ScanTagName))
		{
			return;
		}

		UClass* GeneratedClass = Blueprint->GeneratedClass;

		// Reached rather than created: a save is the wrong moment to construct a class-default object, and a class
		// without one cannot answer the container question anyway. That is a refusal, not a "no".
		const bool bClassFormed =
			GeneratedClass != nullptr && GeneratedClass->GetDefaultObject(/*bCreateIfNeeded*/ false) != nullptr;

		CrowdyContainerAssetTags::FContainerTagInput Input;
		Input.bClassFormed = bClassFormed;
		Input.bClassUpToDate = Blueprint->IsUpToDate();
		Input.bMarkedContainer = CrowdyContainerMarker::IsMarkedContainer(Blueprint);
		Input.bClassCarriesContainerTag = bClassFormed
			&& FCrowdyAttributeRegistry::GetContainerTypeName(GeneratedClass, Input.ClassContainerTypeName);

		const CrowdyContainerAssetTags::FContainerTagDecision Decision = CrowdyContainerAssetTags::DecideTags(Input);
		if (!Decision.bWriteScanTag)
		{
			return;
		}

		// Hidden, so neither tag becomes a Content Browser column.
		Context.AddTag(UObject::FAssetRegistryTag(
			ScanTagName, FString(CrowdyGameModelMetaKeys::ScanAssetTagValue),
			UObject::FAssetRegistryTag::TT_Hidden));

		if (!Decision.ContainerTypeName.IsEmpty())
		{
			Context.AddTag(UObject::FAssetRegistryTag(
				FName(CrowdyGameModelMetaKeys::ContainerTypeAssetTag), Decision.ContainerTypeName,
				UObject::FAssetRegistryTag::TT_Hidden));
		}
	}
}

CrowdyContainerAssetTags::FContainerTagDecision CrowdyContainerAssetTags::DecideTags(const FContainerTagInput& Input)
{
	FContainerTagDecision Decision;

	// The scan tag is a claim that the answer below is definite, so it is only written when there is one. A class
	// that never formed, or one whose compiled state does not match the authoring marker, is neither a container
	// nor definitively not one. Publishing nothing puts the asset in the scan's load partition, where a container
	// that fails to compile keeps behaving exactly as it did before these tags existed.
	if (!Input.bClassFormed || !Input.bClassUpToDate)
	{
		return Decision;
	}
	if (Input.bMarkedContainer != Input.bClassCarriesContainerTag)
	{
		return Decision;
	}

	// A container with no type name to publish. A zero-length tag value is rejected outright by the registry's
	// fixed-size store, and writing the scan tag without the container tag would record this container as
	// definitively not one, so the only safe answer is to publish nothing and let the scan open it. Note this is
	// NOT the no-attributes case: a container whose whole contribution is functions has no attributes at all and
	// still has a type name.
	if (Input.bClassCarriesContainerTag && Input.ClassContainerTypeName.IsEmpty())
	{
		return Decision;
	}

	Decision.bWriteScanTag = true;
	if (Input.bClassCarriesContainerTag)
	{
		Decision.ContainerTypeName = Input.ClassContainerTypeName;
	}

	return Decision;
}

void CrowdyContainerAssetTags::Register()
{
	if (!GContainerAssetTagsHandle.IsValid())
	{
		GContainerAssetTagsHandle =
			UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddStatic(&WriteContainerScanTags);
	}
}

void CrowdyContainerAssetTags::Unregister()
{
	if (GContainerAssetTagsHandle.IsValid())
	{
		UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.Remove(GContainerAssetTagsHandle);
		GContainerAssetTagsHandle.Reset();
	}
}
