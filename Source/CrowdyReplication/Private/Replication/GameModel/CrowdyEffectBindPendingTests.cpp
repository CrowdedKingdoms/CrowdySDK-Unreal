#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

// The bind-pending case this phase owes: an entity-bound apply targeting an entity whose Game Model container
// has not bound yet (no ResolveOrCreateContainer has completed for it). InvokeAndApply is the one seam every
// entity-bound apply funnels through - UCrowdyEffects::Apply for an authored effect resolves the target to its
// NetID and calls exactly this - so it is where a bind-pending refusal actually happens. It must refuse
// cleanly through the ordinary OnDone completion, exactly once, with a specific reason, never silently drop the
// caller and never leave it waiting on a bind nothing will ever tell it about. Once the container binds, the
// identical call reaches past that one refusal (a different, transport-layer failure downstream proves the
// bind-pending gate specifically was cleared, not that the target had become unreachable some other way).
//
// UCrowdyEffects::ApplyInternal itself (the Blueprint-facing entry point) is NOT exercised here: it resolves
// its OWN UCrowdyGameModelSubsystem via Target->GetWorld()->GetSubsystem<>() with no test-injectable seam, and
// UCrowdyGameModelSubsystem::ShouldCreateSubsystem restricts auto-creation to PIE/Game worlds, so a headless
// Editor-type test world can never produce the instance it looks for. Reaching ApplyInternal's own code
// (rather than the InvokeAndApply seam it eventually calls) needs a live PIE gate, not a headless test.
namespace
{
	constexpr EAutomationTestFlags CrowdyBindPendingTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Named distinctly from the completion-liveness suite's own helper of the same purpose so two anonymous-
	// namespace definitions never collide when adaptive unity merges this module's translation units.
	void AllowMissingApiContextForBindPendingTests(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("Game API endpoint is empty|No UCrowdyGameSession|No app-scoped game token"),
			EAutomationExpectedErrorFlags::Contains, 0);
	}
}

// Mutating away InvokeAndApply's TryGetContainerId guard would let a bind-pending apply fall through to
// DispatchInvokeAttempt with an empty SelfContainerId, sending a malformed invoke instead of refusing
// client-side; mutating the completion to simply `return` without calling OnDone would strand the caller,
// which is exactly the "silently vanishes" failure this test exists to catch (asserting the exact count of
// notifications, not merely that "an" outcome eventually arrived, is what would turn that red).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelInvokeRefusesUnboundContainerTest,
	"CrowdySDK.GameModel.InvokeRefusesUnboundContainer", CrowdyBindPendingTestFlags)
bool FCrowdyGameModelInvokeRefusesUnboundContainerTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	// A NetID nothing has bound a container for yet: the exact bind-pending state an entity sits in between
	// registering and its ResolveOrCreateContainer round trip completing.
	const FGuid NetID = FGuid::NewGuid();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("amount"), 5);

	int32 Notified = 0;
	FCrowdyInvokeResult LastResult;
	Model->InvokeAndApply(NetID, TEXT("ApplyDamage"), Params, FString(),
		[&Notified, &LastResult](FCrowdyInvokeResult Result) { ++Notified; LastResult = MoveTemp(Result); });

	TestEqual(TEXT("the bind-pending refusal reaches the caller through OnDone exactly once; it does not vanish"),
		Notified, 1);
	TestFalse(TEXT("the refused apply did not succeed"), LastResult.bSuccess);
	TestTrue(TEXT("the reported reason names the actual problem, not a generic failure"),
		LastResult.ErrorMessage.Contains(TEXT("no container bound")));

	// Bind the container the entity was missing and repeat the IDENTICAL call. It must get PAST the
	// bind-pending refusal now: whatever it reports next has to be a different failure (the headless completion-
	// liveness suite's own missing-API-context error, whitelisted below), never the same "no container bound".
	AllowMissingApiContextForBindPendingTests(*this);
	Model->BindEntityContainer(NetID, TEXT("cid-now-bound"));

	Notified = 0;
	LastResult = FCrowdyInvokeResult();
	Model->InvokeAndApply(NetID, TEXT("ApplyDamage"), Params, FString(),
		[&Notified, &LastResult](FCrowdyInvokeResult Result) { ++Notified; LastResult = MoveTemp(Result); });

	TestEqual(TEXT("still notified exactly once after binding"), Notified, 1);
	TestFalse(TEXT("the bind-pending reason is gone: the gate this test targets specifically cleared"),
		LastResult.ErrorMessage.Contains(TEXT("no container bound")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
