#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Mass/CrowdyMassCharacterAvatar.h"
#include "Mass/CrowdyMassEntity.h"
#include "Nodes/CrowdyMassEventCatalog.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The send node's dropdown is built from entity CLASSES, and the SDK's own action event is declared on
// none of them: it lives on the stand-in base every entity is given, which is not an entity class and
// never appears in one's field chain. Admitting it needs a deliberate addition, and the risk in making
// that addition is admitting everything else that is not an entity class along with it.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassEventCatalogTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FCrowdyMassEventClassGroup* FindGroupFor(const FCrowdyMassEventCatalog& Catalog, const UClass* EntityClass)
	{
		return Catalog.Groups.FindByPredicate([EntityClass](const FCrowdyMassEventClassGroup& Group)
		{
			return Group.EntityClass.Get() == EntityClass;
		});
	}

	// A compiled Blueprint actor class, which is what the catalog is asked about at authoring time. Linked
	// before its default object is built: building one against an unlinked class allocates a zero-sized
	// object and runs the actor constructor over it.
	UBlueprintGeneratedClass* MakeCompiledActorClass()
	{
		UClass* ParentClass = AActor::StaticClass();
		UBlueprintGeneratedClass* GeneratedClass =
			NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient);
		GeneratedClass->ClassWithin = ParentClass->ClassWithin;
		GeneratedClass->ClassConstructor = ParentClass->ClassConstructor;
		GeneratedClass->ClassCastFlags |= ParentClass->ClassCastFlags;
		GeneratedClass->SetSuperStruct(ParentClass);
		GeneratedClass->Bind();
		GeneratedClass->StaticLink(/*bRelinkExistingProperties=*/true);
		GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
		return GeneratedClass;
	}
}

// Mutating AppendSdkDeclaredClass in CrowdyMassEventCatalog.cpp turns the first half red (dropping the
// AddUnique leaves the SDK event unreachable from the node, which is the whole of C3's authoring half).
// Mutating IsUsableEntityClass to drop its IsChildOf(AActor) filter turns the second half red, which is
// the failure that would silently re-open the retired non-actor authoring surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCatalogOffersTheSdkActionEventTest,
	"CrowdySDK.MassNodes.CatalogOffersTheSdkActionEvent", CrowdyMassEventCatalogTestFlags)
bool FCrowdyMassCatalogOffersTheSdkActionEventTest::RunTest(const FString& Parameters)
{
	FCrowdyMassEventCatalog Catalog;
	CrowdyMassEventCatalog::Build(Catalog, ECrowdyMassEventCatalogPurpose::Send);

	const FCrowdyMassEventClassGroup* SdkGroup = FindGroupFor(Catalog, UCrowdyMassEntity::StaticClass());
	if (!TestNotNull(TEXT("the catalog offers the SDK's own group, whatever the project configures"), SdkGroup))
	{
		return false;
	}

	// Named for what it means to an author, because the C++ class name is not something anybody authors
	// against: these events go to a crowd entity of any class.
	TestEqual(TEXT("named for the entities it can be sent to rather than for the C++ class"),
		SdkGroup->DisplayName, FString(TEXT("Any Crowd Entity")));

	const FCrowdyMassEventEntry* ActionEntry = SdkGroup->Events.FindByPredicate([](const FCrowdyMassEventEntry& Entry)
	{
		return Entry.DisplayName == TEXT("PlayCrowdyAction");
	});
	if (!TestNotNull(TEXT("and it offers the shipped action event"), ActionEntry))
	{
		return false;
	}
	TestTrue(TEXT("addressed by the class that declares it, which is what the receiver resolves against"),
		ActionEntry->DeclaringClass.Get() == UCrowdyMassEntity::StaticClass());

	// This is the "without admitting everything" half. The avatar class derives the same base and declares
	// Crowdy events of its own, so a change that admitted non-actor classes generally, rather than this one
	// class, would list it too and would put back the authoring surface the roadmap deliberately closed.
	TestNull(TEXT("the avatar class is still not offered as an entity class of its own"),
		FindGroupFor(Catalog, UCrowdyMassCharacterAvatar::StaticClass()));

	for (const FCrowdyMassEventClassGroup& Group : Catalog.Groups)
	{
		const UClass* GroupClass = Group.EntityClass.Get();
		if (GroupClass == UCrowdyMassEntity::StaticClass())
		{
			continue;
		}

		TestTrue(FString::Printf(TEXT("every other group is an Actor class: %s"), *Group.DisplayName),
			GroupClass != nullptr && GroupClass->IsChildOf(AActor::StaticClass()));
	}

	return true;
}

// The other half of the same decision, and the one a shared list gets wrong. An event the crowd plane
// applies itself resolves a stand-in on every machine that receives it and runs the event's own body
// there, so the observer path that hands a call to a registered handler is never reached for it: a graph
// bound to one compiles clean, binds without a warning, and never runs. Sendable is therefore not the
// same question as receivable, and the receive dropdown must not offer it.
//
// Mutating the Purpose test in CrowdyMassEventCatalog::Build so the SDK class is appended either way
// turns the first half red; mutating IsAppliedByTheCrowdPlane turns the second half red, which is the
// compile-time refusal that catches a node already saved with one picked.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCatalogWithholdsPlaneAppliedEventsFromReceiveTest,
	"CrowdySDK.MassNodes.CatalogWithholdsPlaneAppliedEventsFromReceive", CrowdyMassEventCatalogTestFlags)
bool FCrowdyMassCatalogWithholdsPlaneAppliedEventsFromReceiveTest::RunTest(const FString& Parameters)
{
	FCrowdyMassEventCatalog ReceiveCatalog;
	CrowdyMassEventCatalog::Build(ReceiveCatalog, ECrowdyMassEventCatalogPurpose::Receive);

	TestNull(TEXT("the receive list does not offer the events the crowd plane applies itself"),
		FindGroupFor(ReceiveCatalog, UCrowdyMassEntity::StaticClass()));

	// Asserted against the SEND list rather than in the abstract, so this cannot pass by both lists being
	// empty: the one group withheld here is exactly the one the send list is required to carry.
	FCrowdyMassEventCatalog SendCatalog;
	CrowdyMassEventCatalog::Build(SendCatalog, ECrowdyMassEventCatalogPurpose::Send);
	TestNotNull(TEXT("while the send list still does"),
		FindGroupFor(SendCatalog, UCrowdyMassEntity::StaticClass()));

	// The predicate the receive node's compile check reads. A node saved before the dropdown stopped
	// offering the event still holds it, so withholding it from the list is only half the answer.
	UFunction* PlaneApplied = UCrowdyMassEntity::StaticClass()->FindFunctionByName(
		TEXT("PlayCrowdyAction_Implementation"));
	if (!TestNotNull(TEXT("the SDK's own action event resolves"), PlaneApplied))
	{
		return false;
	}
	TestTrue(TEXT("and is named as applied by the plane, which is what the receive node refuses on"),
		CrowdyMassEventCatalog::IsAppliedByTheCrowdPlane(PlaneApplied));

	// An event on a game's own entity class is the case that must NOT be refused, or the receive node
	// would refuse everything it exists for.
	for (const FCrowdyMassEventClassGroup& Group : ReceiveCatalog.Groups)
	{
		for (const FCrowdyMassEventEntry& Entry : Group.Events)
		{
			UClass* DeclaringClass = Entry.DeclaringClass.Get();
			UFunction* Function = DeclaringClass ? DeclaringClass->FindFunctionByName(Entry.FunctionName) : nullptr;
			if (!Function)
			{
				continue;
			}

			TestFalse(FString::Printf(TEXT("an offered event is receivable: %s"), *Entry.DisplayName),
				CrowdyMassEventCatalog::IsAppliedByTheCrowdPlane(Function));
		}
	}

	return true;
}

// A dev-only or test-only entity class carries a Crowdy entity component like any other, so the catalog
// enumerates it and offers an author events that exist only to be called from a harness. Hiding is the
// engine's own class-picker answer rather than a Crowdy marker, so one specifier keeps such a class out of
// every picker at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCatalogSkipsHiddenEntityClassesTest,
	"CrowdySDK.MassNodes.CatalogSkipsHiddenEntityClasses", CrowdyMassEventCatalogTestFlags)
bool FCrowdyMassCatalogSkipsHiddenEntityClassesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> EntityClass(MakeCompiledActorClass());
	if (!TestNotNull(TEXT("the fixture class was built"), EntityClass.Get()))
	{
		return false;
	}

	TestTrue(TEXT("an ordinary actor class is offered"),
		CrowdyMassEventCatalog::IsSelectableEntityClass(EntityClass.Get()));

	EntityClass->ClassFlags |= CLASS_Hidden;
	TestFalse(TEXT("the same class marked Hidden is not"),
		CrowdyMassEventCatalog::IsSelectableEntityClass(EntityClass.Get()));

	EntityClass->ClassFlags &= ~CLASS_Hidden;
	EntityClass->ClassFlags |= CLASS_HideDropDown;
	TestFalse(TEXT("nor is one marked HideDropDown, which is the specifier a details panel honours"),
		CrowdyMassEventCatalog::IsSelectableEntityClass(EntityClass.Get()));

	EntityClass->ClassFlags &= ~CLASS_HideDropDown;
	TestTrue(TEXT("and clearing the flag offers it again, so the answer is read rather than cached"),
		CrowdyMassEventCatalog::IsSelectableEntityClass(EntityClass.Get()));

	// The rest of the filter still has to hold, or a change that only ever asked about hiding would let a
	// reinstancing artefact or a non-actor through.
	TestFalse(TEXT("a non-actor class is still refused"),
		CrowdyMassEventCatalog::IsSelectableEntityClass(UCrowdyMassEntity::StaticClass()));
	TestFalse(TEXT("and so is nothing at all"), CrowdyMassEventCatalog::IsSelectableEntityClass(nullptr));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
