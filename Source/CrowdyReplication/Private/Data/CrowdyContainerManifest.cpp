#include "Data/CrowdyContainerManifest.h"

#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
	// FString's own set semantics ignore case; type names and keys are compared exactly everywhere else.
	struct FCrowdyManifestRowKeyFuncs : BaseKeyFuncs<FString, FString, false>
	{
		static const FString& GetSetKey(const FString& Element) { return Element; }
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
	};
}

TArray<int32> UCrowdyContainerManifest::FindDuplicateRows() const
{
	TArray<int32> Duplicates;
	TSet<FString, FCrowdyManifestRowKeyFuncs> Seen;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		bool bAlready = false;
		Seen.Add(Rows[Index].TypeName + TEXT("|") + Rows[Index].BindingKey, &bAlready);
		if (bAlready)
		{
			Duplicates.Add(Index);
		}
	}
	return Duplicates;
}

#if WITH_EDITOR
EDataValidationResult UCrowdyContainerManifest::IsDataValid(FDataValidationContext& ValidationContext) const
{
	const EDataValidationResult Base = Super::IsDataValid(ValidationContext);
	const TArray<int32> Duplicates = FindDuplicateRows();
	for (const int32 Index : Duplicates)
	{
		ValidationContext.AddError(FText::Format(
			NSLOCTEXT("CrowdyContainerManifest", "DuplicateRow", "Row {0} ({1}) repeats the binding key of an earlier row of type {2}; two placements would share one server row."),
			Index, FText::FromString(Rows[Index].DisplayName), FText::FromString(Rows[Index].TypeName)));
	}
	if (Duplicates.Num() > 0)
	{
		return EDataValidationResult::Invalid;
	}
	return Base == EDataValidationResult::NotValidated ? EDataValidationResult::Valid : Base;
}
#endif

FGuid FCrowdyContainerManifestKeys::ActorNetID(const FGuid& PlacementGuid, const FString& BindingKeyOverride)
{
	return BindingKeyOverride.IsEmpty() ? PlacementGuid : FCrowdyModelIdentity::NetIDFromBindingKey(BindingKeyOverride);
}

FString FCrowdyContainerManifestKeys::KeyForNetID(const FGuid& NetID)
{
	return NetID.IsValid() ? FCrowdyModelIdentity::NetIDToContainerKey(NetID) : FString();
}

FString FCrowdyContainerManifestKeys::ComponentKey(const FGuid& AnchorNetID, const UClass* ComponentClass,
	const FString& InstanceTerm)
{
	return KeyForNetID(UCrowdyEntitySubsystem::DeriveSubParticipantID(AnchorNetID, ComponentClass, InstanceTerm));
}
