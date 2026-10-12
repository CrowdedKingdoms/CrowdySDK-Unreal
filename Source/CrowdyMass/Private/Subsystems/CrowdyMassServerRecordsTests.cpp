#include "Subsystems/CrowdyMassServerRecordsTestTypes.h"

ACrowdyMassServerRecordsTestActor::ACrowdyMassServerRecordsTestActor()
{
	Records = CreateDefaultSubobject<UCrowdyServerObjectComponent>(TEXT("Records"));
}

void UCrowdyMassServerRecordsTestHandler::OnChanged(FCrowdyMassEntityTarget Target, const TArray<FString>& Changed)
{
	++Calls;
	LastEntity = Target.Entity;
	LastChanged = Changed;
}

void UCrowdyMassServerRecordsTestHandler::OnNearTierChanged(const TArray<FString>& Changed)
{
	++NearTierCalls;
	NearTierLastChanged = Changed;
}

void UCrowdyMassServerRecordsTestHandler::WrongShape(int32 Value)
{
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/CrowdyMassNearTierComponent.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Derivation/CrowdyMassServerRecordsDerivation.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/GameInstance.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Fragments/CrowdyMassFragments.h"
#include "MassEntityManager.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Subsystems/CrowdyMassServerRecordsSubsystem.h"
#include "Subsystems/CrowdyMassStateLogCapture.h"
#include "Target/CrowdyMassServerValuesLibrary.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

struct FCrowdyMassServerRecordsTestAccess
{
	static void Bind(UCrowdyMassNearTierComponent& View, UCrowdyMassServerRecordsSubsystem* Records, FMassEntityHandle Entity, const FGuid& UUID)
	{
		View.ServerRecordsForTest = Records;
		View.SetBoundEntity(Entity, UUID);
	}
};

namespace CrowdyMassServerRecordsTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr double ChunkSize = 100.0;
	const TCHAR* const CrowdInstance = TEXT("crowd");

	FString RecordIdOf(const FGuid& UUID)
	{
		return UUID.ToString(EGuidFormats::Digits);
	}

	/** A linked Blueprint class with a default object, parented to Parent, as a compiled Blueprint is. */
	UBlueprintGeneratedClass* MakeCompiledClass(UClass* Parent)
	{
		UBlueprintGeneratedClass* Class = NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient);
		Class->ClassWithin = Parent->ClassWithin;
		Class->ClassConstructor = Parent->ClassConstructor;
		Class->ClassCastFlags |= Parent->ClassCastFlags;
		Class->SetSuperStruct(Parent);
		Class->Bind();
		Class->StaticLink(/*bRelinkExistingProperties=*/true);
		Class->GetDefaultObject(/*bCreateIfNeeded=*/true);
		return Class;
	}

	/** A Blueprint class whose construction script adds the component with Definition and Mode, as the Blueprint editor does. */
	UBlueprintGeneratedClass* MakeBlueprintClass(UCrowdyServerObjectDefinition* Definition, ECrowdyServerObjectInstanceMode Mode)
	{
		UBlueprintGeneratedClass* Class = MakeCompiledClass(AActor::StaticClass());
		Class->SimpleConstructionScript = NewObject<USimpleConstructionScript>(Class, NAME_None, RF_Transient);
		UCrowdyServerObjectComponent* Template = NewObject<UCrowdyServerObjectComponent>(Class, NAME_None, RF_Transient);
		Template->Definition = Definition;
		Template->InstanceMode = Mode;
		Template->InstanceId = CrowdInstance;
		USCS_Node* Node = NewObject<USCS_Node>(Class->SimpleConstructionScript, NAME_None, RF_Transient);
		Node->ComponentClass = UCrowdyServerObjectComponent::StaticClass();
		Node->ComponentTemplate = Template;
		Class->SimpleConstructionScript->AddNode(Node);
		return Class;
	}

	/** Puts the native test actor's component on Definition for as long as it lives. */
	struct FNativeComponentScope
	{
		UCrowdyServerObjectComponent* Component = nullptr;

		explicit FNativeComponentScope(UCrowdyServerObjectDefinition* Definition)
		{
			Component = GetMutableDefault<ACrowdyMassServerRecordsTestActor>()->Records;
			if (Component)
			{
				Component->Definition = Definition;
				Component->InstanceMode = ECrowdyServerObjectInstanceMode::InstanceId;
				Component->InstanceId = CrowdInstance;
			}
		}

		~FNativeComponentScope()
		{
			if (Component)
			{
				Component->Definition = nullptr;
				Component->InstanceId.Reset();
			}
		}
	};

	/** Rows in a standalone entity manager, Server Objects with no connection, and the binder joining them. */
	struct FFixture
	{
		FAutomationTestBase& Test;
		TSharedPtr<FMassEntityManager> EntityManager;
		TStrongObjectPtr<UCrowdyMassEntitySubsystem> Rows;
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyServerObjectSubsystem> ServerObjects;
		TStrongObjectPtr<UCrowdyMassServerRecordsSubsystem> Binder;
		TStrongObjectPtr<UCrowdyMassServerRecordsTestHandler> Handler;
		bool bTornDown = false;

		explicit FFixture(FAutomationTestBase& InTest)
			: Test(InTest)
		{
			// The manager needs an owner: its relation observers are outered to it.
			EntityManager = MakeShareable(new FMassEntityManager(GetTransientPackage()));
			FMassEntityManagerStorageInitParams InitParams;
			InitParams.Emplace<FMassEntityManager_InitParams_Concurrent>();
			EntityManager->Initialize(InitParams);
			EntityManager->PostInitialize();
			Rows.Reset(NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage()));
			Rows->SetMassEntityManagerForTest(EntityManager.Get());

			Instance.Reset(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
			Session.Reset(NewObject<UCrowdyGameSession>(Instance.Get()));
			Session->SetChunkSize(ChunkSize);
			ServerObjects.Reset(NewObject<UCrowdyServerObjectSubsystem>(Instance.Get()));
			ServerObjects->InitializeForTest(nullptr, Session.Get());

			Binder.Reset(NewObject<UCrowdyMassServerRecordsSubsystem>(GetTransientPackage()));
			Binder->InitializeForTest(Rows.Get(), ServerObjects.Get());
			Handler.Reset(NewObject<UCrowdyMassServerRecordsTestHandler>(GetTransientPackage()));
		}

		~FFixture()
		{
			TearDown();
			ServerObjects->Deinitialize();
			EntityManager->Deinitialize();
		}

		void TearDown()
		{
			if (!bTornDown)
			{
				bTornDown = true;
				Binder->Deinitialize();
			}
		}

		TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(ECrowdyServerObjectHolds Holds) const
		{
			TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
			Definition->TypeName = TEXT("mass_bots");
			Definition->Holds = Holds;
			Definition->State = FCrowdyMassServerRecordsTestRecord::StaticStruct();
			Definition->WatchedFields = {TEXT("Health"), TEXT("Ammo")};
			Definition->AreaSizeChunks = 1;
			Definition->InterestRadiusAreas = 1;
			TArray<FString> Errors;
			const bool bBaked = Definition->Bake(Errors);
			Test.TestTrue(FString::Printf(TEXT("the test definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
			return Definition;
		}

		/** A row of Class, as a spawn records it. */
		FMassEntityHandle MakeRow(const FGuid& UUID, const UClass* Class) const
		{
			const UScriptStruct* Fragments[] = {FCrowdyRemoteIdentityFragment::StaticStruct()};
			const FMassArchetypeHandle Archetype = EntityManager->CreateArchetype(MakeArrayView(Fragments, 1));
			const FMassEntityHandle Entity = EntityManager->CreateEntity(Archetype);
			Rows->RegisterTrackedEntityForTest(UUID, Entity);
			Rows->SetStateEntityClassForTest(UUID, Class);
			return Entity;
		}

		static void Apply(UCrowdyServerObject* Object, const FGuid& UUID, int32 Health, int32 Ammo)
		{
			FCrowdyMassServerRecordsTestRecord Record;
			Record.Health = Health;
			Record.Ammo = Ammo;
			TArray<TPair<FString, FInstancedStruct>> Changed;
			Changed.Emplace(RecordIdOf(UUID), FInstancedStruct::Make(Record));
			Object->ApplyRecordsForTest(Changed, TArray<FString>());
		}

		static void Remove(UCrowdyServerObject* Object, const FGuid& UUID)
		{
			Object->ApplyRecordsForTest(TArray<TPair<FString, FInstancedStruct>>(), TArray<FString>{RecordIdOf(UUID)});
		}

		int32 HealthOf(FMassEntityHandle Entity) const
		{
			const FInstancedStruct* Values = Binder->FindServerValues(Entity);
			const FCrowdyMassServerRecordsTestRecord* Record = Values ? Values->GetPtr<FCrowdyMassServerRecordsTestRecord>() : nullptr;
			return Record ? Record->Health : -1;
		}

		void TickServerObjects(float Seconds) const
		{
			for (float Left = Seconds; Left > 0.f; Left -= 1.f)
			{
				ServerObjects->TickForTest(1.f);
			}
		}
	};

	bool NamesAre(const TArray<FString>& Names, const TArray<FString>& Expected)
	{
		if (Names.Num() != Expected.Num())
		{
			return false;
		}
		for (const FString& Name : Expected)
		{
			if (!Names.Contains(Name))
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsDerivationRoutesTest,
	"CrowdySDK.Mass.ServerRecordsDerivationFindsTheComponentOnEveryRoute", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsDerivationRoutesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);

	{
		const FNativeComponentScope Scope(Definition.Get());
		const FCrowdyMassDerivedServerRecords Native = FCrowdyMassServerRecordsDerivation::FromClass(ACrowdyMassServerRecordsTestActor::StaticClass());
		TestTrue(TEXT("a component declared in C++ is found"), Native.Definition == Definition.Get() && Native.bSupported);
		TestEqual(TEXT("through the class default object"), static_cast<int32>(Native.Route), static_cast<int32>(ECrowdyMassEntityTraitsRoute::NativeComponent));
		TestEqual(TEXT("with its Instance Id"), Native.InstanceId, FString(CrowdInstance));
	}

	const TStrongObjectPtr<UBlueprintGeneratedClass> Blueprint(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));
	const FCrowdyMassDerivedServerRecords FromBlueprint = FCrowdyMassServerRecordsDerivation::FromClass(Blueprint.Get());
	TestTrue(TEXT("a component added in the Blueprint is found"), FromBlueprint.Definition == Definition.Get() && FromBlueprint.bSupported);
	TestEqual(TEXT("through its construction script"), static_cast<int32>(FromBlueprint.Route), static_cast<int32>(ECrowdyMassEntityTraitsRoute::BlueprintComponent));
	TestTrue(TEXT("following the areas"), FromBlueprint.bArea && FromBlueprint.InstanceId.IsEmpty());

	const TStrongObjectPtr<UBlueprintGeneratedClass> Child(MakeCompiledClass(Blueprint.Get()));
	const FCrowdyMassDerivedServerRecords Inherited = FCrowdyMassServerRecordsDerivation::FromClass(Child.Get());
	TestTrue(TEXT("a child class inherits its parent's component"), Inherited.Definition == Definition.Get() && Inherited.bArea);

	const FCrowdyMassDerivedServerRecords Plain = FCrowdyMassServerRecordsDerivation::FromClass(AActor::StaticClass());
	TestTrue(TEXT("a class with no component has none, and nothing to warn about"), !Plain.Definition && Plain.Reason.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsDerivationOneSetTest,
	"CrowdySDK.Mass.ServerRecordsDerivationIgnoresOneSetDefinitions", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsDerivationOneSetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> OneSet = Fixture.MakeDefinition(ECrowdyServerObjectHolds::OneSet);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(OneSet.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FCrowdyMassDerivedServerRecords Derived = FCrowdyMassServerRecordsDerivation::FromClass(Class.Get());
	TestNull(TEXT("a component whose definition keeps one set of variables gives rows nothing"), Derived.Definition);
	TestTrue(TEXT("and is not reported as a mistake"), Derived.Reason.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsUnsupportedModeTest,
	"CrowdySDK.Mass.ServerRecordsUnsupportedModeWarnsOnce", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsUnsupportedModeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::SignedInPlayer));

	FCrowdyMassStateLogCapture Capture;
	Fixture.MakeRow(FGuid::NewGuid(), Class.Get());
	Fixture.MakeRow(FGuid::NewGuid(), Class.Get());
	Fixture.Binder->FlushForTest();
	Fixture.Binder->RefreshForTest();

	const TArray<FString> Warnings = Capture.TakeWarnings();
	TestEqual(TEXT("a mode rows cannot follow is warned about once for the class"), Warnings.Num(), 1);
	TestTrue(TEXT("naming the mode"), Warnings.Num() == 1 && Warnings[0].Contains(TEXT("SignedInPlayer")));
	TestNull(TEXT("and its rows read from no definition"), Fixture.Binder->FindClassRecords(Class.Get()).Definition.Get());
	TestEqual(TEXT("so nothing is acquired"), Fixture.Binder->NumHeldForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsAreasTest,
	"CrowdySDK.Mass.ServerRecordsFollowAreasAroundTheView", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsAreasTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));

	Fixture.Binder->SetViewLocationForTest(FVector(50.0, 50.0, 0.0));
	Fixture.MakeRow(FGuid::NewGuid(), Class.Get());
	Fixture.Binder->FlushForTest();

	TestEqual(TEXT("a radius of 1 follows the 3 by 3 areas around the view"), Fixture.Binder->NumHeldForTest(), 9);
	TestNotNull(TEXT("from -1,-1"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("-1,-1")));
	TestNotNull(TEXT("to 1,1"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("1,1")));
	TestNull(TEXT("and no further"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("2,0")));
	UCrowdyServerObject* Left = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("-1,0"));

	Fixture.Binder->SetViewLocationForTest(FVector(150.0, 50.0, 0.0));
	Fixture.Binder->RefreshForTest();
	TestNotNull(TEXT("crossing into the next area takes the column ahead"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("2,0")));
	TestNotNull(TEXT("and keeps the one just behind, one area beyond the radius"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("-1,0")));
	TestEqual(TEXT("so twelve are held"), Fixture.Binder->NumHeldForTest(), 12);

	Fixture.Binder->SetViewLocationForTest(FVector(50.0, 50.0, 0.0));
	Fixture.Binder->RefreshForTest();
	TestEqual(TEXT("stepping back over the border releases nothing"), Fixture.Binder->NumHeldForTest(), 12);
	TestEqual(TEXT("and acquires nothing"), Fixture.ServerObjects->NumObjectsForTest(), 12);

	Fixture.Binder->SetViewLocationForTest(FVector(350.0, 50.0, 0.0));
	Fixture.Binder->RefreshForTest();
	TestNull(TEXT("moving three areas on lets go of the columns more than one beyond the radius"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("-1,0")));
	TestNull(TEXT("both of them"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("0,0")));
	TestNotNull(TEXT("and keeps the one just behind"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("1,0")));
	TestEqual(TEXT("so twelve are held again"), Fixture.Binder->NumHeldForTest(), 12);
	TestTrue(TEXT("whose changes it no longer hears"), Left && !Left->OnRecordsChanged.IsBound());
	TestEqual(TEXT("the released areas wait out their grace"), Fixture.ServerObjects->NumObjectsForTest(), 18);

	Fixture.TickServerObjects(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestEqual(TEXT("and are given back after it"), Fixture.ServerObjects->NumObjectsForTest(), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsRowReadTest,
	"CrowdySDK.Mass.ServerRecordsRowReadFindsItsRecord", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsRowReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();
	const FMassEntityHandle RowA = Fixture.MakeRow(A, Class.Get());
	const FMassEntityHandle RowB = Fixture.MakeRow(B, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}

	const FProperty* HealthProperty = FCrowdyMassServerRecordsTestRecord::StaticStruct()->FindPropertyByName(TEXT("Health"));
	int32 Health = 7;
	TestFalse(TEXT("a row reads nothing before its record arrives"), Fixture.Binder->ReadServerValueInto(RowA, TEXT("Health"), HealthProperty, &Health));
	TestEqual(TEXT("and gets the pin's default"), Health, 0);

	FFixture::Apply(Object, A, 42, 3);
	TestTrue(TEXT("a row reads its own record"), Fixture.Binder->ReadServerValueInto(RowA, TEXT("Health"), HealthProperty, &Health));
	TestEqual(TEXT("its value"), Health, 42);
	TestEqual(TEXT("as the native read does"), Fixture.HealthOf(RowA), 42);
	TestNull(TEXT("another row's record is not its"), Fixture.Binder->FindServerValues(RowB));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsChangeTest,
	"CrowdySDK.Mass.ServerRecordsChangeFiresOnlyThatRowsClassHandler", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsChangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();
	const FMassEntityHandle RowA = Fixture.MakeRow(A, Class.Get());
	Fixture.MakeRow(B, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}

	TestTrue(TEXT("a handler of the right shape binds"), Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged")));
	TestTrue(TEXT("binding it again is the same binding"), Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged")));
	FCrowdyMassStateLogCapture Capture;
	TestFalse(TEXT("one of another shape is refused"), Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("WrongShape")));
	TestEqual(TEXT("and says so"), Capture.TakeWarnings().Num(), 1);
	int32 NativeCalls = 0;
	Fixture.Binder->AddClassHandler(AActor::StaticClass(), FOnCrowdyMassServerValuesChanged::FDelegate::CreateLambda(
		[&NativeCalls](const FCrowdyMassEntityTarget&, TConstArrayView<FString>) { ++NativeCalls; }));

	FFixture::Apply(Object, A, 100, 0);
	FFixture::Apply(Object, B, 100, 0);
	TestEqual(TEXT("each new record fires its row's handler once"), Fixture.Handler->Calls, 2);
	Fixture.Handler->Calls = 0;
	NativeCalls = 0;

	FFixture::Apply(Object, A, 60, 0);
	TestEqual(TEXT("a change fires the handler once"), Fixture.Handler->Calls, 1);
	TestTrue(TEXT("for the row that changed"), Fixture.Handler->LastEntity == RowA);
	TestTrue(TEXT("naming only what changed"), NamesAre(Fixture.Handler->LastChanged, {TEXT("Health")}));
	TestEqual(TEXT("a handler of a base class hears it too"), NativeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsRecordBeforeRowTest,
	"CrowdySDK.Mass.ServerRecordsRecordBeforeRowFiresOnceWhenTheRowAppears", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsRecordBeforeRowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	Fixture.MakeRow(FGuid::NewGuid(), Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}
	Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));

	const FGuid Late = FGuid::NewGuid();
	FFixture::Apply(Object, Late, 80, 5);
	TestEqual(TEXT("a record with no row yet fires nothing"), Fixture.Handler->Calls, 0);

	const FMassEntityHandle Row = Fixture.MakeRow(Late, Class.Get());
	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("its row appearing fires once"), Fixture.Handler->Calls, 1);
	TestTrue(TEXT("for that row"), Fixture.Handler->LastEntity == Row);
	TestTrue(TEXT("naming every variable"), NamesAre(Fixture.Handler->LastChanged, {TEXT("Health"), TEXT("Ammo")}));

	Fixture.Binder->FlushForTest();
	Fixture.Rows->SetStateEntityClassForTest(Late, Class.Get());
	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("and only once"), Fixture.Handler->Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsAlreadyReadyTest,
	"CrowdySDK.Mass.ServerRecordsObjectAlreadyReadyAnnouncesOnce", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsAlreadyReadyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	// Another owner, such as an actor's component, already holds the instance and has read its record.
	FString Error;
	UCrowdyServerObject* Object = Fixture.ServerObjects->Acquire(Definition.Get(), CrowdInstance, Fixture.Handler.Get(), Error);
	if (!TestNotNull(TEXT("another owner holds the instance"), Object))
	{
		return false;
	}
	const FGuid A = FGuid::NewGuid();
	FFixture::Apply(Object, A, 70, 0);
	Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));

	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("the row appearing beside a ready instance fires once"), Fixture.Handler->Calls, 1);
	TestTrue(TEXT("for that row"), Fixture.Handler->LastEntity == Row);
	Fixture.ServerObjects->Release(Object, Fixture.Handler.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsRemovalTest,
	"CrowdySDK.Mass.ServerRecordsRemovalFiresWithNoNames", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsRemovalTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}
	Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));
	FFixture::Apply(Object, A, 50, 0);
	Fixture.Handler->Calls = 0;

	FFixture::Remove(Object, A);
	TestEqual(TEXT("a removed record fires once"), Fixture.Handler->Calls, 1);
	TestTrue(TEXT("for its row"), Fixture.Handler->LastEntity == Row);
	TestTrue(TEXT("naming nothing"), Fixture.Handler->LastChanged.IsEmpty());
	TestNull(TEXT("and the row reads nothing after"), Fixture.Binder->FindServerValues(Row));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsCrossingTest,
	"CrowdySDK.Mass.ServerRecordsEntityCrossingAreasIsNotRemoved", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsCrossingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));

	Fixture.Binder->SetViewLocationForTest(FVector(50.0, 50.0, 0.0));
	const FGuid A = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	Fixture.Binder->RefreshForTest();
	UCrowdyServerObject* From = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("0,0"));
	UCrowdyServerObject* To = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("1,0"));
	if (!TestNotNull(TEXT("the area it leaves is held"), From) || !TestNotNull(TEXT("and the one it enters"), To))
	{
		return false;
	}
	Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));
	FFixture::Apply(From, A, 50, 0);

	// The entity walks into the next area: the server adds its record there, then removes it from the area it left.
	FFixture::Apply(To, A, 40, 0);
	Fixture.Handler->Calls = 0;
	FFixture::Remove(From, A);
	TestEqual(TEXT("leaving an area for another held one fires nothing"), Fixture.Handler->Calls, 0);
	TestNotNull(TEXT("and the row still reads its record"), Fixture.Binder->FindServerValues(Row));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsOtherTypeTest,
	"CrowdySDK.Mass.ServerRecordsAnotherTypesRecordIsNotTheRows", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsOtherTypeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Mine = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Other = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> MyClass(MakeBlueprintClass(Mine.Get(), ECrowdyServerObjectInstanceMode::InstanceId));
	const TStrongObjectPtr<UBlueprintGeneratedClass> OtherClass(MakeBlueprintClass(Other.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	Fixture.MakeRow(A, MyClass.Get());
	Fixture.MakeRow(FGuid::NewGuid(), OtherClass.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* OtherObject = Fixture.Binder->FindHeldForTest(Other.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the other class's type is held"), OtherObject))
	{
		return false;
	}
	Fixture.Binder->BindObjectHandler(MyClass.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));
	Fixture.Handler->Calls = 0;

	// The other type keeps a record under this row's id too; the row's class reads only its own type.
	FFixture::Apply(OtherObject, A, 50, 0);
	TestEqual(TEXT("a record of another type does not reach the row's handler"), Fixture.Handler->Calls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsNoComponentTest,
	"CrowdySDK.Mass.ServerRecordsClassWithoutRecordsAcquiresNothing", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsNoComponentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> OneSet = Fixture.MakeDefinition(ECrowdyServerObjectHolds::OneSet);
	const TStrongObjectPtr<UBlueprintGeneratedClass> OneSetClass(MakeBlueprintClass(OneSet.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	Fixture.Binder->SetViewLocationForTest(FVector::ZeroVector);
	Fixture.MakeRow(FGuid::NewGuid(), AActor::StaticClass());
	Fixture.MakeRow(FGuid::NewGuid(), OneSetClass.Get());
	Fixture.Binder->FlushForTest();
	Fixture.Binder->RefreshForTest();
	TestEqual(TEXT("rows of classes with no records component hold nothing"), Fixture.Binder->NumHeldForTest(), 0);
	TestEqual(TEXT("and acquire nothing"), Fixture.ServerObjects->NumObjectsForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsTeardownTest,
	"CrowdySDK.Mass.ServerRecordsTeardownReleasesEverything", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsTeardownTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Areas(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));
	const TStrongObjectPtr<UBlueprintGeneratedClass> Single(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	Fixture.Binder->SetViewLocationForTest(FVector::ZeroVector);
	Fixture.MakeRow(FGuid::NewGuid(), Areas.Get());
	Fixture.MakeRow(FGuid::NewGuid(), Single.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	TestEqual(TEXT("both classes' objects are held"), Fixture.Binder->NumHeldForTest(), 10);

	Fixture.TearDown();
	TestEqual(TEXT("teardown lets go of every one"), Fixture.Binder->NumHeldForTest(), 0);
	TestTrue(TEXT("and stops hearing them"), Object && !Object->OnRecordsChanged.IsBound());
	Fixture.TickServerObjects(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestEqual(TEXT("so they are given back"), Fixture.ServerObjects->NumObjectsForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsNearTierTest,
	"CrowdySDK.Mass.ServerRecordsNearTierPassesValuesThrough", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsNearTierTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	const FGuid OtherId = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.MakeRow(OtherId, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}
	FFixture::Apply(Object, A, 30, 1);

	const TStrongObjectPtr<UCrowdyMassNearTierComponent> View(NewObject<UCrowdyMassNearTierComponent>(GetTransientPackage()));
	View->OnServerValuesChanged.AddDynamic(Fixture.Handler.Get(), &UCrowdyMassServerRecordsTestHandler::OnNearTierChanged);
	FCrowdyMassServerRecordsTestAccess::Bind(*View, Fixture.Binder.Get(), Row, A);

	const FProperty* HealthProperty = FCrowdyMassServerRecordsTestRecord::StaticStruct()->FindPropertyByName(TEXT("Health"));
	int32 Health = 0;
	TestTrue(TEXT("the promoted actor reads its entity's value"), View->ReadServerValueInto(TEXT("Health"), HealthProperty, &Health));
	TestEqual(TEXT("the row's value"), Health, 30);

	FFixture::Apply(Object, A, 20, 1);
	TestEqual(TEXT("and hears its change"), Fixture.Handler->NearTierCalls, 1);
	TestTrue(TEXT("naming what changed"), NamesAre(Fixture.Handler->NearTierLastChanged, {TEXT("Health")}));
	FFixture::Apply(Object, OtherId, 5, 5);
	TestEqual(TEXT("but not another entity's"), Fixture.Handler->NearTierCalls, 1);

	View->UnbindFromEntity();
	FFixture::Apply(Object, A, 10, 1);
	TestEqual(TEXT("an unbound actor hears nothing more"), Fixture.Handler->NearTierCalls, 1);
	TestFalse(TEXT("and reads nothing"), View->ReadServerValueInto(TEXT("Health"), HealthProperty, &Health));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsLastAnnouncerTest,
	"CrowdySDK.Mass.ServerRecordsReadFollowsTheLastAnnouncer", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsLastAnnouncerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));

	Fixture.Binder->SetViewLocationForTest(FVector(50.0, 50.0, 0.0));
	const FGuid A = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* From = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("0,0"));
	UCrowdyServerObject* To = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("1,0"));
	if (!TestNotNull(TEXT("the area it leaves is held"), From) || !TestNotNull(TEXT("and the one it enters"), To))
	{
		return false;
	}

	// While an entity crosses, both areas hold its record until the one it left removes it.
	FFixture::Apply(From, A, 50, 0);
	FFixture::Apply(To, A, 40, 0);
	TestEqual(TEXT("the row reads the area that announced its record last"), Fixture.HealthOf(Row), 40);
	FFixture::Apply(From, A, 55, 0);
	TestEqual(TEXT("whichever area that is"), Fixture.HealthOf(Row), 55);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsReleasedAreaTest,
	"CrowdySDK.Mass.ServerRecordsReleasedAreaFiresRemoval", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsReleasedAreaTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));

	Fixture.Binder->SetViewLocationForTest(FVector(50.0, 50.0, 0.0));
	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();
	const FMassEntityHandle RowA = Fixture.MakeRow(A, Class.Get());
	const FMassEntityHandle RowB = Fixture.MakeRow(B, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Behind = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("-1,0"));
	UCrowdyServerObject* Ahead = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("1,0"));
	if (!TestNotNull(TEXT("the area left behind is held"), Behind) || !TestNotNull(TEXT("and the one ahead"), Ahead))
	{
		return false;
	}
	Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));
	FFixture::Apply(Behind, A, 50, 0);
	FFixture::Apply(Behind, B, 50, 0);
	FFixture::Apply(Ahead, B, 60, 0);
	Fixture.Handler->Calls = 0;

	Fixture.Binder->SetViewLocationForTest(FVector(350.0, 50.0, 0.0));
	Fixture.Binder->RefreshForTest();
	TestNull(TEXT("the view moving away releases the area behind"), Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("-1,0")));
	TestEqual(TEXT("which fires removal once, for the record no held area keeps"), Fixture.Handler->Calls, 1);
	TestTrue(TEXT("for its row"), Fixture.Handler->LastEntity == RowA);
	TestTrue(TEXT("naming nothing"), Fixture.Handler->LastChanged.IsEmpty());
	TestNull(TEXT("and that row reads nothing"), Fixture.Binder->FindServerValues(RowA));
	TestEqual(TEXT("while a record still held ahead reads on"), Fixture.HealthOf(RowB), 60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsNearTierCatchUpTest,
	"CrowdySDK.Mass.ServerRecordsNearTierCatchesUpOnTheNextFlush", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsNearTierCatchUpTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}
	FFixture::Apply(Object, A, 30, 2);
	Fixture.Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));

	const TStrongObjectPtr<UCrowdyMassNearTierComponent> View(NewObject<UCrowdyMassNearTierComponent>(GetTransientPackage()));
	View->OnServerValuesChanged.AddDynamic(Fixture.Handler.Get(), &UCrowdyMassServerRecordsTestHandler::OnNearTierChanged);
	FCrowdyMassServerRecordsTestAccess::Bind(*View, Fixture.Binder.Get(), Row, A);
	TestEqual(TEXT("binding onto a row whose record is long read says nothing inline"), Fixture.Handler->NearTierCalls, 0);

	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("the next flush catches the actor up once"), Fixture.Handler->NearTierCalls, 1);
	TestTrue(TEXT("naming every variable"), NamesAre(Fixture.Handler->NearTierLastChanged, {TEXT("Health"), TEXT("Ammo")}));
	TestEqual(TEXT("without firing the class's handlers again"), Fixture.Handler->Calls, 0);
	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("and only once"), Fixture.Handler->NearTierCalls, 1);
	View->UnbindFromEntity();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsNoListenersTest,
	"CrowdySDK.Mass.ServerRecordsNoNamesAreBuiltWithoutListeners", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsNoListenersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid A = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}

	FFixture::Apply(Object, A, 30, 0);
	FFixture::Apply(Object, A, 20, 0);
	TestEqual(TEXT("with nobody listening no names are built"), Fixture.Binder->NamesBuiltForTest, 0);
	TestEqual(TEXT("while the row still reads its record"), Fixture.HealthOf(Row), 20);

	const FDelegateHandle Handle = Fixture.Binder->AddClassHandler(Class.Get(), FOnCrowdyMassServerValuesChanged::FDelegate::CreateLambda(
		[](const FCrowdyMassEntityTarget&, TConstArrayView<FString>) {}));
	FFixture::Apply(Object, A, 10, 0);
	TestEqual(TEXT("a listener has them built for its row"), Fixture.Binder->NamesBuiltForTest, 1);

	Fixture.Binder->RemoveClassHandler(Handle);
	FFixture::Apply(Object, A, 5, 0);
	TestEqual(TEXT("and once it goes they are not built again"), Fixture.Binder->NamesBuiltForTest, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsRecordedDuringFlushTest,
	"CrowdySDK.Mass.ServerRecordsRowRecordedDuringAFlushIsAnnounced", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsRecordedDuringFlushTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	Fixture.MakeRow(FGuid::NewGuid(), Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}
	const FGuid A = FGuid::NewGuid();
	const FGuid B = FGuid::NewGuid();
	FFixture::Apply(Object, A, 30, 0);
	FFixture::Apply(Object, B, 40, 0);

	// A handler of A's arrival spawns B, whose record is already there.
	int32 Calls = 0;
	FMassEntityHandle RowB;
	Fixture.Binder->AddClassHandler(Class.Get(), FOnCrowdyMassServerValuesChanged::FDelegate::CreateLambda(
		[&Calls, &RowB, &Fixture, &Class, B](const FCrowdyMassEntityTarget&, TConstArrayView<FString>)
		{
			if (++Calls == 1)
			{
				RowB = Fixture.MakeRow(B, Class.Get());
			}
		}));

	Fixture.MakeRow(A, Class.Get());
	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("the flush announces the row that appeared"), Calls, 1);
	Fixture.Binder->FlushForTest();
	TestEqual(TEXT("and the row a handler recorded during it is announced by the next"), Calls, 2);
	TestTrue(TEXT("that row exists"), RowB.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsOwnClassTest,
	"CrowdySDK.Mass.ServerRecordsRowReadsOnlyItsOwnClassesInstances", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsOwnClassTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Single(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));
	const TStrongObjectPtr<UBlueprintGeneratedClass> Areas(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::ThisActorsArea));

	Fixture.Binder->SetViewLocationForTest(FVector(50.0, 50.0, 0.0));
	const FGuid S = FGuid::NewGuid();
	const FMassEntityHandle Row = Fixture.MakeRow(S, Single.Get());
	Fixture.MakeRow(FGuid::NewGuid(), Areas.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Area = Fixture.Binder->FindHeldForTest(Definition.Get(), TEXT("0,0"));
	UCrowdyServerObject* Instance = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the other class's area is held"), Area) || !TestNotNull(TEXT("and the row class's instance"), Instance))
	{
		return false;
	}
	Fixture.Binder->BindObjectHandler(Single.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));

	// The same type, read by another class through its areas, keeps a record under this row's id.
	FFixture::Apply(Area, S, 50, 0);
	TestEqual(TEXT("an object only another class reads does not reach the row's handler"), Fixture.Handler->Calls, 0);
	TestNull(TEXT("nor its reads"), Fixture.Binder->FindServerValues(Row));

	FFixture::Apply(Instance, S, 42, 0);
	TestEqual(TEXT("its own instance does"), Fixture.Handler->Calls, 1);
	TestEqual(TEXT("and is what it reads"), Fixture.HealthOf(Row), 42);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsFailedHeldTest,
	"CrowdySDK.Mass.ServerRecordsSignInAcquiresFailedObjectsAgain", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsFailedHeldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	Fixture.MakeRow(FGuid::NewGuid(), Class.Get());
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}

	Fixture.ServerObjects->SignOutForTest();
	TestTrue(TEXT("signing out fails the held object"), Object->GetStatus() == ECrowdyServerObjectStatus::Failed);
	Fixture.Binder->RefreshForTest();
	TestTrue(TEXT("the follow timer alone does not retry it"), Object->GetStatus() == ECrowdyServerObjectStatus::Failed);
	Fixture.Binder->SignInForTest(false);
	TestTrue(TEXT("nor does a sign-in that failed"), Object->GetStatus() == ECrowdyServerObjectStatus::Failed);

	Fixture.Binder->SignInForTest(true);
	TestFalse(TEXT("a sign-in acquires it again"), Object->GetStatus() == ECrowdyServerObjectStatus::Failed);
	TestTrue(TEXT("the same object stays held"), Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance) == Object);
	TestTrue(TEXT("and heard"), Object->OnRecordsChanged.IsBound());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsRowRecordsTest,
	"CrowdySDK.Mass.ServerRecordsOnlyRowsOfRecordClassesKeepARecordId", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsRowRecordsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> OneSet = Fixture.MakeDefinition(ECrowdyServerObjectHolds::OneSet);
	const TStrongObjectPtr<UBlueprintGeneratedClass> RecordClass(MakeBlueprintClass(Records.Get(), ECrowdyServerObjectInstanceMode::InstanceId));
	const TStrongObjectPtr<UBlueprintGeneratedClass> OneSetClass(MakeBlueprintClass(OneSet.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	Fixture.MakeRow(FGuid::NewGuid(), AActor::StaticClass());
	Fixture.MakeRow(FGuid::NewGuid(), OneSetClass.Get());
	TestEqual(TEXT("rows of classes with no records component keep no record id"), Fixture.Binder->NumRowRecordsForTest(), 0);

	const FGuid A = FGuid::NewGuid();
	Fixture.MakeRow(A, RecordClass.Get());
	TestEqual(TEXT("a row of a class with one keeps its own"), Fixture.Binder->NumRowRecordsForTest(), 1);

	Fixture.Rows->OnRowRemoved.Broadcast(A);
	TestEqual(TEXT("and gives it up when the row goes"), Fixture.Binder->NumRowRecordsForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsClassGetsValuesTest,
	"CrowdySDK.Mass.ServerRecordsClassGetsServerValuesOnlyWithARecordsComponent", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsClassGetsValuesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> OneSet = Fixture.MakeDefinition(ECrowdyServerObjectHolds::OneSet);
	const TStrongObjectPtr<UBlueprintGeneratedClass> RecordClass(MakeBlueprintClass(Records.Get(), ECrowdyServerObjectInstanceMode::InstanceId));
	const TStrongObjectPtr<UBlueprintGeneratedClass> OneSetClass(MakeBlueprintClass(OneSet.Get(), ECrowdyServerObjectInstanceMode::InstanceId));
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnsupportedClass(MakeBlueprintClass(Records.Get(), ECrowdyServerObjectInstanceMode::SignedInPlayer));

	TestTrue(TEXT("a class with a records component gets server values"), UCrowdyMassServerValuesLibrary::ClassGetsServerValues(RecordClass.Get()));
	TestFalse(TEXT("a class with no such component does not"), UCrowdyMassServerValuesLibrary::ClassGetsServerValues(AActor::StaticClass()));
	TestFalse(TEXT("nor one whose component holds something other than records"), UCrowdyMassServerValuesLibrary::ClassGetsServerValues(OneSetClass.Get()));
	TestFalse(TEXT("nor one reading in a mode rows cannot follow"), UCrowdyMassServerValuesLibrary::ClassGetsServerValues(UnsupportedClass.Get()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassServerRecordsHandlerBatchTest,
	"CrowdySDK.Mass.ServerRecordsHandlersAreWorkedOutOncePerBatchAndClass", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassServerRecordsHandlerBatchTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassServerRecordsTests;
	FFixture Fixture(*this);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = Fixture.MakeDefinition(ECrowdyServerObjectHolds::RecordPerEntity);
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(MakeBlueprintClass(Definition.Get(), ECrowdyServerObjectInstanceMode::InstanceId));

	const FGuid Ids[] = {FGuid::NewGuid(), FGuid::NewGuid(), FGuid::NewGuid()};
	for (const FGuid& Id : Ids)
	{
		Fixture.MakeRow(Id, Class.Get());
	}
	Fixture.Binder->FlushForTest();
	UCrowdyServerObject* Object = Fixture.Binder->FindHeldForTest(Definition.Get(), CrowdInstance);
	if (!TestNotNull(TEXT("the class's one instance is held"), Object))
	{
		return false;
	}

	int32 Calls = 0;
	int32 LaterCalls = 0;
	UCrowdyMassServerRecordsSubsystem* Binder = Fixture.Binder.Get();
	Binder->BindObjectHandler(Class.Get(), Fixture.Handler.Get(), TEXT("OnChanged"));
	Binder->AddClassHandler(AActor::StaticClass(), FOnCrowdyMassServerValuesChanged::FDelegate::CreateLambda(
		[&Calls](const FCrowdyMassEntityTarget&, TConstArrayView<FString>) { ++Calls; }));
	Binder->HandlerListsBuiltForTest = 0;

	auto MakeChanged = [&Ids](int32 Health)
	{
		FCrowdyMassServerRecordsTestRecord Record;
		Record.Health = Health;
		TArray<TPair<FString, FInstancedStruct>> Changed;
		for (const FGuid& Id : Ids)
		{
			Changed.Emplace(RecordIdOf(Id), FInstancedStruct::Make(Record));
		}
		return Changed;
	};
	Object->ApplyRecordsForTest(MakeChanged(90), TArray<FString>());
	TestEqual(TEXT("each record of the batch is announced"), Fixture.Handler->Calls, 3);
	TestEqual(TEXT("to both the class's handler and its base class's"), Calls, 3);
	TestEqual(TEXT("with the class's handlers worked out once for the batch"), Binder->HandlerListsBuiltForTest, 1);

	// A handler added while the batch runs is heard by the records after the one that added it.
	Calls = 0;
	bool bAdded = false;
	Binder->AddClassHandler(Class.Get(), FOnCrowdyMassServerValuesChanged::FDelegate::CreateLambda(
		[Binder, &bAdded, &LaterCalls](const FCrowdyMassEntityTarget&, TConstArrayView<FString>)
		{
			if (bAdded)
			{
				return;
			}
			bAdded = true;
			Binder->AddClassHandler(AActor::StaticClass(), FOnCrowdyMassServerValuesChanged::FDelegate::CreateLambda(
				[&LaterCalls](const FCrowdyMassEntityTarget&, TConstArrayView<FString>) { ++LaterCalls; }));
		}));
	Object->ApplyRecordsForTest(MakeChanged(80), TArray<FString>());
	TestEqual(TEXT("the handler added during the batch hears the records that follow"), LaterCalls, 2);
	return true;
}

// The typed C++ read takes a value only into exactly its own type, so a mismatch is refused rather than reinterpreted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTypedServerValueMatchesTheExactTypeTest, "CrowdySDK.Mass.ServerRecords.TypedReadMatchesTheExactType", CrowdyMassServerRecordsTests::TestFlags)
bool FCrowdyMassTypedServerValueMatchesTheExactTypeTest::RunTest(const FString& Parameters)
{
	using Library = UCrowdyMassServerValuesLibrary;

	const FProperty* Health = FCrowdyMassServerRecordsTestRecord::StaticStruct()->FindPropertyByName(TEXT("Health"));
	const FProperty* Translation = TBaseStructure<FTransform>::Get()->FindPropertyByName(TEXT("Translation"));
	const FProperty* Mode = UCrowdyServerObjectComponent::StaticClass()->FindPropertyByName(TEXT("InstanceMode"));
	if (!TestNotNull(TEXT("an int32 field"), Health) || !TestNotNull(TEXT("an FVector field"), Translation) || !TestNotNull(TEXT("an enum field"), Mode))
	{
		return false;
	}

	TestTrue(TEXT("an int32 holds an int32"), Library::PropertyHolds<int32>(*Health));
	TestFalse(TEXT("but not an int64"), Library::PropertyHolds<int64>(*Health));
	TestFalse(TEXT("nor a float"), Library::PropertyHolds<float>(*Health));
	const FProperty* X = TBaseStructure<FVector>::Get()->FindPropertyByName(TEXT("X"));
	if (TestNotNull(TEXT("a double field"), X))
	{
		TestTrue(TEXT("a double holds a double"), Library::PropertyHolds<double>(*X));
		TestFalse(TEXT("but not an int32"), Library::PropertyHolds<int32>(*X));
	}
	TestTrue(TEXT("a vector holds an FVector"), Library::PropertyHolds<FVector>(*Translation));
	TestFalse(TEXT("but not an FRotator"), Library::PropertyHolds<FRotator>(*Translation));
	TestTrue(TEXT("an enum holds its own enum"), Library::PropertyHolds<ECrowdyServerObjectInstanceMode>(*Mode));
	TestFalse(TEXT("but not a bare byte"), Library::PropertyHolds<uint8>(*Mode));

	int32 Out = 7;
	TestFalse(TEXT("a row with no world reads nothing"), Library::GetServerValueAs(FCrowdyMassEntityTarget(), TEXT("Health"), Out));
	TestEqual(TEXT("and the destination is reset"), Out, 0);
	return true;
}

#endif
