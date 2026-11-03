#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

namespace CrowdyDefaultMapping
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	struct FResolution
	{
		ECrowdyRole PlacedRole = ECrowdyRole::None;
		ECrowdyRole SpawnedRole = ECrowdyRole::None;
		bool bStreamsMovement = false;

		bool operator==(const FResolution& Other) const = default;

		FString ToString() const
		{
			return FString::Printf(TEXT("placed %s, spawned %s, movement %s"),
				*UEnum::GetValueAsString(PlacedRole), *UEnum::GetValueAsString(SpawnedRole),
				bStreamsMovement ? TEXT("on") : TEXT("off"));
		}
	};

	struct FEntry
	{
		FString Name;
		TObjectPtr<const UCrowdyEntityComponent> Component;
		TObjectPtr<UClass> ActorClass;
	};

	ECrowdyRole RoleFor(const ECrowdyOwnership Ownership, const bool bPlaced)
	{
		ECrowdyRole Role = ECrowdyRole::None;
		FGuid OwnerID;
		UCrowdyEntityComponent::DeriveAuthority(UCrowdyEntityComponent::ResolveEffectiveOwnership(Ownership, bPlaced),
			FGuid(1, 0, 0, 0), Role, OwnerID);
		return Role;
	}

	// Today's behaviour, read through the same functions the component uses.
	FResolution ResolveToday(const UCrowdyEntityComponent& Component)
	{
		FResolution Result;
		Result.PlacedRole = RoleFor(Component.GetOwnership(), true);
		Result.SpawnedRole = RoleFor(Component.GetOwnership(), false);
		Result.bStreamsMovement = Component.GetMode() == ECrowdyEntityMode::Dynamic;
		return Result;
	}

	// The role on the client that placed or spawned the entity: a Leader entity belongs to someone else there.
	ECrowdyRole RoleFor(const ECrowdyOwnedBy OwnedBy)
	{
		switch (OwnedBy)
		{
		case ECrowdyOwnedBy::Player: return ECrowdyRole::Owner;
		case ECrowdyOwnedBy::World: return ECrowdyRole::HostOwned;
		default: return ECrowdyRole::RemoteProxy;
		}
	}

	// The replacement settings as the component holds them: mapped on load for a saved asset, Auto for a C++ default.
	FResolution ResolveReplacement(const FEntry& Entry)
	{
		const UCrowdyEntityComponent& Component = *Entry.Component;
		FResolution Result;
		Result.PlacedRole = RoleFor(UCrowdyEntityComponent::ResolveOwnedBy(Component.OwnedBy, Component.GetOwnership(), true));
		Result.SpawnedRole = RoleFor(UCrowdyEntityComponent::ResolveOwnedBy(Component.OwnedBy, Component.GetOwnership(), false));
		Result.bStreamsMovement = UCrowdyEntityComponent::ResolveReplicatesMovement(Component.ReplicateMovement,
			Component.GetMode(), UCrowdyEntityComponent::IsPawnWithMovement(Entry.ActorClass.Get()));
		return Result;
	}

	// Reports every entry that resolves differently under the replacement, unless it is a listed intended change.
	int32 CompareEntries(FAutomationTestBase& Test, const TArray<FEntry>& Entries, const TCHAR* Kind,
		const TMap<FString, FString>& IntendedChanges)
	{
		int32 Changed = 0;
		for (const FEntry& Entry : Entries)
		{
			const FResolution Today = ResolveToday(*Entry.Component);
			const FResolution Replacement = ResolveReplacement(Entry);
			Test.AddInfo(FString::Printf(TEXT("%s [%s]: %s"), *Entry.Name, Kind, *Today.ToString()));
			if (Today == Replacement) continue;

			++Changed;
			if (const FString* Reason = IntendedChanges.Find(Entry.Name))
			{
				Test.AddInfo(FString::Printf(TEXT("intended change on %s (%s): %s"), *Entry.Name, **Reason, *Replacement.ToString()));
				continue;
			}

			Test.AddError(FString::Printf(TEXT("%s [%s] resolves differently: today %s, replacement %s"), *Entry.Name, Kind,
				*Today.ToString(), *Replacement.ToString()));
		}
		return Changed;
	}

	bool IsComparableClass(const UClass* Class)
	{
		return Class && Class->IsChildOf<AActor>()
			&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
			&& !Class->GetName().StartsWith(TEXT("SKEL_")) && !Class->GetName().StartsWith(TEXT("REINST_"));
	}

	FTopLevelAssetPath TagClassPath(const FAssetData& Asset, const FName Tag)
	{
		const FString Path = FPackageName::ExportTextPathToObjectPath(Asset.GetTagValueRef<FString>(Tag));
		return Path.StartsWith(TEXT("/")) ? FTopLevelAssetPath(Path) : FTopLevelAssetPath();
	}

	const UClass* FindLoadedClass(const FTopLevelAssetPath& Path)
	{
		return Path.IsValid() ? FindObject<UClass>(Path) : nullptr;
	}

	void AddEntries(UClass* Class, const FString& Name, TArray<FEntry>& Out)
	{
		TArray<const UCrowdyEntityComponent*> Components;
		AActor::GetActorClassDefaultComponents<UCrowdyEntityComponent>(Class, Components);
		for (const UCrowdyEntityComponent* Component : Components)
		{
			Out.Add({ Name, Component, Class });
		}
	}

	// The packages an entity component's class lives in: a saved object carrying the component imports one of them.
	TSet<FName> GatherComponentPackages(IAssetRegistry& Registry)
	{
		TSet<FName> Packages;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf<UCrowdyEntityComponent>())
			{
				Packages.Add(It->GetOutermost()->GetFName());
			}
		}

		TArray<FAssetData> Blueprints;
		Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Blueprints, true);
		for (const FAssetData& Asset : Blueprints)
		{
			const UClass* Native = FindLoadedClass(TagClassPath(Asset, FBlueprintTags::NativeParentClassPath));
			if (Native && Native->IsChildOf<UCrowdyEntityComponent>())
			{
				Packages.Add(Asset.PackageName);
			}
		}
		return Packages;
	}

	TSet<FName> GatherReferencers(IAssetRegistry& Registry, const TSet<FName>& Packages)
	{
		TSet<FName> Referencers;
		for (const FName Package : Packages)
		{
			TArray<FName> Found;
			Registry.GetReferencers(Package, Found);
			Referencers.Append(Found);
		}
		return Referencers;
	}

	void GatherNative(TArray<FEntry>& Out, TSet<FTopLevelAssetPath>& OutEntityClasses)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->HasAnyClassFlags(CLASS_Native) || !Class->IsChildOf<AActor>()) continue;
			if (!AActor::GetActorClassDefaultComponent<UCrowdyEntityComponent>(Class)) continue;

			OutEntityClasses.Add(Class->GetClassPathName());
			if (IsComparableClass(Class))
			{
				AddEntries(Class, Class->GetPathName(), Out);
			}
		}
	}

	// Loads only Actor Blueprints that import a component class or whose parent carries the component.
	void GatherBlueprints(IAssetRegistry& Registry, const TSet<FName>& ComponentReferencers, TArray<FEntry>& Out,
		TSet<FTopLevelAssetPath>& InOutEntityClasses)
	{
		TArray<FAssetData> Blueprints;
		Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Blueprints, true);

		struct FPending
		{
			const FAssetData* Asset;
			FTopLevelAssetPath Parent;
		};
		TArray<FPending> Pending;
		for (const FAssetData& Asset : Blueprints)
		{
			const UClass* Native = FindLoadedClass(TagClassPath(Asset, FBlueprintTags::NativeParentClassPath));
			if (Native && Native->IsChildOf<AActor>())
			{
				Pending.Add({ &Asset, TagClassPath(Asset, FBlueprintTags::ParentClassPath) });
			}
		}

		bool bFoundMore = true;
		while (bFoundMore)
		{
			bFoundMore = false;
			for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
			{
				const FAssetData& Asset = *Pending[Index].Asset;
				if (!ComponentReferencers.Contains(Asset.PackageName) && !InOutEntityClasses.Contains(Pending[Index].Parent))
				{
					continue;
				}

				Pending.RemoveAtSwap(Index);

				const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				UClass* Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
				if (!Class || !AActor::GetActorClassDefaultComponent<UCrowdyEntityComponent>(Class)) continue;

				InOutEntityClasses.Add(Class->GetClassPathName());
				bFoundMore = true;
				if (IsComparableClass(Class))
				{
					AddEntries(Class, Class->GetPathName(), Out);
				}
			}
		}
	}

	// One-file-per-actor packages and whole-level maps that import a component class; any actor in them may carry one.
	void GatherPlaced(IAssetRegistry& Registry, const TSet<FName>& ComponentReferencers, TArray<FEntry>& Out)
	{
		const UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FName EditorPackage = EditorWorld ? EditorWorld->GetOutermost()->GetFName() : NAME_None;

		TArray<const AActor*> Actors;
		TArray<UWorld*> LoadedWorlds;
		for (const FName PackageName : ComponentReferencers)
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPackageName(PackageName, Assets);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.IsInstanceOf<UWorld>())
				{
					if (Asset.GetTagValueRef<FString>(TEXT("LevelIsUsingExternalActors")) == TEXT("1")) continue;

					const bool bWasLoaded = FindPackage(nullptr, *PackageName.ToString()) != nullptr;
					UWorld* World = Cast<UWorld>(Asset.GetAsset());
					if (!World || !World->PersistentLevel) continue;

					if (!bWasLoaded)
					{
						LoadedWorlds.Add(World);
					}
					for (const AActor* Actor : World->PersistentLevel->Actors)
					{
						if (Actor) Actors.Add(Actor);
					}
					continue;
				}

				if (!Asset.IsInstanceOf<AActor>()) continue;
				if (Asset.GetSoftObjectPath().GetLongPackageFName() == EditorPackage) continue;
				if (const AActor* Actor = Cast<AActor>(Asset.GetAsset()))
				{
					Actors.Add(Actor);
				}
			}
		}

		for (const AActor* Actor : Actors)
		{
			ForEachObjectWithOuter(Actor, [&](UObject* Object)
			{
				const UCrowdyEntityComponent* Component = Cast<UCrowdyEntityComponent>(Object);
				if (Component && !Component->IsTemplate())
				{
					Out.Add({ Actor->GetPathName(), Component, Actor->GetClass() });
				}
			});
		}

		// A loaded editor world is Standalone; release the ones this sweep loaded so the next collection frees them.
		for (UWorld* World : LoadedWorlds)
		{
			World->ClearFlags(RF_Standalone);
		}
	}

}

// Loading a project's content logs whatever errors that content has; only this test's own assertions fail it.
class FCrowdyContentSweepTestBase : public FAutomationTestBase
{
public:
	using FAutomationTestBase::FAutomationTestBase;

protected:
	virtual bool SuppressLogErrors() override { return true; }
};

/**
 * Every C++ entity class and every Blueprint resolves the same way under Owned By and Replicate Movement as under
 * Ownership and Mode: its role when placed and when spawned, and whether it streams movement. A Blueprint saved before
 * those settings existed is compared through the load mapping. Deliberate changes go in IntendedChanges.
 */
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyEntityDefaultMappingGuardTest, FCrowdyContentSweepTestBase,
	"CrowdySDK.CrowdySDKEditor.EntityDefaultMappingGuard", CrowdyDefaultMapping::TestFlags)
bool FCrowdyEntityDefaultMappingGuardTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyDefaultMapping;

	static const TMap<FString, FString> IntendedChanges;

	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.SearchAllAssets(true);

	TArray<FEntry> Native;
	TSet<FTopLevelAssetPath> EntityClasses;
	GatherNative(Native, EntityClasses);
	if (!TestTrue(TEXT("the SDK's own entity classes were found, so the sweep is not silently empty"), Native.Num() > 0))
	{
		return false;
	}

	const int32 NativeChanged = CompareEntries(*this, Native, TEXT("C++"), IntendedChanges);

	TArray<FEntry> Blueprints;
	GatherBlueprints(Registry, GatherReferencers(Registry, GatherComponentPackages(Registry)), Blueprints, EntityClasses);
	const int32 BlueprintChanged = CompareEntries(*this, Blueprints, TEXT("Blueprint"), IntendedChanges);

	AddInfo(FString::Printf(TEXT("%d C++ and %d Blueprint entity components compared, %d and %d resolve differently"),
		Native.Num(), Blueprints.Num(), NativeChanged, BlueprintChanged));
	return true;
}

/**
 * The same comparison for every placed entity component in the project's levels. Loads every level that carries one,
 * so it is not part of the default run.
 */
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlacedDefaultsSweepTest, FCrowdyContentSweepTestBase,
	"CrowdySweep.Migration.EntityPlacedDefaults", CrowdyDefaultMapping::TestFlags)
bool FCrowdyEntityPlacedDefaultsSweepTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyDefaultMapping;

	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.SearchAllAssets(true);

	TArray<FEntry> Placed;
	GatherPlaced(Registry, GatherReferencers(Registry, GatherComponentPackages(Registry)), Placed);
	const int32 Changed = CompareEntries(*this, Placed, TEXT("placed"), {});

	AddInfo(FString::Printf(TEXT("%d placed entity components compared, %d resolve differently"), Placed.Num(), Changed));
	return true;
}

#endif
