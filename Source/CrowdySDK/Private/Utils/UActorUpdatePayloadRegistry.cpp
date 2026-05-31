// Fill out your copyright notice in the Description page of Project Settings.
#include "Utils/UActorUpdatePayloadRegistry.h"

std::atomic<UActorUpdatePayloadRegistry*> UActorUpdatePayloadRegistry::Instance(nullptr);

void UActorUpdatePayloadRegistry::LoadFromDataAsset(const UActorUpdatePayloadType* DataAsset)
{
	ensure(!bLoaded.load());
	ensure(IsInGameThread());
	
	if (!DataAsset) return;
	
	for (const FActorUpdatePayloadTypeEntry& Entry : DataAsset->Entries)
	{
		if (!Entry.ActorUpdateType)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ActorUpdateRegistry] Entry with ID %d has null struct - skipped."), Entry.TypeID)
			continue;
		}
		
		if (IDToStruct.Contains(Entry.TypeID))
		{
			UE_LOG(LogTemp, Error, TEXT("[ActorUpdateRegistry] Duplicate TypeID %d for struct %s - skipped."),
				Entry.TypeID, *Entry.ActorUpdateType->GetName());
			continue;
		}
		
		StructPathToID.Add(FName(Entry.ActorUpdateType.GetPathName()), Entry.TypeID);
		IDToStruct.Add(Entry.TypeID, Entry.ActorUpdateType);
		IDToName.Add(Entry.TypeID, Entry.ActorUpdateName);
		
		UE_LOG(LogTemp, Log, TEXT("[ActorUpdateRegistry] Registered: ID=%d -> %s"), Entry.TypeID, *Entry.ActorUpdateType->GetName())
	}
	
	bLoaded.store(true, std::memory_order_release);
}

bool UActorUpdatePayloadRegistry::GetID(const UScriptStruct* Struct, FCrowdyTypeID& OutID) const
{
	if (!Struct) return false;

	const FName PathKey = FName(*Struct->GetPathName());
	
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

	const FName* Found = IDToName.Find(TypeID);
	if (!Found) return false;

	OutName = *Found;
	return true;
}

UScriptStruct* UActorUpdatePayloadRegistry::Resolve(const FCrowdyTypeID ID) const
{
	const TObjectPtr<UScriptStruct>* Found = IDToStruct.Find(ID);
	return Found ? Found->Get() : nullptr;
}

void UActorUpdatePayloadRegistry::RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID)
{
	ensure(IsInGameThread());
	ensureMsgf(!bSealed.load(std::memory_order_relaxed),
		TEXT("[ActorUpdatePayloadRegistry] RegisterStruct called after Seal()"));

	if (IDToStruct.Contains(TypeID))
	{
		const UScriptStruct* Existing = IDToStruct[TypeID].Get();
		if (Existing == Struct) return;

		UE_LOG(LogTemp, Fatal,
			TEXT("[ActorUpdatePayloadRegistry] Hash collision: TypeID=%d claimed by '%s' and '%s'."),
			TypeID, *Existing->GetPathName(), *Struct->GetPathName());
		return;
	}

	const FName PathKey = FName(*Struct->GetPathName());

	IDToStruct.Add(TypeID, Struct);
	StructPathToID.Add(PathKey, TypeID);
	IDToName.Add(TypeID, FName(*Struct->GetName()));

	UE_LOG(LogTemp, Log,
		TEXT("[ActorUpdatePayloadRegistry] Registered '%s' -> TypeID=%d | Path=%s"),
		*Struct->GetName(), TypeID, *Struct->GetPathName());
}

void UActorUpdatePayloadRegistry::Seal()
{
	ensure(IsInGameThread());
	bSealed.store(true, std::memory_order_seq_cst);

	UE_LOG(LogTemp, Log,
		TEXT("[ActorUpdatePayloadRegistry] Sealed. %d actor update types registered."),
		IDToStruct.Num());
}

void UActorUpdatePayloadRegistry::GetAllRegisteredNames(TArray<FName>& OutNames) const
{
	ensureMsgf(bSealed.load(std::memory_order_acquire),
		TEXT("[ActorUpdatePayloadRegistry] GetAllRegisteredNames called before Seal()"));

	IDToName.GenerateValueArray(OutNames);
}
