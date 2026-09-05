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

	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);

	// Keyed on the struct itself, because this runs once per outbound update and the path lookup below
	// does not: GetPathName walks the outer chain and builds an FString, which is then hashed into an
	// FName, so keying on the name alone spent a heap allocation per send to rediscover something the
	// caller already had a pointer to. The pointer cannot dangle while it is a key here, since
	// IDToStruct holds every registered struct through a TObjectPtr for exactly as long.
	if (const FCrowdyTypeID* FoundByStruct = StructToID.Find(Struct))
	{
		OutID = *FoundByStruct;
		return true;
	}

	// The path map stays the source of truth and answers for anything registered before the struct map
	// existed, or reached through a different UScriptStruct instance for the same type.
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

	// Zero is not an assignable id. GenerateFromStruct never produces it, but LoadFromDataAsset and the
	// override resolver both assign ids outright, and the actor-state framing leads with a reserved zero
	// to tell a framed message from one predating the framing. A struct registered here would answer to
	// that sentinel and be decoded out of a frame's leading bytes.
	if (TypeID == CROWDY_INVALID_TYPE_ID)
	{
		UE_LOG(LogCrowdyNet, Error,
			TEXT("[ActorUpdatePayloadRegistry] '%s' was given TypeID 0, which is reserved and never assignable. Registration refused."),
			*Struct->GetName());
		return;
	}

	FRWScopeLock WriteLock(RegistryLock, SLT_Write);

	if (IDToStruct.Contains(TypeID))
	{
		const UScriptStruct* Existing = IDToStruct[TypeID].Get();
		if (Existing == Struct) return;

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

	UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log,
		TEXT("[ActorUpdatePayloadRegistry] Registered '%s' -> TypeID=%d | Path=%s"),
		*Struct->GetName(), TypeID, *Struct->GetPathName());
}

void UActorUpdatePayloadRegistry::RecordConflict(const FCrowdyTypeID TypeID, const UScriptStruct* Incumbent, const UScriptStruct* Rejected)
{
	const FString IncumbentPath = Incumbent ? Incumbent->GetPathName() : FString();
	const FString RejectedPath = Rejected ? Rejected->GetPathName() : FString();

	for (const FCrowdyIDConflict& Existing : Conflicts)
	{
		if (Existing.ID == TypeID && Existing.RejectedPath == RejectedPath)
			return;
	}

	FCrowdyIDConflict Conflict;
	Conflict.RegistryName = TEXT("ActorUpdatePayloadRegistry");
	Conflict.ID = TypeID;
	Conflict.IncumbentPath = IncumbentPath;
	Conflict.RejectedPath = RejectedPath;
	Conflict.Remedy = TEXT("Give one of the two an explicit TypeID via IDOverrides in CrowdySDKDeveloperSettings (Project Settings, Plugins, Crowdy SDK).");
	Conflicts.Add(Conflict);

	UE_LOG(LogCrowdyNet, Warning,
		TEXT("[ActorUpdatePayloadRegistry] TypeID=%d is already held by '%s', so '%s' was not registered and cannot be sent. %s"),
		TypeID, *IncumbentPath, *RejectedPath, *Conflict.Remedy);
}

TArray<FCrowdyIDConflict> UActorUpdatePayloadRegistry::GetConflicts() const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	return Conflicts;
}

bool UActorUpdatePayloadRegistry::HasConflicts() const
{
	FRWScopeLock ReadLock(RegistryLock, SLT_ReadOnly);
	return Conflicts.Num() > 0;
}

void UActorUpdatePayloadRegistry::ClearConflicts()
{
	FRWScopeLock WriteLock(RegistryLock, SLT_Write);
	Conflicts.Reset();
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
