#include "GameModel/CrowdyContainerBlueprintExtension.h"

#include "CrowdySDKEditor.h" // LogCrowdyEditor
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "CrowdyContainerMarker"

namespace
{
	// True when Blueprint's class tree already provides a Crowdy entity component: its own construction script, an
	// inherited Blueprint parent's script, or the first native ancestor's CDO. Mirrors the compiler extension's
	// ClassHasEntityComponent, but reads Blueprint->SimpleConstructionScript directly (this runs at mark time, not
	// mid-compile, so consulting the source script is safe).
	bool BlueprintTreeHasEntityComponent(const UBlueprint* Blueprint)
	{
		if (Blueprint->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf<UCrowdyEntityComponent>())
				{
					return true;
				}
			}
		}

		for (const UClass* Current = Blueprint->ParentClass; Current; Current = Current->GetSuperClass())
		{
			if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Current))
			{
				if (!BPClass->SimpleConstructionScript)
				{
					continue;
				}
				for (const USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf<UCrowdyEntityComponent>())
					{
						return true;
					}
				}
			}
			else if (const AActor* CDO = Cast<AActor>(Current->GetDefaultObject(false)))
			{
				return CDO->FindComponentByClass<UCrowdyEntityComponent>() != nullptr;
			}
		}

		return false;
	}

	// Add a Crowdy entity component to a marked ACTOR Blueprint's construction script when its tree has none, so a
	// designer never has to drop the component by hand for auto-bind to work. No-op for a non-actor container (a
	// plain-UObject/subsystem container has no construction script and auto-registers a participant at runtime
	// instead). Caller has already opened a transaction and called Blueprint->Modify(); this Modifies the script so
	// the add is undoable with the mark.
	void EnsureActorBlueprintHasEntityComponent(UBlueprint* Blueprint)
	{
		const bool bIsActorBlueprint = Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
		if (!bIsActorBlueprint)
		{
			return;
		}
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS || BlueprintTreeHasEntityComponent(Blueprint))
		{
			return;
		}

		SCS->Modify();
		if (USCS_Node* NewNode = SCS->CreateNode(UCrowdyEntityComponent::StaticClass(), TEXT("CrowdyEntity")))
		{
			SCS->AddNode(NewNode);
			UE_LOG(LogCrowdyEditor, Log,
				TEXT("[CrowdySDK] Added a Crowdy Entity component to '%s' so its Game Model container auto-binds."),
				*Blueprint->GetName());
		}
	}
}

UCrowdyContainerBlueprintExtension* CrowdyContainerMarker::FindExtension(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	for (const TObjectPtr<UBlueprintExtension>& Extension : Blueprint->GetExtensions())
	{
		if (UCrowdyContainerBlueprintExtension* Marker = Cast<UCrowdyContainerBlueprintExtension>(Extension))
		{
			return Marker;
		}
	}

	return nullptr;
}

bool CrowdyContainerMarker::IsMarkedContainer(const UBlueprint* Blueprint)
{
	return FindExtension(Blueprint) != nullptr;
}

FString CrowdyContainerMarker::ResolveTypeName(const FString& AuthoredTypeName, const FString& AssetName)
{
	const FString Trimmed = AuthoredTypeName.TrimStartAndEnd();
	return Trimmed.IsEmpty() ? AssetName : Trimmed;
}

FString CrowdyContainerMarker::ResolveContainerTypeName(const UBlueprint* Blueprint)
{
	const UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint);
	if (!Marker)
	{
		return FString();
	}

	return ResolveTypeName(Marker->ContainerTypeName, Blueprint->GetName());
}

bool CrowdyContainerMarker::GetPullModelOnStart(const UBlueprint* Blueprint)
{
	const UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint);
	return Marker ? Marker->bPullModelOnStart : true;
}

bool CrowdyContainerMarker::ShouldStampPullOnStartOff(const UBlueprint* Blueprint)
{
	const UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint);
	return Marker != nullptr && !Marker->bPullModelOnStart;
}

ECrowdyPullOnStartStamp CrowdyContainerMarker::ResolvePullOnStartStamp(const UBlueprint* Blueprint)
{
	const UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint);
	if (!Marker)
	{
		return ECrowdyPullOnStartStamp::None;
	}

	// A marked container states its answer outright rather than relying on the tag's absence to mean "on". The
	// lookup takes the nearest answer up the class chain, so a container whose parent turned the pull off would
	// otherwise inherit that while its own checkbox showed the pull enabled.
	return Marker->bPullModelOnStart ? ECrowdyPullOnStartStamp::On : ECrowdyPullOnStartStamp::Off;
}

bool CrowdyContainerMarker::IsGameModelContainerClass(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return false;
	}
	if (IsMarkedContainer(Blueprint))
	{
		return true;
	}
#if WITH_METADATA
	const UClass* Class = Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->ParentClass.Get();
	return Class && Class->HasMetaData(CrowdyGameModelMetaKeys::Container);
#else
	// Metadata is editor-only; a cooked editor never runs the customizations that ask this, so the marker alone
	// answers (and it is already false here).
	return false;
#endif
}

void CrowdyContainerMarker::SetMarked(UBlueprint* Blueprint, bool bMarked)
{
	if (!Blueprint || Blueprint->bBeingCompiled)
	{
		return;
	}

	if (IsMarkedContainer(Blueprint) == bMarked)
	{
		return;
	}

	FScopedTransaction Transaction(bMarked
		? LOCTEXT("MarkContainer", "Mark as Game Model class")
		: LOCTEXT("UnmarkContainer", "Unmark Game Model class"));
	Blueprint->Modify();

	if (bMarked)
	{
		// RF_Transactional so a later ContainerTypeName edit is undoable: UBlueprintExtension is not transactional
		// by default, so without this flag Marker->Modify() in SetAuthoredTypeName records nothing and Undo cannot
		// revert the field.
		UCrowdyContainerBlueprintExtension* Marker =
			NewObject<UCrowdyContainerBlueprintExtension>(Blueprint, NAME_None, RF_Transactional);
		Blueprint->AddExtension(Marker);

		// An actor container needs a Crowdy entity component to get a NetID and register (auto-bind keys off the
		// registration). Add it automatically so the designer only marks the class; un-marking leaves it in place
		// (the actor may still want to be a networked entity, and removing it could break other Crowdy features).
		EnsureActorBlueprintHasEntityComponent(Blueprint);
	}
	else
	{
		// Remove every marker: normally exactly one, but a stray duplicate (e.g. from a hand-edited asset) is
		// cleared too so the class ends up genuinely unmarked.
		while (UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint))
		{
			Blueprint->RemoveExtension(Marker);
		}
	}

	// The class tag is stamped or removed by the compiler extension on the next compile; mark the Blueprint
	// structurally modified so a recompile runs, and dirty so the change persists. Mirrors the variable
	// dropdown's RecompileForMetaChange rather than forcing a synchronous compile inside this callback.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Blueprint '%s' %s as a Game Model container. Recompile to apply."),
		*Blueprint->GetName(), bMarked ? TEXT("marked") : TEXT("unmarked"));
}

void CrowdyContainerMarker::SetAuthoredTypeName(UBlueprint* Blueprint, const FString& TypeName)
{
	if (!Blueprint || Blueprint->bBeingCompiled)
	{
		return;
	}

	UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint);
	if (!Marker)
	{
		return;
	}

	// Store empty when the value is blank or matches the asset name, so a defaulted container keeps following its
	// asset name (and a duplicate re-derives cleanly); otherwise store the trimmed override.
	const FString Trimmed = TypeName.TrimStartAndEnd();
	const FString NewValue = (Trimmed.IsEmpty() || Trimmed == Blueprint->GetName()) ? FString() : Trimmed;
	if (Marker->ContainerTypeName == NewValue)
	{
		return;
	}

	FScopedTransaction Transaction(LOCTEXT("SetContainerType", "Set Game Model container type"));
	Blueprint->Modify();
	Marker->Modify();
	Marker->ContainerTypeName = NewValue;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Blueprint '%s' Game Model container type set to '%s'. Recompile to apply."),
		*Blueprint->GetName(), *ResolveTypeName(NewValue, Blueprint->GetName()));
}

void CrowdyContainerMarker::SetPullModelOnStart(UBlueprint* Blueprint, bool bPullOnStart)
{
	if (!Blueprint || Blueprint->bBeingCompiled)
	{
		return;
	}

	UCrowdyContainerBlueprintExtension* Marker = FindExtension(Blueprint);
	if (!Marker || Marker->bPullModelOnStart == bPullOnStart)
	{
		return;
	}

	FScopedTransaction Transaction(LOCTEXT("SetPullOnStart", "Set Game Model pull on start"));
	Blueprint->Modify();
	Marker->Modify();
	Marker->bPullModelOnStart = bPullOnStart;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->MarkPackageDirty();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdySDK] Blueprint '%s' Game Model pull on start %s. Recompile to apply."),
		*Blueprint->GetName(), bPullOnStart ? TEXT("enabled") : TEXT("disabled"));
}

#undef LOCTEXT_NAMESPACE
