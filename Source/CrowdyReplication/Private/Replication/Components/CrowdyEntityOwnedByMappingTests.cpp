#include "Replication/Components/CrowdyEntityComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdySDKCustomVersion.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/Pawn.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyOwnedByTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An editor world, like the other headless fixtures: it never dispatches BeginPlay, so spawning has no side effects.
	struct FCrowdyOwnedByTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyOwnedByTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyOwnedByTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnedByResolutionTest,
	"CrowdySDK.Entity.OwnedByResolution", CrowdyOwnedByTestFlags)
bool FCrowdyOwnedByResolutionTest::RunTest(const FString& Parameters)
{
	using C = UCrowdyEntityComponent;

	TestEqual(TEXT("an explicit Owned By is kept"), C::ResolveOwnedBy(ECrowdyOwnedBy::Leader, true), ECrowdyOwnedBy::Leader);
	TestEqual(TEXT("Leader spawned during play stays the Leader's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Leader, false), ECrowdyOwnedBy::Leader);
	TestEqual(TEXT("Auto placed in the level is the world's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Auto, true), ECrowdyOwnedBy::World);
	TestEqual(TEXT("Auto spawned during play is the spawner's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Auto, false), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("World spawned during play is the spawner's too"), C::ResolveOwnedBy(ECrowdyOwnedBy::World, false), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("Player placed in the level is the player's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Player, true), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("World placed in the level is the world's"), C::ResolveOwnedBy(ECrowdyOwnedBy::World, true), ECrowdyOwnedBy::World);

	TestTrue(TEXT("a character is a pawn with movement"), C::IsPawnWithMovement(ACharacter::StaticClass()));
	TestTrue(TEXT("so is any pawn with a movement component"), C::IsPawnWithMovement(ADefaultPawn::StaticClass()));
	TestFalse(TEXT("a pawn without one is not"), C::IsPawnWithMovement(APawn::StaticClass()));
	TestFalse(TEXT("nor is a plain actor"), C::IsPawnWithMovement(AActor::StaticClass()));

	TestTrue(TEXT("On sends movement"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::On, false));
	TestFalse(TEXT("Off sends none, even for a pawn with movement"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Off, true));
	TestTrue(TEXT("Auto on a pawn with movement sends it"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Auto, true));
	TestFalse(TEXT("Auto on anything else sends none"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Auto, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutoMovementFollowsClassTest,
	"CrowdySDK.Entity.AutoMovementFollowsClass", CrowdyOwnedByTestFlags)
bool FCrowdyAutoMovementFollowsClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyEntityComponent> Component(NewObject<UCrowdyEntityComponent>(GetTransientPackage()));
	TestTrue(TEXT("Auto on a character sends movement"), Component->ReplicatesMovementFor(ACharacter::StaticClass()));
	TestFalse(TEXT("Auto on a plain actor sends none"), Component->ReplicatesMovementFor(AActor::StaticClass()));

	Component->ReplicateMovement = ECrowdyReplicateMovement::Off;
	TestFalse(TEXT("an explicit Off wins over the class"), Component->ReplicatesMovementFor(ACharacter::StaticClass()));
	Component->ReplicateMovement = ECrowdyReplicateMovement::On;
	TestTrue(TEXT("an explicit On wins over the class"), Component->ReplicatesMovementFor(AActor::StaticClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAddedComponentSendsNoMovementTest,
	"CrowdySDK.Entity.AddedComponentSendsNoMovement", CrowdyOwnedByTestFlags)
bool FCrowdyAddedComponentSendsNoMovementTest::RunTest(const FString& Parameters)
{
	FCrowdyOwnedByTestWorld TestWorld;
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	ACharacter* Character = TestWorld.World->SpawnActor<ACharacter>();
	const UCrowdyEntityComponent* Component = Character ? Entities->AddStaticEntityComponentForTest(Character) : nullptr;
	if (!TestNotNull(TEXT("a component is added"), Component)) return false;

	TestFalse(TEXT("a component added at run time to a character sends no movement"),
		Component->ReplicatesMovementFor(Character->GetClass()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntitySaveRecordsVersionTest,
	"CrowdySDK.Entity.SaveRecordsVersion", CrowdyOwnedByTestFlags)
bool FCrowdyEntitySaveRecordsVersionTest::RunTest(const FString& Parameters)
{
	// An older SDK remaps any save without this version from Ownership and Mode, so every save must carry it.
	const TStrongObjectPtr<UCrowdyEntityComponent> Component(NewObject<UCrowdyEntityComponent>(GetTransientPackage()));
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FObjectAndNameAsStringProxyArchive Archive(Writer, false);
	Component->Serialize(Archive);

	const FCustomVersion* Recorded = Writer.GetCustomVersions().GetVersion(FCrowdySDKCustomVersion::GUID);
	if (!TestNotNull(TEXT("a saved component records the CrowdySDK version"), Recorded))
	{
		return false;
	}
	TestEqual(TEXT("at the latest version"), Recorded->Version, static_cast<int32>(FCrowdySDKCustomVersion::LatestVersion));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWorldOwnedRoleRedirectTest,
	"CrowdySDK.Entity.WorldOwnedRoleRedirect", CrowdyOwnedByTestFlags)
bool FCrowdyWorldOwnedRoleRedirectTest::RunTest(const FString& Parameters)
{
	const UEnum* RoleEnum = StaticEnum<ECrowdyRole>();
	if (!TestNotNull(TEXT("the role enum is reflected"), RoleEnum))
	{
		return false;
	}

	// A Blueprint pin or saved value written as HostOwned resolves by name, through the plugin's enum redirect.
	const int64 WorldOwned = static_cast<int64>(ECrowdyRole::WorldOwned);
	TestEqual(TEXT("the old name resolves to World Owned"), RoleEnum->GetValueByNameString(TEXT("HostOwned")), WorldOwned);
	TestEqual(TEXT("so does its qualified form"), RoleEnum->GetValueByNameString(TEXT("ECrowdyRole::HostOwned")), WorldOwned);
	TestEqual(TEXT("the new name resolves as itself"), RoleEnum->GetValueByNameString(TEXT("WorldOwned")), WorldOwned);

	// A role sent or saved as a number keeps meaning the same thing.
	TestEqual(TEXT("World Owned keeps the old value"), static_cast<int32>(ECrowdyRole::WorldOwned), 3);
	return true;
}

#if WITH_METADATA
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWorldOwnedRoleDisplayNameTest,
	"CrowdySDK.Entity.WorldOwnedRoleDisplayName", CrowdyOwnedByTestFlags)
bool FCrowdyWorldOwnedRoleDisplayNameTest::RunTest(const FString& Parameters)
{
	const FString DisplayName = StaticEnum<ECrowdyRole>()->GetDisplayNameTextByValue(static_cast<int64>(ECrowdyRole::WorldOwned)).ToString();
	TestTrue(FString::Printf(TEXT("the role reads World Owned (got '%s')"), *DisplayName),
		DisplayName.Equals(TEXT("World Owned"), ESearchCase::CaseSensitive));
	return true;
}
#endif

#endif
