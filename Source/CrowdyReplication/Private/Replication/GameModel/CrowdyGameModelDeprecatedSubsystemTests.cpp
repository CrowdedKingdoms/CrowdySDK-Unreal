#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

// WITH_METADATA as well: the enrolment cases read the CrowdyContainer tag of test fixtures, which a cooked game's
// baked registry leaves out, so there they would refuse for the wrong reason.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "CrowdyCppClient.h"
#include "Core/FCrowdyTypeID.h"
#include "Data/CrowdyContainerManifest.h"
#include "Data/CrowdyEntityTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyEntityClassContainerTestTypes.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystems/SubsystemCollection.h"
#include "TimerManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/UCrowdyClassRegistry.h"

// Game Models are deprecated: every subsystem entry point refuses at once, exactly once, with the bridge's code and
// message, and nothing enrolls, waits or retries. Each case names the guard it fails without.
namespace
{
	constexpr EAutomationTestFlags CrowdyDeprecatedTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The refusal warns once per process, so whichever case refuses first sees it; none of them is about the warning.
	void AllowDeprecationWarning(FAutomationTestBase& Test)
	{
		Test.AddExpectedMessagePlain(CrowdyCppGameModelDeprecatedMessage, ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, -1);
	}

	// FAutomationTestBase::TestEqual compares FStrings ignoring case; the code is matched exactly.
	void TestSameDeprecatedText(FAutomationTestBase& Test, const TCHAR* What, const FString& Actual,
		const TCHAR* Expected)
	{
		Test.TestTrue(FString::Printf(TEXT("%s (got '%s')"), What, *Actual),
			Actual.Equals(Expected, ESearchCase::CaseSensitive));
	}

	void TestDeprecatedInvokeResult(FAutomationTestBase& Test, const FCrowdyInvokeResult& Result)
	{
		Test.TestFalse(TEXT("nothing reached a server"), Result.bTransportOk);
		Test.TestFalse(TEXT("the call did not succeed"), Result.bSuccess);
		Test.TestFalse(TEXT("and is never worth repeating"), Result.bRetryable);
		TestSameDeprecatedText(Test, TEXT("the fault code is the deprecation code"), Result.FaultCode,
			CrowdyCppGameModelDeprecatedCode);
		TestSameDeprecatedText(Test, TEXT("the message is the deprecation message"), Result.ErrorMessage,
			CrowdyCppGameModelDeprecatedMessage);
	}

	void TestDeprecatedLastError(FAutomationTestBase& Test, const UCrowdyGameModelSubsystem* Model)
	{
		TestSameDeprecatedText(Test, TEXT("the last error code is the deprecation code"),
			Model->GetLastModelErrorCode(), CrowdyCppGameModelDeprecatedCode);
		TestSameDeprecatedText(Test, TEXT("the last failure carries the same code"), Model->GetLastFailure().Code,
			CrowdyCppGameModelDeprecatedCode);
		TestSameDeprecatedText(Test, TEXT("and the deprecation message"), Model->GetLastModelError(),
			CrowdyCppGameModelDeprecatedMessage);
	}

	// An editor world, so a timer a refusal armed would really be armed and then fire on a tick.
	struct FCrowdyDeprecatedTestWorld
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;

		FCrowdyDeprecatedTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			Context = &GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context->SetCurrentWorld(World);
		}

		~FCrowdyDeprecatedTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	// The API context seam is set so it cannot be what lets a call through.
	UCrowdyGameModelSubsystem* MakeDeprecatedTestModel(UObject* Outer)
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Outer);
		Model->SetApiContextForTest(TEXT("https://gamemodel.example.invalid/graphql"), TEXT("test-token"), 42);
		return Model;
	}

	TSharedPtr<FJsonObject> MakeDeprecatedTestParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("amount"), 5);
		return Params;
	}

	// The first tick activates a pending timer and the second, a frame later, fires it.
	void TickDeprecatedTestTimers(UWorld* World)
	{
		World->GetTimerManager().Tick(30.0f);
		++GFrameCounter;
		World->GetTimerManager().Tick(30.0f);
	}

	// Makes a class resolvable from an entity record's ClassID; false once the registry is sealed in this process.
	bool RecordDeprecatedTestClass(const UClass* Class, uint32& OutClassID)
	{
		UCrowdyClassRegistry* Registry = UCrowdyClassRegistry::Get();
		const FCrowdyClassID ClassID = Registry->GetID(Class);
		Registry->RegisterClass(ClassID, FSoftClassPath(Class));
		OutClassID = ClassID;
		return Registry->Resolve(ClassID).ResolveClass() == Class;
	}
}

// Without the refusal at the top of InvokeAndApplyResolved, an entity with no container answers "no container bound".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedInvokeAndApplyTest,
	"CrowdySDK.GameModel.DeprecatedInvokeAndApply", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedInvokeAndApplyTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	FCrowdyDeprecatedTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(Env.World);

	int32 Notified = 0;
	FCrowdyInvokeResult Result;
	Model->InvokeAndApply(FGuid::NewGuid(), TEXT("ApplyDamage"), MakeDeprecatedTestParams(), FString(),
		[&Notified, &Result](FCrowdyInvokeResult InResult) { ++Notified; Result = MoveTemp(InResult); });

	TestEqual(TEXT("answered once, before the call returned"), Notified, 1);
	TestDeprecatedInvokeResult(*this, Result);
	TestDeprecatedLastError(*this, Model);

	TickDeprecatedTestTimers(Env.World);
	TestEqual(TEXT("and never again: nothing was left to retry"), Notified, 1);
	return true;
}

// Without the refusal at the top of InvokeOnContainerResolved, an empty id answers "empty container id". A real id
// is refused the same way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedInvokeOnContainerTest,
	"CrowdySDK.GameModel.DeprecatedInvokeOnContainer", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedInvokeOnContainerTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	FCrowdyDeprecatedTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(Env.World);

	for (const TCHAR* ContainerId : { TEXT(""), TEXT("cid-deprecated") })
	{
		int32 Notified = 0;
		FCrowdyInvokeResult Result;
		Model->InvokeOnContainer(ContainerId, TEXT("ApplyDamage"), MakeDeprecatedTestParams(), FString(),
			[&Notified, &Result](FCrowdyInvokeResult InResult) { ++Notified; Result = MoveTemp(InResult); });

		AddInfo(FString::Printf(TEXT("container id '%s'"), ContainerId));
		TestEqual(TEXT("answered once, before the call returned"), Notified, 1);
		TestDeprecatedInvokeResult(*this, Result);

		TickDeprecatedTestTimers(Env.World);
		TestEqual(TEXT("and never again: nothing was left to retry"), Notified, 1);
	}
	TestDeprecatedLastError(*this, Model);
	return true;
}

// Without the refusal at the top of DispatchInvokeAttempt, the raw invoke answers "API context unavailable" with no code.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedInvokeTest,
	"CrowdySDK.GameModel.DeprecatedInvoke", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedInvokeTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	FCrowdyDeprecatedTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(Env.World);

	FCrowdyInvokeRequest Request;
	Request.FunctionName = TEXT("ApplyDamage");
	Request.SelfContainerId = TEXT("cid-deprecated");
	Request.Params = MakeDeprecatedTestParams();

	int32 Notified = 0;
	FCrowdyInvokeResult Result;
	Model->Invoke(Request, [&Notified, &Result](FCrowdyInvokeResult InResult) { ++Notified; Result = MoveTemp(InResult); });

	TestEqual(TEXT("answered once, before the call returned"), Notified, 1);
	TestDeprecatedInvokeResult(*this, Result);
	TestEqual(TEXT("nothing was counted against the invoke allowance"), Model->GetRecentInvokeCount(), 0);

	TickDeprecatedTestTimers(Env.World);
	TestEqual(TEXT("and never again: nothing was left to retry"), Notified, 1);
	return true;
}

// Without IsCoalescingAvailable answering false, a mergeable apply waits in a window instead of being refused now.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedEffectApplyNotHeldTest,
	"CrowdySDK.GameModel.DeprecatedEffectApplyNotHeld", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedEffectApplyNotHeldTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	FCrowdyDeprecatedTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(Env.World);

	TestFalse(TEXT("a live subsystem in a world offers no merge window"), Model->IsCoalescingAvailable());

	FCrowdyCoalesceRequest Request;
	Request.ContainerId = TEXT("cid-deprecated");
	Request.FunctionName = TEXT("ApplyDamage");
	Request.Params = MakeDeprecatedTestParams();
	Request.AccumulateParam = TEXT("amount");
	Request.bAccumulateIsInteger = true;
	Request.WindowSeconds = 1.0f;
	Request.MergeDiscriminator = 0x5151515151515151ULL;

	int32 Notified = 0;
	FCrowdyInvokeResult Result;
	Model->EnqueueCoalescedInvoke(Request,
		[&Notified, &Result](FCrowdyInvokeResult InResult) { ++Notified; Result = MoveTemp(InResult); });

	TestEqual(TEXT("no window was opened"), Model->GetOpenCoalesceWindowCountForTest(), 0);
	TestEqual(TEXT("the apply was answered before the call returned"), Notified, 1);
	TestDeprecatedInvokeResult(*this, Result);

	TickDeprecatedTestTimers(Env.World);
	TestEqual(TEXT("and never again"), Notified, 1);
	return true;
}

// Without ResolveApiContext recording the refusal, a session or container op leaves the older error in place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedSessionAndContainerOpsTest,
	"CrowdySDK.GameModel.DeprecatedSessionAndContainerOps", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedSessionAndContainerOpsTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(GetTransientPackage());

	Model->SetLastModelError(TEXT("an older failure"));
	int32 ListNotified = 0;
	bool bListOk = true;
	Model->ListSessions(TEXT("active"),
		[&ListNotified, &bListOk](bool bOk, const TArray<FCrowdyGameModelSession>&) { ++ListNotified; bListOk = bOk; });
	TestEqual(TEXT("the session list answered once, before the call returned"), ListNotified, 1);
	TestFalse(TEXT("and failed"), bListOk);
	TestDeprecatedLastError(*this, Model);

	Model->SetLastModelError(TEXT("an older failure"));
	int32 CreateNotified = 0;
	bool bCreateOk = true;
	FString CreatedId = TEXT("unset");
	Model->CreateDataContainer(TEXT("Inventory"), TEXT("Bag"), FString(), FString(),
		[&CreateNotified, &bCreateOk, &CreatedId](bool bOk, const FString& ContainerId)
		{
			++CreateNotified;
			bCreateOk = bOk;
			CreatedId = ContainerId;
		});
	TestEqual(TEXT("the container create answered once, before the call returned"), CreateNotified, 1);
	TestFalse(TEXT("and failed"), bCreateOk);
	TestTrue(TEXT("with no container id"), CreatedId.IsEmpty());
	TestDeprecatedLastError(*this, Model);
	return true;
}

// Without the refusal at the top of LeaveSession, a leave with no remembered incarnation reports that instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedLeaveSessionTest,
	"CrowdySDK.GameModel.DeprecatedLeaveSession", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedLeaveSessionTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(GetTransientPackage());

	Model->SetLastModelError(TEXT("an older failure"));
	int32 Notified = 0;
	bool bLeaveOk = true;
	Model->LeaveSession(TEXT("session-deprecated"), 0,
		[&Notified, &bLeaveOk](bool bOk, const FCrowdyGameModelSessionParticipant&) { ++Notified; bLeaveOk = bOk; });

	TestEqual(TEXT("answered once, before the call returned"), Notified, 1);
	TestFalse(TEXT("and failed"), bLeaveOk);
	TestDeprecatedLastError(*this, Model);
	return true;
}

// Without the refusal in ApplyContainerManifest, the apply fails as a retryable "no_token" instead of a final refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedManifestApplyTest,
	"CrowdySDK.GameModel.DeprecatedManifestApply", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedManifestApplyTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(GetTransientPackage());

	UCrowdyContainerManifest* Manifest = NewObject<UCrowdyContainerManifest>(GetTransientPackage());
	FCrowdyContainerManifestRow& Row = Manifest->Rows.AddDefaulted_GetRef();
	Row.TypeName = TEXT("Camp");
	Row.BindingKey = TEXT("camp-1");

	int32 Notified = 0;
	FCrowdyApplyManifestResult Result;
	Model->ApplyContainerManifest(Manifest, FString(), false,
		[&Notified, &Result](const FCrowdyApplyManifestResult& InResult) { ++Notified; Result = InResult; });

	TestEqual(TEXT("answered once, before the call returned"), Notified, 1);
	TestEqual(TEXT("one failure"), Result.Failed, 1);
	if (!TestEqual(TEXT("one failure entry"), Result.Failures.Num(), 1))
	{
		return false;
	}
	TestSameDeprecatedText(*this, TEXT("the failure code is the deprecation code"), Result.Failures[0].Code,
		CrowdyCppGameModelDeprecatedCode);
	TestSameDeprecatedText(*this, TEXT("the failure message is the deprecation message"), Result.Failures[0].Message,
		CrowdyCppGameModelDeprecatedMessage);
	TestFalse(TEXT("and it is final, not retryable"), Result.Failures[0].bRetryable);
	return true;
}

// Without the refusal at the top of EnrollModelComponent, a keyed component added to a registered actor enrolls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedEnrollModelComponentTest,
	"CrowdySDK.GameModel.DeprecatedEnrollModelComponent", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedEnrollModelComponentTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(GetTransientPackage());
	Model->SetEntitySubsystemForTest(Entities);

	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the anchor registered"), AnchorNetID.IsValid()))
	{
		return false;
	}

	UCrowdyGameModelTestKeyedComponent* Late = NewObject<UCrowdyGameModelTestKeyedComponent>(Anchor);
	Late->BindingKey = TEXT("late_added");
	Anchor->AddOwnedComponent(Late);
	Model->EnrollModelComponent(Late);

	FGuid SubID;
	TestFalse(TEXT("the container component did not enroll"), Model->ResolveTargetNetID(Late, SubID));
	TArray<FGuid> Subs;
	Model->GetSubParticipants(AnchorNetID, Subs);
	TestEqual(TEXT("nothing is tracked under the anchor"), Subs.Num(), 0);
	TestEqual(TEXT("and nothing waits to bind"), Model->GetPendingModelEntityCountForTest(), 0);
	return true;
}

// Without the refusal in EnrollDerivedComponentContainers, a row whose class declares a container component gets a
// stand-in enrolled for it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedDerivedContainersTest,
	"CrowdySDK.GameModel.DeprecatedDerivedContainers", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedDerivedContainersTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	uint32 ComponentClassID = 0;
	uint32 ActorClassID = 0;
	if (!RecordDeprecatedTestClass(UCrowdyGameModelTestComponent::StaticClass(), ComponentClassID)
		|| !RecordDeprecatedTestClass(ACrowdyEntityClassComponentOwnerActor::StaticClass(), ActorClassID))
	{
		AddInfo(TEXT("the class registry is sealed in this process, so the record's class cannot resolve; skipping."));
		return true;
	}

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(GetTransientPackage());
	Model->SetEntitySubsystemForTest(Entities);

	const FGuid AnchorNetID(0xDE9, 1, 2, 3);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	FCrowdyEntityRecord Record;
	Record.NetID = AnchorNetID;
	Record.OwnerID = FGuid(0x0BADC0DE, 4, 5, 6);
	Record.Role = ECrowdyRole::RemoteProxy;
	Record.ClassID = ActorClassID;
	Record.Participant = RowStandIn;
	Entities->RegisterEntity(Record);

	bool bShouldRetry = true;
	const int32 Enrolled = Model->EnrollDerivedComponentContainers(AnchorNetID, RowStandIn, &bShouldRetry);

	TestEqual(TEXT("no component container was stood in for"), Enrolled, 0);
	TestFalse(TEXT("and the caller is not told to ask again"), bShouldRetry);
	TArray<FGuid> Subs;
	Model->GetSubParticipants(AnchorNetID, Subs);
	TestEqual(TEXT("nothing is tracked under the anchor"), Subs.Num(), 0);
	TestEqual(TEXT("no stand-in is held"), Model->GetHeldContainerStandInCountForTest(), 0);
	return true;
}

// Initialize in a world with a game instance subscribes to nothing and keeps the active session; Deinitialize is safe
// with nothing bound. Without removing the entity-subsystem dependency, Initialize trips an ensure here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedInitializeTest,
	"CrowdySDK.GameModel.DeprecatedInitialize", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedInitializeTest::RunTest(const FString& Parameters)
{
	AllowDeprecationWarning(*this);
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	FCrowdyDeprecatedTestWorld Env;
	Env.World->SetGameInstance(Instance.Get());
	Env.Context->OwningGameInstance = Instance.Get();

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	FObjectSubsystemCollection<UWorldSubsystem> Collection;
	Model->Initialize(Collection);

	Model->SetActiveSession(TEXT("session-deprecated"));
	TestEqual(TEXT("the active session is still kept"), Model->GetActiveSession(), FString(TEXT("session-deprecated")));
	TestEqual(TEXT("nothing waits to bind"), Model->GetPendingModelEntityCountForTest(), 0);

	Model->Deinitialize();
	TestTrue(TEXT("teardown clears the active session"), Model->GetActiveSession().IsEmpty());
	return true;
}

// The refusal warns once per process, however often it is hit. Fails if the once-guard is removed (two warnings) or
// the warning is (none).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeprecatedWarnsOnceTest,
	"CrowdySDK.GameModel.DeprecatedWarnsOnce", CrowdyDeprecatedTestFlags)
bool FCrowdyGameModelDeprecatedWarnsOnceTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem::ResetDeprecationWarningForTest();
	AddExpectedMessagePlain(CrowdyCppGameModelDeprecatedMessage, ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	UCrowdyGameModelSubsystem* Model = MakeDeprecatedTestModel(GetTransientPackage());
	int32 Notified = 0;
	for (int32 Call = 0; Call < 2; ++Call)
	{
		Model->InvokeOnContainer(TEXT("cid-deprecated"), TEXT("ApplyDamage"), MakeDeprecatedTestParams(), FString(),
			[&Notified](FCrowdyInvokeResult) { ++Notified; });
	}
	TestEqual(TEXT("both calls were refused"), Notified, 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
