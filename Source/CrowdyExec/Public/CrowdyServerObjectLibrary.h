#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObjectTypes.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "CrowdyServerObjectLibrary.generated.h"

class UCrowdyServerObject;
class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectLink;
class UCrowdyServerObjectSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FCrowdyServerCallDone, const FInstancedStruct&, Outputs, ECrowdyServerCallOutcome, Outcome, const FString&, Reason, bool, bRetryable, FName, Code);

/** Blueprint access to Server Objects: get one, and read or set its values by name. */
UCLASS()
class CROWDYEXEC_API UCrowdyServerObjectLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * The Server Object for Definition and Instance Id, held for Owner until Owner is destroyed or calls Release Server
	 * Object. Owners that ask for the same one share it. None, with Error saying why, when it cannot be used.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (WorldContext = "WorldContextObject", DefaultToSelf = "Owner"))
	static UCrowdyServerObject* GetServerObject(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, const FString& InstanceId, UObject* Owner, FString& Error);

	/** Stops Owner holding Object. It is given back a few seconds after its last owner lets go. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (DefaultToSelf = "Owner"))
	static void ReleaseServerObject(UCrowdyServerObject* Object, UObject* Owner);

	/**
	 * How many players are in the app's world, held by the app's root: the Game Kit's world when the Game Kit World setting
	 * names one, else the root Server Object the Crowdy SDK deploys with a project's types.
	 * Owner follows it from the first call until Owner is destroyed. False, with 0, until the count first arrives.
	 */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", meta = (DefaultToSelf = "Owner"))
	static bool GetPlayersOnline(UObject* Owner, int32& PlayersOnline);

	/** The definition the Game Kit World setting names, loaded on first use; null when the setting is empty or names no Game Kit starter. */
	static UCrowdyServerObjectDefinition* GetGameKitWorldDefinition();

	/** The root Server Object's definition, CSO_PlayersOnline in the Crowdy SDK's content: Players Online is its one variable. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	static UCrowdyServerObjectDefinition* GetPlayersOnlineDefinition();

	/** Where Get Players Online Definition loads it from. */
	static const TCHAR* const PlayersOnlineDefinitionPath;

	/** The signed-in player's user id, the Instance Id of their own Owner Only Server Object. Empty when nobody is signed in. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", meta = (WorldContext = "WorldContextObject"))
	static FString GetPlayerInstanceId(UObject* WorldContextObject);

	/** Calls Function on a Server Function Library; OnDone runs once with the answer, the refusal or the timeout; it is not called if the game instance shuts down first. The world keeps the library's connection, so there is nothing to release. */
	static void CallLibraryFunction(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, FName Function, const FInstancedStruct& Inputs, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds = 0.f);

	/** Calls a Server Function Library's function that takes a List, with Inputs from MakeParams. */
	static void CallLibraryFunction(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, FName Function, const FInstancedPropertyBag& Inputs, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds = 0.f);

	/** Sets the value called Name in Values (inputs from Make Inputs). False when there is none by that name or Value is of another type. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Crowdy SDK|Server Objects", meta = (CustomStructureParam = "Value", DisplayName = "Set Server Value"))
	static bool SetServerValue(UPARAM(ref) FInstancedStruct& Values, FName Name, const int32& Value);

	/** Reads the value called Name from Values (variables or outputs). False when there is none by that name or Value is of another type. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Crowdy SDK|Server Objects", meta = (CustomStructureParam = "Value", DisplayName = "Get Server Value"))
	static bool GetServerValue(const FInstancedStruct& Values, FName Name, int32& Value);

	/** The value of Values' struct called Name, ignoring case, spaces and punctuation; null when there is none that travels. */
	static const FProperty* FindServerValue(const FInstancedStruct& Values, FName Name);

	/** Set Server Value for C++: copies the value at Source, of type SourceProperty, into Values' Name. */
	static bool SetServerValueFrom(FInstancedStruct& Values, FName Name, const FProperty* SourceProperty, const void* Source);

	/** Get Server Value for C++: copies Values' Name into Dest, of type DestProperty. */
	static bool GetServerValueInto(const FInstancedStruct& Values, FName Name, const FProperty* DestProperty, void* Dest);

	/** The object Target holds: a Server Object of Definition, its component, an actor with one such component, or a Link; else null. */
	static UCrowdyServerObject* ResolveServerObject(const UObject* Target, const UCrowdyServerObjectDefinition* Definition);

	/** The Server Object Target holds, as Resolve Server Object finds it. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", DefaultToSelf = "Target"))
	static UCrowdyServerObject* GetHeldServerObject(const UObject* Target, const UCrowdyServerObjectDefinition* Definition);

	/** The Link following Definition's instance for Owner: one per owner and arguments, made on first use, kept while Owner lives. Only One Instance types ignore Instance. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", DefaultToSelf = "Owner"))
	static UCrowdyServerObjectLink* FindServerObjectLink(UObject* Owner, UCrowdyServerObjectDefinition* Definition, ECrowdyServerObjectFind Instance, const FString& InstanceId, int64 TeamId);

	/** Reads Variable of the object Target holds into Value; true only for a Ready object's value, not before it is Ready or for an empty optional. */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", CustomStructureParam = "Value"))
	static bool ReadServerVariable(const UObject* Target, const UCrowdyServerObjectDefinition* Definition, FName Variable, int32& Value);

	/** Read Server Variable for C++: Dest is of type DestProperty. */
	static bool ReadServerVariableInto(const UObject* Target, const UCrowdyServerObjectDefinition* Definition, FName Variable, const FProperty* DestProperty, void* Dest);

	/** Copies Values' Name into Dest, true only when it fits and bReady; otherwise Dest is cleared, and a value that does not fit is warned about once. */
	static bool ReadReadyValueInto(const FInstancedStruct* Values, bool bReady, FName Name, const FProperty* DestProperty, void* Dest);

	/** Runs Handler's HandlerFunction (the value, then Has Value for an optional) now when Ready and after each change; rebinding moves it. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true"))
	static void BindServerVariableChanged(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Variable, UObject* Handler, FName HandlerFunction);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true"))
	static void UnbindServerVariableChanged(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Variable, UObject* Handler, FName HandlerFunction);

	/** Runs Handler's HandlerFunction, with the payload as a Server Value struct, each time the server code sends Message to Target's object; rebinding moves it. Nothing is replayed. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true"))
	static void BindServerMessage(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Message, UObject* Handler, FName HandlerFunction);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true"))
	static void UnbindServerMessage(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Message, UObject* Handler, FName HandlerFunction);

	/** Function's inputs with their default values; empty when it takes none. */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true"))
	static FInstancedStruct MakeFunctionInputs(const UCrowdyServerObjectDefinition* Definition, FName Function);

	/** Sets an optional input called Name to Value, or leaves it unset without Has Value. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", CustomStructureParam = "Value"))
	static bool SetServerValueOrUnset(UPARAM(ref) FInstancedStruct& Values, FName Name, bool bHasValue, const int32& Value);

	/** Set Server Value Or Unset for C++. */
	static bool SetServerValueOrUnsetFrom(FInstancedStruct& Values, FName Name, bool bHasValue, const FProperty* SourceProperty, const void* Source);

	/** Errors, plus a sentence naming Input when bSet is false. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true"))
	static FString NoteInputError(const FString& Errors, bool bSet, FName Input);

private:
	DECLARE_FUNCTION(execSetServerValue);
	DECLARE_FUNCTION(execGetServerValue);
	DECLARE_FUNCTION(execReadServerVariable);
	DECLARE_FUNCTION(execSetServerValueOrUnset);
};

/** Calls a Server Function and fires On Success or On Failed exactly once, unless the game shuts down first. */
UCLASS()
class CROWDYEXEC_API UCrowdyServerCallAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Outputs holds the function's outputs; read them with Get Server Value. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects")
	FCrowdyServerCallDone OnSuccess;

	/** Reason says what went wrong; Retryable means calling again later may succeed; Code is a Game Kit starter's refusal code. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects")
	FCrowdyServerCallDone OnFailed;

	/**
	 * Inputs come from Make Inputs, or Make Instanced Struct for a function whose inputs are a struct; leave empty when it takes none.
	 * @param Timeout Seconds before the call gives up, when shorter than the project's Server Object Call Timeout; 0 leaves that timeout.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Inputs", AdvancedDisplay = "Timeout"), DisplayName = "Call Server Function")
	static UCrowdyServerCallAction* CallServerFunction(UObject* WorldContextObject, UCrowdyServerObject* Object, FName Function, const FInstancedStruct& Inputs, float Timeout = 0.f);

	virtual void Activate() override;

protected:
	void Setup(UObject* WorldContextObject, UCrowdyServerObject* InObject, FName InFunction, const FInstancedStruct& InInputs, const FString& InInputError, float InTimeout);

private:
	void Finish(const FCrowdyServerCallResult& Result);

	UPROPERTY()
	TObjectPtr<UCrowdyServerObject> CalledObject;

	FName CalledFunction;
	FInstancedStruct CalledInputs;
	FString RefusedInputs;
	float CalledTimeout = 0.f;
};

/** The typed Call node's action; the node places it itself, so it has no palette entry of its own. */
UCLASS(meta = (HasDedicatedAsyncNode))
class CROWDYEXEC_API UCrowdyServerTypedCallAction : public UCrowdyServerCallAction
{
	GENERATED_BODY()

public:
	/**
	 * Call Server Function, except that a non-empty InputError sends nothing and fails with it as a Bad Request.
	 * @param Timeout Seconds before the call gives up, when shorter than the project's Server Object Call Timeout; 0 leaves that timeout.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", AutoCreateRefTerm = "Inputs", AdvancedDisplay = "Timeout"))
	static UCrowdyServerCallAction* CallServerFunctionWithInputs(UObject* WorldContextObject, UCrowdyServerObject* Object, FName Function, const FInstancedStruct& Inputs, const FString& InputError, float Timeout = 0.f);
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyServerPingDone, float, RoundTripMs, const FString&, Reason);

/** Sends the platform a ping over the Server Object connection and fires On Success with the round trip, or On Failed with why, exactly once, unless the game shuts down first. */
UCLASS()
class CROWDYEXEC_API UCrowdyServerPingAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** RoundTripMs is how long the platform took to answer; Reason is empty. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects")
	FCrowdyServerPingDone OnSuccess;

	/** Reason says why there was no answer; RoundTripMs is 0. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects")
	FCrowdyServerPingDone OnFailed;

	/** Pings over the connection Server Objects already hold open; it never opens one. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"), DisplayName = "Ping Server Objects")
	static UCrowdyServerPingAction* PingServerObjects(UObject* WorldContextObject);

	virtual void Activate() override;

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for the game instance's subsystem, which a test without a world cannot reach. */
	void SetSubsystemForTest(UCrowdyServerObjectSubsystem* Subsystem);
#endif

private:
	void Finish(bool bOk, float RoundTripMs, const FString& Reason);

	TWeakObjectPtr<UCrowdyServerObjectSubsystem> PingSubsystem;
};
