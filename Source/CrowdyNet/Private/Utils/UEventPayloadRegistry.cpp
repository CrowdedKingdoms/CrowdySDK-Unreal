#include "Utils/UEventPayloadRegistry.h"
#include "CrowdyNetLog.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"

std::atomic<UEventPayloadRegistry*> UEventPayloadRegistry::Instance(nullptr);

void UEventPayloadRegistry::LoadFromDataAsset(const UEventPayloadType* DataAsset)
{
	if (!DataAsset) return;

	for (const FEventPayloadTypeEntry& Entry : DataAsset->GetAllEntries())
	{
		if (!Entry.EventType)
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("[EventPayloadRegistry] Entry with ID %d has null struct — skipped."), Entry.TypeID);
			continue;
		}

		RegisterStruct(Entry.EventType, static_cast<FCrowdyTypeID>(Entry.TypeID));
	}
}

bool UEventPayloadRegistry::GetID(const UScriptStruct* Struct, FCrowdyTypeID& OutID) const
{
	if (!Struct) return false;

	const FName PathKey = FName(*Struct->GetPathName());

	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	const FCrowdyTypeID* Found = StructPathToID.Find(PathKey);
	if (Found)
	{
		OutID = *Found;
		return true;
	}

	return false;
}

bool UEventPayloadRegistry::GetName(const FCrowdyTypeID ID, FName& OutName) const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	const FName* Found = IDToName.Find(ID);
	if (!Found) return false;

	OutName = *Found;
	return true;
}

UScriptStruct* UEventPayloadRegistry::Resolve(const FCrowdyTypeID ID) const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	const TObjectPtr<UScriptStruct>* Found = IDToStruct.Find(ID);
	return Found ? Found->Get() : nullptr;
}

void UEventPayloadRegistry::RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID)
{
	if (!Struct) return;

	FRWScopeLock WriteLock(RegistryLock, SLT_Write);

	if (IDToStruct.Contains(TypeID))
	{
		const UScriptStruct* Existing = IDToStruct[TypeID].Get();
		if (Existing == Struct) return; // benign duplicate

		UE_LOG(LogCrowdyNet, Fatal,
			TEXT("[EventPayloadRegistry] Hash collision: TypeID=%d is claimed by")
			TEXT(" both '%s' and '%s'. Add an entry to IDOverrides in")
			TEXT(" CrowdyDeveloperSettings to resolve."),
			TypeID,
			*Existing->GetPathName(),
			*Struct->GetPathName());
		return;
	}

	const FName PathKey = FName(*Struct->GetPathName());

	IDToStruct.Add(TypeID, Struct);
	StructPathToID.Add(PathKey, TypeID);
	IDToName.Add(TypeID, FName(*Struct->GetName()));

	UE_LOG(LogCrowdyNet, Warning,
		TEXT("[EventPayloadRegistry] Registered '%s' -> TypeID=%d"),
		*Struct->GetName(), TypeID);
}

void UEventPayloadRegistry::RegisterStructAuto(UScriptStruct* Struct)
{
	if (!Struct)
		return;

	FCrowdyTypeID ExistingID;
	if (GetID(Struct, ExistingID))
		return;

	const FCrowdyTypeID TypeID = IDResolver
		? IDResolver(Struct)
		: FCrowdyTypeIDGenerator::GenerateFromStruct(Struct);

	RegisterStruct(Struct, TypeID);
}
