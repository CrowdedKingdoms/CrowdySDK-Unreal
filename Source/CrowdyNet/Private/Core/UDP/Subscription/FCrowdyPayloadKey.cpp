#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"

#include "UObject/Class.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

bool FCrowdyPayloadKey::operator==(const FCrowdyPayloadKey& Other) const
{
	return Category == Other.Category && TypeID == Other.TypeID;
}

FCrowdyPayloadKey FCrowdyPayloadKey::Event(const FCrowdyTypeID InTypeID)
{
	FCrowdyPayloadKey Key;
	Key.Category = ECrowdyPayloadCategory::Event;
	Key.TypeID = InTypeID;
	return Key;
}

FCrowdyPayloadKey FCrowdyPayloadKey::ActorUpdate(const FCrowdyTypeID InTypeID)
{
	FCrowdyPayloadKey Key;
	Key.Category = ECrowdyPayloadCategory::ActorUpdate;
	Key.TypeID = InTypeID;
	return Key;
}

const TCHAR* FCrowdyPayloadKey::CategoryName(const ECrowdyPayloadCategory InCategory)
{
	switch (InCategory)
	{
	case ECrowdyPayloadCategory::Event:       return TEXT("Event");
	case ECrowdyPayloadCategory::ActorUpdate: return TEXT("ActorUpdate");
	default:                                  return TEXT("None");
	}
}

FString FCrowdyPayloadKey::Describe() const
{
	if (!IsSet())
	{
		return TEXT("None");
	}

	const UScriptStruct* Struct = Category == ECrowdyPayloadCategory::Event
		? UEventPayloadRegistry::Get()->Resolve(TypeID)
		: UActorUpdatePayloadRegistry::Get()->Resolve(TypeID);

	const FString StructName = Struct ? Struct->GetName() : FString(TEXT("unregistered"));

	return FString::Printf(TEXT("%s/%u (%s)"),
		CategoryName(Category), static_cast<uint32>(TypeID), *StructName);
}

uint32 GetTypeHash(const FCrowdyPayloadKey& Key)
{
	return HashCombine(::GetTypeHash(static_cast<uint8>(Key.Category)), ::GetTypeHash(Key.TypeID));
}
