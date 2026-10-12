#include "Derivation/CrowdyMassMeshDerivation.h"

#include "Components/SceneComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Containers/Set.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "GameFramework/Actor.h"
#include "HAL/CriticalSection.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeLock.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"

const TCHAR* LexToString(const ECrowdyMassMeshRoute Route)
{
	switch (Route)
	{
	case ECrowdyMassMeshRoute::NativeComponent:
		return TEXT("a component declared in C++");
	case ECrowdyMassMeshRoute::BlueprintComponent:
		return TEXT("a component added in the Blueprint");
	case ECrowdyMassMeshRoute::InheritedOverride:
		return TEXT("an inherited component this Blueprint overrode");
	default:
		return TEXT("nothing");
	}
}

namespace CrowdyMassMeshPlacement
{
	// Composed up the attach chain; the actor's root contributes nothing, because the engine folds its transform into the actor's.
	FTransform NativeToRoot(const USceneComponent& Component, const USceneComponent* Root)
	{
		FTransform Placement = FTransform::Identity;
		for (const USceneComponent* Current = &Component; Current && Current != Root; Current = Current->GetAttachParent())
		{
			Placement = Placement * Current->GetRelativeTransform();
		}
		return Placement;
	}

	// The construction script node holding Template, searched from Class up through its Blueprint parents. A child Blueprint's
	// override template is a separate object carrying its original's name, so it is matched by name and class.
	USCS_Node* FindNode(const UClass* Class, const UActorComponent* Template, const USimpleConstructionScript*& OutScript)
	{
		const bool bOverride = Template->HasAnyFlags(RF_InheritableComponentTemplate);
		for (const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class); Generated;
			Generated = Cast<UBlueprintGeneratedClass>(Generated->GetSuperClass()))
		{
			const USimpleConstructionScript* Script = Generated->SimpleConstructionScript;
			if (!Script)
			{
				continue;
			}
			for (USCS_Node* Node : Script->GetAllNodes())
			{
				const UActorComponent* Original = Node ? Node->ComponentTemplate.Get() : nullptr;
				if (Original && (Original == Template
					|| (bOverride && Original->GetFName() == Template->GetFName() && Original->GetClass() == Template->GetClass())))
				{
					OutScript = Script;
					return Node;
				}
			}
		}
		return nullptr;
	}

	// The template the actor's root is built from: the native root, else the first root node of the base-most Blueprint's script.
	const USceneComponent* SceneRootTemplate(const UClass* Class, const AActor& DefaultActor)
	{
		if (const USceneComponent* NativeRoot = DefaultActor.GetRootComponent())
		{
			return NativeRoot;
		}
		const USceneComponent* Root = nullptr;
		for (const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class); Generated;
			Generated = Cast<UBlueprintGeneratedClass>(Generated->GetSuperClass()))
		{
			const USimpleConstructionScript* Script = Generated->SimpleConstructionScript;
			if (!Script)
			{
				continue;
			}
			for (const USCS_Node* Node : Script->GetRootNodes())
			{
				if (const USceneComponent* Scene = Node ? Cast<USceneComponent>(Node->ComponentTemplate) : nullptr)
				{
					Root = Scene;
					break;
				}
			}
		}
		return Root;
	}

	// Composed up the node tree to the actor's root, crossing to the native chain where a node hangs off a native component.
	// A node attached to a parent Blueprint's component stops there, placed relative to that component.
	FTransform ScriptToRoot(const UClass* Class, const AActor& DefaultActor, const USceneComponent& Template)
	{
		const USimpleConstructionScript* Script = nullptr;
		const USCS_Node* Node = FindNode(Class, &Template, Script);
		if (!Node)
		{
			return Template.GetRelativeTransform();
		}

		const USceneComponent* Root = SceneRootTemplate(Class, DefaultActor);
		FTransform Placement = FTransform::Identity;
		for (USCS_Node* Current = const_cast<USCS_Node*>(Node); Current; Current = Script->FindParentNode(Current))
		{
			// The mesh's own node takes the override's placement when there is one; the chain above it is the parent's.
			const USceneComponent* Scene = Current == Node ? &Template : Cast<USceneComponent>(Current->ComponentTemplate);
			if (!Scene || Current->ComponentTemplate == Root)
			{
				return Placement;
			}
			Placement = Placement * Scene->GetRelativeTransform();

			if (!Script->FindParentNode(Current) && Current->bIsParentComponentNative)
			{
				for (const UActorComponent* Native : DefaultActor.GetComponents())
				{
					const USceneComponent* NativeScene = Cast<USceneComponent>(Native);
					if (NativeScene && NativeScene->GetFName() == Current->ParentComponentOrVariableName)
					{
						return Placement * NativeToRoot(*NativeScene, DefaultActor.GetRootComponent());
					}
				}
			}
		}
		return Placement;
	}
}

FCrowdyMassDerivedMesh FCrowdyMassMeshDerivation::FromClass(const UClass* ActorClass)
{
	FCrowdyMassDerivedMesh Result;

	if (!ActorClass)
	{
		Result.Reason = TEXT("No class was given, so there is nothing to take a mesh from.");
		return Result;
	}

	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' is not an Actor class, so it has no components to take a mesh from."),
			*ActorClass->GetPathName());
		return Result;
	}

	// Nothing here may bring a default object into existence. A class can still be loading or compiling when
	// this runs, and constructing its default object early runs the class constructor at the wrong moment;
	// a class that has not finished forming may not have a constructor to run at all, which asserts rather
	// than failing. The walk below reads the default object and WOULD build one, so the only safe order is
	// to ask whether it already exists and refuse before reaching it.
	const UObject* DefaultObject = ActorClass->GetDefaultObject(/*bCreateIfNeeded=*/false);
	if (!DefaultObject)
	{
		Result.Reason = FString::Printf(
			TEXT("'%s' has no class default object yet, so it is still loading or is not a fully formed class. Ask again once it has finished loading."),
			*ActorClass->GetPathName());
		return Result;
	}

	bool bSawMeshComponent = false;

	// A mesh reaches an actor by three routes and none of them sees the other two: a native component, which
	// lives on the class default object; a component added in a Blueprint, which exists only as a template in
	// that Blueprint's construction script; and an inherited component whose defaults a child Blueprint
	// changed, which is kept in neither of those places.
	//
	// This one call covers all three, and for the third it asks the most derived Blueprint first, so a child
	// that overrode its parent's mesh is the answer. Walking the three by hand gets that precedence backwards:
	// the parent's construction script template is found first and the child's override never runs.
	const TSubclassOf<AActor> AsActorClass{const_cast<UClass*>(ActorClass)};
	AActor::ForEachComponentOfActorClassDefault(AsActorClass, USkinnedMeshComponent::StaticClass(),
		[&Result, &bSawMeshComponent, DefaultObject, ActorClass](const UActorComponent* Component)
		{
			const USkinnedMeshComponent* MeshComponent = Cast<USkinnedMeshComponent>(Component);
			if (!MeshComponent)
			{
				return true;
			}

			// Recorded even when the component has nothing assigned, because "you have a mesh component and
			// left it empty" and "you have no mesh component at all" need different advice.
			bSawMeshComponent = true;

			USkinnedAsset* Asset = MeshComponent->GetSkinnedAsset();
			if (!Asset)
			{
				// Keep looking: a class can carry several mesh components and only one of them be filled in.
				return true;
			}

			Result.Asset = Asset;
			Result.ComponentName = MeshComponent->GetName();

			const UObject* Owner = MeshComponent->GetOuter();
			Result.RouteOwnerName = Owner ? Owner->GetName() : FString();

			// The call above hands back components from all three routes without saying which, so they are
			// told apart by where each one lives. A native component belongs to the class default object.
			// A template does not, and the override handler is the only thing that builds one with the
			// inheritable-template flag, which is what separates an override from a plain Blueprint
			// component: both are outered to a Blueprint class and are otherwise indistinguishable.
			const AActor& DefaultActor = *CastChecked<const AActor>(DefaultObject);
			if (Owner == DefaultObject)
			{
				Result.Route = ECrowdyMassMeshRoute::NativeComponent;
				Result.Transform = CrowdyMassMeshPlacement::NativeToRoot(*MeshComponent, DefaultActor.GetRootComponent());
				return false;
			}

			Result.Route = MeshComponent->HasAnyFlags(RF_InheritableComponentTemplate)
				? ECrowdyMassMeshRoute::InheritedOverride
				: ECrowdyMassMeshRoute::BlueprintComponent;
			Result.Transform = CrowdyMassMeshPlacement::ScriptToRoot(ActorClass, DefaultActor, *MeshComponent);

			return false;
		});

	if (Result.Asset)
	{
		return Result;
	}

	Result.Reason = bSawMeshComponent
		? FString::Printf(
			TEXT("'%s' has a skinned mesh component, but no mesh is assigned to it. Assign one on the component's defaults."),
			*ActorClass->GetPathName())
		: FString::Printf(
			TEXT("'%s' has no skinned mesh component. Add a Skeletal Mesh Component to the class."),
			*ActorClass->GetPathName());

	return Result;
}

namespace CrowdyMassMeshDerivationPrivate
{
	const TCHAR* const InstancedSkinnedUsageName = TEXT("Used with Instanced Skinned Meshes");

	// The same three domains the engine itself gates usage flags on. A material outside them is never
	// checked against a usage flag at all, so reporting one as unflagged would be a sentence about
	// something that has no bearing on what the crowd looks like.
	bool UsageFlagsApplyTo(const UMaterial& BaseMaterial)
	{
		const EMaterialDomain Domain = BaseMaterial.MaterialDomain.GetValue();
		return Domain == MD_Surface || Domain == MD_DeferredDecal || Domain == MD_Volume;
	}

	// Every material already reported, keyed by the material the flag is ticked on. Both callers are on
	// the game thread today; the lock is what keeps that from being something a third caller has to know.
	FCriticalSection& ReportedMaterialsMutex()
	{
		static FCriticalSection Mutex;
		return Mutex;
	}

	TSet<FObjectKey>& ReportedMaterials()
	{
		static TSet<FObjectKey> Reported;
		return Reported;
	}
}

FString FCrowdyMassInstancedSkinnedMaterials::Describe() const
{
	using namespace CrowdyMassMeshDerivationPrivate;

	if (SlotCount == 0)
	{
		return TEXT("the mesh declares no material slots");
	}

	if (SlotsChecked == 0)
	{
		return FString::Printf(
			TEXT("none of its %d material slots holds a material the instanced skinned usage flag applies to, so nothing there could be checked"),
			SlotCount);
	}

	if (IsClear())
	{
		return FString::Printf(TEXT("all %d checked material slots are flagged for instanced skinned meshes"), SlotsChecked);
	}

	FString Slots;
	for (const FCrowdyMassUnflaggedMaterialSlot& Slot : Unflagged)
	{
		Slots += Slots.IsEmpty() ? TEXT("") : TEXT("; ");
		Slots += FString::Printf(TEXT("slot %d '%s' uses '%s', flag it on '%s'"),
			Slot.SlotIndex, *Slot.SlotName.ToString(), *Slot.MaterialPath, *Slot.FlagOwnerPath);
	}

	return FString::Printf(
		TEXT("%d of %d checked material slots are NOT flagged for instanced skinned meshes, so a crowd draws them as the default material: %s. Tick '%s' on each material named after 'flag it on' and resave it"),
		Unflagged.Num(), SlotsChecked, *Slots, InstancedSkinnedUsageName);
}

FCrowdyMassInstancedSkinnedMaterials FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(const USkinnedAsset* Asset)
{
	using namespace CrowdyMassMeshDerivationPrivate;

	FCrowdyMassInstancedSkinnedMaterials Result;

	if (!Asset)
	{
		return Result;
	}

	const TArray<FSkeletalMaterial>& Slots = Asset->GetMaterials();
	Result.SlotCount = Slots.Num();

	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
	{
		const UMaterialInterface* SlotMaterial = Slots[SlotIndex].MaterialInterface;
		if (!SlotMaterial)
		{
			continue;
		}

		// The flag is normally ticked on the material at the root of the chain and inherited, but an
		// instance can override it, so what the engine gates on is the assigned material's own answer and
		// that is what is read. The root is carried alongside because it is where a person goes to fix it.
		const UMaterial* BaseMaterial = SlotMaterial->GetMaterial();
		if (BaseMaterial && !UsageFlagsApplyTo(*BaseMaterial))
		{
			continue;
		}

		++Result.SlotsChecked;

		if (SlotMaterial->GetUsageByFlag(MATUSAGE_InstancedSkinnedMesh))
		{
			continue;
		}

		// A parentless instance has no root to name, so it names itself: an entry with nothing in this
		// field would fall through a caller's once-per-material record and take a real material's place
		// in it.
		const UObject* FlagOwner = BaseMaterial ? static_cast<const UObject*>(BaseMaterial) : static_cast<const UObject*>(SlotMaterial);

		FCrowdyMassUnflaggedMaterialSlot& Entry = Result.Unflagged.AddDefaulted_GetRef();
		Entry.SlotIndex = SlotIndex;
		Entry.SlotName = Slots[SlotIndex].MaterialSlotName;
		Entry.MaterialPath = SlotMaterial->GetPathName();
		Entry.FlagOwnerPath = FlagOwner->GetPathName();
		Entry.FlagOwner = FObjectKey(FlagOwner);
	}

	return Result;
}

void FCrowdyMassMaterialUsageReports::TakeUnreported(const FCrowdyMassInstancedSkinnedMaterials& Verdict,
	const USkinnedAsset* Asset, const UClass* Context, TArray<FString>& OutMessages)
{
	using namespace CrowdyMassMeshDerivationPrivate;

	FScopeLock Lock(&ReportedMaterialsMutex());
	TSet<FObjectKey>& Reported = ReportedMaterials();

	for (const FCrowdyMassUnflaggedMaterialSlot& Slot : Verdict.Unflagged)
	{
		bool bAlready = false;
		Reported.Add(Slot.FlagOwner, &bAlready);
		if (bAlready)
		{
			continue;
		}

		FString Where;
		if (Asset)
		{
			Where = FString::Printf(TEXT(" Run into on slot %d '%s' ('%s') of mesh '%s'."),
				Slot.SlotIndex, *Slot.SlotName.ToString(), *Slot.MaterialPath, *Asset->GetPathName());
		}

		if (Context)
		{
			Where += FString::Printf(TEXT(" First asked about while working out how to draw '%s'."), *Context->GetPathName());
		}

		OutMessages.Add(FString::Printf(
			TEXT("material '%s' is not flagged for instanced skinned meshes, so a crowd drawn through it renders as the default material rather than as what it should look like. Tick '%s' under Usage on it and RESAVE it: a flag the editor sets on the fly and nobody saves comes back missing on the next launch.%s Said once per material, so other meshes and classes sharing it stay quiet; crowdy.mass.derive answers in full for any class at any time."),
			*Slot.FlagOwnerPath, InstancedSkinnedUsageName, *Where));
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void FCrowdyMassMaterialUsageReports::ResetForTest()
{
	using namespace CrowdyMassMeshDerivationPrivate;

	FScopeLock Lock(&ReportedMaterialsMutex());
	ReportedMaterials().Empty();
}
#endif
