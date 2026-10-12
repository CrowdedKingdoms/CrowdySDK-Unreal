#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StructUtils/InstancedStruct.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include <type_traits>
#include "CrowdyMassServerValuesLibrary.generated.h"

/** The server values of a crowd entity: the record its actor class's Crowdy Server Object component reads. Game thread only. */
UCLASS()
class CROWDYMASS_API UCrowdyMassServerValuesLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Reads the server variable called Name of the entity Target names into Value. True only for a value the server sent that fits Value. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Crowdy Mass|Server Values", meta = (CustomStructureParam = "Value", DisplayName = "Get Server Value"))
	static bool GetServerValue(const FCrowdyMassEntityTarget& Target, FName Name, int32& Value);

	/** Get Server Value for C++: Dest is of type DestProperty. */
	static bool GetServerValueInto(const FCrowdyMassEntityTarget& Target, FName Name, const FProperty* DestProperty, void* Dest);

	/**
	 * Get Server Value for C++, typed: the variable must hold exactly T. True only for a value the server sent. Out is reset
	 * to T() when there is no such variable or it holds another type, and holds the record's current value before it is ready.
	 */
	template <typename T>
	static bool GetServerValueAs(const FCrowdyMassEntityTarget& Target, FName Name, T& Out)
	{
		const FProperty* Source = FindServerValueProperty(Target, Name);
		if (!Source || !PropertyHolds<T>(*Source))
		{
			Out = T();
			return false;
		}
		if constexpr (std::is_same_v<T, bool>)
		{
			// A record bool may be a bitfield, so it is read through its property rather than written over Out.
			return GetServerBoolValue(Target, Name, *CastFieldChecked<FBoolProperty>(Source), Out);
		}
		else
		{
			return GetServerValueInto(Target, Name, Source, &Out);
		}
	}

	/** GetServerValueAs for a bool, which may be a bitfield in the record. */
	static bool GetServerBoolValue(const FCrowdyMassEntityTarget& Target, FName Name, const FBoolProperty& Source, bool& Out);

	/** The property the server variable Name is stored in for the entity Target names, or null before its values arrive or when there is no such variable. */
	static const FProperty* FindServerValueProperty(const FCrowdyMassEntityTarget& Target, FName Name);

	/** Whether Property stores exactly a T: a number of the same width, a string, a name, text, a UENUM or a USTRUCT. */
	template <typename T>
	static bool PropertyHolds(const FProperty& Property)
	{
		if constexpr (std::is_same_v<T, bool>) { return Property.IsA<FBoolProperty>(); }
		else if constexpr (std::is_same_v<T, int32>) { return Property.IsA<FIntProperty>(); }
		else if constexpr (std::is_same_v<T, int64>) { return Property.IsA<FInt64Property>(); }
		else if constexpr (std::is_same_v<T, float>) { return Property.IsA<FFloatProperty>(); }
		else if constexpr (std::is_same_v<T, double>) { return Property.IsA<FDoubleProperty>(); }
		else if constexpr (std::is_same_v<T, FString>) { return Property.IsA<FStrProperty>(); }
		else if constexpr (std::is_same_v<T, FName>) { return Property.IsA<FNameProperty>(); }
		else if constexpr (std::is_same_v<T, FText>) { return Property.IsA<FTextProperty>(); }
		else if constexpr (std::is_same_v<T, uint8>)
		{
			const FByteProperty* Byte = CastField<FByteProperty>(&Property);
			return Byte && !Byte->Enum;
		}
		else if constexpr (std::is_enum_v<T>)
		{
			const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property);
			const FByteProperty* Byte = CastField<FByteProperty>(&Property);
			const UEnum* Enum = EnumProperty ? EnumProperty->GetEnum() : (Byte ? Byte->Enum.Get() : nullptr);
			return Enum && Enum == StaticEnum<T>() && Property.GetElementSize() == sizeof(T);
		}
		else
		{
			const FStructProperty* Struct = CastField<FStructProperty>(&Property);
			return Struct && Struct->Struct == TBaseStructure<T>::Get();
		}
	}


	/** The record values of the entity Target names, or null when it has none. */
	static const FInstancedStruct* FindServerValues(const FCrowdyMassEntityTarget& Target);

	/** Whether crowd rows of ActorClass can get server values: it carries a Crowdy Server Object component that reads a record per entity in a mode rows can follow. */
	static bool ClassGetsServerValues(const UClass* ActorClass);

	/** What the Receive Server Values Changed on Entity node's Bind compiles to: runs HandlerFunctionName on HandlerObject for every change of a row of EntityClass. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy Mass|Server Values", meta = (BlueprintInternalUseOnly = "true"))
	static bool CrowdyMassBindServerValuesChangedOnEntity(UObject* HandlerObject, FName HandlerFunctionName, UClass* EntityClass);

	UFUNCTION(BlueprintCallable, Category = "Crowdy Mass|Server Values", meta = (BlueprintInternalUseOnly = "true"))
	static bool CrowdyMassUnbindServerValuesChangedOnEntity(UObject* HandlerObject, FName HandlerFunctionName, UClass* EntityClass);

private:
	DECLARE_FUNCTION(execGetServerValue);
};
