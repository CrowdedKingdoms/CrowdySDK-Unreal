// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyModelValue.generated.h"

/**
 * Blueprint decoders for a single serialized Game Model value: the OldValueJson/NewValueJson the "Listen for Model
 * Changes" node hands out, or the ReturnValueJson an effect apply answers with, each a lone JSON value such as "5",
 * "true", or "\"text\"". Each read is pure and treats the input as forged: an empty, malformed, or wrong-typed
 * value yields the supplied Default and never crashes. Use these so a Blueprint reacting to a change reads a typed
 * value instead of parsing raw JSON by hand.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyModelValue : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// The value as an integer when it is a JSON number (truncated toward zero, matching the collection getters);
	// Default when it is empty, malformed, or any other JSON type.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "As Integer")
	static int32 AsInt(const FString& ValueJson, int32 Default = 0);

	// The value as a float when it is a JSON number; Default otherwise.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "As Float")
	static float AsFloat(const FString& ValueJson, float Default = 0.0f);

	// The value as a double when it is a JSON number; Default otherwise. Not a Blueprint node (AsFloat is the
	// palette entry); this exists for a C++ caller that needs full double precision, such as a PC_Double pin.
	static double AsDouble(const FString& ValueJson, double Default = 0.0);

	// The value as a bool when it is a JSON boolean; bDefault otherwise (a numeric 0/1 is not treated as a bool).
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "As Boolean")
	static bool AsBool(const FString& ValueJson, bool bDefault = false);

	// The string content when the value is a JSON string; Default otherwise (a number or bool is not a string).
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", DisplayName = "As String")
	static FString AsString(const FString& ValueJson, const FString& Default = TEXT(""));
};
