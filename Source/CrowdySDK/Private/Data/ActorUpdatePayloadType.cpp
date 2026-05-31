// Fill out your copyright notice in the Description page of Project Settings.


#include "Data/ActorUpdatePayloadType.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"

void UActorUpdatePayloadType::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	
	TSet<int32> SeenIDs;
	TSet<FName> SeenNames;
	
	for (int32 i = 0; i < Entries.Num(); i++)
	{
		uint8 ID = Entries[i].TypeID;
		
		if (SeenIDs.Contains(ID))
		{
			uint8 Next = ID + 1;
			while (SeenIDs.Contains(Next))
				Next++;
			
			UE_LOG(LogTemp, Warning, TEXT("[UActorUpdatePayloadType]: Duplicate TypeID %d at index %d - reassigned to %d"),
				ID, i, Next);
			
			Entries[i].TypeID = Next;
			ID = Next;
		}
		SeenIDs.Add(ID);
		
		if (Entries[i].ActorUpdateName.IsNone())
			Entries[i].ActorUpdateName = FName(*FString::Printf(TEXT("ActorUpdate_%d"), Entries[i].TypeID));
		
		if (SeenNames.Contains(Entries[i].ActorUpdateName))
		{
			FName Unique = FName(*FString::Printf(TEXT("%s_%d"), *Entries[i].ActorUpdateName.ToString(), i));
			
			UE_LOG(LogTemp, Warning, TEXT("[UActorUpdatePayloadType]: Duplicate ActorUpdateName '%s' at index %d - reassigned to '%s'"),
				*Entries[i].ActorUpdateName.ToString(), i, *Unique.ToString());
			
			Entries[i].ActorUpdateName = Unique;
		}
		
		SeenNames.Add(Entries[i].ActorUpdateName);
	}
}

EDataValidationResult UActorUpdatePayloadType::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result =  Super::IsDataValid(Context);
	
	TMap<int32, uint8> SeenIDs;
	TSet<const UScriptStruct*> SeenStructs;
	TMap<FName, int32> SeenNames;
	
	for (int32 i = 0; i < Entries.Num(); i++)
	{
		const FActorUpdatePayloadTypeEntry& Entry = Entries[i];
		
		if (!Entry.ActorUpdateType)
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("Entry[%d] has not StructType Assigned."), i)));
			Result = EDataValidationResult::Invalid;
			continue;
		}
		
		if (SeenIDs.Contains(Entry.TypeID))
		{
			Context.AddError(FText::FromString(
				FString::Printf(TEXT("Entry[%d] TypeID %d is already used by Entry[%d]."),
					i, Entry.TypeID, SeenIDs[Entry.TypeID])));
			Result = EDataValidationResult::Invalid;
		}
		else
		{
			SeenIDs.Add(Entry.TypeID, i);
		}
		
		// --- Duplicate EventType struct ---
		if (SeenStructs.Contains(Entry.ActorUpdateType))
		{
			Context.AddError(FText::FromString(
				FString::Printf(TEXT("Entry[%d] ActorUpdateType '%s' is already registered under a different ID."),
					i, *Entry.ActorUpdateType->GetName())));
			Result = EDataValidationResult::Invalid;
		}
		else
		{
			SeenStructs.Add(Entry.ActorUpdateType);
		}
		
		// --- EventName ---
		if (Entry.ActorUpdateName.IsNone())
		{
			Context.AddError(FText::FromString(
				FString::Printf(TEXT("Entry[%d] has no ActorUpdateName assigned."), i)));
			Result = EDataValidationResult::Invalid;
		}
		else if (SeenNames.Contains(Entry.ActorUpdateName))
		{
			Context.AddError(FText::FromString(
				FString::Printf(TEXT("Entry[%d] EventName '%s' is already used by Entry[%d]."),
					i, *Entry.ActorUpdateName.ToString(), SeenNames[Entry.ActorUpdateName])));
			Result = EDataValidationResult::Invalid;
		}
		else
		{
			SeenNames.Add(Entry.ActorUpdateName, i);
		}
	}

	return Result;
}

#endif
