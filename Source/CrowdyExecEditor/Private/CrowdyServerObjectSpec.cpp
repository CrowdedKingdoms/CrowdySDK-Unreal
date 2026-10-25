#include "CrowdyServerObjectSpec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CrowdyExecInternal.h"
#include "CrowdyGameKitStarters.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PropertyBagDetails.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyServerObjectSpecDetail
{
	using CrowdyServerObjectSpec::FFindType;

	bool IsAuthored(const FProperty& Property)
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst);
	}

	/** The flags the property bag editor sets on a variable: shown, read-only, advanced, and the access specifiers. */
	constexpr uint64 BagFlagsMask = CPF_Edit | CPF_EditConst | CPF_AdvancedDisplay | CPF_NativeAccessSpecifiers;

	bool IsStoredValue(const FProperty& Property)
	{
		return !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated);
	}

	FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	FString DescribeJson(const FJsonValue& Json)
	{
		FString Text;
		switch (Json.Type)
		{
		case EJson::Null: return TEXT("null");
		case EJson::String: return FString::Printf(TEXT("the text \"%s\""), *Json.AsString());
		case EJson::Number: return Json.TryGetString(Text) ? FString::Printf(TEXT("the number %s"), *Text) : FString(TEXT("a number"));
		case EJson::Boolean: return Json.AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Array: return TEXT("an array");
		case EJson::Object: return TEXT("an object");
		default: return TEXT("something else");
		}
	}

	UObject* LoadByPath(UClass* Class, const FString& Path)
	{
		if (!FPackageName::IsValidObjectPath(Path))
		{
			return nullptr;
		}
		return StaticLoadObject(Class, nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	TSharedPtr<FJsonValue> ExportSpecial(FProperty* Property, const void* Value);

	TSharedPtr<FJsonValue> ExportBag(const FInstancedPropertyBag& Bag)
	{
		TArray<TSharedPtr<FJsonValue>> Variables;
		const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
		const UEnum* TypeEnum = StaticEnum<EPropertyBagPropertyType>();
		const UEnum* ContainerEnum = StaticEnum<EPropertyBagContainerType>();
		const FJsonObjectConverter::CustomExportCallback Callback = FJsonObjectConverter::CustomExportCallback::CreateStatic(&ExportSpecial);
		for (const FPropertyBagPropertyDesc& Desc : BagStruct ? BagStruct->GetPropertyDescs() : TConstArrayView<FPropertyBagPropertyDesc>())
		{
			const TSharedRef<FJsonObject> Variable = MakeShared<FJsonObject>();
			Variable->SetStringField(TEXT("Name"), Desc.Name.ToString());
			Variable->SetStringField(TEXT("Type"), TypeEnum->GetAuthoredNameStringByValue(static_cast<int64>(Desc.ValueType)));
			if (Desc.ValueTypeObject)
			{
				Variable->SetStringField(TEXT("TypeObject"), Desc.ValueTypeObject->GetPathName());
			}
			if (!Desc.ContainerTypes.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Containers;
				for (const EPropertyBagContainerType Container : Desc.ContainerTypes)
				{
					Containers.Add(MakeShared<FJsonValueString>(ContainerEnum->GetAuthoredNameStringByValue(static_cast<int64>(Container))));
				}
				Variable->SetArrayField(TEXT("Containers"), Containers);
			}
			if (Desc.KeyType != EPropertyBagPropertyType::None)
			{
				Variable->SetStringField(TEXT("KeyType"), TypeEnum->GetAuthoredNameStringByValue(static_cast<int64>(Desc.KeyType)));
			}
			if (Desc.KeyTypeObject)
			{
				Variable->SetStringField(TEXT("KeyTypeObject"), Desc.KeyTypeObject->GetPathName());
			}
			Variable->SetStringField(TEXT("Id"), GuidText(Desc.ID));
			if (!Desc.MetaData.IsEmpty())
			{
				const TSharedRef<FJsonObject> MetaData = MakeShared<FJsonObject>();
				for (const FPropertyBagPropertyDescMetaData& Entry : Desc.MetaData)
				{
					MetaData->SetStringField(Entry.Key.ToString(), Entry.Value);
				}
				Variable->SetObjectField(TEXT("MetaData"), MetaData);
			}
			if (Desc.PropertyFlags != static_cast<uint64>(CPF_Edit))
			{
				Variable->SetStringField(TEXT("PropertyFlags"), LexToString(Desc.PropertyFlags));
			}
			if (Desc.CachedProperty)
			{
				const void* Address = Bag.GetValue().GetMemory() + Desc.CachedProperty->GetOffset_ForInternal();
				TSharedPtr<FJsonValue> Default = FJsonObjectConverter::UPropertyToJsonValue(const_cast<FProperty*>(Desc.CachedProperty), Address, 0, 0, &Callback, nullptr,
					EJsonObjectConversionFlags::SkipStandardizeCase);
				if (!Default.IsValid())
				{
					Default = MakeShared<FJsonValueNull>();
				}
				Variable->SetField(TEXT("Default"), Default);
			}
			Variables.Add(MakeShared<FJsonValueObject>(Variable));
		}
		return MakeShared<FJsonValueArray>(Variables);
	}

	/** What the engine's converter would write badly: 64-bit integers, GUIDs, object references, Lists. */
	TSharedPtr<FJsonValue> ExportSpecial(FProperty* Property, const void* Value)
	{
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FInstancedPropertyBag::StaticStruct())
			{
				return ExportBag(*static_cast<const FInstancedPropertyBag*>(Value));
			}
			if (StructProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				return MakeShared<FJsonValueString>(GuidText(*static_cast<const FGuid*>(Value)));
			}
			return nullptr;
		}
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
			if (!Object)
			{
				return MakeShared<FJsonValueNull>();
			}
			const UCrowdyServerObjectDefinition* Type = Cast<UCrowdyServerObjectDefinition>(Object);
			return MakeShared<FJsonValueString>(Type ? Type->TypeName : Object->GetPathName());
		}
		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr Soft = SoftProperty->GetPropertyValue(Value);
			if (Soft.IsNull())
			{
				return MakeShared<FJsonValueNull>();
			}
			return MakeShared<FJsonValueString>(Soft.ToSoftObjectPath().ToString());
		}
		if (const FInt64Property* Signed = CastField<FInt64Property>(Property))
		{
			return MakeShared<FJsonValueString>(LexToString(Signed->GetPropertyValue(Value)));
		}
		if (const FUInt64Property* Unsigned = CastField<FUInt64Property>(Property))
		{
			return MakeShared<FJsonValueString>(LexToString(Unsigned->GetPropertyValue(Value)));
		}
		return nullptr;
	}

	/** The smallest and largest whole number a numeric property holds; a 64-bit unsigned one is held to what an int64 holds. */
	void GetIntBounds(const FNumericProperty& Property, int64& OutMin, int64& OutMax)
	{
		const int32 Bits = Property.GetElementSize() * 8;
		const bool bUnsigned = Property.IsA<FByteProperty>() || Property.IsA<FUInt16Property>() || Property.IsA<FUInt32Property>() || Property.IsA<FUInt64Property>();
		if (bUnsigned)
		{
			OutMin = 0;
			OutMax = Bits >= 64 ? MAX_int64 : (static_cast<int64>(1) << Bits) - 1;
			return;
		}
		OutMin = Bits >= 64 ? MIN_int64 : -(static_cast<int64>(1) << (Bits - 1));
		OutMax = Bits >= 64 ? MAX_int64 : (static_cast<int64>(1) << (Bits - 1)) - 1;
	}

	struct FReader
	{
		FReader(const UCrowdyServerObjectDefinition& InDefinition, FFindType InFindType, TArray<FString>& InErrors)
			: Definition(InDefinition)
			, FindType(InFindType)
			, Errors(InErrors)
		{
		}

		const UCrowdyServerObjectDefinition& Definition;
		FFindType FindType;
		TArray<FString>& Errors;
		/** Whether a struct's settings are the ones authored in the editor; a List's default values take every stored field. */
		bool bAuthoredOnly = true;

		void Add(const FString& Path, const FString& Problem)
		{
			Errors.Add(Path.IsEmpty() ? Problem : Path + TEXT(" ") + Problem);
		}

		static FString Child(const FString& Path, const FString& Key)
		{
			return Path.IsEmpty() ? Key : Path + TEXT(".") + Key;
		}

		static FString Indexed(const FString& Path, int32 Index)
		{
			return FString::Printf(TEXT("%s[%d]"), *Path, Index);
		}

		bool IsReadable(const FProperty& Property) const
		{
			return bAuthoredOnly ? IsAuthored(Property) : IsStoredValue(Property);
		}

		FProperty* FindProperty(const UStruct& Struct, const FString& Key, const FString& Path)
		{
			FString Spelled;
			TArray<FString> Known;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				const FString Name = It->GetAuthoredName();
				if (!IsReadable(**It))
				{
					if (Name.Equals(Key, ESearchCase::CaseSensitive))
					{
						Add(Path, TEXT("is filled in by the bake, not authored; leave it out"));
						return nullptr;
					}
					continue;
				}
				if (Name.Equals(Key, ESearchCase::CaseSensitive))
				{
					return *It;
				}
				Spelled = Name.Equals(Key, ESearchCase::IgnoreCase) ? Name : Spelled;
				Known.Add(Name);
			}
			Add(Path, Spelled.IsEmpty() ? FString::Printf(TEXT("is not a setting; the settings are %s"), *FString::Join(Known, TEXT(", ")))
				: FString::Printf(TEXT("is not a setting; it is spelled %s (capitals matter)"), *Spelled));
			return nullptr;
		}

		void ReadFields(const decltype(FJsonObject::Values)& Fields, const UStruct& Struct, void* Container, const FString& Path)
		{
			for (const auto& Field : Fields)
			{
				const FString Key(*Field.Key);
				const FString FieldPath = Child(Path, Key);
				FProperty* Property = FindProperty(Struct, Key, FieldPath);
				if (Property)
				{
					ReadValue(*Field.Value, *Property, Property->ContainerPtrToValuePtr<void>(Container), FieldPath, false);
				}
			}
		}

		void ReadValue(const FJsonValue& Json, FProperty& Property, void* Ptr, const FString& Path, bool bTextOk)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
			{
				ReadEnum(Json, *EnumProperty->GetEnum(), *EnumProperty->GetUnderlyingProperty(), Ptr, Path);
			}
			else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(&Property))
			{
				ReadNumber(Json, *NumericProperty, Ptr, Path, bTextOk);
			}
			else if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(&Property))
			{
				ReadBool(Json, *BoolProperty, Ptr, Path);
			}
			else if (Property.IsA<FStrProperty>() || Property.IsA<FNameProperty>() || Property.IsA<FTextProperty>())
			{
				ReadText(Json, Property, Ptr, Path);
			}
			else if (FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
			{
				ReadStruct(Json, *StructProperty, Ptr, Path);
			}
			else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property))
			{
				ReadArray(Json, *ArrayProperty, Ptr, Path);
			}
			else if (FSetProperty* SetProperty = CastField<FSetProperty>(&Property))
			{
				ReadSet(Json, *SetProperty, Ptr, Path);
			}
			else if (FMapProperty* MapProperty = CastField<FMapProperty>(&Property))
			{
				ReadMap(Json, *MapProperty, Ptr, Path);
			}
			else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(&Property))
			{
				ReadObject(Json, *ObjectProperty, Ptr, Path);
			}
			else if (FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(&Property))
			{
				ReadSoft(Json, *SoftProperty, Ptr, Path);
			}
			else
			{
				Add(Path, TEXT("is a kind of setting a spec cannot hold"));
			}
		}

		bool ReadEnumValue(const FJsonValue& Json, const UEnum& Enum, const FString& Path, int64& OutValue)
		{
			FString Name;
			const bool bText = Json.Type == EJson::String && Json.TryGetString(Name);
			TArray<FString> Names;
			const int32 Count = Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Names.Add(Enum.GetAuthoredNameStringByIndex(Index));
				if (bText && Names.Last().Equals(Name, ESearchCase::CaseSensitive))
				{
					OutValue = Enum.GetValueByIndex(Index);
					return true;
				}
			}
			const FString* Spelled = bText ? Names.FindByPredicate([&Name](const FString& Each) { return Each.Equals(Name, ESearchCase::IgnoreCase); }) : nullptr;
			Add(Path, Spelled ? FString::Printf(TEXT("is \"%s\", which is spelled %s (capitals matter)"), *Name, **Spelled)
				: FString::Printf(TEXT("needs one of %s but got %s"), *FString::Join(Names, TEXT(", ")), *DescribeJson(Json)));
			return false;
		}

		void ReadEnum(const FJsonValue& Json, const UEnum& Enum, const FNumericProperty& Underlying, void* Ptr, const FString& Path)
		{
			int64 Value = 0;
			if (ReadEnumValue(Json, Enum, Path, Value))
			{
				Underlying.SetIntPropertyValue(Ptr, Value);
			}
		}

		/** A whole number from JSON: a number, or for a 64-bit integer or map key also its text, which keeps digits a double cannot. */
		bool ReadWhole(const FJsonValue& Json, bool bTextOk, const FString& Path, int64& OutValue)
		{
			FString Text;
			const bool bNumber = Json.Type == EJson::Number;
			if ((bNumber || (bTextOk && Json.Type == EJson::String)) && Json.TryGetString(Text))
			{
				if (CrowdyExec::ParseRangeWhole(Text, OutValue))
				{
					return true;
				}
				const double Real = FCString::Atod(*Text);
				if (bNumber && FMath::IsFinite(Real) && Real == FMath::FloorToDouble(Real) && FMath::Abs(Real) < 9.0e15)
				{
					OutValue = static_cast<int64>(Real);
					return true;
				}
			}
			Add(Path, TEXT("needs a whole number but got ") + DescribeJson(Json));
			return false;
		}

		bool IsInRange(const FProperty& Property, double Value, const FString& Path)
		{
			const bool bHasMin = Property.HasMetaData(TEXT("ClampMin"));
			const bool bHasMax = Property.HasMetaData(TEXT("ClampMax"));
			const double Min = bHasMin ? FCString::Atod(*Property.GetMetaData(TEXT("ClampMin"))) : 0.0;
			const double Max = bHasMax ? FCString::Atod(*Property.GetMetaData(TEXT("ClampMax"))) : 0.0;
			// A float holds 0.01f, not 0.01, so a float is judged against its bounds as a float.
			const auto AsHeld = [&Property](double Real) { return Property.IsA<FFloatProperty>() ? static_cast<double>(static_cast<float>(Real)) : Real; };
			if ((!bHasMin || AsHeld(Value) >= AsHeld(Min)) && (!bHasMax || AsHeld(Value) <= AsHeld(Max)))
			{
				return true;
			}
			const FString MinText = FString::SanitizeFloat(Min, 0);
			const FString MaxText = FString::SanitizeFloat(Max, 0);
			const FString Range = bHasMin && bHasMax ? FString::Printf(TEXT("%s to %s"), *MinText, *MaxText) : bHasMin ? TEXT("at least ") + MinText : TEXT("at most ") + MaxText;
			Add(Path, FString::Printf(TEXT("is %s, outside its range of %s"), *FString::SanitizeFloat(Value, 0), *Range));
			return false;
		}

		void ReadNumber(const FJsonValue& Json, const FNumericProperty& Property, void* Ptr, const FString& Path, bool bTextOk)
		{
			if (const UEnum* Enum = Property.GetIntPropertyEnum())
			{
				ReadEnum(Json, *Enum, Property, Ptr, Path);
				return;
			}
			if (Property.IsFloatingPoint())
			{
				double Real = 0.0;
				if (Json.Type != EJson::Number || !Json.TryGetNumber(Real))
				{
					Add(Path, TEXT("needs a number but got ") + DescribeJson(Json));
				}
				else if (Property.IsA<FFloatProperty>() && FMath::Abs(Real) > static_cast<double>(MAX_flt))
				{
					Add(Path, FString::Printf(TEXT("is %s, too large for a float"), *FString::SanitizeFloat(Real, 0)));
				}
				else if (IsInRange(Property, Real, Path))
				{
					Property.SetFloatingPointPropertyValue(Ptr, Real);
				}
				return;
			}
			int64 Whole = 0;
			int64 Min = 0;
			int64 Max = 0;
			GetIntBounds(Property, Min, Max);
			if (!ReadWhole(Json, bTextOk || Property.GetElementSize() == 8, Path, Whole))
			{
				return;
			}
			if (Whole < Min || Whole > Max)
			{
				Add(Path, FString::Printf(TEXT("is %lld, outside the %lld to %lld this kind of number holds"), Whole, Min, Max));
			}
			else if (IsInRange(Property, static_cast<double>(Whole), Path))
			{
				Property.SetIntPropertyValue(Ptr, Whole);
			}
		}

		void ReadBool(const FJsonValue& Json, const FBoolProperty& Property, void* Ptr, const FString& Path)
		{
			if (Json.Type != EJson::Boolean)
			{
				Add(Path, TEXT("needs true or false but got ") + DescribeJson(Json));
				return;
			}
			Property.SetPropertyValue(Ptr, Json.AsBool());
		}

		void ReadText(const FJsonValue& Json, FProperty& Property, void* Ptr, const FString& Path)
		{
			FString Text;
			if (Json.Type != EJson::String || !Json.TryGetString(Text))
			{
				Add(Path, TEXT("needs text but got ") + DescribeJson(Json));
				return;
			}
			const bool bMultiLine = Property.HasMetaData(TEXT("MultiLine"));
			const int32 Control = Property.IsA<FTextProperty>() ? INDEX_NONE : Text.FindLastCharByPredicate([bMultiLine](TCHAR Char)
			{
				return Char == 0x7F || (Char < 0x20 && !(bMultiLine && (Char == TEXT('\n') || Char == TEXT('\r') || Char == TEXT('\t'))));
			});
			if (Control != INDEX_NONE)
			{
				Add(Path, FString::Printf(TEXT("holds a control character (code %d) at position %d; leave out line breaks and other control characters%s"), static_cast<int32>(Text[Control]), Control,
					bMultiLine ? TEXT(" besides line breaks and tabs") : TEXT("")));
				return;
			}
			if (FNameProperty* NameProperty = CastField<FNameProperty>(&Property))
			{
				if (Text.Len() >= NAME_SIZE)
				{
					Add(Path, FString::Printf(TEXT("is a name of %d characters; names hold fewer than %d"), Text.Len(), NAME_SIZE));
					return;
				}
				NameProperty->SetPropertyValue(Ptr, FName(*Text));
			}
			else if (FStrProperty* StringProperty = CastField<FStrProperty>(&Property))
			{
				StringProperty->SetPropertyValue(Ptr, Text);
			}
			else if (FTextProperty* TextProperty = CastField<FTextProperty>(&Property))
			{
				TextProperty->SetPropertyValue(Ptr, FText::FromString(Text));
			}
		}

		void ReadStruct(const FJsonValue& Json, const FStructProperty& Property, void* Ptr, const FString& Path)
		{
			if (Property.Struct == TBaseStructure<FGuid>::Get())
			{
				FString Text;
				if (Json.Type != EJson::String || !Json.TryGetString(Text) || !FGuid::Parse(Text, *static_cast<FGuid*>(Ptr)))
				{
					Add(Path, TEXT("needs a GUID such as 6f9619ff-8b86-d011-b42d-00c04fc964ff but got ") + DescribeJson(Json));
				}
				return;
			}
			if (Property.Struct == FInstancedPropertyBag::StaticStruct())
			{
				ReadBag(Json, *static_cast<FInstancedPropertyBag*>(Ptr), Path);
				return;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Json.Type != EJson::Object || !Json.TryGetObject(Object))
			{
				Add(Path, TEXT("needs an object but got ") + DescribeJson(Json));
				return;
			}
			ReadFields((*Object)->Values, *Property.Struct, Ptr, Path);
		}

		void ReadArray(const FJsonValue& Json, const FArrayProperty& Property, void* Ptr, const FString& Path)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Json.Type != EJson::Array || !Json.TryGetArray(Items))
			{
				Add(Path, TEXT("needs an array but got ") + DescribeJson(Json));
				return;
			}
			FScriptArrayHelper Helper(&Property, Ptr);
			Helper.EmptyAndAddValues(Items->Num());
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				ReadValue(*(*Items)[Index], *Property.Inner, Helper.GetRawPtr(Index), Indexed(Path, Index), false);
			}
		}

		void ReadSet(const FJsonValue& Json, const FSetProperty& Property, void* Ptr, const FString& Path)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Json.Type != EJson::Array || !Json.TryGetArray(Items))
			{
				Add(Path, TEXT("needs an array but got ") + DescribeJson(Json));
				return;
			}
			FScriptSetHelper Helper(&Property, Ptr);
			Helper.EmptyElements(Items->Num());
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
				ReadValue(*(*Items)[Index], *Property.ElementProp, Helper.GetElementPtr(NewIndex), Indexed(Path, Index), false);
			}
			Helper.Rehash();
		}

		void ReadMap(const FJsonValue& Json, const FMapProperty& Property, void* Ptr, const FString& Path)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Json.Type != EJson::Object || !Json.TryGetObject(Object))
			{
				Add(Path, TEXT("needs an object but got ") + DescribeJson(Json));
				return;
			}
			FScriptMapHelper Helper(&Property, Ptr);
			Helper.EmptyValues((*Object)->Values.Num());
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Object)->Values)
			{
				const int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
				const FString EntryPath = FString::Printf(TEXT("%s[%s]"), *Path, *Entry.Key);
				const TSharedRef<FJsonValueString> Key = MakeShared<FJsonValueString>(Entry.Key);
				ReadValue(*Key, *Property.KeyProp, Helper.GetKeyPtr(NewIndex), EntryPath, true);
				ReadValue(*Entry.Value, *Property.ValueProp, Helper.GetValuePtr(NewIndex), EntryPath, false);
			}
			Helper.Rehash();
		}

		void ReadObject(const FJsonValue& Json, const FObjectProperty& Property, void* Ptr, const FString& Path)
		{
			FString Name;
			if (Json.IsNull())
			{
				Property.SetObjectPropertyValue(Ptr, nullptr);
				return;
			}
			if (Json.Type != EJson::String || !Json.TryGetString(Name))
			{
				Add(Path, TEXT("needs a name or path, or null, but got ") + DescribeJson(Json));
				return;
			}
			const bool bType = Property.PropertyClass->IsChildOf(UCrowdyServerObjectDefinition::StaticClass());
			UObject* Object = bType ? static_cast<UObject*>(FindType(Name)) : LoadByPath(Property.PropertyClass, Name);
			if (!Object)
			{
				Add(Path, bType ? FString::Printf(TEXT("is \"%s\", which is not a Server Object type of the project"), *Name)
					: FString::Printf(TEXT("is \"%s\", which is not a %s that can be loaded"), *Name, *Property.PropertyClass->GetName()));
				return;
			}
			Property.SetObjectPropertyValue(Ptr, Object);
		}

		void ReadSoft(const FJsonValue& Json, const FSoftObjectProperty& Property, void* Ptr, const FString& Path)
		{
			FString Name;
			if (Json.IsNull())
			{
				Property.SetPropertyValue(Ptr, FSoftObjectPtr());
				return;
			}
			if (Json.Type != EJson::String || !Json.TryGetString(Name))
			{
				Add(Path, TEXT("needs a path, or null, but got ") + DescribeJson(Json));
				return;
			}
			const FSoftObjectPath SoftPath(Name);
			if (!Name.IsEmpty() && !SoftPath.IsValid())
			{
				Add(Path, FString::Printf(TEXT("is \"%s\", which is not an object path such as /Script/Engine.Actor"), *Name));
				return;
			}
			Property.SetPropertyValue(Ptr, FSoftObjectPtr(SoftPath));
		}

		UObject* LoadTypeObject(const FJsonValue& Json, UClass* Class, const FString& Path)
		{
			FString Name;
			if (Json.Type != EJson::String || !Json.TryGetString(Name))
			{
				Add(Path, TEXT("needs a path but got ") + DescribeJson(Json));
				return nullptr;
			}
			UObject* Object = LoadByPath(Class, Name);
			if (!Object)
			{
				Add(Path, FString::Printf(TEXT("is \"%s\", which is not a %s that can be loaded"), *Name, *Class->GetName()));
			}
			return Object;
		}

		static bool NeedsTypeObject(EPropertyBagPropertyType Type, UClass*& OutClass)
		{
			switch (Type)
			{
			case EPropertyBagPropertyType::Enum: OutClass = UEnum::StaticClass(); return true;
			case EPropertyBagPropertyType::Struct: OutClass = UScriptStruct::StaticClass(); return true;
			case EPropertyBagPropertyType::Object:
			case EPropertyBagPropertyType::SoftObject:
			case EPropertyBagPropertyType::Class:
			case EPropertyBagPropertyType::SoftClass: OutClass = UClass::StaticClass(); return true;
			default: return false;
			}
		}

		/** Reads a bag type name; false, with a problem added, for None, Count or a name the enum lacks. */
		bool ReadBagType(const FJsonValue& Json, const FString& Path, EPropertyBagPropertyType& OutType)
		{
			int64 Value = 0;
			if (!ReadEnumValue(Json, *StaticEnum<EPropertyBagPropertyType>(), Path, Value))
			{
				return false;
			}
			if (Value == static_cast<int64>(EPropertyBagPropertyType::None) || Value >= static_cast<int64>(EPropertyBagPropertyType::Count))
			{
				Add(Path, TEXT("needs a type such as Int32, String or Struct"));
				return false;
			}
			OutType = static_cast<EPropertyBagPropertyType>(Value);
			return true;
		}

		/** The type and its object, as TypeKey and ObjectKey name them in Variable, which are both needed or both absent by the type. */
		bool ReadBagTypeAndObject(const FJsonObject& Variable, const TCHAR* TypeKey, const TCHAR* ObjectKey, const FString& Path, EPropertyBagPropertyType& OutType, TObjectPtr<const UObject>& OutObject)
		{
			const FString TypePath = Child(Path, TypeKey);
			const TSharedPtr<FJsonValue>* Type = Variable.Values.Find(TypeKey);
			const TSharedPtr<FJsonValue>* Object = Variable.Values.Find(ObjectKey);
			if (!Type)
			{
				Add(TypePath, TEXT("is missing"));
				return false;
			}
			if (!ReadBagType(**Type, TypePath, OutType))
			{
				return false;
			}
			UClass* ObjectClass = nullptr;
			const bool bNeeds = NeedsTypeObject(OutType, ObjectClass);
			if (!bNeeds && Object)
			{
				Add(Child(Path, ObjectKey), TEXT("is only for Enum, Struct, Object, SoftObject, Class and SoftClass variables"));
				return false;
			}
			if (bNeeds && !Object)
			{
				Add(Child(Path, ObjectKey), FString::Printf(TEXT("is missing; a %s variable needs the path of its %s"), *StaticEnum<EPropertyBagPropertyType>()->GetAuthoredNameStringByValue(static_cast<int64>(OutType)),
					*ObjectClass->GetName()));
				return false;
			}
			OutObject = bNeeds ? LoadTypeObject(**Object, ObjectClass, Child(Path, ObjectKey)) : nullptr;
			return !bNeeds || OutObject;
		}

		bool ReadContainers(const FJsonObject& Variable, const FString& Path, FPropertyBagContainerTypes& OutContainers)
		{
			const TSharedPtr<FJsonValue>* Json = Variable.Values.Find(TEXT("Containers"));
			if (!Json)
			{
				return true;
			}
			const FString ContainersPath = Child(Path, TEXT("Containers"));
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if ((*Json)->Type != EJson::Array || !(*Json)->TryGetArray(Items))
			{
				Add(ContainersPath, TEXT("needs an array of Array, Set or Map but got ") + DescribeJson(**Json));
				return false;
			}
			if (Items->Num() > 2)
			{
				Add(ContainersPath, TEXT("holds at most two containers"));
				return false;
			}
			bool bOk = true;
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				int64 Value = 0;
				const bool bRead = ReadEnumValue(*(*Items)[Index], *StaticEnum<EPropertyBagContainerType>(), Indexed(ContainersPath, Index), Value);
				const bool bContainer = bRead && Value != static_cast<int64>(EPropertyBagContainerType::None) && Value < static_cast<int64>(EPropertyBagContainerType::Count);
				if (bRead && !bContainer)
				{
					Add(Indexed(ContainersPath, Index), TEXT("needs Array, Set or Map"));
				}
				bOk = bOk && bContainer;
				if (bContainer)
				{
					OutContainers.Add(static_cast<EPropertyBagContainerType>(Value));
				}
			}
			return bOk;
		}

		bool ReadVariableId(const FJsonObject& Variable, const FString& Path, FGuid& OutId)
		{
			const TSharedPtr<FJsonValue>* Json = Variable.Values.Find(TEXT("Id"));
			FString Text;
			if (!Json)
			{
				return true;
			}
			if ((*Json)->Type != EJson::String || !(*Json)->TryGetString(Text) || !FGuid::Parse(Text, OutId) || !OutId.IsValid())
			{
				Add(Child(Path, TEXT("Id")), TEXT("needs a GUID such as 6f9619ff-8b86-d011-b42d-00c04fc964ff but got ") + DescribeJson(**Json));
				return false;
			}
			return true;
		}

		bool ReadMetaData(const FJsonObject& Variable, const FString& Path, FPropertyBagPropertyDesc& Desc)
		{
			const TSharedPtr<FJsonValue>* Json = Variable.Values.Find(TEXT("MetaData"));
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Json)
			{
				return true;
			}
			const FString MetaPath = Child(Path, TEXT("MetaData"));
			if ((*Json)->Type != EJson::Object || !(*Json)->TryGetObject(Object))
			{
				Add(MetaPath, TEXT("needs an object but got ") + DescribeJson(**Json));
				return false;
			}
			bool bOk = true;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Object)->Values)
			{
				FString Text;
				const bool bMin = Entry.Key.Equals(CrowdyServerValueRange::MinKey, ESearchCase::CaseSensitive);
				const bool bMax = Entry.Key.Equals(CrowdyServerValueRange::MaxKey, ESearchCase::CaseSensitive);
				const TCHAR* const Spelled = Entry.Key.Equals(CrowdyServerValueRange::MinKey, ESearchCase::IgnoreCase) ? CrowdyServerValueRange::MinKey
					: Entry.Key.Equals(CrowdyServerValueRange::MaxKey, ESearchCase::IgnoreCase) ? CrowdyServerValueRange::MaxKey : nullptr;
				if (Spelled && !bMin && !bMax)
				{
					Add(Child(MetaPath, Entry.Key), FString::Printf(TEXT("is spelled %s (capitals matter)"), Spelled));
					bOk = false;
				}
				else if (!Entry.Value->TryGetString(Text))
				{
					Add(Child(MetaPath, Entry.Key), TEXT("needs text or a number but got ") + DescribeJson(*Entry.Value));
					bOk = false;
				}
				else if (!bMin && !bMax)
				{
					Desc.SetMetaData(FName(*Entry.Key), Text);
				}
				else if (!CrowdyServerValueRange::SetBound(Desc, FName(bMin ? CrowdyServerValueRange::MinKey : CrowdyServerValueRange::MaxKey), Text))
				{
					Add(Child(MetaPath, Entry.Key), FString::Printf(TEXT("is \"%s\"; a value range is a plain number (digits and one point, no exponent; whole digits for a whole-number variable) on a variable holding one number"), *Text));
					bOk = false;
				}
			}
			return bOk;
		}

		bool ReadPropertyFlags(const FJsonObject& Variable, const FString& Path, uint64& OutFlags)
		{
			const TSharedPtr<FJsonValue>* Json = Variable.Values.Find(TEXT("PropertyFlags"));
			int64 Whole = 0;
			if (!Json)
			{
				return true;
			}
			const FString FlagsPath = Child(Path, TEXT("PropertyFlags"));
			if (!ReadWhole(**Json, true, FlagsPath, Whole) || Whole < 0)
			{
				return false;
			}
			if (static_cast<uint64>(Whole) & ~BagFlagsMask)
			{
				Add(FlagsPath, FString::Printf(TEXT("is %lld, which sets flags the Server Object editor never does; a variable's flags are made of the bits in %llu"), Whole, BagFlagsMask));
				return false;
			}
			OutFlags = static_cast<uint64>(Whole);
			return true;
		}

		bool ReadVariableName(const FJsonObject& Variable, const FString& Path, TConstArrayView<FPropertyBagPropertyDesc> Earlier, FName& OutName)
		{
			const TSharedPtr<FJsonValue>* Json = Variable.Values.Find(TEXT("Name"));
			FString Text;
			const FString NamePath = Child(Path, TEXT("Name"));
			if (!Json || (*Json)->Type != EJson::String || !(*Json)->TryGetString(Text))
			{
				Add(NamePath, Json ? TEXT("needs text but got ") + DescribeJson(**Json) : FString(TEXT("is missing")));
				return false;
			}
			if (!FInstancedPropertyBag::IsPropertyNameValid(Text))
			{
				Add(NamePath, FString::Printf(TEXT("is \"%s\", which is not a valid variable name"), *Text));
				return false;
			}
			OutName = FName(*Text);
			if (Earlier.ContainsByPredicate([OutName](const FPropertyBagPropertyDesc& Each) { return Each.Name == OutName; }))
			{
				Add(NamePath, FString::Printf(TEXT("is \"%s\", which an earlier variable already has"), *Text));
				return false;
			}
			return true;
		}

		/** Whether the Server Object editor would offer this variable's type. */
		bool IsTypeOffered(const FPropertyBagPropertyDesc& Desc, const FString& Path)
		{
			bool bOffered = UE::StructUtils::CanCreateValidGraphPinTypeForPropertyDesc(Desc);
			if (bOffered)
			{
				const FEdGraphPinType Pin = UE::StructUtils::GetPropertyDescAsPin(Desc);
				bOffered = Definition.IsServerValueTypeAccepted(Pin, false) && Definition.IsServerValueTypeAccepted(Pin, true);
			}
			if (!bOffered)
			{
				Add(Path, TEXT("is a type the server does not take: it holds bool, byte, int, int64, real, name, string, struct, enum, soft object and soft class values, alone or in one array, set or map"));
			}
			return bOffered;
		}

		bool ReadVariable(const FJsonValue& Json, const FString& Path, TConstArrayView<FPropertyBagPropertyDesc> Earlier, FPropertyBagPropertyDesc& OutDesc, const FJsonValue*& OutDefault)
		{
			static const TCHAR* const Keys[] = {TEXT("Name"), TEXT("Type"), TEXT("TypeObject"), TEXT("Containers"), TEXT("KeyType"), TEXT("KeyTypeObject"), TEXT("Id"), TEXT("MetaData"),
				TEXT("PropertyFlags"), TEXT("Default")};
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Json.Type != EJson::Object || !Json.TryGetObject(Object))
			{
				Add(Path, TEXT("needs an object but got ") + DescribeJson(Json));
				return false;
			}
			const FJsonObject& Variable = **Object;
			const int32 ErrorsBefore = Errors.Num();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Variable.Values)
			{
				if (!MakeArrayView(Keys).ContainsByPredicate([&Field](const TCHAR* Key) { return Field.Key.Equals(Key, ESearchCase::CaseSensitive); }))
				{
					Add(Child(Path, Field.Key), TEXT("is not a variable setting; the settings are Name, Type, TypeObject, Containers, KeyType, KeyTypeObject, Id, MetaData, PropertyFlags, Default"));
				}
			}
			FName Name;
			EPropertyBagPropertyType Type = EPropertyBagPropertyType::None;
			EPropertyBagPropertyType KeyType = EPropertyBagPropertyType::None;
			TObjectPtr<const UObject> TypeObject;
			TObjectPtr<const UObject> KeyTypeObject;
			FPropertyBagContainerTypes Containers;
			FGuid Id;
			uint64 Flags = static_cast<uint64>(CPF_Edit);
			ReadVariableName(Variable, Path, Earlier, Name);
			ReadBagTypeAndObject(Variable, TEXT("Type"), TEXT("TypeObject"), Path, Type, TypeObject);
			ReadContainers(Variable, Path, Containers);
			ReadVariableId(Variable, Path, Id);
			ReadPropertyFlags(Variable, Path, Flags);
			if (Id.IsValid() && Earlier.ContainsByPredicate([&Id](const FPropertyBagPropertyDesc& Each) { return Each.ID == Id; }))
			{
				Add(Child(Path, TEXT("Id")), TEXT("is the Id of an earlier variable"));
			}
			if (Containers.GetFirstContainerType() == EPropertyBagContainerType::Map)
			{
				ReadBagTypeAndObject(Variable, TEXT("KeyType"), TEXT("KeyTypeObject"), Path, KeyType, KeyTypeObject);
			}
			else if (Variable.Values.Contains(TEXT("KeyType")) || Variable.Values.Contains(TEXT("KeyTypeObject")))
			{
				Add(Child(Path, TEXT("KeyType")), TEXT("is only for a variable whose first container is Map"));
			}
			if (Errors.Num() != ErrorsBefore)
			{
				return false;
			}
			OutDesc = FPropertyBagPropertyDesc(Name, Containers, Type, TypeObject, static_cast<EPropertyFlags>(Flags), KeyType, KeyTypeObject);
			OutDesc.ID = Id;
			if (!ReadMetaData(Variable, Path, OutDesc) || !IsTypeOffered(OutDesc, Child(Path, TEXT("Type"))))
			{
				return false;
			}
			const TSharedPtr<FJsonValue>* Default = Variable.Values.Find(TEXT("Default"));
			OutDefault = Default ? Default->Get() : nullptr;
			return true;
		}

		void ReadBag(const FJsonValue& Json, FInstancedPropertyBag& Bag, const FString& Path)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Json.Type != EJson::Array || !Json.TryGetArray(Items))
			{
				Add(Path, TEXT("needs an array of variables but got ") + DescribeJson(Json));
				return;
			}
			TArray<FPropertyBagPropertyDesc> Descs;
			TArray<const FJsonValue*> Defaults;
			TArray<FString> Paths;
			bool bOk = true;
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const FJsonValue* Default = nullptr;
				FPropertyBagPropertyDesc Desc;
				if (!ReadVariable(*(*Items)[Index], Indexed(Path, Index), Descs, Desc, Default))
				{
					bOk = false;
					continue;
				}
				Descs.Add(Desc);
				Defaults.Add(Default);
				Paths.Add(Indexed(Path, Index));
			}
			Bag.Reset();
			if (!bOk || Descs.IsEmpty())
			{
				return;
			}
			if (Bag.AddProperties(Descs) != EPropertyBagAlterationResult::Success)
			{
				Add(Path, TEXT("could not be built; check the variable names and key types"));
				return;
			}
			TGuardValue<bool> Stored(bAuthoredOnly, false);
			for (int32 Index = 0; Index < Descs.Num(); ++Index)
			{
				const FPropertyBagPropertyDesc* Built = Defaults[Index] ? Bag.FindPropertyDescByName(Descs[Index].Name) : nullptr;
				if (Built && Built->CachedProperty)
				{
					uint8* Address = Bag.GetMutableValue().GetMemory() + Built->CachedProperty->GetOffset_ForInternal();
					ReadValue(*Defaults[Index], *const_cast<FProperty*>(Built->CachedProperty), Address, Child(Paths[Index], TEXT("Default")), false);
				}
			}
		}
	};

	void CheckSelfReferences(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (Definition.TypeName.IsEmpty())
		{
			return;
		}
		const auto Check = [&Definition, &Errors](const TArray<TObjectPtr<UCrowdyServerObjectDefinition>>& Types, const TCHAR* Key)
		{
			for (int32 Index = 0; Index < Types.Num(); ++Index)
			{
				if (Types[Index] && Types[Index]->TypeName.Equals(Definition.TypeName, ESearchCase::IgnoreCase))
				{
					Errors.Add(FString::Printf(TEXT("%s[%d] names this spec's own TypeName, %s; a type cannot refer to itself"), Key, Index, *Definition.TypeName));
				}
			}
		};
		Check(Definition.CanCall, TEXT("CanCall"));
		Check(Definition.ListenTo, TEXT("ListenTo"));
	}

	void CheckLogicFile(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		const FString& Path = Definition.LogicFile.FilePath;
		if (Path.IsEmpty())
		{
			return;
		}
		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const bool bInside = FPaths::IsRelative(Path) && FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Path)), ProjectDir);
		if (!bInside || !Path.EndsWith(TEXT(".rs"), ESearchCase::IgnoreCase))
		{
			Errors.Add(FString::Printf(TEXT("LogicFile.FilePath is \"%s\"; it must be a .rs file inside the project, written relative to the project folder"), *Path));
		}
	}

	void CheckKitActors(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		for (const TPair<FName, TSoftClassPtr<AActor>>& Entry : Definition.KitActors)
		{
			if (Entry.Value.IsNull())
			{
				continue;
			}
			const FString Path = Entry.Value.ToString();
			const UClass* Class = Cast<UClass>(LoadByPath(UClass::StaticClass(), Path));
			if (!Class || !Class->IsChildOf(AActor::StaticClass()))
			{
				Errors.Add(FString::Printf(TEXT("KitActors[%s] is \"%s\", which is not an Actor class that can be loaded"), *Entry.Key.ToString(), *Path));
			}
		}
	}

	void CheckKitConfig(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		const FString Config = Definition.KitConfig.TrimStartAndEnd();
		if (!Definition.IsGameKitStarter() || Config.IsEmpty())
		{
			return;
		}
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Config), Object) || !Object)
		{
			Errors.Add(FString::Printf(TEXT("KitConfig is not a JSON object: %s"), *Config));
			return;
		}
		const FString Problem = CrowdyGameKitStarters::FindConfigProblem(Definition.TypeName, Config);
		if (!Problem.IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("KitConfig has %s"), *Problem));
		}
	}

	UCrowdyServerObjectDefinition* FindIn(TConstArrayView<UCrowdyServerObjectDefinition*> Project, const FString& Name)
	{
		UCrowdyServerObjectDefinition* const* Found = Project.FindByPredicate([&Name](const UCrowdyServerObjectDefinition* Each)
		{
			return Each && Each->TypeName.Equals(Name, ESearchCase::CaseSensitive);
		});
		return Found ? *Found : nullptr;
	}

	/** Takes back a new asset that did not bake or save, so no package or registry entry outlives the failure. */
	void DiscardNewAsset(UCrowdyServerObjectDefinition& Asset, bool bAnnounced)
	{
		UPackage* Package = Asset.GetOutermost();
		if (bAnnounced)
		{
			FAssetRegistryModule::AssetDeleted(&Asset);
		}
		Asset.ClearFlags(RF_Standalone | RF_Public);
		Asset.MarkAsGarbage();
		Package->MarkAsGarbage();
	}

	TStrongObjectPtr<UCrowdyServerObjectDefinition> NewCandidate()
	{
		return TStrongObjectPtr<UCrowdyServerObjectDefinition>(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	}

	/** Bakes Candidate when nothing has gone wrong yet, so a half-read spec adds no confusing bake errors; empty on any error. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> BakeIfClean(TStrongObjectPtr<UCrowdyServerObjectDefinition> Candidate, TArray<FString>& Errors)
	{
		if (!Errors.IsEmpty() || !Candidate->Bake(Errors))
		{
			return TStrongObjectPtr<UCrowdyServerObjectDefinition>();
		}
		return Candidate;
	}
}

TArray<FString> CrowdyServerObjectSpec::GetAuthoredKeys()
{
	using namespace CrowdyServerObjectSpecDetail;
	TArray<FString> Keys;
	for (TFieldIterator<FProperty> It(UCrowdyServerObjectDefinition::StaticClass()); It; ++It)
	{
		if (IsAuthored(**It))
		{
			Keys.Add(It->GetAuthoredName());
		}
	}
	return Keys;
}

TSharedRef<FJsonObject> CrowdyServerObjectSpec::ToJson(const UCrowdyServerObjectDefinition& Definition)
{
	using namespace CrowdyServerObjectSpecDetail;
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	const FJsonObjectConverter::CustomExportCallback Callback = FJsonObjectConverter::CustomExportCallback::CreateStatic(&ExportSpecial);
	// A non-zero SkipFlags turns off the converter's own skips, so Transient and Deprecated are named here too.
	FJsonObjectConverter::UStructToJsonObject(UCrowdyServerObjectDefinition::StaticClass(), &Definition, Out, CPF_Edit, CPF_EditConst | CPF_Transient | CPF_Deprecated, &Callback,
		EJsonObjectConversionFlags::SkipStandardizeCase);
	return Out;
}

bool CrowdyServerObjectSpec::FromJson(const FJsonObject& Spec, UCrowdyServerObjectDefinition& Out, FFindType FindType, TArray<FString>& OutErrors)
{
	using namespace CrowdyServerObjectSpecDetail;
	const int32 ErrorsBefore = OutErrors.Num();
	FReader Reader(Out, FindType, OutErrors);
	Reader.ReadFields(Spec.Values, *UCrowdyServerObjectDefinition::StaticClass(), &Out, FString());
	CheckSelfReferences(Out, OutErrors);
	CheckLogicFile(Out, OutErrors);
	CheckKitActors(Out, OutErrors);
	CheckKitConfig(Out, OutErrors);
	return OutErrors.Num() == ErrorsBefore;
}

TStrongObjectPtr<UCrowdyServerObjectDefinition> CrowdyServerObjectSpec::MakeNew(const FJsonObject& Spec, TConstArrayView<UCrowdyServerObjectDefinition*> Project, TArray<FString>& OutErrors)
{
	using namespace CrowdyServerObjectSpecDetail;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Candidate = NewCandidate();
	FromJson(Spec, *Candidate, [&Project](const FString& Name) { return FindIn(Project, Name); }, OutErrors);
	UCrowdyServerObjectDefinition* const* Taken = Project.FindByPredicate([&Candidate](const UCrowdyServerObjectDefinition* Each)
	{
		return Each && !Candidate->TypeName.IsEmpty() && Each->TypeName.Equals(Candidate->TypeName, ESearchCase::IgnoreCase);
	});
	if (Taken)
	{
		OutErrors.Add(FString::Printf(TEXT("TypeName is \"%s\", which the type at %s already has (capitals do not tell types apart); give a new type a name no type has"), *Candidate->TypeName, *(*Taken)->GetPathName()));
	}
	if (Candidate->IsGameKitStarter())
	{
		OutErrors.Add(TEXT("CodeSource is GameKitStarter, which only the Crowdy SDK's starters use; add one with -op=addstarter"));
	}
	return BakeIfClean(Candidate, OutErrors);
}

TStrongObjectPtr<UCrowdyServerObjectDefinition> CrowdyServerObjectSpec::MakeEdited(const FJsonObject& Spec, const UCrowdyServerObjectDefinition& Existing,
	TConstArrayView<UCrowdyServerObjectDefinition*> Project, TArray<FString>& OutErrors)
{
	using namespace CrowdyServerObjectSpecDetail;
	const TArray<FString> Missing = GetAuthoredKeys().FilterByPredicate([&Spec](const FString& Key) { return !Spec.HasField(Key); });
	if (!Missing.IsEmpty())
	{
		OutErrors.Add(FString::Printf(TEXT("The spec leaves out %s. An edit replaces every setting, so a spec must hold them all; start from -op=exporttype -type=%s"), *FString::Join(Missing, TEXT(", ")), *Existing.TypeName));
	}
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Candidate = NewCandidate();
	FromJson(Spec, *Candidate, [&Project](const FString& Name) { return FindIn(Project, Name); }, OutErrors);
	if (!Candidate->TypeName.Equals(Existing.TypeName, ESearchCase::CaseSensitive))
	{
		OutErrors.Add(FString::Printf(TEXT("TypeName is \"%s\" but the type is \"%s\". A new name is a new type on the server, so make it with -op=newtype"), *Candidate->TypeName, *Existing.TypeName));
	}
	if (Candidate->IsGameKitStarter() != Existing.IsGameKitStarter())
	{
		OutErrors.Add(TEXT("CodeSource cannot change to or from GameKitStarter; -op=addstarter and -op=removestarter do that"));
	}
	return BakeIfClean(Candidate, OutErrors);
}

TArray<FString> CrowdyServerObjectSpec::FindChangedKeys(const UCrowdyServerObjectDefinition& A, const UCrowdyServerObjectDefinition& B)
{
	using namespace CrowdyServerObjectSpecDetail;
	TArray<FString> Changed;
	for (TFieldIterator<FProperty> It(UCrowdyServerObjectDefinition::StaticClass()); It; ++It)
	{
		if (IsAuthored(**It) && !It->Identical_InContainer(&A, &B))
		{
			Changed.Add(It->GetAuthoredName());
		}
	}
	return Changed;
}

TArray<CrowdyServerObjectSpec::FRenamedListValue> CrowdyServerObjectSpec::FindRenamedListValues(const UCrowdyServerObjectDefinition& Before, const UCrowdyServerObjectDefinition& After)
{
	TArray<FRenamedListValue> Renamed;
	TArray<FCrowdyServerNamedList> BeforeLists;
	TArray<FCrowdyServerNamedList> AfterLists;
	Before.GetLists(BeforeLists);
	After.GetLists(AfterLists);
	for (const FCrowdyServerNamedList& Old : BeforeLists)
	{
		const FCrowdyServerNamedList* New = AfterLists.FindByPredicate([&Old](const FCrowdyServerNamedList& Each) { return Each.Name.Equals(Old.Name, ESearchCase::CaseSensitive); });
		const UPropertyBag* OldBag = Old.Values ? Old.Values->GetPropertyBagStruct() : nullptr;
		for (const FPropertyBagPropertyDesc& Desc : OldBag ? OldBag->GetPropertyDescs() : TConstArrayView<FPropertyBagPropertyDesc>())
		{
			const FPropertyBagPropertyDesc* Now = New && New->Values ? New->Values->FindPropertyDescByID(Desc.ID) : nullptr;
			if (Now && Now->Name != Desc.Name)
			{
				Renamed.Add({Old.Name, Desc.ID, Desc.Name.ToString(), Now->Name.ToString()});
			}
		}
	}
	return Renamed;
}

void CrowdyServerObjectSpec::CopyAuthored(const UCrowdyServerObjectDefinition& From, UCrowdyServerObjectDefinition& To)
{
	for (TFieldIterator<FProperty> It(UCrowdyServerObjectDefinition::StaticClass()); It; ++It)
	{
		if (CrowdyServerObjectSpecDetail::IsAuthored(**It))
		{
			It->CopyCompleteValue_InContainer(&To, &From);
		}
	}
}

FString CrowdyServerObjectSpec::MakePackageName(const FString& ContentPath, const UCrowdyServerObjectDefinition& Definition)
{
	return FPaths::Combine(ContentPath, TEXT("CSO_") + CrowdyExec::PascalCase(Definition.TypeName));
}

UCrowdyServerObjectDefinition* CrowdyServerObjectSpec::CreateAsset(const UCrowdyServerObjectDefinition& Candidate, const FString& ContentPath, FString& OutError)
{
	if (!ContentPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(TEXT("-path is \"%s\"; a new type is made inside the project, so it must start with /Game/"), *ContentPath);
		return nullptr;
	}
	const FString PackageName = MakePackageName(ContentPath, Candidate);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
	{
		OutError = FString::Printf(TEXT("%s is not a package a new asset can have: %s"), *PackageName, *Reason.ToString());
		return nullptr;
	}
	if (FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName))
	{
		OutError = FString::Printf(TEXT("%s already exists; move or delete it, or choose another -path"), *PackageName);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	UCrowdyServerObjectDefinition* Asset = NewObject<UCrowdyServerObjectDefinition>(Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone | RF_Transactional);
	CopyAuthored(Candidate, *Asset);
	TArray<FString> Errors;
	if (!Asset->Bake(Errors))
	{
		OutError = FString::Printf(TEXT("%s does not bake: %s"), *PackageName, *FString::Join(Errors, TEXT("; ")));
		CrowdyServerObjectSpecDetail::DiscardNewAsset(*Asset, false);
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Asset);
	if (!SaveDefinitionPackage(*Asset, OutError))
	{
		CrowdyServerObjectSpecDetail::DiscardNewAsset(*Asset, true);
		return nullptr;
	}
	return Asset;
}

bool CrowdyServerObjectSpec::ApplyEdit(const UCrowdyServerObjectDefinition& Candidate, UCrowdyServerObjectDefinition& Existing, FString& OutError)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Before = CrowdyServerObjectSpecDetail::NewCandidate();
	CopyAuthored(Existing, *Before);
	CopyAuthored(Candidate, Existing);
	TArray<FString> BakeErrors;
	if (Existing.Bake(BakeErrors) && SaveDefinitionPackage(Existing, OutError))
	{
		return true;
	}
	OutError = BakeErrors.IsEmpty() ? OutError : FString::Join(BakeErrors, TEXT("\n"));
	CopyAuthored(*Before, Existing);
	TArray<FString> Ignored;
	Existing.Bake(Ignored);
	return false;
}

bool CrowdyServerObjectSpec::SaveDefinitionPackage(UCrowdyServerObjectDefinition& Definition, FString& OutError)
{
	UPackage* Package = Definition.GetOutermost();
	Definition.MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, &Definition, *FileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("could not save %s to %s"), *Package->GetName(), *FileName);
		return false;
	}
	return true;
}

bool CrowdyServerObjectSpec::SyncActorStarters(const UCrowdyServerObjectDefinition& Definition)
{
	const FSoftObjectPath Path(&Definition);
	const bool bWasListed = GetDefault<UCrowdySDKDeveloperSettings>()->GameKitActorStarters.Contains(Path);
	Definition.SyncGameKitActorStarters();
	return bWasListed != GetDefault<UCrowdySDKDeveloperSettings>()->GameKitActorStarters.Contains(Path);
}
