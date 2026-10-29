#include "CrowdyServerObjectLibrary.h"

#include "Components/ActorComponent.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyGameKitTypes.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/ScriptMacros.h"
#include "UObject/UnrealType.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyServerObjectLibraryDetail
{
	UGameInstance* FindGameInstance(const UObject* WorldContextObject)
	{
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		return World ? World->GetGameInstance() : nullptr;
	}

	/** The integer an enum value is held in, and its enum: an enum property's underlying one, or a byte of an enum. */
	const FNumericProperty* FindEnumNumber(const FProperty* Property, const UEnum*& OutEnum)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			OutEnum = EnumProperty->GetEnum();
			return EnumProperty->GetUnderlyingProperty();
		}
		const FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
		OutEnum = ByteProperty ? ByteProperty->Enum.Get() : nullptr;
		return OutEnum ? ByteProperty : nullptr;
	}

	bool IsUnsigned(const FNumericProperty* Number)
	{
		return Number->IsA<FByteProperty>() || Number->IsA<FUInt16Property>() || Number->IsA<FUInt32Property>() || Number->IsA<FUInt64Property>();
	}

	/** Property as an integer that converts to other sizes and signs; null for anything else, enums included. */
	const FNumericProperty* AsInteger(const FProperty* Property)
	{
		const FNumericProperty* Number = CastField<FNumericProperty>(Property);
		return Number && Number->IsInteger() && !Number->IsEnum() ? Number : nullptr;
	}

	/** Whether the integer at Source fits DestNumber's size and sign. */
	bool IntegerFits(const FNumericProperty* DestNumber, const FNumericProperty* SourceNumber, const void* Source)
	{
		const int32 DestBits = DestNumber->GetElementSize() * 8;
		const bool bSourceUnsigned = IsUnsigned(SourceNumber);
		const uint64 Unsigned = bSourceUnsigned ? SourceNumber->GetUnsignedIntPropertyValue(Source) : 0;
		const int64 Signed = bSourceUnsigned ? 0 : SourceNumber->GetSignedIntPropertyValue(Source);
		if (IsUnsigned(DestNumber))
		{
			const uint64 Max = DestBits >= 64 ? MAX_uint64 : (uint64(1) << DestBits) - 1;
			return bSourceUnsigned ? Unsigned <= Max : Signed >= 0 && static_cast<uint64>(Signed) <= Max;
		}
		const int64 Max = DestBits >= 64 ? MAX_int64 : (int64(1) << (DestBits - 1)) - 1;
		return bSourceUnsigned ? Unsigned <= static_cast<uint64>(Max) : Signed >= -Max - 1 && Signed <= Max;
	}

	/** Writes the integer at Source into Dest, which IntegerFits has allowed. */
	void WriteInteger(const FNumericProperty* DestNumber, void* Dest, const FNumericProperty* SourceNumber, const void* Source)
	{
		if (IsUnsigned(SourceNumber))
		{
			DestNumber->SetIntPropertyValue(Dest, SourceNumber->GetUnsignedIntPropertyValue(Source));
			return;
		}
		DestNumber->SetIntPropertyValue(Dest, SourceNumber->GetSignedIntPropertyValue(Source));
	}

	/** Copies an integer into an integer of another size or sign when it fits. False, writing nothing, otherwise. */
	bool CopyInteger(const FNumericProperty* DestNumber, void* Dest, const FNumericProperty* SourceNumber, const void* Source)
	{
		if (!IntegerFits(DestNumber, SourceNumber, Source))
		{
			return false;
		}
		WriteInteger(DestNumber, Dest, SourceNumber, Source);
		return true;
	}

	bool CopyValue(const FProperty* DestProperty, void* Dest, const FProperty* SourceProperty, const void* Source);

	/** Whether container elements of SourceProperty go into DestProperty one by one: one enum in either form, reals, integers, or the same type. */
	bool ElementsConvert(const FProperty* DestProperty, const FProperty* SourceProperty)
	{
		const UEnum* DestEnum = nullptr;
		const UEnum* SourceEnum = nullptr;
		const bool bDestEnum = FindEnumNumber(DestProperty, DestEnum) != nullptr;
		const bool bSourceEnum = FindEnumNumber(SourceProperty, SourceEnum) != nullptr;
		if (bDestEnum || bSourceEnum)
		{
			return DestEnum == SourceEnum;
		}
		const FNumericProperty* DestNumeric = CastField<FNumericProperty>(DestProperty);
		const FNumericProperty* SourceNumeric = CastField<FNumericProperty>(SourceProperty);
		const bool bReals = DestNumeric && SourceNumeric && DestNumeric->IsFloatingPoint() && SourceNumeric->IsFloatingPoint();
		return bReals || (AsInteger(DestProperty) && AsInteger(SourceProperty)) || DestProperty->SameType(SourceProperty);
	}

	/** Whether one element of types ElementsConvert allows fits: always, unless it is an integer out of range. */
	bool ElementFits(const FProperty* DestProperty, const FProperty* SourceProperty, const void* Source)
	{
		const FNumericProperty* DestNumber = AsInteger(DestProperty);
		const FNumericProperty* SourceNumber = AsInteger(SourceProperty);
		return !DestNumber || !SourceNumber || IntegerFits(DestNumber, SourceNumber, Source);
	}

	// Each container copy checks every element before writing any, so one that does not fit leaves Dest as it was.
	bool CopyArray(const FArrayProperty* DestArray, void* Dest, const FArrayProperty* SourceArray, const void* Source)
	{
		FScriptArrayHelper From(SourceArray, Source);
		for (int32 Index = 0; Index < From.Num(); ++Index)
		{
			if (!ElementFits(DestArray->Inner, SourceArray->Inner, From.GetRawPtr(Index)))
			{
				return false;
			}
		}
		FScriptArrayHelper To(DestArray, Dest);
		To.Resize(From.Num());
		for (int32 Index = 0; Index < From.Num(); ++Index)
		{
			CopyValue(DestArray->Inner, To.GetRawPtr(Index), SourceArray->Inner, From.GetRawPtr(Index));
		}
		return true;
	}

	bool CopySet(const FSetProperty* DestSet, void* Dest, const FSetProperty* SourceSet, const void* Source)
	{
		FScriptSetHelper From(SourceSet, Source);
		for (FScriptSetHelper::FIterator It = From.CreateIterator(); It; ++It)
		{
			if (!ElementFits(DestSet->ElementProp, SourceSet->ElementProp, From.GetElementPtr(It)))
			{
				return false;
			}
		}
		FScriptSetHelper To(DestSet, Dest);
		To.EmptyElements(From.Num());
		for (FScriptSetHelper::FIterator It = From.CreateIterator(); It; ++It)
		{
			const int32 Added = To.AddDefaultValue_Invalid_NeedsRehash();
			CopyValue(DestSet->ElementProp, To.GetElementPtr(Added), SourceSet->ElementProp, From.GetElementPtr(It));
		}
		To.Rehash();
		return true;
	}

	bool CopyMap(const FMapProperty* DestMap, void* Dest, const FMapProperty* SourceMap, const void* Source)
	{
		FScriptMapHelper From(SourceMap, Source);
		for (FScriptMapHelper::FIterator It = From.CreateIterator(); It; ++It)
		{
			if (!ElementFits(DestMap->KeyProp, SourceMap->KeyProp, From.GetKeyPtr(It)) || !ElementFits(DestMap->ValueProp, SourceMap->ValueProp, From.GetValuePtr(It)))
			{
				return false;
			}
		}
		FScriptMapHelper To(DestMap, Dest);
		To.EmptyValues(From.Num());
		for (FScriptMapHelper::FIterator It = From.CreateIterator(); It; ++It)
		{
			const int32 Added = To.AddDefaultValue_Invalid_NeedsRehash();
			CopyValue(DestMap->KeyProp, To.GetKeyPtr(Added), SourceMap->KeyProp, From.GetKeyPtr(It));
			CopyValue(DestMap->ValueProp, To.GetValuePtr(Added), SourceMap->ValueProp, From.GetValuePtr(It));
		}
		To.Rehash();
		return true;
	}

	/** A container whose elements differ from its counterpart's as scalars may, converted element by element. False, writing nothing, otherwise. */
	bool CopyContainer(const FProperty* DestProperty, void* Dest, const FProperty* SourceProperty, const void* Source)
	{
		const FArrayProperty* DestArray = CastField<FArrayProperty>(DestProperty);
		const FArrayProperty* SourceArray = CastField<FArrayProperty>(SourceProperty);
		if (DestArray && SourceArray)
		{
			return ElementsConvert(DestArray->Inner, SourceArray->Inner) && CopyArray(DestArray, Dest, SourceArray, Source);
		}
		const FSetProperty* DestSet = CastField<FSetProperty>(DestProperty);
		const FSetProperty* SourceSet = CastField<FSetProperty>(SourceProperty);
		if (DestSet && SourceSet)
		{
			return ElementsConvert(DestSet->ElementProp, SourceSet->ElementProp) && CopySet(DestSet, Dest, SourceSet, Source);
		}
		const FMapProperty* DestMap = CastField<FMapProperty>(DestProperty);
		const FMapProperty* SourceMap = CastField<FMapProperty>(SourceProperty);
		const bool bMapsConvert = DestMap && SourceMap && ElementsConvert(DestMap->KeyProp, SourceMap->KeyProp) && ElementsConvert(DestMap->ValueProp, SourceMap->ValueProp);
		return bMapsConvert && CopyMap(DestMap, Dest, SourceMap, Source);
	}

	/** An optional on exactly one side: its value is copied, an empty one answers false, and a plain value sets an optional. */
	bool CopyOptional(const FProperty* DestProperty, void* Dest, const FProperty* SourceProperty, const void* Source)
	{
		if (const FOptionalProperty* SourceOptional = CastField<FOptionalProperty>(SourceProperty))
		{
			return SourceOptional->IsSet(Source) && CopyValue(DestProperty, Dest, SourceOptional->GetValueProperty(), SourceOptional->GetValuePointerForRead(Source));
		}
		const FOptionalProperty* DestOptional = CastFieldChecked<FOptionalProperty>(DestProperty);
		const bool bWasSet = DestOptional->IsSet(Dest);
		void* Value = DestOptional->MarkSetAndGetInitializedValuePointerToReplace(Dest);
		if (CopyValue(DestOptional->GetValueProperty(), Value, SourceProperty, Source))
		{
			return true;
		}
		if (!bWasSet)
		{
			DestOptional->MarkUnset(Dest);
		}
		return false;
	}

	/** SameType, except that bytes of different enums differ, inside containers and optionals too. */
	bool IsSameType(const FProperty* DestProperty, const FProperty* SourceProperty)
	{
		if (!DestProperty->SameType(SourceProperty))
		{
			return false;
		}
		if (const FByteProperty* DestByte = CastField<FByteProperty>(DestProperty))
		{
			return DestByte->Enum == CastFieldChecked<FByteProperty>(SourceProperty)->Enum;
		}
		if (const FArrayProperty* DestArray = CastField<FArrayProperty>(DestProperty))
		{
			return IsSameType(DestArray->Inner, CastFieldChecked<FArrayProperty>(SourceProperty)->Inner);
		}
		if (const FSetProperty* DestSet = CastField<FSetProperty>(DestProperty))
		{
			return IsSameType(DestSet->ElementProp, CastFieldChecked<FSetProperty>(SourceProperty)->ElementProp);
		}
		if (const FOptionalProperty* DestOptional = CastField<FOptionalProperty>(DestProperty))
		{
			return IsSameType(DestOptional->GetValueProperty(), CastFieldChecked<FOptionalProperty>(SourceProperty)->GetValueProperty());
		}
		const FMapProperty* DestMap = CastField<FMapProperty>(DestProperty);
		const FMapProperty* SourceMap = CastField<FMapProperty>(SourceProperty);
		return !DestMap || (IsSameType(DestMap->KeyProp, SourceMap->KeyProp) && IsSameType(DestMap->ValueProp, SourceMap->ValueProp));
	}

	/** Copies a value of the same type, a converted enum, integer, real or container of them, or into or out of an optional; false writes nothing. */
	bool CopyValue(const FProperty* DestProperty, void* Dest, const FProperty* SourceProperty, const void* Source)
	{
		const bool bDestOptional = CastField<FOptionalProperty>(DestProperty) != nullptr;
		const bool bSourceOptional = CastField<FOptionalProperty>(SourceProperty) != nullptr;
		if (bDestOptional != bSourceOptional)
		{
			return CopyOptional(DestProperty, Dest, SourceProperty, Source);
		}
		if (IsSameType(DestProperty, SourceProperty))
		{
			const FBoolProperty* DestBool = CastField<FBoolProperty>(DestProperty);
			if (!DestBool)
			{
				DestProperty->CopySingleValue(Dest, Source);
				return true;
			}
			// Either side may be a bitfield, so each side reads or writes through its own mask.
			DestBool->SetPropertyValue(Dest, CastFieldChecked<FBoolProperty>(SourceProperty)->GetPropertyValue(Source));
			return true;
		}
		if (DestProperty->IsA<FArrayProperty>() || DestProperty->IsA<FSetProperty>() || DestProperty->IsA<FMapProperty>())
		{
			return CopyContainer(DestProperty, Dest, SourceProperty, Source);
		}
		// Blueprint floats are doubles, so a C++ float value meets a double pin.
		const FNumericProperty* DestNumeric = CastField<FNumericProperty>(DestProperty);
		const FNumericProperty* SourceNumeric = CastField<FNumericProperty>(SourceProperty);
		if (DestNumeric && SourceNumeric && DestNumeric->IsFloatingPoint() && SourceNumeric->IsFloatingPoint())
		{
			DestNumeric->SetFloatingPointPropertyValue(Dest, SourceNumeric->GetFloatingPointPropertyValue(Source));
			return true;
		}
		// Blueprint integers are int32 and int64 only, so C++ integers of other sizes meet them.
		const bool bIntegers = DestNumeric && SourceNumeric && DestNumeric->IsInteger() && SourceNumeric->IsInteger();
		if (bIntegers && !DestNumeric->IsEnum() && !SourceNumeric->IsEnum())
		{
			return CopyInteger(DestNumeric, Dest, SourceNumeric, Source);
		}
		const UEnum* DestEnum = nullptr;
		const UEnum* SourceEnum = nullptr;
		const FNumericProperty* DestNumber = FindEnumNumber(DestProperty, DestEnum);
		const FNumericProperty* SourceNumber = FindEnumNumber(SourceProperty, SourceEnum);
		if (!DestNumber || !SourceNumber || DestEnum != SourceEnum)
		{
			return false;
		}
		DestNumber->SetIntPropertyValue(Dest, SourceNumber->GetSignedIntPropertyValue(Source));
		return true;
	}

	/** Where a target's per-variable handlers live, what owns them, the object it holds now, and the record it reads, if any. */
	struct FHolder
	{
		UCrowdyServerObject* Object = nullptr;
		FCrowdyServerVariableBindings* Bindings = nullptr;
		UObject* Owner = nullptr;
		const FString* RecordId = nullptr;
		FCrowdyServerMessageBindings* Messages = nullptr;
	};

	UCrowdyServerObjectComponent* FindOnlyComponent(AActor* Actor, const UCrowdyServerObjectDefinition* Definition)
	{
		UCrowdyServerObjectComponent* Found = nullptr;
		for (UCrowdyServerObjectComponent* Component : TInlineComponentArray<UCrowdyServerObjectComponent*>(Actor))
		{
			if (Definition && Component->Definition != Definition)
			{
				continue;
			}
			if (Found)
			{
				return nullptr;
			}
			Found = Component;
		}
		return Found;
	}

	FHolder FindHolder(const UObject* Target, const UCrowdyServerObjectDefinition* Definition)
	{
		UObject* Mutable = const_cast<UObject*>(Target);
		auto Fits = [Definition](const UCrowdyServerObjectDefinition* Held) { return !Definition || Held == Definition; };
		if (UCrowdyServerObject* Object = Cast<UCrowdyServerObject>(Mutable))
		{
			return Fits(Object->GetDefinition()) ? FHolder{Object, &Object->GetVariableBindings(), Object, nullptr, &Object->GetMessageBindings()} : FHolder();
		}
		if (UCrowdyServerObjectLink* Link = Cast<UCrowdyServerObjectLink>(Mutable))
		{
			return Fits(Link->GetDefinition()) ? FHolder{Link->GetServerObject(), &Link->GetVariableBindings(), Link, nullptr, &Link->GetMessageBindings()} : FHolder();
		}
		UCrowdyServerObjectComponent* Component = Cast<UCrowdyServerObjectComponent>(Mutable);
		if (AActor* Actor = Cast<AActor>(Mutable))
		{
			Component = FindOnlyComponent(Actor, Definition);
		}
		if (!Component || !Fits(Component->Definition))
		{
			return FHolder();
		}
		return FHolder{Component->GetServerObject(), &Component->GetVariableBindings(), Component, Component->ReadsRecord() ? &Component->GetRecordId() : nullptr,
			&Component->GetMessageBindings()};
	}

	UCrowdyServerObjectSubsystem* FindSubsystem(const FHolder& Holder)
	{
		if (const UCrowdyServerObjectComponent* Component = Cast<UCrowdyServerObjectComponent>(Holder.Owner))
		{
			return Component->FindServerObjects();
		}
		return Holder.Owner ? Cast<UCrowdyServerObjectSubsystem>(Holder.Owner->GetOuter()) : nullptr;
	}

	/** A non-optional value that does not fit the pin reading it, warned about once per variable. */
	void WarnUnfitValue(const FInstancedStruct& Values, FName Name, const FProperty* DestProperty)
	{
		static TSet<FName> Warned;
		const FProperty* Field = UCrowdyServerObjectLibrary::FindServerValue(Values, Name);
		if (!Field || !DestProperty || CastField<FOptionalProperty>(Field) || Warned.Contains(Name))
		{
			return;
		}
		Warned.Add(Name);
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s's value does not fit a %s pin, so it reads as the pin's default"), *Name.ToString(), *DestProperty->GetCPPType());
	}

	/** Calls Function on the library Definition names and runs OnDone exactly once: refused before sending, or with the answer. */
	template <typename TInputs>
	void CallLibrary(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, FName Function, const TInputs& Inputs,
		TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds)
	{
		if (!OnDone)
		{
			OnDone = [](const FCrowdyServerCallResult&) {};
		}
		auto Refuse = [&OnDone](ECrowdyServerCallOutcome Outcome, const FString& Reason)
		{
			FCrowdyServerCallResult Result;
			Result.Outcome = Outcome;
			Result.Reason = Reason;
			OnDone(Result);
		};
		if (!Definition || !Definition->IsFunctionLibrary())
		{
			Refuse(ECrowdyServerCallOutcome::BadRequest, FString::Printf(TEXT("%s is not a Server Function Library"), Definition ? *Definition->GetName() : TEXT("None")));
			return;
		}
		if (!Definition->FindFunction(Function))
		{
			Refuse(ECrowdyServerCallOutcome::BadRequest, FString::Printf(TEXT("%s has no function named %s"), *Definition->TypeName, *Function.ToString()));
			return;
		}
		const UGameInstance* GameInstance = FindGameInstance(WorldContextObject);
		UCrowdyServerObjectSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UCrowdyServerObjectSubsystem>() : nullptr;
		if (!Subsystem)
		{
			Refuse(ECrowdyServerCallOutcome::Unavailable, TEXT("Calling a Server Function Library needs a world with a game instance"));
			return;
		}
		Subsystem->CallLibraryFunction(Definition, Function, Inputs, MoveTemp(OnDone), TimeoutSeconds);
	}
}

UCrowdyServerObject* UCrowdyServerObjectLibrary::GetServerObject(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, const FString& InstanceId, UObject* Owner, FString& Error)
{
	const UGameInstance* GameInstance = CrowdyServerObjectLibraryDetail::FindGameInstance(WorldContextObject);
	UCrowdyServerObjectSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UCrowdyServerObjectSubsystem>() : nullptr;
	if (!Subsystem)
	{
		Error = TEXT("Get Server Object needs a world with a game instance");
		return nullptr;
	}
	return Subsystem->Acquire(Definition, InstanceId, Owner, Error);
}

void UCrowdyServerObjectLibrary::ReleaseServerObject(UCrowdyServerObject* Object, UObject* Owner)
{
	if (!Object || !Owner)
	{
		return;
	}
	UCrowdyServerObjectSubsystem* Subsystem = Cast<UCrowdyServerObjectSubsystem>(Object->GetOuter());
	if (!Subsystem)
	{
		return;
	}
	Subsystem->Release(Object, Owner);
}

void UCrowdyServerObjectLibrary::CallLibraryFunction(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, FName Function, const FInstancedStruct& Inputs,
	TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds)
{
	CrowdyServerObjectLibraryDetail::CallLibrary(WorldContextObject, Definition, Function, Inputs, MoveTemp(OnDone), TimeoutSeconds);
}

void UCrowdyServerObjectLibrary::CallLibraryFunction(UObject* WorldContextObject, UCrowdyServerObjectDefinition* Definition, FName Function, const FInstancedPropertyBag& Inputs,
	TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds)
{
	CrowdyServerObjectLibraryDetail::CallLibrary(WorldContextObject, Definition, Function, Inputs, MoveTemp(OnDone), TimeoutSeconds);
}

FString UCrowdyServerObjectLibrary::GetPlayerInstanceId(UObject* WorldContextObject)
{
	const UGameInstance* GameInstance = CrowdyServerObjectLibraryDetail::FindGameInstance(WorldContextObject);
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	const int64 UserId = Session ? Session->GetUserID() : 0;
	return UserId > 0 ? LexToString(UserId) : FString();
}

const TCHAR* const UCrowdyServerObjectLibrary::PlayersOnlineDefinitionPath = TEXT("/CrowdySDK/Data/CSO_PlayersOnline.CSO_PlayersOnline");

bool UCrowdyServerObjectLibrary::GetPlayersOnline(UObject* Owner, int32& PlayersOnline)
{
	PlayersOnline = 0;
	// A setting naming no starter's definition deploys under the SDK's root, so that is where the count is.
	UCrowdyServerObjectDefinition* KitWorld = GetGameKitWorldDefinition();
	const bool bKitWorld = KitWorld != nullptr;
	UCrowdyServerObjectDefinition* Definition = bKitWorld ? KitWorld : GetPlayersOnlineDefinition();
	UCrowdyServerObjectLink* Link = Definition ? FindServerObjectLink(Owner, Definition, ECrowdyServerObjectFind::InstanceId, FString(), 0) : nullptr;
	if (!Link)
	{
		return false;
	}
	Link->KeepWhileOwnerLives();
	static const FProperty* const Count = FCrowdyPlayersOnlineState::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FCrowdyPlayersOnlineState, PlayersOnline));
	bool bUnsent = false;
	if (bKitWorld ? ReadKitWorldOnline(Link, Definition, PlayersOnline, bUnsent) : ReadServerVariableInto(Link, Definition, Count->GetFName(), Count, &PlayersOnline))
	{
		return true;
	}
	const UCrowdyServerObject* Root = Link->GetServerObject();
	UCrowdyServerObjectSubsystem* Subsystem = Root ? Cast<UCrowdyServerObjectSubsystem>(Root->GetOuter()) : nullptr;
	if (bUnsent)
	{
		static const FString NoCountReason = TEXT("the world's snapshot carries no player count; a world from the platform's own starter pack does not send one");
		if (Subsystem && Subsystem->NotePlayersOnlineReason(NoCountReason))
		{
			UE_LOG(LogCrowdyExec, Warning, TEXT("Get Players Online has no count: %s"), *NoCountReason);
		}
		return false;
	}
	// An app with no root, or one an older SDK deployed, refuses every read, which would otherwise read as 0 with no word why.
	const FString Reason = Root ? Root->GetFailureReason() : FString();
	if (Subsystem && !Reason.IsEmpty() && Subsystem->NotePlayersOnlineReason(Reason))
	{
		const bool bAdvised = Reason.Contains(TEXT("Server Compute"), ESearchCase::CaseSensitive);
		UE_LOG(LogCrowdyExec, Warning, TEXT("Get Players Online has no count yet: %s%s"), *Reason,
			bAdvised ? TEXT("") : TEXT(". An app last deployed with an older Crowdy SDK keeps none; deploy it from Server Compute."));
	}
	return false;
}

bool UCrowdyServerObjectLibrary::ReadKitWorldOnline(const UObject* Target, const UCrowdyServerObjectDefinition* World, int32& Online, bool& bUnsent)
{
	static const FProperty* const Count = FCrowdyKitWorldConditions::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FCrowdyKitWorldConditions, Online));
	static const FProperty* const HasCount = FCrowdyKitWorldConditions::StaticStruct()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FCrowdyKitWorldConditions, bHasOnline));
	bUnsent = false;
	if (!ReadServerVariableInto(Target, World, Count->GetFName(), Count, &Online))
	{
		return false;
	}
	bool bSent = false;
	ReadServerVariableInto(Target, World, HasCount->GetFName(), HasCount, &bSent);
	if (bSent)
	{
		return true;
	}
	Online = 0;
	bUnsent = true;
	return false;
}

UCrowdyServerObjectDefinition* UCrowdyServerObjectLibrary::GetGameKitWorldDefinition()
{
	const FSoftObjectPath& Path = GetDefault<UCrowdySDKDeveloperSettings>()->GameKitWorld;
	if (Path.IsNull())
	{
		return nullptr;
	}
	// Get Players Online asks every frame, so a path that gave nothing is not loaded again until the setting changes.
	static FSoftObjectPath Tried;
	static TWeakObjectPtr<UCrowdyServerObjectDefinition> Found;
	if (Path == Tried && (Found.IsValid() || Found.IsExplicitlyNull()))
	{
		return Found.Get();
	}
	Tried = Path;
	UObject* Loaded = Path.ResolveObject();
	Loaded = Loaded ? Loaded : LoadObject<UObject>(nullptr, *Path.ToString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
	UCrowdyServerObjectDefinition* Definition = Cast<UCrowdyServerObjectDefinition>(Loaded);
	Found = Definition && Definition->IsGameKitStarter() ? Definition : nullptr;
	UE_CLOG(!Found.IsValid(), LogCrowdyExec, Warning, TEXT("The Game Kit World setting names %s, which is not a Game Kit starter's Server Object definition, so Get Players Online reads the Crowdy SDK's root; clear the setting, or add the world starter again in Server Compute."),
		*Path.ToString());
	return Found.Get();
}

UCrowdyServerObjectDefinition* UCrowdyServerObjectLibrary::GetPlayersOnlineDefinition()
{
	// Loaded once and kept, so a missing asset is reported once and a held one is never loaded again mid-game.
	static UCrowdyServerObjectDefinition* const Loaded = []()
	{
		UCrowdyServerObjectDefinition* Definition = LoadObject<UCrowdyServerObjectDefinition>(nullptr, PlayersOnlineDefinitionPath);
		UE_CLOG(!Definition, LogCrowdyExec, Error, TEXT("%s did not load, so there is no Players Online; it ships in the Crowdy SDK's content, which every cook includes."), PlayersOnlineDefinitionPath);
		if (Definition)
		{
			Definition->AddToRoot();
		}
		return Definition;
	}();
	return Loaded;
}

const FProperty* UCrowdyServerObjectLibrary::FindServerValue(const FInstancedStruct& Values, FName Name)
{
	return CrowdyExec::FindTravellingField(Values.GetScriptStruct(), Name);
}

bool UCrowdyServerObjectLibrary::SetServerValueFrom(FInstancedStruct& Values, FName Name, const FProperty* SourceProperty, const void* Source)
{
	const FProperty* Field = FindServerValue(Values, Name);
	uint8* Memory = Values.GetMutableMemory();
	if (!Field || !Memory || !SourceProperty || !Source)
	{
		return false;
	}
	return CrowdyServerObjectLibraryDetail::CopyValue(Field, Field->ContainerPtrToValuePtr<void>(Memory), SourceProperty, Source);
}

bool UCrowdyServerObjectLibrary::GetServerValueInto(const FInstancedStruct& Values, FName Name, const FProperty* DestProperty, void* Dest)
{
	const FProperty* Field = FindServerValue(Values, Name);
	const uint8* Memory = Values.GetMemory();
	if (!Field || !Memory || !DestProperty || !Dest)
	{
		return false;
	}
	return CrowdyServerObjectLibraryDetail::CopyValue(DestProperty, Dest, Field, Field->ContainerPtrToValuePtr<void>(Memory));
}

UCrowdyServerObject* UCrowdyServerObjectLibrary::ResolveServerObject(const UObject* Target, const UCrowdyServerObjectDefinition* Definition)
{
	return CrowdyServerObjectLibraryDetail::FindHolder(Target, Definition).Object;
}

UCrowdyServerObject* UCrowdyServerObjectLibrary::GetHeldServerObject(const UObject* Target, const UCrowdyServerObjectDefinition* Definition)
{
	return ResolveServerObject(Target, Definition);
}

UCrowdyServerObjectLink* UCrowdyServerObjectLibrary::FindServerObjectLink(UObject* Owner, UCrowdyServerObjectDefinition* Definition, ECrowdyServerObjectFind Instance, const FString& InstanceId, int64 TeamId)
{
	const UGameInstance* GameInstance = CrowdyServerObjectLibraryDetail::FindGameInstance(Owner);
	UCrowdyServerObjectSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UCrowdyServerObjectSubsystem>() : nullptr;
	return Subsystem ? Subsystem->FindLink(Owner, Definition, Instance, InstanceId, TeamId) : nullptr;
}

bool UCrowdyServerObjectLibrary::ReadServerVariableInto(const UObject* Target, const UCrowdyServerObjectDefinition* Definition, FName Variable, const FProperty* DestProperty, void* Dest)
{
	if (!DestProperty || !Dest)
	{
		return false;
	}
	const CrowdyServerObjectLibraryDetail::FHolder Holder = CrowdyServerObjectLibraryDetail::FindHolder(Target, Definition);
	// A component that moved to another area reads the one it left until the new one holds its record.
	const UCrowdyServerObjectComponent* Component = Cast<UCrowdyServerObjectComponent>(Holder.Owner);
	const UCrowdyServerObject* Read = Component ? Component->GetReadObject() : Holder.Object;
	const FInstancedStruct* Values = Read ? Read->FindValues(Holder.RecordId) : nullptr;
	// Before its first read the object holds the defaults, which are no answer from the server, as does an Owner Value it never reads.
	const bool bReady = Read && Read->GetStatus() == ECrowdyServerObjectStatus::Ready && (Holder.RecordId || !Read->IsUnreadOwnerValue(Variable));
	return ReadReadyValueInto(Values, bReady, Variable, DestProperty, Dest);
}

bool UCrowdyServerObjectLibrary::ReadReadyValueInto(const FInstancedStruct* Values, bool bReady, FName Name, const FProperty* DestProperty, void* Dest)
{
	if (!DestProperty || !Dest)
	{
		return false;
	}
	if (Values && GetServerValueInto(*Values, Name, DestProperty, Dest))
	{
		return bReady;
	}
	if (Values)
	{
		CrowdyServerObjectLibraryDetail::WarnUnfitValue(*Values, Name, DestProperty);
	}
	DestProperty->ClearValue(Dest);
	return false;
}

void UCrowdyServerObjectLibrary::BindServerVariableChanged(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Variable, UObject* Handler, FName HandlerFunction)
{
	const CrowdyServerObjectLibraryDetail::FHolder Holder = CrowdyServerObjectLibraryDetail::FindHolder(Target, Definition);
	if (!Holder.Bindings)
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s is not a Server Object of %s, a Crowdy Server Object component with it, an actor with one, or a Server Object Link, so %s's changes are not watched"),
			*GetNameSafe(Target), *GetNameSafe(Definition), *Variable.ToString());
		return;
	}
	UCrowdyServerObjectSubsystem* Subsystem = Handler ? CrowdyServerObjectLibraryDetail::FindSubsystem(Holder) : nullptr;
	UObject* Previous = Subsystem ? Subsystem->MoveBinding(Handler, HandlerFunction, Holder.Owner) : nullptr;
	const CrowdyServerObjectLibraryDetail::FHolder Old = CrowdyServerObjectLibraryDetail::FindHolder(Previous, nullptr);
	if (Old.Bindings && Old.Owner != Holder.Owner)
	{
		Old.Bindings->UnbindHandler(Handler, HandlerFunction);
	}
	Holder.Bindings->Bind(Holder.Object, Variable, Handler, HandlerFunction);
}

void UCrowdyServerObjectLibrary::UnbindServerVariableChanged(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Variable, UObject* Handler, FName HandlerFunction)
{
	const CrowdyServerObjectLibraryDetail::FHolder Holder = CrowdyServerObjectLibraryDetail::FindHolder(Target, Definition);
	if (Holder.Bindings)
	{
		Holder.Bindings->Unbind(Variable, Handler, HandlerFunction);
	}
}

void UCrowdyServerObjectLibrary::BindServerMessage(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Message, UObject* Handler, FName HandlerFunction)
{
	const CrowdyServerObjectLibraryDetail::FHolder Holder = CrowdyServerObjectLibraryDetail::FindHolder(Target, Definition);
	if (!Holder.Messages)
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s is not a Server Object of %s, a Crowdy Server Object component with it, an actor with one, or a Server Object Link, so its %s messages are not heard"),
			*GetNameSafe(Target), *GetNameSafe(Definition), *Message.ToString());
		return;
	}
	UCrowdyServerObjectSubsystem* Subsystem = Handler ? CrowdyServerObjectLibraryDetail::FindSubsystem(Holder) : nullptr;
	UObject* Previous = Subsystem ? Subsystem->MoveBinding(Handler, HandlerFunction, Holder.Owner) : nullptr;
	const CrowdyServerObjectLibraryDetail::FHolder Old = CrowdyServerObjectLibraryDetail::FindHolder(Previous, nullptr);
	if (Old.Messages && Old.Owner != Holder.Owner)
	{
		Old.Messages->UnbindHandler(Handler, HandlerFunction);
	}
	Holder.Messages->Bind(Message, Handler, HandlerFunction);
}

void UCrowdyServerObjectLibrary::UnbindServerMessage(UObject* Target, UCrowdyServerObjectDefinition* Definition, FName Message, UObject* Handler, FName HandlerFunction)
{
	const CrowdyServerObjectLibraryDetail::FHolder Holder = CrowdyServerObjectLibraryDetail::FindHolder(Target, Definition);
	if (Holder.Messages)
	{
		Holder.Messages->Unbind(Message, Handler, HandlerFunction);
	}
}

FInstancedStruct UCrowdyServerObjectLibrary::MakeFunctionInputs(const UCrowdyServerObjectDefinition* Definition, FName Function)
{
	FInstancedStruct Out;
	const FCrowdyServerFunction* Found = Definition ? Definition->FindFunction(Function) : nullptr;
	if (Found)
	{
		Found->InitializeParams(Out);
	}
	return Out;
}

bool UCrowdyServerObjectLibrary::SetServerValueOrUnsetFrom(FInstancedStruct& Values, FName Name, bool bHasValue, const FProperty* SourceProperty, const void* Source)
{
	if (bHasValue)
	{
		return SetServerValueFrom(Values, Name, SourceProperty, Source);
	}
	const FOptionalProperty* Optional = CastField<FOptionalProperty>(FindServerValue(Values, Name));
	uint8* Memory = Values.GetMutableMemory();
	if (!Optional || !Memory)
	{
		return false;
	}
	Optional->MarkUnset(Optional->ContainerPtrToValuePtr<void>(Memory));
	return true;
}

DEFINE_FUNCTION(UCrowdyServerObjectLibrary::execSetServerValue)
{
	P_GET_STRUCT_REF(FInstancedStruct, Values);
	P_GET_PROPERTY(FNameProperty, Name);

	// Value is a wildcard: step over it by hand to learn its type and address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	const void* Value = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bSet = false;
	P_NATIVE_BEGIN;
	bSet = SetServerValueFrom(Values, Name, ValueProperty, Value);
	P_NATIVE_END;
	*(bool*)RESULT_PARAM = bSet;
}

DEFINE_FUNCTION(UCrowdyServerObjectLibrary::execGetServerValue)
{
	P_GET_STRUCT_REF(FInstancedStruct, Values);
	P_GET_PROPERTY(FNameProperty, Name);

	// Value is a wildcard: step over it by hand to learn its type and address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	void* Value = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bGot = false;
	P_NATIVE_BEGIN;
	bGot = GetServerValueInto(Values, Name, ValueProperty, Value);
	if (!bGot)
	{
		CrowdyServerObjectLibraryDetail::WarnUnfitValue(Values, Name, ValueProperty);
	}
	P_NATIVE_END;
	*(bool*)RESULT_PARAM = bGot;
}

DEFINE_FUNCTION(UCrowdyServerObjectLibrary::execReadServerVariable)
{
	P_GET_OBJECT(UObject, Target);
	P_GET_OBJECT(UCrowdyServerObjectDefinition, Definition);
	P_GET_PROPERTY(FNameProperty, Variable);

	// Value is a wildcard: step over it by hand to learn its type and address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	void* Value = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bRead = false;
	P_NATIVE_BEGIN;
	bRead = ReadServerVariableInto(Target, Definition, Variable, ValueProperty, Value);
	P_NATIVE_END;
	*(bool*)RESULT_PARAM = bRead;
}

DEFINE_FUNCTION(UCrowdyServerObjectLibrary::execSetServerValueOrUnset)
{
	P_GET_STRUCT_REF(FInstancedStruct, Values);
	P_GET_PROPERTY(FNameProperty, Name);
	P_GET_UBOOL(bHasValue);

	// Value is a wildcard: step over it by hand to learn its type and address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentPropertyContainer = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	const void* Value = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bSet = false;
	P_NATIVE_BEGIN;
	bSet = SetServerValueOrUnsetFrom(Values, Name, bHasValue, ValueProperty, Value);
	P_NATIVE_END;
	*(bool*)RESULT_PARAM = bSet;
}

FString UCrowdyServerObjectLibrary::NoteInputError(const FString& Errors, bool bSet, FName Input)
{
	if (bSet)
	{
		return Errors;
	}
	const FString Sentence = FString::Printf(TEXT("%s does not fit its type."), *Input.ToString());
	return Errors.IsEmpty() ? Sentence : Errors + TEXT(" ") + Sentence;
}

UCrowdyServerCallAction* UCrowdyServerCallAction::CallServerFunction(UObject* WorldContextObject, UCrowdyServerObject* Object, FName Function, const FInstancedStruct& Inputs, float Timeout)
{
	UCrowdyServerCallAction* Action = NewObject<UCrowdyServerCallAction>();
	Action->Setup(WorldContextObject, Object, Function, Inputs, FString(), Timeout);
	return Action;
}

UCrowdyServerCallAction* UCrowdyServerTypedCallAction::CallServerFunctionWithInputs(UObject* WorldContextObject, UCrowdyServerObject* Object, FName Function, const FInstancedStruct& Inputs, const FString& InputError, float Timeout)
{
	UCrowdyServerTypedCallAction* Action = NewObject<UCrowdyServerTypedCallAction>();
	Action->Setup(WorldContextObject, Object, Function, Inputs, InputError, Timeout);
	return Action;
}

void UCrowdyServerCallAction::Setup(UObject* WorldContextObject, UCrowdyServerObject* InObject, FName InFunction, const FInstancedStruct& InInputs, const FString& InInputError, float InTimeout)
{
	CalledObject = InObject;
	CalledFunction = InFunction;
	CalledInputs = InInputs;
	RefusedInputs = InInputError;
	CalledTimeout = InTimeout;
	RegisterWithGameInstance(WorldContextObject);
}

void UCrowdyServerCallAction::Activate()
{
	FCrowdyServerCallResult Refused;
	Refused.Outcome = ECrowdyServerCallOutcome::BadRequest;
	Refused.Reason = RefusedInputs.IsEmpty() ? FString(TEXT("Call Server Function needs a Server Object")) : RefusedInputs;
	if (!RefusedInputs.IsEmpty() || !CalledObject)
	{
		Finish(Refused);
		return;
	}
	CalledObject->Call(CalledFunction, CalledInputs, [WeakThis = TWeakObjectPtr<UCrowdyServerCallAction>(this)](const FCrowdyServerCallResult& Result)
	{
		if (UCrowdyServerCallAction* This = WeakThis.Get())
		{
			This->Finish(Result);
		}
	}, CalledTimeout);
}

void UCrowdyServerCallAction::Finish(const FCrowdyServerCallResult& Result)
{
	if (Result.IsSuccess())
	{
		OnSuccess.Broadcast(Result.Reply, Result.Outcome, Result.Reason, Result.bRetryable, Result.Code);
	}
	else
	{
		OnFailed.Broadcast(Result.Reply, Result.Outcome, Result.Reason, Result.bRetryable, Result.Code);
	}
	SetReadyToDestroy();
}

UCrowdyServerPingAction* UCrowdyServerPingAction::PingServerObjects(UObject* WorldContextObject)
{
	UCrowdyServerPingAction* Action = NewObject<UCrowdyServerPingAction>();
	const UGameInstance* GameInstance = CrowdyServerObjectLibraryDetail::FindGameInstance(WorldContextObject);
	Action->PingSubsystem = GameInstance ? GameInstance->GetSubsystem<UCrowdyServerObjectSubsystem>() : nullptr;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyServerPingAction::SetSubsystemForTest(UCrowdyServerObjectSubsystem* Subsystem)
{
	PingSubsystem = Subsystem;
}
#endif

void UCrowdyServerPingAction::Activate()
{
	UCrowdyServerObjectSubsystem* Subsystem = PingSubsystem.Get();
	if (!Subsystem)
	{
		Finish(false, 0.f, TEXT("Ping Server Objects needs a world with a game instance"));
		return;
	}
	Subsystem->Ping([WeakThis = TWeakObjectPtr<UCrowdyServerPingAction>(this)](bool bOk, float RoundTripMs, const FString& Reason)
	{
		if (UCrowdyServerPingAction* This = WeakThis.Get())
		{
			This->Finish(bOk, RoundTripMs, Reason);
		}
	});
}

void UCrowdyServerPingAction::Finish(bool bOk, float RoundTripMs, const FString& Reason)
{
	if (bOk)
	{
		OnSuccess.Broadcast(RoundTripMs, FString());
	}
	else
	{
		OnFailed.Broadcast(0.f, Reason);
	}
	SetReadyToDestroy();
}
