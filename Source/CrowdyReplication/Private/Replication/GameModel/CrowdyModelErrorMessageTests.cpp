// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModelErrorText.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelErrorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// A failure envelope must surface a non-empty, human-readable message on the effect-apply Failed path. The effect
// apply node broadcasts FCrowdyInvokeResult::ErrorMessage, populated by ParseInvokeEnvelope, so this exercises the
// exact seam a Blueprint reads: a transport error, a rolled-back server call, and the pure fallback all yield text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectApplyErrorMessageSurfacedTest,
	"CrowdySDK.Replication.EffectApplyErrorMessageSurfaced", CrowdyModelErrorTestFlags)
bool FCrowdyEffectApplyErrorMessageSurfacedTest::RunTest(const FString& Parameters)
{
	// Transport-layer failure: the request never reached the server (or the GraphQL envelope carried errors[]).
	// ErrorMessage must be the transport error verbatim and bTransportOk stays false.
	{
		TArray<FString> TransportErrors;
		TransportErrors.Add(TEXT("unauthorized: missing bearer token"));
		const FCrowdyInvokeResult Result =
			FCrowdyGameApiCodec::ParseInvokeEnvelope(nullptr, false, TransportErrors);
		TestFalse(TEXT("a transport failure is not transport-ok"), Result.bTransportOk);
		TestFalse(TEXT("a transport failure is not a success"), Result.bSuccess);
		TestFalse(TEXT("transport failure message is non-empty"), Result.ErrorMessage.IsEmpty());
		TestEqual(TEXT("transport failure message is verbatim"), Result.ErrorMessage,
			TEXT("unauthorized: missing bearer token"));
	}

	// Server-side rollback: the server ran the function, its logic/authority check failed, and it returned an
	// errorMessage. Reached the server (bTransportOk) but bSuccess is false and the message is the server's.
	{
		const TSharedPtr<FJsonObject> Envelope = ParseObject(TEXT(
			"{\"data\":{\"gameModelInvoke\":{\"success\":false,\"returnValueJson\":\"\",\"errorMessage\":\"not enough mana\"}}}"));
		if (!TestNotNull(TEXT("envelope parsed"), Envelope.Get()))
		{
			return false;
		}
		const FCrowdyInvokeResult Result =
			FCrowdyGameApiCodec::ParseInvokeEnvelope(Envelope, true, TArray<FString>());
		TestTrue(TEXT("a rolled-back call reached the server"), Result.bTransportOk);
		TestFalse(TEXT("a rolled-back call is not a success"), Result.bSuccess);
		TestEqual(TEXT("server rejection message is surfaced"), Result.ErrorMessage, TEXT("not enough mana"));
	}

	// The shared helper never yields an empty string, even when the server returned no message text: a bare
	// non-success result still reads as a distinguishable, human-readable sentence.
	{
		FCrowdyInvokeResult Bare;
		Bare.bTransportOk = true;
		Bare.bSuccess = false;
		const FString Message = CrowdyModelErrorText::FromInvokeResult(Bare);
		TestFalse(TEXT("a messageless rejection still yields text"), Message.IsEmpty());

		FCrowdyInvokeResult NoTransport;
		NoTransport.bTransportOk = false;
		NoTransport.bSuccess = false;
		const FString TransportMessage = CrowdyModelErrorText::FromInvokeResult(NoTransport);
		TestFalse(TEXT("a messageless transport failure still yields text"), TransportMessage.IsEmpty());
		TestNotEqual(TEXT("transport and rejection fallbacks differ"), TransportMessage, Message);
	}

	return true;
}

// The last-error cache round-trips: SetLastModelError stores a message and GetLastModelError reads it back, and a
// second write replaces the first. This is what "Get Last Crowdy Model Error" surfaces after a payload-less Failed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLastModelErrorCacheTest,
	"CrowdySDK.Replication.LastModelErrorCache", CrowdyModelErrorTestFlags)
bool FCrowdyLastModelErrorCacheTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Subsystem = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem constructed"), Subsystem))
	{
		return false;
	}

	Subsystem->SetLastModelError(TEXT("Set Model Attribute: the value must be a JSON literal."));
	TestEqual(TEXT("first error is retrievable"), Subsystem->GetLastModelError(),
		TEXT("Set Model Attribute: the value must be a JSON literal."));

	Subsystem->SetLastModelError(TEXT("container not found"));
	TestEqual(TEXT("a later error replaces the earlier one"), Subsystem->GetLastModelError(),
		TEXT("container not found"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
