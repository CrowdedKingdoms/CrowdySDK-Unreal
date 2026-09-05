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

	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);

	// Keyed on the struct itself, because this runs on every outbound event and the path lookup below does
	// not: GetPathName walks the outer chain and builds an FString, which is then hashed into an FName.
	if (const FCrowdyTypeID* FoundByStruct = StructToID.Find(Struct))
	{
		OutID = *FoundByStruct;
		return true;
	}

	// The path map stays the source of truth and answers for anything reached through a different
	// UScriptStruct instance for the same type.
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

		// The incumbent keeps the ID and the newcomer gets none, so serializing
		// it fails loudly instead of arriving as the incumbent's bytes.
		RecordConflict(TypeID, Existing, Struct);
		return;
	}

	const FName PathKey = FName(*Struct->GetPathName());

	IDToStruct.Add(TypeID, Struct);
	StructPathToID.Add(PathKey, TypeID);
	StructToID.Add(Struct, TypeID);
	IDToName.Add(TypeID, FName(*Struct->GetName()));

	UE_LOG(LogCrowdyNet, Warning,
		TEXT("[EventPayloadRegistry] Registered '%s' -> TypeID=%d"),
		*Struct->GetName(), TypeID);
}

void UEventPayloadRegistry::RecordConflict(const FCrowdyTypeID TypeID, const UScriptStruct* Incumbent, const UScriptStruct* Rejected)
{
	const FString IncumbentPath = Incumbent ? Incumbent->GetPathName() : FString();
	const FString RejectedPath = Rejected ? Rejected->GetPathName() : FString();

	for (const FCrowdyIDConflict& Existing : Conflicts)
	{
		if (Existing.ID == TypeID && Existing.RejectedPath == RejectedPath)
			return;
	}

	FCrowdyIDConflict Conflict;
	Conflict.RegistryName = TEXT("EventPayloadRegistry");
	Conflict.ID = TypeID;
	Conflict.IncumbentPath = IncumbentPath;
	Conflict.RejectedPath = RejectedPath;
	Conflict.Remedy = TEXT("Give one of the two an explicit TypeID via IDOverrides in CrowdySDKDeveloperSettings (Project Settings, Plugins, Crowdy SDK).");
	Conflicts.Add(Conflict);

	UE_LOG(LogCrowdyNet, Warning,
		TEXT("[EventPayloadRegistry] TypeID=%d is already held by '%s', so '%s' was not registered and cannot be sent. %s"),
		TypeID, *IncumbentPath, *RejectedPath, *Conflict.Remedy);
}

TArray<FCrowdyIDConflict> UEventPayloadRegistry::GetConflicts() const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	return Conflicts;
}

bool UEventPayloadRegistry::HasConflicts() const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	return Conflicts.Num() > 0;
}

void UEventPayloadRegistry::ClearConflicts()
{
	FRWScopeLock WriteLock(RegistryLock, SLT_Write);
	Conflicts.Reset();
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
