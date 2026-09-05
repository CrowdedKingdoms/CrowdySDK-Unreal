#include "Replication/Components/CrowdyEntityComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyEntityTypes.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEntityDiagnosticTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every condition met: the starting point each case below breaks exactly one field of.
	FCrowdyPlayerDerivedIdentityFacts ResolvableFacts()
	{
		FCrowdyPlayerDerivedIdentityFacts Facts;
		Facts.bIsPawn = true;
		Facts.bHasController = true;
		Facts.bControllerIsPlayerController = true;
		Facts.bIsLocalController = true;
		Facts.bIsPrimaryPlayer = true;
		Facts.bHasGameSession = true;
		return Facts;
	}
}

// Player Derived identity has five separate ways to miss plus a missing session, and they need different fixes: a
// world actor is set to Stable, an unpossessed pawn is a timing question, a remote pawn is expected. The verdict
// must name the condition actually measured, so each case breaks exactly one fact and expects its own answer, and
// the earlier questions must win over the later ones (a bare actor is not answered "no game session").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedVerdictTest,
	"CrowdySDK.Entity.PlayerDerivedIdentityVerdict", CrowdyEntityDiagnosticTestFlags)
bool FCrowdyEntityPlayerDerivedVerdictTest::RunTest(const FString& Parameters)
{
	using UEC = UCrowdyEntityComponent;
	using EVerdict = ECrowdyPlayerDerivedIdentity;

	TestEqual(TEXT("every condition met resolves"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(ResolvableFacts())), static_cast<uint8>(EVerdict::Resolved));

	FCrowdyPlayerDerivedIdentityFacts Facts = ResolvableFacts();
	Facts.bIsPawn = false;
	TestEqual(TEXT("an owner that is not a pawn is named as such"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::NotAPawn));

	Facts = ResolvableFacts();
	Facts.bHasController = false;
	Facts.bControllerIsPlayerController = false;
	Facts.bIsLocalController = false;
	Facts.bIsPrimaryPlayer = false;
	TestEqual(TEXT("an unpossessed pawn is named as unpossessed"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::PawnHasNoController));

	Facts = ResolvableFacts();
	Facts.bControllerIsPlayerController = false;
	Facts.bIsLocalController = false;
	Facts.bIsPrimaryPlayer = false;
	TestEqual(TEXT("an AI-possessed pawn is separated from an unpossessed one"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::ControllerIsNotAPlayer));

	Facts = ResolvableFacts();
	Facts.bIsLocalController = false;
	TestEqual(TEXT("another client's pawn is named as remote"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::ControllerIsRemote));

	Facts = ResolvableFacts();
	Facts.bIsPrimaryPlayer = false;
	TestEqual(TEXT("a split-screen secondary player is named as secondary"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::NotThePrimaryPlayer));

	Facts = ResolvableFacts();
	Facts.bHasGameSession = false;
	TestEqual(TEXT("the right pawn with no session is named as having no session"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::NoGameSession));

	// Ordering: with nothing true at all, the answer is what the actor IS, not the last thing that was missing.
	// Answering "no game session" for a plain world actor would send an author looking for a login problem.
	Facts = FCrowdyPlayerDerivedIdentityFacts();
	TestEqual(TEXT("the first unmet condition wins over the later ones"),
		static_cast<uint8>(UEC::ClassifyPlayerDerivedIdentity(Facts)), static_cast<uint8>(EVerdict::NotAPawn));

	// Each verdict describes itself distinctly, so the message never reuses one condition's words for another.
	const TArray<EVerdict> Verdicts = {
		EVerdict::NotAPawn, EVerdict::PawnHasNoController, EVerdict::ControllerIsNotAPlayer,
		EVerdict::ControllerIsRemote, EVerdict::NotThePrimaryPlayer, EVerdict::NoGameSession };
	TSet<FString> Clauses;
	for (const EVerdict Verdict : Verdicts)
	{
		Clauses.Add(FString(UEC::DescribePlayerDerivedIdentity(Verdict)));
	}
	TestEqual(TEXT("every verdict has its own clause"), Clauses.Num(), Verdicts.Num());
	return true;
}

// The degradation itself, driven end to end through identity resolution: a component set to Player Derived on an
// actor that is not a pawn silently produced a random id. It now says so once, names the condition it measured,
// and names what to set instead. The id is still minted, so the diagnostic changes nothing but the silence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedFallbackReportTest,
	"CrowdySDK.Entity.PlayerDerivedFallbackReport", CrowdyEntityDiagnosticTestFlags)
bool FCrowdyEntityPlayerDerivedFallbackReportTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("component created"), Component))
	{
		return false;
	}

	Component->IdentityPolicy = ECrowdyIdentityPolicy::PlayerDerived;
	const FGuid NetID = Component->ResolveIdentityForTest(Actor);

	TestTrue(TEXT("the fallback id is still minted"), NetID.IsValid());
	TestEqual(TEXT("the degradation is reported"), Component->PlayerDerivedFallbackReportCount, 1);

	const FString& Message = Component->LastPlayerDerivedFallbackMessage;
	TestTrue(TEXT("the message names the actor"), Message.Contains(Actor->GetName()));
	TestTrue(TEXT("the message names the condition it measured"),
		Message.Contains(UCrowdyEntityComponent::DescribePlayerDerivedIdentity(ECrowdyPlayerDerivedIdentity::NotAPawn)));
	TestTrue(TEXT("the message names the consequence"), Message.Contains(TEXT("random id")));
	TestTrue(TEXT("the message names what to set"), Message.Contains(TEXT("set Identity Policy to Stable")));
	return true;
}

// The other silent degradation: a Dynamic host-owned entity with Auto Register ticked finishes BeginPlay without
// joining the continuous channel, because Auto Register starts it only for an entity this client owns outright.
// The predicate is the whole condition, so each input that would make it fire is checked one at a time.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityHostOwnedAutoRegisterReportTest,
	"CrowdySDK.Entity.HostOwnedAutoRegisterReport", CrowdyEntityDiagnosticTestFlags)
bool FCrowdyEntityHostOwnedAutoRegisterReportTest::RunTest(const FString& Parameters)
{
	using UEC = UCrowdyEntityComponent;

	TestTrue(TEXT("Dynamic, Auto Register, host-owned is the case that goes quiet"),
		UEC::ShouldReportHostOwnedAutoRegister(ECrowdyEntityMode::Dynamic, true, ECrowdyRole::HostOwned));
	TestFalse(TEXT("an entity this client owns outright starts, so there is nothing to say"),
		UEC::ShouldReportHostOwnedAutoRegister(ECrowdyEntityMode::Dynamic, true, ECrowdyRole::Owner));
	TestFalse(TEXT("a remote proxy was never going to auto-register"),
		UEC::ShouldReportHostOwnedAutoRegister(ECrowdyEntityMode::Dynamic, true, ECrowdyRole::RemoteProxy));
	TestFalse(TEXT("Auto Register off asked for nothing, so nothing is withheld"),
		UEC::ShouldReportHostOwnedAutoRegister(ECrowdyEntityMode::Dynamic, false, ECrowdyRole::HostOwned));
	TestFalse(TEXT("a Static entity has no continuous channel to join"),
		UEC::ShouldReportHostOwnedAutoRegister(ECrowdyEntityMode::Static, true, ECrowdyRole::HostOwned));

	// The host clause must not overstate what the client knows: an unelected host and an unknown one read alike.
	TestEqual(TEXT("no host id means the host is not known yet"),
		FString(UEC::DescribeHostElectionState(false, false)), FString(UEC::DescribeHostElectionState(false, true)));
	TestNotEqual(TEXT("being the host reads differently from another client being it"),
		FString(UEC::DescribeHostElectionState(true, true)), FString(UEC::DescribeHostElectionState(true, false)));

	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("component created"), Component))
	{
		return false;
	}

	Component->ReportHostOwnedAutoRegisterSkippedForTest();
	TestEqual(TEXT("the degradation is reported"), Component->HostOwnedAutoRegisterReportCount, 1);

	const FString& Message = Component->LastHostOwnedAutoRegisterMessage;
	TestTrue(TEXT("the message names the actor"), Message.Contains(Actor->GetName()));
	TestTrue(TEXT("the message names what this client knows about the host"),
		Message.Contains(UCrowdyEntityComponent::DescribeHostElectionState(false, false)));
	TestTrue(TEXT("the message names the consequence"), Message.Contains(TEXT("Nothing goes out on the continuous channel")));
	TestTrue(TEXT("the message names what to set"), Message.Contains(TEXT("Set Ownership to Local Client")));
	TestTrue(TEXT("the message names the other way out"), Message.Contains(TEXT("call Start Replication")));
	return true;
}

// Both diagnostics describe how one component was configured, which does not change while it lives, so each speaks
// once on that component and never again. Once per INSTANCE, not once per class: a second component with the same
// problem is a second thing to fix and still says so. Nothing here is time-gated, so a report is never lost to a
// window that was already open.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityDiagnosticsOncePerInstanceTest,
	"CrowdySDK.Entity.DiagnosticsOncePerInstance", CrowdyEntityDiagnosticTestFlags)
bool FCrowdyEntityDiagnosticsOncePerInstanceTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* First = NewObject<UCrowdyEntityComponent>(Actor);
	UCrowdyEntityComponent* Second = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("first component created"), First) || !TestNotNull(TEXT("second component created"), Second))
	{
		return false;
	}

	First->IdentityPolicy = ECrowdyIdentityPolicy::PlayerDerived;
	Second->IdentityPolicy = ECrowdyIdentityPolicy::PlayerDerived;

	First->ResolveIdentityForTest(Actor);
	TestEqual(TEXT("the first resolution reports"), First->PlayerDerivedFallbackReportCount, 1);

	First->ResolveIdentityForTest(Actor);
	TestEqual(TEXT("resolving again on the same component is silent"), First->PlayerDerivedFallbackReportCount, 1);

	Second->ResolveIdentityForTest(Actor);
	TestEqual(TEXT("a second component with the same problem still speaks"), Second->PlayerDerivedFallbackReportCount, 1);

	First->ReportHostOwnedAutoRegisterSkippedForTest();
	TestEqual(TEXT("the host-owned report speaks once"), First->HostOwnedAutoRegisterReportCount, 1);

	First->ReportHostOwnedAutoRegisterSkippedForTest();
	TestEqual(TEXT("a second host-owned report on the same component is silent"), First->HostOwnedAutoRegisterReportCount, 1);

	Second->ReportHostOwnedAutoRegisterSkippedForTest();
	TestEqual(TEXT("a second component still speaks about its own channel"), Second->HostOwnedAutoRegisterReportCount, 1);

	// The two diagnostics are memoised independently: one speaking must not silence the other.
	TestEqual(TEXT("the identity report is untouched by the channel report"), First->PlayerDerivedFallbackReportCount, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
