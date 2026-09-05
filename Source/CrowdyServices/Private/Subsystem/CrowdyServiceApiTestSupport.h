#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Helpers shared by the service test files. They live in one header rather than file-locally in each, because a
 * helper defined in an anonymous namespace in two translation units of one module redefines itself the moment a
 * unity build merges them.
 */
namespace CrowdyServiceApiTest
{
	/**
	 * Whether an operation name resolves to a document in the vendored tables.
	 *
	 * A name that does not resolve is answered inside the RunOp call, before the request reaches the dispatcher;
	 * one that does resolve only completes from Poll(). So whether the completion ran before any Poll() is a direct
	 * read of whether resolution happened rather than a guess from the error wording.
	 */
	inline bool OperationResolves(const TSharedPtr<FCrowdyCppClient>& Client, ECrowdyCppApiDomain Domain,
		const TCHAR* OperationName, FString& OutError)
	{
		bool bFiredBeforePoll = false;
		FCrowdyCppJsonResult Captured;
		Client->RunOp(Domain, OperationName, MakeShared<FJsonObject>(),
			[&Captured, &bFiredBeforePoll](FCrowdyCppJsonResult Result)
			{
				Captured = MoveTemp(Result);
				bFiredBeforePoll = true;
			});

		if (bFiredBeforePoll)
		{
			OutError = Captured.ErrorMessage;
			return false;
		}

		Client->Poll();
		OutError = Captured.ErrorMessage;
		return true;
	}

	/** Build an FCrowdyCppJsonResult around a literal `data` payload, which is the shape RunOp hands a completion. */
	inline FCrowdyCppJsonResult MakeResult(const FString& DataJson)
	{
		FCrowdyCppJsonResult Result;
		Result.bTransportOk = true;

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DataJson);
		FJsonSerializer::Deserialize(Reader, Result.Data);
		return Result;
	}

	/** A result for a call that never reached a verdict, carrying the message a cancellation delivers. */
	inline FCrowdyCppJsonResult MakeCanceledResult()
	{
		FCrowdyCppJsonResult Result;
		Result.bTransportOk = false;
		Result.ErrorMessage = FCrowdyCppClient::CanceledErrorMessage();
		return Result;
	}
}

#endif
