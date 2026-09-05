#include "Utils/CrowdyPodCopyPlan.h"

#include "CrowdyNetLog.h"
#include "Misc/ScopeRWLock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include <atomic>

namespace
{
	// A struct instance is filled in place and handed to the struct's own serializer, so the scratch has
	// to satisfy the alignment any member could ask for.
	using FCrowdyPlanScratch = TArray<uint8, TAlignedHeapAllocator<16>>;

	// Flags that make an archive skip a value, or serialize it on one side and not the other. A plan that
	// copied one would move bytes the property walk never touched.
	constexpr uint64 CrowdyPlanSkipFlags =
		CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_NonTransactional
		| CPF_Deprecated | CPF_DevelopmentAssets | CPF_SkipSerialization | CPF_EditorOnly;

	constexpr int32 CrowdyPlanProbeAttempts = 3;

	uint8 ProbeByte(const int32 Attempt, const int32 Index)
	{
		// The third term keeps the pattern from repeating every 256 bytes, so a swap of two far-apart
		// bytes cannot pass a comparison that a short struct would have caught.
		return static_cast<uint8>(0x11 * (Attempt + 1) + Index * 7 + (Index >> 8) * 3);
	}

	bool BuildRuns(const UStruct* Struct, int32 BaseOffset, TArray<FCrowdyCopyRun>& OutRuns, FString& OutReason);

	// A byte the struct's own serializer can reach but no accepted leaf covers could be a member
	// reflection never saw, and the probe below would be writing a pattern into it before calling that
	// serializer. Runs arrive in archive order, so this also rejects a struct whose leaves are out of
	// memory order.
	bool RunsTileWholeStruct(const TArray<FCrowdyCopyRun>& Runs, const int32 StructureSize)
	{
		int32 Next = 0;

		for (const FCrowdyCopyRun& Run : Runs)
		{
			if (Run.Offset != Next)
			{
				return false;
			}

			Next += Run.Size;
		}

		return Next == StructureSize;
	}

	// Fills an instance with a fixed pattern and requires the struct's own serializer to reproduce that
	// memory, at that width, in both directions. It calls SerializeItem, which is the call a nested
	// struct actually takes, and not the property walk: a struct with a native serializer admits no proof
	// from reflection, so this is the only thing that lets one into a plan.
	bool ProbeNativeSerializer(UScriptStruct* Struct, FString& OutReason)
	{
		const int32 Size = Struct->GetStructureSize();

		if (Size <= 0)
		{
			OutReason = FString::Printf(TEXT("'%s' occupies no memory"), *Struct->GetName());
			return false;
		}

		FCrowdyPlanScratch Pattern;
		Pattern.SetNumUninitialized(Size);

		FCrowdyPlanScratch Instance;
		Instance.SetNumUninitialized(Size);

		TArray<uint8> Written;

		for (int32 Attempt = 0; Attempt < CrowdyPlanProbeAttempts; ++Attempt)
		{
			for (int32 Index = 0; Index < Size; ++Index)
			{
				Pattern[Index] = ProbeByte(Attempt, Index);
			}

			FMemory::Memcpy(Instance.GetData(), Pattern.GetData(), Size);

			Written.Reset();
			{
				FMemoryWriter Writer(Written, /*bIsPersistent=*/true);
				Struct->SerializeItem(Writer, Instance.GetData(), nullptr);
			}

			if (Written.Num() != Size)
			{
				OutReason = FString::Printf(
					TEXT("'%s' serializes to %d bytes but occupies %d, so its serializer is not a straight copy"),
					*Struct->GetName(), Written.Num(), Size);
				return false;
			}

			if (FMemory::Memcmp(Written.GetData(), Pattern.GetData(), Size) != 0)
			{
				OutReason = FString::Printf(
					TEXT("'%s' serializes to bytes that are not its own memory"), *Struct->GetName());
				return false;
			}

			FMemory::Memzero(Instance.GetData(), Size);
			{
				FMemoryReader Reader(Written, /*bIsPersistent=*/true);
				Struct->SerializeItem(Reader, Instance.GetData(), nullptr);

				if (Reader.IsError() || Reader.Tell() != Size)
				{
					OutReason = FString::Printf(
						TEXT("'%s' reads back a different number of bytes than it wrote"), *Struct->GetName());
					return false;
				}
			}

			if (FMemory::Memcmp(Instance.GetData(), Pattern.GetData(), Size) != 0)
			{
				OutReason = FString::Printf(
					TEXT("'%s' reads its own bytes back as different memory"), *Struct->GetName());
				return false;
			}
		}

		return true;
	}

	bool AcceptProperty(const FProperty* Prop, const int32 BaseOffset, TArray<FCrowdyCopyRun>& OutRuns,
		FString& OutReason)
	{
		if (Prop->ArrayDim != 1)
		{
			OutReason = FString::Printf(TEXT("'%s' is a fixed-size array"), *Prop->GetName());
			return false;
		}

		if ((Prop->PropertyFlags & CrowdyPlanSkipFlags) != 0)
		{
			OutReason = FString::Printf(TEXT("'%s' carries a flag that makes an archive skip it"), *Prop->GetName());
			return false;
		}

		const int32 Offset = BaseOffset + Prop->GetOffset_ForInternal();
		const int32 Size = Prop->GetElementSize();

		// An enum reaches a persistent archive as its entry name, so its width follows the name rather
		// than its storage. An enum class and a TEnumAsByte are different property classes and reach here
		// down different branches, so refusing one says nothing about the other and each needs its own.
		if (Prop->IsA<FEnumProperty>())
		{
			OutReason = FString::Printf(TEXT("'%s' is an enum, which serializes by name"), *Prop->GetName());
			return false;
		}

		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum != nullptr)
			{
				OutReason = FString::Printf(TEXT("'%s' is an enum, which serializes by name"), *Prop->GetName());
				return false;
			}

			OutRuns.Add({ Offset, Size, false });
			return true;
		}

		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			// A bitfield shares its byte with its neighbours, so no span of memory belongs to it alone.
			if (!BoolProp->IsNativeBool())
			{
				OutReason = FString::Printf(TEXT("'%s' is a bitfield"), *Prop->GetName());
				return false;
			}

			if (Size != 1)
			{
				OutReason = FString::Printf(TEXT("'%s' is a bool that does not occupy one byte"), *Prop->GetName());
				return false;
			}

			OutRuns.Add({ Offset, 1, true });
			return true;
		}

		if (Prop->IsA<FInt8Property>() || Prop->IsA<FInt16Property>() || Prop->IsA<FIntProperty>()
			|| Prop->IsA<FInt64Property>() || Prop->IsA<FUInt16Property>() || Prop->IsA<FUInt32Property>()
			|| Prop->IsA<FUInt64Property>() || Prop->IsA<FFloatProperty>() || Prop->IsA<FDoubleProperty>())
		{
			OutRuns.Add({ Offset, Size, false });
			return true;
		}

		const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || !StructProp->Struct)
		{
			OutReason = FString::Printf(TEXT("'%s' is a %s, whose width is not its own size"),
				*Prop->GetName(), *Prop->GetClass()->GetName());
			return false;
		}

		UScriptStruct* Inner = StructProp->Struct;

		// The inner leaves have to be copyable before anything else is decided, so the probe below can
		// never write a pattern into a pointer or a length.
		TArray<FCrowdyCopyRun> InnerRuns;
		if (!BuildRuns(Inner, 0, InnerRuns, OutReason))
		{
			return false;
		}

		if (!RunsTileWholeStruct(InnerRuns, Inner->GetStructureSize()))
		{
			OutReason = FString::Printf(
				TEXT("'%s' holds memory none of its properties account for, so its own serializer cannot be probed safely"),
				*Prop->GetName());
			return false;
		}

		// A nested struct with no serializer of its own reaches a persistent archive as tagged
		// properties, names and widths and all, rather than as its packed leaves. Only a struct that
		// serializes itself can be shown to be a copy.
		if (!Inner->UseNativeSerialization())
		{
			OutReason = FString::Printf(
				TEXT("'%s' is a nested struct with no serializer of its own, so the archive writes its property names rather than its bytes"),
				*Prop->GetName());
			return false;
		}

		if (!ProbeNativeSerializer(Inner, OutReason))
		{
			return false;
		}

		OutRuns.Add({ Offset, Size, false });
		return true;
	}

	bool BuildRuns(const UStruct* Struct, const int32 BaseOffset, TArray<FCrowdyCopyRun>& OutRuns, FString& OutReason)
	{
		// PropertyLink in this exact order is what UStruct::SerializeBin walks, so the plan's field order
		// is the archive's rather than the declaration order a header happens to read in.
		const int32 FirstRun = OutRuns.Num();

		for (const FProperty* Prop = Struct->PropertyLink; Prop != nullptr; Prop = Prop->PropertyLinkNext)
		{
			if (!AcceptProperty(Prop, BaseOffset, OutRuns, OutReason))
			{
				return false;
			}
		}

		if (OutRuns.Num() == FirstRun)
		{
			OutReason = FString::Printf(TEXT("'%s' has no properties to copy"), *Struct->GetName());
			return false;
		}

		return true;
	}

	// The claim the whole plan rests on, turned into a per-struct fact rather than an argument: the plan
	// and the property walk are run against each other over several byte patterns, in both directions,
	// and a struct that differs by one octet never gets a plan.
	bool MatchesReflectiveWalk(const UScriptStruct* Struct, const FCrowdyCopyPlan& Plan, FString& OutReason)
	{
		const int32 Size = Struct->GetStructureSize();

		if (Size <= 0)
		{
			OutReason = FString::Printf(TEXT("'%s' occupies no memory"), *Struct->GetName());
			return false;
		}

		FCrowdyPlanScratch Pattern;
		Pattern.SetNumUninitialized(Size);

		FCrowdyPlanScratch Source;
		Source.SetNumUninitialized(Size);

		FCrowdyPlanScratch WalkedIn;
		WalkedIn.SetNumUninitialized(Size);

		FCrowdyPlanScratch PlannedIn;
		PlannedIn.SetNumUninitialized(Size);

		TArray<uint8> Walked;
		TArray<uint8> Planned;

		for (int32 Attempt = 0; Attempt < CrowdyPlanProbeAttempts; ++Attempt)
		{
			for (int32 Index = 0; Index < Size; ++Index)
			{
				Pattern[Index] = ProbeByte(Attempt, Index);
			}

			// The property walk normalises a bool byte in the struct it is writing FROM, so each side
			// gets its own copy of the pattern rather than sharing one.
			FMemory::Memcpy(Source.GetData(), Pattern.GetData(), Size);
			Walked.Reset();
			{
				FMemoryWriter Writer(Walked, /*bIsPersistent=*/true);
				Struct->SerializeBin(Writer, Source.GetData());
			}

			FMemory::Memcpy(Source.GetData(), Pattern.GetData(), Size);
			Planned.Reset();
			{
				FMemoryWriter Writer(Planned, /*bIsPersistent=*/true);
				if (!CrowdyPodCopyPlan::Apply(Plan, Writer, Source.GetData()))
				{
					OutReason = FString::Printf(
						TEXT("'%s' has a plan that refused a plain binary archive"), *Struct->GetName());
					return false;
				}
			}

			if (Walked != Planned)
			{
				OutReason = FString::Printf(
					TEXT("'%s' does not write the same bytes through its plan as through its properties (%d against %d)"),
					*Struct->GetName(), Planned.Num(), Walked.Num());
				return false;
			}

			FMemory::Memzero(WalkedIn.GetData(), Size);
			int64 WalkedTell = 0;
			{
				FMemoryReader Reader(Walked, /*bIsPersistent=*/true);
				Struct->SerializeBin(Reader, WalkedIn.GetData());
				WalkedTell = Reader.Tell();

				if (Reader.IsError())
				{
					OutReason = FString::Printf(
						TEXT("'%s' cannot read back the bytes it just wrote"), *Struct->GetName());
					return false;
				}
			}

			FMemory::Memzero(PlannedIn.GetData(), Size);
			int64 PlannedTell = 0;
			{
				FMemoryReader Reader(Walked, /*bIsPersistent=*/true);
				if (!CrowdyPodCopyPlan::Apply(Plan, Reader, PlannedIn.GetData()))
				{
					OutReason = FString::Printf(
						TEXT("'%s' has a plan that refused to read a plain binary archive"), *Struct->GetName());
					return false;
				}

				PlannedTell = Reader.Tell();
			}

			// The reader's position is what the caller reports an unread tail from, so a plan that landed
			// the right bytes at the wrong position would still be wrong.
			if (PlannedTell != WalkedTell)
			{
				OutReason = FString::Printf(
					TEXT("'%s' leaves the reader at %d through its plan and at %d through its properties"),
					*Struct->GetName(), static_cast<int32>(PlannedTell), static_cast<int32>(WalkedTell));
				return false;
			}

			if (FMemory::Memcmp(WalkedIn.GetData(), PlannedIn.GetData(), Size) != 0)
			{
				OutReason = FString::Printf(
					TEXT("'%s' does not read back to the same memory through its plan as through its properties"),
					*Struct->GetName());
				return false;
			}
		}

		return true;
	}

	FRWLock PlanCacheLock;
	TMap<TObjectKey<UScriptStruct>, TUniquePtr<FCrowdyCopyPlan>> PlanCache;

	// A plan dropped by an invalidation is kept rather than freed: a decode on another thread may still
	// be holding the pointer Find handed it.
	TArray<TUniquePtr<FCrowdyCopyPlan>> RetiredPlans;

	std::atomic<int32> PlanBuildCount{ 0 };
	bool bPlanReloadHookInstalled = false;
}

FCrowdyCopyPlan CrowdyPodCopyPlan::Build(const UScriptStruct* Struct, FString& OutReason)
{
	OutReason.Reset();

	if (!Struct)
	{
		OutReason = TEXT("no struct");
		return FCrowdyCopyPlan();
	}

	// A Blueprint-authored struct is recompiled into the same object: its properties are destroyed and
	// its offsets re-linked in place, and nothing a cache can key on changes. So a plan built for the old
	// layout would still be found and would move bytes at offsets that have moved. It keeps the
	// reflective walk permanently, which reads the property chain afresh on every call.
	if (Struct->IsA<UUserDefinedStruct>())
	{
		OutReason = FString::Printf(
			TEXT("'%s' is a Blueprint-authored struct, whose layout is rebuilt in place with nothing to notice it by"),
			*Struct->GetName());
		return FCrowdyCopyPlan();
	}

	TArray<FCrowdyCopyRun> Runs;
	if (!BuildRuns(Struct, 0, Runs, OutReason))
	{
		return FCrowdyCopyPlan();
	}

	FCrowdyCopyPlan Plan;

	for (const FCrowdyCopyRun& Run : Runs)
	{
		if (Plan.Runs.Num() > 0)
		{
			FCrowdyCopyRun& Last = Plan.Runs.Last();

			// The walk follows the archive's order, so a run starting before the previous one ended means
			// two fields claim the same bytes.
			if (Run.Offset < Last.Offset + Last.Size)
			{
				OutReason = FString::Printf(TEXT("'%s' has two fields that overlap in memory"), *Struct->GetName());
				return FCrowdyCopyPlan();
			}

			// Merge what is already adjacent, so a fully packed struct costs one copy rather than one per
			// field. A normalised bool is not a copy, so it stays a span of its own.
			if (!Last.bNormalisedBool && !Run.bNormalisedBool && Last.Offset + Last.Size == Run.Offset)
			{
				Last.Size += Run.Size;
				Plan.TotalBytes += Run.Size;
				continue;
			}
		}

		Plan.Runs.Add(Run);
		Plan.TotalBytes += Run.Size;
	}

	if (!MatchesReflectiveWalk(Struct, Plan, OutReason))
	{
		return FCrowdyCopyPlan();
	}

	return Plan;
}

bool CrowdyPodCopyPlan::Apply(const FCrowdyCopyPlan& Plan, FArchive& Ar, void* Container)
{
	if (!Plan.IsValid() || Container == nullptr)
	{
		return false;
	}

	// The plan reproduces one archive shape, the plain binary property walk. Anything that makes that
	// walk take a different branch, or reorder the bytes it moves, has to keep the walk.
	if (Ar.IsObjectReferenceCollector() || Ar.ArUseCustomPropertyList || Ar.IsSaveGame() || Ar.IsByteSwapping())
	{
		return false;
	}

	// A payload that ends inside the struct keeps the property walk: the walk stops at the first field
	// that does not fit and leaves that field and every later one at its default, which is a decision
	// made at field boundaries and not at the boundaries of a merged span.
	if (Ar.IsLoading() && Ar.TotalSize() - Ar.Tell() < Plan.TotalBytes)
	{
		return false;
	}

	uint8* const Bytes = static_cast<uint8*>(Container);
	const bool bLoading = Ar.IsLoading();

	for (const FCrowdyCopyRun& Run : Plan.Runs)
	{
		if (!Run.bNormalisedBool)
		{
			Ar.Serialize(Bytes + Run.Offset, Run.Size);
			continue;
		}

		uint8 Value = bLoading ? 0 : (Bytes[Run.Offset] != 0 ? 1 : 0);
		Ar.Serialize(&Value, 1);

		if (bLoading)
		{
			Bytes[Run.Offset] = Value != 0 ? 1 : 0;
		}
	}

	// The archive's own verdict, so a true never means more than the archive is willing to say.
	return !Ar.IsError();
}

const FCrowdyCopyPlan* CrowdyPodCopyPlan::Find(const UScriptStruct* Struct)
{
	if (!Struct)
	{
		return nullptr;
	}

	// Keyed by an object key rather than by the address: a reinstanced struct is a different object at a
	// address the old one may have been freed from, and a stale entry has to miss rather than match.
	const TObjectKey<UScriptStruct> Key(Struct);

	{
		FRWScopeLock ReadLock(PlanCacheLock, SLT_ReadOnly);
		if (const TUniquePtr<FCrowdyCopyPlan>* Existing = PlanCache.Find(Key))
		{
			return (*Existing)->IsValid() ? Existing->Get() : nullptr;
		}
	}

	// Built outside the lock: a walk that probes nested structs is far too long to hold a lock every
	// decode has to take.
	FString Reason;
	FCrowdyCopyPlan Built = Build(Struct, Reason);

	FRWScopeLock WriteLock(PlanCacheLock, SLT_Write);

	if (const TUniquePtr<FCrowdyCopyPlan>* Existing = PlanCache.Find(Key))
	{
		return (*Existing)->IsValid() ? Existing->Get() : nullptr;
	}

#if WITH_EDITOR
	// A decode can build the first plan on a receive thread, and a multicast delegate is not safe to add
	// to from one, so the hook waits for a build that happens where it can be installed.
	if (!bPlanReloadHookInstalled && IsInGameThread())
	{
		bPlanReloadHookInstalled = true;

		// Live Coding rebuilds native reflection, so every offset in every cached plan goes stale at once.
		FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda(
			[](EReloadCompleteReason)
			{
				CrowdyPodCopyPlan::InvalidateAll();
			});
	}
#endif

	UE_CLOG(!Built.IsValid(), LogCrowdyNet, Log,
		TEXT("[PodCopyPlan] '%s' keeps the reflective walk: %s"), *Struct->GetName(), *Reason);

	PlanBuildCount.fetch_add(1, std::memory_order_relaxed);

	const TUniquePtr<FCrowdyCopyPlan>& Added = PlanCache.Add(Key, MakeUnique<FCrowdyCopyPlan>(MoveTemp(Built)));
	return Added->IsValid() ? Added.Get() : nullptr;
}

void CrowdyPodCopyPlan::InvalidateAll()
{
	FRWScopeLock WriteLock(PlanCacheLock, SLT_Write);

	for (TPair<TObjectKey<UScriptStruct>, TUniquePtr<FCrowdyCopyPlan>>& Entry : PlanCache)
	{
		RetiredPlans.Add(MoveTemp(Entry.Value));
	}

	PlanCache.Reset();
	PlanBuildCount.store(0, std::memory_order_relaxed);
}

#if WITH_DEV_AUTOMATION_TESTS
int32 CrowdyPodCopyPlan::GetBuildCount()
{
	return PlanBuildCount.load(std::memory_order_relaxed);
}

void CrowdyPodCopyPlan::InstallForTests(const UScriptStruct* Struct, FCrowdyCopyPlan Plan)
{
	if (!Struct)
	{
		return;
	}

	const TObjectKey<UScriptStruct> Key(Struct);

	FRWScopeLock WriteLock(PlanCacheLock, SLT_Write);

	if (TUniquePtr<FCrowdyCopyPlan>* Existing = PlanCache.Find(Key))
	{
		RetiredPlans.Add(MoveTemp(*Existing));
	}

	PlanCache.Add(Key, MakeUnique<FCrowdyCopyPlan>(MoveTemp(Plan)));
}
#endif
