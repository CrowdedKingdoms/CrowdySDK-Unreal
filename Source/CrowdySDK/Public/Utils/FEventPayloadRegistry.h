#pragma once
#include "Data/EventPayloadType.h"

struct FEventPayloadRegistry
{
	static FEventPayloadRegistry& Get()
	{
		static FEventPayloadRegistry Instance;
		return Instance;
	}
	
	void LoadFromDataAsset(const UEventPayloadType* DataAsset)
	{
		if (!DataAsset) return;
		
		IDToStruct.Reset();
		StructToID.Reset();
		
		for (const FEventPayloadTypeEntry& Entry: DataAsset->Entries)
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
			
			StructToID.Add(Entry.EventType, Entry.TypeID);
			IDToStruct.Add(Entry.TypeID, Entry.EventType);
			
			UE_LOG(LogTemp, Log, TEXT("[PayloadTypeRegistry] Registered: ID=%d -> %s"),
				Entry.TypeID, *Entry.EventType->GetName());
		}
	}
	
	UScriptStruct* Resolve(const int32 ID) const
	{
		const TObjectPtr<UScriptStruct>* Found = IDToStruct.Find(ID);
		return Found ? Found->Get() : nullptr;
	}
	
	bool GetID(const UScriptStruct* Struct, int32& OutID) const
	{
		const int32* Found = StructToID.Find(Struct);
		
		if (Found)
		{
			OutID = *Found;
			return true;
		}
		return false;
	}
	
	void Reset()
	{
		IDToStruct.Reset();
		StructToID.Reset();
	}
	
	bool IsLoaded() const { return IDToStruct.Num() > 0; }
	
	

private:
	
	TMap<int32, TObjectPtr<UScriptStruct>> IDToStruct;
	TMap<const UScriptStruct*, int32> StructToID;

};
