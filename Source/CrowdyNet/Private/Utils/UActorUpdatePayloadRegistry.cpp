// Fill out your copyright notice in the Description page of Project Settings.
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "CrowdyNetLog.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"

std::atomic<UActorUpdatePayloadRegistry*> UActorUpdatePayloadRegistry::Instance(nullptr);

void UActorUpdatePayloadRegistry::LoadFromDataAsset(const UActorUpdatePayloadType* DataAsset)
{
	if (!DataAsset) return;

	for (const FActorUpdatePayloadTypeEntry& Entry : DataAsset->Entries)
	{
		if (!Entry.ActorUpdateType)
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("[ActorUpdateRegistry] Entry with ID %d has null struct - skipped."), Entry.TypeID)
			continue;
		}

		RegisterStruct(Entry.ActorUpdateType, static_cast<FCrowdyTypeID>(Entry.TypeID));
	}
}

bool UActorUpdatePayloadRegistry::GetID(const UScriptStruct* Struct, FCrowdyTypeID& OutID) const
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

bool UActorUpdatePayloadRegistry::GetName(const UScriptStruct* Struct, FName& OutName) const
{
	if (!Struct) return false;

	FCrowdyTypeID TypeID;
	if (!GetID(Struct, TypeID)) return false;

	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	const FName* Found = IDToName.Find(TypeID);
	if (!Found) return false;

	OutName = *Found;
	return true;
}

UScriptStruct* UActorUpdatePayloadRegistry::Resolve(const FCrowdyTypeID ID) const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	const TObjectPtr<UScriptStruct>* Found = IDToStruct.Find(ID);
	return Found ? Found->Get() : nullptr;
}

void UActorUpdatePayloadRegistry::RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID)
{
	if (!Struct) return;

	FRWScopeLock WriteLock(RegistryLock, SLT_Write);

	if (IDToStruct.Contains(TypeID))
	{
		const UScriptStruct* Existing = IDToStruct[TypeID].Get();
		if (Existing == Struct) return;

		UE_LOG(LogCrowdyNet, Fatal,
			TEXT("[ActorUpdatePayloadRegistry] Hash collision: TypeID=%d claimed by '%s' and '%s'."),
			TypeID, *Existing->GetPathName(), *Struct->GetPathName());
		return;
	}

	const FName PathKey = FName(*Struct->GetPathName());

	IDToStruct.Add(TypeID, Struct);
	StructPathToID.Add(PathKey, TypeID);
	IDToName.Add(TypeID, FName(*Struct->GetName()));

	UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log,
		TEXT("[ActorUpdatePayloadRegistry] Registered '%s' -> TypeID=%d | Path=%s"),
		*Struct->GetName(), TypeID, *Struct->GetPathName());
}

void UActorUpdatePayloadRegistry::RegisterStructAuto(UScriptStruct* Struct)
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
