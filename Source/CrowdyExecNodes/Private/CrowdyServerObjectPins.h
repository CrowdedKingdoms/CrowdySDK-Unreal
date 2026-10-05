#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class FProperty;
class UCrowdyServerObjectDefinition;
class UScriptStruct;
struct FCrowdyServerFunction;

/** One variable, input or output as a node shows it: its pin type, or why it has none. */
struct FCrowdyServerValuePin
{
	const FProperty* Property = nullptr;

	/** The authored name: the pin's name and the name the runtime finds the value by. */
	FName Name;

	/** The List or Blueprint struct id, which keeps the pin when the value is renamed; invalid for a C++ field. */
	FGuid Id;

	FEdGraphPinType Type;

	/** A TOptional value, which gets a Has Value pin beside its value pin. */
	bool bOptional = false;

	/** A variable in Visible to Players, which players receive; inputs and outputs always are. */
	bool bVisible = true;

	/** Why the value has no pin; empty when it has one. */
	FText Refusal;

	bool HasPin() const { return Refusal.IsEmpty(); }
};

namespace CrowdyServerObjectPins
{
	/** The schema's pin for Property, integers Blueprints lack widened and a TOptional as its value; false with Refusal when none fits. */
	bool MakePinType(const FProperty* Property, FEdGraphPinType& OutType, bool& bOutOptional, FText& OutRefusal);

	/** The values of Struct that travel, in declaration order. */
	void GatherValues(const UScriptStruct* Struct, TArray<FCrowdyServerValuePin>& Out);

	/** Definition's variables that travel, visible to players or not. */
	void GatherVariables(const UCrowdyServerObjectDefinition* Definition, TArray<FCrowdyServerValuePin>& Out);

	/** Struct's value with Id, or else called Name. Null when there is neither. */
	const FCrowdyServerValuePin* FindValue(TConstArrayView<FCrowdyServerValuePin> Values, const FGuid& Id, FName Name);

	/** The name of Name's Has Value pin. */
	FName HasValuePinName(FName Name);

	/** The persistent id of a Has Value pin, derived from its value's id so it follows a rename too. */
	FGuid HasValuePinId(const FGuid& Id);

	/** The id that follows a struct field or List entry through renames; invalid for a C++ field. */
	FGuid GetValueId(const UScriptStruct* Struct, const FProperty* Property);

	/** Refuses each value named like one of the node's own pins or another value's Has Value pin. */
	void RefuseClashes(TArray<FCrowdyServerValuePin>& Values);

	/** Loads Definition and its structs now if still waiting to be read, as they can be while a Blueprint compiles on load. */
	void PreloadDefinition(UCrowdyServerObjectDefinition* Definition);
}
