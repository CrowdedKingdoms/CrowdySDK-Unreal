#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

// Pins the network path's current behaviour; helpers carry a GmBaseline prefix so a unity build cannot merge them.
namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelBaselineTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	UCrowdyGameModelSubsystem* MakeGmBaselineModel()
	{
		return NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	}

	UCrowdyGameModelTestTarget* MakeGmBaselineTarget()
	{
		return NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	}

	TSharedPtr<FJsonObject> GmBaselineOneNumber(const FString& Key, double Value)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(Key, Value);
		return Object;
	}

	TSharedPtr<FJsonObject> GmBaselineTwoNumbers(const FString& KeyA, double ValueA, const FString& KeyB, double ValueB)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(KeyA, ValueA);
		Object->SetNumberField(KeyB, ValueB);
		return Object;
	}

	TArray<FCrowdyMutationApplied> GmBaselineOneMutation(const FString& Key, const FString& NewValueJson)
	{
		TArray<FCrowdyMutationApplied> Mutations;
		FCrowdyMutationApplied& Mutation = Mutations.AddDefaulted_GetRef();
		Mutation.Key = Key;
		Mutation.NewValueJson = NewValueJson;
		return Mutations;
	}
}

// Only a changed, declared key fires its OnRep and one attribute-changed broadcast.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineApplyStateTest,
	"CrowdySDK.GameModel.Baseline.ApplyStateToContainer", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineApplyStateTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeGmBaselineModel();
	UCrowdyGameModelTestTarget* Target = MakeGmBaselineTarget();
	UCrowdyGameModelTestTarget* Observer = MakeGmBaselineTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target)
		|| !TestNotNull(TEXT("observer created"), Observer))
	{
		return false;
	}

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("baseline-apply"));

	Model->ApplyStateToContainer(NetID, Target, GmBaselineOneNumber(TEXT("hp"), 87));
	TestEqual(TEXT("hp OnRep fires once on first apply"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("one attribute-changed broadcast on first apply"), Observer->AttributeChangedCount, 1);

	Model->ApplyStateToContainer(NetID, Target, GmBaselineOneNumber(TEXT("hp"), 87));
	TestEqual(TEXT("hp OnRep does not re-fire on an unchanged re-apply"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("no further broadcast on an unchanged re-apply"), Observer->AttributeChangedCount, 1);

	Model->ApplyStateToContainer(NetID, Target, GmBaselineTwoNumbers(TEXT("hp"), 87, TEXT("mana"), 60));
	TestEqual(TEXT("only the changed key's OnRep fires"), Target->ManaOnRepCount, 1);
	TestEqual(TEXT("the unchanged key's OnRep stays put"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("exactly one further broadcast for the one changed key"), Observer->AttributeChangedCount, 2);

	Model->ApplyStateToContainer(NetID, Target, GmBaselineOneNumber(TEXT("notanattribute"), 999));
	TestEqual(TEXT("a key the container never declared advances no OnRep"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("a key the container never declared fires no broadcast either"), Observer->AttributeChangedCount, 2);

	return true;
}

// A confirmed value equal to the cached one fires no OnRep and no broadcast.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineApplyMutationsEqualValueTest,
	"CrowdySDK.GameModel.Baseline.ApplyMutationsEqualValueIsSilent", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineApplyMutationsEqualValueTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeGmBaselineModel();
	UCrowdyGameModelTestTarget* Target = MakeGmBaselineTarget();
	UCrowdyGameModelTestTarget* Observer = MakeGmBaselineTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target)
		|| !TestNotNull(TEXT("observer created"), Observer))
	{
		return false;
	}

	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("baseline-mutation"));

	// Seed the cache first; this apply is setup, not the case under test.
	Model->ApplyMutationsToContainer(NetID, Target, GmBaselineOneMutation(TEXT("hp"), TEXT("87")));
	TestEqual(TEXT("seed apply fired OnRep once"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("seed apply broadcast once"), Observer->AttributeChangedCount, 1);

	// The case under test: a confirmed value equal to the cached one.
	Model->ApplyMutationsToContainer(NetID, Target, GmBaselineOneMutation(TEXT("hp"), TEXT("87")));
	TestEqual(TEXT("an equal-to-cached mutation fires no further OnRep"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("an equal-to-cached mutation fires no further broadcast"), Observer->AttributeChangedCount, 1);

	return true;
}

// K marks for one container drop K echoes, and the next hint pulls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineSelfEchoOnePerMarkTest,
	"CrowdySDK.GameModel.Baseline.SelfEchoOnePerMark", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineSelfEchoOnePerMarkTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeGmBaselineModel();
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	const FString ContainerId = TEXT("baseline-echo");
	constexpr int32 Marks = 4;
	for (int32 Index = 0; Index < Marks; ++Index)
	{
		Model->MarkSelfActed(ContainerId);
	}

	for (int32 Index = 0; Index < Marks; ++Index)
	{
		TestTrue(FString::Printf(TEXT("echo %d of %d is dropped"), Index + 1, Marks), Model->ConsumeSelfEcho(ContainerId));
	}
	TestFalse(TEXT("the hint past the marks pulls"), Model->ConsumeSelfEcho(ContainerId));
	TestEqual(TEXT("every drop is counted"), Model->GetNetStats().SelfEchoesDropped, Marks);
	return true;
}

// Only a thrown, Budget-blamed RATE_LIMITED result is a budget refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineBudgetRefusalTest,
	"CrowdySDK.GameModel.Baseline.IsBudgetRefusal", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineBudgetRefusalTest::RunTest(const FString& Parameters)
{
	FCrowdyInvokeResult Thrown;
	Thrown.bTransportOk = false;
	Thrown.Blame = ECrowdyPlayerFaultBlame::Budget;
	Thrown.FaultCode = TEXT("RATE_LIMITED");
	TestTrue(TEXT("thrown Budget/RATE_LIMITED is a refusal"), UCrowdyGameModelSubsystem::IsBudgetRefusal(Thrown));

	FCrowdyInvokeResult InBand = Thrown;
	InBand.bTransportOk = true;
	TestFalse(TEXT("the same attribution returned in band is not a refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(InBand));

	FCrowdyInvokeResult PlatformBusy;
	PlatformBusy.bTransportOk = false;
	PlatformBusy.Blame = ECrowdyPlayerFaultBlame::Platform;
	PlatformBusy.bRetryable = true;
	PlatformBusy.FaultCode = TEXT("PLATFORM_BUSY");
	TestFalse(TEXT("a thrown, retryable Platform fault is not a budget refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(PlatformBusy));

	return true;
}

// Unchanged below half the allowance, stretched linearly above it, capped at 2.0s.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineStretchCoalesceWindowTest,
	"CrowdySDK.GameModel.Baseline.StretchCoalesceWindowSeconds", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineStretchCoalesceWindowTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("below half the allowance spent, the authored window is unchanged"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.5f, 40, 120), 0.5f);

	TestEqual(TEXT("three-quarters of the allowance spent stretches the window by the expected exact factor"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.25f, 90, 120), 1.125f);

	TestEqual(TEXT("a fully spent allowance clamps the stretched window to the 2.0s ceiling"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.5f, 120, 120), 2.0f);

	return true;
}

// A host-owned entity is bulk-resolve eligible; a per-player one is not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineBulkResolveEligibilityTest,
	"CrowdySDK.GameModel.Baseline.BulkResolveEligibility", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineBulkResolveEligibilityTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeGmBaselineModel();
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* PerPlayer = MakeGmBaselineTarget();
	UCrowdyGameModelTestTarget* HostOwned = MakeGmBaselineTarget();
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("entities created"), Entities)
		|| !TestNotNull(TEXT("per-player target created"), PerPlayer) || !TestNotNull(TEXT("host target created"), HostOwned))
	{
		return false;
	}

	Entities->SetLocalPlayerID(FGuid::NewGuid());
	Model->SetEntitySubsystemForTest(Entities);

	const FGuid PerPlayerNetID = Entities->RegisterParticipant(PerPlayer, ECrowdyOwnership::LocalClient);
	const FGuid HostNetID = Entities->RegisterParticipant(HostOwned, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("per-player participant registered"), PerPlayerNetID.IsValid())
		|| !TestTrue(TEXT("host-owned participant registered"), HostNetID.IsValid()))
	{
		return false;
	}

	TestFalse(TEXT("a per-player entity is not bulk-resolve eligible"), Model->IsBulkResolveEligibleForTest(PerPlayerNetID));
	TestTrue(TEXT("a host-owned entity is bulk-resolve eligible"), Model->IsBulkResolveEligibleForTest(HostNetID));

	return true;
}

// Unregistering an entity removes its container binding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBaselineUnregisterClearsBindingTest,
	"CrowdySDK.GameModel.Baseline.EntityUnregisteredClearsContainerBinding", CrowdyGameModelBaselineTestFlags)
bool FCrowdyGameModelBaselineUnregisterClearsBindingTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = MakeGmBaselineModel();
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("baseline-unregister"));

	FString Bound;
	TestTrue(TEXT("the binding resolves before unregistration"), Model->TryGetContainerId(NetID, Bound));

	Model->HandleEntityUnregisteredForTest(NetID);

	TestFalse(TEXT("the binding is gone after entity-unregistered runs"), Model->TryGetContainerId(NetID, Bound));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
