#include "Utils/UEventPayloadRegistry.h"

std::atomic<UEventPayloadRegistry*> UEventPayloadRegistry::Instance(nullptr);

void UEventPayloadRegistry::LoadFromDataAsset(const UEventPayloadType* DataAsset)
{
	ensure(!bLoaded.load());
	ensure(IsInGameThread());

	if (!DataAsset) return;

	for (const FEventPayloadTypeEntry& Entry : DataAsset->GetAllEntries())
	{
		if (!Entry.EventType)
		{
			UE_LOG(LogTemp, Warning, TEXT("[PayloadTypeRegistry] Entry with ID %d has null struct — skipped."), Entry.TypeID);
			continue;
		}

		if (IDToStruct.Contains(Entry.TypeID))
		{
			UE_LOG(LogTemp, Error, TEXT("[PayloadTypeRegistry] Duplicate TypeID %d for struct %s — skipped."),
				Entry.TypeID, *Entry.EventType->GetName());
			continue;
		}

		StructPathToID.Add(FName(Entry.EventType.GetPathName()), Entry.TypeID);
		IDToStruct.Add(Entry.TypeID, Entry.EventType);
		IDToName.Add(Entry.TypeID, Entry.EventName);

		UE_LOG(LogTemp, Log, TEXT("[PayloadTypeRegistry] Registered: ID=%d -> %s"),
			Entry.TypeID, *Entry.EventType->GetName());
	}

	bLoaded.store(true, std::memory_order_release);
}

bool UEventPayloadRegistry::GetID(const UScriptStruct* Struct, FCrowdyTypeID& OutID) const
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

bool UEventPayloadRegistry::GetName(const FCrowdyTypeID ID, FName& OutName) const
{
	const FName* Found = IDToName.Find(ID);
	if (!Found) return false;

	OutName = *Found;
	return true;
}

UScriptStruct* UEventPayloadRegistry::Resolve(const FCrowdyTypeID ID) const
{

	const TObjectPtr<UScriptStruct>* Found = IDToStruct.Find(ID);
	return Found ? Found->Get() : nullptr;
}

void UEventPayloadRegistry::RegisterStruct(UScriptStruct* Struct, FCrowdyTypeID TypeID)
{
	ensure(IsInGameThread());
	ensureMsgf(!bSealed.load(std::memory_order_relaxed),
		TEXT("[EventPayloadRegistry] RegisterStruct called after Seal()"));

	if (IDToStruct.Contains(TypeID))
	{
		const UScriptStruct* Existing = IDToStruct[TypeID].Get();
		if (Existing == Struct) return; // benign duplicate

		UE_LOG(LogTemp, Fatal,
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

	UE_LOG(LogTemp, Verbose,
		TEXT("[EventPayloadRegistry] Registered '%s' -> TypeID=%d"),
		*Struct->GetName(), TypeID);
}

void UEventPayloadRegistry::Seal()
{
	ensure(IsInGameThread());
	// seq_cst fence: guarantees every worker thread started after
	// this call sees the complete, populated maps.
	bSealed.store(true, std::memory_order_seq_cst);

	UE_LOG(LogTemp, Log,
		TEXT("[EventPayloadRegistry] Sealed. %d event types registered."),
		IDToStruct.Num());
}

void UEventPayloadRegistry::GetAllRegisteredNames(TArray<const UScriptStruct*> RegisteredEvents,
	TArray<FName>& OutNames) const
{
	ensureMsgf(bSealed.load(std::memory_order_acquire),
		TEXT("[EventPayloadRegistry] GetAllRegisteredNames called before Seal()"));
	
	for (const UScriptStruct* Event : RegisteredEvents)
	{
		FCrowdyTypeID ID;
		
		if (!GetID(Event, ID))
			continue;
		
		FName Name;
		
		if (!GetName(ID, Name))
			continue;
			
		OutNames.Add(Name);
	}
}
