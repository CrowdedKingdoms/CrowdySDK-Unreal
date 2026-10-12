#include "Derivation/CrowdyMassCollisionDerivation.h"

#include "AnimationRuntime.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"

static_assert(FCrowdyMassCollisionDerivation::ChannelCount == 64,
	"A collision response container is expected to be one response byte per channel and nothing else. If it has gained a member, the channel loops below no longer cover every channel.");

FCrowdyMassDerivedTraceCollision FCrowdyMassCollisionDerivation::FromActorClass(const UClass* ActorClass)
{
	FCrowdyMassDerivedTraceCollision Derived;

	if (!ActorClass)
	{
		Derived.Reason = TEXT("No class was given, so there is nothing to read trace collision from.");
		return Derived;
	}

	if (!ActorClass->IsChildOf(AActor::StaticClass()))
	{
		Derived.Reason = FString::Printf(TEXT("'%s' is not an actor class, so it has no components to read trace collision from."), *ActorClass->GetName());
		return Derived;
	}

	if (const UCapsuleComponent* Capsule = FindCapsuleComponent(ActorClass))
	{
		Derived.Settings = SettingsFromComponent(*Capsule);
		Derived.Radius = Capsule->GetUnscaledCapsuleRadius();
		Derived.HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
		Derived.Source = ECrowdyMassCollisionDerivationSource::CapsuleComponent;
		return Derived;
	}

	const UPhysicsAsset* PhysicsAsset = nullptr;
	const USkinnedMeshComponent* SkinnedMeshComponent = FindPhysicsAssetComponent(ActorClass, PhysicsAsset);
	if (!SkinnedMeshComponent || !PhysicsAsset)
	{
		Derived.Reason = FString::Printf(
			TEXT("'%s' has no capsule component, and no skinned mesh on it has a physics asset, so neither its size nor its collision channels can be read from the class. Give the actor a capsule component, assign a physics asset to its mesh, or author trace collision on a Mass entity config for it."),
			*ActorClass->GetName());
		return Derived;
	}

	float Radius = 0.0f;
	float HalfHeight = 0.0f;
	if (!ComputePhysicsAssetExtents(*SkinnedMeshComponent, *PhysicsAsset, Radius, HalfHeight))
	{
		Derived.Reason = FString::Printf(
			TEXT("'%s' uses physics asset '%s', but none of its bodies names a bone that the mesh's reference skeleton has, so there is no shape left to measure. Rebuild the physics asset against the mesh it is assigned to."),
			*ActorClass->GetName(), *PhysicsAsset->GetName());
		return Derived;
	}

	Derived.Settings = SettingsFromComponent(*SkinnedMeshComponent);
	Derived.Radius = Radius;
	Derived.HalfHeight = HalfHeight;
	Derived.Source = ECrowdyMassCollisionDerivationSource::PhysicsAsset;
	return Derived;
}

FCrowdyMassTraceCollisionSettings FCrowdyMassCollisionDerivation::SettingsFromComponent(const UPrimitiveComponent& Component)
{
	FCrowdyMassTraceCollisionSettings Settings;

	// A component whose collision answers no queries presents nothing to a trace, whatever channels it
	// names, so that is carried across as trace collision being off rather than as a silent mismatch.
	Settings.bEnableTraceCollision = CollisionEnabledHasQuery(Component.GetCollisionEnabled());
	Settings.ObjectType = Component.GetCollisionObjectType();

	const FCollisionResponseContainer& Responses = Component.GetCollisionResponseToChannels();

	int32 ResponseCounts[ECollisionResponse::ECR_MAX] = { 0 };
	for (int32 ChannelIndex = 0; ChannelIndex < ChannelCount; ++ChannelIndex)
	{
		const ECollisionResponse Response = Responses.GetResponse(static_cast<ECollisionChannel>(ChannelIndex));
		if (Response < ECollisionResponse::ECR_MAX)
		{
			++ResponseCounts[Response];
		}
	}

	// The response most channels share becomes the default and the rest become overrides, which is the
	// smallest description that still reproduces the component exactly. Ties settle on the lower
	// response so the same component always describes itself the same way.
	ECollisionResponse DefaultResponse = ECollisionResponse::ECR_Ignore;
	int32 HighestCount = -1;
	for (int32 ResponseIndex = 0; ResponseIndex < ECollisionResponse::ECR_MAX; ++ResponseIndex)
	{
		if (ResponseCounts[ResponseIndex] <= HighestCount)
		{
			continue;
		}

		HighestCount = ResponseCounts[ResponseIndex];
		DefaultResponse = static_cast<ECollisionResponse>(ResponseIndex);
	}

	Settings.DefaultResponse = DefaultResponse;

	// The struct's own constructor seeds one override, so start from nothing rather than adding to it.
	Settings.ResponseOverrides.Reset();
	for (int32 ChannelIndex = 0; ChannelIndex < ChannelCount; ++ChannelIndex)
	{
		const ECollisionChannel Channel = static_cast<ECollisionChannel>(ChannelIndex);
		const ECollisionResponse Response = Responses.GetResponse(Channel);
		if (Response == DefaultResponse)
		{
			continue;
		}

		Settings.ResponseOverrides.Emplace(Channel, Response);
	}

	return Settings;
}

FCollisionResponseContainer FCrowdyMassCollisionDerivation::ExpandResponses(const FCrowdyMassTraceCollisionSettings& Settings)
{
	FCollisionResponseContainer Responses(Settings.DefaultResponse.GetValue());
	for (const FCrowdyMassCollisionResponseOverride& Override : Settings.ResponseOverrides)
	{
		Responses.SetResponse(Override.Channel.GetValue(), Override.Response.GetValue());
	}

	return Responses;
}

const UCapsuleComponent* FCrowdyMassCollisionDerivation::FindCapsuleComponent(const UClass* ActorClass)
{
	if (!ActorClass)
	{
		return nullptr;
	}

	const UCapsuleComponent* FirstCapsule = nullptr;
	const UCapsuleComponent* QueryableCapsule = nullptr;

	// This walks the class default object for native components and then every Blueprint in the
	// hierarchy for components added in the editor, including a child Blueprint's overrides of an
	// inherited one. A class default object alone would find only the native ones, which would miss a
	// capsule added in a Blueprint. The class is only read, but the engine takes it non-const.
	AActor::ForEachComponentOfActorClassDefault(
		TSubclassOf<AActor>(const_cast<UClass*>(ActorClass)),
		UCapsuleComponent::StaticClass(),
		[&FirstCapsule, &QueryableCapsule](const UActorComponent* Component)
		{
			const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Component);
			if (!Capsule)
			{
				return true;
			}

			if (!FirstCapsule)
			{
				FirstCapsule = Capsule;
			}

			if (!CollisionEnabledHasQuery(Capsule->GetCollisionEnabled()))
			{
				return true;
			}

			QueryableCapsule = Capsule;
			return false;
		});

	return QueryableCapsule ? QueryableCapsule : FirstCapsule;
}

const USkinnedMeshComponent* FCrowdyMassCollisionDerivation::FindPhysicsAssetComponent(const UClass* ActorClass, const UPhysicsAsset*& OutPhysicsAsset)
{
	OutPhysicsAsset = nullptr;

	if (!ActorClass)
	{
		return nullptr;
	}

	const USkinnedMeshComponent* FoundComponent = nullptr;
	const UPhysicsAsset* FoundPhysicsAsset = nullptr;

	AActor::ForEachComponentOfActorClassDefault(
		TSubclassOf<AActor>(const_cast<UClass*>(ActorClass)),
		USkinnedMeshComponent::StaticClass(),
		[&FoundComponent, &FoundPhysicsAsset](const UActorComponent* Component)
		{
			const USkinnedMeshComponent* SkinnedMeshComponent = Cast<USkinnedMeshComponent>(Component);
			if (!SkinnedMeshComponent)
			{
				return true;
			}

			// Reports the component's own override if it has one, and the mesh's physics asset if it
			// does not, which is the same asset the component would use once it was running.
			const UPhysicsAsset* PhysicsAsset = SkinnedMeshComponent->GetPhysicsAsset();
			if (!PhysicsAsset)
			{
				return true;
			}

			FoundComponent = SkinnedMeshComponent;
			FoundPhysicsAsset = PhysicsAsset;
			return false;
		});

	OutPhysicsAsset = FoundPhysicsAsset;
	return FoundComponent;
}

bool FCrowdyMassCollisionDerivation::ComputePhysicsAssetExtents(const USkinnedMeshComponent& Component, const UPhysicsAsset& PhysicsAsset, float& OutRadius, float& OutHalfHeight)
{
	OutRadius = 0.0f;
	OutHalfHeight = 0.0f;

	const USkinnedAsset* SkinnedAsset = Component.GetSkinnedAsset();
	if (!SkinnedAsset)
	{
		return false;
	}

	const FReferenceSkeleton& ReferenceSkeleton = SkinnedAsset->GetRefSkeleton();

	// A physics body's shapes are positioned relative to the bone it belongs to, so each body has to be
	// placed by that bone before the bodies can be measured together. The reference pose is the only
	// pose a class has: it is the mesh as imported, before any animation.
	FBox Bounds(ForceInit);
	for (const TObjectPtr<USkeletalBodySetup>& BodySetupPtr : PhysicsAsset.SkeletalBodySetups)
	{
		const USkeletalBodySetup* BodySetup = BodySetupPtr.Get();
		if (!BodySetup)
		{
			continue;
		}

		const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(BodySetup->BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		Bounds += BodySetup->AggGeom.CalcAABB(FAnimationRuntime::GetComponentSpaceTransformRefPose(ReferenceSkeleton, BoneIndex));
	}

	if (!Bounds.IsValid)
	{
		return false;
	}

	const FVector Extent = Bounds.GetExtent();
	OutRadius = static_cast<float>(FMath::Max(Extent.X, Extent.Y));
	OutHalfHeight = static_cast<float>(Extent.Z);
	return true;
}
