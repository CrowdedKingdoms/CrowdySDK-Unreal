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

	/** The same, also true for a Game Kit starter's function its server code keeps for developers, as an asset saved before it said so. */
	bool IsForDevelopers(const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function);

	/** Inputs as JSON, read into an instance of the function's input struct; empty for a function without inputs. False with OutError on a field that does not fit. With Definition, an input the JSON names that has a Has flag (bHasSeed beside Seed) is marked present unless the JSON names the flag. */
	bool InputsFromJson(const FCrowdyServerFunction& Function, const FString& Json, FInstancedStruct& OutInputs, FString& OutError, const UCrowdyServerObjectDefinition* Definition = nullptr);

	/** A Ref no call sent before, such as studio-grant-<32 hex digits>: a Game Kit starter's Grant, Record and Deposit run once per Ref and answer a Ref used before as they did then, giving nothing more. */
	FString MakeFreshRef(const FCrowdyServerFunction& Function);

	/** When Definition is a Game Kit starter and Inputs has a Ref text field left empty, fills it with MakeFreshRef. OutRef is the Ref the call sends, empty for inputs without one. */
	void FillFreshRef(const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function, FInstancedStruct& Inputs, FString& OutRef);

	/** Whether Key is a user id as a player's hub is keyed and a game reads it: decimal, above 0, with no sign or leading zero. */
	bool IsPlayerKey(const FString& Key);

	/** Why a call to Definition's instance Key with Inputs would do nothing a developer meant, such as a grant of an empty bundle or to a player id the game never reads; empty when there is no such reason. */
	FString FindPointlessCall(const UCrowdyServerObjectDefinition& Definition, const FString& Key, const FInstancedStruct& Inputs);

	/** Calls Function on Definition's instance Key with Inputs as the signed-in developer and decodes the reply; OnDone runs once on the game thread. */
	void Call(const CrowdyExecDeveloper::FClient& Client, const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function,
		const FString& Key, const FInstancedStruct& Inputs, TFunction<void(const FResult&)> OnDone);
}
