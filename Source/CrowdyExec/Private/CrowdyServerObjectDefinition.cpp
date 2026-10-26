#include "CrowdyServerObjectDefinition.h"

#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "Engine/UserDefinedEnum.h"
#include "Misc/StringBuilder.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

FString CrowdyExec::KeepNameCharacters(const FString& Authored)
{
	FString Name;
	for (const TCHAR Char : Authored)
	{
		if ((Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') || (Char >= '0' && Char <= '9') || Char == '_')
		{
			Name.AppendChar(Char);
		}
	}
	return Name;
}

FString CrowdyExec::PascalCase(const FString& SnakeName)
{
	FString Name;
	bool bWordStart = true;
	for (const TCHAR Char : KeepNameCharacters(SnakeName))
	{
		if (Char == '_')
		{
			bWordStart = true;
			continue;
		}
		Name.AppendChar(bWordStart && Char >= 'a' && Char <= 'z' ? static_cast<TCHAR>(Char - 'a' + 'A') : Char);
		bWordStart = false;
	}
	return Name;
}

bool CrowdyExec::SameKeptName(FStringView A, FStringView B)
{
	auto IsKept = [](TCHAR Char) { return (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') || (Char >= '0' && Char <= '9') || Char == '_'; };
	int32 AtA = 0;
	int32 AtB = 0;
	for (;;)
	{
		while (AtA < A.Len() && !IsKept(A[AtA]))
		{
			++AtA;
		}
		while (AtB < B.Len() && !IsKept(B[AtB]))
		{
			++AtB;
		}
		if (AtA == A.Len() || AtB == B.Len())
		{
			return AtA == A.Len() && AtB == B.Len();
		}
		if (FChar::ToLower(A[AtA++]) != FChar::ToLower(B[AtB++]))
		{
			return false;
		}
	}
}

namespace
{
	const FProperty* ScanTravellingField(const UStruct* Struct, FName Name)
	{
		// A cooked Blueprint struct only knows its field names without spaces and punctuation, so compare what is left of both.
		const bool bAuthoredNames = Struct->IsA<UUserDefinedStruct>();
		TStringBuilder<128> Wanted;
		Name.AppendString(Wanted);
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (!CrowdyExec::IsTravellingProperty(*It))
			{
				continue;
			}
			TStringBuilder<128> Field;
			if (bAuthoredNames)
			{
				Field << It->GetAuthoredName();
			}
			else
			{
				It->GetFName().AppendString(Field);
			}
			if (CrowdyExec::SameKeptName(Field.ToView(), Wanted.ToView()))
			{
				return *It;
			}
		}
		return nullptr;
	}

	struct FFoundField
	{
		TWeakObjectPtr<const UStruct> Struct;
		const FProperty* Field = nullptr;
	};
}

const FProperty* CrowdyExec::FindTravellingField(const UStruct* Struct, FName Name)
{
	if (!Struct)
	{
		return nullptr;
	}
	if (!IsInGameThread())
	{
		return ScanTravellingField(Struct, Name);
	}
	// Get Server Variable runs per read, often in Tick; a scan of a Blueprint struct builds a name per field.
	static TMap<TPair<const UStruct*, FName>, FFoundField> Found;
	static uint32 FoundGeneration = 0;
	if (FoundGeneration != GetStructGeneration())
	{
		Found.Reset();
		FoundGeneration = GetStructGeneration();
	}
	const TPair<const UStruct*, FName> Key(Struct, Name);
	if (const FFoundField* Known = Found.Find(Key); Known && Known->Struct.Get() == Struct)
	{
		return Known->Field;
	}
	const FProperty* Field = ScanTravellingField(Struct, Name);
	Found.Add(Key, FFoundField{ Struct, Field });
	return Field;
}

const TCHAR* const UCrowdyServerObjectDefinition::OnlyInstanceId = TEXT("main");
const TCHAR* const UCrowdyServerObjectDefinition::RootTypeName = TEXT("root");

FString UCrowdyServerObjectDefinition::GetOnlyInstanceId() const
{
	// A Game Kit hub with one instance, such as world, is reached by the empty key.
	return IsFunctionLibrary() || IsGameKitStarter() || TypeName.Equals(RootTypeName, ESearchCase::CaseSensitive) ? FString() : FString(OnlyInstanceId);
}

namespace
{
	bool IsLowerAscii(TCHAR Char) { return Char >= 'a' && Char <= 'z'; }
	bool IsUpperAscii(TCHAR Char) { return Char >= 'A' && Char <= 'Z'; }
	bool IsDigitAscii(TCHAR Char) { return Char >= '0' && Char <= '9'; }

	/** Type and function names: lowercase ASCII. Field and enum value names: any-case ASCII. */
	bool IsServerName(const FString& Name, int32 MaxLength, bool bLowercase)
	{
		if (Name.IsEmpty() || Name.Len() > MaxLength)
		{
			return false;
		}
		const TCHAR First = Name[0];
		if (!(IsLowerAscii(First) || (!bLowercase && (IsUpperAscii(First) || First == '_'))))
		{
			return false;
		}
		for (const TCHAR Char : Name)
		{
			if (!(IsLowerAscii(Char) || IsDigitAscii(Char) || Char == '_' || (!bLowercase && IsUpperAscii(Char))))
			{
				return false;
			}
		}
		return true;
	}

	using CrowdyExec::KeepNameCharacters;
	using CrowdyExec::PascalCase;

	/** AttackBoss becomes attack_boss. */
	FString SnakeCase(const FString& Source)
	{
		FString Snake;
		for (int32 Index = 0; Index < Source.Len(); ++Index)
		{
			const TCHAR Char = Source[Index];
			if (!IsLowerAscii(Char) && !IsUpperAscii(Char) && !IsDigitAscii(Char) && Char != '_')
			{
				continue;
			}
			const TCHAR Previous = Index > 0 ? Source[Index - 1] : TCHAR(0);
			const bool bNextLower = Index + 1 < Source.Len() && IsLowerAscii(Source[Index + 1]);
			const bool bWordStart = IsLowerAscii(Previous) || IsDigitAscii(Previous) || (IsUpperAscii(Previous) && bNextLower);
			if (IsUpperAscii(Char) && Index > 0 && bWordStart)
			{
				Snake.AppendChar('_');
			}
			Snake.AppendChar(IsUpperAscii(Char) ? static_cast<TCHAR>(Char - 'A' + 'a') : Char);
		}
		return Snake;
	}

	/** A Game Kit starter's field as its hub names it: TimeOfDay becomes time_of_day, and a bool's bReady becomes ready. */
	FString KitFieldName(const FProperty* Property, const FString& Authored)
	{
		const bool bBoolPrefix = Property->IsA<FBoolProperty>() && Authored.Len() > 1 && Authored[0] == 'b' && IsUpperAscii(Authored[1]);
		return SnakeCase(bBoolPrefix ? Authored.RightChop(1) : Authored);
	}

	FString AuthoredMethodName(const FCrowdyServerFunction& Function)
	{
		return Function.ServerName.IsEmpty() ? SnakeCase(Function.Name.ToString()) : Function.ServerName;
	}

	/** The method a check judges: in the editor the one the next bake writes down, never a stale baked one. */
	FString CheckedMethodName(const FCrowdyServerFunction& Function)
	{
#if WITH_EDITOR
		return AuthoredMethodName(Function);
#else
		return Function.GetMethodName();
#endif
	}

	FString AuthoredTopic(const FCrowdyServerMessage& Message)
	{
		return Message.ServerName.IsEmpty() ? SnakeCase(Message.Name.ToString()) : Message.ServerName;
	}

	/** The topic a check judges: in the editor the one the next bake writes down, never a stale baked one. */
	FString CheckedTopic(const FCrowdyServerMessage& Message)
	{
#if WITH_EDITOR
		return AuthoredTopic(Message);
#else
		return Message.GetTopic();
#endif
	}

	const UScriptStruct* PickValues(ECrowdyServerValuesForm Form, const UScriptStruct* Struct, const FInstancedPropertyBag& List)
	{
		if (Form == ECrowdyServerValuesForm::Struct)
		{
			return Struct;
		}
		const UPropertyBag* Bag = List.GetPropertyBagStruct();
		return Bag && Bag->GetPropertyDescs().Num() > 0 ? Bag : nullptr;
	}

	/** The List whose struct is Struct, without naming every List; the State's first, as GetLists orders them. */
	const FInstancedPropertyBag* FindListValues(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct)
	{
		auto Holds = [Struct](ECrowdyServerValuesForm Form, const FInstancedPropertyBag& List)
		{
			return Form == ECrowdyServerValuesForm::List && List.GetPropertyBagStruct() == Struct;
		};
		const bool bLibrary = Definition.IsFunctionLibrary();
		if (!bLibrary && Holds(Definition.StateForm, Definition.StateList))
		{
			return &Definition.StateList;
		}
		for (const FCrowdyServerFunction& Function : Definition.Functions)
		{
			if (Holds(Function.ParamsForm, Function.ParamsList))
			{
				return &Function.ParamsList;
			}
			if (Holds(Function.ReplyForm, Function.ReplyList))
			{
				return &Function.ReplyList;
			}
		}
		if (bLibrary)
		{
			return nullptr;
		}
		for (const FCrowdyServerMessage& Message : Definition.Messages)
		{
			if (Holds(Message.PayloadForm, Message.PayloadList))
			{
				return &Message.PayloadList;
			}
		}
		return nullptr;
	}


	/** An enum value as the editor names it: the display name for a Blueprint enum, the short name for C++. */
	FString AuthoredValueName(const UEnum* Enum, int32 Index)
	{
		return Cast<UUserDefinedEnum>(Enum) ? Enum->GetDisplayNameTextByIndex(Index).ToString() : Enum->GetNameStringByIndex(Index);
	}

	int32 ValueCount(const UEnum* Enum)
	{
		return Enum->ContainsExistingMax() ? Enum->NumEnums() - 1 : Enum->NumEnums();
	}

	bool HasField(const UStruct* Struct, FName Field)
	{
		if (!Struct)
		{
			return false;
		}
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (It->GetAuthoredName().Equals(Field.ToString(), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool HasValue(const UEnum* Enum, FName Value)
	{
		if (!Enum)
		{
			return false;
		}
		for (int32 Index = 0; Index < ValueCount(Enum); ++Index)
		{
			if (AuthoredValueName(Enum, Index).Equals(Value.ToString(), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	int32 ValueDepth(const FProperty* Property, int32 Level);

	/** The deepest level a struct at Level reaches on the wire; counting stops once past the limit. */
	int32 StructDepth(const UStruct* Struct, int32 Level)
	{
		int32 Deepest = Level;
		for (TFieldIterator<FProperty> It(Struct); It && Deepest <= CrowdyExec::MaxDepth; ++It)
		{
			Deepest = CrowdyExec::IsTravellingProperty(*It) ? FMath::Max(Deepest, ValueDepth(*It, Level + 1)) : Deepest;
		}
		return Deepest;
	}

	/** Counted like the codec counts: structs, containers and engine structs are a level each, an optional is not. */
	int32 ValueDepth(const FProperty* Property, int32 Level)
	{
		ECrowdyExecKind Kind;
		FString Refusal;
		if (!CrowdyExec::ClassifyProperty(Property, Kind, Refusal))
		{
			return 0;
		}
		switch (Kind)
		{
		case ECrowdyExecKind::Struct:
			return StructDepth(CastFieldChecked<FStructProperty>(Property)->Struct, Level);
		case ECrowdyExecKind::Array:
			return FMath::Max(Level, ValueDepth(CastFieldChecked<FArrayProperty>(Property)->Inner, Level + 1));
		case ECrowdyExecKind::Map:
			return FMath::Max(Level, ValueDepth(CastFieldChecked<FMapProperty>(Property)->ValueProp, Level + 1));
		case ECrowdyExecKind::Set:
			return Level;
		case ECrowdyExecKind::Optional:
			return ValueDepth(CastFieldChecked<FOptionalProperty>(Property)->GetValueProperty(), Level);
		default:
			return Kind >= ECrowdyExecKind::Vector && Kind <= ECrowdyExecKind::Color ? Level : 0;
		}
	}

	/** Struct's travelling bool called bHas<Authored>, which records whether the field Authored arrived with a value; null when there is none. */
	const FBoolProperty* FindPresenceFlag(const UStruct* Struct, const FString& Authored)
	{
		const FString FlagName = TEXT("bHas") + Authored;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FBoolProperty* Flag = CastField<FBoolProperty>(*It);
			if (Flag && CrowdyExec::IsTravellingProperty(Flag) && It.GetStruct()->GetAuthoredNameForField(Flag).Equals(FlagName, ESearchCase::CaseSensitive))
			{
				return Flag;
			}
		}
		return nullptr;
	}

	/** Whether Property is a bHas<X> bool beside a travelling field X, which makes it a presence flag and no field of its own. */
	bool IsPresenceFlag(const UStruct* Struct, const FProperty* Property, const FString& Authored)
	{
		if (!Property->IsA<FBoolProperty>() || Authored.Len() <= 4 || !Authored.StartsWith(TEXT("bHas"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		const FString Value = Authored.RightChop(4);
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (*It != Property && CrowdyExec::IsTravellingProperty(*It) && It.GetStruct()->GetAuthoredNameForField(*It).Equals(Value, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** Walks the structs a definition reaches, collecting their field tables and the enums they use. */
	struct FTableBuilder
	{
		const UCrowdyServerObjectDefinition& Definition;
		TArray<FString>& Errors;
		TArray<const UScriptStruct*> Structs;
		TArray<const UEnum*> Enums;
		TArray<FName> WatchedFound;
		TArray<FName> StateFields;

		void CheckType(const FProperty* Property, const FString& FieldPath, bool bKey)
		{
			ECrowdyExecKind Kind;
			FString Refusal;
			if (!CrowdyExec::ClassifyProperty(Property, Kind, Refusal))
			{
				Errors.Add(FieldPath + TEXT(": ") + Refusal);
				return;
			}
			if (bKey && !CrowdyExec::IsKeyKind(Kind))
			{
				Errors.Add(FieldPath + TEXT(": set elements and map keys must be strings, names, integers or enums"));
				return;
			}
			switch (Kind)
			{
			case ECrowdyExecKind::Struct:
				Structs.AddUnique(CastFieldChecked<FStructProperty>(Property)->Struct);
				break;
			case ECrowdyExecKind::Enum:
				Enums.AddUnique(CrowdyExec::GetPropertyEnum(Property));
				break;
			case ECrowdyExecKind::Array:
				CheckType(CastFieldChecked<FArrayProperty>(Property)->Inner, FieldPath, false);
				break;
			case ECrowdyExecKind::Optional:
				CheckType(CastFieldChecked<FOptionalProperty>(Property)->GetValueProperty(), FieldPath, false);
				break;
			case ECrowdyExecKind::Set:
				CheckType(CastFieldChecked<FSetProperty>(Property)->ElementProp, FieldPath, true);
				break;
			case ECrowdyExecKind::Map:
				CheckType(CastFieldChecked<FMapProperty>(Property)->KeyProp, FieldPath, true);
				CheckType(CastFieldChecked<FMapProperty>(Property)->ValueProp, FieldPath, false);
				break;
			default:
				break;
			}
		}

		FString FindFieldOverride(const UStruct* Owner, const UStruct* Declarer, const FString& Authored) const
		{
			for (const FCrowdyServerFieldName& Override : Definition.FieldNames)
			{
				const bool bStruct = Override.Struct.Get() == Owner || Override.Struct.Get() == Declarer;
				if (bStruct && Override.Field.ToString().Equals(Authored, ESearchCase::IgnoreCase))
				{
					return Override.ServerName;
				}
			}
			return FString();
		}

		/** The server name a List Value Names entry keeps for the value ValueId of List; the entry must match both. */
		FString FindListValueOverride(const FString& List, const FGuid& ValueId) const
		{
			const FName ListName(*List);
			const FCrowdyServerListValueName* Found = Definition.ListValueNames.FindByPredicate([ListName, &ValueId](const FCrowdyServerListValueName& Entry)
			{
				return Entry.List == ListName && Entry.ValueId == ValueId;
			});
			return Found ? Found->ServerName : FString();
		}

		FString Describe(const UScriptStruct* Struct) const
		{
			const FString List = Definition.FindListName(Struct);
			return List.IsEmpty() ? CrowdyExec::DisplayName(Struct) : List;
		}

		void BakeStruct(const UScriptStruct* Struct, FCrowdyExecBakedStruct& Out)
		{
			Out.List = Definition.FindListName(Struct);
			Out.Struct = Out.List.IsEmpty() ? const_cast<UScriptStruct*>(Struct) : nullptr;
			const FString StructName = Describe(Struct);
			const bool bState = Struct == Definition.GetStateStruct();
			TArray<const UStruct*> Chain;
			for (const UStruct* Link = Struct; Link; Link = Link->GetSuperStruct())
			{
				Chain.Insert(Link, 0);
			}
			TSet<FString> Seen;
			for (const UStruct* Declarer : Chain)
			{
				for (TFieldIterator<FProperty> It(Declarer, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					const FProperty* Property = *It;
					if (!CrowdyExec::IsTravellingProperty(Property))
					{
						continue;
					}
					const FString Authored = Declarer->GetAuthoredNameForField(Property);
					const bool bKit = Definition.IsGameKitStarter();
					if (bKit && IsPresenceFlag(Struct, Property, Authored))
					{
						continue;
					}
					const FString FieldPath = StructName + TEXT(".") + Authored;
					const FGuid PropertyGuid = Declarer->ArePropertyGuidsAvailable() ? Declarer->FindPropertyGuidFromName(Property->GetFName()) : FGuid();
					FString ServerName = Out.List.IsEmpty() ? FindFieldOverride(Struct, Declarer, Authored) : FindListValueOverride(Out.List, PropertyGuid);
					const bool bOverridden = !ServerName.IsEmpty();
					ServerName = bOverridden ? ServerName : bKit ? KitFieldName(Property, Authored) : KeepNameCharacters(Authored);
					if (!IsServerName(ServerName, 64, false))
					{
						Errors.Add(FString::Printf(TEXT("%s: '%s' cannot be its server name, which must be letters, digits and underscores, not start with a digit, and be at most 64 characters; %s"),
							*FieldPath, *ServerName, Out.List.IsEmpty() || bOverridden ? TEXT("set one under Server Names") : TEXT("rename the value")));
					}
					else if (Seen.Contains(ServerName))
					{
						Errors.Add(FString::Printf(TEXT("%s: another field of %s is also named '%s'; server names must differ in more than case"), *FieldPath, *StructName, *ServerName));
					}
					Seen.Add(ServerName);
					CheckType(Property, FieldPath, false);

					FCrowdyExecBakedField& Field = Out.Fields.AddDefaulted_GetRef();
					Field.ServerName = ServerName;
					Field.Property = Property->GetFName();
					Field.PropertyGuid = PropertyGuid;
					const FBoolProperty* Presence = bKit ? FindPresenceFlag(Struct, Authored) : nullptr;
					Field.PresenceProperty = Presence ? Presence->GetFName() : NAME_None;
					// A collection member carries all its props.
					if (!bState)
					{
						Field.bWatched = Definition.HasKitCollection() && Struct == Definition.KitRecord;
						continue;
					}
					StateFields.Add(FName(*Authored));
					// A starter's snapshot carries its whole State.
					if (Definition.IsGameKitStarter())
					{
						Field.bWatched = true;
						continue;
					}
					if (!Definition.WatchedFields.Contains(FName(*Authored)))
					{
						continue;
					}
					Field.bWatched = true;
					WatchedFound.Add(FName(*Authored));
					if (Definition.Holds != ECrowdyServerObjectHolds::RecordPerEntity)
					{
						continue;
					}
					if (Definition.OwnerFields.Contains(FName(*Authored)))
					{
						Field.Audience = ECrowdyServerFieldAudience::Owner;
					}
					else if (Definition.TeamFields.Contains(FName(*Authored)))
					{
						Field.Audience = ECrowdyServerFieldAudience::Team;
					}
				}
			}
		}

		/** Each name must be a State field that is Visible to Players, and not also in Other. */
		void CheckAudience(const TArray<FName>& Names, const TCHAR* Label, const TCHAR* OtherLabel, const TArray<FName>& Other, const UScriptStruct* StateStruct)
		{
			for (const FName& Name : Names)
			{
				if (!StateFields.Contains(Name))
				{
					Errors.Add(FString::Printf(TEXT("%s variable '%s' is not a field of %s"), Label, *Name.ToString(), *Describe(StateStruct)));
				}
				else if (!Definition.WatchedFields.Contains(Name))
				{
					Errors.Add(FString::Printf(TEXT("%s variable '%s' is not Visible to Players; choose %s for it again"), Label, *Name.ToString(), Label));
				}
				if (Other.Contains(Name))
				{
					Errors.Add(FString::Printf(TEXT("'%s' is both %s and %s; choose one"), *Name.ToString(), Label, OtherLabel));
				}
			}
		}

		void BakeEnum(const UEnum* Enum, FCrowdyExecBakedEnum& Out)
		{
			Out.Enum = const_cast<UEnum*>(Enum);
			const int32 Count = ValueCount(Enum);
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Count; ++Index)
			{
#if WITH_METADATA
				if (Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
#endif
				const FString Authored = AuthoredValueName(Enum, Index);
				FString ServerName;
				for (const FCrowdyServerEnumValueName& Override : Definition.EnumValueNames)
				{
					if (Override.Enum.Get() == Enum &&Override.Value.ToString().Equals(Authored, ESearchCase::IgnoreCase))
					{
						ServerName = Override.ServerName;
					}
				}
				ServerName = !ServerName.IsEmpty() ? ServerName : Definition.IsGameKitStarter() ? SnakeCase(Authored) : KeepNameCharacters(Authored);
				const FString ValuePath = Enum->GetName() + TEXT(".") + Authored;
				if (!IsServerName(ServerName, 64, false))
				{
					Errors.Add(FString::Printf(TEXT("%s: '%s' cannot be its server name, which must be letters, digits and underscores, not start with a digit, and be at most 64 characters; set one under Server Names"),
						*ValuePath, *ServerName));
				}
				else if (Seen.Contains(ServerName))
				{
					Errors.Add(FString::Printf(TEXT("%s: another value of %s is also named '%s'; server names must differ in more than case"), *ValuePath, *Enum->GetName(), *ServerName));
				}
				Seen.Add(ServerName);
				Out.Values.Add(Enum->GetValueByIndex(Index));
				Out.ServerNames.Add(ServerName);
				Out.EnumeratorNames.Add(Enum->GetNameByIndex(Index));
			}
		}
	};

	bool IsBuiltInMethod(const FString& Method)
	{
		return UCrowdyServerObjectDefinition::GetMemberFunctions().ContainsByPredicate([&Method](const FCrowdyServerFunction& BuiltIn)
		{
			return BuiltIn.GetMethodName().Equals(Method, ESearchCase::IgnoreCase);
		});
	}

	/** What a Server Function Library cannot do: it has no members and saves nothing, so a function may not need either. */
	void CheckLibrary(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (!Definition.IsFunctionLibrary())
		{
			return;
		}
		for (const FCrowdyServerFunction& Function : Definition.Functions)
		{
			const FString Name = Function.Name.ToString();
			if (Function.WhoCanCall == ECrowdyServerFunctionCaller::Members || Function.WhoCanCall == ECrowdyServerFunctionCaller::Leader)
			{
				Errors.Add(FString::Printf(TEXT("%s: Members and Leader need members, and a Server Function Library has none."), *Name));
			}
			if (Function.CooldownSeconds > 0.f)
			{
				Errors.Add(FString::Printf(TEXT("%s: a cooldown is saved data, and a Server Function Library saves none."), *Name));
			}
		}
#if WITH_EDITORONLY_DATA
		if (Definition.MaxInstances < Definition.MinInstances)
		{
			Errors.Add(TEXT("Max Instances must be at least Min Instances."));
		}
#endif
	}

	bool HasDoubleField(const UScriptStruct* Struct, FName Name)
	{
		return CastField<FDoubleProperty>(CrowdyExec::FindTravellingField(Struct, Name)) != nullptr;
	}

	/** The Position Function must be a repeatable call whose inputs, and reply when it has one, carry the ground position as doubles X and Y. */
	void CheckPositionFunction(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (Definition.KitPositionFunction.IsNone())
		{
			return;
		}
		const FString Name = Definition.KitPositionFunction.ToString();
		const FCrowdyServerFunction* Function = Definition.Functions.FindByPredicate([&Definition](const FCrowdyServerFunction& Candidate)
		{
			return Candidate.Name == Definition.KitPositionFunction;
		});
		if (!Function)
		{
			Errors.Add(FString::Printf(TEXT("Position Function '%s' is not a function of this starter"), *Name));
			return;
		}
		if (Function->bSendOnce)
		{
			Errors.Add(FString::Printf(TEXT("Position Function %s is Send Once, which would run only the first report; turn Send Once off"), *Name));
		}
		const UScriptStruct* Inputs = Function->GetParamsStruct();
		if (!Inputs || !HasDoubleField(Inputs, TEXT("X")) || !HasDoubleField(Inputs, TEXT("Y")))
		{
			Errors.Add(FString::Printf(TEXT("Position Function %s needs inputs with double variables X and Y: the position in metres"), *Name));
		}
		const UScriptStruct* Reply = Function->GetReplyStruct();
		if (Reply && (!HasDoubleField(Reply, TEXT("X")) || !HasDoubleField(Reply, TEXT("Y"))))
		{
			Errors.Add(FString::Printf(TEXT("Position Function %s must reply with nothing or with double variables X and Y: the position the server kept"), *Name));
		}
	}

	/** A starter's server code decides who reads and calls it, so only what the Game Kit's protocol carries may be set. */
	void CheckGameKitStarter(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (!Definition.IsGameKitStarter())
		{
			return;
		}
		if (Definition.IsFunctionLibrary())
		{
			Errors.Add(TEXT("A Game Kit starter is a Server Object, not a Server Function Library"));
		}
		if (!Definition.KitStateTopic.IsEmpty() && !IsServerName(Definition.KitStateTopic, 64, true))
		{
			Errors.Add(FString::Printf(TEXT("State Topic '%s' must be lowercase letters, digits and underscores, starting with a letter"), *Definition.KitStateTopic));
		}
		const FString& Collection = Definition.KitCollectionTopic;
		if (!Collection.IsEmpty() && !IsServerName(Collection, 64, true))
		{
			Errors.Add(FString::Printf(TEXT("Collection Topic '%s' must be lowercase letters, digits and underscores, starting with a letter"), *Collection));
		}
		if (Collection.IsEmpty() != !Definition.KitRecord)
		{
			Errors.Add(TEXT("A Collection Topic needs a Record Struct, and a Record Struct a Collection Topic"));
		}
		if (!Collection.IsEmpty() && (Collection.Equals(Definition.KitStateTopic, ESearchCase::CaseSensitive) || Definition.FindMessageByTopic(Collection)))
		{
			Errors.Add(FString::Printf(TEXT("Collection Topic '%s' is also the State Topic or a message's topic"), *Collection));
		}
		if (Definition.KitRecord && Definition.KitRecord == Definition.GetStateStruct())
		{
			Errors.Add(TEXT("The Record Struct must differ from the State struct"));
		}
		if (!Definition.KitOwnerValues.IsEmpty() && Definition.KitStateTopic.IsEmpty())
		{
			Errors.Add(TEXT("Owner Values need a State Topic: they are read after the State and kept beside it"));
		}
		for (const FCrowdyKitOwnerValue& Owned : Definition.KitOwnerValues)
		{
			if (!IsServerName(Owned.Method, 64, true) || !IsServerName(Owned.What, 64, true))
			{
				Errors.Add(FString::Printf(TEXT("Owner Value %s needs a Method and a What of lowercase letters, digits and underscores, starting with a letter"), *Owned.Variable.ToString()));
			}
			if (!Definition.FindVariable(Owned.Variable))
			{
				Errors.Add(FString::Printf(TEXT("Owner Value %s is not a variable of the State"), *Owned.Variable.ToString()));
			}
		}
		CheckPositionFunction(Definition, Errors);
		if (Definition.Visibility != ECrowdyServerObjectVisibility::Public || Definition.Holds != ECrowdyServerObjectHolds::OneSet
			|| Definition.MembersFrom != ECrowdyServerMembersSource::None)
		{
			Errors.Add(TEXT("A Game Kit starter keeps one set of variables, Readable By Every Player, with no Members From: its server code decides who reads it"));
		}
	}

	void CheckMembers(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (Definition.IsFunctionLibrary())
		{
			return;
		}
		const bool bNoMembers = Definition.MembersFrom == ECrowdyServerMembersSource::None;
		const bool bOwnMembers = Definition.MembersFrom == ECrowdyServerMembersSource::ThisObject;
		if (bNoMembers && Definition.Visibility == ECrowdyServerObjectVisibility::Members)
		{
			Errors.Add(TEXT("Readable By Members needs Members From This Object or Crowdy Team"));
		}
		if (Definition.MembersFrom == ECrowdyServerMembersSource::CrowdyTeam && Definition.Visibility == ECrowdyServerObjectVisibility::OwnerOnly)
		{
			Errors.Add(TEXT("Readable By Owner Only cannot be used with Members From Crowdy Team, whose Instance Id is the team id"));
		}
		if (Definition.bOnlyOneInstance && Definition.Visibility == ECrowdyServerObjectVisibility::OwnerOnly)
		{
			Errors.Add(TEXT("Only One Instance cannot be used with Readable By Owner Only, whose Instance Id is each player's user id"));
		}
		if (Definition.bOnlyOneInstance && Definition.MembersFrom == ECrowdyServerMembersSource::CrowdyTeam)
		{
			Errors.Add(TEXT("Only One Instance cannot be used with Members From Crowdy Team, whose Instance Id is the team id"));
		}
		if (bOwnMembers && Definition.MaxMembers < 0)
		{
			Errors.Add(FString::Printf(TEXT("Max Members is %d; it must be 0 (no limit) or more"), Definition.MaxMembers));
		}
		// A client refuses a members list longer than it decodes, so a shown list needs a limit it can hold.
		if (bOwnMembers && Definition.bShowMembers && (Definition.MaxMembers < 1 || Definition.MaxMembers > CrowdyExec::MaxElements))
		{
			Errors.Add(FString::Printf(TEXT("Show Members to Players needs Max Members from 1 to %d; turn it off for larger groups"), CrowdyExec::MaxElements));
		}
		for (const FCrowdyServerFunction& Function : Definition.Functions)
		{
			const bool bLeader = Function.WhoCanCall == ECrowdyServerFunctionCaller::Leader;
			if (bNoMembers && (bLeader || Function.WhoCanCall == ECrowdyServerFunctionCaller::Members))
			{
				Errors.Add(FString::Printf(TEXT("Function %s: Callable By %s needs Members From This Object or Crowdy Team"),
					*Function.Name.ToString(), bLeader ? TEXT("Leader") : TEXT("Members")));
			}
			if (bOwnMembers && (UCrowdyServerObjectDefinition::IsMemberFunction(Function.Name) || IsBuiltInMethod(CheckedMethodName(Function))))
			{
				Errors.Add(FString::Printf(TEXT("Function %s: the name is taken by a built-in function while Members From is This Object; rename it"),
					*Function.Name.ToString()));
			}
		}
	}

	void CheckRecords(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (Definition.IsFunctionLibrary())
		{
			return;
		}
		if (Definition.AreaSizeChunks < 1 || Definition.AreaSizeChunks > CrowdyExec::MaxAreaSizeChunks)
		{
			Errors.Add(FString::Printf(TEXT("Area Size is %d; it must be from 1 to %d"), Definition.AreaSizeChunks, CrowdyExec::MaxAreaSizeChunks));
		}
		if (Definition.InterestRadiusAreas < 0 || Definition.InterestRadiusAreas > CrowdyExec::MaxInterestRadiusAreas)
		{
			Errors.Add(FString::Printf(TEXT("Areas Followed Around a Player is %d; it must be from 0 to %d"), Definition.InterestRadiusAreas, CrowdyExec::MaxInterestRadiusAreas));
		}
		if (Definition.Holds != ECrowdyServerObjectHolds::RecordPerEntity)
		{
			return;
		}
		if (Definition.Visibility != ECrowdyServerObjectVisibility::Public)
		{
			Errors.Add(TEXT("A Record per Entity needs Readable By Every Player; make the variables others must not see Owner Only or Team Only"));
		}
		if (Definition.MaxRecords < 1 || Definition.MaxRecords > CrowdyExec::MaxElements)
		{
			Errors.Add(FString::Printf(TEXT("Max Records is %d; it must be from 1 to %d"), Definition.MaxRecords, CrowdyExec::MaxElements));
		}
		if (Definition.PushIntervalMs < CrowdyExec::MinPushIntervalMs || Definition.PushIntervalMs > CrowdyExec::MaxPushIntervalMs)
		{
			Errors.Add(FString::Printf(TEXT("Send Changes Every is %d ms; it must be from %d to %d ms"), Definition.PushIntervalMs, CrowdyExec::MinPushIntervalMs, CrowdyExec::MaxPushIntervalMs));
		}
		if (Definition.MembersFrom != ECrowdyServerMembersSource::None)
		{
			Errors.Add(TEXT("A Record per Entity cannot use Members From; each record carries its own owner and team"));
		}
		for (const FCrowdyServerFunction& Function : Definition.Functions)
		{
			if (CheckedMethodName(Function).Equals(TEXT("changes"), ESearchCase::CaseSensitive))
			{
				Errors.Add(FString::Printf(TEXT("Function %s: 'changes' is reserved in a Record per Entity; rename the function"), *Function.Name.ToString()));
			}
		}
	}

	bool IsFeatureKey(const FString& Key)
	{
		return !Key.IsEmpty() && Key.Len() <= 64 && Algo::AllOf(Key, [](TCHAR Char)
		{
			return IsLowerAscii(Char) || IsUpperAscii(Char) || IsDigitAscii(Char) || Char == '_' || Char == '-' || Char == '.';
		});
	}

	void CheckRequiredFeature(const FCrowdyServerFunction& Function, TArray<FString>& Errors)
	{
		if (Function.RequiredFeature.IsEmpty())
		{
			return;
		}
		const FString& Key = Function.RequiredFeature;
		if (!IsFeatureKey(Key))
		{
			Errors.Add(FString::Printf(TEXT("Function %s: Required Feature '%s' must be 1 to 64 letters, digits, underscores, hyphens and dots"), *Function.Name.ToString(), *Key));
		}
		if (Function.WhoCanCall == ECrowdyServerFunctionCaller::ServerOnly || Function.WhoCanCall == ECrowdyServerFunctionCaller::DevelopersOnly)
		{
			Errors.Add(FString::Printf(TEXT("Function %s: Required Feature is only checked for players, and Callable By %s refuses every player; clear it or change Callable By"),
				*Function.Name.ToString(), Function.WhoCanCall == ECrowdyServerFunctionCaller::ServerOnly ? TEXT("Server Only") : TEXT("Developers Only")));
		}
	}

	enum class ECronField : uint8
	{
		Plain,
		DayOfMonth,
		DayOfWeek,
	};

	struct FCronLimits
	{
		const TCHAR* Label;
		const TCHAR* Range;
		int32 Min;
		int32 Max;
		const TCHAR* Names;
		ECronField Kind;
	};

	bool IsPlainNumber(const FString& Text)
	{
		return !Text.IsEmpty() && Text.Len() <= 4 && Algo::AllOf(Text, [](TCHAR Char) { return FChar::IsDigit(Char); });
	}

	/** A number in the field's range, or one of its three-letter names, in any case. */
	bool CronValue(const FString& Text, const FCronLimits& Limits)
	{
		if (IsPlainNumber(Text))
		{
			const int32 Value = FCString::Atoi(*Text);
			return Value >= Limits.Min && Value <= Limits.Max;
		}
		return Limits.Names && Text.Len() == 3 && FCString::Strstr(Limits.Names, *Text.ToUpper()) != nullptr;
	}

	/** One comma-separated item: * or ? or a value or a range, with an optional /step, or L, 5L or 1#2 where the field allows. */
	bool CronItemFits(const FString& Item, const FCronLimits& Limits)
	{
		FString Left;
		FString Step;
		const bool bStepped = Item.Split(TEXT("/"), &Left, &Step);
		const FString& Base = bStepped ? Left : Item;
		if (bStepped && (!IsPlainNumber(Step) || FCString::Atoi(*Step) < 1))
		{
			return false;
		}
		if (Base.Equals(TEXT("*"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (Base.Equals(TEXT("?"), ESearchCase::CaseSensitive))
		{
			return !bStepped && Limits.Kind != ECronField::Plain;
		}
		if (Limits.Kind == ECronField::DayOfMonth && Base.Equals(TEXT("L"), ESearchCase::CaseSensitive))
		{
			return !bStepped;
		}
		FString From;
		FString To;
		if (Limits.Kind == ECronField::DayOfWeek && Base.Split(TEXT("#"), &From, &To))
		{
			return !bStepped && CronValue(From, Limits) && To.Len() == 1 && To[0] >= '1' && To[0] <= '5';
		}
		if (Limits.Kind == ECronField::DayOfWeek && Base.Len() > 1 && Base.EndsWith(TEXT("L"), ESearchCase::CaseSensitive))
		{
			return !bStepped && CronValue(Base.LeftChop(1), Limits);
		}
		if (!Base.Split(TEXT("-"), &From, &To))
		{
			return CronValue(Base, Limits);
		}
		return CronValue(From, Limits) && CronValue(To, Limits);
	}

	bool CronFieldFits(const FString& Field, const FCronLimits& Limits)
	{
		TArray<FString> Items;
		Field.ParseIntoArray(Items, TEXT(","), false);
		return !Field.EndsWith(TEXT(",")) && Algo::AllOf(Items, [&Limits](const FString& Item) { return CronItemFits(Item, Limits); });
	}

	/** Why a cron expression cannot be one, or empty when the server will read it; the server's own parser has the last word. */
	FString CronProblem(const FString& Expression)
	{
		const FString Trimmed = Expression.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return TEXT("On Schedule needs a Cron expression, such as 0 */6 * * *");
		}
		static const TCHAR* const Macros[] = {TEXT("@yearly"), TEXT("@monthly"), TEXT("@weekly"), TEXT("@daily"), TEXT("@hourly")};
		if (Trimmed.StartsWith(TEXT("@")))
		{
			const bool bKnown = Algo::AnyOf(Macros, [&Trimmed](const TCHAR* Macro) { return Trimmed.Equals(Macro, ESearchCase::CaseSensitive); });
			return bKnown ? FString() : FString::Printf(TEXT("Cron '%s' is not a schedule; use @hourly, @daily, @weekly, @monthly or @yearly"), *Trimmed);
		}
		const bool bOnlySyntax = Algo::AllOf(Trimmed, [](TCHAR Char) { return (Char < 128 && FChar::IsAlnum(Char)) || FCString::Strchr(TEXT("*?,-/# "), Char) != nullptr; });
		TArray<FString> Fields;
		Trimmed.ParseIntoArray(Fields, TEXT(" "), true);
		if (!bOnlySyntax || (Fields.Num() != 5 && Fields.Num() != 6))
		{
			return FString::Printf(TEXT("Cron '%s' is not a schedule; use five fields (minute hour day-of-month month day-of-week), or six with seconds first"), *Trimmed);
		}
		static const FCronLimits Limits[] = {
			{TEXT("seconds"), TEXT("0 to 59"), 0, 59, nullptr, ECronField::Plain},
			{TEXT("minute"), TEXT("0 to 59"), 0, 59, nullptr, ECronField::Plain},
			{TEXT("hour"), TEXT("0 to 23"), 0, 23, nullptr, ECronField::Plain},
			{TEXT("day of the month"), TEXT("1 to 31, or L"), 1, 31, nullptr, ECronField::DayOfMonth},
			{TEXT("month"), TEXT("1 to 12 or JAN to DEC"), 1, 12, TEXT("JAN FEB MAR APR MAY JUN JUL AUG SEP OCT NOV DEC"), ECronField::Plain},
			{TEXT("day of the week"), TEXT("0 to 7 or SUN to SAT, such as 5L or 1#2"), 0, 7, TEXT("SUN MON TUE WED THU FRI SAT"), ECronField::DayOfWeek},
		};
		const int32 FirstLimit = UE_ARRAY_COUNT(Limits) - Fields.Num();
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			const FCronLimits& Field = Limits[FirstLimit + Index];
			if (!CronFieldFits(Fields[Index], Field))
			{
				return FString::Printf(TEXT("Cron '%s': the %s must be %s"), *Trimmed, Field.Label, Field.Range);
			}
		}
		return FString();
	}

	/** Methods holds every function's method, built-in ones included, since a timer's handler shares their namespace. */
	void CheckTimers(const UCrowdyServerObjectDefinition& Definition, const TSet<FString>& Methods, TArray<FString>& Errors)
	{
		if (Definition.IsFunctionLibrary())
		{
			return;
		}
		static const FString EventHandlers[] = {TEXT("on_player_joined"), TEXT("on_player_left")};
		static const FString Unwritable[] = {TEXT("self"), TEXT("super"), TEXT("crate")};
		TSet<FString> Seen;
		for (const FCrowdyServerTimer& Timer : Definition.Timers)
		{
			const FString Name = Timer.Name.ToString();
			const FString Handler = SnakeCase(Name);
			if (Timer.Name.IsNone())
			{
				Errors.Add(TEXT("Timers: a timer has no Name"));
			}
			else if (!IsServerName(Handler, 64, true) || MakeArrayView(Unwritable).Contains(Handler))
			{
				Errors.Add(FString::Printf(TEXT("Timers: %s: '%s' cannot be its name in the server code (Rust); rename the timer"), *Name, *Handler));
			}
			else if (Seen.Contains(Handler))
			{
				Errors.Add(FString::Printf(TEXT("Timers: two timers are named '%s'"), *Handler));
			}
			else if (Methods.Contains(Handler))
			{
				Errors.Add(FString::Printf(TEXT("Timers: %s: a function is also named '%s'"), *Name, *Handler));
			}
			else if (MakeArrayView(EventHandlers).Contains(Handler))
			{
				Errors.Add(FString::Printf(TEXT("Timers: %s: '%s' is kept for the On Player Joined and On Player Left events"), *Name, *Handler));
			}
			else if (CrowdyExec::IsReservedMethodName(Handler, false) || Handler.Equals(CrowdyExec::FlushTimerName, ESearchCase::CaseSensitive)
				|| Handler.Equals(CrowdyExec::SessionsTimerName, ESearchCase::CaseSensitive))
			{
				Errors.Add(FString::Printf(TEXT("Timers: %s: '%s' is reserved in the server code; rename the timer"), *Name, *Handler));
			}
			Seen.Add(Handler);
			if (Timer.Repeat == ECrowdyServerTimerRepeat::Schedule)
			{
				const FString Problem = CronProblem(Timer.Cron);
				if (!Problem.IsEmpty())
				{
					Errors.Add(FString::Printf(TEXT("Timers: %s: %s"), *Name, *Problem));
				}
				if (FMath::Abs(Timer.UtcOffsetMinutes) > CrowdyExec::MaxUtcOffsetMinutes)
				{
					Errors.Add(FString::Printf(TEXT("Timers: %s: UTC Offset must be from -%d to %d minutes"), *Name, CrowdyExec::MaxUtcOffsetMinutes, CrowdyExec::MaxUtcOffsetMinutes));
				}
				continue;
			}
			if (!(Timer.Seconds >= 0.01f))
			{
				Errors.Add(FString::Printf(TEXT("Timers: %s: Seconds must be at least 0.01"), *Name));
			}
			else if (!(Timer.Seconds <= static_cast<float>(CrowdyExec::MaxWaitSeconds)))
			{
				Errors.Add(FString::Printf(TEXT("Timers: %s: Seconds must be at most %d"), *Name, CrowdyExec::MaxWaitSeconds));
			}
		}
	}

#if WITH_EDITORONLY_DATA
	/** Label is the category the list is under, and SelfProblem what is wrong with a type naming itself. */
	void CheckTypeList(const UCrowdyServerObjectDefinition& Definition, const TArray<TObjectPtr<UCrowdyServerObjectDefinition>>& Types, const TCHAR* Label,
		const TCHAR* SelfProblem, TArray<FString>& Errors)
	{
		for (const TObjectPtr<UCrowdyServerObjectDefinition>& Type : Types)
		{
			if (!Type)
			{
				Errors.Add(FString::Printf(TEXT("%s: an entry is empty; choose a Server Object definition or remove the entry"), Label));
			}
			else if (Type.Get() == &Definition)
			{
				Errors.Add(FString::Printf(TEXT("%s: %s"), Label, SelfProblem));
			}
			else if (Type->TypeName.IsEmpty())
			{
				Errors.Add(FString::Printf(TEXT("%s: %s has no Type Name"), Label, *Type->GetName()));
			}
			else if (!IsServerName(Type->TypeName, 48, true))
			{
				Errors.Add(FString::Printf(TEXT("%s: %s has the Type Name '%s', which must be lowercase letters, digits and underscores, start with a letter, and be at most 48 characters"),
					Label, *Type->GetName(), *Type->TypeName));
			}
		}
	}

	/** A library sends no messages, so listening to one hears nothing. */
	void CheckListenToLibraries(const TArray<TObjectPtr<UCrowdyServerObjectDefinition>>& Types, TArray<FString>& Errors)
	{
		for (const TObjectPtr<UCrowdyServerObjectDefinition>& Type : Types)
		{
			if (Type && Type->IsFunctionLibrary())
			{
				Errors.Add(FString::Printf(TEXT("%s is a Server Function Library and sends no messages."), Type->TypeName.IsEmpty() ? *Type->GetName() : *Type->TypeName));
			}
		}
	}
#endif

	/** The messages' topics must be server names, not the SDK's own state topic, and differ from each other. */
	void CheckMessages(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& Errors)
	{
		if (Definition.IsFunctionLibrary())
		{
			return;
		}
		TSet<FString> Topics;
		TSet<FName> Names;
		for (const FCrowdyServerMessage& Message : Definition.Messages)
		{
			const FString Topic = CheckedTopic(Message);
			const FString Name = Message.Name.ToString();
			bool bNameTaken = false;
			Names.Add(Message.Name, &bNameTaken);
			if (Message.Name.IsNone())
			{
				Errors.Add(TEXT("Messages: a message has no Name"));
			}
			else if (bNameTaken)
			{
				Errors.Add(FString::Printf(TEXT("Message %s: another message has this name"), *Name));
			}
			else if (!IsServerName(Topic, 64, true))
			{
				Errors.Add(FString::Printf(TEXT("Message %s: '%s' cannot be its topic, which must be lowercase letters, digits and underscores, start with a letter, and be at most 64 characters"), *Name, *Topic));
			}
			else if (Topic.Equals(TEXT("state"), ESearchCase::CaseSensitive))
			{
				Errors.Add(FString::Printf(TEXT("Message %s: '%s' is reserved for the variables players watch; rename the message"), *Name, *Topic));
			}
			else if (Topics.Contains(Topic))
			{
				Errors.Add(FString::Printf(TEXT("Message %s: another message is also sent on the topic '%s'"), *Name, *Topic));
			}
			Topics.Add(Topic);
		}
	}

	struct FRangeBound
	{
		double Real = 0.0;
		int64 Whole = 0;
	};

	/** Reads one set bound, or says why it is not one. A List input's text must be as the editor saves it; a C++ struct's may be UE metadata such as 0.0. */
	bool ReadRangeBound(const FString& Input, const FString& Text, bool bWhole, bool bListInput, FRangeBound& Out, TArray<FString>& Errors)
	{
		const bool bNumber = bListInput ? CrowdyExec::ParseRangeNumber(Text, Out.Real) : FCString::IsNumeric(*Text);
		Out.Real = bNumber ? FCString::Atod(*Text) : 0.0;
		if (!bNumber || !FMath::IsFinite(Out.Real))
		{
			Errors.Add(FString::Printf(TEXT("%s: Value Range '%s' is not a number"), *Input, *Text));
			return false;
		}
		if (!bWhole)
		{
			return true;
		}
		const bool bWholeValue = bListInput ? CrowdyExec::ParseRangeWhole(Text, Out.Whole) : FMath::Frac(Out.Real) == 0.0 && FMath::Abs(Out.Real) <= 9.0e18;
		if (!bWholeValue)
		{
			// A whole number that did not fit is too large, not a fraction.
			const bool bTooLarge = FMath::Frac(Out.Real) == 0.0 && FMath::Abs(Out.Real) > 9.0e18;
			Errors.Add(bTooLarge
				? FString::Printf(TEXT("%s: Value Range '%s' is too large for the input"), *Input, *Text)
				: FString::Printf(TEXT("%s: Value Range '%s' is not a whole number, and the input is"), *Input, *Text));
			return false;
		}
		Out.Whole = bListInput ? Out.Whole : static_cast<int64>(Out.Real);
		return true;
	}

	/** Min and Max are the ClampMin and ClampMax text of a number input; empty is unset. Whole bounds compare exactly. */
	void CheckValueRange(const FString& Input, bool bWhole, bool bListInput, const FString& Min, const FString& Max, TArray<FString>& Errors)
	{
		FRangeBound Low;
		FRangeBound High;
		if (!Min.IsEmpty() && !ReadRangeBound(Input, Min, bWhole, bListInput, Low, Errors))
		{
			return;
		}
		if (!Max.IsEmpty() && !ReadRangeBound(Input, Max, bWhole, bListInput, High, Errors))
		{
			return;
		}
		const bool bReversed = bWhole ? Low.Whole > High.Whole : Low.Real > High.Real;
		if (!Min.IsEmpty() && !Max.IsEmpty() && bReversed)
		{
			Errors.Add(FString::Printf(TEXT("%s: Value Range minimum %s is above its maximum %s"), *Input, *Min, *Max));
		}
	}

	void CheckListRanges(const FCrowdyServerFunction& Function, TArray<FString>& Errors)
	{
#if WITH_EDITOR
		const UPropertyBag* Bag = Function.ParamsList.GetPropertyBagStruct();
		if (Function.ParamsForm != ECrowdyServerValuesForm::List || !Bag)
		{
			return;
		}
		for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyDescs())
		{
			const FString Input = FString::Printf(TEXT("Function %s: %s"), *Function.Name.ToString(), *Desc.Name.ToString());
			const FString Min = Desc.GetMetaData(TEXT("ClampMin"));
			const FString Max = Desc.GetMetaData(TEXT("ClampMax"));
			const bool bNumber = Desc.ContainerTypes.IsEmpty() && (Desc.IsNumericIntegralType() || Desc.IsNumericFloatType());
			if (!bNumber && !(Min.IsEmpty() && Max.IsEmpty()))
			{
				Errors.Add(FString::Printf(TEXT("%s has a Value Range, which only a number input can have"), *Input));
				continue;
			}
			CheckValueRange(Input, Desc.IsNumericIntegralType(), true, Min, Max, Errors);
		}
#endif
	}

	void CheckStructRanges(const FCrowdyServerFunction& Function, TArray<FString>& Errors)
	{
#if WITH_METADATA
		if (Function.ParamsForm != ECrowdyServerValuesForm::Struct || !Function.Params)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Function.Params.Get()); It; ++It)
		{
			// Only a plain number has a Value Range; UE's own ClampMin on a vector, container, optional or enum is left alone.
			const FNumericProperty* Number = CastField<FNumericProperty>(*It);
			if (!CrowdyExec::IsTravellingProperty(*It) || !Number || Number->IsEnum())
			{
				continue;
			}
			CheckValueRange(FString::Printf(TEXT("Function %s: %s"), *Function.Name.ToString(), *It->GetAuthoredName()), Number->IsInteger(), false,
				It->GetMetaData(TEXT("ClampMin")), It->GetMetaData(TEXT("ClampMax")), Errors);
		}
#endif
	}

	/** Out starts as the values a function sends or replies with: List's own when the form is List, otherwise Struct's defaults. */
	void InitializeFunctionValues(ECrowdyServerValuesForm Form, const UScriptStruct* Struct, const FInstancedPropertyBag& List, FInstancedStruct& Out)
	{
		const UScriptStruct* Picked = PickValues(Form, Struct, List);
		if (!Picked)
		{
			Out.Reset();
			return;
		}
		Out.InitializeAs(Picked, Form == ECrowdyServerValuesForm::List ? List.GetValue().GetMemory() : nullptr);
	}
}

FString FCrowdyServerFunction::GetMethodName() const
{
	return BakedMethodName.IsEmpty() ? AuthoredMethodName(*this) : BakedMethodName;
}

const UScriptStruct* FCrowdyServerFunction::GetParamsStruct() const
{
	return PickValues(ParamsForm, Params, ParamsList);
}

const UScriptStruct* FCrowdyServerFunction::GetReplyStruct() const
{
	return PickValues(ReplyForm, Reply, ReplyList);
}

void FCrowdyServerFunction::InitializeParams(FInstancedStruct& Out) const
{
	InitializeFunctionValues(ParamsForm, Params, ParamsList, Out);
}

void FCrowdyServerFunction::InitializeReply(FInstancedStruct& Out) const
{
	InitializeFunctionValues(ReplyForm, Reply, ReplyList, Out);
}

FString FCrowdyServerFunction::GetParamsListName() const
{
	return ParamsForm == ECrowdyServerValuesForm::List ? KeepNameCharacters(Name.ToString()) + TEXT("Params") : FString();
}

FString FCrowdyServerFunction::GetReplyListName() const
{
	return ReplyForm == ECrowdyServerValuesForm::List ? KeepNameCharacters(Name.ToString()) + TEXT("Reply") : FString();
}

FString FCrowdyServerMessage::GetTopic() const
{
	return BakedTopic.IsEmpty() ? AuthoredTopic(*this) : BakedTopic;
}

const UScriptStruct* FCrowdyServerMessage::GetPayloadStruct() const
{
	return PickValues(PayloadForm, Payload, PayloadList);
}

void FCrowdyServerMessage::InitializePayload(FInstancedStruct& Out) const
{
	InitializeFunctionValues(PayloadForm, Payload, PayloadList, Out);
}

FString FCrowdyServerMessage::GetPayloadListName() const
{
	return PayloadForm == ECrowdyServerValuesForm::List ? KeepNameCharacters(Name.ToString()) + TEXT("Message") : FString();
}

const UScriptStruct* UCrowdyServerObjectDefinition::GetStateStruct() const
{
	if (IsFunctionLibrary())
	{
		return nullptr;
	}
	return PickValues(StateForm, State, StateList);
}

const FProperty* UCrowdyServerObjectDefinition::FindVariable(FName Variable) const
{
	return CrowdyExec::FindTravellingField(GetStateStruct(), Variable);
}

void UCrowdyServerObjectDefinition::InitializeValues(const UScriptStruct* Struct, FInstancedStruct& Out) const
{
	if (!Struct)
	{
		Out.Reset();
		return;
	}
	const FInstancedPropertyBag* List = FindListValues(*this, Struct);
	Out.InitializeAs(Struct, List ? List->GetValue().GetMemory() : nullptr);
}

FString UCrowdyServerObjectDefinition::FindListName(const UScriptStruct* Struct) const
{
	if (!Struct)
	{
		return FString();
	}
	TArray<FCrowdyServerNamedList> Lists;
	GetLists(Lists);
	const FCrowdyServerNamedList* Found = Lists.FindByPredicate([Struct](const FCrowdyServerNamedList& List) { return List.Values->GetPropertyBagStruct() == Struct; });
	return Found ? Found->Name : FString();
}

const UScriptStruct* UCrowdyServerObjectDefinition::FindListStruct(const FString& Name) const
{
	TArray<FCrowdyServerNamedList> Lists;
	GetLists(Lists);
	// A cooked build keeps one spelling per FName, so a function named Hit may read back as hit; the bake refuses names differing only in case.
	const FCrowdyServerNamedList* Found = Lists.FindByPredicate([&Name](const FCrowdyServerNamedList& List) { return List.Name.Equals(Name, ESearchCase::IgnoreCase); });
	return Found ? Found->Values->GetPropertyBagStruct() : nullptr;
}

void UCrowdyServerObjectDefinition::GetLists(TArray<FCrowdyServerNamedList>& Out) const
{
	const bool bLibrary = IsFunctionLibrary();
	if (!bLibrary && StateForm == ECrowdyServerValuesForm::List)
	{
		Out.Add({PascalCase(TypeName) + TEXT("State"), &StateList});
	}
	for (const FCrowdyServerFunction& Function : Functions)
	{
		if (Function.ParamsForm == ECrowdyServerValuesForm::List)
		{
			Out.Add({Function.GetParamsListName(), &Function.ParamsList});
		}
		if (Function.ReplyForm == ECrowdyServerValuesForm::List)
		{
			Out.Add({Function.GetReplyListName(), &Function.ReplyList});
		}
	}
	if (bLibrary)
	{
		return;
	}
	for (const FCrowdyServerMessage& Message : Messages)
	{
		if (Message.PayloadForm == ECrowdyServerValuesForm::List)
		{
			Out.Add({Message.GetPayloadListName(), &Message.PayloadList});
		}
	}
}

const FCrowdyServerMessage* UCrowdyServerObjectDefinition::FindMessage(FName Name) const
{
	if (IsFunctionLibrary())
	{
		return nullptr;
	}
	return Messages.FindByPredicate([Name](const FCrowdyServerMessage& Message) { return Message.Name == Name; });
}

const FCrowdyServerMessage* UCrowdyServerObjectDefinition::FindMessageByTopic(const FString& Topic) const
{
	if (IsFunctionLibrary())
	{
		return nullptr;
	}
	return Messages.FindByPredicate([&Topic](const FCrowdyServerMessage& Message)
	{
		return Message.BakedTopic.IsEmpty() ? Message.GetTopic().Equals(Topic, ESearchCase::CaseSensitive) : Message.BakedTopic.Equals(Topic, ESearchCase::CaseSensitive);
	});
}

const FCrowdyServerFunction* UCrowdyServerObjectDefinition::FindFunction(FName Name) const
{
	auto Named = [Name](const FCrowdyServerFunction& Function) { return Function.Name == Name; };
	if (const FCrowdyServerFunction* Own = Functions.FindByPredicate(Named))
	{
		return Own;
	}
	if (IsFunctionLibrary() || MembersFrom != ECrowdyServerMembersSource::ThisObject)
	{
		return nullptr;
	}
	return GetMemberFunctions().FindByPredicate(Named);
}

TConstArrayView<FCrowdyServerFunction> UCrowdyServerObjectDefinition::GetMemberFunctions()
{
	static const TArray<FCrowdyServerFunction> MemberFunctions = []()
	{
		auto Make = [](const TCHAR* Name, const TCHAR* Method, UScriptStruct* Inputs, ECrowdyServerFunctionCaller Caller)
		{
			FCrowdyServerFunction Function;
			Function.Name = Name;
			Function.ServerName = Method;
			Function.Params = Inputs;
			Function.WhoCanCall = Caller;
			return Function;
		};
		UScriptStruct* Member = FCrowdyServerMemberInputs::StaticStruct();
		return TArray<FCrowdyServerFunction>{
			Make(TEXT("Join"), TEXT("join"), nullptr, ECrowdyServerFunctionCaller::Players),
			Make(TEXT("Leave"), TEXT("leave"), nullptr, ECrowdyServerFunctionCaller::Players),
			Make(TEXT("AddMember"), TEXT("add_member"), Member, ECrowdyServerFunctionCaller::ServerOnly),
			Make(TEXT("RemoveMember"), TEXT("remove_member"), Member, ECrowdyServerFunctionCaller::Leader),
			Make(TEXT("MakeLeader"), TEXT("make_leader"), Member, ECrowdyServerFunctionCaller::Leader),
			Make(TEXT("SetOpenForJoining"), TEXT("set_open_for_joining"), FCrowdyServerOpenInputs::StaticStruct(), ECrowdyServerFunctionCaller::Leader)};
	}();
	return MemberFunctions;
}

FString UCrowdyServerObjectDefinition::MakeAreaKey(int64 ChunkX, int64 ChunkY, int32 AreaSize)
{
	const int64 Size = FMath::Max(AreaSize, 1);
	auto Floor = [Size](int64 Chunk) { return Chunk / Size - ((Chunk % Size != 0 && Chunk < 0) ? 1 : 0); };
	return FString::Printf(TEXT("%lld,%lld"), Floor(ChunkX), Floor(ChunkY));
}

FString UCrowdyServerObjectDefinition::GetAreaKey(int64 ChunkX, int64 ChunkY) const
{
	return MakeAreaKey(ChunkX, ChunkY, AreaSizeChunks);
}

bool UCrowdyServerObjectDefinition::IsReachableByPlayers() const
{
	const bool bPlayerCallable = Functions.ContainsByPredicate([](const FCrowdyServerFunction& Function)
	{
		return Function.WhoCanCall != ECrowdyServerFunctionCaller::ServerOnly && Function.WhoCanCall != ECrowdyServerFunctionCaller::DevelopersOnly;
	});
	if (IsFunctionLibrary())
	{
		return bPlayerCallable;
	}
	// Who may call and read a starter is its server code's choice.
	if (IsGameKitStarter())
	{
		return true;
	}
	// Records reach players without a watched variable: each carries its entity, owner and team.
	return bPlayerCallable || !WatchedFields.IsEmpty() || Holds == ECrowdyServerObjectHolds::RecordPerEntity || MembersFrom != ECrowdyServerMembersSource::None
		|| bOnPlayerJoined || bOnPlayerLeft || HasPlayerMessages();
}

bool UCrowdyServerObjectDefinition::HasPlayerMessages() const
{
	return !IsFunctionLibrary() && Messages.ContainsByPredicate([](const FCrowdyServerMessage& Message) { return Message.bPlayersCanListen; });
}

bool UCrowdyServerObjectDefinition::HasRestrictedReaders() const
{
	return !IsFunctionLibrary() && (Visibility != ECrowdyServerObjectVisibility::Public || !OwnerFields.IsEmpty() || !TeamFields.IsEmpty());
}

bool UCrowdyServerObjectDefinition::IsMemberFunction(FName Name)
{
	return GetMemberFunctions().ContainsByPredicate([Name](const FCrowdyServerFunction& Function) { return Function.Name == Name; });
}

bool UCrowdyServerObjectDefinition::BuildTables(TArray<FCrowdyExecBakedStruct>& OutStructs, TArray<FCrowdyExecBakedEnum>& OutEnums,
	TArray<FString>& OutErrors) const
{
	OutStructs.Reset();
	OutEnums.Reset();
	const int32 ErrorsBefore = OutErrors.Num();
	if (!IsServerName(TypeName, 48, true))
	{
		OutErrors.Add(FString::Printf(TEXT("Type Name '%s' must be lowercase letters, digits and underscores, start with a letter, and be at most 48 characters"), *TypeName));
	}
	const bool bLibrary = IsFunctionLibrary();
	CheckLibrary(*this, OutErrors);
	CheckGameKitStarter(*this, OutErrors);
	CheckMembers(*this, OutErrors);
	CheckRecords(*this, OutErrors);
#if WITH_EDITORONLY_DATA
	CheckTypeList(*this, CanCall, TEXT("Can Call"), TEXT("a type cannot call itself"), OutErrors);
	if (!bLibrary)
	{
		CheckTypeList(*this, ListenTo, TEXT("Listen To"), TEXT("a type cannot listen to itself"), OutErrors);
		CheckListenToLibraries(ListenTo, OutErrors);
	}
#endif
	CheckMessages(*this, OutErrors);
	const UScriptStruct* StateStruct = GetStateStruct();
	if (!StateStruct && !bLibrary)
	{
		OutErrors.Add(StateForm == ECrowdyServerValuesForm::List ? TEXT("Add at least one variable, or use a State struct") : TEXT("Choose a State struct"));
	}
	TArray<FCrowdyServerNamedList> Lists;
	GetLists(Lists);
	TSet<FString> ListNames;
	for (const FCrowdyServerNamedList& List : Lists)
	{
		bool bTaken = false;
		ListNames.Add(List.Name, &bTaken);
		if (bTaken)
		{
			OutErrors.Add(FString::Printf(TEXT("Two lists are named %s; give their functions different names"), *List.Name));
		}
	}

	FTableBuilder Builder{*this, OutErrors};
	if (StateStruct)
	{
		Builder.Structs.Add(StateStruct);
	}
	if (HasKitCollection())
	{
		Builder.Structs.AddUnique(KitRecord);
	}
	TSet<FString> Methods;
	for (const FCrowdyServerFunction& Function : Functions)
	{
		const FString Method = CheckedMethodName(Function);
		if (!IsServerName(Method, 64, true))
		{
			OutErrors.Add(FString::Printf(TEXT("Function %s: '%s' cannot be its server name, which must be lowercase letters, digits and underscores, start with a letter, and be at most 64 characters"),
				*Function.Name.ToString(), *Method));
		}
		else if (CrowdyExec::IsReservedMethodName(Method, false))
		{
			OutErrors.Add(FString::Printf(TEXT("Function %s: '%s' is reserved"), *Function.Name.ToString(), *Method));
		}
		else if (Methods.Contains(Method))
		{
			OutErrors.Add(FString::Printf(TEXT("Function %s: another function is also named '%s'"), *Function.Name.ToString(), *Method));
		}
		Methods.Add(Method);
		if (!(Function.CooldownSeconds <= static_cast<float>(CrowdyExec::MaxWaitSeconds)))
		{
			OutErrors.Add(FString::Printf(TEXT("Function %s: Cooldown must be at most %d seconds"), *Function.Name.ToString(), CrowdyExec::MaxWaitSeconds));
		}
		CheckRequiredFeature(Function, OutErrors);
		CheckListRanges(Function, OutErrors);
		CheckStructRanges(Function, OutErrors);
		if (const UScriptStruct* ParamsStruct = Function.GetParamsStruct())
		{
			Builder.Structs.AddUnique(ParamsStruct);
		}
		if (const UScriptStruct* ReplyStruct = Function.GetReplyStruct())
		{
			Builder.Structs.AddUnique(ReplyStruct);
		}
	}
	const TConstArrayView<FCrowdyServerMessage> OwnMessages = bLibrary ? TConstArrayView<FCrowdyServerMessage>() : TConstArrayView<FCrowdyServerMessage>(Messages);
	for (const FCrowdyServerMessage& Message : OwnMessages)
	{
		if (const UScriptStruct* PayloadStruct = Message.GetPayloadStruct())
		{
			Builder.Structs.AddUnique(PayloadStruct);
		}
	}
	const bool bBuiltIns = !bLibrary && MembersFrom == ECrowdyServerMembersSource::ThisObject;
	const TConstArrayView<FCrowdyServerFunction> BuiltIns = bBuiltIns ? GetMemberFunctions() : TConstArrayView<FCrowdyServerFunction>();
	for (const FCrowdyServerFunction& BuiltIn : BuiltIns)
	{
		Methods.Add(BuiltIn.GetMethodName());
		if (const UScriptStruct* ParamsStruct = BuiltIn.GetParamsStruct())
		{
			Builder.Structs.AddUnique(ParamsStruct);
		}
	}
	CheckTimers(*this, Methods, OutErrors);

	// Only the State, Params, Reply and Payload structs are listed yet; baking adds the structs they reach.
	for (const UScriptStruct* Root : Builder.Structs)
	{
		if (StructDepth(Root, 1) > CrowdyExec::MaxDepth)
		{
			OutErrors.Add(FString::Printf(TEXT("%s nests structs and containers more than %d levels deep"), *Builder.Describe(Root), CrowdyExec::MaxDepth));
		}
	}
	for (int32 Index = 0; Index < Builder.Structs.Num(); ++Index)
	{
		Builder.BakeStruct(Builder.Structs[Index], OutStructs.AddDefaulted_GetRef());
	}
	for (const UEnum* Enum : Builder.Enums)
	{
		Builder.BakeEnum(Enum, OutEnums.AddDefaulted_GetRef());
	}
	const TConstArrayView<FName> Visible = bLibrary || IsGameKitStarter() ? TConstArrayView<FName>() : TConstArrayView<FName>(WatchedFields);
	for (const FName& Watched : Visible)
	{
		if (!Builder.WatchedFound.Contains(Watched))
		{
			OutErrors.Add(FString::Printf(TEXT("Watched field '%s' is not a field of %s"), *Watched.ToString(), *Builder.Describe(StateStruct)));
		}
	}
	if (!bLibrary && Holds == ECrowdyServerObjectHolds::RecordPerEntity)
	{
		Builder.CheckAudience(OwnerFields, TEXT("Owner Only"), TEXT("Team Only"), TeamFields, StateStruct);
		Builder.CheckAudience(TeamFields, TEXT("Team Only"), TEXT("Owner Only"), TArray<FName>(), StateStruct);
	}
	for (const FCrowdyServerFieldName& Override : FieldNames)
	{
		if (!HasField(Override.Struct.Get(), Override.Field))
		{
			OutErrors.Add(FString::Printf(TEXT("Server Names: %s has no field '%s'"), *CrowdyExec::DisplayName(Override.Struct.Get()), *Override.Field.ToString()));
		}
	}
	for (const FCrowdyServerEnumValueName& Override : EnumValueNames)
	{
		if (!HasValue(Override.Enum.Get(), Override.Value))
		{
			OutErrors.Add(FString::Printf(TEXT("Server Names: %s has no value '%s'"), *GetNameSafe(Override.Enum.Get()), *Override.Value.ToString()));
		}
	}

	if (OutErrors.Num() == ErrorsBefore)
	{
		return true;
	}
	OutStructs.Reset();
	OutEnums.Reset();
	return false;
}

bool UCrowdyServerObjectDefinition::Bake(TArray<FString>& OutErrors)
{
	for (FCrowdyServerFunction& Function : Functions)
	{
		Function.BakedMethodName = AuthoredMethodName(Function);
	}
	for (FCrowdyServerMessage& Message : Messages)
	{
		Message.BakedTopic = AuthoredTopic(Message);
	}
	const bool bBuilt = BuildTables(BakedStructs, BakedEnums, OutErrors);
	Layout.Reset();
	return bBuilt;
}

const FCrowdyExecLayout* UCrowdyServerObjectDefinition::ResolveLayout(FString& OutError) const
{
	const uint32 Generation = CrowdyExec::GetStructGeneration();
	if (Layout.IsValid() && LayoutGeneration == Generation)
	{
		return Layout.Get();
	}
	TSharedPtr<FCrowdyExecLayout> Resolved = MakeShared<FCrowdyExecLayout>();
	if (!CrowdyExec::BuildLayout(*this, *Resolved, OutError))
	{
		Layout.Reset();
		return nullptr;
	}
	Layout = MoveTemp(Resolved);
	LayoutGeneration = Generation;
	return Layout.Get();
}

#if WITH_EDITOR
bool UCrowdyServerObjectDefinition::IsServerValueTypeAccepted(FEdGraphPinType PinType, bool bIsChild) const
{
	static const FName Accepted[] = {TEXT("bool"), TEXT("byte"), TEXT("int"), TEXT("int64"), TEXT("real"), TEXT("name"), TEXT("string"),
		TEXT("struct"), TEXT("enum"), TEXT("softobject"), TEXT("softclass")};
	if (!bIsChild)
	{
		return MakeArrayView(Accepted).Contains(PinType.PinCategory);
	}
	const UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
	return !Struct || (Struct != FInstancedStruct::StaticStruct() && Struct != FInstancedPropertyBag::StaticStruct()
		&& Struct->GetFName() != FName(TEXT("GameplayTagContainer")));
}

void UCrowdyServerObjectDefinition::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);
	TArray<FString> Errors;
	if (Bake(Errors))
	{
		return;
	}
	// An error fails the cook, so a definition that cannot bake never ships; an editor save only warns.
	for (const FString& Problem : Errors)
	{
		UE_CLOG(SaveContext.IsCooking(), LogCrowdyExec, Error, TEXT("%s: %s"), *GetPathName(), *Problem);
		UE_CLOG(!SaveContext.IsCooking(), LogCrowdyExec, Warning, TEXT("%s: %s"), *GetPathName(), *Problem);
	}
}

void UCrowdyServerObjectDefinition::KeepListValueServerName(FName List, const FGuid& ValueId, const FString& ServerName)
{
	Modify();
	FCrowdyServerListValueName* Entry = ListValueNames.FindByPredicate([List, &ValueId](const FCrowdyServerListValueName& Candidate)
	{
		return Candidate.List == List && Candidate.ValueId == ValueId;
	});
	Entry = Entry ? Entry : &ListValueNames.AddDefaulted_GetRef();
	Entry->List = List;
	Entry->ValueId = ValueId;
	Entry->ServerName = ServerName;
	PostEditChange();
}

void UCrowdyServerObjectDefinition::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	TArray<FString> Errors;
	Bake(Errors);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, KitActors))
	{
		SyncGameKitActorStarters();
	}
}

void UCrowdyServerObjectDefinition::SyncGameKitActorStarters() const
{
	// A packaged game draws only the starters this setting lists, and cooks them for it.
	UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
	const FSoftObjectPath Path(this);
	const bool bListed = Settings->GameKitActorStarters.Contains(Path);
	if (bListed == !KitActors.IsEmpty())
	{
		return;
	}
	if (bListed)
	{
		Settings->GameKitActorStarters.Remove(Path);
	}
	else
	{
		Settings->GameKitActorStarters.Add(Path);
	}
	Settings->TryUpdateDefaultConfigFile();
}

EDataValidationResult UCrowdyServerObjectDefinition::IsDataValid(FDataValidationContext& Context) const
{
	if (CodeSource == ECrowdyServerCodeSource::OwnFile && LogicFile.FilePath.IsEmpty())
	{
		Context.AddWarning(FText::FromString(TEXT("Code Source is My Own File but no file is chosen; choose one, or switch Code Source to Generated")));
	}
	TArray<FCrowdyExecBakedStruct> Structs;
	TArray<FCrowdyExecBakedEnum> Enums;
	TArray<FString> Errors;
	if (BuildTables(Structs, Enums, Errors))
	{
		return Super::IsDataValid(Context);
	}
	for (const FString& Error : Errors)
	{
		Context.AddError(FText::FromString(Error));
	}
	return EDataValidationResult::Invalid;
}
#endif
