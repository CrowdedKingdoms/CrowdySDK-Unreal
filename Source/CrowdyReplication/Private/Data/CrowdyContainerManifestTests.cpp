#include "Data/CrowdyContainerManifest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyManifestTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The manifest's component key must be the key the runtime ensures with, so it is checked against a real enrolment
// through RegisterSubParticipant rather than against a copy of the seed formula: if the runtime's derivation ever
// changes, this fails instead of the manifest silently drifting from it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestComponentKeyMatchesRuntimeTest,
	"CrowdySDK.GameModel.ManifestComponentKeyMatchesRuntime", CrowdyManifestTestFlags)
bool FCrowdyManifestComponentKeyMatchesRuntimeTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("anchor registered"), AnchorNetID.IsValid()))
	{
		return false;
	}

	UCrowdyGameModelTestComponent* Component = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid SubNetID = Entities->RegisterSubParticipant(Component, AnchorNetID);
	TestEqual(TEXT("component key equals the enrolled sub-participant's key"),
		FCrowdyContainerManifestKeys::ComponentKey(AnchorNetID, Component->GetClass(), Component->GetName()),
		FCrowdyModelIdentity::NetIDToContainerKey(SubNetID));

	UCrowdyGameModelTestComponent* Keyed = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid KeyedNetID = Entities->RegisterSubParticipant(Keyed, AnchorNetID, TEXT("chest_left"));
	TestEqual(TEXT("an authored key replaces the object name in the seed"),
		FCrowdyContainerManifestKeys::ComponentKey(AnchorNetID, Keyed->GetClass(), TEXT("chest_left")),
		FCrowdyModelIdentity::NetIDToContainerKey(KeyedNetID));
	return true;
}

// An authored binding key names the actor instead of its placement guid, the same precedence ResolveIdentity applies.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestActorNetIDPrecedenceTest,
	"CrowdySDK.GameModel.ManifestActorNetIDPrecedence", CrowdyManifestTestFlags)
bool FCrowdyManifestActorNetIDPrecedenceTest::RunTest(const FString& Parameters)
{
	const FGuid Placement(11, 22, 33, 44);
	TestEqual(TEXT("no key: the placement guid"), FCrowdyContainerManifestKeys::ActorNetID(Placement, FString()), Placement);
	TestEqual(TEXT("a key: the key's NetID"), FCrowdyContainerManifestKeys::ActorNetID(Placement, TEXT("boss_1")),
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("boss_1")));
	TestEqual(TEXT("the key of a NetID is its digest"), FCrowdyContainerManifestKeys::KeyForNetID(Placement),
		FCrowdyModelIdentity::NetIDToContainerKey(Placement));
	TestTrue(TEXT("an invalid NetID has no key"), FCrowdyContainerManifestKeys::KeyForNetID(FGuid()).IsEmpty());
	return true;
}

// Two rows with one (type, key) would fight over one server row; the same key under another type is a different row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyManifestDuplicateRowsTest,
	"CrowdySDK.GameModel.ManifestDuplicateRows", CrowdyManifestTestFlags)
bool FCrowdyManifestDuplicateRowsTest::RunTest(const FString& Parameters)
{
	UCrowdyContainerManifest* Manifest = NewObject<UCrowdyContainerManifest>(GetTransientPackage());
	auto Add = [Manifest](const TCHAR* Type, const TCHAR* Key)
	{
		FCrowdyContainerManifestRow& Row = Manifest->Rows.AddDefaulted_GetRef();
		Row.TypeName = Type;
		Row.BindingKey = Key;
	};
	Add(TEXT("Camp"), TEXT("aaaa"));
	Add(TEXT("Turret"), TEXT("aaaa"));
	Add(TEXT("Camp"), TEXT("bbbb"));
	Add(TEXT("Camp"), TEXT("aaaa"));
	Add(TEXT("camp"), TEXT("aaaa"));

	const TArray<int32> Duplicates = Manifest->FindDuplicateRows();
	TestEqual(TEXT("one duplicate: a different type or a different case is another row"), Duplicates.Num(), 1);
	TestTrue(TEXT("the repeat is row 3"), Duplicates.Contains(3));
	return true;
}

#endif
