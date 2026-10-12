#include "Subsystems/CrowdyMassEntitySubsystem.h"

#include "Animation/AnimSequenceTransformProviderData.h"
#include "Animation/TransformProviderData.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Backend/CrowdyMassRenderingBackend.h"
#include "Components/CrowdyMassAnimSetComponent.h"
#include "Components/CrowdyMassEntityTraitsComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "CoreGlobals.h"
#include "CrowdyMassLog.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyMapProfile.h"
#include "Derivation/CrowdyMassAnimSetDerivation.h"
#include "Derivation/CrowdyMassCollisionDerivation.h"
#include "Derivation/CrowdyMassEntityTraitsDerivation.h"
#include "Derivation/CrowdyMassMeshDerivation.h"
#include "Derivation/CrowdyMassNearTierAuthoredConfig.h"
#include "Derivation/CrowdyMassNearTierDerivation.h"
#include "Dispatch/CrowdyMassDispatchProxy.h"
#include "Engine/Engine.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Fragments/CrowdyMassFragments.h"
#include "HAL/IConsoleManager.h"
#include "Interpolation/CrowdyInterpolationTiming.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "Mass/CrowdyMassEntity.h"
#include "Mass/EntityFragments.h"
#include "MassCommandBuffer.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntityTraitBase.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassEntityTemplateRegistry.h"
#include "MassMovableVisualizationTrait.h"
#include "MassMovementFragments.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "MassSpawnerSubsystem.h"
#include "MassVisualizationTrait.h"
#include "Reflection/CrowdyMassByteProperty.h"
#include "Reflection/CrowdyMassStateFieldTypeTable.h"
#include "Representation/CrowdyMassRepresentationActorManagement.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Target/CrowdyMassFieldAccess.h"
#include "Templates/UnrealTemplate.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "Traits/CrowdyMassAnimationTrait.h"
#include "Traits/CrowdyMassRemoteEntityTrait.h"
#include "Traits/CrowdyMassTraceCollisionTrait.h"
#include "Traits/CrowdyMassViewPoseTrait.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Settings/CrowdyMassDeveloperSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "View/CrowdyMassViewState.h"

// How smooth remote motion looks depends on the connection it arrives over, and the only way to know
// what suits a given route is to watch it on that route. These mirror the project settings so they can
// be moved while a session is running; a negative value means the setting still decides.
namespace CrowdyMassInterpolationTuning
{
#if !UE_BUILD_SHIPPING
	int32 GAdaptive = -1;
	FAutoConsoleVariableRef CVarAdaptive(
		TEXT("crowdy.mass.interp.adaptive"),
		GAdaptive,
		TEXT("Replay each remote segment over the observed arrival interval. 1 on, 0 off (fixed duration), -1 to follow the project setting."),
		ECVF_Default);

	float GOvershoot = -1.0f;
	FAutoConsoleVariableRef CVarOvershoot(
		TEXT("crowdy.mass.interp.overshoot"),
		GOvershoot,
		TEXT("How much longer than the observed arrival interval a remote segment is replayed over. Negative to follow the project setting."),
		ECVF_Default);

	float GMinDuration = -1.0f;
	FAutoConsoleVariableRef CVarMinDuration(
		TEXT("crowdy.mass.interp.min"),
		GMinDuration,
		TEXT("Shortest span a remote segment may be replayed over, in seconds. Negative to follow the project setting."),
		ECVF_Default);

	float GMaxDuration = -1.0f;
	FAutoConsoleVariableRef CVarMaxDuration(
		TEXT("crowdy.mass.interp.max"),
		GMaxDuration,
		TEXT("Longest span a remote segment may be replayed over, in seconds. Negative to follow the project setting."),
		ECVF_Default);
#endif

	void ApplyRuntimeOverrides(FCrowdyInterpolationTuning& Tuning)
	{
#if !UE_BUILD_SHIPPING
		if (GAdaptive >= 0)
			Tuning.bAdaptive = GAdaptive != 0;

		if (GOvershoot >= 0.0f)
			Tuning.Overshoot = GOvershoot;

		if (GMinDuration >= 0.0f)
			Tuning.MinDuration = GMinDuration;

		if (GMaxDuration >= 0.0f)
			Tuning.MaxDuration = GMaxDuration;
#endif
	}
}

// Building a Mass entity config out of an actor class is the last thing a spawn tries, so on any project
// that configures its crowd it is never reached and cannot be watched. This turns it into the FIRST thing
// tried, without editing a config, a level or an asset anyone else is working on.
namespace CrowdyMassDerivation
{
#if !UE_BUILD_SHIPPING
	int32 GForceDerive = 0;
	FAutoConsoleVariableRef CVarForceDerive(
		TEXT("crowdy.mass.derive.force"),
		GForceDerive,
		TEXT("Spawn every remote entity from a Mass entity config built out of its own class, pushing aside the per-class binding, the backend override and the project-wide Remote Entity Config. 1 on, 0 off. The class's animation set is pushed aside with it, so the entity animates from the set and the Bank the class itself declares, and an entity that appears in bind pose under this is showing what an unconfigured class carries. Entities already spawned keep the config they were built from; every class's animation answer is worked out again."),
		// Changing this moves which animation set a class renders through, and a pose or action id is a row
		// position in one particular set. Without dropping the resolved answers, a class resolved before the
		// change would keep handing out ids into the set it no longer renders, and those ids would index
		// whatever rows happen to occupy the same positions in the set it does.
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
		{
			UCrowdyMassEntitySubsystem::InvalidateAnimSetCachesInEveryWorld();
		}),
		ECVF_Cheat);
#endif

	bool ShouldForceDerive()
	{
#if !UE_BUILD_SHIPPING
		return GForceDerive != 0;
#else
		return false;
#endif
	}
}

namespace
{
bool TryGetAnimationMinScreenSize(const UInstancedSkinnedMeshComponent* Component, float& OutAnimationMinScreenSize)
{
	if (!Component)
	{
		return false;
	}

	const FFloatProperty* AnimationMinScreenSizeProperty = FindFProperty<FFloatProperty>(
		UInstancedSkinnedMeshComponent::StaticClass(),
		TEXT("AnimationMinScreenSize"));
	if (!AnimationMinScreenSizeProperty)
	{
		return false;
	}

	OutAnimationMinScreenSize = AnimationMinScreenSizeProperty->GetPropertyValue_InContainer(Component);
	return true;
}

void LogSkinnedMeshShadowDiagnostics(UWorld* World)
{
	if (!World || World->bIsTearingDown)
	{
		return;
	}

	const IConsoleVariable* ForceRefPoseCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.InstancedSkinnedMeshes.ForceRefPose"));
	const IConsoleVariable* AnimationBoundsCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.InstancedSkinnedMeshes.AnimationBounds"));
	const IConsoleVariable* SkinningProvidersCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.TransformProviders"));
	const IConsoleVariable* SkinningAsyncCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.Buffers.AsyncUpdate"));
	const IConsoleVariable* SkinningForceFullUploadCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.Buffers.ForceFullUpload"));
	const IConsoleVariable* DefaultAnimationMinScreenSizeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.DefaultAnimationMinScreenSize"));
	const IConsoleVariable* VirtualShadowCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Enable"));
	const IConsoleVariable* VirtualShadowCacheCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Cache"));
	const IConsoleVariable* VirtualShadowDeformableInvalidateCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.Virtual.Cache.DeformableMeshesInvalidate"));

	UE_LOG(LogCrowdyMass, Warning, TEXT("==================== CROWDY MASS SHADOW DIAGNOSTICS BEGIN ===================="));
	UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassShadowDiag] World='%s' ForceRefPose=%d AnimationBounds=%d SkinningTransformProviders=%d SkinningAsyncUpdate=%d SkinningForceFullUpload=%d DefaultAnimationMinScreenSize=%.3f VirtualShadow=%d VirtualShadowCache=%d VirtualShadowDeformableInvalidate=%d"),
		*GetNameSafe(World),
		ForceRefPoseCVar ? ForceRefPoseCVar->GetInt() : INDEX_NONE,
		AnimationBoundsCVar ? AnimationBoundsCVar->GetInt() : INDEX_NONE,
		SkinningProvidersCVar ? SkinningProvidersCVar->GetInt() : INDEX_NONE,
		SkinningAsyncCVar ? SkinningAsyncCVar->GetInt() : INDEX_NONE,
		SkinningForceFullUploadCVar ? SkinningForceFullUploadCVar->GetInt() : INDEX_NONE,
		DefaultAnimationMinScreenSizeCVar ? DefaultAnimationMinScreenSizeCVar->GetFloat() : -1.0f,
		VirtualShadowCVar ? VirtualShadowCVar->GetInt() : INDEX_NONE,
		VirtualShadowCacheCVar ? VirtualShadowCacheCVar->GetInt() : INDEX_NONE,
		VirtualShadowDeformableInvalidateCVar ? VirtualShadowDeformableInvalidateCVar->GetInt() : INDEX_NONE);

	int32 ComponentCount = 0;
	for (TObjectIterator<UInstancedSkinnedMeshComponent> ComponentIt; ComponentIt; ++ComponentIt)
	{
		const UInstancedSkinnedMeshComponent* Component = *ComponentIt;
		if (!IsValid(Component) || Component->GetWorld() != World)
		{
			continue;
		}

		++ComponentCount;
		const UTransformProviderData* TransformProvider = Component->GetTransformProvider();
		const USkinnedAsset* SkinnedAsset = Component->GetSkinnedAsset();
		float AnimationMinScreenSize = 0.0f;
		const bool bHasAnimationMinScreenSize = TryGetAnimationMinScreenSize(Component, AnimationMinScreenSize);

		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassShadowDiag] ISKMC[%d] Component='%s' Owner='%s' Instances=%d Asset='%s' Provider='%s' ProviderClass='%s' ProviderEnabled=%d ProviderCompiling=%d UniqueAnimations=%u AnimationMinScreenSize=%s CastShadow=%d DynamicShadow=%d HiddenShadow=%d CapsuleDirectShadow=%d CapsuleIndirectShadow=%d RenderStatic=%d UseScreenRenderStateForUpdate=%d UpdateRateOptimizations=%d VisibilityBasedAnimTickOption=%d Mobility=%d"),
			ComponentCount,
			*GetPathNameSafe(Component),
			*GetNameSafe(Component->GetOwner()),
			Component->GetInstanceCount(),
			*GetPathNameSafe(SkinnedAsset),
			*GetPathNameSafe(TransformProvider),
			*GetNameSafe(TransformProvider ? TransformProvider->GetClass() : nullptr),
			TransformProvider && TransformProvider->IsEnabled() ? 1 : 0,
			TransformProvider && TransformProvider->IsCompiling() ? 1 : 0,
			TransformProvider ? TransformProvider->GetUniqueAnimationCount() : 0u,
			bHasAnimationMinScreenSize ? *FString::Printf(TEXT("%.3f"), AnimationMinScreenSize) : TEXT("Unavailable"),
			Component->CastShadow ? 1 : 0,
			Component->bCastDynamicShadow ? 1 : 0,
			Component->bCastHiddenShadow ? 1 : 0,
			Component->bCastCapsuleDirectShadow ? 1 : 0,
			Component->bCastCapsuleIndirectShadow ? 1 : 0,
			Component->bRenderStatic ? 1 : 0,
			Component->bUseScreenRenderStateForUpdate ? 1 : 0,
			Component->bEnableUpdateRateOptimizations ? 1 : 0,
			static_cast<int32>(Component->VisibilityBasedAnimTickOption),
			static_cast<int32>(Component->Mobility));
	}

	if (ComponentCount == 0)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassShadowDiag] No UInstancedSkinnedMeshComponent instances found in world '%s'."), *GetNameSafe(World));
	}

	UE_LOG(LogCrowdyMass, Warning, TEXT("===================== CROWDY MASS SHADOW DIAGNOSTICS END ====================="));
}

FAutoConsoleCommandWithWorld CmdCrowdyMassLogSkinnedMeshShadowDiagnostics(
	TEXT("CrowdyMass.LogSkinnedMeshShadowDiagnostics"),
	TEXT("Logs shadow/render flags for Crowdy Mass instanced skinned mesh components in the current world."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&LogSkinnedMeshShadowDiagnostics));

void RecreateSkinnedMeshRenderState(UWorld* World)
{
	if (!World || World->bIsTearingDown)
	{
		return;
	}

	int32 ComponentCount = 0;
	int32 ReadyComponentCount = 0;
	int32 RecreatedComponentCount = 0;

	UE_LOG(LogCrowdyMass, Warning, TEXT("==================== CROWDY MASS ISKMC RENDER STATE RECREATE BEGIN ===================="));

	for (TObjectIterator<UInstancedSkinnedMeshComponent> ComponentIt; ComponentIt; ++ComponentIt)
	{
		UInstancedSkinnedMeshComponent* Component = *ComponentIt;
		if (!IsValid(Component) || Component->GetWorld() != World)
		{
			continue;
		}

		++ComponentCount;

		UTransformProviderData* TransformProvider = Component->GetTransformProvider();
		const bool bProviderReady =
			TransformProvider &&
			TransformProvider->IsEnabled() &&
			!TransformProvider->IsCompiling() &&
			TransformProvider->GetUniqueAnimationCount() > 0 &&
			Component->GetInstanceCount() > 0;

		if (bProviderReady)
		{
			++ReadyComponentCount;
		}

		Component->MarkRenderStateDirty();
		++RecreatedComponentCount;

		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassISKMCRecreate] Component='%s' Owner='%s' Instances=%d Provider='%s' ProviderClass='%s' ProviderEnabled=%d ProviderCompiling=%d UniqueAnimations=%u Ready=%d"),
			*GetPathNameSafe(Component),
			*GetNameSafe(Component->GetOwner()),
			Component->GetInstanceCount(),
			*GetPathNameSafe(TransformProvider),
			*GetNameSafe(TransformProvider ? TransformProvider->GetClass() : nullptr),
			TransformProvider && TransformProvider->IsEnabled() ? 1 : 0,
			TransformProvider && TransformProvider->IsCompiling() ? 1 : 0,
			TransformProvider ? TransformProvider->GetUniqueAnimationCount() : 0u,
			bProviderReady ? 1 : 0);
	}

	UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassISKMCRecreate] World='%s' Components=%d ReadyComponents=%d RecreatedComponents=%d"),
		*GetNameSafe(World),
		ComponentCount,
		ReadyComponentCount,
		RecreatedComponentCount);

	UE_LOG(LogCrowdyMass, Warning, TEXT("===================== CROWDY MASS ISKMC RENDER STATE RECREATE END ====================="));
}

FAutoConsoleCommandWithWorld CmdCrowdyMassRecreateSkinnedMeshRenderState(
	TEXT("CrowdyMass.RecreateSkinnedMeshRenderState"),
	TEXT("Calls MarkRenderStateDirty on instanced skinned mesh components in the current world."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&RecreateSkinnedMeshRenderState));

void ForceSkinnedMeshAlwaysAnimate(UWorld* World)
{
	if (!World || World->bIsTearingDown)
	{
		return;
	}

	const IConsoleVariable* ForceRefPoseCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.InstancedSkinnedMeshes.ForceRefPose"));
	const IConsoleVariable* AnimationBoundsCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.InstancedSkinnedMeshes.AnimationBounds"));
	const IConsoleVariable* SkinningProvidersCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.TransformProviders"));
	const IConsoleVariable* SkinningForceFullUploadCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.Buffers.ForceFullUpload"));
	const IConsoleVariable* DefaultAnimationMinScreenSizeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Skinning.DefaultAnimationMinScreenSize"));

	int32 ComponentCount = 0;

	UE_LOG(LogCrowdyMass, Warning, TEXT("==================== CROWDY MASS FORCE ISKMC ALWAYS ANIMATE BEGIN ===================="));
	UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassISKMCAlwaysAnimate] World='%s' ForceRefPose=%d AnimationBounds=%d SkinningTransformProviders=%d SkinningForceFullUpload=%d DefaultAnimationMinScreenSize=%.3f"),
		*GetNameSafe(World),
		ForceRefPoseCVar ? ForceRefPoseCVar->GetInt() : INDEX_NONE,
		AnimationBoundsCVar ? AnimationBoundsCVar->GetInt() : INDEX_NONE,
		SkinningProvidersCVar ? SkinningProvidersCVar->GetInt() : INDEX_NONE,
		SkinningForceFullUploadCVar ? SkinningForceFullUploadCVar->GetInt() : INDEX_NONE,
		DefaultAnimationMinScreenSizeCVar ? DefaultAnimationMinScreenSizeCVar->GetFloat() : -1.0f);

	for (TObjectIterator<UInstancedSkinnedMeshComponent> ComponentIt; ComponentIt; ++ComponentIt)
	{
		UInstancedSkinnedMeshComponent* Component = *ComponentIt;
		if (!IsValid(Component) || Component->GetWorld() != World)
		{
			continue;
		}

		++ComponentCount;

		float PreviousAnimationMinScreenSize = 0.0f;
		const bool bHadPreviousAnimationMinScreenSize = TryGetAnimationMinScreenSize(Component, PreviousAnimationMinScreenSize);

		UTransformProviderData* TransformProvider = Component->GetTransformProvider();
		Component->SetAnimationMinScreenSize(-1.0f);
		Component->MarkRenderStateDirty();

		float CurrentAnimationMinScreenSize = 0.0f;
		const bool bHasCurrentAnimationMinScreenSize = TryGetAnimationMinScreenSize(Component, CurrentAnimationMinScreenSize);

		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassISKMCAlwaysAnimate] Component='%s' Owner='%s' Instances=%d Asset='%s' Provider='%s' ProviderClass='%s' ProviderEnabled=%d ProviderCompiling=%d UniqueAnimations=%u PreviousAnimationMinScreenSize=%s CurrentAnimationMinScreenSize=%s"),
			*GetPathNameSafe(Component),
			*GetNameSafe(Component->GetOwner()),
			Component->GetInstanceCount(),
			*GetPathNameSafe(Component->GetSkinnedAsset()),
			*GetPathNameSafe(TransformProvider),
			*GetNameSafe(TransformProvider ? TransformProvider->GetClass() : nullptr),
			TransformProvider && TransformProvider->IsEnabled() ? 1 : 0,
			TransformProvider && TransformProvider->IsCompiling() ? 1 : 0,
			TransformProvider ? TransformProvider->GetUniqueAnimationCount() : 0u,
			bHadPreviousAnimationMinScreenSize ? *FString::Printf(TEXT("%.3f"), PreviousAnimationMinScreenSize) : TEXT("Unavailable"),
			bHasCurrentAnimationMinScreenSize ? *FString::Printf(TEXT("%.3f"), CurrentAnimationMinScreenSize) : TEXT("Unavailable"));
	}

	UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassISKMCAlwaysAnimate] World='%s' Components=%d"),
		*GetNameSafe(World),
		ComponentCount);
	UE_LOG(LogCrowdyMass, Warning, TEXT("===================== CROWDY MASS FORCE ISKMC ALWAYS ANIMATE END ====================="));
}

FAutoConsoleCommandWithWorld CmdCrowdyMassForceSkinnedMeshAlwaysAnimate(
	TEXT("CrowdyMass.ForceSkinnedMeshAlwaysAnimate"),
	TEXT("Sets instanced skinned mesh components in the current world to component-level always animate and recreates their render state."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&ForceSkinnedMeshAlwaysAnimate));

const TCHAR* GetMassRepresentationTypeName(const EMassRepresentationType RepresentationType)
{
	switch (RepresentationType)
	{
	case EMassRepresentationType::HighResSpawnedActor:
		return TEXT("HighResSpawnedActor");
	case EMassRepresentationType::LowResSpawnedActor:
		return TEXT("LowResSpawnedActor");
	case EMassRepresentationType::SkinnedMeshInstance:
		return TEXT("SkinnedMeshInstance");
	case EMassRepresentationType::StaticMeshInstance:
		return TEXT("StaticMeshInstance");
	case EMassRepresentationType::None:
		return TEXT("None");
	default:
		return TEXT("Unknown");
	}
}

void LogRepresentationAnimationDiagnostics(UWorld* World)
{
	if (!World || World->bIsTearingDown)
	{
		return;
	}

	UCrowdyMassEntitySubsystem* CrowdySubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	UMassEntitySubsystem* EntitySubsystem = World->GetSubsystem<UMassEntitySubsystem>();
	if (!CrowdySubsystem || !EntitySubsystem)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] Missing subsystem. World='%s' CrowdySubsystem=%d EntitySubsystem=%d"),
			*GetNameSafe(World),
			CrowdySubsystem ? 1 : 0,
			EntitySubsystem ? 1 : 0);
		return;
	}

	TArray<FMassEntityHandle> Entities;
	CrowdySubsystem->GetTrackedRemoteEntities(Entities);

	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();
	int32 ValidEntities = 0;
	int32 EntitiesWithAnimation = 0;
	int32 EntitiesWithRepresentation = 0;
	int32 EntitiesWithLOD = 0;
	int32 EntitiesWithVelocity = 0;
	int32 SkinnedRepresentationCount = 0;
	int32 ActorRepresentationCount = 0;
	int32 NoneRepresentationCount = 0;
	int32 InvalidSkinnedDescCount = 0;
	TMap<int32, int32> SequenceCounts;
	TMap<int32, int32> RepresentationCounts;
	TMap<int32, int32> LODCounts;
	TMap<int32, int32> DescHandleCounts;

	UE_LOG(LogCrowdyMass, Warning, TEXT("==================== CROWDY MASS REPRESENTATION ANIMATION DIAGNOSTICS BEGIN ===================="));

	for (const FMassEntityHandle Entity : Entities)
	{
		if (!EntityManager.IsEntityValid(Entity))
		{
			continue;
		}

		++ValidEntities;

		const FMassRepresentationAnimationFragment* AnimationFragment = EntityManager.GetFragmentDataPtr<FMassRepresentationAnimationFragment>(Entity);
		const FMassRepresentationFragment* RepresentationFragment = EntityManager.GetFragmentDataPtr<FMassRepresentationFragment>(Entity);
		const FMassRepresentationLODFragment* LODFragment = EntityManager.GetFragmentDataPtr<FMassRepresentationLODFragment>(Entity);
		const FMassVelocityFragment* VelocityFragment = EntityManager.GetFragmentDataPtr<FMassVelocityFragment>(Entity);
		const FTransformFragment* TransformFragment = EntityManager.GetFragmentDataPtr<FTransformFragment>(Entity);

		if (AnimationFragment)
		{
			++EntitiesWithAnimation;
			SequenceCounts.FindOrAdd(AnimationFragment->AnimData.SequenceIndex)++;
		}

		int32 DescHandleIndex = INDEX_NONE;
		if (RepresentationFragment)
		{
			++EntitiesWithRepresentation;
			const int32 RepresentationType = static_cast<int32>(RepresentationFragment->CurrentRepresentation);
			RepresentationCounts.FindOrAdd(RepresentationType)++;

			DescHandleIndex = RepresentationFragment->SkinnedMeshDescHandle.IsValid()
				? RepresentationFragment->SkinnedMeshDescHandle.ToIndex()
				: INDEX_NONE;
			DescHandleCounts.FindOrAdd(DescHandleIndex)++;

			if (RepresentationFragment->CurrentRepresentation == EMassRepresentationType::SkinnedMeshInstance)
			{
				++SkinnedRepresentationCount;
				if (!RepresentationFragment->SkinnedMeshDescHandle.IsValid())
				{
					++InvalidSkinnedDescCount;
				}
			}
			else if (UE::Mass::Representation::IsValidActorRepresentation(RepresentationFragment->CurrentRepresentation))
			{
				++ActorRepresentationCount;
			}
			else if (RepresentationFragment->CurrentRepresentation == EMassRepresentationType::None)
			{
				++NoneRepresentationCount;
			}
		}

		if (LODFragment)
		{
			++EntitiesWithLOD;
			LODCounts.FindOrAdd(static_cast<int32>(LODFragment->LOD.GetValue()))++;
		}

		if (VelocityFragment)
		{
			++EntitiesWithVelocity;
		}

		if (ValidEntities <= 10)
		{
			FGuid UUID;
			CrowdySubsystem->FindUUIDByEntity(Entity, UUID);
			const FString LocationString = TransformFragment
				? TransformFragment->GetTransform().GetLocation().ToCompactString()
				: FString(TEXT("Missing"));

			UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] Sample[%d] UUID='%s' Rep='%s' PrevRep='%s' LOD=%d PrevLOD=%d Visibility=%d LODSignificance=%.3f DescHandle=%d Seq=%d Pos=%.3f PlayRate=%.3f Velocity=%.2f Location='%s'"),
				ValidEntities,
				*UUID.ToString(),
				RepresentationFragment ? GetMassRepresentationTypeName(RepresentationFragment->CurrentRepresentation) : TEXT("Missing"),
				RepresentationFragment ? GetMassRepresentationTypeName(RepresentationFragment->PrevRepresentation) : TEXT("Missing"),
				LODFragment ? static_cast<int32>(LODFragment->LOD.GetValue()) : INDEX_NONE,
				LODFragment ? static_cast<int32>(LODFragment->PrevLOD.GetValue()) : INDEX_NONE,
				LODFragment ? static_cast<int32>(LODFragment->Visibility) : INDEX_NONE,
				LODFragment ? LODFragment->LODSignificance : -1.0f,
				DescHandleIndex,
				AnimationFragment ? AnimationFragment->AnimData.SequenceIndex : INDEX_NONE,
				AnimationFragment ? AnimationFragment->AnimData.Position : -1.0f,
				AnimationFragment ? AnimationFragment->AnimData.PlayRate : -1.0f,
				VelocityFragment ? VelocityFragment->Value.Size() : -1.0,
				*LocationString);
		}
	}

	UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] World='%s' TrackedEntities=%d ValidEntities=%d AnimationFragments=%d RepresentationFragments=%d LODFragments=%d VelocityFragments=%d Skinned=%d Actor=%d None=%d InvalidSkinnedDesc=%d"),
		*GetNameSafe(World),
		Entities.Num(),
		ValidEntities,
		EntitiesWithAnimation,
		EntitiesWithRepresentation,
		EntitiesWithLOD,
		EntitiesWithVelocity,
		SkinnedRepresentationCount,
		ActorRepresentationCount,
		NoneRepresentationCount,
		InvalidSkinnedDescCount);

	if (UMassRepresentationSubsystem* RepresentationSubsystem = World->GetSubsystem<UMassRepresentationSubsystem>())
	{
		FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();
		for (int32 MeshInfoIndex = 0; MeshInfoIndex < MeshInfos.Num(); ++MeshInfoIndex)
		{
			const FSkinnedMeshInstanceVisualizationDesc& Desc = MeshInfos[MeshInfoIndex].GetDesc();
			for (int32 MeshDescIndex = 0; MeshDescIndex < Desc.Meshes.Num(); ++MeshDescIndex)
			{
				const FMassSkinnedMeshInstanceVisualizationMeshDesc& MeshDesc = Desc.Meshes[MeshDescIndex];
				UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] MeshInfo[%d] Mesh[%d] Asset='%s' Provider='%s' MinSig=%.3f MaxSig=%.3f AnimationMinScreenSize=%.3f CastShadows=%d"),
					MeshInfoIndex,
					MeshDescIndex,
					*GetPathNameSafe(MeshDesc.Asset),
					*GetPathNameSafe(MeshDesc.TransformProvider),
					MeshDesc.MinLODSignificance,
					MeshDesc.MaxLODSignificance,
					MeshDesc.AnimationMinScreenSize,
					MeshDesc.bCastShadows ? 1 : 0);
			}
		}
	}

	for (const TPair<int32, int32>& Pair : RepresentationCounts)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] Representation '%s'(%d) = %d entities"),
			GetMassRepresentationTypeName(static_cast<EMassRepresentationType>(Pair.Key)),
			Pair.Key,
			Pair.Value);
	}

	for (const TPair<int32, int32>& Pair : LODCounts)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] LOD %d = %d entities"), Pair.Key, Pair.Value);
	}

	for (const TPair<int32, int32>& Pair : DescHandleCounts)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] SkinnedDescHandle %d = %d entities"), Pair.Key, Pair.Value);
	}

	for (const TPair<int32, int32>& Pair : SequenceCounts)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] Sequence %d = %d entities"), Pair.Key, Pair.Value);
	}

	for (TObjectIterator<UInstancedSkinnedMeshComponent> ComponentIt; ComponentIt; ++ComponentIt)
	{
		const UInstancedSkinnedMeshComponent* Component = *ComponentIt;
		if (!IsValid(Component) || Component->GetWorld() != World)
		{
			continue;
		}

		const UAnimSequenceTransformProviderData* AnimProvider = Cast<UAnimSequenceTransformProviderData>(Component->GetTransformProvider());
		if (!AnimProvider)
		{
			continue;
		}

		const auto& ProviderSequences = AnimProvider->GetSequences();
		for (const TPair<int32, int32>& Pair : SequenceCounts)
		{
			const int32 SequenceIndex = Pair.Key;
			const UObject* SequenceAsset = ProviderSequences.IsValidIndex(SequenceIndex)
				? ProviderSequences[SequenceIndex].Sequence.Get()
				: nullptr;

			UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassRepAnimDiag] Component='%s' Asset='%s' Provider='%s' Sequence=%d SequenceAsset='%s' Entities=%d"),
				*GetNameSafe(Component),
				*GetPathNameSafe(Component->GetSkinnedAsset()),
				*GetNameSafe(AnimProvider),
				SequenceIndex,
				*GetPathNameSafe(SequenceAsset),
				Pair.Value);
		}
	}

	UE_LOG(LogCrowdyMass, Warning, TEXT("===================== CROWDY MASS REPRESENTATION ANIMATION DIAGNOSTICS END ====================="));
}

FAutoConsoleCommandWithWorld CmdCrowdyMassLogRepresentationAnimationDiagnostics(
	TEXT("CrowdyMass.LogRepresentationAnimationDiagnostics"),
	TEXT("Logs Mass representation, LOD, velocity, and animation state for Crowdy remote entities."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&LogRepresentationAnimationDiagnostics));

float GetSpatialGridCellEntryDistance(const FBox& Bounds, const FVector& Start, const FVector& TraceDirection)
{
	if (!Bounds.IsValid || TraceDirection.IsNearlyZero())
	{
		return 0.0f;
	}

	const FVector& Min = Bounds.Min;
	const FVector& Max = Bounds.Max;
	float EntryDistance = TNumericLimits<float>::Max();

	for (int32 X = 0; X < 2; ++X)
	{
		for (int32 Y = 0; Y < 2; ++Y)
		{
			for (int32 Z = 0; Z < 2; ++Z)
			{
				const FVector Corner(
					X == 0 ? Min.X : Max.X,
					Y == 0 ? Min.Y : Max.Y,
					Z == 0 ? Min.Z : Max.Z);
				EntryDistance = FMath::Min(EntryDistance, static_cast<float>(FVector::DotProduct(Corner - Start, TraceDirection)));
			}
		}
	}

	return FMath::Max(EntryDistance, 0.0f);
}
}

void UCrowdyMassEntitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Passive Mass-entity service. Lifecycle is driven by UCrowdyMassRenderingBackend (selected as the
	// rendering backend in the map profile), not by subscribing to UCrowdyActorTracker directly: doing
	// that would double-spawn against whatever backend UCrowdyActorManager is already running.
	MassEntitySubsystem = Collection.InitializeDependency<UMassEntitySubsystem>();
	MassSpawnerSubsystem = Collection.InitializeDependency<UMassSpawnerSubsystem>();

	// Registers before the early returns below, and through the same call a consuming game would use,
	// so an entity spawned or updated in a world with no event router still gets its view state
	// decoded.
	RegisterStandardViewStateFields();
	RegisterStandardStateFragmentFields();

#if WITH_EDITOR
	// Live Coding hands out new class objects and re-instances what was built from the old ones. A cached
	// avatar would then be an object of a class that has been replaced, so the cache is emptied rather
	// than fixed up: everything in it is derivable from the tracker and costs nothing to make again.
	ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddUObject(
		this, &UCrowdyMassEntitySubsystem::HandleObjectsReplaced);

	// Editing an animation set or a Mass entity config in the details panel replaces no object, so the hook
	// above never sees it, and the answer cached for every class that resolves through the edited asset
	// stays as it was. This is the hook that makes acting on one of this subsystem's own warnings work in
	// the session it was printed in.
	ObjectPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(
		this, &UCrowdyMassEntitySubsystem::HandleObjectPropertyChanged);
#endif

	// Null in the transient world the engine builds at startup, where there is no session to route
	// anything for and no router to register with.
	const UWorld* World = GetWorld();
	if (!World || !World->GetGameInstance())
	{
		return;
	}

	// Entities living as rows in a table are in no actor-side registry, so everything addressed to them
	// resolves through this subsystem or not at all. This registration covers the two planes that arrive
	// off the wire: the calls and the view state.
	UCrowdyEventRouter* Router = Collection.InitializeDependency<UCrowdyEventRouter>();
	if (!Router)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: No Crowdy event router in this world; payloads addressed to Mass entities cannot be delivered."));
		return;
	}

	EventRouter = Router;
	Router->RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(this));
}

void UCrowdyMassEntitySubsystem::Deinitialize()
{
#if WITH_EDITOR
	if (ObjectsReplacedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
		ObjectsReplacedHandle.Reset();
	}

	if (ObjectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectPropertyChangedHandle);
		ObjectPropertyChangedHandle.Reset();
	}
#endif

	// Releases only this subsystem's own claim. Two worlds exist at once while travelling between levels
	// and the one being left tears down after the one being entered has registered, so a release that did
	// not check who holds the slot would silence the world the player is now in.
	if (UCrowdyEventRouter* Router = EventRouter.Get())
	{
		Router->UnregisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber>(this));
	}
	EventRouter.Reset();

	// Before the cache is dropped, so this world leaves no entity record behind naming an avatar it is
	// about to release. World subsystem teardown order is not fixed, so the registry may already be gone;
	// resolving it answers null in that case and there is nothing left to clean up anyway.
	UnregisterAllAvatarParticipants();

	// Grace entries hold records too, and this world is not coming back for them.
	for (TPair<FGuid, FCrowdyMassAvatarGraceEntry>& Pair : AvatarReclaimGrace)
	{
		EndAvatarGrace(Pair.Key, Pair.Value);
	}
	AvatarReclaimGrace.Empty();

	Avatars.Empty();
	EvictedAvatarUUIDs.Empty();
	ReceiveBudgets.Empty();
	StateEntityClassByUUID.Empty();

	// Dropped with the world they were registered for. A handler captures whatever the registering game
	// gave it, and this world's is not the next world's; the arriving world registers its own.
	EventHandlersByFunction.Empty();

	// Every proxy is outered to this world's persistent level, so holding one past the world would keep
	// that level alive and hand the next world a stand-in belonging to the last one.
	DispatchProxyByClass.Empty();
	ReportedUnproxyableClasses.Empty();
	ReportedUnhostableEventClasses.Empty();

	if (FMassEntityManager* EntityManager = GetMassEntityManager())
	{
		for (const TPair<FGuid, FMassEntityHandle>& Pair : UUIDToEntity)
		{
			if (EntityManager->IsEntityValid(Pair.Value))
			{
				EntityManager->DestroyEntity(Pair.Value);
			}
		}
	}

	UUIDToEntity.Empty();
	EntityToUUID.Empty();
	SpatialGridCells.Empty();
	SpatialGridEntries.Empty();
	TraceInstanceCache.Empty();

	// Emptied with the grid rather than left to the destructor. A cached candidate cell holds a raw
	// pointer into SpatialGridCells, and dropping the grid while a scratch entry still names one of its
	// sets leaves a pointer into freed memory for anything that queried afterwards.
	ScratchQueryCells.Empty();
	ScratchRaySeenCells.Empty();
	ScratchUniqueEntities.Empty();
	ScratchTraversalSeenCells.Empty();
	ScratchTraversalCells.Empty();
	ScratchSortedCandidateCells.Empty();
	ScratchTraceTestedEntities.Empty();
	MassEntitySubsystem = nullptr;
	MassSpawnerSubsystem = nullptr;
	PluginDefaultRemoteEntityConfig = nullptr;

	Super::Deinitialize();
}

bool UCrowdyMassEntitySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World || (World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game))
	{
		return false;
	}

	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(World);
	const UClass* BackendClass = Profile ? Profile->ActorManagement.BackendClass.Get() : nullptr;

	// Exactly the worlds where the actor manager builds the Mass backend, which asks for this subsystem while the world's subsystems are still being created.
	return BackendClass && BackendClass->IsChildOf(UCrowdyMassRenderingBackend::StaticClass());
}

bool UCrowdyMassEntitySubsystem::DrawsCrowdRows(const UWorld* World)
{
	return World && World->GetSubsystem<UCrowdyMassEntitySubsystem>();
}

FMassEntityHandle UCrowdyMassEntitySubsystem::SpawnRemoteEntity(const FGuid& UUID, const FTransform& InitialTransform)
{
	return SpawnRemoteEntity(UUID, InitialTransform, FInstancedStruct());
}

FMassEntityHandle UCrowdyMassEntitySubsystem::SpawnRemoteEntity(const FGuid& UUID, const FTransform& InitialTransform, const FInstancedStruct& InitialState, const UClass* StateEntityClass, const ECrowdyMassRemoteEntitySource Source)
{
	if (!UUID.IsValid())
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: Refusing to spawn remote Mass entity with invalid UUID."));
		return FMassEntityHandle();
	}

	FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager || !MassSpawnerSubsystem)
	{
		return FMassEntityHandle();
	}

	// Recorded on every successful (re)activation that names a class. A caller with no class to give leaves
	// whatever is already recorded alone rather than clearing it, which is the opposite of what it first
	// looks like it should do, so the reasoning matters. Not every spawn path carries a class: the
	// Blueprint-callable overload has none to give, and neither does a test or tool spawning by hand. If
	// such a call cleared the entry for a live entity, this subsystem would still HOLD that entity while
	// having no class to name for it, and the receive path reads "no class" as "not my entity", which sends
	// every delta for it to the spawn-wait queue to be retried and eventually dropped. At crowd scale that
	// is a queue full of work that can never resolve, evicting events for entities that really are still
	// spawning. Keeping the last known class is both cheaper and closer to the truth, and the entry is
	// removed for real when the entity goes away (UnregisterEntity and the map's own teardown).
	auto SetStateEntityClass = [this, &UUID](const UClass* Class)
	{
		RecordStateEntityClass(UUID, Class);
	};

	if (const FMassEntityHandle* Found = UUIDToEntity.Find(UUID))
	{
		// Held by value, never as the pointer into the map: recording the class below reaches entity
		// registration, whose listeners can spawn or destroy a remote entity and rehash this map.
		const FMassEntityHandle ExistingEntity = *Found;
		if (EntityManager->IsEntityValid(ExistingEntity))
		{
			if (InitializeCrowdyFragments(*EntityManager, ExistingEntity, UUID, InitialTransform))
			{
				SetStateEntityClass(StateEntityClass);

				// Applied before the transform work below reads the same payload, so a struct shaped
				// differently from what transform extraction expects still lands its view state
				// rather than having it silently dropped.
				ApplyViewStateToEntity(InitialState, *EntityManager, ExistingEntity);
				UpdateSpatialGridEntity(ExistingEntity, InitialTransform.GetLocation(), GetSpatialGridEntityBoundsRadius());
				return ExistingEntity;
			}

			UE_LOG(LogCrowdyMass, Error, TEXT("[CrowdyMassEntitySubsystem]: Existing Mass entity for UUID %s is missing required Crowdy fragments."), *UUID.ToString());
			return FMassEntityHandle();
		}

		UnregisterEntity(UUID, ExistingEntity);
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogCrowdyMass, Error, TEXT("[CrowdyMassEntitySubsystem]: Cannot spawn remote Mass entity for UUID %s: this subsystem has no world, so no Mass entity template can be resolved."), *UUID.ToString());
		return FMassEntityHandle();
	}

	FString TemplateResolveFailure;
	const FMassEntityTemplate* EntityTemplate = ResolveRemoteEntityTemplate(*World, Source, StateEntityClass, TemplateResolveFailure);
	if (!EntityTemplate)
	{
		UE_LOG(LogCrowdyMass, Error, TEXT("[CrowdyMassEntitySubsystem]: %s"), *TemplateResolveFailure);
		return FMassEntityHandle();
	}

	// Worked out here, on the first entity of a class, rather than left for whoever asks first. Resolving a
	// class's animation set loads its entity config and the set itself, and the callers that ask for one
	// afterwards are an event body or a Blueprint tick on the frame of the first death. The config was just
	// loaded above, so the first entity of a class is the cheapest moment there is, and it is also where
	// the line naming which source supplied the set lands early enough to be useful.
	//
	// Skipped entirely for a class-less spawn, which is the deliberate position-only crowd rather than a
	// mistake: there is nothing to resolve and nothing worth saying about it.
	if (StateEntityClass)
	{
		GetRemoteEntityAnimSet(StateEntityClass);
	}

	TArray<FMassEntityHandle> SpawnedEntities;
	SpawnedEntities.Reserve(1);
	auto CreationContext = MassSpawnerSubsystem->SpawnEntities(*EntityTemplate, 1, SpawnedEntities);

	if (SpawnedEntities.Num() != 1 || !EntityManager->IsEntityValid(SpawnedEntities[0]))
	{
		UE_LOG(LogCrowdyMass, Error, TEXT("[CrowdyMassEntitySubsystem]: Failed to spawn remote Mass entity for UUID %s."), *UUID.ToString());
		return FMassEntityHandle();
	}

	const FMassEntityHandle Entity = SpawnedEntities[0];
	if (!InitializeCrowdyFragments(*EntityManager, Entity, UUID, InitialTransform))
	{
		// Named generically rather than as a config asset: the same failure can come from either
		// resolution path, and only ConfiguredAsset actually names an asset. Read off the template that
		// was actually spawned from rather than by resolving the config a second time, which would load
		// it again to name it.
		UE_LOG(LogCrowdyMass, Error, TEXT("[CrowdyMassEntitySubsystem]: Remote entity template '%s' is missing required Crowdy fragments. Add the Crowdy Remote Entity trait or provide equivalent fragments."),
			*EntityTemplate->GetTemplateName());
		EntityManager->DestroyEntity(Entity);
		return FMassEntityHandle();
	}

	UUIDToEntity.Add(UUID, Entity);
	EntityToUUID.Add(Entity, UUID);
	SetStateEntityClass(StateEntityClass);

	// View state before transform work, matching UCrowdyMassRenderingBackend::ExtractUpdate: a
	// transform that fails to extract must not stop this initial payload's view state from landing.
	ApplyViewStateToEntity(InitialState, *EntityManager, Entity);
	UpdateSpatialGridEntity(Entity, InitialTransform.GetLocation(), GetSpatialGridEntityBoundsRadius());

	FVector Location;
	FQuat Rotation;
	float SmoothingDuration;
	if (TryExtractTransformState(InitialState, Location, Rotation, SmoothingDuration))
	{
		ApplyRemoteEntityTransform(UUID, Location, Rotation, 0, SmoothingDuration);
	}

	return Entity;
}

void UCrowdyMassEntitySubsystem::DestroyRemoteEntity(const FGuid& UUID)
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(UUID, Entity))
	{
		return;
	}

	if (FMassEntityManager* EntityManager = GetMassEntityManager())
	{
		if (EntityManager->IsEntityValid(Entity))
		{
			EntityManager->DestroyEntity(Entity);
		}
	}

	UnregisterEntity(UUID, Entity);
}

bool UCrowdyMassEntitySubsystem::UpdateRemoteEntityTransform(const FGuid& UUID, const FVector& Location, const FRotator& Rotation, const int64 ServerTimestampMs, const float SmoothingDuration)
{
	return ApplyRemoteEntityTransform(UUID, Location, Rotation.Quaternion(), ServerTimestampMs, SmoothingDuration);
}

bool UCrowdyMassEntitySubsystem::FindEntityByUUID(const FGuid& UUID, FMassEntityHandle& OutEntity) const
{
#if WITH_DEV_AUTOMATION_TESTS
	++EntityLookupCountForTest;
#endif

	const FMassEntityHandle* Entity = UUIDToEntity.Find(UUID);
	if (!Entity)
	{
		OutEntity.Reset();
		return false;
	}

	if (const FMassEntityManager* EntityManager = GetMassEntityManager())
	{
		if (EntityManager->IsEntityValid(*Entity))
		{
			OutEntity = *Entity;
			return true;
		}
	}

	OutEntity.Reset();
	return false;
}

bool UCrowdyMassEntitySubsystem::FindUUIDByEntity(const FMassEntityHandle Entity, FGuid& OutUUID) const
{
	if (const FGuid* UUID = EntityToUUID.Find(Entity))
	{
		OutUUID = *UUID;
		return true;
	}

	OutUUID.Invalidate();
	return false;
}

void UCrowdyMassEntitySubsystem::GetTrackedRemoteEntities(TArray<FMassEntityHandle>& OutEntities) const
{
	OutEntities.Reset();

	const FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager)
	{
		return;
	}

	OutEntities.Reserve(EntityToUUID.Num());
	for (const TPair<FMassEntityHandle, FGuid>& Pair : EntityToUUID)
	{
		if (EntityManager->IsEntityValid(Pair.Key))
		{
			OutEntities.Add(Pair.Key);
		}
	}
}

int32 UCrowdyMassEntitySubsystem::GetTrackedRemoteEntityCount() const
{
	return EntityToUUID.Num();
}

bool UCrowdyMassEntitySubsystem::GetTraceCandidateEntities(const FVector& Start, const FVector& End, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount) const
{
	return GetTraceCandidateEntities(Start, End, GetSpatialGridTraceQueryRadius(), OutEntities, OutVisitedCellCount);
}

bool UCrowdyMassEntitySubsystem::GetTraceCandidateEntities(const FVector& Start, const FVector& End, const float QueryRadius, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount) const
{
	return GetTraceCandidateEntities(Start, End, QueryRadius, OutEntities, OutVisitedCellCount, nullptr);
}

bool UCrowdyMassEntitySubsystem::GetTraceCandidateEntities(const FVector& Start, const FVector& End, const float QueryRadius, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount, TArray<FBox>* OutVisitedCellBounds) const
{
	OutEntities.Reset();
	OutVisitedCellCount = 0;
	if (OutVisitedCellBounds)
	{
		OutVisitedCellBounds->Reset();
	}

	if (!IsSpatialGridEnabled() || SpatialGridCells.IsEmpty())
	{
		GetTrackedRemoteEntities(OutEntities);
		return false;
	}

	TArray<FIntVector>& QueryCells = ScratchQueryCells;
	const float CombinedQueryRadius = FMath::Max(GetSpatialGridTraceQueryRadius(), QueryRadius);
	if (!GetSpatialGridCellsForRay(Start, End, CombinedQueryRadius, GetSpatialGridMaxRayCells(), QueryCells))
	{
		GetTrackedRemoteEntities(OutEntities);
		return false;
	}

	OutVisitedCellCount = QueryCells.Num();
	if (OutVisitedCellBounds)
	{
		OutVisitedCellBounds->Reserve(QueryCells.Num());
		for (const FIntVector& Cell : QueryCells)
		{
			OutVisitedCellBounds->Add(GetSpatialGridDebugCellBounds(Cell, Start, End));
		}
	}

	// Reset, never Empty: the storage this grew to over earlier queries is what stops a query that
	// gathers most of the crowd rehashing its way back up to the same size, and it costs nothing in the
	// queries that touch a handful, which is what reserving the crowd size up front would not.
	TSet<FMassEntityHandle>& UniqueEntities = ScratchUniqueEntities;
	UniqueEntities.Reset();
	for (const FIntVector& Cell : QueryCells)
	{
		if (const TSet<FMassEntityHandle>* CellEntities = SpatialGridCells.Find(Cell))
		{
			for (const FMassEntityHandle Entity : *CellEntities)
			{
				UniqueEntities.Add(Entity);
			}
		}
	}

	OutEntities.Reserve(UniqueEntities.Num());
	for (const FMassEntityHandle Entity : UniqueEntities)
	{
		OutEntities.Add(Entity);
	}

	return true;
}

bool UCrowdyMassEntitySubsystem::GetTraceCandidateCells(const FVector& Start, const FVector& End, const float QueryRadius, TArray<FCrowdyMassTraceCandidateCell>& OutCells, int32& OutVisitedCellCount, TArray<FBox>* OutVisitedCellBounds) const
{
	OutCells.Reset();
	OutVisitedCellCount = 0;
	if (OutVisitedCellBounds)
	{
		OutVisitedCellBounds->Reset();
	}

	if (!IsSpatialGridEnabled() || SpatialGridCells.IsEmpty())
	{
		return false;
	}

	TArray<FIntVector>& QueryCells = ScratchQueryCells;
	const float CombinedQueryRadius = FMath::Max(GetSpatialGridTraceQueryRadius(), QueryRadius);
	if (!GetSpatialGridCellsForRay(Start, End, CombinedQueryRadius, GetSpatialGridMaxRayCells(), QueryCells))
	{
		return false;
	}

	OutVisitedCellCount = QueryCells.Num();
	const FVector TraceDirection = (End - Start).GetSafeNormal();

	OutCells.Reserve(QueryCells.Num());
	if (OutVisitedCellBounds)
	{
		OutVisitedCellBounds->Reserve(QueryCells.Num());
	}

	for (const FIntVector& Cell : QueryCells)
	{
		const FBox DebugBounds = GetSpatialGridDebugCellBounds(Cell, Start, End);
		if (OutVisitedCellBounds)
		{
			OutVisitedCellBounds->Add(DebugBounds);
		}

		FCrowdyMassTraceCandidateCell& CandidateCell = OutCells.AddDefaulted_GetRef();
		CandidateCell.EntryDistance = GetSpatialGridCellEntryDistance(DebugBounds, Start, TraceDirection);
		CandidateCell.DebugBounds = DebugBounds;
		CandidateCell.Entities = SpatialGridCells.Find(Cell);
	}

	if (CombinedQueryRadius > UE_KINDA_SMALL_NUMBER)
	{
		OutCells.Sort([](const FCrowdyMassTraceCandidateCell& A, const FCrowdyMassTraceCandidateCell& B)
		{
			return A.EntryDistance < B.EntryDistance;
		});
	}

	return true;
}

bool UCrowdyMassEntitySubsystem::ForEachTraceCandidateCell(
	const FVector& Start,
	const FVector& End,
	const float QueryRadius,
	TFunctionRef<bool(const FCrowdyMassTraceCandidateCell& Cell, bool bTraversalIsEntrySorted)> Visitor,
	int32& OutVisitedCellCount) const
{
	OutVisitedCellCount = 0;

	if (!IsSpatialGridEnabled() || SpatialGridCells.IsEmpty())
	{
		return false;
	}

	const float CellSize = GetSpatialGridCellSize();
	const bool bUse3D = GetSpatialGridMode() == ECrowdyMassSpatialGridMode::ThreeDimensional;
	const int32 ClampedMaxCells = GetSpatialGridMaxRayCells();
	const float CombinedQueryRadius = FMath::Max(GetSpatialGridTraceQueryRadius(), QueryRadius);
	const int32 RadiusCells = FMath::Max(FMath::CeilToInt(FMath::Max(CombinedQueryRadius, 0.0f) / CellSize), 0);
	if (RadiusCells > 0)
	{
		// The visitor is called while this array is being walked, so it must not reach any call that
		// refills it. See the scratch members' declaration for the rule.
		TArray<FCrowdyMassTraceCandidateCell>& CandidateCells = ScratchSortedCandidateCells;
		if (!GetTraceCandidateCells(Start, End, QueryRadius, CandidateCells, OutVisitedCellCount))
		{
			return false;
		}

		for (const FCrowdyMassTraceCandidateCell& CandidateCell : CandidateCells)
		{
			if (!Visitor(CandidateCell, true))
			{
				break;
			}
		}

		return true;
	}

	constexpr bool bTraversalIsEntrySorted = true;
	const FIntVector StartCell = GetSpatialGridCell(Start);
	const FIntVector EndCell = GetSpatialGridCell(End);
	const FVector Delta = End - Start;
	const double SegmentLength = Delta.Size();
	const FVector TraceDirection = Delta.GetSafeNormal();
	// Reset rather than reserved from the cell ceiling: a traversal visits a handful of cells and the
	// ceiling is thousands, so reserving to it would cost far more than it saves. Keeping the storage
	// lets the set settle at whatever this world's traces actually need.
	TSet<FIntVector>& SeenCells = ScratchTraversalSeenCells;
	SeenCells.Reset();
	TArray<FCrowdyMassTraceCandidateCell>& StepCells = ScratchTraversalCells;

	auto VisitExpandedCells = [&](const FIntVector& CenterCell, const float CenterEntryDistance)
	{
		StepCells.Reset();

		const int32 MinZ = bUse3D ? CenterCell.Z - RadiusCells : 0;
		const int32 MaxZ = bUse3D ? CenterCell.Z + RadiusCells : 0;
		for (int32 X = CenterCell.X - RadiusCells; X <= CenterCell.X + RadiusCells; ++X)
		{
			for (int32 Y = CenterCell.Y - RadiusCells; Y <= CenterCell.Y + RadiusCells; ++Y)
			{
				for (int32 Z = MinZ; Z <= MaxZ; ++Z)
				{
					const FIntVector Cell(X, Y, Z);
					if (SeenCells.Contains(Cell))
					{
						continue;
					}

					if (OutVisitedCellCount >= ClampedMaxCells)
					{
						return false;
					}

					SeenCells.Add(Cell);
					++OutVisitedCellCount;

					FCrowdyMassTraceCandidateCell& CandidateCell = StepCells.AddDefaulted_GetRef();
					CandidateCell.DebugBounds = GetSpatialGridDebugCellBounds(Cell, Start, End);
					CandidateCell.EntryDistance = bTraversalIsEntrySorted
						? CenterEntryDistance
						: GetSpatialGridCellEntryDistance(CandidateCell.DebugBounds, Start, TraceDirection);
					CandidateCell.Entities = SpatialGridCells.Find(Cell);
				}
			}
		}

		if (!bTraversalIsEntrySorted && StepCells.Num() > 1)
		{
			StepCells.Sort([](const FCrowdyMassTraceCandidateCell& A, const FCrowdyMassTraceCandidateCell& B)
			{
				return A.EntryDistance < B.EntryDistance;
			});
		}

		for (const FCrowdyMassTraceCandidateCell& CandidateCell : StepCells)
		{
			if (!Visitor(CandidateCell, bTraversalIsEntrySorted))
			{
				return false;
			}
		}

		return true;
	};

	if (!VisitExpandedCells(StartCell, 0.0f))
	{
		return true;
	}

	if (StartCell == EndCell || SegmentLength <= UE_KINDA_SMALL_NUMBER)
	{
		return true;
	}

	FIntVector CurrentCell = StartCell;
	const double InfiniteT = TNumericLimits<double>::Max();

	auto ConfigureAxis = [CellSize, InfiniteT](const double StartCoordinate, const double DeltaCoordinate, const int32 CellCoordinate, int32& OutStep, double& OutTMax, double& OutTDelta)
	{
		if (FMath::Abs(DeltaCoordinate) <= UE_KINDA_SMALL_NUMBER)
		{
			OutStep = 0;
			OutTMax = InfiniteT;
			OutTDelta = InfiniteT;
			return;
		}

		OutStep = DeltaCoordinate > 0.0 ? 1 : -1;
		const double Boundary = OutStep > 0
			? static_cast<double>(CellCoordinate + 1) * CellSize
			: static_cast<double>(CellCoordinate) * CellSize;
		OutTMax = FMath::Max((Boundary - StartCoordinate) / DeltaCoordinate, 0.0);
		OutTDelta = CellSize / FMath::Abs(DeltaCoordinate);
	};

	int32 StepX = 0;
	int32 StepY = 0;
	int32 StepZ = 0;
	double TMaxX = InfiniteT;
	double TMaxY = InfiniteT;
	double TMaxZ = InfiniteT;
	double TDeltaX = InfiniteT;
	double TDeltaY = InfiniteT;
	double TDeltaZ = InfiniteT;

	ConfigureAxis(Start.X, Delta.X, StartCell.X, StepX, TMaxX, TDeltaX);
	ConfigureAxis(Start.Y, Delta.Y, StartCell.Y, StepY, TMaxY, TDeltaY);
	if (bUse3D)
	{
		ConfigureAxis(Start.Z, Delta.Z, StartCell.Z, StepZ, TMaxZ, TDeltaZ);
	}

	const int32 MaxSteps = FMath::Abs(EndCell.X - StartCell.X)
		+ FMath::Abs(EndCell.Y - StartCell.Y)
		+ (bUse3D ? FMath::Abs(EndCell.Z - StartCell.Z) : 0)
		+ 3;
	int32 Steps = 0;

	while (CurrentCell != EndCell)
	{
		if (++Steps > MaxSteps)
		{
			return true;
		}

		const double NextT = FMath::Min(TMaxX, FMath::Min(TMaxY, TMaxZ));
		constexpr double TieTolerance = 1.e-8;
		if (StepX != 0 && TMaxX <= NextT + TieTolerance)
		{
			CurrentCell.X += StepX;
			TMaxX += TDeltaX;
		}
		if (StepY != 0 && TMaxY <= NextT + TieTolerance)
		{
			CurrentCell.Y += StepY;
			TMaxY += TDeltaY;
		}
		if (bUse3D && StepZ != 0 && TMaxZ <= NextT + TieTolerance)
		{
			CurrentCell.Z += StepZ;
			TMaxZ += TDeltaZ;
		}

		const float CellEntryDistance = static_cast<float>(FMath::Clamp(NextT, 0.0, 1.0) * SegmentLength);
		if (!VisitExpandedCells(CurrentCell, CellEntryDistance))
		{
			return true;
		}
	}

	return true;
}

bool UCrowdyMassEntitySubsystem::GetOverlapCandidateEntities(const FVector& Origin, const float QueryRadius, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount) const
{
	OutEntities.Reset();
	OutVisitedCellCount = 0;

	if (!IsSpatialGridEnabled() || SpatialGridCells.IsEmpty())
	{
		GetTrackedRemoteEntities(OutEntities);
		return false;
	}

	// Past the cap the query scans every tracked entity, exactly as a ray past its cap does.
	TArray<FIntVector>& QueryCells = ScratchQueryCells;
	if (!GetSpatialGridCellsForSphere(Origin, FMath::Max(QueryRadius, 0.0f), GetSpatialGridMaxRayCells(), QueryCells))
	{
		GetTrackedRemoteEntities(OutEntities);
		return false;
	}
	OutVisitedCellCount = QueryCells.Num();

	// Reset, never Empty; see GetTraceCandidateEntities for why this keeps its storage between queries.
	TSet<FMassEntityHandle>& UniqueEntities = ScratchUniqueEntities;
	UniqueEntities.Reset();
	for (const FIntVector& Cell : QueryCells)
	{
		if (const TSet<FMassEntityHandle>* CellEntities = SpatialGridCells.Find(Cell))
		{
			for (const FMassEntityHandle Entity : *CellEntities)
			{
				UniqueEntities.Add(Entity);
			}
		}
	}

	OutEntities.Reserve(UniqueEntities.Num());
	for (const FMassEntityHandle Entity : UniqueEntities)
	{
		OutEntities.Add(Entity);
	}

	return true;
}

const FCrowdyMassTraceInstanceCacheEntry* UCrowdyMassEntitySubsystem::FindTraceInstanceCache(const FMassEntityHandle Entity) const
{
	return TraceInstanceCache.Find(Entity);
}

void UCrowdyMassEntitySubsystem::UpdateTraceInstanceCache(const FMassEntityHandle Entity, const FCrowdyMassTraceInstanceCacheEntry& Entry)
{
	TraceInstanceCache.Add(Entity, Entry);
}

void UCrowdyMassEntitySubsystem::RemoveTraceInstanceCache(const FMassEntityHandle Entity)
{
	TraceInstanceCache.Remove(Entity);
}

void UCrowdyMassEntitySubsystem::ClearTraceInstanceCache()
{
	TraceInstanceCache.Empty();
}

TSet<FMassEntityHandle>& UCrowdyMassEntitySubsystem::ResetTraceTestedEntitiesScratch()
{
	// Reset, never Empty: the storage this grew to over previous traces is exactly what stops the next
	// one rehashing its way back up to the same size.
	ScratchTraceTestedEntities.Reset();
	return ScratchTraceTestedEntities;
}

void UCrowdyMassEntitySubsystem::ConfigureOverrides(const TSoftObjectPtr<UMassEntityConfigAsset>& InConfigOverride, const float InSmoothingOverride,
	const TArray<FCrowdyMassEntityConfigBinding>& InPerClassConfigs)
{
	RemoteEntityConfigOverride = InConfigOverride;
	SmoothingDurationOverride = InSmoothingOverride;

	// Rebuilt rather than merged, because a backend swap replaces the whole binding set and a stale
	// entry from the previous backend would silently keep building rows from an asset nothing points at
	// any more.
	EntityConfigByClass.Reset();

	for (const FCrowdyMassEntityConfigBinding& Binding : InPerClassConfigs)
	{
		if (Binding.EntityClass.IsNull() && Binding.EntityConfig.IsNull())
		{
			continue;
		}

		const UClass* BoundClass = Binding.EntityClass.LoadSynchronous();
		const UMassEntityConfigAsset* BoundConfig = Binding.EntityConfig.LoadSynchronous();

		// Reported rather than skipped quietly. A half-filled row is an authoring mistake whose only
		// other symptom is one kind of entity rendering as another, which is a long way from its cause.
		if (!BoundClass || !BoundConfig)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: per-class entity config entry ignored, class '%s' and config '%s' must both resolve. Entities of that class will use the default config."),
				*Binding.EntityClass.ToString(), *Binding.EntityConfig.ToString());
			continue;
		}

		EntityConfigByClass.Add(BoundClass, BoundConfig);
	}

	UE_CLOG(EntityConfigByClass.Num() > 0, LogCrowdyMass, Log,
		TEXT("[CrowdyMassEntitySubsystem]: %d per-class Mass entity config binding(s) resolved."),
		EntityConfigByClass.Num());

	// A config is what names a class's animation set, so replacing the bindings replaces every authored
	// pose row and with it the meaning of every pose id already handed out. Dropped for every class rather
	// than for the ones whose binding moved: the backend override and the project setting answer for every
	// class that has no binding of its own, so a change to either moves answers no binding names.
	InvalidateAnimSetCaches();

	// A near tier patch names the config it was copied from as its parent, so it is only correct while
	// that config is still the one answering for the class. Every entry is dropped rather than the ones
	// whose binding moved, for the same reason the animation answers are.
	InvalidateNearTierPatchedConfigs();
}

bool UCrowdyMassEntitySubsystem::IsEntityKnown(const FGuid& EntityUUID) const
{
	// The tracker is the whole answer. Not the avatar cache, and not the recorded class: an entity is held
	// here from the moment it is spawned until it is unregistered, whether or not anything has minted an
	// avatar for it and whether or not a class was ever named for it.
	FMassEntityHandle Entity;
	return FindEntityByUUID(EntityUUID, Entity);
}

const UClass* UCrowdyMassEntitySubsystem::GetEntityClass(const FGuid& EntityUUID) const
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(EntityUUID, Entity))
	{
		// Not a live entity this subsystem holds: either never known, or known and since destroyed. Checked
		// so a class recorded for an id cannot outlive the entity it described and answer for whatever
		// spawns under that id next.
		return nullptr;
	}

	// The class stored is whatever SpawnRemoteEntity was last given for this UUID, resolved locally by
	// UCrowdyActorManager::ResolveEntityClass rather than read off the payload being asked about. That is
	// what makes this answer a security boundary the wire cannot influence.
	//
	// Null when the entity was spawned without one, which is ordinary: a crowd that is only ever a position
	// declares nothing. IsEntityKnown above still answers true for it, so such an entity refuses payloads
	// rather than making the router wait for a spawn that already happened.
	const TWeakObjectPtr<const UClass>* StateClass = StateEntityClassByUUID.Find(EntityUUID);
	return StateClass ? StateClass->Get() : nullptr;
}

UObject* UCrowdyMassEntitySubsystem::ResolveReceiver(const FGuid& EntityUUID, const UFunction* Function)
{
	if (!Function)
	{
		return nullptr;
	}

	FMassEntityHandle Entity;
	if (!FindEntityByUUID(EntityUUID, Entity))
	{
		// Not an id this subsystem holds. Nothing is minted for it: an id off the wire is arbitrary, and an
		// avatar is only ever created for one the tracker already holds.
		return nullptr;
	}

	// Asked before anything is charged, because an event declared on the class the entity's owner runs as a
	// real actor legitimately has no avatar to run on. That call is not lost, it goes to HandleEntityEvent,
	// which charges the entity's allowance itself; charging here as well would bill one inbound call twice.
	// It also keeps GetOrCreateAvatar's mismatch warning for what it is meant for, an avatar class that
	// cannot receive an event authored ON an avatar class, rather than firing for every actor-class event.
	const UClass* RequiredClass = Function->GetOwnerClass();
	if (!CanAvatarReceive(EntityUUID, Entity, RequiredClass))
	{
		return nullptr;
	}

	if (!ChargeReceiveBudget(EntityUUID))
	{
		UE_LOG(LogCrowdyMass, Verbose,
			TEXT("[CrowdyMassEntitySubsystem]: Inbound event for entity %s refused; its allowance is spent."),
			*EntityUUID.ToString());
		return nullptr;
	}

	return GetOrCreateAvatar(EntityUUID, RequiredClass);
}

bool UCrowdyMassEntitySubsystem::CanAvatarReceive(const FGuid& UUID, const FMassEntityHandle Entity,
	const UClass* RequiredClass) const
{
	if (!RequiredClass)
	{
		return true;
	}

	// The cached avatar decides only while it still stands for THIS entity. GetOrCreateAvatar drops one
	// whose handle has moved on and mints a fresh one from the spawn class, so an answer taken from a
	// stale avatar would describe an object that is about to be thrown away.
	if (const FCrowdyMassAvatarEntry* Existing = Avatars.Find(UUID))
	{
		const UCrowdyMassEntity* Cached = Existing->Avatar;
		if (::IsValid(Cached) && Cached->GetHandle() == Entity)
		{
			return Cached->IsA(RequiredClass);
		}
	}

	const UClass* SpawnClass = GetEffectiveAvatarClass();
	return SpawnClass && SpawnClass->IsChildOf(RequiredClass);
}

FCrowdyMassEventHandlerHandle UCrowdyMassEntitySubsystem::AddEventHandlerRegistration(const UFunction* Function,
	FCrowdyMassEventHandlerRegistration&& Registration)
{
	FCrowdyMassEventHandlerHandle Handle;
	Handle.Id = NextEventHandlerId++;

	Registration.Function = Function;
	Registration.Handle = Handle;

	EventHandlersByFunction.FindOrAdd(FObjectKey(Function))
		.Add(MakeShared<FCrowdyMassEventHandlerRegistration, ESPMode::NotThreadSafe>(MoveTemp(Registration)));

	return Handle;
}

bool UCrowdyMassEntitySubsystem::RemoveEventHandlerRegistration(const FCrowdyMassEventHandlerRegistration& Registration)
{
	// Searched across every function rather than looked up by the registration's own, because that function
	// may be exactly what has gone: a binding is dropped when its Blueprint is recompiled out from under it,
	// and by then there is no key left to find it under.
	for (TPair<FObjectKey, TArray<FCrowdyMassEventHandlerRef>>& Pair : EventHandlersByFunction)
	{
		const int32 RemovedCount = Pair.Value.RemoveAll([&Registration](const FCrowdyMassEventHandlerRef& Entry)
		{
			return &Entry.Get() == &Registration;
		});

		if (RemovedCount > 0)
		{
			// The entry is only dropped from the list; whoever is walking it still holds a reference, so a
			// handler that removes itself keeps running to its own return. An emptied list is left in place
			// rather than removed, because removing it would rehash a map a dispatch may be reading.
			return true;
		}
	}

	return false;
}

FCrowdyMassEventHandlerHandle UCrowdyMassEntitySubsystem::RegisterEventHandler(const UFunction* Function,
	FCrowdyMassEventIngestor Ingestor)
{
	if (!Function || !Ingestor)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Ignoring an event handler registration with no function or no handler."));
		return FCrowdyMassEventHandlerHandle();
	}

	FCrowdyMassEventHandlerRegistration Registration;
	Registration.Ingestor = MoveTemp(Ingestor);
	return AddEventHandlerRegistration(Function, MoveTemp(Registration));
}

bool UCrowdyMassEntitySubsystem::BuildObjectEventHandlerPlan(const UFunction* EventFunction,
	const UFunction* HandlerFunction, const FStructProperty*& OutTargetParam,
	TArray<FCrowdyMassEventParamCopy>& OutParamCopies, TArray<FString>& OutUnmatched, FString& OutRefusal)
{
	OutTargetParam = nullptr;
	OutParamCopies.Reset();
	OutUnmatched.Reset();
	OutRefusal.Reset();

	if (!EventFunction || !HandlerFunction)
	{
		OutRefusal = TEXT("the event or the handler is gone.");
		return false;
	}

	for (TFieldIterator<FProperty> It(HandlerFunction); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		const FProperty* DestParam = *It;

		// A handler cannot answer anything: there is nobody to answer, since it is run for a call that
		// arrived from another machine. Accepting an output parameter would leave the call writing it back
		// into a frame this code owns and then throws away.
		if (FCrowdyRPC::IsTrueOutputParam(DestParam))
		{
			OutRefusal = FString::Printf(
				TEXT("its parameter '%s' is an output or a return value, and a handler has nobody to return anything to."),
				*DestParam->GetName());
			return false;
		}

		if (!OutTargetParam)
		{
			const FStructProperty* AsStruct = CastField<FStructProperty>(DestParam);
			if (!AsStruct || AsStruct->Struct != FCrowdyMassEntityTarget::StaticStruct())
			{
				OutRefusal = FString::Printf(
					TEXT("its first parameter must be a Crowdy Mass Entity Target naming the entity the call arrived for, and it is '%s'."),
					*DestParam->GetName());
				return false;
			}

			OutTargetParam = AsStruct;
			continue;
		}

		// BY NAME, and only where the two sides declare exactly the same type. The same rule the by-name
		// reads on FCrowdyEventParams follow, and for the same reason: a parameter inserted above another
		// must not start feeding it, and one retyped since the handler was written must read as absent
		// rather than be reinterpreted through the wrong property.
		const FProperty* SourceParam = EventFunction->FindPropertyByName(DestParam->GetFName());
		const bool bCarriedByTheCall = SourceParam
			&& SourceParam->HasAnyPropertyFlags(CPF_Parm)
			&& !FCrowdyRPC::IsTrueOutputParam(SourceParam)
			&& SourceParam->SameType(DestParam);

		if (!bCarriedByTheCall)
		{
			OutUnmatched.Add(DestParam->GetName());
			continue;
		}

		FCrowdyMassEventParamCopy Copy;
		Copy.SourceParam = SourceParam;
		Copy.DestParam = DestParam;
		OutParamCopies.Add(Copy);
	}

	if (!OutTargetParam)
	{
		OutRefusal = TEXT("it takes no parameters, and the first must be a Crowdy Mass Entity Target naming the entity the call arrived for.");
		return false;
	}

	return true;
}

FCrowdyMassEventHandlerHandle UCrowdyMassEntitySubsystem::RegisterObjectEventHandler(const UFunction* Function,
	UObject* HandlerObject, const FName HandlerFunctionName)
{
	const FCrowdyMassEventHandlerHandle Refused;

	if (!Function || !::IsValid(HandlerObject) || HandlerFunctionName.IsNone())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Ignoring an event binding with no event, no object or no function name."));
		return Refused;
	}

	UFunction* Handler = HandlerObject->FindFunction(HandlerFunctionName);
	if (!Handler)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Cannot bind '%s': '%s' declares no function called '%s'. Recompile the Blueprint that binds it."),
			*Function->GetName(), *GetNameSafe(HandlerObject->GetClass()), *HandlerFunctionName.ToString());
		return Refused;
	}

	// The same object and function name bound again is the same binding, not a second one. A graph that
	// reaches its bind node twice (a respawn, a re-entered state) must not start running its handler twice
	// for every call. Answered before the plan is built, so a repeated bind is silent rather than repeating
	// whatever the first one had to say about the signature.
	if (const TArray<FCrowdyMassEventHandlerRef>* Existing = EventHandlersByFunction.Find(FObjectKey(Function)))
	{
		for (const FCrowdyMassEventHandlerRef& Entry : *Existing)
		{
			if (Entry->Function.Get() == Function
				&& Entry->BoundFunctionName == HandlerFunctionName
				&& Entry->BoundObject.Get() == HandlerObject)
			{
				return Entry->Handle;
			}
		}
	}

	const FStructProperty* TargetParam = nullptr;
	TArray<FCrowdyMassEventParamCopy> ParamCopies;
	TArray<FString> Unmatched;
	FString Refusal;

	if (!BuildObjectEventHandlerPlan(Function, Handler, TargetParam, ParamCopies, Unmatched, Refusal))
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: Cannot bind '%s' to '%s::%s': %s"),
			*Function->GetName(), *GetNameSafe(HandlerObject->GetClass()), *HandlerFunctionName.ToString(),
			*Refusal);
		return Refused;
	}

	// Said once, here, rather than once per arriving call: what does and does not land is decided by two
	// signatures that are both fixed by the time the binding is made.
	if (Unmatched.Num() > 0)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Binding '%s' to '%s::%s': the call carries no matching parameter for %s. Those keep their defaults. Recompile the binding Blueprint if the event's signature has changed."),
			*Function->GetName(), *GetNameSafe(HandlerObject->GetClass()), *HandlerFunctionName.ToString(),
			*FString::Join(Unmatched, TEXT(", ")));
	}

	FCrowdyMassEventHandlerRegistration Registration;
	Registration.BoundObject = HandlerObject;
	Registration.BoundFunction = Handler;
	Registration.BoundFunctionName = HandlerFunctionName;
	Registration.BoundTargetParam = TargetParam;
	Registration.BoundParamCopies = MoveTemp(ParamCopies);

	return AddEventHandlerRegistration(Function, MoveTemp(Registration));
}

bool UCrowdyMassEntitySubsystem::UnregisterEventHandler(const FCrowdyMassEventHandlerHandle Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	for (TPair<FObjectKey, TArray<FCrowdyMassEventHandlerRef>>& Pair : EventHandlersByFunction)
	{
		const int32 RemovedCount = Pair.Value.RemoveAll([Handle](const FCrowdyMassEventHandlerRef& Entry)
		{
			return Entry->Handle.Id == Handle.Id;
		});

		if (RemovedCount > 0)
		{
			return true;
		}
	}

	return false;
}

bool UCrowdyMassEntitySubsystem::UnregisterObjectEventHandler(const UFunction* Function,
	const UObject* HandlerObject, const FName HandlerFunctionName)
{
	if (!Function || !HandlerObject || HandlerFunctionName.IsNone())
	{
		return false;
	}

	TArray<FCrowdyMassEventHandlerRef>* Handlers = EventHandlersByFunction.Find(FObjectKey(Function));
	if (!Handlers)
	{
		return false;
	}

	// GetEvenIfUnreachable on the bound object, so an unbind that runs while the object is being torn down
	// still finds its own binding instead of leaving one behind that matches nothing.
	const int32 RemovedCount = Handlers->RemoveAll(
		[Function, HandlerObject, HandlerFunctionName](const FCrowdyMassEventHandlerRef& Entry)
		{
			return Entry->Function.Get() == Function
				&& Entry->BoundFunctionName == HandlerFunctionName
				&& Entry->BoundObject.GetEvenIfUnreachable() == HandlerObject;
		});

	return RemovedCount > 0;
}

bool UCrowdyMassEntitySubsystem::IsEventHandlerRegistered(const UFunction* Function) const
{
	// Registration, and only registration. This deliberately does NOT ask HasEntityEventHandler, which
	// answers the router's wider question of whether this plane can apply the event at all and is true for
	// every event once the dispatch proxy is on. A game asking this is asking whether its own handler
	// registered, and an answer of yes for a handler that never did is the whole failure it exists to catch.
	return NumEventHandlersRegistered(Function) > 0;
}

int32 UCrowdyMassEntitySubsystem::NumEventHandlersRegistered(const UFunction* Function) const
{
	if (!Function)
	{
		return 0;
	}

	const TArray<FCrowdyMassEventHandlerRef>* Handlers = EventHandlersByFunction.Find(FObjectKey(Function));
	if (!Handlers)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FCrowdyMassEventHandlerRef& Entry : *Handlers)
	{
		// The weak function is compared, not just the key: a registration whose function has gone describes
		// a signature that no longer exists, and running its handler against a frame laid out for a
		// different one would read another parameter's bytes.
		if (Entry->Function.Get() != Function)
		{
			continue;
		}

		// A binding whose object has been destroyed is not a handler. Counting one would have the router
		// deliver a call this subsystem then drops in silence, rather than report that the event reached a
		// client with nothing registered for it.
		const bool bLive = Entry->IsObjectBinding() ? Entry->BoundObject.IsValid() : static_cast<bool>(Entry->Ingestor);
		Count += bLive ? 1 : 0;
	}

	return Count;
}

namespace
{
	// Whether the AUTHOR declared this event an action, through meta=(CrowdyAction), read via the routing
	// info so it comes from the baked snapshot in a cooked build where the metadata itself is gone.
	//
	// The declaration is the game's, never this plugin's. Nothing here decides that an event means an
	// animation by the shape of its parameters: which of a game's events are actions is the game's to say,
	// and a plugin inferring it would be deciding what a game's nouns mean.
	bool IsDeclaredActionEvent(const UFunction* Function)
	{
		// GetFnInfo does not modify the function; it memoises routing per function object behind a lock,
		// which is why this can be asked on every inbound event.
		return Function && FCrowdyRPC::GetFnInfo(const_cast<UFunction*>(Function)).bIsAction;
	}

	// Built once. The handler captures three names and nothing else, so one instance serves every entity
	// and every event; rebuilding it per inbound call would allocate for nothing.
	const FCrowdyMassEventIngestor& DefaultActionIngestor()
	{
		static const FCrowdyMassEventIngestor Ingestor = CrowdyMassMakeActionEventHandler();
		return Ingestor;
	}

	// Whether a stand-in of StandInClass can run this event's own body. ProcessEvent applies the declaring
	// class's property offsets to whatever object it is given, so that object's class has to derive the
	// declaring class or every one of those offsets names a different variable.
	//
	// A function declared on a COMPONENT of the entity's class fails this, and so does one declared on
	// UCrowdyMassEntity. Both are legitimate declarations for an observer holding the entity as a real actor,
	// which resolves such a body onto the object that declares it; a stand-in cannot, because it is
	// constructed rather than spawned and carries only what its C++ constructor made.
	//
	// One function rather than a rule restated in two places, because the allowance step and the dispatch have
	// to give the same answer: a call the dispatch will decline must be declined before it costs the named
	// entity a token, and a call the dispatch will run must not be declined at all.
	bool ClassCanHostEventBody(const UClass* StandInClass, const UFunction* Function)
	{
		const UClass* DeclaringClass = Function ? Function->GetOwnerClass() : nullptr;
		return StandInClass && DeclaringClass && StandInClass->IsChildOf(DeclaringClass);
	}
}

bool UCrowdyMassEntitySubsystem::HasEntityEventHandler(const UFunction* Function) const
{
	// An event the author declared an action needs no registration: this plane already knows the whole of
	// what a crowd entity does with one, which is to start it in the action slot. Asked here as well as at
	// apply time because this is what the router reads before it decodes anything, so answering false for
	// a declared action would drop it before the parameters existed.
	if (NumEventHandlersRegistered(Function) > 0 || IsDeclaredActionEvent(Function))
	{
		return true;
	}

	// With the dispatch proxy on, this plane can apply an event nothing registered a handler for, because
	// what it applies is the author's own body run on a stand-in of the entity's own class. Answered from the
	// function alone like the two above and deliberately without consulting an entity, for the reason the
	// interface gives: the router asks this while a call may still be waiting for its target to arrive.
	//
	// So this claims what the plane can do, not that a particular call will land. Neither an event aimed at the
	// wrong kind of entity nor a body no stand-in of the entity's class can host is decidable here at all:
	// both depend on the class the entity was recorded as, which is exactly what this question does not have.
	// Both are refused in ChargeEntityEventBudget, which does have it, and both are refused there before a
	// single token is spent, so claiming them here costs the entity named nothing.
	return Function != nullptr && CrowdyMassDispatchProxy::IsEnabled();
}

bool UCrowdyMassEntitySubsystem::PassesEventClassGuard(const FGuid& EntityUUID, const UFunction* Function)
{
	// The SAME check the send path makes, through the same function, and that is the point of calling it rather
	// than restating the rule: an event a peer is allowed to send must be applicable on every representation of
	// the entity it names. An event declared on a component of the entity's class is the case a restated rule
	// loses, since a class cannot say which components an instance carries; it sends, it lands on an observer
	// holding the entity as an actor, and a narrower check here would silently drop it on one holding the same
	// entity as a row.
	//
	// An AUTHORING guard, not a security boundary. The recorded class traces back to what the entity's own
	// sender said it was, so it catches an event aimed at the wrong kind of entity and nothing more; every
	// parameter stays forgeable, which is why a handler may only ever produce a visual. Refused when no class
	// has been recorded yet, like the send side: an entity nothing has identified is not yet known to be the
	// kind this event was declared for.
	const UClass* RecordedClass = GetEntityClass(EntityUUID);
	if (CrowdyMassTarget::CheckEventClass(Function, RecordedClass) == CrowdyMassTarget::EClassCheckResult::Ok)
	{
		return true;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds >= NextEventClassRefusalWarningSeconds)
	{
		NextEventClassRefusalWarningSeconds = NowSeconds + 5.0;
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Refusing '%s' for entity %s: it is declared on '%s', and this client has the entity recorded as '%s'."),
			*Function->GetName(), *EntityUUID.ToString(), *GetNameSafe(Function->GetOwnerClass()),
			*GetNameSafe(RecordedClass));
	}
	return false;
}

void UCrowdyMassEntitySubsystem::ReportUnhostableEventDeclaration(const UClass* StandInClass,
	const UFunction* Function)
{
	if (!Function)
	{
		return;
	}

	// Said once per DECLARING class rather than once per event, matching the unproxyable-class report in the
	// dispatch: what cannot be hosted is a declaration, it cannot be hosted on the next event either, and a
	// crowd would bury the one line that says which declaration to move. That also caps what an inbound flood
	// of such events can write to the log at a single line.
	//
	// Keyed by FObjectKey so a function with no owner class at all still has an entry of its own to be
	// reported under, instead of falling through the throttle and saying nothing, and so a collected class
	// cannot be recycled into a stale hit that would silence the next declaration. Add reports whether the
	// entry was already there, so asking and claiming cost one hash between them rather than one each.
	const UClass* DeclaringClass = Function->GetOwnerClass();

	bool bAlreadyReported = false;
	ReportedUnhostableEventClasses.Add(FObjectKey(DeclaringClass), &bAlreadyReported);
	if (bAlreadyReported)
	{
		return;
	}

	UE_LOG(LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' is declared on '%s'. An entity held here as a row stands in as '%s', which does not derive it, so its body cannot run on a stand-in and the event lands nothing on entities held as rows. A stand-in is constructed rather than spawned, so it carries none of the components a Blueprint adds and there is nothing on it for such a body to run on. Register a handler for this event with RegisterEventHandler, or declare it on the entity's own class. Said once for '%s'."),
		*Function->GetName(), *GetNameSafe(DeclaringClass), *GetNameSafe(StandInClass),
		*GetNameSafe(DeclaringClass));
}

bool UCrowdyMassEntitySubsystem::ChargeEntityEventBudget(const FGuid& EntityUUID, const UFunction* Function)
{
	if (!Function)
	{
		return false;
	}

	// The class refusal is deliberately FREE, and it is asked before the allowance rather than after: an event
	// aimed at the wrong kind of entity must not be able to spend that entity's tokens, or a peer streaming
	// mismatched events would starve an entity's real traffic without a single one of them ever being applied.
	if (!PassesEventClassGuard(EntityUUID, Function))
	{
		return false;
	}

	// FREE for the same reason, and it has to be asked here rather than left to the dispatch. The guard above
	// admits an event declared on a component of the entity's class, deliberately, because an observer holding
	// that entity as a real actor resolves the body onto the component; a stand-in has no component to resolve
	// onto and declines the call. Charged first and declined there, such an event would spend one of the named
	// entity's tokens on a call that was never able to apply anything, and the id of the entity to aim at is
	// the whole of what a sender needs to know to do that to somebody else's crowd row.
	//
	// The two terms before it are what makes this safe to ask this early: an event a handler is registered for,
	// or one the author declared an action, is applied by this plane WITHOUT a stand-in, so neither needs a
	// class that can host the body and neither may be refused here. Asking them first also means the entity's
	// own class is only looked up once both cheap answers have come back no.
	const bool bNeedsAStandIn = NumEventHandlersRegistered(Function) == 0 && !IsDeclaredActionEvent(Function);
	const UClass* StandInClass = bNeedsAStandIn ? GetEntityClass(EntityUUID) : nullptr;
	if (bNeedsAStandIn && !ClassCanHostEventBody(StandInClass, Function))
	{
		// Refusing for free is only half of it. Such a call never reaches the dispatch, so this is where the
		// declaration gets named: an authoring mistake that lands nothing in silence is indistinguishable
		// from an event that never arrived.
		ReportUnhostableEventDeclaration(StandInClass, Function);
		return false;
	}

	// Charged here rather than where the call is applied, so the allowance bounds the work of DECODING the
	// parameter frame as well as the work of running the handler. An inbound event still costs exactly one
	// token, because this is the only place that spends one. See CrowdyMassReceiveEventsPerSecond for the
	// ceiling.
	if (!ChargeReceiveBudget(EntityUUID))
	{
		UE_LOG(LogCrowdyMass, Verbose,
			TEXT("[CrowdyMassEntitySubsystem]: Inbound event for entity %s refused; its allowance is spent."),
			*EntityUUID.ToString());
		return false;
	}

	return true;
}

void UCrowdyMassEntitySubsystem::HandleEntityEvent(const FGuid& EntityUUID, const FGuid& SenderID,
	const UFunction* Function, const FCrowdyEventParams& Params)
{
	if (!Function)
	{
		return;
	}

	// Every parameter offset read below belongs to Function, so the frame has to be one laid out for it.
	// The caller passes both, which makes them agreeing an assumption about the caller rather than
	// something the arguments prove; a frame for another function would be read at the wrong offsets.
	if (Params.Function != Function)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Refusing an inbound '%s' for entity %s: its parameter frame was decoded for '%s'. Nothing was applied."),
			*Function->GetName(), *EntityUUID.ToString(), *GetNameSafe(Params.Function));
		return;
	}

	const TArray<FCrowdyMassEventHandlerRef>* Handlers = EventHandlersByFunction.Find(FObjectKey(Function));
	const bool bDeclaredAction = IsDeclaredActionEvent(Function);

	// The stand-in belongs in this early-out, not only further down. It applies an event nothing registered
	// a handler for and that the author never declared an action, which is exactly the case the other two
	// terms describe as nothing to do.
	if ((!Handlers || Handlers->Num() == 0) && !bDeclaredAction && !CrowdyMassDispatchProxy::IsEnabled())
	{
		return;
	}

	FMassEntityHandle Entity;
	if (!FindEntityByUUID(EntityUUID, Entity))
	{
		return;
	}

	FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager)
	{
		return;
	}

	// FindEntityByUUID validated this handle against the same entity manager before returning it, so this
	// repeats that answer rather than adding one. Kept, and kept HERE: the handlers below reach straight for
	// fragment memory, the handle is only as fresh as the last UnregisterEntity call, and one whose index has
	// been reused would land this event on whatever entity now occupies it. The guarantee they rely on is
	// asserted beside them rather than left to a helper's internals. The re-checks further down are a
	// different matter, and are not redundant: those catch an entity a handler destroyed while running.
	if (!EntityManager->IsEntityValid(Entity))
	{
		return;
	}

	// Asked again here even though the allowance step already asked it, because this is a public entry point
	// and a caller reaching it directly must get the same answer as one arriving through the router.
	if (!PassesEventClassGuard(EntityUUID, Function))
	{
		return;
	}

	// What actually ran, counted rather than predicted. The declared-action write below is what happens when
	// nothing else handled the call, and deciding that in advance from "are any handlers live" would skip
	// the walk entirely for a list holding only dead bindings, which is the one place those get dropped.
	int32 HandlersRun = 0;

	// A C++ handler takes over the action too; a Blueprint receive node only replaces the body, as it has no way to start the action itself.
	int32 IngestorsRun = 0;

	if (Handlers && Handlers->Num() > 0)
	{
		// A COPY, taken before the first handler runs. A handler can register or unregister while it is
		// running, which writes the very list this walk reads; every entry is held by reference, so one
		// removed midway stays alive until it returns. A handler registered during the walk does not run for
		// this call, which is the only answer that does not depend on where in the list it happened to land.
		TArray<FCrowdyMassEventHandlerRef, TInlineAllocator<4>> Snapshot(*Handlers);

		// SenderID is deliberately not forwarded to the handler. The relay does not bind a sender to the
		// entity an event names, so it is a claim rather than an identity, and a handler given it would
		// sooner or later key something off it. Anything that must know who did something belongs on the
		// server-authoritative plane, where it is checked.
		for (const FCrowdyMassEventHandlerRef& Entry : Snapshot)
		{
			const FCrowdyMassEventHandlerRegistration& Registration = Entry.Get();

			// The weak function is compared, not just the key: a registration whose function has gone
			// describes a signature that no longer exists, and running its handler against a frame laid out
			// for a different one would read another parameter's bytes.
			if (Registration.Function.Get() != Function)
			{
				continue;
			}

			// Re-checked before every handler rather than once for the batch. A handler bound to an object
			// runs arbitrary game code, which can destroy the entity, and the next handler would then reach
			// straight into storage the entity has already left.
			if (!EntityManager->IsEntityValid(Entity))
			{
				return;
			}

			if (Registration.IsObjectBinding())
			{
				if (DispatchObjectEventHandler(Registration, Entity, Params))
				{
					++HandlersRun;
				}
				else
				{
					RemoveEventHandlerRegistration(Registration);
				}
				continue;
			}

			if (Registration.Ingestor)
			{
				// One context per handler, never one shared across them: the context memoises fragment
				// pointers, which are only valid while the entity stays in the archetype they were resolved
				// against.
				const FCrowdyMassViewStateContext Context{*EntityManager, Entity};
				Registration.Ingestor(Params, Context);
				++HandlersRun;
				++IngestorsRun;
			}
		}
	}

	// Nothing the game registered took this call, so this plane runs the event's OWN body on a per-class
	// stand-in. A FALLBACK rather than something that coexists with a registered handler, for the same
	// reason the action write below is one: a game that registered a handler has already said what this
	// event means for an entity held as a row, and running the author's body as well would do that work
	// twice for one inbound event.
	//
	// It does NOT count toward HandlersRun. The declared-action write below is a different plane of the
	// same event, the animation rather than the world-space presentation, and both belong to an entity
	// whose owner ran one body that did both.
	//
	// Whether the body actually ran, not whether it was attempted. The stand-in declines a call it cannot
	// host, and every way it declines says so itself, so the report further down must not also claim the body
	// ran for one of those.
	bool bBodyRan = false;
	if (HandlersRun == 0)
	{
		bBodyRan = DispatchEventOnProxy(EntityUUID, Entity, Function, Params);

		// The body above is arbitrary game code and can destroy the entity, and everything below reaches
		// straight for fragment memory.
		if (!EntityManager->IsEntityValid(Entity))
		{
			return;
		}
	}

	if (IngestorsRun > 0 || !bDeclaredAction)
	{
		// The one way an event can arrive, pass every guard, run its own body on the stand-in, and still
		// animate nothing. From the outside it is indistinguishable from an event that never arrived, and
		// the body running is what makes it look as though everything worked, so it is said rather than
		// left to be deduced. Once per function: a marker either travelled to this process or it did not.
		//
		// Only when the body really ran. A call the stand-in declined has already reported its own reason,
		// and this line would tell the reader the opposite of what happened.
		if (HandlersRun == 0 && bBodyRan && !ReportedEventsNotDeclaredActions.Contains(FObjectKey(Function)))
		{
			ReportedEventsNotDeclaredActions.Add(FObjectKey(Function));
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' arrived for entity %s and started no action, because it is not marked as a one-shot action in this process. Its body ran. Tick Is A One-Shot Action on the event, and check crowdy.rpc.dumpfn reports action=1 here as well as on the sender."),
				*Function->GetName(), *EntityUUID.ToString());
		}

		return;
	}

	// No C++ handler took this call, and the author declared it an action, so this plane
	// applies the whole of what a crowd entity does with one. A fallback rather than a coexisting handler:
	// two writers of one action slot would bump the retrigger serial twice for a single inbound event, which
	// downstream is indistinguishable from the action having been started again.
	//
	// Said once per function rather than per inbound event, and said at all rather than left to be noticed:
	// an event declared an action that carries no id under the name actions are read by lands nothing,
	// forever, and looks from the outside exactly like an event that never arrived.
	if (!CrowdyMassDeclaresActionId(Function) && !ReportedActionEventsMissingId.Contains(FObjectKey(Function)))
	{
		ReportedActionEventsMissingId.Add(FObjectKey(Function));

		// The same sentence the Blueprint compile pass gets, rather than a second one written here. Three
		// different signatures reach this line (no such parameter, one of the wrong type, one declared as an
		// output) and only the first is fixed by renaming anything, which is what this line used to say for
		// all three.
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' is marked CrowdyAction but %s Register a handler of your own for this event if that is not what you meant."),
			*Function->GetName(), *CrowdyMassDescribeActionEventProblem(Function));
	}

	// Re-checked because a handler that ran above may have destroyed the entity, and this reaches straight
	// for fragment memory.
	if (!EntityManager->IsEntityValid(Entity))
	{
		return;
	}

	const FCrowdyMassViewStateContext Context{*EntityManager, Entity};
	DefaultActionIngestor()(Params, Context);
}

bool UCrowdyMassEntitySubsystem::DispatchObjectEventHandler(const FCrowdyMassEventHandlerRegistration& Registration,
	const FMassEntityHandle Entity, const FCrowdyEventParams& Params)
{
	UObject* HandlerObject = Registration.BoundObject.Get();
	UFunction* Handler = Registration.BoundFunction.Get();

	// Every FProperty below is owned by one of the two functions this binding names, and a parameter list
	// cannot outlive the function that owns it, so both are checked before any of them is read. A false
	// here is what tells the caller the binding is dead and should be dropped: a destroyed object, or a
	// Blueprint recompiled out from under it.
	if (!::IsValid(HandlerObject) || !Handler || !Registration.Function.IsValid() || !Registration.BoundTargetParam)
	{
		return false;
	}

	const int32 FrameSize = FMath::Max<int32>(Handler->ParmsSize, 1);
	void* Frame = FMemory_Alloca(FrameSize);
	FMemory::Memzero(Frame, FrameSize);

	// Every parameter is constructed, including the ones the call carries nothing for: those are what the
	// handler sees as its own defaults, and ProcessEvent reads all of them either way.
	for (TFieldIterator<FProperty> It(Handler); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Frame);
	}

	FCrowdyMassEntityTarget Target;
	Target.Entity = Entity;
	Target.World = GetWorld();
	Registration.BoundTargetParam->CopyCompleteValue(
		Registration.BoundTargetParam->ContainerPtrToValuePtr<void>(Frame), &Target);

	// Params.Frame is null for an event that declares no parameters, which is also the case where the table
	// is empty. Checked all the same rather than left to follow from that, because the two come from
	// different places and only one of them is this code's to guarantee.
	if (Params.Frame)
	{
		for (const FCrowdyMassEventParamCopy& Copy : Registration.BoundParamCopies)
		{
			Copy.DestParam->CopyCompleteValue(
				Copy.DestParam->ContainerPtrToValuePtr<void>(Frame),
				Copy.SourceParam->ContainerPtrToValuePtr<void>(Params.Frame));
		}
	}

	// Nothing may return between the construction above and the teardown below, or the values constructed
	// into the frame are leaked. The handler runs arbitrary game code, which is why the frame is torn down
	// from this function rather than at the end of the loop that walks the handlers.
	HandlerObject->ProcessEvent(Handler, Frame);

	for (TFieldIterator<FProperty> It(Handler); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Frame);
	}

	return true;
}

FCrowdyMassDispatchProxyEntry* UCrowdyMassEntitySubsystem::GetOrCreateDispatchProxyEntry(const UClass* EntityClass)
{
	if (!EntityClass)
	{
		return nullptr;
	}

	if (FCrowdyMassDispatchProxyEntry* Existing = DispatchProxyByClass.Find(EntityClass))
	{
		// Checked rather than trusted. A proxy is held strongly by this map, so it does not go on its own,
		// but a Blueprint recompile can mark one pending kill while the map still names it and running a
		// body on that is running it on an object the engine has finished with.
		if (::IsValid(Existing->Proxy))
		{
			return Existing;
		}

		DispatchProxyByClass.Remove(EntityClass);
	}

	AActor* Proxy = CrowdyMassDispatchProxy::Construct(GetWorld(), const_cast<UClass*>(EntityClass));
	if (!Proxy)
	{
		return nullptr;
	}

	FCrowdyMassDispatchProxyEntry& Entry = DispatchProxyByClass.Add(EntityClass);
	Entry.Proxy = Proxy;

	// Worked out against the proxy as built, which is the state every reset restores it to.
	CrowdyMassDispatchProxy::BuildResetPlan(Proxy, Entry.ResetProperties, Entry.SubobjectProperties, Entry.SubobjectValues);
	return &Entry;
}

AActor* UCrowdyMassEntitySubsystem::GetOrCreateDispatchProxy(const UClass* EntityClass)
{
	const FCrowdyMassDispatchProxyEntry* Entry = GetOrCreateDispatchProxyEntry(EntityClass);
	return Entry ? Entry->Proxy.Get() : nullptr;
}

bool UCrowdyMassEntitySubsystem::DispatchEventOnProxy(const FGuid& EntityUUID, const FMassEntityHandle Entity,
	const UFunction* Function, const FCrowdyEventParams& Params)
{
	// Every exit below says so under crowdy.rpc.trace. This is the last step of the receive path and the
	// only one that used to be silent, which left "the body ran", "no proxy was built" and "the world would
	// not run a body" looking identical from the log: one recv line and nothing after it.
	const bool bTrace = FCrowdyRPC::IsRpcTraceEnabled();

	if (!CrowdyMassDispatchProxy::IsEnabled() || !Function)
	{
		UE_CLOG(bTrace, LogCrowdyMass, Log,
			TEXT("[CrowdyMassProxy] entity=%s skipped: the stand-in dispatch is switched off (crowdy.mass.event.proxy)."),
			*EntityUUID.ToString());
		return false;
	}

	// The recorded class, never anything off the wire. The class guard upstream has compared the function
	// against it, but that comparison does NOT establish that this class declares the function: it admits,
	// deliberately, a function declared on a component or on a UCrowdyMassEntity that the recorded class does
	// not derive, because an observer holding the same entity as a real actor resolves the body onto whichever
	// object declares it. So the only thing known here is that the event was allowed to be aimed at an entity
	// of this class. Whether the stand-in can host the body is decided below.
	const UClass* EntityClass = GetEntityClass(EntityUUID);
	FCrowdyMassDispatchProxyEntry* ProxyEntry = GetOrCreateDispatchProxyEntry(EntityClass);
	AActor* Proxy = ProxyEntry ? ProxyEntry->Proxy.Get() : nullptr;
	if (!Proxy)
	{
		// A null class exits here too, and the warning below cannot cover it, having no class to name it
		// under. The trace line is what covers it instead: the guard upstream refuses an unrecorded class
		// before this is reached, so this is only ever a class that exists and cannot have a proxy, but a
		// silent exit on a path with no other output is how a live gate ends up with nothing to read.
		UE_CLOG(bTrace, LogCrowdyMass, Log,
			TEXT("[CrowdyMassProxy] %s entity=%s NO PROXY for class '%s'."),
			*Function->GetName(), *EntityUUID.ToString(), *GetNameSafe(EntityClass));

		// A class is what the report below is keyed by, so with none there is nothing to key it under and
		// nothing to name in it.
		if (!EntityClass)
		{
			return false;
		}

		// Said once per class rather than once per event. A class that cannot supply a proxy cannot supply
		// one for the next event either, and a crowd of them would bury the one line that says which class.
		// Add reports whether the entry was already there, so asking and claiming cost one hash between them
		// rather than one each.
		bool bAlreadyReported = false;
		ReportedUnproxyableClasses.Add(EntityClass, &bAlreadyReported);
		if (!bAlreadyReported)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: No dispatch proxy can be built for '%s', so an event's own body does not run for entities of it here. An abstract class cannot have one; nor can any class before this world has a persistent level."),
				*GetNameSafe(EntityClass));
		}
		return false;
	}

	// ProcessEvent at the bottom runs the body with this proxy as its self, so the function has to be one the
	// proxy's own class declares. Read off the object rather than off the recorded class, because the object
	// is what the property offsets in the body will be applied to.
	//
	// A BACKSTOP rather than the first place this is decided: the allowance step declines such a call before
	// it costs the entity a token, through the same predicate, so a call reaching here having failed it came
	// from a caller that drove this path directly. Kept all the same, because running a body against an object
	// of the wrong class would read and write the declaring class's property offsets in this actor's memory,
	// where every one of them names a different variable, and a public entry point may not depend on its
	// callers to prevent that.
	//
	// Refused before the reset and the transform projection below, so a call that cannot run leaves the shared
	// stand-in exactly as the previous entity's dispatch left it.
	const UClass* ProxyClass = Proxy->GetClass();
	if (!ClassCanHostEventBody(ProxyClass, Function))
	{
		UE_CLOG(bTrace, LogCrowdyMass, Log,
			TEXT("[CrowdyMassProxy] %s entity=%s REFUSED: declared on '%s', which the stand-in class '%s' does not derive."),
			*Function->GetName(), *EntityUUID.ToString(), *GetNameSafe(Function->GetOwnerClass()),
			*GetNameSafe(ProxyClass));

		ReportUnhostableEventDeclaration(ProxyClass, Function);
		return false;
	}

	// Before the body rather than after it, so a body that wrote a member cannot leak into the next entity's
	// dispatch even if that dispatch takes a different path out of here. Members come back from the class
	// default object and the proxy's own components are restored: see ResetToClassDefaults for why the two
	// halves cannot be one.
	CrowdyMassDispatchProxy::ResetToClassDefaults(Proxy, ProxyEntry->ResetProperties,
		ProxyEntry->SubobjectProperties, ProxyEntry->SubobjectValues);

	// The entity's position, so a body doing world-space work at its own location does it where the entity
	// is rather than at the origin. Nothing else is projected: a class that needs more of itself than a
	// transform is asking for state, and state for a crowd entity lives in fragments.
	bool bProjectedTransform = false;
	if (const FMassEntityManager* EntityManager = GetMassEntityManager())
	{
		if (const FTransformFragment* TransformFragment =
			EntityManager->GetFragmentDataPtr<FTransformFragment>(Entity))
		{
			bProjectedTransform = Proxy->SetActorTransform(TransformFragment->GetTransform());
		}
	}

	// AActor::ProcessEvent runs a body only where the actor's world reports its actors initialized OR script
	// execution is allowed outside a played level, and it does nothing at all otherwise WITHOUT SAYING SO.
	// That condition is a property of the world rather than of the proxy, so it is read here and reported
	// beside the dispatch: a body that never runs and a body that ran and did nothing visible are otherwise
	// the same reading.
	//
	// The second term is the engine's own, mirrored rather than approximated. Reading only "are the actors
	// initialized" would report that no body ever runs wherever script execution has been allowed by hand,
	// which is every editor context, and a diagnostic that says nothing runs while everything does is worse
	// than none.
#if WITH_EDITOR
	static const FName CallInEditorMeta("CallInEditor");
	const bool bScriptExecutionAllowed = GAllowActorScriptExecutionInEditor
		|| Function->GetBoolMetaData(CallInEditorMeta)
		|| Function->HasAnyFunctionFlags(FUNC_UbergraphFunction);
#else
	const bool bScriptExecutionAllowed = GAllowActorScriptExecutionInEditor;
#endif

	const UWorld* ProxyWorld = Proxy->GetWorld();
	const bool bWorldWillRunABody =
		ProxyWorld && (ProxyWorld->AreActorsInitialized() || bScriptExecutionAllowed);

	UE_CLOG(bTrace, LogCrowdyMass, Log,
		TEXT("[CrowdyMassProxy] %s entity=%s proxy=%s at=%s projected=%d worldRunsBodies=%d"),
		*Function->GetName(), *EntityUUID.ToString(), *GetNameSafe(Proxy),
		*Proxy->GetActorLocation().ToCompactString(), bProjectedTransform ? 1 : 0,
		bWorldWillRunABody ? 1 : 0);

	// Said always, not only under trace, and once per world: a world that will not run a body makes every
	// event authored for a crowd entity do nothing here, silently, for the whole session.
	if (!bWorldWillRunABody && !bReportedProxyWorldWillNotRunBodies)
	{
		bReportedProxyWorldWillNotRunBodies = true;
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: This world neither reports its actors initialized nor allows actor script to run, so an inbound event's own body cannot run on a stand-in here and every such event lands nothing. Reported once for this world."));
	}

	// The same scope FCrowdyRPC::ApplyCall holds around a received call, and holding it is NOT optional. A
	// Blueprint replicated event carries a dispatch gate spliced in at the top of its body; without the
	// scope naming this object and this function, the gate reads the call as freshly originated and routes
	// it over the network again, from every observer that received it.
	const FCrowdyRPC::FScopedReplay ReplayScope(Proxy, const_cast<UFunction*>(Function));

	// The frame the router decoded for this exact function, handed straight over. This is the whole economy
	// of the shape: the function being called IS the author's own, so there is no second signature to bridge
	// and no parameter to copy by name. ProcessEvent does not write to an input slot, and this function
	// declares no output that could travel the wire.
	//
	// An event declaring no parameters is decoded to a null frame, and ProcessEvent reads a frame pointer
	// either way, so it is handed a byte of zeroed stack rather than null.
	uint8 EmptyFrame[1] = { 0 };
	void* Frame = Params.Frame ? const_cast<void*>(Params.Frame) : static_cast<void*>(EmptyFrame);

	// Whether the call below will run a body at all, which is the whole of what this function reports. The
	// remaining terms are the rest of ProcessEvent's own condition: a function with neither a native
	// implementation nor any compiled script has no body to run, a class default object runs one in any world,
	// and nothing runs one while a collection is in progress.
	//
	// Read rather than used to skip the call, so a term that ever drifts from the engine's can only misreport,
	// never suppress a body that would have run. Reporting it truthfully is what stops the caller announcing
	// that an event's body ran in a world that executed nothing, which reads from the log as the event having
	// worked and is the one failure this whole path is hardest to see.
	const bool bBodyWillRun = (Function->HasAnyFunctionFlags(FUNC_Native) || Function->Script.Num() > 0)
		&& (bWorldWillRunABody || Proxy->HasAnyFlags(RF_ClassDefaultObject))
		&& !IsGarbageCollecting();

	Proxy->ProcessEvent(const_cast<UFunction*>(Function), Frame);
	return bBodyWillRun;
}

void UCrowdyMassEntitySubsystem::ApplyDecodedState(const FGuid& EntityUUID, const FCrowdyRepLayout& Layout,
	const void* DecodedContainer, const TConstArrayView<int32> DeliveredIndices)
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(EntityUUID, Entity))
	{
		return;
	}

	FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager)
	{
		return;
	}

	// Repeats what FindEntityByUUID already answered, kept where the reads are: everything below reaches
	// straight for fragment memory, and a reused handle index would land this delta on another entity.
	if (!EntityManager->IsEntityValid(Entity))
	{
		return;
	}

	// The class the buffer is shaped like, and therefore the class every property offset below is valid
	// against. Read off the layout rather than re-queried, which ties the resolution below to the one class
	// this delta was actually decoded for.
	const UClass* EntityClass = Layout.OwnerClass.Get();
	if (!EntityClass)
	{
		return;
	}

	// That it is also the class this subsystem named for the id is an assumption about the caller, not
	// something the arguments prove, so it is checked rather than trusted. Every offset below is applied to
	// memory shaped like EntityClass, so a layout for some other class would read this entity's bytes at the
	// wrong offsets and write the results into its fragments: a silent value corruption rather than a miss.
	// The check costs one map lookup per delta and turns a cross-file assumption into a local guarantee.
	const TWeakObjectPtr<const UClass>* ExpectedClass = StateEntityClassByUUID.Find(EntityUUID);
	if (!ExpectedClass || ExpectedClass->Get() != EntityClass)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Refusing a decoded state delta for entity %s: it was decoded against '%s', but this entity was activated as '%s'. Nothing was written."),
			*EntityUUID.ToString(), *EntityClass->GetName(),
			*GetNameSafe(ExpectedClass ? ExpectedClass->Get() : nullptr));
		return;
	}

	const FCrowdyMassStateResolvedClassConstRef Resolved = ResolveStateFragmentClass(*EntityClass, Layout);

	// One context for the whole delta rather than one per field, so fields landing on the same fragment
	// share the lookup that found it: see FCrowdyMassViewStateContext::GetFragment. The class travels with
	// it because an arriving value's meaning is authored per class: the pose field turns a state value
	// into a row id through the animation set this class renders through.
	FCrowdyMassViewStateContext Context{*EntityManager, Entity, EntityClass};

	// Driven entirely by DeliveredIndices: a layout slot this delta did not carry holds whatever the
	// previous entity's delta left in the shared scratch buffer, not a default and not this entity's own
	// previous value. Reading outside DeliveredIndices would write another entity's state into this one.
	for (const int32 LayoutIndex : DeliveredIndices)
	{
		if (!Resolved->RegistrationIndexByLayoutIndex.IsValidIndex(LayoutIndex))
		{
			continue;
		}

		const int32 RegistrationIndex = Resolved->RegistrationIndexByLayoutIndex[LayoutIndex];
		if (RegistrationIndex == INDEX_NONE)
		{
			// No registered field claims this layout slot. Not an error: a class may carry CrowdyState
			// properties CrowdyMass has no ingestor for at all. Silent HERE on purpose, and not silent
			// overall: this runs per property per delta per entity, so the property is named once by
			// ResolveStateFragmentClass, which is where the INDEX_NONE below was decided.
			continue;
		}

		const FProperty* Property = Layout.Properties[LayoutIndex].Property;
		if (!Property)
		{
			continue;
		}

		const FCrowdyMassStateFieldRegistration& Registration = *StateFieldRegistrations[RegistrationIndex];
		const void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(DecodedContainer);
		Registration.Ingestor(PropertyValuePtr, Context);
	}
}

UCrowdyMassEntity* UCrowdyMassEntitySubsystem::GetOrCreateAvatar(const FGuid& UUID, const UClass* RequiredClass)
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(UUID, Entity))
	{
		// The tracker is the only thing that can authorise minting. Ids reaching here come off the
		// network, and creating an object for one that names no tracked entity would let a peer fill this
		// map with whatever it liked.
		return nullptr;
	}

	if (FCrowdyMassAvatarEntry* Existing = Avatars.Find(UUID))
	{
		UCrowdyMassEntity* Cached = Existing->Avatar;

		// The handle is compared, not just the id. An entity can be destroyed and another spawned under
		// the same id, and a cached avatar would then point at the entity that left rather than the one
		// that arrived.
		if (::IsValid(Cached) && Cached->GetHandle() == Entity)
		{
			if (RequiredClass && !Cached->IsA(RequiredClass))
			{
				return nullptr;
			}

			Existing->LastTouch = ++AvatarTouchCounter;
			return Cached;
		}

		// The id has been reused by a different entity, so the avatar standing for the one that left goes
		// with its registry record. Dropping the record first is what lets the replacement enrol at all:
		// the registry refuses a second live participant on one id, so a stale avatar left holding it
		// would leave the entity that has just arrived unable to enrol for the rest of its life.
		Avatars.Remove(UUID);
		UnregisterAvatarParticipant(UUID, Cached);
	}

	// An entity drawn again inside the reclaim grace window keeps the avatar it had, and with it its
	// registry record.
	if (UCrowdyMassEntity* Reclaimed = ReclaimAvatarFromGrace(UUID, Entity))
	{
		if (!RequiredClass || Reclaimed->IsA(RequiredClass))
		{
			return Reclaimed;
		}

		// The caller needs a class this avatar is not, so keeping it would refuse every such call for the
		// rest of the window while a fresh mint would have answered.
		Avatars.Remove(UUID);
		UnregisterAvatarParticipant(UUID, Reclaimed);
	}

	UClass* SpawnClass = GetEffectiveAvatarClass();
	if (!SpawnClass)
	{
		return nullptr;
	}

	if (RequiredClass && !SpawnClass->IsChildOf(RequiredClass))
	{
		UE_CLOG(!bWarnedAvatarClassMismatch, LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Avatar class '%s' does not derive from '%s', which declares an event addressed to a Mass entity. Those events cannot be delivered until the avatar class is changed (logged once)."),
			*GetNameSafe(SpawnClass), *GetNameSafe(RequiredClass));
		bWarnedAvatarClassMismatch = true;
		return nullptr;
	}

	// The capacity follows the tracker, so in a real world this loop does not run: an avatar exists only
	// for an id the tracker holds and is released when that id goes, so the cache cannot reach the count
	// it is compared against. It stays because the comparison is what makes that true rather than merely
	// believed, and because a capacity floor still applies to a world with almost no entities.
	while (Avatars.Num() >= GetAvatarCacheCapacity())
	{
		if (!EvictOldestAvatar())
		{
			break;
		}
	}

	UCrowdyMassEntity* Avatar = NewObject<UCrowdyMassEntity>(this, SpawnClass);
	Avatar->BindToEntity(this, Entity, UUID);

	FCrowdyMassAvatarEntry& Entry = Avatars.Add(UUID);
	Entry.Avatar = Avatar;
	Entry.LastTouch = ++AvatarTouchCounter;
	EvictedAvatarUUIDs.Remove(UUID);

	// Every map above is settled first, and the reference into Avatars is finished with, because enrolling
	// broadcasts a registration that other subsystems handle synchronously. One of them re-enters this
	// object to activate an entity whose class had not resolved, so a caller reaching this line mid-insert
	// would see half-written bookkeeping, and a reference held into a map that gains an entry is dangling.
	RegisterAvatarParticipant(UUID, Avatar);

	return Avatar;
}

int32 UCrowdyMassEntitySubsystem::GetAvatarCacheCapacity() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AvatarCacheCapacityForTest > 0)
	{
		return AvatarCacheCapacityForTest;
	}
#endif

	return FMath::Max(CrowdyMassMinCachedAvatars, GetTrackedRemoteEntityCount());
}

UCrowdyEntitySubsystem* UCrowdyMassEntitySubsystem::ResolveEntitySubsystem() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyEntitySubsystem* Injected = EntitySubsystemForTest.Get())
	{
		return Injected;
	}
#endif

	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyMassEntitySubsystem::SetEntitySubsystemForTest(UCrowdyEntitySubsystem* InEntities)
{
	EntitySubsystemForTest = InEntities;
}
#endif

void UCrowdyMassEntitySubsystem::RegisterAvatarParticipant(const FGuid& UUID, UCrowdyMassEntity* Avatar)
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!Entities || !::IsValid(Avatar) || !UUID.IsValid())
	{
		return;
	}

	FCrowdyEntityRecord Record;

	// The entity's own id, and nothing derived from the avatar. This is the whole reason the record is
	// filled here rather than through RegisterParticipant, which mints an id from the participant's class
	// path: every avatar in a crowd is one class, so that id would be one id for the entire crowd. The id
	// used here is the one the rest of the system already addresses this entity by, which is what makes a
	// call aimed at a victim reach the victim and not somebody else.
	Record.NetID = UUID;

	// Read-only by default, and that default is a security position rather than a placeholder: an avatar only
	// ever stands for another client's entity, and guessing wrong toward read-only costs an authority this
	// client was probably not entitled to claim anyway.
	Record.Role = ECrowdyRole::RemoteProxy;

	if (const FCrowdyEntityRecord* Existing = Entities->FindRecord(UUID))
	{
		UObject* Held = Existing->GetParticipant();
		if (::IsValid(Held) && Held != Avatar)
		{
			// Something live already answers for this id: an actor carrying an entity component, or a
			// second registration that derived the same id. Taking the slot would unbind it, and this id is
			// what damage addresses a victim by, so a takeover lands attacks on the wrong participant.
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: Entity %s is already registered to '%s', so its Mass avatar was not enrolled. Two participants deriving one id is the usual cause."),
				*UUID.ToString(), *GetNameSafe(Held));
			return;
		}

		if (Existing->Role == ECrowdyRole::Owner)
		{
			// The id belongs to something this client simulates and sends for, which has died rather than
			// gone: its own registration is on its way back. Claiming the slot now would make that
			// registration collide with a live participant.
			UE_LOG(LogCrowdyMass, Verbose,
				TEXT("[CrowdyMassEntitySubsystem]: Entity %s is locally owned, so its Mass avatar was not enrolled; the owning participant keeps the record."),
				*UUID.ToString());
			return;
		}

		// An identity something else supplied outranks the one assumed above for who the entity is, so the
		// owner and the class are taken from it. The ROLE is not, and that exception is the point rather
		// than an oversight. Role decides whether this participant is authoritative for the entity, and an
		// avatar must never be: adopting HostOwned here would undo the RemoteProxy default set above. On a
		// host client a HostOwned participant is also one the state replicator will try to make a
		// CrowdyState SENDER under the crowd entity's own id; nothing but the shipped avatar class having
		// no replicated properties stops that today.
		Record.OwnerID = Existing->OwnerID;
		Record.ClassID = Existing->ClassID;
	}
	else
	{
		// Who the entity says owns it. Invalid when nothing has told this client, which is honest rather
		// than a guess: a RemoteProxy record with no owner id claims only "another client simulates this",
		// which is all that is known.
		const FGuid OwnerID = Avatar->GetEventOwnerID();
		if (OwnerID.IsValid() && OwnerID == Entities->GetLocalPlayerID())
		{
			// A proxy of the local player contradicts itself, and the contradiction is not harmless: a
			// record naming this client as owner is authoritative, so the avatar would claim what the local
			// player's own participant is supposed to own.
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: Mass entity %s names the local player as its owner, so it was not enrolled as a remote proxy. A crowd entity stands for another client; check what set this entity's owner id."),
				*UUID.ToString());
			return;
		}

		Record.OwnerID = OwnerID;

		// The class this entity's replicated state is decoded as, which is the same fact a spawn event's
		// ClassID carries. Zero when nothing has named one, matching a record that arrived without it.
		if (const UClass* StateClass = GetEntityClass(UUID))
		{
			Record.ClassID = UCrowdyClassRegistry::Get()->GetID(StateClass);
		}
	}

	Record.Participant = Avatar;
	Entities->RegisterEntity(Record);
}

void UCrowdyMassEntitySubsystem::RecordStateEntityClass(const FGuid& UUID, const UClass* Class)
{
	if (!Class)
	{
		return;
	}

	const TWeakObjectPtr<const UClass>* Recorded = StateEntityClassByUUID.Find(UUID);
	if (Recorded && Recorded->Get() == Class)
	{
		return;
	}
	StateEntityClassByUUID.Add(UUID, Class);
	OnRowClassRecorded.Broadcast(UUID, Class);
}

void UCrowdyMassEntitySubsystem::UnregisterAvatarParticipant(const FGuid& UUID, const UCrowdyMassEntity* Avatar)
{
	UCrowdyEntitySubsystem* Entities = ResolveEntitySubsystem();
	if (!Entities || !Avatar)
	{
		return;
	}

	// Compared against the record's current participant, never removed by id alone. The two orderings a
	// respawn produces both arrive here: an entity torn down before its replacement registers, and one
	// whose teardown lands after. Removing by id would make the second case unbind the replacement that
	// has just claimed the id, which is a player who died once and can never be hit again. Comparing
	// identity makes a late teardown a no-op instead.
	//
	// The participant is read even while it is being destroyed, because that is exactly when this runs:
	// an avatar dropped from the cache is unreachable and pending collection, and a weak pointer that
	// refused to resolve it would leave its record behind naming a dead object.
	const FCrowdyEntityRecord* Record = Entities->FindRecord(UUID);
	const UObject* Held = Record ? Record->Participant.Get(/*bEvenIfPendingKill*/ true) : nullptr;
	if (Held != static_cast<const UObject*>(Avatar))
	{
		return;
	}

	Entities->UnregisterEntity(UUID);
}

void UCrowdyMassEntitySubsystem::UnregisterAllAvatarParticipants()
{
	// Snapshotted rather than walked in place. Each removal broadcasts, and a handler is free to
	// unregister further entities of its own, so the map this loop reads must not be the one anything
	// downstream can reach. These paths run on a class change, a Live Coding reload and teardown, never
	// per entity, so the copy costs nothing that matters.
	TArray<TPair<FGuid, TObjectPtr<UCrowdyMassEntity>>> Snapshot;
	Snapshot.Reserve(Avatars.Num());
	for (const TPair<FGuid, FCrowdyMassAvatarEntry>& Pair : Avatars)
	{
		Snapshot.Emplace(Pair.Key, Pair.Value.Avatar);
	}

	for (const TPair<FGuid, TObjectPtr<UCrowdyMassEntity>>& Pair : Snapshot)
	{
		UnregisterAvatarParticipant(Pair.Key, Pair.Value);
	}
}

ECrowdyMassAvatarState UCrowdyMassEntitySubsystem::GetAvatarState(const FGuid& UUID) const
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(UUID, Entity))
	{
		return ECrowdyMassAvatarState::NoEntity;
	}

	const FCrowdyMassAvatarEntry* Entry = Avatars.Find(UUID);
	if (Entry && ::IsValid(Entry->Avatar) && Entry->Avatar->GetHandle() == Entity)
	{
		return ECrowdyMassAvatarState::Present;
	}

	return EvictedAvatarUUIDs.Contains(UUID)
		? ECrowdyMassAvatarState::Evicted
		: ECrowdyMassAvatarState::NotMinted;
}

void UCrowdyMassEntitySubsystem::SetAvatarClass(TSubclassOf<UCrowdyMassEntity> InAvatarClass)
{
	if (!InAvatarClass)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Refusing a null avatar class; keeping '%s'."),
			*GetNameSafe(GetEffectiveAvatarClass()));
		return;
	}

	if (AvatarClass == InAvatarClass)
	{
		return;
	}

	AvatarClass = InAvatarClass;

	// Everything already minted is of the previous class, and an entity holding one would keep receiving
	// through it while its neighbours received through the new one.
	ReleaseAllAvatars(TEXT("the avatar class changed"));
}

UClass* UCrowdyMassEntitySubsystem::GetEffectiveAvatarClass() const
{
	return AvatarClass ? AvatarClass.Get() : UCrowdyMassCharacterAvatar::StaticClass();
}

bool UCrowdyMassEntitySubsystem::EvictOldestAvatar()
{
	const FGuid* OldestUUID = nullptr;
	int64 OldestTouch = MAX_int64;

	// A linear scan, but only on the eviction path: the cache is small, eviction only happens once the
	// cap is reached, and keeping a heap in order would cost something on every lookup instead.
	for (const TPair<FGuid, FCrowdyMassAvatarEntry>& Pair : Avatars)
	{
		if (Pair.Value.LastTouch < OldestTouch)
		{
			OldestTouch = Pair.Value.LastTouch;
			OldestUUID = &Pair.Key;
		}
	}

	if (!OldestUUID)
	{
		return false;
	}

	const FGuid EvictedUUID = *OldestUUID;

	// Before the entry is dropped, because dropping it releases the last reference to the avatar and the
	// registry holds only a weak one. A record left behind would name an object on its way to collection:
	// the entity would still look enrolled while nothing could resolve it, with no way back.
	if (const FCrowdyMassAvatarEntry* Entry = Avatars.Find(EvictedUUID))
	{
		UnregisterAvatarParticipant(EvictedUUID, Entry->Avatar);
	}

	Avatars.Remove(EvictedUUID);
	EvictedAvatarUUIDs.Add(EvictedUUID);
	ReportAvatarEviction(EvictedUUID);
	return true;
}

void UCrowdyMassEntitySubsystem::ReleaseAvatarState(const FGuid& UUID)
{
	SweepExpiredAvatarGrace();

	UCrowdyMassEntity* Held = nullptr;
	if (const FCrowdyMassAvatarEntry* Entry = Avatars.Find(UUID))
	{
		Held = Entry->Avatar;
	}

	Avatars.Remove(UUID);
	EvictedAvatarUUIDs.Remove(UUID);
	ReceiveBudgets.Remove(UUID);

	if (!::IsValid(Held))
	{
		return;
	}

	if (AvatarReclaimGraceSeconds <= 0.0f)
	{
		// No window is no window: the record goes with the avatar, exactly as it did before there was one.
		UnregisterAvatarParticipant(UUID, Held);
		return;
	}

	// The record is deliberately left in place, so an entity drawn again inside the window reuses it.
	FCrowdyMassAvatarGraceEntry& Grace = AvatarReclaimGrace.FindOrAdd(UUID);
	if (Grace.Avatar && Grace.Avatar != Held)
	{
		// Two avatars for one id cannot both hold its record, and the one just released is the one the
		// record names.
		EndAvatarGrace(UUID, Grace);
	}
	Grace.Avatar = Held;
	Grace.ExpiresAtSeconds = FPlatformTime::Seconds() + static_cast<double>(AvatarReclaimGraceSeconds);

	// The window has to close by itself. Every other path into the sweep needs another row to leave the crowd, so
	// the last row to go would keep its record for the rest of the session with nothing left to trigger the
	// sweep.
	ArmAvatarGraceSweep(Grace.ExpiresAtSeconds);
}

void UCrowdyMassEntitySubsystem::ArmAvatarGraceSweep(const double DeadlineSeconds)
{
	UWorld* World = GetWorld();
	if (!World || AvatarReclaimGrace.IsEmpty())
	{
		// No world is no timer manager. The sweep still runs from ReleaseAvatarState and an entry asked for is
		// still expired exactly when it is asked for, which is the whole of the behaviour a headless test sees.
		ArmedGraceDeadlineSeconds = TNumericLimits<double>::Max();
		return;
	}

	// A departure whose deadline is no sooner than the armed one rides the existing timer. Re-deriving the
	// earliest deadline per departure made a relevance cull of the whole crowd quadratic, and re-armed the
	// timer once per row on the frame the crowd is already respawning.
	if (DeadlineSeconds >= ArmedGraceDeadlineSeconds && World->GetTimerManager().IsTimerActive(AvatarGraceSweepTimer))
	{
		return;
	}

	// Aimed at the SOONEST deadline rather than a whole window ahead, so an entry cannot outlive its deadline by
	// most of another window.
	ArmedGraceDeadlineSeconds = DeadlineSeconds;
	const float Delay = FMath::Max(static_cast<float>(DeadlineSeconds - FPlatformTime::Seconds()), KINDA_SMALL_NUMBER);
	World->GetTimerManager().SetTimer(AvatarGraceSweepTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			SweepExpiredAvatarGrace(/*bForce*/ true);
		}),
		Delay, false);
}

UCrowdyMassEntity* UCrowdyMassEntitySubsystem::ReclaimAvatarFromGrace(const FGuid& UUID, const FMassEntityHandle Entity)
{
	FCrowdyMassAvatarGraceEntry* Grace = AvatarReclaimGrace.Find(UUID);
	if (!Grace)
	{
		return nullptr;
	}

	UCrowdyMassEntity* Avatar = Grace->Avatar;
	const bool bExpired = FPlatformTime::Seconds() >= Grace->ExpiresAtSeconds;
	if (bExpired || !::IsValid(Avatar))
	{
		EndAvatarGrace(UUID, *Grace);
		AvatarReclaimGrace.Remove(UUID);
		return nullptr;
	}

	AvatarReclaimGrace.Remove(UUID);

	// The entity is a new one under the same id, so the avatar is pointed at the handle it is drawn as now.
	// Its registry record is keyed on the id and never moved, which is why nothing here enrolls it again.
	Avatar->BindToEntity(this, Entity, UUID);

	FCrowdyMassAvatarEntry& Entry = Avatars.Add(UUID);
	Entry.Avatar = Avatar;
	Entry.LastTouch = ++AvatarTouchCounter;
	EvictedAvatarUUIDs.Remove(UUID);

	return Avatar;
}

void UCrowdyMassEntitySubsystem::EndAvatarGrace(const FGuid& UUID, FCrowdyMassAvatarGraceEntry& Entry)
{
	if (Entry.Avatar)
	{
		UnregisterAvatarParticipant(UUID, Entry.Avatar);
		Entry.Avatar = nullptr;
	}
}

void UCrowdyMassEntitySubsystem::SweepExpiredAvatarGrace(const bool bForce)
{
	const double Now = FPlatformTime::Seconds();
	if (AvatarReclaimGrace.IsEmpty() || (!bForce && Now < NextAvatarGraceSweepSeconds))
	{
		return;
	}

	double EarliestRemaining = TNumericLimits<double>::Max();
	for (auto It = AvatarReclaimGrace.CreateIterator(); It; ++It)
	{
		if (Now < It->Value.ExpiresAtSeconds && ::IsValid(It->Value.Avatar))
		{
			EarliestRemaining = FMath::Min(EarliestRemaining, It->Value.ExpiresAtSeconds);
			continue;
		}
		EndAvatarGrace(It->Key, It->Value);
		It.RemoveCurrent();
	}

	// Due again when the soonest survivor is, rather than a whole window from now: advancing by the window let an
	// entry whose deadline fell just after a sweep wait out most of a second one.
	NextAvatarGraceSweepSeconds = AvatarReclaimGrace.IsEmpty() ? 0.0 : EarliestRemaining;

	// The sweep just consumed the armed deadline, so the next one is armed unconditionally from what it measured
	// rather than compared against a deadline that has already fired.
	ArmedGraceDeadlineSeconds = TNumericLimits<double>::Max();
	ArmAvatarGraceSweep(EarliestRemaining);
}

void UCrowdyMassEntitySubsystem::ReleaseAllAvatars(const TCHAR* Reason)
{
	// The grace window goes first and unconditionally. These paths run because the avatars themselves are
	// wrong (a class change, a reload), and one waiting to be reused is as wrong as one in the cache.
	for (TPair<FGuid, FCrowdyMassAvatarGraceEntry>& Pair : AvatarReclaimGrace)
	{
		EndAvatarGrace(Pair.Key, Pair.Value);
	}
	AvatarReclaimGrace.Empty();

	if (Avatars.Num() == 0)
	{
		return;
	}

	UE_LOG(LogCrowdyMass, Log,
		TEXT("[CrowdyMassEntitySubsystem]: Released %d cached avatars because %s. The entities themselves are untouched, but each one loses the entity registry record its avatar enrolled, so it has to be enrolled again the next time an avatar is asked for. Anything still holding a pointer to one of the released avatars keeps an object that no longer receives events."),
		Avatars.Num(), Reason);

	// Marked as reclaimed rather than forgotten, so an entity that still exists reports "reclaimed, ask
	// again" instead of looking as though nothing ever asked for it.
	for (const TPair<FGuid, FCrowdyMassAvatarEntry>& Pair : Avatars)
	{
		EvictedAvatarUUIDs.Add(Pair.Key);
	}

	// Their registry records go with them, for the same reason an eviction's does: the registry holds the
	// avatar weakly, so a record outliving the last strong reference names an object nothing can resolve.
	UnregisterAllAvatarParticipants();

	Avatars.Empty();
}

void UCrowdyMassEntitySubsystem::ReportAvatarEviction(const FGuid& UUID)
{
	++AvatarEvictionCount;
	++AvatarEvictionsSinceLog;

	UE_LOG(LogCrowdyMass, Verbose,
		TEXT("[CrowdyMassEntitySubsystem]: Reclaimed the avatar for entity %s to stay under the cache capacity of %d. The entity is untouched, but its entity registry record went with the avatar, so it is enrolled again on the next request."),
		*UUID.ToString(), GetAvatarCacheCapacity());

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAvatarEvictionLogSeconds < 5.0)
	{
		return;
	}

	LastAvatarEvictionLogSeconds = Now;
	UE_LOG(LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: %d avatars reclaimed since the last report, %d in this world so far, against a capacity of %d. The capacity follows the tracked-entity count, so reaching it means avatars exist for ids the tracker no longer holds."),
		AvatarEvictionsSinceLog, AvatarEvictionCount, GetAvatarCacheCapacity());
	AvatarEvictionsSinceLog = 0;
}

void UCrowdyMassEntitySubsystem::SetHitZoneTable(const UCrowdyMassHitZoneTable* HitZoneTable)
{
	// Flattened here, once, rather than lazily on first use: assigning the table is already the rare,
	// deliberate call, so the trace path never has to ask "has this changed" on the frequent one.
	ResolvedHitZoneTable.Build(HitZoneTable);
}

void UCrowdyMassEntitySubsystem::ReportMissingTraceCollisionFragment()
{
	++MissingTraceCollisionFragmentCount;

	if (bWarnedMissingTraceCollisionFragment)
	{
		return;
	}

	bWarnedMissingTraceCollisionFragment = true;
	UE_LOG(LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: A Mass entity carries no Crowdy Mass trace collision fragment, so it can never be hit by a Crowdy Mass trace. Nothing adds that fragment when a remote entity spawns: add the Crowdy Mass Trace Collision trait to the Remote Entity Config, or the whole crowd stays untraceable and looks exactly like a trace that is aimed wrong (logged once)."));
}

bool UCrowdyMassEntitySubsystem::ChargeReceiveBudget(const FGuid& UUID)
{
	const double Now = FPlatformTime::Seconds();
	FCrowdyMassReceiveBudget& Budget = ReceiveBudgets.FindOrAdd(UUID);

	if (Budget.LastRefillSeconds == 0.0)
	{
		Budget.Tokens = CrowdyMassReceiveBurstEvents;
		Budget.LastRefillSeconds = Now;
	}

	const double Elapsed = FMath::Max(Now - Budget.LastRefillSeconds, 0.0);
	Budget.LastRefillSeconds = Now;
	Budget.Tokens = FMath::Min(
		Budget.Tokens + static_cast<float>(Elapsed) * CrowdyMassReceiveEventsPerSecond,
		CrowdyMassReceiveBurstEvents);

	if (Budget.Tokens < 1.0f)
	{
		return false;
	}

	Budget.Tokens -= 1.0f;
	return true;
}

#if WITH_EDITOR
void UCrowdyMassEntitySubsystem::HandleObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap)
{
	// The recorded entity classes are REMAPPED, not discarded, and they are the one thing here that must
	// not be. Every other cache below rebuilds itself on the next update; this one is only ever written
	// when an entity spawns, so emptying it would leave every already-spawned entity with no recorded
	// class. That reads as "this client has not learned the class yet" and refuses every event sent to
	// those entities for the rest of the session, which is what recompiling a Blueprint during PIE used
	// to do. Read through even a stale pointer: reinstancing has already marked the old class by the time
	// this runs, so asking for it the ordinary way answers null and there is nothing left to remap.
	for (TPair<FGuid, TWeakObjectPtr<const UClass>>& Pair : StateEntityClassByUUID)
	{
		const UClass* PreviousClass = Pair.Value.GetEvenIfUnreachable();
		if (!PreviousClass)
		{
			continue;
		}

		if (UObject* const* Replacement = ReplacementMap.Find(const_cast<UClass*>(PreviousClass)))
		{
			if (const UClass* ReplacementClass = Cast<UClass>(*Replacement))
			{
				Pair.Value = ReplacementClass;
			}
		}
	}

	// Registered event handlers are REMAPPED for the same reason as the recorded classes above, and never
	// discarded: a handler is registered once by a game at startup, so dropping one would silence that event
	// for the rest of the session. A function the replacement map does not name is left as it was, which
	// leaves its weak pointer stale if the function really was reinstanced; the call it then fails to match
	// is reported by the router as having nothing registered, rather than disappearing.
	//
	// The whole table is rebuilt at the end rather than each key edited in place, because a function is
	// keyed by identity here and several handlers share one key: editing keys while walking them is what
	// would leave a handler filed under a function that no longer exists.
	TArray<FCrowdyMassEventHandlerRef> Remapped;
	for (TPair<FObjectKey, TArray<FCrowdyMassEventHandlerRef>>& Pair : EventHandlersByFunction)
	{
		for (const FCrowdyMassEventHandlerRef& Entry : Pair.Value)
		{
			FCrowdyMassEventHandlerRegistration& Registration = Entry.Get();
			bool bAnythingReplaced = false;

			const UFunction* PreviousFunction = Registration.Function.GetEvenIfUnreachable();
			UObject* const* Replacement =
				PreviousFunction ? ReplacementMap.Find(const_cast<UFunction*>(PreviousFunction)) : nullptr;
			if (const UFunction* ReplacementFunction = Replacement ? Cast<UFunction>(*Replacement) : nullptr)
			{
				Registration.Function = ReplacementFunction;
				bAnythingReplaced = true;
			}

			// An object binding names a function on the object's class as well as the event, and both are
			// reinstanced together. The intermediate event a receive node compiles to keeps its name across
			// a recompile, so re-resolving it on the replacement object is what keeps a bound graph working
			// instead of going quiet until the level is reloaded.
			if (Registration.IsObjectBinding())
			{
				UObject* PreviousObject = Registration.BoundObject.GetEvenIfUnreachable();
				UObject* const* ObjectReplacement = PreviousObject ? ReplacementMap.Find(PreviousObject) : nullptr;
				if (ObjectReplacement && *ObjectReplacement)
				{
					Registration.BoundObject = *ObjectReplacement;
					Registration.BoundFunction = (*ObjectReplacement)->FindFunction(Registration.BoundFunctionName);
					bAnythingReplaced = true;
				}
			}

			// The copy table holds parameters OWNED by those two functions, so replacing either leaves it
			// pointing into storage that has just been thrown away. It is rebuilt against whatever the
			// binding now names, and the binding is dropped when it cannot be.
			if (Registration.IsObjectBinding() && bAnythingReplaced)
			{
				TArray<FString> Unmatched;
				FString Refusal;
				const bool bRebuilt = BuildObjectEventHandlerPlan(Registration.Function.Get(),
					Registration.BoundFunction.Get(), Registration.BoundTargetParam,
					Registration.BoundParamCopies, Unmatched, Refusal);

				if (!bRebuilt)
				{
					Registration.BoundTargetParam = nullptr;
					Registration.BoundParamCopies.Reset();
					continue;
				}
			}

			Remapped.Add(Entry);
		}
	}

	EventHandlersByFunction.Reset();
	for (const FCrowdyMassEventHandlerRef& Entry : Remapped)
	{
		// GetEvenIfUnreachable, so a registration whose function was neither replaced nor remapped is still
		// filed under the key it had. Dropping it here would be the silencing this remap exists to prevent.
		const UFunction* Owner = Entry->Function.GetEvenIfUnreachable();
		if (!Owner)
		{
			continue;
		}

		EventHandlersByFunction.FindOrAdd(FObjectKey(Owner)).Add(Entry);
	}

	// The replacement map is deliberately not consulted: an avatar's class can be replaced without the
	// avatar itself appearing in it, and every entry here is cheap to rebuild, so the whole cache goes.
	ReleaseAllAvatars(TEXT("objects were replaced, which leaves cached avatars on classes that no longer exist"));

	// The same reasoning, for the same reason, one cache further on. This one holds raw FProperty
	// pointers keyed by a raw UScriptStruct pointer, and both are read on the network-arrival path. A
	// struct that is rebuilt rather than replaced would keep the key matching while every property
	// behind it was freed, which is not a state worth reasoning about when discarding the table costs
	// one reflection walk per struct on the next update.
	ViewStateResolvedStructs.Empty();

	// The CrowdyState side of the same cache, which needs discarding for the same reason and is easy to
	// forget precisely because it is keyed by a class rather than a struct: it holds raw FProperty pointers
	// resolved against a class that may have just been reinstanced, and it is read on the network-arrival
	// path. Rebuilding costs one reflection walk per class on the next delta.
	StateResolvedClasses.Empty();
	StateFieldAbsentReportedClasses.Empty();

	// And every class's animation set with the pose ids derived from it, which name row positions in an
	// asset that has just been reloaded underneath them. This one also un-caches the misses, which is what
	// makes adding the component or the Poses row a warning asked for take effect in the session that
	// warned.
	InvalidateAnimSetCaches();

	// Remapped and never cleared: nothing rebuilds these bindings except ConfigureOverrides at backend
	// activation, so emptying them here would unbind every class for the rest of the session.
	//
	// A key that still names a replaced class after the remap is DROPPED. The hook fires once per replaced
	// instance and each broadcast names one pair, so a bound class with no instance in the world is never
	// named by any of them; keeping it would hold the trashed class and its default object alive through a
	// strong reference, and leave an entity of the replacement class inheriting some ancestor's row instead.
	auto NamesAReplacedClass = [](const UClass* Class)
	{
		return !Class || Class->HasAnyClassFlags(CLASS_NewerVersionExists);
	};

	bool bConfigBindingsAffected = false;
	for (const TPair<TObjectPtr<const UClass>, TObjectPtr<const UMassEntityConfigAsset>>& Pair : EntityConfigByClass)
	{
		const UClass* BoundClass = Pair.Key.Get();
		const UMassEntityConfigAsset* BoundConfig = Pair.Value.Get();
		if (NamesAReplacedClass(BoundClass) || !BoundConfig
			|| ReplacementMap.Contains(const_cast<UClass*>(BoundClass))
			|| ReplacementMap.Contains(const_cast<UMassEntityConfigAsset*>(BoundConfig)))
		{
			bConfigBindingsAffected = true;
			break;
		}
	}

	// Most broadcasts name only components, and a construction script re-run sends one per instance, so a
	// rebuild-and-rehash on every firing would be paid for nothing far more often than not.
	if (bConfigBindingsAffected)
	{
		TMap<TObjectPtr<const UClass>, TObjectPtr<const UMassEntityConfigAsset>> RemappedConfigs;
		RemappedConfigs.Reserve(EntityConfigByClass.Num());

		for (const TPair<TObjectPtr<const UClass>, TObjectPtr<const UMassEntityConfigAsset>>& Pair : EntityConfigByClass)
		{
			const UClass* BoundClass = Pair.Key.Get();
			const UMassEntityConfigAsset* BoundConfig = Pair.Value.Get();
			if (!BoundClass || !BoundConfig)
			{
				continue;
			}

			if (UObject* const* Replacement = ReplacementMap.Find(const_cast<UClass*>(BoundClass)))
			{
				if (const UClass* ReplacementClass = Cast<UClass>(*Replacement))
				{
					BoundClass = ReplacementClass;
				}
			}

			if (NamesAReplacedClass(BoundClass))
			{
				continue;
			}

			// A config asset can be reinstanced too, and a row left naming the old one builds from storage
			// that has just been thrown away.
			if (UObject* const* Replacement = ReplacementMap.Find(const_cast<UMassEntityConfigAsset*>(BoundConfig)))
			{
				if (const UMassEntityConfigAsset* ReplacementConfig = Cast<UMassEntityConfigAsset>(*Replacement))
				{
					BoundConfig = ReplacementConfig;
				}
			}

			RemappedConfigs.Add(BoundClass, BoundConfig);
		}

		EntityConfigByClass = MoveTemp(RemappedConfigs);
	}

	// Configs built from a class, which is the one cache here that is keyed by a class AND derived from
	// what that class contains, so a reinstanced class invalidates it twice over: the key may name a class
	// that no longer exists, and the mesh it was built from may have just changed.
	//
	// The reported-once set goes with it for the same reason the pose ids un-cache their misses: adding
	// the mesh the warning asked for has to take effect in the session that warned, and a class still
	// missing one has to be able to say so again.
	DerivedConfigByClass.Empty();
	ReportedUnderivableClasses.Empty();

	// The classes already asked whether the traits they declare are shadowed by an authored config, for the
	// same two reasons: the key may name a class that no longer exists, and the answer was read out of a
	// component on a class that has just been rebuilt. Adding the trait a warning asked about has to take
	// effect in the session that warned, and a class whose traits are still shadowed has to be able to say
	// so again.
	CheckedDeclaredTraitsAgainstAuthoredConfigClasses.Empty();

	// And the authored configs a class's near tier declaration was patched into, which are keyed by the
	// same replaced class and hold a copy of a trait made when that class named something else.
	InvalidateNearTierPatchedConfigs();

	// The stand-ins go for the same reason and one more: a proxy is an INSTANCE of the class it is keyed
	// by, so a recompiled Blueprint leaves it standing for a class that no longer exists, and its property
	// block was laid out by that class. Rebuilt on the next event, at the cost of one construction.
	//
	// The two reported-once sets go with them, so a class that was just recompiled to derive the declaration
	// it could not host stops warning in the session that warned, rather than in the next one.
	DispatchProxyByClass.Empty();
	ReportedUnproxyableClasses.Empty();
	ReportedUnhostableEventClasses.Empty();
}
#endif

bool UCrowdyMassEntitySubsystem::ApplyRemoteEntityTransform(const FGuid& UUID, const FVector& Location, const FQuat& Rotation, const int64 ServerTimestampMs, const float SmoothingDuration)
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(UUID, Entity))
	{
		return false;
	}

	FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager)
	{
		return false;
	}

	return ApplyRemoteEntityTransformToEntity(*EntityManager, Entity, Location, Rotation, ServerTimestampMs, SmoothingDuration);
}

bool UCrowdyMassEntitySubsystem::ApplyRemoteEntityTransformToEntity(FMassEntityManager& InEntityManager, const FMassEntityHandle Entity, const FVector& Location, const FQuat& Rotation, const int64 ServerTimestampMs, const float SmoothingDuration)
{
	FMassEntityManager* EntityManager = &InEntityManager;

	FTransformFragment* TransformFragment = EntityManager->GetFragmentDataPtr<FTransformFragment>(Entity);
	FCrowdyRemoteInterpolationFragment* InterpolationFragment = EntityManager->GetFragmentDataPtr<FCrowdyRemoteInterpolationFragment>(Entity);
	if (!TransformFragment || !InterpolationFragment)
	{
		return false;
	}

	const FTransform& CurrentTransform = TransformFragment->GetTransform();
	InterpolationFragment->StartLocation = CurrentTransform.GetLocation();
	InterpolationFragment->StartRotation = CurrentTransform.GetRotation().GetNormalized();
	InterpolationFragment->TargetLocation = Location;
	InterpolationFragment->TargetRotation = Rotation.GetNormalized();
	InterpolationFragment->ServerTimestampMs = ServerTimestampMs;
	InterpolationFragment->ElapsedSeconds = 0.0f;

	// Timed off local arrival rather than ServerTimestampMs. The SDK treats the server value as
	// unauthenticated, so a sender could name any interval it liked and stretch or compress how its
	// entity moves on everybody else's screen.
	const FCrowdySegmentTiming SegmentTiming = CrowdyInterpolationTiming::OpenSegment(
		InterpolationFragment->ArrivalIntervals,
		InterpolationFragment->LastArrivalSeconds,
		FPlatformTime::Seconds(),
		SmoothingDuration,
		GetInterpolationTuning());

	InterpolationFragment->SmoothingDuration = SegmentTiming.Duration;
	InterpolationFragment->VelocityScale = SegmentTiming.VelocityScale;

	const FVector CurrentLocation = CurrentTransform.GetLocation();
	const FVector GridOrigin = (CurrentLocation + Location) * 0.5f;

	// The sweep grows with how far the sender says it moved, and that distance comes off the wire, so the
	// cell box it asks for grows with the square of a number a peer chooses. Clamped rather than refused: an
	// entity that claims an impossible jump is still placed, in a bounded neighbourhood of where it claims to
	// be, so it stays traceable instead of dropping out of the grid. The finite check is part of the clamp
	// and not belt and braces: a NaN reaches FloorToInt through any Max and is undefined there.
	const float SweptRadius = GetSpatialGridEntityBoundsRadius() + FVector::Dist(CurrentLocation, Location) * 0.5f;
	const float GridRadius = FMath::IsFinite(SweptRadius)
		? FMath::Min(SweptRadius, GetSpatialGridMaxEntitySweepRadius())
		: GetSpatialGridEntityBoundsRadius();

	if (GridOrigin.ContainsNaN())
	{
		// Nothing about where this entity is can be trusted this update, so it keeps the cells it already
		// had rather than being placed somewhere arbitrary or dropped out of the grid entirely.
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: a remote update carried a position that is not a number, so the "
				"entity's grid placement was left as it was."));
	}
	else
	{
		UpdateSpatialGridEntity(Entity, GridOrigin, GridRadius);
	}

	EntityManager->Defer().AddTag<FCrowdyRemoteInterpolatingTag>(Entity);
	if (!EntityManager->IsProcessing())
	{
		EntityManager->FlushCommands();
	}

	return true;
}

bool UCrowdyMassEntitySubsystem::InitializeCrowdyFragments(FMassEntityManager& EntityManager, const FMassEntityHandle Entity, const FGuid& UUID, const FTransform& InitialTransform) const
{
	FTransformFragment* TransformFragment = EntityManager.GetFragmentDataPtr<FTransformFragment>(Entity);
	FCrowdyRemoteIdentityFragment* IdentityFragment = EntityManager.GetFragmentDataPtr<FCrowdyRemoteIdentityFragment>(Entity);
	FCrowdyRemoteInterpolationFragment* InterpolationFragment = EntityManager.GetFragmentDataPtr<FCrowdyRemoteInterpolationFragment>(Entity);
	if (!TransformFragment || !IdentityFragment || !InterpolationFragment)
	{
		return false;
	}

	const FVector InitialLocation = InitialTransform.GetLocation();
	const FQuat InitialRotation = InitialTransform.GetRotation().GetNormalized();

	// Owner and role come from the SDK's registry record for this id. An entity with no record yet is
	// treated as host-owned, never as locally owned: the send path reads this to decide whether this
	// client may speak for the entity, so guessing Owner here would let it originate events on another
	// player's character.
	const UWorld* World = GetWorld();
	const UCrowdyEntitySubsystem* Entities = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
	const FCrowdyEntityRecord* Record = Entities ? Entities->FindRecord(UUID) : nullptr;

	TransformFragment->SetTransform(InitialTransform);
	IdentityFragment->UUID = UUID;
	IdentityFragment->OwnerID = Record ? Record->OwnerID : FGuid();
	IdentityFragment->Role = Record ? Record->Role : ECrowdyRole::HostOwned;
	InterpolationFragment->StartLocation = InitialLocation;
	InterpolationFragment->StartRotation = InitialRotation;
	InterpolationFragment->TargetLocation = InitialLocation;
	InterpolationFragment->TargetRotation = InitialRotation;
	InterpolationFragment->ServerTimestampMs = 0;
	InterpolationFragment->ElapsedSeconds = 0.0f;
	InterpolationFragment->SmoothingDuration = GetDefaultSmoothingDuration();

	// Explicit, because a Mass entity is recycled rather than constructed: whatever occupied this slot
	// before would otherwise hand its arrival timings to an unrelated player who has sent nothing yet.
	InterpolationFragment->LastArrivalSeconds = 0.0;
	InterpolationFragment->ArrivalIntervals.Reset();
	InterpolationFragment->VelocityScale = 1.0f;

	return true;
}

void UCrowdyMassEntitySubsystem::UnregisterEntity(const FGuid& UUID, const FMassEntityHandle Entity)
{
	RemoveSpatialGridEntity(Entity);
	RemoveTraceInstanceCache(Entity);
	ReleaseAvatarState(UUID);
	StateEntityClassByUUID.Remove(UUID);
	UUIDToEntity.Remove(UUID);
	EntityToUUID.Remove(Entity);
	OnRowRemoved.Broadcast(UUID);
}

const TObjectPtr<const UMassEntityConfigAsset>* UCrowdyMassEntitySubsystem::FindEntityConfigBinding(
	const UClass* EntityClass, const UClass*& OutBoundClass) const
{
	OutBoundClass = nullptr;

	// Nothing bound means nothing to walk for. Kept ahead of the loop because an empty binding list is the
	// ordinary case for a project using one config for its whole crowd, and it would otherwise pay a map
	// lookup for every class between the entity and UObject to find out.
	if (!EntityClass || EntityConfigByClass.IsEmpty())
	{
		return nullptr;
	}

	// Most-derived wins, and it comes out that way by construction: the walk starts at the class itself and
	// only moves up, so the first binding it meets is the nearest one above the entity.
	//
	// Hands back the entry rather than only the class so a caller wanting the config does not hash the key
	// a second time to fetch what this already found. The class-only form below is for callers that want
	// to NAME the binding.
	for (const UClass* Candidate = EntityClass; Candidate; Candidate = Candidate->GetSuperClass())
	{
		if (const TObjectPtr<const UMassEntityConfigAsset>* Bound = EntityConfigByClass.Find(Candidate))
		{
			OutBoundClass = Candidate;
			return Bound;
		}
	}

	return nullptr;
}

const UClass* UCrowdyMassEntitySubsystem::FindEntityConfigBindingClass(const UClass* EntityClass) const
{
	const UClass* BoundClass = nullptr;
	FindEntityConfigBinding(EntityClass, BoundClass);
	return BoundClass;
}

FString UCrowdyMassEntitySubsystem::DescribeAuthoredConfigRemedy(const UClass* EntityClass) const
{
	// Asked through the same resolve a spawn asks, for the reason the derivation report gives for doing
	// the same: a second reading of the precedence would answer confidently and wrongly the first time
	// the precedence changed, which is exactly what happened to these messages when it did.
	ECrowdyMassEntityConfigSource Source = ECrowdyMassEntityConfigSource::None;
	GetRemoteEntityConfigAsset(EntityClass, &Source);

	switch (Source)
	{
	case ECrowdyMassEntityConfigSource::PerClassBinding:
		return TEXT("remove the row naming this class in Per Class Entity Configs on the rendering backend, so its entities are built from what the class carries");

	case ECrowdyMassEntityConfigSource::InheritedClassBinding:
		return FString::Printf(
			TEXT("give this class its own row in Per Class Entity Configs on the rendering backend. Nothing binds this class: the config is inherited from the ancestor '%s', so deleting that row would change every class beneath it and not only this one"),
			*GetNameSafe(FindEntityConfigBindingClass(EntityClass)));

	case ECrowdyMassEntityConfigSource::BackendOverride:
		return TEXT("give this class its own row in Per Class Entity Configs on the rendering backend. Nothing binds this class: the config is the backend's Remote Entity Config Override, which answers for every class it does not bind individually");

	case ECrowdyMassEntityConfigSource::ProjectSetting:
		return TEXT("give this class its own row in Per Class Entity Configs on the rendering backend. Nothing binds this class: the config is the project-wide Remote Entity Config in Project Settings");

	default:
		// Reached only if a caller asks about a class nothing authored a config for, which is not a state
		// either warning is raised in. Answered rather than left blank so a future caller gets a sentence.
		return TEXT("give this class its own row in Per Class Entity Configs on the rendering backend");
	}
}

const UMassEntityConfigAsset* UCrowdyMassEntitySubsystem::GetRemoteEntityConfigAsset(const UClass* EntityClass,
	ECrowdyMassEntityConfigSource* OutSource) const
{
	if (OutSource)
	{
		*OutSource = ECrowdyMassEntityConfigSource::None;
	}

	// A per-class binding wins over everything below it, and a class with no binding of its own inherits
	// the nearest one above it. By inheritance rather than exactly, because a subclass of a bound class is
	// the same kind of thing drawn the same way: an author who binds a base and then makes three variants
	// of it should not have to re-list every one, and the alternative failure is worse than a wrong guess,
	// since an unlisted variant falls all the way to the backend-wide default and renders as some other
	// class entirely. A variant that really does differ says so by carrying its own row.
	const UClass* BoundClass = nullptr;
	if (const TObjectPtr<const UMassEntityConfigAsset>* Bound = FindEntityConfigBinding(EntityClass, BoundClass))
	{
		if (OutSource)
		{
			*OutSource = BoundClass == EntityClass
				? ECrowdyMassEntityConfigSource::PerClassBinding
				: ECrowdyMassEntityConfigSource::InheritedClassBinding;
		}

		return Bound->Get();
	}

#if WITH_DEV_AUTOMATION_TESTS
	// A fixture that installed its own bindings stops here, so a case about a class nothing configures is
	// not answered by whatever Remote Entity Config the running project happens to ship.
	if (bSuppressUnboundEntityConfigForTest)
	{
		return nullptr;
	}
#endif

	// Per-backend override wins when set; otherwise fall back to the project-wide setting.
	if (!RemoteEntityConfigOverride.IsNull())
	{
		if (OutSource)
		{
			*OutSource = ECrowdyMassEntityConfigSource::BackendOverride;
		}

		return RemoteEntityConfigOverride.LoadSynchronous();
	}

	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	if (!Settings)
	{
		return nullptr;
	}

	if (OutSource)
	{
		*OutSource = ECrowdyMassEntityConfigSource::ProjectSetting;
	}

	return Settings->RemoteEntityConfig.LoadSynchronous();
}

const UMassEntityConfigAsset* UCrowdyMassEntitySubsystem::FindAuthoredEntityConfig(const UClass* EntityClass,
	ECrowdyMassEntityConfigSource& OutSource) const
{
	const UMassEntityConfigAsset* Config = GetRemoteEntityConfigAsset(EntityClass, &OutSource);

	// A branch that answered with nothing is not a source. Every one of them can be reached and still
	// produce null (a binding holding an empty asset, an override that failed to load, an unset project
	// setting), and all three fall through to deriving exactly as an absent one does, so reporting them
	// as configured would say a spawn uses an asset that is not there.
	if (!Config)
	{
		OutSource = ECrowdyMassEntityConfigSource::None;
	}

	return Config;
}

const FMassEntityTemplate* UCrowdyMassEntitySubsystem::ResolveRemoteEntityTemplate(const UWorld& World, const ECrowdyMassRemoteEntitySource Source,
	const UClass* EntityClass, FString& OutWhy)
{
	if (Source == ECrowdyMassRemoteEntitySource::PluginDefaults)
	{
		const FMassEntityTemplate* Template = GetOrBuildPluginDefaultTemplate(World);
		if (!Template)
		{
			OutWhy = TEXT("This plugin's own remote entity traits produced no usable Mass entity template, so a spawn that asked to depend on no project content had nothing to spawn from.");
			return nullptr;
		}

		return Template;
	}

	const UMassEntityConfigAsset* EntityConfig = GetRemoteEntityConfigAsset(EntityClass);

	// The one question the authored paths cannot be asked while they are answering: whether a class that
	// nothing configures would appear on its own. Pushing the authored answer aside is deliberately all
	// this does, so a crowd that appears under it is built the same way an unconfigured project's would
	// be, rather than by some third path that only exists while the variable is on.
	const bool bForcedAside = CrowdyMassDerivation::ShouldForceDerive() && EntityConfig != nullptr;
	if (bForcedAside)
	{
		EntityConfig = nullptr;
	}

	// Nothing was authored for this class, so the last resort is what the class itself carries. This can
	// only ever ADD an answer where there was none: it is reached exactly when the per-class binding, the
	// backend override and the project-wide setting have all answered null, so no project that configures
	// its crowd behaves any differently for it existing.
	if (!EntityConfig)
	{
		EntityConfig = GetOrBuildDerivedEntityConfig(EntityClass, bForcedAside);
	}
	else
	{
		// Something WAS authored, and the near tier is declared on the class rather than in a config, so
		// this is where the two meet. It hands back the authored config itself unless the class declares a
		// shell the config has left no room for, which is what stops the declaration being a thing that
		// only works for projects that authored nothing.
		//
		// Kept before that call because a patched config is a per-class copy made in memory, with a generated
		// name and no asset behind it. The report below tells an author to go and edit the config, so it has
		// to name the one they bound rather than the copy they cannot open.
		const UMassEntityConfigAsset* const AuthoredConfig = EntityConfig;

		EntityConfig = GetOrBuildNearTierPatchedConfig(World, EntityClass, EntityConfig);

		// And the traits a class declares, which are NOT read for a class an authored config answers for.
		// Only reported here, never applied: this is the one place that knows a config answered.
		ReportDeclaredTraitsNotConsumed(EntityClass, AuthoredConfig);
	}

	if (!EntityConfig)
	{
		OutWhy = bForcedAside
			? TEXT("crowdy.mass.derive.force is on, so the authored Mass entity config was pushed aside, and one could not be worked out from the entity's own class either. Set it back to 0, or give that class a skeletal mesh.")
			: TEXT("Remote Entity Config is not set, and one could not be worked out from the entity's own class either. Either give that class a skeletal mesh, or create a Mass Entity Config asset with the Crowdy Remote Entity trait and assign it in Project Settings > Plugins > Crowdy Mass.");
		return nullptr;
	}

	const FMassEntityTemplate& EntityTemplate = EntityConfig->GetOrCreateEntityTemplate(World);
	if (!EntityTemplate.IsValid())
	{
		OutWhy = FString::Printf(TEXT("Remote Entity Config '%s' produced an invalid Mass entity template."), *GetNameSafe(EntityConfig));
		return nullptr;
	}

	return &EntityTemplate;
}

const FMassEntityTemplate* UCrowdyMassEntitySubsystem::GetOrBuildPluginDefaultTemplate(const UWorld& World)
{
	// An entity config built here rather than loaded, so this plugin has a remote entity template of its
	// own without shipping an asset for one. Composed from traits rather than from a hand-written list of
	// fragments, so what it produces is whatever those traits produce and the two cannot drift apart.
	//
	// Deliberately NOT the animation trait: that one requires a representation fragment only a
	// visualization trait provides, and its animation set is an asset reference that logs an error when
	// unset, so including it would make a spawn that references no content emit errors that mean nothing.
	// A crowd entity here is state and no picture; drawing one needs a mesh, and this plugin has none.
	if (!PluginDefaultRemoteEntityConfig)
	{
		PluginDefaultRemoteEntityConfig = NewObject<UMassEntityConfigAsset>(this);

		UCrowdyMassRemoteEntityTrait* RemoteEntityTrait = NewObject<UCrowdyMassRemoteEntityTrait>(this);
		UCrowdyMassViewPoseTrait* ViewPoseTrait = NewObject<UCrowdyMassViewPoseTrait>(this);
		PluginDefaultRemoteEntityConfig->GetMutableConfig().AddTrait(*RemoteEntityTrait);
		PluginDefaultRemoteEntityConfig->GetMutableConfig().AddTrait(*ViewPoseTrait);
	}

	// Reported by the caller rather than logged here, so one failure produces one line.
	const FMassEntityTemplate& Template = PluginDefaultRemoteEntityConfig->GetOrCreateEntityTemplate(World);
	return Template.IsValid() ? &Template : nullptr;
}

static UAnimSequenceTransformProviderData* CrowdyMassLoadDerivedBank(const UClass* EntityClass,
	const FCrowdyMassDerivedAnimSet& DerivedAnimSet, const USkinnedAsset* DerivedMeshAsset)
{
	if (!DerivedAnimSet.IsValid())
	{
		return nullptr;
	}

	const UCrowdyMassAnimSet* AnimSet = DerivedAnimSet.AnimSet.LoadSynchronous();
	if (!AnimSet)
	{
		return nullptr;
	}

	if (AnimSet->Bank.IsNull())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: anim set '%s' for '%s' names no Bank, so its entities render in their bind pose. Set the Bank on the anim set to the transform provider for the class's mesh."),
			*AnimSet->GetPathName(), *EntityClass->GetPathName());
		return nullptr;
	}

	UAnimSequenceTransformProviderData* Bank = AnimSet->Bank.LoadSynchronous();
	if (!Bank)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: anim set '%s' for '%s' names Bank '%s', which did not load, so its entities render in their bind pose."),
			*AnimSet->GetPathName(), *EntityClass->GetPathName(), *AnimSet->Bank.ToString());
		return nullptr;
	}

	const USkinnedAsset* BankMesh = Bank->GetSkinnedAsset();
	UE_CLOG(BankMesh && BankMesh != DerivedMeshAsset, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: Bank '%s' on anim set '%s' animates mesh '%s' but '%s' renders '%s', so the crowd renders in its bind pose. Point the Bank at a provider for the class's mesh."),
		*Bank->GetPathName(), *AnimSet->GetPathName(), *GetNameSafe(BankMesh), *EntityClass->GetPathName(),
		*GetNameSafe(DerivedMeshAsset));

	return Bank;
}

UMassEntityConfigAsset* UCrowdyMassEntitySubsystem::GetOrBuildDerivedEntityConfig(const UClass* EntityClass,
	const bool bAuthoredConfigForcedAside)
{
	// This path reaches a synchronous class load, several NewObject calls and a DuplicateObject, none of
	// which a worker thread may do. Asserted rather than assumed because every caller today is on the game
	// thread and nothing would catch the one that stopped being.
	check(IsInGameThread());

	if (!EntityClass)
	{
		return nullptr;
	}

	if (const TObjectPtr<UMassEntityConfigAsset>* Existing = DerivedConfigByClass.Find(EntityClass))
	{
		return Existing->Get();
	}

	// Reading a class default object and every Blueprint in its parent chain is far too much to do per
	// spawn, and the answer cannot change while the class is the same object, so it is worked out once and
	// kept.
	//
	// Only the SUCCESS is kept. A class that supplies no mesh is walked again for every entity of it that
	// ever appears, and only the warning is suppressed. That is deliberate: the walk below refuses a class
	// whose default object does not exist yet, which is a class still loading rather than a class that will
	// never work, and a negative entry would hold that verdict until something invalidated it.
	const FCrowdyMassDerivedMesh DerivedMesh = FCrowdyMassMeshDerivation::FromClass(EntityClass);
	if (!DerivedMesh.IsValid())
	{
		if (!ReportedUnderivableClasses.Contains(EntityClass))
		{
			ReportedUnderivableClasses.Add(EntityClass);
			UE_CLOG(!bAuthoredConfigForcedAside, LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: nothing configures a Mass entity config for '%s', and one could not be worked out from the class, so entities of it will not appear. %s"),
				*EntityClass->GetPathName(), *DerivedMesh.Reason);
			UE_CLOG(bAuthoredConfigForcedAside, LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: crowdy.mass.derive.force is on, so the Mass entity config authored for '%s' was pushed aside, and one could not be worked out from the class either, so entities of it will not appear. Set the variable back to 0 to use the authored config again. %s"),
				*EntityClass->GetPathName(), *DerivedMesh.Reason);
		}

		return nullptr;
	}

	// A material only draws through the instanced skinned mesh path if it carries the matching usage flag,
	// and without it the engine substitutes the default material without anything on the entity changing.
	// So the flag is read off the mesh here, where the mesh is already loaded, rather than left to be
	// noticed as a crowd that turned up looking wrong.
	//
	// Once per material rather than once per class: the flag is ticked on one asset, and a project whose
	// character materials share a parent would otherwise get the same instruction once per class and bury
	// it. What that leaves unsaid is never lost, because the full verdict for any class is in the line
	// below and can be asked for again with crowdy.mass.derive.
	const FCrowdyMassInstancedSkinnedMaterials DerivedMaterials =
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(DerivedMesh.Asset);

	TArray<FString> UnflaggedMaterialMessages;
	FCrowdyMassMaterialUsageReports::TakeUnreported(DerivedMaterials, DerivedMesh.Asset, EntityClass, UnflaggedMaterialMessages);
	for (const FString& Message : UnflaggedMaterialMessages)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: %s"), *Message);
	}

	UMassEntityConfigAsset* DerivedConfig = NewObject<UMassEntityConfigAsset>(this);

	// The same two traits the plugin's own default template composes, which is what makes a derived entity
	// a Crowdy entity rather than an ordinary Mass one.
	UCrowdyMassRemoteEntityTrait* RemoteEntityTrait = NewObject<UCrowdyMassRemoteEntityTrait>(this);
	UCrowdyMassViewPoseTrait* ViewPoseTrait = NewObject<UCrowdyMassViewPoseTrait>(this);
	DerivedConfig->GetMutableConfig().AddTrait(*RemoteEntityTrait);
	DerivedConfig->GetMutableConfig().AddTrait(*ViewPoseTrait);

	// And the picture, which is the whole reason a config had to be authored before. This plugin ships no
	// mesh of its own, but the class carries one, and this is the trait that turns it into instances.
	UMassMovableVisualizationTrait* VisualizationTrait = NewObject<UMassMovableVisualizationTrait>(this);

	// Resolved before the mesh description because Meshes.Add copies it, so a bank assigned afterwards
	// would never reach the trait. Only the class's own declaration is read, never an authored config,
	// because this whole function is reached exactly when no authored config answered for the class.
	const FCrowdyMassDerivedAnimSet DerivedAnimSet = FCrowdyMassAnimSetDerivation::FromClass(EntityClass);

	FMassSkinnedMeshInstanceVisualizationMeshDesc MeshDesc;
	MeshDesc.Asset = DerivedMesh.Asset;
	// The crowd draws at LocalTransform * the entity's transform, so this keeps a row standing where the actor's mesh stands.
	MeshDesc.LocalTransform = DerivedMesh.Transform;
	MeshDesc.TransformProvider = CrowdyMassLoadDerivedBank(EntityClass, DerivedAnimSet, DerivedMesh.Asset);
	VisualizationTrait->SkinnedMeshInstanceDesc.Meshes.Add(MeshDesc);

	// Every level draws as a skinned instance, and none of this is the default. Left alone, the two nearest
	// levels spawn actual Actors, which is the per-entity cost a crowd exists to avoid, and the third asks
	// for a static mesh while the static mesh description is empty, which the trait's own validation
	// refuses. The description that was filled in and the levels that read it have to agree.
	for (int32 LODIndex = 0; LODIndex < EMassLOD::Max; ++LODIndex)
	{
		VisualizationTrait->Params.LODRepresentation[LODIndex] = EMassRepresentationType::SkinnedMeshInstance;
	}

	// Unless the class asks for a real actor when it gets close. A class that declares no near tier shell
	// keeps the table above untouched at every level and is drawn as a crowd row however near it comes,
	// which is what makes promotion something a class opts into rather than something every class pays for.
	//
	// Loaded and checked in the derivation, because the authored config path asks the same question and a
	// second copy of the checks would drift: the failure that drift produces is a class promoted on one
	// path and silently left as crowd rows on the other. It happens once per class, never per entity.
	//
	// Checked at all rather than left to the visualization trait, which quietly records no template for a
	// class it cannot use while the nearest level still asks for an actor. That pairing is the one thing
	// this must never produce: it logs an error for every entity every frame and draws nothing.
	const FCrowdyMassDerivedNearTierShell DerivedShell = FCrowdyMassNearTierDerivation::FromClass(EntityClass);

	FString ShellRefusal;
	bool bShellAskAgainLater = false;
	UClass* NearTierActorClass = FCrowdyMassNearTierDerivation::LoadUsableShellClass(EntityClass, DerivedShell,
		ShellRefusal, bShellAskAgainLater);

	UE_CLOG(!ShellRefusal.IsEmpty(), LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: %s"), *ShellRefusal);

	if (NearTierActorClass)
	{
		VisualizationTrait->HighResTemplateActor = NearTierActorClass;
		VisualizationTrait->Params.LODRepresentation[EMassLOD::High] = EMassRepresentationType::HighResSpawnedActor;
		VisualizationTrait->Params.RepresentationActorManagementClass = UCrowdyMassRepresentationActorManagement::StaticClass();

		// The three levels below the nearest keep drawing as skinned instances, and the engine reads the
		// first non-actor entry in that table as what to draw while a spawn is still in flight, so the crowd
		// row covers the gap on its own and nothing has to be authored to make it.
		//
		// Two things are deliberately left alone. No low resolution template actor, so a demoted entity's
		// actor is released rather than kept alive disabled: the engine only holds one when the near and far
		// template indices are the same, and leaving this null makes them differ. And bKeepLowResActors
		// stays at its default, because hiding a disabled actor is already handled where the representation
		// changes and a second thing doing the same job would mean neither can be shown to be doing it.
		const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();

		// Distances have to keep rising across the levels or the level below the nearest never wins an
		// entity, so a near tier band asked to reach past the band beyond it is cut back to meet it.
		const float FarBandDistance = VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::Low];
		const float FarBandVisibleDistance = VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::Low];

		const float RequestedDistance = Settings ? Settings->NearTierDistance : VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::Medium];
		const float RequestedVisibleDistance = Settings ? Settings->NearTierVisibleDistance : VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::Medium];

		const float NearTierDistance = FMath::Clamp(RequestedDistance, 0.0f, FarBandDistance);
		const float NearTierVisibleDistance = FMath::Clamp(RequestedVisibleDistance, 0.0f, FarBandVisibleDistance);

		UE_CLOG(NearTierDistance != RequestedDistance || NearTierVisibleDistance != RequestedVisibleDistance,
			LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: the near tier distances for '%s' (%.0f, %.0f visible) reach past the band beyond them and were cut back to %.0f and %.0f. Lower them, or a crowd would skip the band in between."),
			*EntityClass->GetPathName(), RequestedDistance, RequestedVisibleDistance, NearTierDistance, NearTierVisibleDistance);

		VisualizationTrait->LODParams.BaseLODDistance[EMassLOD::Medium] = NearTierDistance;
		VisualizationTrait->LODParams.VisibleLODDistance[EMassLOD::Medium] = NearTierVisibleDistance;

		// The ceiling on how many of these actors exist at once. The nearest entities take them and the rest
		// stay crowd rows however close they come, which is what stops a crowd walking into the camera from
		// costing whatever it likes.
		VisualizationTrait->LODParams.LODMaxCount[EMassLOD::High] =
			Settings ? FMath::Max(Settings->MaxNearTierActors, 0) : VisualizationTrait->LODParams.LODMaxCount[EMassLOD::High];

		UE_LOG(LogCrowdyMass, Log,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' declares near tier actor '%s' on %s, so its entities are promoted to actors within %.0f cm (%.0f cm visible), at most %d at once."),
			*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route),
			NearTierDistance, NearTierVisibleDistance, VisualizationTrait->LODParams.LODMaxCount[EMassLOD::High]);
	}

	DerivedConfig->GetMutableConfig().AddTrait(*VisualizationTrait);

	// Being drawable is not the same as being hittable: an entity with no trace collision fragment is not
	// part of tracing at all, which looks exactly like an empty spatial grid from the outside. The shape it
	// is hit with does not come from here, it comes from the physics asset on the mesh resolved above, so
	// this trait exists to say the entity takes part and what sort of thing it is.
	//
	// Everything else is left at the trait's own defaults on purpose, so an entity built from a class
	// behaves exactly like one an author added this trait to and did not touch: it ignores every channel
	// except the camera, and widening that is what authoring a config is for.
	UCrowdyMassTraceCollisionTrait* TraceCollisionTrait = NewObject<UCrowdyMassTraceCollisionTrait>(this);

	const FCrowdyMassDerivedTraceCollision DerivedCollision = FCrowdyMassCollisionDerivation::FromActorClass(EntityClass);
	if (DerivedCollision.IsValid())
	{
		TraceCollisionTrait->SetObjectType(DerivedCollision.Settings.ObjectType.GetValue());
	}

	DerivedConfig->GetMutableConfig().AddTrait(*TraceCollisionTrait);

	// And the animation, which is what separates a crowd that moves from a crowd that slides around in its
	// bind pose. A pose and gait table is authored intent and cannot be worked out from a mesh, so this is
	// the one part of a derived config that the class has to name rather than supply: the Crowdy Mass Anim
	// Set component on the class is where it names it. The set's Bank was given to the mesh above.
	if (DerivedAnimSet.IsValid())
	{
		UCrowdyMassAnimationTrait* AnimationTrait = NewObject<UCrowdyMassAnimationTrait>(this);
		AnimationTrait->AnimSet = DerivedAnimSet.AnimSet;

		// The fragment the renderer joins animation to an instance through has one producer in an authored
		// setup, whichever visualization trait that project chose, and the movable visualization trait used
		// above is not one of them. Nothing else in a derived config can add it, so the animation trait
		// does, and template validation stays loud for every authored config that is missing one.
		AnimationTrait->bAddRepresentationAnimationFragment = true;

		DerivedConfig->GetMutableConfig().AddTrait(*AnimationTrait);
	}

	// Said whether or not a set was found, because "the crowd appeared and stands still" is the outcome
	// with no other explanation anywhere: the entity spawns, renders and moves, and only the animation is
	// missing. The reason names the component to add.
	UE_CLOG(!DerivedAnimSet.IsValid(), LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' was built from the class itself with no animation, so its entities render in their bind pose. %s"),
		*EntityClass->GetPathName(), *DerivedAnimSet.Reason);

	// And the one part of a config built from a class that the game gets to write. Everything above is worked
	// out from what the class contains; this is what the class ASKS for, and it goes on the end.
	//
	// Last on purpose. Mass combines a config's traits by class and keeps the LAST one of each class it is
	// offered, so this is the only position at which a game's own trait replaces the one worked out for it
	// instead of being replaced by it. Adding it before the traits above would put it in the list and then
	// throw it away, which is the worst of the available outcomes: authored, listed, and never built.
	//
	// After the animation trait specifically, not merely after the visualization one, because the animation
	// trait above is only added for a class that names an animation set. Any position among them would give a
	// declared trait a different precedence depending on whether the class happened to name one, which is a
	// rule nobody could predict.
	const FCrowdyMassDerivedEntityTraits DeclaredTraits = FCrowdyMassEntityTraitsDerivation::FromClass(EntityClass);

	for (const UMassEntityTraitBase* Declared : DeclaredTraits.Traits)
	{
		FString Refusal;
		FString Overridden;
		if (!FCrowdyMassEntityTraitsDerivation::CanAppend(*DerivedConfig, *Declared, Refusal, Overridden))
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' lists trait '%s' on its Crowdy Mass Entity Traits component and it was not added, so its entities are built without it: %s. Author a Mass entity config for the class if that trait has to decide how its entities are drawn."),
				*EntityClass->GetPathName(), *Declared->GetClass()->GetName(), *Refusal);
			continue;
		}

		// COPIED rather than used where it sits. The instance in the list belongs to the class that declares
		// it, and this config is kept for as long as that class is loaded: recompiling the class, or editing
		// the trait's own settings, replaces the objects that component owns, and a config holding those
		// directly would be built from traits of a class that no longer exists. The copy belongs to this
		// config, so what a crowd is made of cannot change underneath it half way through a session.
		//
		// The other side of that is real and worth knowing: an edit to the trait's settings is not seen until
		// this config is worked out again, which is what editing one in the editor drops it for.
		//
		// The copy keeps the source's concrete class, which is what makes Mass treat it as a replacement for
		// a trait of that class rather than as a second trait beside it.
		UMassEntityTraitBase* Copy = DuplicateObject<UMassEntityTraitBase>(Declared, DerivedConfig);
		if (!Copy)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' lists trait '%s' on its Crowdy Mass Entity Traits component and a copy of it could not be made, so its entities are built without it."),
				*EntityClass->GetPathName(), *Declared->GetClass()->GetName());
			continue;
		}

		DerivedConfig->GetMutableConfig().AddTrait(*Copy);

		// Replacing the trait that DRAWS the entity is not the same size of event as replacing any other, so
		// it is not said at the same volume. The one worked out here is what carries the mesh found on the
		// class, the level table that selects it, and the near tier promotion the class declared: a
		// replacement takes all three, and a crowd drawing nothing looks identical to a crowd that never
		// arrived. Every other trait replaces something the class did not supply, and is ordinary.
		const bool bReplacesTheDrawnRepresentation =
			!Overridden.IsEmpty() && Declared->IsA(UMassVisualizationTrait::StaticClass());

		UE_CLOG(bReplacesTheDrawnRepresentation, LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' lists trait '%s' on %s, which replaces the '%s' worked out for it, and with it the mesh found on the class, the animation bank from its anim set, the level-of-detail table naming that mesh, and any near tier promotion the class declares. The listed trait has to supply all of those itself, or entities of this class draw nothing."),
			*EntityClass->GetPathName(), *Declared->GetClass()->GetName(), LexToString(DeclaredTraits.Route),
			*Overridden);

		UE_CLOG(!Overridden.IsEmpty() && !bReplacesTheDrawnRepresentation, LogCrowdyMass, Log,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' lists trait '%s' on %s, which replaces the '%s' worked out for it."),
			*EntityClass->GetPathName(), *Declared->GetClass()->GetName(), LexToString(DeclaredTraits.Route),
			*Overridden);
	}

	// Said only when the component is there and adds nothing, which is the case with no other symptom: the
	// crowd appears and behaves correctly, and the only thing wrong is that what somebody added does nothing.
	// A class with no such component at all is the ordinary case and says nothing. Once per class, because
	// this whole function runs once per class.
	UE_CLOG(DeclaredTraits.bSawComponent && !DeclaredTraits.IsValid(), LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: %s"), *DeclaredTraits.Reason);

	// A list that is partly filled in is its own mistake and reaches neither of the lines around it: the
	// reason above is only built when the component adds NOTHING, and a row left unset otherwise disappears
	// between the traits beside it that did apply.
	UE_CLOG(DeclaredTraits.IsValid() && DeclaredTraits.EmptyEntryCount > 0, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' lists %d Mass trait(s) on %s and leaves %d row(s) set to none, which add nothing. Pick a trait class on each row, or remove the rows."),
		*EntityClass->GetPathName(), DeclaredTraits.Traits.Num(), LexToString(DeclaredTraits.Route),
		DeclaredTraits.EmptyEntryCount);

	UE_CLOG(DeclaredTraits.ShadowedComponentCount > 0, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' carries %d further Crowdy Mass Entity Traits component(s) beyond '%s', and only that one is read. Put every trait on one component."),
		*EntityClass->GetPathName(), DeclaredTraits.ShadowedComponentCount, *DeclaredTraits.ComponentName);

	DerivedConfigByClass.Add(EntityClass, DerivedConfig);

	// The material verdict rides this line rather than a line of its own, so it is stated once for every
	// class that ever derives one, including the classes whose warning above was left unsaid because
	// another class had already named the same material.
	const FString MaterialVerdict = DerivedMaterials.Describe();

	UE_CLOG(!bAuthoredConfigForcedAside, LogCrowdyMass, Log,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' has no authored Mass entity config, so one was built from the class itself using mesh '%s' (found on %s). Materials: %s. Authoring a config for it overrides this."),
		*EntityClass->GetPathName(), *GetNameSafe(DerivedMesh.Asset), LexToString(DerivedMesh.Route), *MaterialVerdict);
	UE_CLOG(bAuthoredConfigForcedAside, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: crowdy.mass.derive.force is on, so the Mass entity config authored for '%s' was pushed aside and one was built from the class itself using mesh '%s' (found on %s). Materials: %s. Set the variable back to 0 to use the authored config again."),
		*EntityClass->GetPathName(), *GetNameSafe(DerivedMesh.Asset), LexToString(DerivedMesh.Route), *MaterialVerdict);

	return DerivedConfig;
}

bool UCrowdyMassEntitySubsystem::ReportDeclaredTraitsNotConsumed(const UClass* EntityClass,
	const UMassEntityConfigAsset* AuthoredConfig)
{
	if (!EntityClass)
	{
		return false;
	}

	// The set records every class ASKED, not every class told. Reading the declaration costs a class default
	// object walk down a Blueprint parent chain, and a class that declares nothing is the common case, so
	// without this the walk would happen for every entity of every configured class that ever appears.
	if (CheckedDeclaredTraitsAgainstAuthoredConfigClasses.Contains(EntityClass))
	{
		return false;
	}

	CheckedDeclaredTraitsAgainstAuthoredConfigClasses.Add(EntityClass);

	const FCrowdyMassDerivedEntityTraits DeclaredTraits = FCrowdyMassEntityTraitsDerivation::FromClass(EntityClass);
	if (!DeclaredTraits.IsValid())
	{
		// A component carrying nothing still gets its own answer here. The line below cannot say it, because
		// there are no traits to move, and the class never reaches the derived path where the same mistake is
		// reported, so without this the one case of "I added the component and filled in nothing, and a
		// config is authored too" is the one that says nothing at all.
		UE_CLOG(DeclaredTraits.bSawComponent, LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: %s Mass entity config '%s' is authored for this class in any case, so its entities are built from that config alone and this component is never read."),
			*DeclaredTraits.Reason, *GetNameSafe(AuthoredConfig));

		return false;
	}

	// A config's trait list is the whole statement of what its entities are made of, so a component is not
	// allowed to add to it: doing so would give a project traits its config never lists, removable only by
	// finding a component on some other class. The declaration is therefore not read at all here, and this
	// exists because a list somebody filled in that quietly does nothing has no other symptom.
	UE_LOG(LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' lists %d Mass trait(s) on %s, and Mass entity config '%s' is authored for it, so its entities are built from that config alone and the listed traits are never used. Move them into that config's own trait list, or %s."),
		*EntityClass->GetPathName(), DeclaredTraits.Traits.Num(), LexToString(DeclaredTraits.Route),
		*GetNameSafe(AuthoredConfig), *DescribeAuthoredConfigRemedy(EntityClass));

	// Said here as well as on the derived path, because an author who moves the traits off the component the
	// line above names still has a second one further up the chain that nothing else will ever mention.
	UE_CLOG(DeclaredTraits.ShadowedComponentCount > 0, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' carries %d further Crowdy Mass Entity Traits component(s) beyond '%s', which are not read either. Put every trait on one component."),
		*EntityClass->GetPathName(), DeclaredTraits.ShadowedComponentCount, *DeclaredTraits.ComponentName);

	return true;
}

const UMassEntityConfigAsset* UCrowdyMassEntitySubsystem::GetOrBuildNearTierPatchedConfig(const UWorld& World,
	const UClass* EntityClass, const UMassEntityConfigAsset* AuthoredConfig)
{
	// Asserted rather than assumed, like the sibling accessors: this path reaches a synchronous class load,
	// NewObject and DuplicateObject, none of which a worker thread may do, and today it is only ever reached
	// from a spawn that has already hopped to the game thread.
	check(IsInGameThread());

	if (!EntityClass || !AuthoredConfig)
	{
		return AuthoredConfig;
	}

	// The authored config is compared as well as the class, because which config answers for a class is
	// decided by a per-class binding, a backend override and a project setting, and a backend swap moves
	// all three. A patch built over the previous one would name a parent nothing points at any more.
	if (const FCrowdyMassNearTierPatchedConfig* Existing = NearTierPatchedConfigByClass.Find(EntityClass))
	{
		if (Existing->AuthoredConfig == AuthoredConfig)
		{
			return Existing->PatchedConfig ? Existing->PatchedConfig.Get() : AuthoredConfig;
		}
	}

	// Kept whichever way it goes, including when nothing is patched. Answering costs a class default object
	// walk down a parent chain and a synchronous class load, and most classes in a project declare no shell,
	// so a miss that was not kept would pay that cost once per entity for every one of them.
	FCrowdyMassNearTierPatchedConfig Entry;
	Entry.AuthoredConfig = AuthoredConfig;

	const FCrowdyMassDerivedNearTierShell DerivedShell = FCrowdyMassNearTierDerivation::FromClass(EntityClass);

	FString ShellRefusal;
	bool bShellAskAgainLater = false;
	UClass* NearTierActorClass = FCrowdyMassNearTierDerivation::LoadUsableShellClass(EntityClass, DerivedShell,
		ShellRefusal, bShellAskAgainLater);

	UE_CLOG(!ShellRefusal.IsEmpty(), LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: %s"), *ShellRefusal);

	if (!NearTierActorClass)
	{
		// A class still forming answers exactly as a class that declared nothing does, and memoising that
		// would make a moment permanent: nothing asks again until an invalidation nobody has a reason to
		// trigger. Left unmemoised so the next spawn of it pays the walk again and gets a real answer.
		if (!bShellAskAgainLater)
		{
			NearTierPatchedConfigByClass.Add(EntityClass, Entry);
		}

		return AuthoredConfig;
	}

	// Which trait Mass will actually build from, rather than the first one of that type in the chain. A
	// second visualization trait that also builds writes the representation parameters FIRST and the patch
	// is then discarded in silence, so a config carrying two is refused rather than half-patched.
	FString TraitRefusal;
	const UMassVisualizationTrait* AuthoredTrait = FCrowdyMassNearTierAuthoredConfig::FindPatchableVisualizationTrait(
		*AuthoredConfig, FCrowdyMassNearTierAuthoredConfig::GetWorldProcessingFlags(World), TraitRefusal);

	if (!AuthoredTrait)
	{
		if (!ReportedNearTierShellNotConsumedClasses.Contains(EntityClass))
		{
			ReportedNearTierShellNotConsumedClasses.Add(EntityClass);
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' declares near tier actor '%s' on %s, but Mass entity config '%s' is authored for it and %s, so its entities stay crowd rows at every distance. Fix that config, or stop binding it to this class so the entity is built from what the class carries."),
				*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route),
				*GetNameSafe(AuthoredConfig), *TraitRefusal);
		}

		NearTierPatchedConfigByClass.Add(EntityClass, Entry);
		return AuthoredConfig;
	}

	// A config that already promotes on its own is an authored decision and keeps it. Two things are still
	// worth saying about it, and neither changes what is used.
	if (FCrowdyMassNearTierAuthoredConfig::AlreadyPromotes(*AuthoredTrait))
	{
		if (!ReportedNearTierShellNotConsumedClasses.Contains(EntityClass))
		{
			ReportedNearTierShellNotConsumedClasses.Add(EntityClass);

			const UClass* AuthoredShellClass = AuthoredTrait->HighResTemplateActor;

			// The declaration it shadows. Both references resolve perfectly and the only thing wrong is
			// that the component somebody filled in does nothing: change it and nothing happens.
			UE_CLOG(AuthoredShellClass == NearTierActorClass, LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' declares near tier actor '%s' on %s, and Mass entity config '%s' authored for it already promotes to that same actor. The config is what does it, so the declaration is never read; remove one of them so the two cannot drift apart."),
				*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route),
				*GetNameSafe(AuthoredConfig));
			UE_CLOG(AuthoredShellClass != NearTierActorClass, LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' declares near tier actor '%s' on %s, and Mass entity config '%s' authored for it already promotes to '%s'. The config wins, so entities of that class are promoted to the config's actor and the declaration is never read; remove one of them so the two cannot drift apart."),
				*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route),
				*GetNameSafe(AuthoredConfig), *GetNameSafe(AuthoredShellClass));

			// And whether the promotion it is doing instead actually works. Nothing else asks: this code
			// changes nothing here, so without this a config promoting to an actor nothing drives, or under
			// a management class that cannot hide one, is reported by the line above as a working promotion.
			FString PromotionProblem;
			UE_CLOG(!FCrowdyMassNearTierAuthoredConfig::PromotesUsably(*AuthoredTrait, PromotionProblem),
				LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: and that promotion is not one this plugin can drive: Mass entity config '%s' %s. Entities of '%s' promoted through it will not behave as a near tier."),
				*GetNameSafe(AuthoredConfig), *PromotionProblem, *EntityClass->GetPathName());
		}

		NearTierPatchedConfigByClass.Add(EntityClass, Entry);
		return AuthoredConfig;
	}

	// Naming an actor while the nearest level is still drawn as an instance promotes nothing, so a config in
	// that state has a half-wired near tier rather than a decision. The declared shell replaces what it
	// names, because that one has been checked for the component that drives it and the config's has not.
	//
	// Said whenever the config named ANYTHING, including the same class, because "the near tier is off in
	// the config and on anyway" is the outcome with no other explanation: turning promotion off by clearing
	// the level while leaving the actor reference is the natural way to do it and is not one this honours.
	const UClass* HalfWiredShellClass = AuthoredTrait->HighResTemplateActor;
	UE_CLOG(HalfWiredShellClass != nullptr, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: Mass entity config '%s' names high resolution actor '%s' while its nearest level is still drawn as an instance, so it promotes nothing on its own. '%s' declares a near tier shell, so it is promoted to '%s' from %s instead. Set the nearest level to a high resolution spawned actor in that config if its own actor was meant to be used, or remove the near tier shell component from the class if promotion was meant to be off."),
		*GetNameSafe(AuthoredConfig), *GetNameSafe(HalfWiredShellClass),
		*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route));

	const UClass* ReplacedManagementClass = nullptr;
	UMassEntityConfigAsset* PatchedConfig = FCrowdyMassNearTierAuthoredConfig::BuildPatchedConfig(*this,
		*AuthoredConfig, *AuthoredTrait, NearTierActorClass, ReplacedManagementClass);

	if (!PatchedConfig)
	{
		if (!ReportedNearTierShellNotConsumedClasses.Contains(EntityClass))
		{
			ReportedNearTierShellNotConsumedClasses.Add(EntityClass);
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' declares near tier actor '%s' on %s and Mass entity config '%s' left room for it, but a per-class copy of that config's visualization trait could not be made, so its entities stay crowd rows at every distance."),
				*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route),
				*GetNameSafe(AuthoredConfig));
		}

		NearTierPatchedConfigByClass.Add(EntityClass, Entry);
		return AuthoredConfig;
	}

	Entry.PatchedConfig = PatchedConfig;
	NearTierPatchedConfigByClass.Add(EntityClass, Entry);

	// Said rather than done quietly, because what is displaced can be doing real work: a management class
	// from another crowd plugin is a sibling of this plugin's rather than a child, and may carry pooled-actor
	// fixups this one has no equivalent of.
	UE_CLOG(ReplacedManagementClass != nullptr, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: promoting '%s' also replaced the representation actor management class Mass entity config '%s' names, '%s', with this plugin's own, because a near tier actor is hidden, placed and made ready by that class and only this plugin's does it the way a crowd row handover needs. Anything the replaced class did for its own actors no longer happens for entities of this class."),
		*EntityClass->GetPathName(), *GetNameSafe(AuthoredConfig), *GetNameSafe(ReplacedManagementClass));

	// The bands BELOW the near tier are not this code's to rewrite, and one default in them cannot work:
	// both engine visualization traits ask the band below for a spawned actor and neither ever names a low
	// resolution template, which logs two errors per entity per frame once entities reach it.
	const int32 UnusableBand = FCrowdyMassNearTierAuthoredConfig::FindUnusableSpawnedActorBand(*AuthoredTrait);
	UE_CLOG(UnusableBand != INDEX_NONE, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: Mass entity config '%s' asks level %d to spawn a low resolution actor and names no low resolution template actor, so entities of '%s' that fall to that level are drawn through a fallback and log an error every frame. Set that level to an instanced representation, or give the config a low resolution template actor."),
		*GetNameSafe(AuthoredConfig), UnusableBand, *EntityClass->GetPathName());

	// The band and the ceiling are the authored config's own and are deliberately left alone: they are
	// somebody's LOD table, and a near tier changes what the nearest level DRAWS rather than where it
	// begins. They are said here because that makes them different from a config built from a class, where
	// the project settings decide both, and a silent difference is the kind nobody finds. The engine also
	// logs "Overriding <trait> with another instance" as this config builds; that line is this working.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' declares near tier actor '%s' on %s, and Mass entity config '%s' authored for it does not promote on its own, so its entities are promoted through a per-class copy of that config. Its own level distances decide when (%.0f cm, %.0f cm visible), at most %d at once; the near tier project settings do not apply to an authored config."),
		*EntityClass->GetPathName(), *NearTierActorClass->GetPathName(), LexToString(DerivedShell.Route),
		*GetNameSafe(AuthoredConfig),
		AuthoredTrait->LODParams.BaseLODDistance[EMassLOD::Medium],
		AuthoredTrait->LODParams.VisibleLODDistance[EMassLOD::Medium],
		AuthoredTrait->LODParams.LODMaxCount[EMassLOD::High]);

	return PatchedConfig;
}

FMassEntityManager* UCrowdyMassEntitySubsystem::GetMassEntityManager() const
{
#if WITH_DEV_AUTOMATION_TESTS
	// A headless test that injected its own manager (no World, no MassEntitySubsystem) reads it here
	// before anything below assumes either exists.
	if (MassEntityManagerForTest)
	{
		return MassEntityManagerForTest;
	}
#endif

	if (!IsValid(MassEntitySubsystem))
	{
		return nullptr;
	}

	// During world teardown UMassEntitySubsystem::Deinitialize() resets its entity manager, and
	// world-subsystem Deinitialize order is NOT guaranteed, so by the time our Deinitialize (or the
	// backend's DeinitializeBackend) runs, the Mass manager may already be gone and
	// GetMutableEntityManager() would assert. BeginTearingDown() runs before CleanupWorld() (which
	// deinitializes subsystems) in every teardown path, so bIsTearingDown is reliably set first. Bail
	// out here: Mass destroys all of its own entities on teardown, so there is nothing for us to do.
	const UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown)
	{
		return nullptr;
	}

	return &MassEntitySubsystem->GetMutableEntityManager();
}

float UCrowdyMassEntitySubsystem::GetDefaultSmoothingDuration() const
{
	// A non-negative per-backend override wins; otherwise fall back to the project-wide setting.
	if (SmoothingDurationOverride >= 0.0f)
	{
		return SmoothingDurationOverride;
	}

	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->DefaultSmoothingDuration, 0.0f) : 0.1f;
}

FCrowdyInterpolationTuning UCrowdyMassEntitySubsystem::GetInterpolationTuning() const
{
	FCrowdyInterpolationTuning Tuning;
	Tuning.FirstUpdateDuration = GetDefaultSmoothingDuration();

	if (const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>())
	{
		Tuning.bAdaptive = Settings->bAdaptiveSmoothing;
		Tuning.Overshoot = Settings->SmoothingIntervalOvershoot;
		Tuning.MinDuration = Settings->MinSmoothingDuration;
		Tuning.MaxDuration = Settings->MaxSmoothingDuration;
	}

	CrowdyMassInterpolationTuning::ApplyRuntimeOverrides(Tuning);
	return Tuning;
}

bool UCrowdyMassEntitySubsystem::IsSpatialGridEnabled() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? Settings->bEnableSpatialGrid : true;
}

ECrowdyMassSpatialGridMode UCrowdyMassEntitySubsystem::GetSpatialGridMode() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? Settings->SpatialGridMode : ECrowdyMassSpatialGridMode::TwoDimensional;
}

float UCrowdyMassEntitySubsystem::GetSpatialGridCellSize() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->SpatialGridCellSize, 100.0f) : 1600.0f;
}

float UCrowdyMassEntitySubsystem::GetSpatialGridEntityBoundsRadius() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->SpatialGridEntityBoundsRadius, 0.0f) : 250.0f;
}

float UCrowdyMassEntitySubsystem::GetSpatialGridTraceQueryRadius() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->SpatialGridTraceQueryRadius, 0.0f) : 0.0f;
}

int32 UCrowdyMassEntitySubsystem::GetSpatialGridMaxRayCells() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->SpatialGridMaxRayCells, 1) : 4096;
}

float UCrowdyMassEntitySubsystem::GetSpatialGridMaxEntitySweepRadius() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->SpatialGridMaxEntitySweepRadius, 0.0f) : 12800.0f;
}

float UCrowdyMassEntitySubsystem::GetSpatialGridDebugTwoDimensionalHeightPadding() const
{
	const UCrowdyMassDeveloperSettings* Settings = GetDefault<UCrowdyMassDeveloperSettings>();
	return Settings ? FMath::Max(Settings->SpatialGridDebugTwoDimensionalHeightPadding, 0.0f) : 0.0f;
}

FIntVector UCrowdyMassEntitySubsystem::GetSpatialGridCell(const FVector& Location) const
{
	const float CellSize = GetSpatialGridCellSize();
	const int32 X = FMath::FloorToInt(Location.X / CellSize);
	const int32 Y = FMath::FloorToInt(Location.Y / CellSize);
	const int32 Z = GetSpatialGridMode() == ECrowdyMassSpatialGridMode::ThreeDimensional
		? FMath::FloorToInt(Location.Z / CellSize)
		: 0;

	return FIntVector(X, Y, Z);
}

FBox UCrowdyMassEntitySubsystem::GetSpatialGridDebugCellBounds(const FIntVector& Cell, const FVector& Start, const FVector& End) const
{
	const float CellSize = GetSpatialGridCellSize();
	const bool bUse3D = GetSpatialGridMode() == ECrowdyMassSpatialGridMode::ThreeDimensional;

	FVector Min(static_cast<double>(Cell.X) * CellSize, static_cast<double>(Cell.Y) * CellSize, 0.0);
	FVector Max(static_cast<double>(Cell.X + 1) * CellSize, static_cast<double>(Cell.Y + 1) * CellSize, 0.0);

	if (bUse3D)
	{
		Min.Z = static_cast<double>(Cell.Z) * CellSize;
		Max.Z = static_cast<double>(Cell.Z + 1) * CellSize;
	}
	else
	{
		const float Padding = GetSpatialGridDebugTwoDimensionalHeightPadding();
		Min.Z = FMath::Min(Start.Z, End.Z) - Padding;
		Max.Z = FMath::Max(Start.Z, End.Z) + Padding;
	}

	return FBox(Min, Max);
}

bool UCrowdyMassEntitySubsystem::GetSpatialGridCellsForSphere(const FVector& Origin, const float Radius, const int32 MaxCells, TArray<FIntVector>& OutCells) const
{
	OutCells.Reset();

	const double ClampedRadius = FMath::Max(static_cast<double>(Radius), 0.0);
	if (Origin.ContainsNaN() || !FMath::IsFinite(ClampedRadius))
	{
		return false;
	}

	// Sized in doubles before anything is added, so a huge radius or position is refused rather than enumerated or overflowed.
	const double CellSize = GetSpatialGridCellSize();
	const bool bUse3D = GetSpatialGridMode() == ECrowdyMassSpatialGridMode::ThreeDimensional;
	// A true division per axis, as GetSpatialGridCell does; a vector divide multiplies by the reciprocal and can land a boundary one cell off.
	auto CellOf = [CellSize](const double Coordinate) { return FMath::FloorToDouble(Coordinate / CellSize); };
	const FVector Low(CellOf(Origin.X - ClampedRadius), CellOf(Origin.Y - ClampedRadius), bUse3D ? CellOf(Origin.Z - ClampedRadius) : 0.0);
	const FVector High(CellOf(Origin.X + ClampedRadius), CellOf(Origin.Y + ClampedRadius), bUse3D ? CellOf(Origin.Z + ClampedRadius) : 0.0);
	const FVector Span = High - Low + FVector(1.0);
	if (Low.GetMin() < MIN_int32 || High.GetMax() >= MAX_int32 || Span.X * Span.Y * Span.Z > FMath::Max(MaxCells, 1))
	{
		return false;
	}

	const FIntVector MinCell(static_cast<int32>(Low.X), static_cast<int32>(Low.Y), static_cast<int32>(Low.Z));
	const FIntVector MaxCell(static_cast<int32>(High.X), static_cast<int32>(High.Y), static_cast<int32>(High.Z));
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				OutCells.Add(FIntVector(X, Y, Z));
			}
		}
	}

	return true;
}

bool UCrowdyMassEntitySubsystem::GetSpatialGridCellsForRay(const FVector& Start, const FVector& End, const float QueryRadius, const int32 MaxCells, TArray<FIntVector>& OutCells) const
{
	OutCells.Reset();

	const float CellSize = GetSpatialGridCellSize();
	const bool bUse3D = GetSpatialGridMode() == ECrowdyMassSpatialGridMode::ThreeDimensional;
	const int32 ClampedMaxCells = FMath::Max(MaxCells, 1);
	const int32 RadiusCells = FMath::Max(FMath::CeilToInt(FMath::Max(QueryRadius, 0.0f) / CellSize), 0);
	const FIntVector StartCell = GetSpatialGridCell(Start);
	const FIntVector EndCell = GetSpatialGridCell(End);

	// Kept across calls and reset here; see the scratch members' declaration. Not reserved, because the
	// cell ceiling is thousands and a real ray touches a handful.
	TSet<FIntVector>& SeenCells = ScratchRaySeenCells;
	SeenCells.Reset();

	auto TryAddCell = [&OutCells, &SeenCells, ClampedMaxCells](const FIntVector& Cell)
	{
		if (SeenCells.Contains(Cell))
		{
			return true;
		}

		if (OutCells.Num() >= ClampedMaxCells)
		{
			OutCells.Reset();
			return false;
		}

		SeenCells.Add(Cell);
		OutCells.Add(Cell);
		return true;
	};

	auto TryAddExpandedCell = [RadiusCells, bUse3D, &TryAddCell](const FIntVector& CenterCell)
	{
		const int32 MinZ = bUse3D ? CenterCell.Z - RadiusCells : 0;
		const int32 MaxZ = bUse3D ? CenterCell.Z + RadiusCells : 0;
		for (int32 X = CenterCell.X - RadiusCells; X <= CenterCell.X + RadiusCells; ++X)
		{
			for (int32 Y = CenterCell.Y - RadiusCells; Y <= CenterCell.Y + RadiusCells; ++Y)
			{
				for (int32 Z = MinZ; Z <= MaxZ; ++Z)
				{
					if (!TryAddCell(FIntVector(X, Y, Z)))
					{
						return false;
					}
				}
			}
		}

		return true;
	};

	if (!TryAddExpandedCell(StartCell))
	{
		return false;
	}

	if (StartCell == EndCell || (End - Start).SizeSquared() <= UE_KINDA_SMALL_NUMBER)
	{
		return true;
	}

	FIntVector CurrentCell = StartCell;
	const FVector Delta = End - Start;
	const double InfiniteT = TNumericLimits<double>::Max();

	auto ConfigureAxis = [CellSize, InfiniteT](const double StartCoordinate, const double DeltaCoordinate, const int32 CellCoordinate, int32& OutStep, double& OutTMax, double& OutTDelta)
	{
		if (FMath::Abs(DeltaCoordinate) <= UE_KINDA_SMALL_NUMBER)
		{
			OutStep = 0;
			OutTMax = InfiniteT;
			OutTDelta = InfiniteT;
			return;
		}

		OutStep = DeltaCoordinate > 0.0 ? 1 : -1;
		const double Boundary = OutStep > 0
			? static_cast<double>(CellCoordinate + 1) * CellSize
			: static_cast<double>(CellCoordinate) * CellSize;
		OutTMax = FMath::Max((Boundary - StartCoordinate) / DeltaCoordinate, 0.0);
		OutTDelta = CellSize / FMath::Abs(DeltaCoordinate);
	};

	int32 StepX = 0;
	int32 StepY = 0;
	int32 StepZ = 0;
	double TMaxX = InfiniteT;
	double TMaxY = InfiniteT;
	double TMaxZ = InfiniteT;
	double TDeltaX = InfiniteT;
	double TDeltaY = InfiniteT;
	double TDeltaZ = InfiniteT;

	ConfigureAxis(Start.X, Delta.X, StartCell.X, StepX, TMaxX, TDeltaX);
	ConfigureAxis(Start.Y, Delta.Y, StartCell.Y, StepY, TMaxY, TDeltaY);
	if (bUse3D)
	{
		ConfigureAxis(Start.Z, Delta.Z, StartCell.Z, StepZ, TMaxZ, TDeltaZ);
	}

	const int32 MaxSteps = FMath::Abs(EndCell.X - StartCell.X)
		+ FMath::Abs(EndCell.Y - StartCell.Y)
		+ (bUse3D ? FMath::Abs(EndCell.Z - StartCell.Z) : 0)
		+ 3;
	int32 Steps = 0;

	while (CurrentCell != EndCell)
	{
		if (++Steps > MaxSteps)
		{
			OutCells.Reset();
			return false;
		}

		const double NextT = FMath::Min(TMaxX, FMath::Min(TMaxY, TMaxZ));
		constexpr double TieTolerance = 1.e-8;
		if (StepX != 0 && TMaxX <= NextT + TieTolerance)
		{
			CurrentCell.X += StepX;
			TMaxX += TDeltaX;
		}
		if (StepY != 0 && TMaxY <= NextT + TieTolerance)
		{
			CurrentCell.Y += StepY;
			TMaxY += TDeltaY;
		}
		if (bUse3D && StepZ != 0 && TMaxZ <= NextT + TieTolerance)
		{
			CurrentCell.Z += StepZ;
			TMaxZ += TDeltaZ;
		}

		if (!TryAddExpandedCell(CurrentCell))
		{
			return false;
		}
	}

	return true;
}

void UCrowdyMassEntitySubsystem::UpdateSpatialGridEntity(const FMassEntityHandle Entity, const FVector& Origin, const float Radius)
{
	if (!IsSpatialGridEnabled())
	{
		// Still removed, because the grid can be turned off while entries from before it was are held.
		RemoveSpatialGridEntity(Entity);
		return;
	}

	// Shrunk to the widest box the cap holds rather than refused, so an entity claiming a huge sweep stays placed near where it says it is.
	// A radius R covers at most 2R/CellSize + 2 cells per axis.
	const int32 MaxCells = GetSpatialGridMaxRayCells();
	const bool bUse3D = GetSpatialGridMode() == ECrowdyMassSpatialGridMode::ThreeDimensional;
	const int32 CellsPerAxis = FMath::FloorToInt(bUse3D ? FMath::Pow(static_cast<double>(MaxCells), 1.0 / 3.0) : FMath::Sqrt(static_cast<double>(MaxCells)));
	const float MaxRadius = FMath::Max(CellsPerAxis - 2, 0) * GetSpatialGridCellSize() * 0.5f;

	// Only input that is not finite or past the cell index range is refused, and that keeps the last placement.
	TArray<FIntVector> Cells;
	if (!GetSpatialGridCellsForSphere(Origin, FMath::Min(Radius, MaxRadius), MaxCells, Cells))
	{
		return;
	}

	// The common case for a crowd: an entity moved without leaving the cells it already covered, so the
	// removal and re-insertion below would put it back exactly where it already is. The comparison is
	// ordered rather than set-wise, which is sound because both sides come from the same generator walking
	// the same bounds in the same order; an equal set in a different order simply falls through and rebuilds.
	if (const FCrowdyMassSpatialGridEntry* Existing = SpatialGridEntries.Find(Entity);
		Existing && Existing->Cells == Cells)
	{
		FCrowdyMassSpatialGridEntry& Unmoved = SpatialGridEntries[Entity];
		Unmoved.Origin = Origin;
		Unmoved.Radius = FMath::Max(Radius, 0.0f);
		return;
	}

	RemoveSpatialGridEntity(Entity);

	FCrowdyMassSpatialGridEntry& Entry = SpatialGridEntries.Add(Entity);
	Entry.Origin = Origin;
	Entry.Radius = FMath::Max(Radius, 0.0f);
	Entry.Cells = MoveTemp(Cells);

	for (const FIntVector& Cell : Entry.Cells)
	{
		SpatialGridCells.FindOrAdd(Cell).Add(Entity);
	}
}

void UCrowdyMassEntitySubsystem::RemoveSpatialGridEntity(const FMassEntityHandle Entity)
{
	const FCrowdyMassSpatialGridEntry* ExistingEntry = SpatialGridEntries.Find(Entity);
	if (!ExistingEntry)
	{
		return;
	}

	for (const FIntVector& Cell : ExistingEntry->Cells)
	{
		if (TSet<FMassEntityHandle>* CellEntities = SpatialGridCells.Find(Cell))
		{
			CellEntities->Remove(Entity);
			if (CellEntities->IsEmpty())
			{
				SpatialGridCells.Remove(Cell);
			}
		}
	}

	SpatialGridEntries.Remove(Entity);
}

void UCrowdyMassEntitySubsystem::RegisterViewStateField(const FName PropertyName, FCrowdyMassViewStateIngestor Ingestor)
{
	if (PropertyName.IsNone() || !Ingestor)
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: Refusing to register a view-state field with no name or no ingestor."));
		return;
	}

	ViewStateFieldRegistrations.Add(MakeUnique<FCrowdyMassViewStateFieldRegistration>(
		FCrowdyMassViewStateFieldRegistration{PropertyName, MoveTemp(Ingestor)}));

	// A field registered after some struct has already been resolved would sit at an index no earlier
	// resolution has an entry for. Registration only happens at startup in practice (CrowdyMass's own
	// two standard fields, Version and PoseId, then whatever a game adds during its own
	// initialization), so clearing here is cheap and keeps the invariant simple: every
	// ViewStateResolvedStructs entry is always exactly as long as ViewStateFieldRegistrations.
	//
	// This can also be reached from inside an ingestor, one call away from any game code an ingestor
	// touches. That is why an update in progress holds its own reference to the entry it is walking
	// rather than reading it back out of this map: see ApplyViewStateToEntity.
	ViewStateResolvedStructs.Empty();
}

bool UCrowdyMassEntitySubsystem::IsViewStateFieldRegistered(const FName PropertyName) const
{
	if (PropertyName.IsNone())
	{
		return false;
	}

	for (const TUniquePtr<FCrowdyMassViewStateFieldRegistration>& Registration : ViewStateFieldRegistrations)
	{
		if (Registration->PropertyName == PropertyName)
		{
			return true;
		}
	}

	return false;
}

bool UCrowdyMassEntitySubsystem::ApplyViewState(const FGuid& UUID, const FInstancedStruct& State)
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(UUID, Entity))
	{
		return false;
	}

	FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager)
	{
		return false;
	}

	// No second validity check: FindEntityByUUID validated this handle against the same entity manager and
	// nothing between there and here can destroy the entity. The re-checks on the event path are a
	// different matter, since arbitrary game code runs between the resolve and the fragment reads there.
	ApplyViewStateToEntity(State, *EntityManager, Entity);
	return true;
}

bool UCrowdyMassEntitySubsystem::ApplyRemoteEntityUpdate(const FGuid& UUID, const FInstancedStruct& State, const int64 ServerTimestampMs)
{
	FMassEntityHandle Entity;
	if (!FindEntityByUUID(UUID, Entity))
	{
		return false;
	}

	FMassEntityManager* EntityManager = GetMassEntityManager();
	if (!EntityManager)
	{
		return false;
	}

	// Ahead of the transform, and not conditional on it: a payload whose transform does not resolve must
	// still deliver the view-state fields it carries.
	ApplyViewStateToEntity(State, *EntityManager, Entity);

	FVector Location;
	FQuat Rotation;
	float SmoothingDuration = -1.0f;
	if (!TryExtractTransformState(State, Location, Rotation, SmoothingDuration))
	{
		return true;
	}

	// Re-checked here and nowhere above: an ingestor is contracted not to destroy the entity, but it is the
	// only game-reachable code on this path and everything below reaches straight for fragment memory.
	if (!EntityManager->IsEntityValid(Entity))
	{
		return true;
	}

	ApplyRemoteEntityTransformToEntity(*EntityManager, Entity, Location, Rotation, ServerTimestampMs, SmoothingDuration);
	return true;
}

void UCrowdyMassEntitySubsystem::ApplyViewStateToEntity(const FInstancedStruct& State, FMassEntityManager& EntityManager, const FMassEntityHandle Entity)
{
	// The fragment-write share of per-message cost, which is the part payload size cannot change. Traced
	// beside the decode scope so the two can be compared: if this dominates, shrinking the wire moves the
	// receive ceiling very little and the honest lever is elsewhere.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_ApplyViewStateToEntity);

	if (!State.IsValid())
	{
		return;
	}

	const UScriptStruct* ScriptStruct = State.GetScriptStruct();
	const void* StructMemory = State.GetMemory();
	if (!ScriptStruct || !StructMemory)
	{
		return;
	}

	// Held for the whole loop rather than read back out of ViewStateResolvedStructs per field. An
	// ingestor is one call away from RegisterViewStateField, directly or through anything it broadcasts
	// to, and that call empties the resolution cache and appends to the registration array. Keeping the
	// resolved list alive here, and reaching each registration through its own stable allocation below,
	// is what stops a registration made from inside an ingestor freeing the list being walked or moving
	// the ingestor that is running. A field registered mid-update simply lands from the next update on,
	// since the field count is the one taken before the loop and registration only ever appends.
	const FCrowdyMassViewStateResolvedStructConstRef Resolved = ResolveViewStateStruct(*ScriptStruct);

	// One context for the whole update rather than one per field, so that fields sharing a fragment
	// share the lookup that found it: see FCrowdyMassViewStateContext::GetFragment.
	FCrowdyMassViewStateContext Context{EntityManager, Entity};

	const int32 FieldCount = Resolved->Fields.Num();
	for (int32 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
	{
		const FProperty* Property = Resolved->Fields[FieldIndex].Property;
		if (!Property)
		{
			// This particular wire struct does not carry the field this registration is for (a
			// game's field on another game's struct, or any field on a struct that predates it). The
			// entity keeps whatever it already had for it.
			continue;
		}

		const FCrowdyMassViewStateFieldRegistration& Registration = *ViewStateFieldRegistrations[FieldIndex];
		const void* PropertyValuePtr = Property->ContainerPtrToValuePtr<void>(StructMemory);
		Registration.Ingestor(PropertyValuePtr, Context);
	}
}

FCrowdyMassViewStateResolvedStructConstRef UCrowdyMassEntitySubsystem::ResolveViewStateStruct(const UScriptStruct& ScriptStruct)
{
	if (const FCrowdyMassViewStateResolvedStructRef* Existing = ViewStateResolvedStructs.Find(&ScriptStruct))
	{
		return *Existing;
	}

	const FCrowdyMassViewStateResolvedStructRef Resolved = ViewStateResolvedStructs.Add(
		&ScriptStruct, MakeShared<FCrowdyMassViewStateResolvedStruct, ESPMode::NotThreadSafe>());
	Resolved->Fields.Reserve(ViewStateFieldRegistrations.Num());
	for (const TUniquePtr<FCrowdyMassViewStateFieldRegistration>& RegistrationPtr : ViewStateFieldRegistrations)
	{
		const FCrowdyMassViewStateFieldRegistration& Registration = *RegistrationPtr;
		FCrowdyMassViewStateResolvedField& Field = Resolved->Fields.AddDefaulted_GetRef();

		// Matched on name AND on being a single byte, never on name alone. Fields are claimed by name
		// across every wire struct in the process, including structs from games this build has never
		// seen, so a name match on its own would let an unrelated struct's field of the same name be
		// read through a byte pointer. A float of the registered name holding a percentage from 0 to
		// 100, which is the natural reading of many such names, would be read as the low byte of its
		// mantissa: 73.0 and 100.0 both give zero, so whatever the byte drives would sit at nothing,
		// and a non-round value would make it flicker. Refusing to bind is the honest outcome, and it
		// leaves the field untouched exactly as an absent property does.
		const FProperty* Candidate = ScriptStruct.FindPropertyByName(Registration.PropertyName);
		if (!Candidate)
		{
			continue;
		}

		// A byte-wide enum counts as a byte. It is how a game ordinarily declares a state, it sits at the
		// same address and is the same width as a plain uint8, and refusing it would look exactly like the
		// field never arriving.
		if (!CrowdyMassIsSingleByteProperty(*Candidate))
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' carries a property named '%s', but it is a %s rather than a single byte, so the view-state echo will not read it. Rename that property, or declare it as a uint8, a TEnumAsByte, or an enum class with a uint8 underlying type."),
				*ScriptStruct.GetName(), *Registration.PropertyName.ToString(), *Candidate->GetClass()->GetName());
			continue;
		}

		Field.Property = Candidate;
	}

	return Resolved;
}

void UCrowdyMassEntitySubsystem::RegisterStandardViewStateFields()
{
	// Version carries no information this consumer branches on: there is only one wire layout today
	// (CrowdyMassViewStateWireVersion in View/CrowdyMassViewState.h), and every field below is found by
	// name through reflection, so an older or newer producer's fields land at the right offset no
	// matter what it wrote here. CrowdyMassViewStateWireVersionUnspecified is what a payload from a
	// peer predating view state reads as, and it is treated exactly like the current version: never
	// gating decode on it is what keeps such a payload reading as the resting state the wire struct's
	// own field defaults already encode. A version above what this build knows about is decoded through
	// the same (only) layout as a forward-compatible best effort and reported once in a while, purely
	// as a diagnostic that a peer is running a newer build.
	RegisterViewStateField(TEXT("Version"), [this](const void* PropertyValuePtr, const FCrowdyMassViewStateContext&)
	{
		const uint8 EchoedVersion = *static_cast<const uint8*>(PropertyValuePtr);
		if (EchoedVersion <= CrowdyMassViewStateWireVersion)
		{
			return;
		}

		const double Now = FPlatformTime::Seconds();
		if (Now < NextUnknownViewStateVersionWarningSeconds)
		{
			return;
		}

		NextUnknownViewStateVersionWarningSeconds = Now + 5.0;
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: View-state echo carries wire version %d, newer than the %d this build decodes. Fields this build does not recognise are ignored rather than rejected."),
			EchoedVersion, CrowdyMassViewStateWireVersion);
	});

	// DELETED 2026-08-23: the pose no longer rides the view-state echo, and CrowdyMass registers nothing
	// on this plane but the version above.
	//
	// There used to be a standard field here claiming the name "PoseId" and writing the arriving byte
	// straight onto the pose fragment. That byte was a ROW ID chosen by the sender, which is exactly what
	// Phase A stopped trusting: it could name a row this client deliberately left unmapped, and it made
	// the pose the one piece of view state with two transports. The pose now arrives on the CrowdyState
	// plane as the game's own enum VALUE and is mapped to a row against this client's own authored set
	// (see RegisterStandardStateFragmentFields).
	//
	// Removed rather than left unused, because a name claimed here is claimed PROCESS-WIDE across every
	// wire struct: leaving it would have kept the unhardened road reachable by any game whose payload
	// happens to carry a byte called PoseId. The echo plane itself is untouched and still does its job for
	// fields a game registers by name through RegisterViewStateField.
}

void UCrowdyMassEntitySubsystem::RegisterStateFragmentField(const FName PropertyName,
	const ECrowdyMassStateFieldType FieldType, FCrowdyMassViewStateIngestor Ingestor)
{
	RegisterStateFragmentFieldInternal(PropertyName, FieldType, MoveTemp(Ingestor), /*bIsStandard=*/false);
}

bool UCrowdyMassEntitySubsystem::IsStateFragmentFieldRegistered(const FName PropertyName) const
{
	if (PropertyName.IsNone())
	{
		return false;
	}

	for (const TUniquePtr<FCrowdyMassStateFieldRegistration>& Registration : StateFieldRegistrations)
	{
		if (Registration->PropertyName == PropertyName)
		{
			return true;
		}
	}

	return false;
}

void UCrowdyMassEntitySubsystem::RegisterStateFragmentFieldInternal(const FName PropertyName,
	const ECrowdyMassStateFieldType FieldType, FCrowdyMassViewStateIngestor Ingestor, const bool bIsStandard,
	const ECrowdyMassStateFieldMatch Match)
{
	const bool bMatchesByName = Match == ECrowdyMassStateFieldMatch::ByName;
	if (!Ingestor || (bMatchesByName && PropertyName.IsNone()))
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassEntitySubsystem]: Refusing to register a CrowdyState fragment field with no name or no ingestor."));
		return;
	}

	// A second registration under a name already taken is not an error, but it is silently dead: a layout
	// slot is claimed by the first registration that matches its name, so the later one is never called
	// and the caller has no way to notice. Said out loud, because a game registering its own field is
	// following a public call whose failure mode is otherwise a field that simply never arrives.
	UE_CLOG(bMatchesByName && IsStateFragmentFieldRegistered(PropertyName), LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: A CrowdyState fragment field named '%s' is already registered in this world. The earlier registration keeps the field and this one will never be called."),
		*PropertyName.ToString());

	StateFieldRegistrations.Add(MakeUnique<FCrowdyMassStateFieldRegistration>(
		FCrowdyMassStateFieldRegistration{PropertyName, MoveTemp(Ingestor), FieldType, Match, bIsStandard}));

	// Mirrors RegisterViewStateField: a field registered after some class has already been resolved would
	// sit at an index no earlier resolution has an entry for, so every cached resolution is dropped rather
	// than patched. Cheap in practice, since registration only happens at startup (CrowdyMass's own
	// standard field, then whatever a game adds during its own initialization).
	StateResolvedClasses.Empty();
	StateFieldAbsentReportedClasses.Empty();
}

void UCrowdyMassEntitySubsystem::RegisterStandardStateFragmentFields()
{
	// CrowdyMass's own first field on this path, landing on the same fragment the view-state echo above
	// writes. It is matched by TYPE rather than by name: the class's animation set already names the enum
	// its pose rows are values of, so the game declares that enum once, on the set, and is then free to
	// call the replicated property whatever suits it.
	//
	// The arriving value is the GAME's own enum value and is not an id. Turning it into one happens here,
	// once per arriving delta, which is what keeps the fragment holding a row index and the per-entity
	// animation loop down to one bounds check and one array index. A value no row authors becomes 0 and
	// clears the pose, which is why a living player needs no authored row at all.
	RegisterStateFragmentFieldInternal(NAME_None, ECrowdyMassStateFieldType::UInt8, [this](const void* PropertyValuePtr, const FCrowdyMassViewStateContext& Context)
	{
		FCrowdyMassViewPoseFragment* PoseFragment = Context.GetFragment<FCrowdyMassViewPoseFragment>();
		if (!PoseFragment)
		{
			return;
		}

		// The other half of the attribution pair above: this counter is the one that says the CrowdyState
		// plane actually delivered a pose, as opposed to the echo delivering it while this path was dead.
		++PoseWritesFromState;

		const uint8 StateValue = *static_cast<const uint8*>(PropertyValuePtr);
		const int32 ResolvedPoseId = MapPoseStateValueToPoseId(Context.EntityClass, static_cast<int64>(StateValue));

		// Counted on the RESOLVED id rather than on the arriving value, because what this pair answers is
		// whether the state plane delivered a pose anyone would see. An entity that is alive sends its
		// enum's default on every heartbeat and that value normally names no row, so counting arrivals
		// would make a road that has never shown anything look busy.
		const uint8 DeliveredPoseId = static_cast<uint8>(ResolvedPoseId);
		if (DeliveredPoseId != 0)
		{
			++NonZeroPoseWritesFromState;
			LastNonZeroPoseFromState = DeliveredPoseId;
		}

		PoseFragment->PoseId = DeliveredPoseId;
	}, /*bIsStandard=*/true, ECrowdyMassStateFieldMatch::ByPoseStateEnum);
}

FCrowdyMassStateResolvedClassConstRef UCrowdyMassEntitySubsystem::ResolveStateFragmentClass(const UClass& Class, const FCrowdyRepLayout& Layout)
{
	if (const FCrowdyMassStateResolvedClassRef* Existing = StateResolvedClasses.Find(&Class))
	{
		if ((*Existing)->LayoutHash == Layout.LayoutHash)
		{
			return *Existing;
		}
	}

	// Worked out BEFORE the entry below is added to the map, because resolving a class's animation set
	// loads assets, and the editor hook that watches those assets empties this very map. An entry added
	// first could be dropped while it was still being filled, leaving this resolution correct for the
	// delta in hand but detached from the cache it was meant to become.
	ECrowdyMassAnimSetSource AnimSetSource = ECrowdyMassAnimSetSource::NoClass;
	const UEnum* const PoseStateEnum = FindPoseStateEnum(&Class, AnimSetSource);

	const FCrowdyMassStateResolvedClassRef Resolved = StateResolvedClasses.Add(
		&Class, MakeShared<FCrowdyMassStateResolvedClass, ESPMode::NotThreadSafe>());
	Resolved->LayoutHash = Layout.LayoutHash;
	Resolved->RegistrationIndexByLayoutIndex.Init(INDEX_NONE, Layout.Properties.Num());

	const int32 RegistrationCount = StateFieldRegistrations.Num();
	TArray<bool> RegistrationClaimed;
	RegistrationClaimed.Init(false, RegistrationCount);

	// Layout slots that end up bound to nothing for a reason this function has ALREADY said out loud: a
	// property refused for being too wide to read, or a second property of the pose enum. They are dropped
	// exactly like an unwanted property is, and the general "nothing claims this" line below would send
	// their author looking for a registration to write when the message they need is the specific one they
	// already have.
	TArray<bool> LayoutIndexExplained;
	LayoutIndexExplained.Init(false, Layout.Properties.Num());

	// Which property the pose field claimed, so a second property of the same enum can be reported as the
	// one that will never be read rather than silently losing to declaration order.
	FName ClaimedPoseStatePropertyName = NAME_None;

	// A property of the pose enum that a game's own field claimed first. Name-matched fields are offered
	// every property before the pose field is, so a game registering a field for the property it poses from
	// takes that slot. The pose field then ends unclaimed with the property still sitting on the class, and
	// saying the class carries no such property would send its author to declare a second one.
	FName PoseStatePropertyClaimedByName = NAME_None;

	// Registrations are offered every property in THIS order, name-matched ones first, whatever order they
	// were registered in. A game asked for a particular property by name; the pose field asked only for a
	// type, and several properties can answer a type. Letting the type match win would take a slot a game
	// explicitly claimed and say nothing about it, because a claimed slot is never reported and a type
	// match cannot know it displaced anything. The pose field registers first, in Initialize, so without
	// this the silent theft would be the ordinary case rather than an edge one.
	TArray<int32, TInlineAllocator<8>> RegistrationOrder;
	RegistrationOrder.Reserve(RegistrationCount);
	for (int32 RegistrationIndex = 0; RegistrationIndex < RegistrationCount; ++RegistrationIndex)
	{
		if (StateFieldRegistrations[RegistrationIndex]->Match == ECrowdyMassStateFieldMatch::ByName)
		{
			RegistrationOrder.Add(RegistrationIndex);
		}
	}
	for (int32 RegistrationIndex = 0; RegistrationIndex < RegistrationCount; ++RegistrationIndex)
	{
		if (StateFieldRegistrations[RegistrationIndex]->Match != ECrowdyMassStateFieldMatch::ByName)
		{
			RegistrationOrder.Add(RegistrationIndex);
		}
	}

	for (int32 LayoutIndex = 0; LayoutIndex < Layout.Properties.Num(); ++LayoutIndex)
	{
		const FCrowdyRepProperty& RepProperty = Layout.Properties[LayoutIndex];
		if (!RepProperty.Property)
		{
			continue;
		}

		const FName PropertyName = RepProperty.Property->GetFName();
		for (const int32 RegistrationIndex : RegistrationOrder)
		{
			if (RegistrationClaimed[RegistrationIndex])
			{
				continue;
			}

			const FCrowdyMassStateFieldRegistration& Registration = *StateFieldRegistrations[RegistrationIndex];

			// A game's own field is claimed by the name it registered. CrowdyMass's pose field is claimed
			// by the TYPE the class's animation set declares, so the property's own name says nothing
			// about whether it matches and a class with no set matches nothing at all.
			const bool bMatches = Registration.Match == ECrowdyMassStateFieldMatch::ByName
				? Registration.PropertyName == PropertyName
				: PoseStateEnum != nullptr && CrowdyMassGetByteEnum(*RepProperty.Property) == PoseStateEnum;
			if (!bMatches)
			{
				continue;
			}

			// Matched on identity AND on the type the field reads, exactly like the view-state echo's own
			// resolution (ResolveViewStateStruct) matches on being a byte: a match on identity alone would
			// let an unrelated property be read through the ingestor's pointer, misreading a float as an
			// int or as a pose id.
			//
			// The two matches want two different rules, and they are not the same rule wearing different
			// names. A game's field is accepted for the type it registered, because that is the type its
			// ingestor casts the value to; an enum counts as the type it is declared over, which is what
			// lets the ordinary `enum class E : uint8` declaration bind. The pose field is accepted only as
			// a single byte, whatever else the layout carries, because the pose ingestor reads a byte and
			// maps it to a row: an enum declared wider cannot be read at all and would otherwise be
			// truncated into whichever row its low byte happened to name.
			//
			// Refused BEFORE the registration is claimed, which matters only for the type-matched field
			// and matters a lot there: several properties can match one enum, so claiming first would let
			// a refused one consume the registration and leave a perfectly good property beside it
			// unbound for the life of the world, decided by declaration order and reported as nothing at
			// all. A name-matched field cannot hit that, since one layout carries one property per name.
			const bool bAccepted = Registration.Match == ECrowdyMassStateFieldMatch::ByPoseStateEnum
				? CrowdyMassIsSingleByteProperty(*RepProperty.Property)
				: CrowdyMassPropertyMatchesStateFieldType(*RepProperty.Property, Registration.FieldType);
			if (!bAccepted)
			{
				LayoutIndexExplained[LayoutIndex] = true;

				// Two different faults want two different sentences, and which one to say follows the MATCH
				// rather than whether the property names an enum. A wide enum can now reach either arm: the
				// pose's author has to change the enum's declaration, while a game that registered a field
				// for one type against a property declared as another has a choice of two ends to fix, and
				// telling that author to declare their enum over a byte would point at a rule their
				// registration is not held to.
				if (Registration.Match == ECrowdyMassStateFieldMatch::ByPoseStateEnum)
				{
					// The match above was made against PoseStateEnum, so it is the enum to name and cannot
					// be null on this path.
					UE_LOG(LogCrowdyMass, Warning,
						TEXT("[CrowdyMassEntitySubsystem]: '%s' declares '%s' as '%s', the enum its animation set names, but that enum's underlying type is wider than a byte, so it cannot be read as one and this subsystem will not read it. Declare the enum as `enum class %s : uint8`."),
						*Class.GetName(), *PropertyName.ToString(), *PoseStateEnum->GetName(), *PoseStateEnum->GetName());
					break;
				}

				UE_LOG(LogCrowdyMass, Warning,
					TEXT("[CrowdyMassEntitySubsystem]: '%s' carries a CrowdyState property named '%s', which is %s, but the field registered for that name reads it as %s, so this subsystem will not read it. %s A field name carries one type for the whole process, so where two classes declare a property of this name as different types, only one of them can be read and the other needs a name of its own."),
					*Class.GetName(), *PropertyName.ToString(),
					*CrowdyMassDescribeStateFieldProperty(*RepProperty.Property),
					CrowdyMassStateFieldTypeName(Registration.FieldType),
					*CrowdyMassStateFieldMismatchRemedyFor(*RepProperty.Property));
				break;
			}

			RegistrationClaimed[RegistrationIndex] = true;
			if (Registration.Match == ECrowdyMassStateFieldMatch::ByPoseStateEnum)
			{
				ClaimedPoseStatePropertyName = PropertyName;
			}
			else if (PoseStateEnum != nullptr && CrowdyMassGetByteEnum(*RepProperty.Property) == PoseStateEnum)
			{
				PoseStatePropertyClaimedByName = PropertyName;
			}

			// An owner-only property is delivered only to the entity's own owning client, and this
			// subsystem exists to show OTHER players as Mass rows: UCrowdyMassRenderingBackend skips the
			// locally-owned entity by default, so a crowd observer running this ingestor never receives a
			// delta carrying it. Binding still happens, in case that owner-skip is ever overridden, but
			// the mismatch is worth a diagnostic rather than a silent no-op.
			if (RepProperty.bOwnerOnly)
			{
				UE_LOG(LogCrowdyMass, Warning,
					TEXT("[CrowdyMassEntitySubsystem]: '%s' declares its CrowdyState property '%s' owner-only, so a crowd observer will never see it change. The ingestor registered for it is a permanent no-op unless the property is made observable."),
					*Class.GetName(), *PropertyName.ToString());
			}

			Resolved->RegistrationIndexByLayoutIndex[LayoutIndex] = RegistrationIndex;
			break;
		}
	}

	// A second CrowdyState property of the same enum is not a second pose: one animation set names one
	// enum, so the two would be asking for the same rows and only the earlier one can win. Whichever way
	// that goes is arbitrary to the author, so it is named rather than left to declaration order.
	if (!ClaimedPoseStatePropertyName.IsNone())
	{
		for (int32 LayoutIndex = 0; LayoutIndex < Layout.Properties.Num(); ++LayoutIndex)
		{
			const FCrowdyRepProperty& RepProperty = Layout.Properties[LayoutIndex];
			if (!RepProperty.Property || RepProperty.Property->GetFName() == ClaimedPoseStatePropertyName
				|| CrowdyMassGetByteEnum(*RepProperty.Property) != PoseStateEnum)
			{
				continue;
			}

			LayoutIndexExplained[LayoutIndex] = true;

			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' declares both '%s' and '%s' as CrowdyState properties of '%s', the enum its animation set names. Only '%s' drives the pose; the other is never read. Replicate one property of that enum, or give the second one a type of its own."),
				*Class.GetName(), *ClaimedPoseStatePropertyName.ToString(), *RepProperty.Property->GetName(),
				*PoseStateEnum->GetName(), *ClaimedPoseStatePropertyName.ToString());
		}
	}

	for (int32 RegistrationIndex = 0; RegistrationIndex < RegistrationCount; ++RegistrationIndex)
	{
		const FCrowdyMassStateFieldRegistration& Registration = *StateFieldRegistrations[RegistrationIndex];
		if (RegistrationClaimed[RegistrationIndex] || !Registration.bIsStandard)
		{
			continue;
		}

		// Once per class, not once per five seconds. This resolution is CACHED, so a class is only ever
		// asked about again after an invalidation: a time gate does not defer the message, it deletes it,
		// and the class it deletes it for is whichever one happened to resolve second in a level-load
		// burst. That is not hypothetical. It is how a shipping animation set that names no state enum
		// went unnoticed. Reported per class, so the burst still costs one line per class that is
		// actually misconfigured and nothing at all for the rest.
		if (StateFieldAbsentReportedClasses.Contains(&Class))
		{
			continue;
		}

		StateFieldAbsentReportedClasses.Add(&Class);

		if (Registration.Match == ECrowdyMassStateFieldMatch::ByName)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' carries no CrowdyState property named '%s'. This build treats that field as standard, so entities of this class never update it from a CrowdyState delta; a pose an earlier delta left standing can never be cleared by a later one."),
				*Class.GetName(), *Registration.PropertyName.ToString());
			continue;
		}

		// Five ways to arrive here and five different places to go: author a set, fix a broken reference,
		// name the enum on the set, free the property a game's own field took, or declare the property on
		// the class. Said apart, because a single "no pose for this class" would send whoever reads it to
		// look in four wrong places first.
		if (AnimSetSource == ECrowdyMassAnimSetSource::Undeclared)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' names no animation set, so there is no enum to recognise its replicated pose state by and entities of this class can never be posed from a CrowdyState delta. Add a Crowdy Mass Anim Set component to the class, or a Crowdy Mass Animation trait to the Mass entity config authored for it."),
				*Class.GetName());
			continue;
		}

		if (AnimSetSource == ECrowdyMassAnimSetSource::LoadFailed)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' names an animation set that did not load, so there is no enum to recognise its replicated pose state by. The asset is missing or renamed; fix the reference rather than the class."),
				*Class.GetName());
			continue;
		}

		if (!PoseStateEnum)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: The animation set '%s' renders through names no pose state enum, so no replicated property can be recognised as its pose. Set Pose State Enum, or Pose State Enum Name for an enum declared in C++, on that set."),
				*Class.GetName());
			continue;
		}

		if (!PoseStatePropertyClaimedByName.IsNone())
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' declares '%s' as '%s', the enum its animation set names, but a fragment field registered under that same name claimed it first, so nothing poses entities of this class from a CrowdyState delta and a pose an earlier delta left standing can never be cleared by a later one. A property can feed one field or the pose, not both: register the game's field against a second property, or drop it and read the pose fragment this subsystem already writes."),
				*Class.GetName(), *PoseStatePropertyClaimedByName.ToString(), *PoseStateEnum->GetName());
			continue;
		}

		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' carries no CrowdyState property declared as '%s', the enum its animation set names, so entities of this class are never posed from a CrowdyState delta and a pose an earlier delta left standing can never be cleared by a later one. Declare a property of that enum on the class and mark it with the CrowdyState meta."),
			*Class.GetName(), *PoseStateEnum->GetName());
		continue;
	}

	// Reported after the loop rather than inside it, because it is not about a field being absent: the
	// property bound and the pose id will land. Nothing DRAWS it. A set reached this way is one the class
	// names on its own component while its entities are built from a config that installs no animation
	// trait, so the resolution already knows the answer will not be rendered and every diagnostic that
	// exists to explain a crowd standing still should say so rather than let it be inferred.
	if (AnimSetSource == ECrowdyMassAnimSetSource::ClassComponentNotRendered && !ClaimedPoseStatePropertyName.IsNone()
		&& !StateFieldAbsentReportedClasses.Contains(&Class))
	{
		StateFieldAbsentReportedClasses.Add(&Class);
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' binds its replicated pose through '%s' and the ids will land, but the animation set they index is named on the class's own component while entities of this class are built from an authored Mass entity config that installs no animation trait, so nothing renders it. Add a Crowdy Mass Animation trait naming that set to the config."),
			*Class.GetName(), *ClaimedPoseStatePropertyName.ToString());
	}

	// Every remaining property decodes correctly and lands nowhere. Nothing is malformed, which is exactly
	// why it was silent: the value crosses the network, is decoded into the scratch buffer, and is then
	// stepped over because no registered field asked for it. A replicated variable that does nothing on the
	// crowd is the shape this reports, and only the resolution can see it, because by the time the apply
	// loop skips the slot the reason is already a cached INDEX_NONE.
	//
	// Decided here, once per class and layout, rather than where the value is stepped over: that step
	// happens per property per delta per entity, and this costs nothing there.
	for (int32 LayoutIndex = 0; LayoutIndex < Layout.Properties.Num(); ++LayoutIndex)
	{
		const FCrowdyRepProperty& RepProperty = Layout.Properties[LayoutIndex];
		const FProperty* Property = RepProperty.Property;
		if (!Property)
		{
			continue;
		}

		const FName PropertyName = Property->GetFName();
		const FCrowdyMassClassPropertyKey Key{FObjectKey(&Class), PropertyName};

		// A property that now reaches a field, or whose own refusal already had its own line above, leaves
		// the record. What the record holds is what is dropped NOW, so registering the field this warning
		// asked for is confirmed by the property leaving the verdict rather than by a message that stops.
		if (Resolved->RegistrationIndexByLayoutIndex[LayoutIndex] != INDEX_NONE || LayoutIndexExplained[LayoutIndex])
		{
			DroppedStateProperties.Remove(Key);
			continue;
		}

		FCrowdyMassDroppedStateProperty& Record = DroppedStateProperties.FindOrAdd(Key);

		// A record that already carries a detection has already been reported, so this is the second time
		// the same property has been decided about and the line is not written again. Counted first either
		// way, so the figure the verdict prints is the number of decisions rather than the number of lines.
		const bool bAlreadyReported = Record.Detections > 0;
		++Record.Detections;
		if (bAlreadyReported)
		{
			continue;
		}

		Record.ClassName = Class.GetName();
		Record.PropertyName = PropertyName;

		// Told before the binding advice below, because for an owner-only property that advice is a permanent
		// no-op: a crowd observer is not the entity's owner, so no delta it receives ever carries this value
		// and a registered field would be handed nothing to read. An author who followed the ordinary remedy
		// here would get the same silence back with a registration behind it.
		if (RepProperty.bOwnerOnly)
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' replicates the CrowdyState property '%s', but no registered Mass fragment field claims it, and it is marked CrowdyOwnerOnly, so it is delivered only to the entity's own owning client. A client drawing this entity as one of a crowd is not its owner and never receives the value, so registering a field for it would read nothing. Drop CrowdyOwnerOnly if the crowd is meant to see it, or read it on the actor representation, which is where an owner-only value does arrive. Said once per class and property: crowdy.mass.state.verdict lists every property currently dropped and how many times each was decided."),
				*Record.ClassName, *PropertyName.ToString());
			continue;
		}

		// The remedy is only followable for a property a registered field could actually read. A field names
		// the type it reads and the resolution above refuses a property declared as anything else AFTER
		// matching it by name, so an author who registers a field for a value no type here can read gets
		// the same silence back with more code behind it.
		//
		// Stated as which type to register for rather than as a width. Width is the wrong frame in both
		// directions: an int is four bytes and binds, while a bitfield bool is one and does not, because
		// what decides it is whether the value can be READ as some declared type.
		const TOptional<ECrowdyMassStateFieldType> BindableAs = CrowdyMassFindStateFieldTypeFor(*Property);
		if (!BindableAs.IsSet())
		{
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' replicates the CrowdyState property '%s', but no registered Mass fragment field claims it, so its value is decoded and then discarded for every entity of this class. RegisterStateFragmentField cannot bind this one as it stands: a field names the type it reads, and '%s' is %s. %s Ignore this if the property exists only for actors. Said once per class and property: crowdy.mass.state.verdict lists every property currently dropped and how many times each was decided."),
				*Record.ClassName, *PropertyName.ToString(), *PropertyName.ToString(),
				*CrowdyMassDescribeStateFieldProperty(*Property),
				*CrowdyMassStateFieldRemedyFor(*Property));
			continue;
		}

		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' replicates the CrowdyState property '%s', but no registered Mass fragment field claims it, so its value is decoded and then discarded for every entity of this class. Call RegisterStateFragmentField with '%s' and the type %s to read it onto a fragment; ignore this if the property exists only for actors. Said once per class and property: crowdy.mass.state.verdict lists every property currently dropped and how many times each was decided."),
			*Record.ClassName, *PropertyName.ToString(), *PropertyName.ToString(),
			CrowdyMassStateFieldTypeName(*BindableAs));
	}

	return Resolved;
}

const UEnum* UCrowdyMassEntitySubsystem::FindPoseStateEnum(const UClass* EntityClass, ECrowdyMassAnimSetSource& OutSource) const
{
	const UCrowdyMassAnimSet* AnimSet = FindRemoteEntityAnimSet(EntityClass, OutSource);

	// A set that answered and names no enum is its own case, which is why the source is reported beside
	// the answer: the remedy is on the set, not on the class and not on the reference to it.
	return AnimSet ? AnimSet->GetPoseStateEnum() : nullptr;
}

int32 UCrowdyMassEntitySubsystem::MapPoseStateValueToPoseId(const UClass* EntityClass, const int64 StateValue) const
{
	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::NoClass;
	const UCrowdyMassAnimSet* AnimSet = FindRemoteEntityAnimSet(EntityClass, Source);

	// Silent on a miss, unlike FindPoseId. Nothing reaches here until ResolveStateFragmentClass has
	// already matched this class's property against the enum its set names, so a set that has gone
	// missing since is a torn-down world rather than an authoring mistake, and this runs once per
	// arriving delta per entity: a line here would be a line per delta for a crowd.
	return AnimSet ? AnimSet->FindPoseIdByStateValue(StateValue) : 0;
}

int32 UCrowdyMassEntitySubsystem::FindPoseId(const UClass* EntityClass, FName PoseName) const
{
	// The cache below is written here and the animation set it describes is resolved on the game
	// thread. Blueprint-callable and documented for a send path, so the constraint is worth asserting
	// rather than only writing down.
	check(IsInGameThread());

	if (PoseName.IsNone())
	{
		return 0;
	}

	// Keyed on the class as well as the name, because the name is a row in THAT class's set. Answering
	// from a name alone would hand every class the first class's rows, which is the same wrong answer per
	// class rather than one visible failure.
	const FCrowdyMassClassPoseKey Key{FObjectKey(EntityClass), PoseName};
	if (const int32* CachedId = PoseIdsByClassAndName.Find(Key))
	{
		return *CachedId;
	}

	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::NoClass;
	const UCrowdyMassAnimSet* AnimSet = FindRemoteEntityAnimSet(EntityClass, Source);
	const int32 PoseId = AnimSet ? AnimSet->FindPoseId(PoseName) : 0;

	// Four different faults, said as four different things, because each sends whoever reads it somewhere
	// else: to the call site, to the class or its config, to the missing asset, or to the Poses array.
	if (Source == ECrowdyMassAnimSetSource::NoClass && ShouldWarnAnimSetNoClass())
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: Asked for pose '%s' with no entity class, so there is no animation set to look it up in. Each class renders through its own set; pass the class the pose is meant for."),
			*PoseName.ToString());
	}

	UE_CLOG(Source == ECrowdyMassAnimSetSource::Undeclared, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: Asked for pose '%s' on '%s', which names no animation set, so no pose can be named for it at all. Add a Crowdy Mass Anim Set component to the class, or a Crowdy Mass Animation trait to the Mass entity config authored for it."),
		*PoseName.ToString(), *GetNameSafe(EntityClass));

	UE_CLOG(Source == ECrowdyMassAnimSetSource::LoadFailed, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: Asked for pose '%s' on '%s', whose animation set is named but did not load, so no pose can be named for it. The asset is missing or renamed; fix the reference rather than the Poses array."),
		*PoseName.ToString(), *GetNameSafe(EntityClass));

	UE_CLOG(AnimSet && PoseId == 0, LogCrowdyMass, Warning,
		TEXT("[CrowdyMassEntitySubsystem]: No pose named '%s' is authored in '%s', the animation set '%s' renders through, so nothing will change. Add a Poses entry with that name, or check the name for a typo."),
		*PoseName.ToString(), *GetNameSafe(AnimSet), *GetNameSafe(EntityClass));

	// Cached either way, including the miss, so a name asked about every frame costs one array search in
	// the life of the world. The miss is only safe to cache because InvalidateAnimSetCaches empties this
	// map: a row added for a name that missed would otherwise never be seen.
	PoseIdsByClassAndName.Add(Key, PoseId);
	return PoseId;
}

bool UCrowdyMassEntitySubsystem::SetEntityPose(const FGuid& UUID, FName PoseName)
{
	check(IsInGameThread());

	FMassEntityManager* EntityManager = GetMassEntityManager();
	const FMassEntityHandle* Entity = UUIDToEntity.Find(UUID);
	if (!EntityManager || !Entity)
	{
		return false;
	}

	// The class comes from the entity rather than from the caller, which is what keeps this signature the
	// same now that a pose name is a row in one class's set: the id already names an entity, and an entity
	// already knows what class it stands for.
	const UClass* EntityClass = GetEntityClass(UUID);

	// Resolved before the refusal below rather than after it, so the rate-gated "asked for a pose with no
	// entity class" warning still names what went wrong.
	const int32 PoseId = FindPoseId(EntityClass, PoseName);

	// An entity with no recorded class has no animation set to look a name up in, so a name cannot be
	// turned into a row for it. Refused rather than written: the lookup answers 0, and 0 CLEARS the pose,
	// so writing it would do the opposite of what was asked and report that as success.
	if (!EntityClass && !PoseName.IsNone())
	{
		return false;
	}

	// Written through the same entry point the target-addressed pose setter uses, rather than reaching for
	// the fragment here. Both are a Blueprint-reachable write of the same byte, so they have to answer the
	// same way when Mass is mid-schedule: a write issued while processors are running races every one of
	// them declared to read that fragment. Writing it directly here made this the one pose door with no
	// such check, which left the protection reading as present while half the callers walked around it.
	//
	// NAME_None clears the pose, which is why this is one call rather than a pair: clearing is the same
	// question as setting, and a separate ClearEntityPose would be a second thing to remember and a
	// second thing to forget. Clearing needs no set, so it still succeeds with no recorded class.
	// The world half is deliberately left unset. SetPoseIdIn is the overload that is handed an entity
	// manager, so it never resolves one from the target and never reads the world; filling it in would cost
	// a weak-pointer serial allocation for a field nothing on this path looks at.
	FCrowdyMassEntityTarget Target;
	Target.Entity = *Entity;

	return CrowdyMassFields::SetPoseIdIn(*EntityManager, Target, static_cast<uint8>(PoseId))
		== CrowdyMassFields::EAccessResult::Ok;
}

void UCrowdyMassEntitySubsystem::InvalidateAnimSetCaches()
{
	// Every id in the pose map is a row position in the set of the class it was keyed under, so none of
	// them survives a set being replaced. Both maps are emptied whether or not any set actually changed,
	// because comparing pointers would still miss the case this is most needed for: the same asset,
	// re-authored with a new row.
	AnimSetByClass.Empty();
	PoseIdsByClassAndName.Empty();

	// The state-plane resolution is made of the same answer: which property carries a class's pose is
	// decided by the enum that class's set names, so a set arriving, going away or naming a different
	// enum changes which property binds. Keyed by class and by layout hash, neither of which moves when
	// an asset does, so nothing else here would ever drop it.
	StateResolvedClasses.Empty();
	StateFieldAbsentReportedClasses.Empty();
}

void UCrowdyMassEntitySubsystem::InvalidateNearTierPatchedConfigs()
{
	// A patched config is made of three things that can each move underneath it: the class that declares
	// the shell, the shell class itself, and the authored config it copies. Emptied whole rather than per
	// entry, because nothing here says which classes were built over the thing that moved, and rebuilding
	// costs one class walk and one class load per class that still spawns.
	NearTierPatchedConfigByClass.Empty();
	ReportedNearTierShellNotConsumedClasses.Empty();
}

#if !UE_BUILD_SHIPPING
void UCrowdyMassEntitySubsystem::BindDevAnimSetForClass(const UClass* EntityClass, const UCrowdyMassAnimSet* AnimSet)
{
	check(IsInGameThread());

	if (!EntityClass || !AnimSet)
	{
		return;
	}

	// The drop goes FIRST, because it empties the very map the binding below writes into. Doing it after
	// would sweep the binding away and leave the caller believing a set had been installed.
	InvalidateAnimSetCaches();

	FCrowdyMassClassAnimSet Entry;
	Entry.AnimSet = AnimSet;
	Entry.Source = ECrowdyMassAnimSetSource::ConfigTrait;
	AnimSetByClass.Add(EntityClass, Entry);
}
#endif

void UCrowdyMassEntitySubsystem::InvalidateAnimSetCachesInEveryWorld()
{
	if (!GEngine)
	{
		return;
	}

	// Every world, because what changed is not a property of any one of them: two worlds exist at once
	// while travelling between levels, and a play-in-editor session can hold several at a time.
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		UWorld* World = WorldContext.World();
		UCrowdyMassEntitySubsystem* Subsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
		if (Subsystem)
		{
			Subsystem->InvalidateAnimSetCaches();
		}
	}
}

#if WITH_EDITOR
void UCrowdyMassEntitySubsystem::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	// The two assets a class's animation answer is made of. Editing either changes the answer without
	// replacing any object, so neither the Live Coding hook nor a backend reconfigure fires: adding the
	// Poses row a warning just asked for, or adding the animation trait a warning just asked for, would
	// otherwise change nothing at all for the rest of the session. Naming a different Bank on a set also
	// changes the derived configs built from it, which is dropped below.
	//
	// The whole cache goes rather than the entry for this asset, because the cache is keyed by CLASS and
	// nothing here says which classes resolve through the asset that moved.
	//
	// The trait is in the list beside the asset because an instanced trait is its own object: adding or
	// removing one edits the config's own trait array and reports the config, but changing a field INSIDE a
	// trait reports the trait. Both are ways of changing what a config says, and only one of them was
	// being watched.
	//
	// The traits component is in the list for the other half of that same split: editing a field inside a
	// trait listed on it reports the trait, which the line above already covers, but ADDING or REMOVING a row
	// edits the component's own array and reports the component. Without it, listing a trait in the details
	// panel would change nothing at all until the editor was restarted.
	if (!Object || (!Object->IsA<UCrowdyMassAnimSet>() && !Object->IsA<UMassEntityConfigAsset>()
		&& !Object->IsA<UMassEntityTraitBase>() && !Object->IsA<UCrowdyMassEntityTraitsComponent>()))
	{
		return;
	}

	// A slider being DRAGGED reports every mouse move, so acting on those empties every cache below at user
	// interface frame rate. Nothing here is cheap to rebuild: the next spawn of each class pays a class
	// default object walk down a Blueprint parent chain, a synchronous class load and a fresh Mass template,
	// and a template built and abandoned stays in the spawner's registry for the life of the world. The
	// value is not settled until the drag ends, so there is nothing to act on before then either.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	InvalidateAnimSetCaches();

	// A near tier patch is a copy of an authored config's visualization trait, so an edit to either the
	// config or a trait in it leaves the copy saying what the asset used to say. Emptied here for the same
	// reason as the animation answers: filling in the high resolution actor a warning just asked about has
	// to take effect in the session that warned.
	InvalidateNearTierPatchedConfigs();

	// And a config built from a class, which holds a COPY of every trait the class lists on its Crowdy Mass
	// Entity Traits component. Nothing else here drops those, so without this a trait added or edited in the
	// details panel would leave the config saying what the class used to say for the rest of the session,
	// which is the one thing this hook exists to prevent.
	//
	// The whole map goes rather than one entry, because it is keyed by CLASS and nothing here says which
	// classes hold a copy of the object that moved.
	//
	// Narrowed to the types that can change what a built config says, rather than everything the filter
	// admits. An edit to a config ASSET cannot: a class an asset answers for is never built from here, and
	// the copies that asset does feed are dropped by the near tier call above. Dropping the map for those
	// too would rebuild every derived class, and abandon a Mass template per class in the spawner's registry
	// for the life of the world, every time an artist nudged a slider in an unrelated config.
	//
	// An anim set qualifies only for its Bank, which a derived config copies into its mesh description. A
	// null Property is a programmatic broadcast that names no field, so it counts.
	const bool bBankEdited = Object->IsA<UCrowdyMassAnimSet>()
		&& (!PropertyChangedEvent.Property
			|| PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UCrowdyMassAnimSet, Bank));

	if (bBankEdited || Object->IsA<UMassEntityTraitBase>() || Object->IsA<UCrowdyMassEntityTraitsComponent>())
	{
		InvalidateDerivedConfigs();
	}
}
#endif

void UCrowdyMassEntitySubsystem::InvalidateDerivedConfigs()
{
	DerivedConfigByClass.Empty();
	ReportedUnderivableClasses.Empty();
	CheckedDeclaredTraitsAgainstAuthoredConfigClasses.Empty();
}

void UCrowdyMassEntitySubsystem::InvalidateDerivedConfigsInEveryWorld()
{
	if (!GEngine)
	{
		return;
	}

	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		UWorld* World = WorldContext.World();
		UCrowdyMassEntitySubsystem* Subsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
		if (Subsystem)
		{
			Subsystem->InvalidateDerivedConfigs();
		}
	}
}

const UCrowdyMassAnimSet* UCrowdyMassEntitySubsystem::GetRemoteEntityAnimSet(const UClass* EntityClass) const
{
	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::NoClass;
	return FindRemoteEntityAnimSet(EntityClass, Source);
}

ECrowdyMassAnimSetSource UCrowdyMassEntitySubsystem::GetResolvedAnimSetSource(const UClass* EntityClass) const
{
	if (!EntityClass)
	{
		return ECrowdyMassAnimSetSource::NoClass;
	}

	const FCrowdyMassClassAnimSet* Resolved = AnimSetByClass.Find(EntityClass);

	// Deliberately does NOT resolve. The whole value of this answer is that it can say nobody has asked,
	// and working one out here is exactly what would stop that from being true.
	return Resolved ? Resolved->Source : ECrowdyMassAnimSetSource::NotResolved;
}

const UCrowdyMassAnimSet* UCrowdyMassEntitySubsystem::FindRemoteEntityAnimSet(const UClass* EntityClass,
	ECrowdyMassAnimSetSource& OutSource) const
{
	check(IsInGameThread());

	if (!EntityClass)
	{
		// Not the same as a class with no set: there is no class to have one. Silent here, because plenty
		// of callers legitimately have no class (a position-only crowd, a tool), and the callers for which
		// it IS a mistake say so themselves.
		OutSource = ECrowdyMassAnimSetSource::NoClass;
		return nullptr;
	}

	if (const FCrowdyMassClassAnimSet* Resolved = AnimSetByClass.Find(EntityClass))
	{
		OutSource = Resolved->Source;
		return Resolved->AnimSet;
	}

	FCrowdyMassClassAnimSet Entry;
	Entry.AnimSet = LoadRemoteEntityAnimSet(EntityClass, Entry.Source);

	// Kept whichever way it went, so the class is walked and the assets loaded once rather than once per
	// entity of it. A miss carries WHICH miss, so the answer stays three answers rather than one.
	AnimSetByClass.Add(EntityClass, Entry);

	OutSource = Entry.Source;
	return Entry.AnimSet;
}

const UCrowdyMassAnimSet* UCrowdyMassEntitySubsystem::LoadRemoteEntityAnimSet(const UClass* EntityClass,
	ECrowdyMassAnimSetSource& OutSource) const
{
	OutSource = ECrowdyMassAnimSetSource::Undeclared;

	// PER CLASS, because an animation set is a class's own pose and gait table. Two classes mapping the
	// same replicated state value to different clips is not a collision, it IS per-class animation: what
	// travels is the game's own state value, whose meaning is fixed by the game's enum, and the row index
	// it selects is worked out on the machine that draws it, against that machine's copy of that class's
	// set. Nothing about the wire changes for a second set to exist.
	//
	// Two authoring routes, and the precedence between them is a CAPABILITY question rather than a
	// presence one. The config wins only when it actually NAMES a set; a config that carries no animation
	// trait, or one whose trait names nothing, falls through to the class's own declaration instead of
	// answering "none". A config authored for collision or level-of-detail reasons must not silently
	// switch animation off for a class that names a set on itself.
	//
	// The authored route is pushed aside by the same variable that pushes aside the authored config a spawn
	// uses, so the two agree: an entity drawn from a config built out of its class must animate from what
	// that class declares, or the forced run would render one class's rows against another's row indices.
	const UMassEntityConfigAsset* ConfigAsset = CrowdyMassDerivation::ShouldForceDerive()
		? nullptr
		: GetRemoteEntityConfigAsset(EntityClass);
	const FCrowdyMassConfigAnimSet ConfigAnimSet = FCrowdyMassAnimSetDerivation::FromConfig(ConfigAsset);
	if (ConfigAnimSet.IsValid())
	{
		// A named set that will not load is a different fault from a class that names none, and it does
		// NOT fall through: falling through would quietly render this class with another set's rows while
		// the config names one nobody can see is missing.
		const UCrowdyMassAnimSet* Loaded = ConfigAnimSet.AnimSet.LoadSynchronous();
		if (!Loaded)
		{
			OutSource = ECrowdyMassAnimSetSource::LoadFailed;
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' names animation set '%s' on the animation trait of Mass entity config '%s', and it did not load. Entities of that class render in their bind pose until the reference is fixed."),
				*EntityClass->GetPathName(), *ConfigAnimSet.AnimSet.ToString(), *GetNameSafe(ConfigAsset));
			return nullptr;
		}

		OutSource = ECrowdyMassAnimSetSource::ConfigTrait;
		UE_LOG(LogCrowdyMass, Log,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' renders through animation set '%s', from the animation trait on Mass entity config '%s'."),
			*EntityClass->GetPathName(), *Loaded->GetName(), *GetNameSafe(ConfigAsset));

		// Two routes naming two different sets is the drift this pair can now produce, and it is the one
		// failure nothing else would catch: both references resolve perfectly and the only thing wrong is
		// that they disagree, so the component an author filled in quietly does nothing. Asked only on the
		// success path, once per class, because it costs a class walk.
		const FCrowdyMassDerivedAnimSet ShadowedClassAnimSet = FCrowdyMassAnimSetDerivation::FromClass(EntityClass);
		UE_CLOG(ShadowedClassAnimSet.IsValid() && ShadowedClassAnimSet.AnimSet != ConfigAnimSet.AnimSet,
			LogCrowdyMass, Warning,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' names animation set '%s' on its Crowdy Mass Anim Set component and '%s' on the animation trait of Mass entity config '%s'. The config wins, so the component's set is never used; remove one of them so the two cannot drift apart."),
			*EntityClass->GetPathName(), *ShadowedClassAnimSet.AnimSet.ToString(),
			*ConfigAnimSet.AnimSet.ToString(), *GetNameSafe(ConfigAsset));

		return Loaded;
	}

	const FCrowdyMassDerivedAnimSet ClassAnimSet = FCrowdyMassAnimSetDerivation::FromClass(EntityClass);
	if (ClassAnimSet.IsValid())
	{
		const UCrowdyMassAnimSet* Loaded = ClassAnimSet.AnimSet.LoadSynchronous();
		if (!Loaded)
		{
			OutSource = ECrowdyMassAnimSetSource::LoadFailed;
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' names animation set '%s' on its Crowdy Mass Anim Set component, and it did not load. Entities of that class render in their bind pose until the reference is fixed."),
				*EntityClass->GetPathName(), *ClassAnimSet.AnimSet.ToString());
			return nullptr;
		}

		// A config authored for this class is what a spawn of it is built from, and this branch is only
		// reached because that config names no set. So the set the class declares is not the set anything
		// renders: with no animation trait the entities carry no animation fragments at all, and with a
		// trait naming nothing they carry one holding no set. Answered as its own outcome rather than as a
		// success, because the answer is read by the diagnostics that explain a crowd in its bind pose.
		if (ConfigAsset)
		{
			OutSource = ECrowdyMassAnimSetSource::ClassComponentNotRendered;
			UE_LOG(LogCrowdyMass, Warning,
				TEXT("[CrowdyMassEntitySubsystem]: '%s' names animation set '%s' on %s on the class itself, but its entities are built from Mass entity config '%s', whose animation trait %s, so nothing renders that set. Add a Crowdy Mass Animation trait naming '%s' to that config, or %s."),
				*EntityClass->GetPathName(), *Loaded->GetName(), LexToString(ClassAnimSet.Route),
				*GetNameSafe(ConfigAsset),
				ConfigAnimSet.bSawAnimationTrait ? TEXT("names no set") : TEXT("is absent"),
				*Loaded->GetName(), *DescribeAuthoredConfigRemedy(EntityClass));
			return Loaded;
		}

		OutSource = ECrowdyMassAnimSetSource::ClassComponent;
		UE_LOG(LogCrowdyMass, Log,
			TEXT("[CrowdyMassEntitySubsystem]: '%s' renders through animation set '%s', from %s on the class itself."),
			*EntityClass->GetPathName(), *Loaded->GetName(), LexToString(ClassAnimSet.Route));
		return Loaded;
	}

	// Neither route answered. Which of them was even present decides the advice, so both are named: a
	// config with an animation trait that points at nothing is a slot to fill in, and a config with no
	// trait at all is a trait to add or a component to put on the class.
	UE_LOG(LogCrowdyMass, Log,
		TEXT("[CrowdyMassEntitySubsystem]: '%s' names no animation set, so its entities render in their bind pose. The Mass entity config authored for it %s. %s"),
		*EntityClass->GetPathName(),
		ConfigAnimSet.bSawAnimationTrait
			? TEXT("has an animation trait with no set assigned")
			: (ConfigAsset ? TEXT("has no animation trait") : TEXT("does not exist")),
		*ClassAnimSet.Reason);

	return nullptr;
}

bool UCrowdyMassEntitySubsystem::ShouldWarnAnimSetNoClass() const
{
	const double Now = FPlatformTime::Seconds();
	if (Now < NextAnimSetNoClassWarningSeconds)
	{
		return false;
	}

	NextAnimSetNoClassWarningSeconds = Now + 5.0;
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
void UCrowdyMassEntitySubsystem::HandleObjectPropertyChangedForTest(UObject* Object)
{
	FPropertyChangedEvent PropertyChangedEvent(nullptr);
	HandleObjectPropertyChanged(Object, PropertyChangedEvent);
}

void UCrowdyMassEntitySubsystem::HandleObjectsReplacedForTest(const TMap<UObject*, UObject*>& ReplacementMap)
{
	HandleObjectsReplaced(ReplacementMap);
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyMassEntitySubsystem::SetAuthoredEntityConfigForTest(const UClass* EntityClass, const UMassEntityConfigAsset* Config)
{
	bSuppressUnboundEntityConfigForTest = true;
	if (EntityClass && Config)
	{
		EntityConfigByClass.Add(EntityClass, Config);
	}

	InvalidateAnimSetCaches();
}
#endif

// What a struct actually declares for one of the field names the transform read looks for. Both halves
// matter to an author: a field that is missing and a field that is present under the wrong type arrive
// here as the same "no transform", and only one of them is a spelling mistake.
static FString CrowdyMassDescribeTransformField(const UScriptStruct& ScriptStruct, const TCHAR* FieldName, const UScriptStruct* RequiredStruct)
{
	const FProperty* Property = FindFProperty<FProperty>(&ScriptStruct, FieldName);
	if (!Property)
	{
		return FString::Printf(TEXT("no '%s' field"), FieldName);
	}

	const FStructProperty* StructProperty = CastField<const FStructProperty>(Property);
	if (StructProperty && StructProperty->Struct == RequiredStruct)
	{
		return FString::Printf(TEXT("'%s' is the right type"), FieldName);
	}

	return FString::Printf(TEXT("'%s' is declared as %s"), FieldName, *Property->GetCPPType());
}

// The message for a struct this cannot read a transform out of. It reports each of the exact lookups the
// read performs, so what it says is only ever what was actually tested for.
static FString CrowdyMassDescribeUnusableTransformLayout(const UScriptStruct& ScriptStruct)
{
	const TArray<FString> Findings =
	{
		CrowdyMassDescribeTransformField(ScriptStruct, TEXT("Transform"), TBaseStructure<FTransform>::Get()),
		CrowdyMassDescribeTransformField(ScriptStruct, TEXT("Location"), TBaseStructure<FVector>::Get()),
		CrowdyMassDescribeTransformField(ScriptStruct, TEXT("Position"), TBaseStructure<FVector>::Get()),
		CrowdyMassDescribeTransformField(ScriptStruct, TEXT("Rotation"), TBaseStructure<FRotator>::Get())
	};

	return FString::Printf(
		TEXT("[CrowdyMassEntitySubsystem]: replicated state struct '%s' carries no transform this backend can read, so entities driven by it are spawned and then never move. ")
		TEXT("Give it either a 'Transform' field of type FTransform, or a 'Location' (or 'Position') field of type FVector together with a 'Rotation' field of type FRotator. ")
		TEXT("An optional 'SmoothingDuration' float sets the interpolation window. Found: %s."),
		*ScriptStruct.GetPathName(), *FString::Join(Findings, TEXT("; ")));
}

bool UCrowdyMassEntitySubsystem::TryExtractTransformState(const FInstancedStruct& State, FVector& OutLocation, FQuat& OutRotation, float& OutSmoothingDuration)
{
	OutLocation = FVector::ZeroVector;
	OutRotation = FQuat::Identity;
	OutSmoothingDuration = -1.0f;

	if (!State.IsValid())
	{
		return false;
	}

	if (const FCrowdyMassTransformState* TransformState = State.GetPtr<FCrowdyMassTransformState>())
	{
		OutLocation = TransformState->Location;
		OutRotation = TransformState->Rotation.Quaternion();
		OutSmoothingDuration = TransformState->SmoothingDuration;
		return true;
	}

	const UScriptStruct* ScriptStruct = State.GetScriptStruct();
	const void* StructMemory = State.GetMemory();
	if (!ScriptStruct || !StructMemory)
	{
		return false;
	}

	// Where this struct keeps its transform, worked out once per struct type rather than per update.
	// The searches below build an FName from a string literal and then walk the property list, and the
	// two that miss walk it to the end, so a game's own actor-state struct paid four of them for
	// every update it received. The layout is a property of the type and cannot change while the type
	// is loaded, so it is answered from a memo after the first update of each type.
	struct FTransformLayout
	{
		enum class ESource : uint8 { Unusable, Transform, LocationAndRotation };

		ESource Source = ESource::Unusable;
		const FStructProperty* TransformProperty = nullptr;
		const FStructProperty* LocationProperty = nullptr;
		const FStructProperty* RotationProperty = nullptr;
		const FFloatProperty* SmoothingProperty = nullptr;
	};

	// Keyed on the struct itself. Wire payload structs are held alive by UActorUpdatePayloadRegistry
	// for as long as they are registered, so a key here cannot name a collected type. The lock is a
	// read almost every time; it replaces four FName searches, so it is cheap by comparison.
	static TMap<const UScriptStruct*, FTransformLayout> LayoutByStruct;
	static FRWLock LayoutLock;

	FTransformLayout Layout;
	bool bHaveLayout = false;

	{
		FRWScopeLock ReadLock(LayoutLock, SLT_ReadOnly);
		if (const FTransformLayout* Found = LayoutByStruct.Find(ScriptStruct))
		{
			Layout = *Found;
			bHaveLayout = true;
		}
	}

	if (!bHaveLayout)
	{
		if (const FStructProperty* TransformProperty = FindFProperty<FStructProperty>(ScriptStruct, TEXT("Transform")))
		{
			if (TransformProperty->Struct == TBaseStructure<FTransform>::Get())
			{
				Layout.Source = FTransformLayout::ESource::Transform;
				Layout.TransformProperty = TransformProperty;
			}
		}

		if (Layout.Source == FTransformLayout::ESource::Unusable)
		{
			const FStructProperty* LocationProperty = FindFProperty<FStructProperty>(ScriptStruct, TEXT("Location"));
			if (!LocationProperty)
			{
				LocationProperty = FindFProperty<FStructProperty>(ScriptStruct, TEXT("Position"));
			}

			const FStructProperty* RotationProperty = FindFProperty<FStructProperty>(ScriptStruct, TEXT("Rotation"));
			if (LocationProperty && RotationProperty
				&& LocationProperty->Struct == TBaseStructure<FVector>::Get()
				&& RotationProperty->Struct == TBaseStructure<FRotator>::Get())
			{
				Layout.Source = FTransformLayout::ESource::LocationAndRotation;
				Layout.LocationProperty = LocationProperty;
				Layout.RotationProperty = RotationProperty;
			}
		}

		// Resolved even when the struct is unusable, because a struct that will never yield a transform
		// is exactly the one that would otherwise pay the full search on every update forever.
		if (Layout.Source != FTransformLayout::ESource::Unusable)
		{
			Layout.SmoothingProperty = FindFProperty<FFloatProperty>(ScriptStruct, TEXT("SmoothingDuration"));
		}

		{
			FRWScopeLock WriteLock(LayoutLock, SLT_Write);
			LayoutByStruct.Add(ScriptStruct, Layout);
		}

		// Inside the block that resolves the layout, which runs once per struct type: that is what makes
		// this said once rather than on every update the struct receives. Moving it past the memo lookup
		// would repeat it for the whole life of the session.
		UE_CLOG(Layout.Source == FTransformLayout::ESource::Unusable, LogCrowdyMass, Warning,
			TEXT("%s"), *CrowdyMassDescribeUnusableTransformLayout(*ScriptStruct));
	}

	if (Layout.Source == FTransformLayout::ESource::Unusable)
	{
		return false;
	}

	if (Layout.Source == FTransformLayout::ESource::Transform)
	{
		const FTransform* Transform = Layout.TransformProperty->ContainerPtrToValuePtr<FTransform>(StructMemory);
		OutLocation = Transform->GetLocation();
		OutRotation = Transform->GetRotation();
	}
	else
	{
		OutLocation = *Layout.LocationProperty->ContainerPtrToValuePtr<FVector>(StructMemory);
		OutRotation = Layout.RotationProperty->ContainerPtrToValuePtr<FRotator>(StructMemory)->Quaternion();
	}

	if (Layout.SmoothingProperty)
	{
		OutSmoothingDuration = Layout.SmoothingProperty->GetPropertyValue_InContainer(StructMemory);
	}

	return true;
}

void UCrowdyMassEntitySubsystem::LogDroppedStatePropertyVerdict() const
{
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] ---- CrowdyState properties no fragment field claims ----"));

	// Zero is printed rather than skipped. A reader sent here by the warning is checking whether the
	// property they were told about is still dropped, and an absent section reads as "the tool did not
	// look", which is the opposite of the answer.
	if (DroppedStateProperties.IsEmpty())
	{
		UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] none: every replicated property of every class resolved so far reaches a registered field."));
		return;
	}

	// The whole record, not the lines the log happens to still hold. Each of these was warned about once,
	// on the first resolution that decided it, so a session that has been running a while shows fewer
	// warnings than there are entries here and this is the figure that is true.
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] %d dropped, each warned about once when it was first decided:"),
		DroppedStateProperties.Num());

	for (const TPair<FCrowdyMassClassPropertyKey, FCrowdyMassDroppedStateProperty>& Pair : DroppedStateProperties)
	{
		UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict]   '%s' property '%s', decided %lld time(s)"),
			*Pair.Value.ClassName, *Pair.Value.PropertyName.ToString(), Pair.Value.Detections);
	}
}

void UCrowdyMassEntitySubsystem::LogStateVerdict() const
{
	// First, and outside the router guard below, because which properties reach no fragment field is
	// decided by class resolution alone: it stays a true and useful answer in a world whose router has
	// already gone, where every counter below can only read zero.
	LogDroppedStatePropertyVerdict();

	const UCrowdyEventRouter* Router = EventRouter.Get();
	if (!Router)
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyState verdict] no event router in this world, so nothing on this path can be running. Run this from a play session."));
		return;
	}

	const UCrowdyEventRouter::FStateSubscriberStats& Stats = Router->GetStateSubscriberStats();

	int32 EntitiesWithClass = 0;
	for (const TPair<FGuid, TWeakObjectPtr<const UClass>>& Pair : StateEntityClassByUUID)
	{
		EntitiesWithClass += Pair.Value.IsValid() ? 1 : 0;
	}

	// One block, every number that decides a verdict, so it can be pasted whole rather than assembled from
	// a scrolled log. Deliberately reports the zeros too: a counter that is zero is the most informative
	// reading here, and a block that omitted them would read as a pass.
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] ---- participant-less state path ----"));
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] entities held=%d, of which with a recorded class=%d, registered fields=%d"),
		UUIDToEntity.Num(), EntitiesWithClass, StateFieldRegistrations.Num());
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] deltas claimed=%lld applied=%lld delivered slots=%lld, not claimed (went on waiting)=%lld"),
		Stats.Claimed, Stats.Applied, Stats.DeliveredSlots, Stats.NotClaimed);
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] drops: classid mismatch=%lld, no layout=%lld, no buffer=%lld, decode failed=%lld"),
		Stats.DroppedClassIdMismatch, Stats.DroppedNoLayout, Stats.DroppedNoScratch, Stats.DroppedDecodeFailed);

	// Reported on its own line because it is the one refusal here that means nothing is broken: the entity
	// is held with no class recorded, so there is nothing to read its bytes against. Without it, a crowd
	// spawned with no class reads as deltas claimed, none applied, and every drop reason zero.
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] claimed but refused for having no recorded class=%lld"),
		Stats.DroppedNoEntityClass);
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] pose writes from CrowdyState=%lld"), PoseWritesFromState);

	// The line above counts writes, and an entity that is alive writes the 0 sentinel on every heartbeat,
	// so it rises even while nothing anyone could see has crossed. This is the line that decides a
	// two-machine gate, and the two are read as a PAIR: take a reading, set a pose, take another, clear
	// it, then take two more. The count below rising says a pose arrived; the count below holding still
	// while the one above keeps climbing says it was cleared, because a heartbeat re-sends whatever the
	// stance currently is.
	//
	// It used to name a second road here, the view-state echo, so a gate could say which of the two wrote
	// a pose. That road was deleted with the echo's PoseId field: there is one road now, and reporting a
	// figure that can only ever read zero would be reporting a question nobody can still ask.
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] of those, carrying a real pose rather than the 0 sentinel: %lld (last value=%u)"),
		NonZeroPoseWritesFromState, LastNonZeroPoseFromState);
	UE_LOG(LogCrowdyMass, Log, TEXT("[CrowdyState verdict] ---- end ----"));
}

#if !UE_BUILD_SHIPPING
static FAutoConsoleCommandWithWorld GCrowdyMassStateVerdict(
	TEXT("crowdy.mass.state.verdict"),
	TEXT("Print one block describing whether CrowdyState is reaching crowd entities: every replicated property no fragment field claims, deltas claimed and applied, every drop reason, and which road wrote the pose. Paste the whole block when reporting a gate."),
	FConsoleCommandWithWorldDelegate::CreateLambda(
		[](UWorld* World)
		{
			const UCrowdyMassEntitySubsystem* Subsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
			if (!Subsystem)
			{
				UE_LOG(LogCrowdyMass, Warning,
					TEXT("[CrowdyState verdict] no Crowdy Mass subsystem for this world, so run this from a play session."));
				return;
			}

			Subsystem->LogStateVerdict();
		}));
#endif
