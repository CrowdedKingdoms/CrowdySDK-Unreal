// EventPayloadType.cpp

#include "Data/EventPayloadType.h"

#include "Messages/Events/FCrowdySelectIDAsHost.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	static FEventPayloadTypeEntry MakeEntry(
		const int32 TypeID,
		const FName EventName,
		UScriptStruct* Struct)
	{
		FEventPayloadTypeEntry Entry;
		Entry.TypeID = TypeID;
		Entry.EventName = EventName;
		Entry.EventType = Struct;
		return Entry;
	}
}

UEventPayloadType::UEventPayloadType()
{
	BuildInternalEntries();
}

void UEventPayloadType::BuildInternalEntries()
{
	InternalEntries.Reset();
	
	InternalEntries.Add(
		MakeEntry(46, 
			"SelectHost", 
			FCrowdySelectIDAsHost::StaticStruct()));
	
	InternalEntries.Add(
		MakeEntry(
			47,
			"CreateCrowdyGameObject",
			FCrowdyEntitySpawnEvent::StaticStruct()));

	InternalEntries.Add(
		MakeEntry(
			49,
			"DestroyCrowdyGameObject",
			FCrowdyEntityDestroyEvent::StaticStruct()));

	InternalEntries.Add(
		MakeEntry(
			48,
			"ChangeCrowdyGameObjectState",
			FCrowdyEntityStateEvent::StaticStruct()));
}

TArray<FEventPayloadTypeEntry> UEventPayloadType::GetAllEntries() const
{
	TArray<FEventPayloadTypeEntry> Result = InternalEntries;
	Result.Append(Entries);
	return Result;
}

void UEventPayloadType::SanitizeCustomEntries()
{
	constexpr int32 MinCustomID = 50;

	TSet<int32> UsedIDs;
	TSet<FName> UsedNames;

	// Reserve internal entries
	for (const FEventPayloadTypeEntry& Entry : InternalEntries)
	{
		UsedIDs.Add(Entry.TypeID);
		UsedNames.Add(Entry.EventName);
	}

	for (int32 i = 0; i < Entries.Num(); i++)
	{
		FEventPayloadTypeEntry& Entry = Entries[i];

		// Enforce custom range
		if (Entry.TypeID < MinCustomID)
		{
			int32 Next = MinCustomID;

			while (UsedIDs.Contains(Next))
			{
				Next++;
			}

			UE_LOG(LogTemp, Warning,
				TEXT("Custom Event[%d] TypeID %d is reserved. Reassigned to %d."),
				i,
				Entry.TypeID,
				Next);

			Entry.TypeID = Next;
		}

		// Resolve duplicate IDs
		if (UsedIDs.Contains(Entry.TypeID))
		{
			int32 Next = Entry.TypeID + 1;

			while (UsedIDs.Contains(Next))
			{
				Next++;
			}

			UE_LOG(LogTemp, Warning,
				TEXT("Duplicate TypeID %d detected. Reassigned to %d."),
				Entry.TypeID,
				Next);

			Entry.TypeID = Next;
		}

		UsedIDs.Add(Entry.TypeID);

		// Auto-generate names
		if (Entry.EventName.IsNone())
		{
			Entry.EventName = FName(
				*FString::Printf(TEXT("Event_%d"), Entry.TypeID));
		}

		// Resolve duplicate names
		if (UsedNames.Contains(Entry.EventName))
		{
			FName UniqueName = FName(
				*FString::Printf(TEXT("%s_%d"),
					*Entry.EventName.ToString(),
					i));

			UE_LOG(LogTemp, Warning,
				TEXT("Duplicate EventName '%s'. Reassigned to '%s'."),
				*Entry.EventName.ToString(),
				*UniqueName.ToString());

			Entry.EventName = UniqueName;
		}

		UsedNames.Add(Entry.EventName);
	}
}

#if WITH_EDITOR

void UEventPayloadType::PostLoad()
{
	Super::PostLoad();

	BuildInternalEntries();
}

void UEventPayloadType::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	BuildInternalEntries();
	SanitizeCustomEntries();
}

EDataValidationResult UEventPayloadType::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = EDataValidationResult::Valid;

	TSet<int32> SeenIDs;
	TSet<FName> SeenNames;

	auto ValidateArray =
		[&](const TArray<FEventPayloadTypeEntry>& Array, const FString& Prefix)
	{
		for (int32 i = 0; i < Array.Num(); i++)
		{
			const FEventPayloadTypeEntry& Entry = Array[i];

			if (!Entry.EventType)
			{
				Context.AddError(FText::FromString(
					FString::Printf(
						TEXT("%s[%d] has no EventType."),
						*Prefix,
						i)));

				Result = EDataValidationResult::Invalid;
			}

			if (SeenIDs.Contains(Entry.TypeID))
			{
				Context.AddError(FText::FromString(
					FString::Printf(
						TEXT("%s[%d] duplicate TypeID %d."),
						*Prefix,
						i,
						Entry.TypeID)));

				Result = EDataValidationResult::Invalid;
			}

			SeenIDs.Add(Entry.TypeID);

			if (SeenNames.Contains(Entry.EventName))
			{
				Context.AddError(FText::FromString(
					FString::Printf(
						TEXT("%s[%d] duplicate EventName '%s'."),
						*Prefix,
						i,
						*Entry.EventName.ToString())));

				Result = EDataValidationResult::Invalid;
			}

			SeenNames.Add(Entry.EventName);
		}
	};

	ValidateArray(InternalEntries, TEXT("InternalEntries"));
	ValidateArray(Entries, TEXT("Entries"));

	return Result;
}

#endif