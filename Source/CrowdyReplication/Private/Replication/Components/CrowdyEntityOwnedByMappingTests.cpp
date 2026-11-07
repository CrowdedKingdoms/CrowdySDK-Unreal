#include "Replication/Components/CrowdyEntityComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdySDKCustomVersion.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/Pawn.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Assets saved before Owned By and Replicate Movement existed get them mapped from Ownership and Mode, so every
// entity resolves exactly as before. These cases write the deprecated Ownership on purpose: it is the old data.
PRAGMA_DISABLE_DEPRECATION_WARNINGS

namespace
{
	constexpr EAutomationTestFlags CrowdyOwnedByTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FMapped
	{
		ECrowdyOwnedBy OwnedBy;
		ECrowdyReplicateMovement ReplicateMovement;
	};

	FMapped Map(const bool bOwnershipAuthored, const bool bModeAuthored, const bool bUnderClassDefault,
		const ECrowdyOwnership Ownership, const ECrowdyEntityMode Mode, const FMapped Inherited)
	{
		FMapped Result = Inherited;
		UCrowdyEntityComponent::MapLegacySettings(bOwnershipAuthored, bModeAuthored, bUnderClassDefault, Ownership, Mode,
			Result.OwnedBy, Result.ReplicateMovement);
		return Result;
	}

	TArray<uint8> SaveProperties(UCrowdyEntityComponent* Component)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		FObjectAndNameAsStringProxyArchive Archive(Writer, false);
		Component->Serialize(Archive);
		return Bytes;
	}

	void LoadProperties(UCrowdyEntityComponent* Component, const TArray<uint8>& Bytes, const int32 Version)
	{
		FMemoryReader Reader(Bytes);
		Reader.SetCustomVersion(FCrowdySDKCustomVersion::GUID, Version, TEXT("CrowdySDK"));
		FObjectAndNameAsStringProxyArchive Archive(Reader, true);
		Component->Serialize(Archive);
	}

	UCrowdyEntityComponent* MakeComponent()
	{
		return NewObject<UCrowdyEntityComponent>(GetTransientPackage());
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLegacySettingsMapTest,
	"CrowdySDK.Entity.LegacySettingsMap", CrowdyOwnedByTestFlags)
bool FCrowdyLegacySettingsMapTest::RunTest(const FString& Parameters)
{
	constexpr FMapped Inherited{ ECrowdyOwnedBy::Leader, ECrowdyReplicateMovement::On };

	const FMapped LocalDynamic = Map(true, true, false, ECrowdyOwnership::LocalClient, ECrowdyEntityMode::Dynamic, Inherited);
	TestEqual(TEXT("an authored Local Client is owned by the player"), LocalDynamic.OwnedBy, ECrowdyOwnedBy::Player);
	TestEqual(TEXT("an authored Dynamic sends movement"), LocalDynamic.ReplicateMovement, ECrowdyReplicateMovement::On);

	const FMapped HostStatic = Map(true, true, false, ECrowdyOwnership::Host, ECrowdyEntityMode::Static, Inherited);
	TestEqual(TEXT("an authored Host is World: the host's when placed, the spawner's when spawned, as before"), HostStatic.OwnedBy, ECrowdyOwnedBy::World);
	TestEqual(TEXT("an authored Static sends no movement, even on a pawn"), HostStatic.ReplicateMovement, ECrowdyReplicateMovement::Off);

	const FMapped Untouched = Map(false, false, false, ECrowdyOwnership::LocalClient, ECrowdyEntityMode::Static, Inherited);
	TestEqual(TEXT("an inherited Ownership keeps the archetype's Owned By"), Untouched.OwnedBy, Inherited.OwnedBy);
	TestEqual(TEXT("an inherited Mode keeps the archetype's Replicate Movement"), Untouched.ReplicateMovement, Inherited.ReplicateMovement);

	constexpr FMapped InheritedAuto{ ECrowdyOwnedBy::Leader, ECrowdyReplicateMovement::Auto };
	const FMapped UnderDefault = Map(false, false, true, ECrowdyOwnership::Host, ECrowdyEntityMode::Static, InheritedAuto);
	TestEqual(TEXT("under an unmapped archetype, Ownership stays inherited: Auto still reads it"), UnderDefault.OwnedBy, InheritedAuto.OwnedBy);
	TestEqual(TEXT("under an unmapped archetype, a Static pawn stays without movement rather than turning Auto on"),
		UnderDefault.ReplicateMovement, ECrowdyReplicateMovement::Off);

	const FMapped NativeOn = Map(false, false, true, ECrowdyOwnership::Host, ECrowdyEntityMode::Static, Inherited);
	TestEqual(TEXT("but a C++ parent that now says On keeps its Blueprint children moving, saved or not"),
		NativeOn.ReplicateMovement, ECrowdyReplicateMovement::On);

	const FMapped DynamicUnderDefault = Map(false, false, true, ECrowdyOwnership::Host, ECrowdyEntityMode::Dynamic, Inherited);
	TestEqual(TEXT("under an unmapped archetype, a Dynamic Mode stays inherited: Auto still reads it"),
		DynamicUnderDefault.ReplicateMovement, Inherited.ReplicateMovement);

	const FMapped OnlyMode = Map(false, true, false, ECrowdyOwnership::LocalClient, ECrowdyEntityMode::Dynamic, Inherited);
	TestEqual(TEXT("each field maps on its own: Ownership left inherited"), OnlyMode.OwnedBy, Inherited.OwnedBy);
	TestEqual(TEXT("while the authored Mode maps"), OnlyMode.ReplicateMovement, ECrowdyReplicateMovement::On);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnedByResolutionTest,
	"CrowdySDK.Entity.OwnedByResolution", CrowdyOwnedByTestFlags)
bool FCrowdyOwnedByResolutionTest::RunTest(const FString& Parameters)
{
	using C = UCrowdyEntityComponent;

	TestEqual(TEXT("an explicit Owned By is kept"), C::ResolveOwnedBy(ECrowdyOwnedBy::Leader, ECrowdyOwnership::LocalClient, true), ECrowdyOwnedBy::Leader);
	TestEqual(TEXT("Leader spawned during play stays the Leader's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Leader, ECrowdyOwnership::Host, false), ECrowdyOwnedBy::Leader);
	TestEqual(TEXT("Auto with a C++ Local Client is the player's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Auto, ECrowdyOwnership::LocalClient, true), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("Auto placed in the level is the world's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Auto, ECrowdyOwnership::Host, true), ECrowdyOwnedBy::World);
	TestEqual(TEXT("Auto spawned during play is the spawner's"), C::ResolveOwnedBy(ECrowdyOwnedBy::Auto, ECrowdyOwnership::Host, false), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("World spawned during play is the spawner's too"), C::ResolveOwnedBy(ECrowdyOwnedBy::World, ECrowdyOwnership::Host, false), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("Player wins over a deprecated Host"), C::ResolveOwnedBy(ECrowdyOwnedBy::Player, ECrowdyOwnership::Host, true), ECrowdyOwnedBy::Player);
	TestEqual(TEXT("World wins over a deprecated Local Client"), C::ResolveOwnedBy(ECrowdyOwnedBy::World, ECrowdyOwnership::LocalClient, true), ECrowdyOwnedBy::World);

	TestTrue(TEXT("a character is a pawn with movement"), C::IsPawnWithMovement(ACharacter::StaticClass()));
	TestTrue(TEXT("so is any pawn with a movement component"), C::IsPawnWithMovement(ADefaultPawn::StaticClass()));
	TestFalse(TEXT("a pawn without one is not"), C::IsPawnWithMovement(APawn::StaticClass()));
	TestFalse(TEXT("nor is a plain actor"), C::IsPawnWithMovement(AActor::StaticClass()));

	TestTrue(TEXT("On sends movement"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::On, ECrowdyEntityMode::Static, false));
	TestFalse(TEXT("Off sends none, even for a pawn with movement"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Off, ECrowdyEntityMode::Dynamic, true));
	TestTrue(TEXT("Auto with a C++ Dynamic sends movement"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Auto, ECrowdyEntityMode::Dynamic, false));
	TestTrue(TEXT("Auto on a pawn with movement sends it"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Auto, ECrowdyEntityMode::Static, true));
	TestFalse(TEXT("Auto on anything else sends none"), C::ResolveReplicatesMovement(ECrowdyReplicateMovement::Auto, ECrowdyEntityMode::Static, false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLegacySaveMapsOnLoadTest,
	"CrowdySDK.Entity.LegacySaveMapsOnLoad", CrowdyOwnedByTestFlags)
bool FCrowdyLegacySaveMapsOnLoadTest::RunTest(const FString& Parameters)
{
	// Saved data carries only what differs from the class default, so a field left at the default is absent.
	const TStrongObjectPtr<UCrowdyEntityComponent> OwnershipOnly(MakeComponent());
	OwnershipOnly->Ownership = ECrowdyOwnership::LocalClient;
	const TArray<uint8> OwnershipBytes = SaveProperties(OwnershipOnly.Get());

	const TStrongObjectPtr<UCrowdyEntityComponent> FromOld(MakeComponent());
	LoadProperties(FromOld.Get(), OwnershipBytes, FCrowdySDKCustomVersion::BeforeCustomVersionWasAdded);
	TestEqual(TEXT("an old save that wrote Local Client loads owned by the player"), FromOld->OwnedBy, ECrowdyOwnedBy::Player);
	TestEqual(TEXT("its unwritten Mode, Static under the class default, loads without movement"), FromOld->ReplicateMovement, ECrowdyReplicateMovement::Off);
	TestEqual(TEXT("its unwritten Mode keeps the inherited value"), FromOld->Mode, ECrowdyEntityMode::Static);
	TestEqual(TEXT("and its written Ownership is read as saved"), FromOld->Ownership, ECrowdyOwnership::LocalClient);

	const TStrongObjectPtr<UCrowdyEntityComponent> ModeOnly(MakeComponent());
	ModeOnly->Mode = ECrowdyEntityMode::Dynamic;
	const TArray<uint8> ModeBytes = SaveProperties(ModeOnly.Get());

	const TStrongObjectPtr<UCrowdyEntityComponent> InheritsDynamic(MakeComponent());
	InheritsDynamic->Ownership = ECrowdyOwnership::LocalClient;
	LoadProperties(InheritsDynamic.Get(), ModeBytes, FCrowdySDKCustomVersion::BeforeCustomVersionWasAdded);
	TestEqual(TEXT("an old save that wrote Dynamic loads sending movement"), InheritsDynamic->ReplicateMovement, ECrowdyReplicateMovement::On);
	TestEqual(TEXT("an unwritten Ownership keeps the value the object held before loading"), InheritsDynamic->Ownership, ECrowdyOwnership::LocalClient);
	TestEqual(TEXT("its Owned By stays inherited"), InheritsDynamic->OwnedBy, ECrowdyOwnedBy::Auto);
	TestEqual(TEXT("and still resolves to the player through it"),
		UCrowdyEntityComponent::ResolveOwnedBy(InheritsDynamic->OwnedBy, InheritsDynamic->Ownership, true), ECrowdyOwnedBy::Player);

	const TStrongObjectPtr<UCrowdyEntityComponent> FromCurrent(MakeComponent());
	LoadProperties(FromCurrent.Get(), OwnershipBytes, FCrowdySDKCustomVersion::LatestVersion);
	TestEqual(TEXT("a current save is read as written, with no mapping"), FromCurrent->OwnedBy, ECrowdyOwnedBy::Auto);
	TestEqual(TEXT("for either field"), FromCurrent->ReplicateMovement, ECrowdyReplicateMovement::Auto);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLegacySaveKeepsCppInheritanceTest,
	"CrowdySDK.Entity.LegacySaveKeepsCppInheritance", CrowdyOwnedByTestFlags)
bool FCrowdyLegacySaveKeepsCppInheritanceTest::RunTest(const FString& Parameters)
{
	FCrowdyOwnedByTestWorld TestWorld;

	// A component a C++ actor creates in its constructor: its archetype is that actor's default subobject.
	ACrowdyStateOverlapActor* DynamicSource = TestWorld.World->SpawnActor<ACrowdyStateOverlapActor>();
	ACrowdyStateOverlapActor* DynamicTarget = TestWorld.World->SpawnActor<ACrowdyStateOverlapActor>();
	ACrowdyStateHostOverrideActor* StaticSource = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	ACrowdyStateHostOverrideActor* StaticTarget = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	if (!TestTrue(TEXT("the fixtures spawn"), DynamicSource && DynamicTarget && StaticSource && StaticTarget))
	{
		return false;
	}

	DynamicSource->Entity->Ownership = ECrowdyOwnership::LocalClient;
	LoadProperties(DynamicTarget->Entity, SaveProperties(DynamicSource->Entity), FCrowdySDKCustomVersion::BeforeCustomVersionWasAdded);
	TestEqual(TEXT("an old save that wrote Local Client over a C++ default loads owned by the player"),
		DynamicTarget->Entity->OwnedBy, ECrowdyOwnedBy::Player);
	TestEqual(TEXT("its Dynamic Mode, left to the C++ default, keeps Replicate Movement inherited"),
		DynamicTarget->Entity->ReplicateMovement, ECrowdyReplicateMovement::Auto);
	TestTrue(TEXT("which still resolves to sending movement"), UCrowdyEntityComponent::ResolveReplicatesMovement(
		DynamicTarget->Entity->ReplicateMovement, DynamicTarget->Entity->Mode, false));

	LoadProperties(StaticTarget->Entity, SaveProperties(StaticSource->Entity), FCrowdySDKCustomVersion::BeforeCustomVersionWasAdded);
	TestEqual(TEXT("a Static Mode left to the C++ default is written out as Off, so a pawn does not start sending movement"),
		StaticTarget->Entity->ReplicateMovement, ECrowdyReplicateMovement::Off);
	TestEqual(TEXT("while its Ownership, left to the C++ default, stays inherited"),
		StaticTarget->Entity->OwnedBy, ECrowdyOwnedBy::Auto);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnedBySaveRoundTripsTest,
	"CrowdySDK.Entity.OwnedBySaveRoundTrips", CrowdyOwnedByTestFlags)
bool FCrowdyOwnedBySaveRoundTripsTest::RunTest(const FString& Parameters)
{
	// Owned By is the edited setting now, so a save writes it as set and a load reads it back untouched.
	const TStrongObjectPtr<UCrowdyEntityComponent> Source(MakeComponent());
	Source->OwnedBy = ECrowdyOwnedBy::World;
	Source->Ownership = ECrowdyOwnership::LocalClient;
	const TStrongObjectPtr<UCrowdyEntityComponent> Target(MakeComponent());
	LoadProperties(Target.Get(), SaveProperties(Source.Get()), FCrowdySDKCustomVersion::LatestVersion);
	TestEqual(TEXT("Owned By loads as it was set, whatever the deprecated Ownership holds"), Target->OwnedBy, ECrowdyOwnedBy::World);

	Source->ReplicateMovement = ECrowdyReplicateMovement::Off;
	Source->Mode = ECrowdyEntityMode::Dynamic;
	LoadProperties(Target.Get(), SaveProperties(Source.Get()), FCrowdySDKCustomVersion::LatestVersion);
	TestEqual(TEXT("Replicate Movement loads as it was set, whatever the deprecated Mode holds"), Target->ReplicateMovement, ECrowdyReplicateMovement::Off);
	TestFalse(TEXT("and an explicit Off wins over it"), Target->ReplicatesMovementFor(ACharacter::StaticClass()));
	Source->ReplicateMovement = ECrowdyReplicateMovement::Auto;
	TestEqual(TEXT("the deprecated Get Mode reads the setting"), Source->GetMode(), ECrowdyEntityMode::Dynamic);
	Source->ReplicateMovement = ECrowdyReplicateMovement::Off;
	TestEqual(TEXT("and reads Off as Static"), Source->GetMode(), ECrowdyEntityMode::Static);

	TestEqual(TEXT("the deprecated Get Ownership reads World as Host"), Source->GetOwnership(), ECrowdyOwnership::Host);
	Source->OwnedBy = ECrowdyOwnedBy::Player;
	Source->Ownership = ECrowdyOwnership::Host;
	TestEqual(TEXT("and Player as Local Client, whatever the stale field holds"), Source->GetOwnership(), ECrowdyOwnership::LocalClient);
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEditingOwnedByRetiresOwnershipTest,
	"CrowdySDK.Entity.EditingOwnedByRetiresOwnership", CrowdyOwnedByTestFlags)
bool FCrowdyEditingOwnedByRetiresOwnershipTest::RunTest(const FString& Parameters)
{
	// A component that once saved Local Client, set back to Auto in the Details panel, means what Auto says.
	const TStrongObjectPtr<UCrowdyEntityComponent> Component(MakeComponent());
	Component->Ownership = ECrowdyOwnership::LocalClient;
	Component->OwnedBy = ECrowdyOwnedBy::Auto;

	FProperty* OwnedByProperty = FindFProperty<FProperty>(UCrowdyEntityComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEntityComponent, OwnedBy));
	FPropertyChangedEvent Edit(OwnedByProperty);
	Component->PostEditChangeProperty(Edit);
	TestEqual(TEXT("an edit of Owned By retires the deprecated Ownership"),
		UCrowdyEntityComponent::ResolveOwnedBy(Component->OwnedBy, Component->Ownership, true), ECrowdyOwnedBy::World);

	// One that once saved Dynamic, set back to Auto, means Auto too: on a plain actor that sends nothing.
	Component->Mode = ECrowdyEntityMode::Dynamic;
	Component->ReplicateMovement = ECrowdyReplicateMovement::Auto;
	FProperty* MovementProperty = FindFProperty<FProperty>(UCrowdyEntityComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UCrowdyEntityComponent, ReplicateMovement));
	FPropertyChangedEvent MovementEdit(MovementProperty);
	Component->PostEditChangeProperty(MovementEdit);
	TestFalse(TEXT("an edit of Replicate Movement retires the deprecated Mode"), Component->ReplicatesMovementFor(AActor::StaticClass()));
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAddedComponentSendsNoMovementTest,
	"CrowdySDK.Entity.AddedComponentSendsNoMovement", CrowdyOwnedByTestFlags)
bool FCrowdyAddedComponentSendsNoMovementTest::RunTest(const FString& Parameters)
{
	FCrowdyOwnedByTestWorld TestWorld;
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	ACharacter* Character = TestWorld.World->SpawnActor<ACharacter>();
	const UCrowdyEntityComponent* Component = Character ? Entities->AddStaticEntityComponentForTest(Character) : nullptr;
	if (!TestNotNull(TEXT("a component is added"), Component)) return false;

	TestFalse(TEXT("a component added at run time to a character keeps it without movement, as Static did"),
		UCrowdyEntityComponent::ResolveReplicatesMovement(Component->ReplicateMovement, Component->Mode,
			UCrowdyEntityComponent::IsPawnWithMovement(Character->GetClass())));
	return true;
}

PRAGMA_ENABLE_DEPRECATION_WARNINGS

#endif
