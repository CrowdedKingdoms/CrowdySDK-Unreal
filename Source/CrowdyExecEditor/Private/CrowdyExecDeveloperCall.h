#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecDeveloperApi.h"
#include "StructUtils/InstancedStruct.h"

class UCrowdyServerObjectDefinition;
struct FCrowdyServerFunction;

/** Calling one Server Function as the app's developer, from the editor or the command line, the way developer tools reach Server Only functions. */
namespace CrowdyExecDeveloperCall
{
	struct FResult
	{
		bool bOk = false;
		/** "Ok", or the platform's status such as "AppError" or "Denied". */
		FString Status;
		/** Why it failed, as the server or the editor says. */
		FString Message;
		/** The reply's values as JSON, when the call succeeded and they decoded. */
		FString ReplyJson;
	};

	/** Only Server Only and Developers Only functions are called this way; players reach the others from a game. */
	bool IsForDevelopers(const FCrowdyServerFunction& Function);

	/** Inputs as JSON, read into an instance of the function's input struct; empty for a function without inputs. False with OutError on a field that does not fit. */
	bool InputsFromJson(const FCrowdyServerFunction& Function, const FString& Json, FInstancedStruct& OutInputs, FString& OutError);

	/** Calls Function on Definition's instance Key with Inputs as the signed-in developer and decodes the reply; OnDone runs once on the game thread. */
	void Call(const CrowdyExecDeveloper::FClient& Client, const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function,
		const FString& Key, const FInstancedStruct& Inputs, TFunction<void(const FResult&)> OnDone);
}
