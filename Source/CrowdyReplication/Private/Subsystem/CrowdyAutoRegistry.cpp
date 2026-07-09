// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyAutoRegistry.h"
#include "CrowdyReplicationLog.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Utils/UEventPayloadRegistry.h"
#include "UObject/UObjectGlobals.h"   // FCoreUObjectDelegates, EReloadCompleteReason
#include "UObject/UObjectIterator.h"

void UCrowdyAutoRegistry::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Ensure singleton registries exist before we feed them
	UEventPayloadRegistry::Get();
	UActorUpdatePayloadRegistry::Get();
	UCrowdyClassRegistry::Get();

	InstallIDOverrideResolvers();

	// Entity classes first, then seal: the RPC scan keys the inbound resolver by
	// the (possibly overridden) class id, so the class registry must be final
	// before it runs. Nothing past this point registers classes only structs,
	// whose registries stay open so sealing here is safe.
	ScanAndRegisterEntityClasses();
	UCrowdyClassRegistry::Get()->Seal();

	RegisterLoadedPayloadTypes();

#if WITH_EDITOR
	// Live Coding recycles native FUNCTION and FPROPERTY addresses across many classes at once, so both
	// the RPC maps and the CrowdyState layout cache (which holds raw FProperty pointers) go stale
	// together; rebuild both wholesale when a reload completes.
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddWeakLambda(
		this, [this](EReloadCompleteReason)
		{
			ScanAndRegisterRpcFunctions();
			ScanAndRegisterRepLayouts();
		});
#endif
}

void UCrowdyAutoRegistry::RegisterLoadedPayloadTypes()
{
	// FCrowdyRpcCall is the fixed wire payload every RPC-style CrowdyEvent rides.
	// Registering it here gives it a stable EventType on both ends, so the dispatch
	// path can resolve the type when sending and the router can recognize it on receipt.
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyRpcCall::StaticStruct());

	// FCrowdyStateDelta is the fixed wire payload every CrowdyState delta rides; registering it here gives
	// it a stable EventType so the send path resolves it and (Phase 4) the router recognizes it on receipt.
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyStateDelta::StaticStruct());

	ScanAndRegisterExecutorStateStructs();
	ScanAndRegisterRpcFunctions();
	ScanAndRegisterRepLayouts();
}

void UCrowdyAutoRegistry::Deinitialize()
{
#if WITH_EDITOR
	if (ReloadCompleteHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
		ReloadCompleteHandle.Reset();
	}
#endif

	UEventPayloadRegistry::Shutdown();
	UActorUpdatePayloadRegistry::Shutdown();
	UCrowdyClassRegistry::Shutdown();
	Super::Deinitialize();
}

void UCrowdyAutoRegistry::InstallIDOverrideResolvers()
{
	// Reads the settings CDO on every call instead of capturing state, so the
	// resolver stays valid for the registry singleton's whole lifetime.
	const auto Resolver = [](const UScriptStruct* Struct) -> FCrowdyTypeID
	{
		for (const FCrowdyIDOverride& Override : GetDefault<UCrowdySDKDeveloperSettings>()->IDOverrides)
		{
			if (Override.Struct.Get() == Struct)
			{
				UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyAutoRegistry] Using override ID=%d for '%s'"),
					Override.OverrideID, *Struct->GetName());
				return static_cast<FCrowdyTypeID>(Override.OverrideID);
			}
		}

		return FCrowdyTypeIDGenerator::GenerateFromStruct(Struct);
	};

	UEventPayloadRegistry::Get()->SetIDResolver(Resolver);
	UActorUpdatePayloadRegistry::Get()->SetIDResolver(Resolver);
}

void UCrowdyAutoRegistry::ScanAndRegisterExecutorStateStructs()
{
	// Executors declare their snapshot struct via GetStateStruct(). Native
	// executors are covered here; Blueprint executors register when the
	// entity component carrying them begins play.
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf<UActorUpdateExecutor>()) continue;
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

		const FString ClassName = Class->GetName();
		if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_"))) continue;

		// This scan also runs during map loads (RegisterLoadedPayloadTypes from
		// world init) never force CDO creation on a class that is still
		// loading/compiling. Blueprint executors whose CDO isn't ready yet
		// register when the carrying entity component begins play.
		const UActorUpdateExecutor* Executor = Cast<UActorUpdateExecutor>(Class->GetDefaultObject(false));
		if (!Executor) continue;

		if (UScriptStruct* StateStruct = Executor->GetStateStruct())
		{
			UActorUpdatePayloadRegistry::Get()->RegisterStructAuto(StateStruct);
		}
	}
}

void UCrowdyAutoRegistry::ScanAndRegisterEntityClasses()
{
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf<AActor>()) continue;
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

		// Skeleton/reinstanced classes shadow the real Blueprint class
		const FString ClassName = Class->GetName();
		if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_"))) continue;

		if (!HasEntityComponent(Class)) continue;

		const FCrowdyClassID ClassID = ResolveClassID(Class);
		if (UCrowdyClassRegistry::Get()->RegisterClass(ClassID, FSoftClassPath(Class)))
		{
			UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyAutoRegistry] Registered entity class '%s' -> ClassID=%u"),
				*Class->GetName(), ClassID);
		}
	}

	// Blueprint entity classes that are not loaded yet cannot be discovered
	// here cheaply; spawn events carry the class path as a fallback, so a
	// receiver can still resolve an unknown ClassID. Manual overrides register
	// by path below so both sides agree without loading the class.
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	for (const FCrowdyClassIDOverride& Override : Settings->ClassIDOverrides)
	{
		if (Override.Class.IsNull() || Override.OverrideID == CROWDY_INVALID_CLASS_ID) continue;

		UCrowdyClassRegistry::Get()->RegisterClass(Override.OverrideID,
			FSoftClassPath(Override.Class.ToSoftObjectPath().ToString()));
	}
}

void UCrowdyAutoRegistry::ScanAndRegisterRpcFunctions()
{
	// Rebuilt wholesale: classes reinstanced on a level load or Live Coding pass
	// invalidate the UFunction pointers held here.
	RpcFunctionInfo.Reset();
	RpcFunctionResolver.Reset();

	for (TObjectIterator<UClass> It; It; ++It)
	{
		RegisterClassRpcFunctions(*It);
	}

	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyAutoRegistry] Discovered %d CrowdyEvent function(s)."),
		RpcFunctionInfo.Num());
}

void UCrowdyAutoRegistry::RegisterClassRpcFunctions(UClass* Class)
{
	if (!Class) return;

	const FString ClassName = Class->GetName();
	if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_"))) return;

	// The id is per-class, not per-function resolve it once. It also stamps each entry's
	// OwnerClassID so an incremental rescan can later evict this class without touching its keys.
	const FCrowdyClassID ClassID = UCrowdyClassRegistry::Get()->GetID(Class);

	// ExcludeSuper visits each function once, on the class that declares it
	// which is its owner class. That keeps the resolver key (declaring ClassID +
	// FunctionID) aligned with what the sender stamps and with the class the
	// FunctionID hash embeds, so inherited handlers resolve without an entry per
	// subclass (FindFunctionByName returns the same owner UFunction either way).
	for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Function = *FuncIt;
		if (!UCrowdyBakedRegistry::IsEventHandlerFunction(Function)) continue;

		// An unsupported signature is the earliest place a mistake surfaces fail it
		// here with a readable error instead of letting it mis-serialize on the wire.
		// Skipping registration leaves the inbound resolver without an entry, so a call
		// that does go out drops cleanly on receipt rather than corrupting a dispatch.
		const FString SignatureProblem = FCrowdyRPC::DescribeSignatureProblem(Function);
		if (!SignatureProblem.IsEmpty())
		{
			UE_LOG(LogCrowdyRPC, Error,
				TEXT("[CrowdyAutoRegistry] CrowdyEvent '%s::%s' %s — not registered; calls to it will be dropped."),
				*ClassName, *Function->GetName(), *SignatureProblem);
			continue;
		}

		FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Function);

		// A Multicast RPC rides the channel payload, which is capped. If even its fixed-width
		// parameters can never fit, registering it would only let it drop at send time reject
		// it here with a readable error instead, the same stance as a bad signature.
		if (Info.Recipient == ECrowdyEventRecipient::Multicast)
		{
			const int32 MinBytes = FCrowdyRPC::EstimateMinChannelPayloadBytes(Function);
			if (MinBytes > CrowdyChannelPayloadMaxBytes)
			{
				UE_LOG(LogCrowdyRPC, Error,
					TEXT("[CrowdyAutoRegistry] Multicast CrowdyEvent '%s::%s' needs at least %d bytes, over the %d-byte channel limit — not registered; calls to it will be dropped. Use Spatial Multicast or shrink its parameters."),
					*ClassName, *Function->GetName(), MinBytes, CrowdyChannelPayloadMaxBytes);
				continue;
			}
		}

		Info.OwnerClassID = ClassID;
		RpcFunctionInfo.Add(Function, Info);
		RpcFunctionResolver.Add(TPair<int64, int64>(static_cast<int64>(ClassID), Info.FunctionID), Function);
	}
}

void UCrowdyAutoRegistry::RemoveClassRpcFunctions(FCrowdyClassID ClassID)
{
	if (ClassID == CROWDY_INVALID_CLASS_ID) return;

	// Identify entries by owned data (OwnerClassID / the resolver's class-id key) and remove the
	// slot currently being visited. The outbound key may be a stale UFunction pointer after a
	// reinstance, so it is never dereferenced only used as a hash key by RemoveCurrent.
	for (auto It = RpcFunctionInfo.CreateIterator(); It; ++It)
	{
		if (It->Value.OwnerClassID == ClassID)
		{
			It.RemoveCurrent();
		}
	}

	const int64 ClassKey = static_cast<int64>(ClassID);
	for (auto It = RpcFunctionResolver.CreateIterator(); It; ++It)
	{
		if (It->Key.Key == ClassKey)
		{
			It.RemoveCurrent();
		}
	}
}

void UCrowdyAutoRegistry::UpdateClassRpcFunctions(UClass* Class)
{
	if (!Class) return;

	// Evict by the class's stable, path-derived id (it survives the recompile that changed the
	// UFunction addresses), then re-scan just this class. Far cheaper than ScanAndRegisterRpcFunctions'
	// full TObjectIterator<UClass> sweep, which is what made every Blueprint compile hitch.
	RemoveClassRpcFunctions(UCrowdyClassRegistry::Get()->GetID(Class));
	RegisterClassRpcFunctions(Class);
}

void UCrowdyAutoRegistry::RescanRpcFunctions()
{
	ScanAndRegisterRpcFunctions();
}

void UCrowdyAutoRegistry::ScanAndRegisterRepLayouts()
{
	// Rebuilt wholesale: a cached layout holds raw FProperty pointers, which a level load or Live Coding
	// pass invalidates across many classes at once.
	RepLayouts.Reset();

	for (TObjectIterator<UClass> It; It; ++It)
	{
		RegisterClassRepLayout(*It);
	}

	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyAutoRegistry] Discovered %d CrowdyState rep layout(s)."),
		RepLayouts.Num());
}

void UCrowdyAutoRegistry::RegisterClassRepLayout(UClass* Class)
{
	if (!Class) return;

	const FString ClassName = Class->GetName();
	if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_"))) return;

	// Live/WITH_METADATA build path. A class with no accepted CrowdyState properties yields an invalid
	// layout, which is simply not cached (no CrowdyState entities of that class). FindRepLayout serves
	// the cooked path from the baked table, so this eager sweep is an editor optimization only.
	FCrowdyRepLayout Layout;
	if (!FCrowdyStateLayoutBuilder::BuildLayout(Class, Layout)) return;

	// Drop any property that also lives in the executor's continuous-channel state struct before caching,
	// so the eager sweep and the lazy FindRepLayout path cache the same filtered layout.
	ApplyExecutorOverlapFilter(Layout);

	// Clear invalid OnRep bindings (missing / not parameterless) on the same built layout, so live == baked.
	ValidateOnRepSignatures(Layout);

	// Filtering may have emptied the layout (every CrowdyState property overlapped); do not cache a
	// now-invalid layout, matching the negative-not-cached rule in FindRepLayout.
	if (!Layout.IsValid()) return;

	// Key by the stable, path-derived class id (survives BP reinstance), matching the RPC eviction path.
	const FCrowdyClassID ClassID = UCrowdyClassRegistry::Get()->GetID(Class);
	RepLayouts.Add(ClassID, MakeUnique<FCrowdyRepLayout>(MoveTemp(Layout)));
}

void UCrowdyAutoRegistry::RemoveClassRepLayout(FCrowdyClassID ClassID)
{
	if (ClassID == CROWDY_INVALID_CLASS_ID) return;

	// Removed by id, never by dereferencing the layout's FProperty pointers a recompile may have left
	// them stale.
	RepLayouts.Remove(ClassID);
}

void UCrowdyAutoRegistry::UpdateClassRepLayout(UClass* Class)
{
	if (!Class) return;

	// Evict by the class's stable, path-derived id (it survives the recompile that invalidated the
	// FProperty addresses), then rebuild just this class the same incremental pattern as
	// UpdateClassRpcFunctions, avoiding the full TObjectIterator<UClass> sweep on every Blueprint compile.
	RemoveClassRepLayout(UCrowdyClassRegistry::Get()->GetID(Class));
	RegisterClassRepLayout(Class);
}

void UCrowdyAutoRegistry::RescanRepLayouts()
{
	ScanAndRegisterRepLayouts();
}

bool UCrowdyAutoRegistry::BuildLayoutFromBaked(const UClass* Class, const UCrowdyBakedRegistry* Baked, FCrowdyRepLayout& Out)
{
	Out = FCrowdyRepLayout();
	if (!Class || !Baked) return false;

	const FSoftClassPath ClassPath(Class);
	const TArray<FCrowdyBakedRepProperty>* BakedProps = Baked->FindRepProperties(ClassPath);
	if (!BakedProps) return false;

	// The baker stores rows already ordered by LayoutOrder and BuildLookups preserves that order, so the
	// baked rows here reproduce declaration/positional order without re-sorting. Re-resolve each
	// FProperty* by name against the live class (the baked table never carries the pointer) and copy the
	// frozen identity/flags, so the assembled layout matches the live builder's output entry-for-entry.
	Out.OwnerClass = Class;
	Out.Properties.Reserve(BakedProps->Num());
	for (const FCrowdyBakedRepProperty& Row : *BakedProps)
	{
		FCrowdyRepProperty& Prop = Out.Properties.AddDefaulted_GetRef();
		Prop.Property = Class->FindPropertyByName(Row.PropertyName);
		Prop.PropertyID = Row.PropertyID;
		Prop.bOwnerOnly = Row.bOwnerOnly;
		Prop.bManualDirty = Row.bManualDirty;
		Prop.bHeartbeat = Row.bHeartbeat;
		Prop.OnRepFunctionName = Row.OnRepFunctionName;
	}

	Out.LayoutHash = Baked->FindRepLayoutHash(ClassPath);
	return Out.IsValid();
}

const FCrowdyRepLayout* UCrowdyAutoRegistry::FindRepLayout(const UClass* Class) const
{
	if (!Class) return nullptr;

	const FCrowdyClassID Id = UCrowdyClassRegistry::Get()->GetID(Class);
	if (const TUniquePtr<FCrowdyRepLayout>* Cached = RepLayouts.Find(Id))
	{
		return Cached->Get();
	}

	// Build live from reflection in the editor (WITH_METADATA) and from the baked table in a cooked
	// build, where UPROPERTY metadata is stripped so the live builder yields nothing. Both paths produce
	// an identical layout.
	FCrowdyRepLayout Built;
#if WITH_METADATA
	FCrowdyStateLayoutBuilder::BuildLayout(Class, Built);
#else
	BuildLayoutFromBaked(Class, UCrowdyBakedRegistry::Get(), Built);
#endif

	// Drop executor-overlapping properties before the validity check, so an all-overlapping class caches
	// nothing rather than an invalid layout. The cache-hit path above is already filtered.
	ApplyExecutorOverlapFilter(Built);

	// Clear invalid OnRep bindings on the same built layout, so the lazy path matches the eager one.
	ValidateOnRepSignatures(Built);

	// Never cache a negative: a class with no CrowdyState properties should re-probe cheaply rather than
	// pin an empty entry that a later bake/reload would need to invalidate.
	if (!Built.IsValid()) return nullptr;

	return RepLayouts.Add(Id, MakeUnique<FCrowdyRepLayout>(MoveTemp(Built))).Get();
}

void UCrowdyAutoRegistry::CollectMulticastChannels(TSet<FString>& OutChannelNames, bool& bOutUsesDefaultChannel) const
{
	bOutUsesDefaultChannel = false;
	for (const TPair<const UFunction*, FCrowdyFnInfo>& Pair : RpcFunctionInfo)
	{
		if (Pair.Value.Recipient != ECrowdyEventRecipient::Multicast)
		{
			continue;
		}

		if (Pair.Value.ChannelName.IsEmpty())
		{
			bOutUsesDefaultChannel = true;
		}
		else
		{
			OutChannelNames.Add(Pair.Value.ChannelName);
		}
	}
}

const FCrowdyFnInfo* UCrowdyAutoRegistry::FindFunctionInfo(const UFunction* Function) const
{
	return RpcFunctionInfo.Find(Function);
}

UFunction* UCrowdyAutoRegistry::ResolveFunction(int64 ClassID, int64 FunctionID) const
{
	const TObjectPtr<UFunction>* Found = RpcFunctionResolver.Find(TPair<int64, int64>(ClassID, FunctionID));
	return Found ? *Found : nullptr;
}

bool UCrowdyAutoRegistry::HasEntityComponent(const UClass* Class)
{
#if WITH_EDITOR
	// Stamped by CrowdyBlueprintCompilerExtension on Blueprint classes and
	// usable directly in UCLASS specifiers for C++ classes.
	if (Class->HasMetaData(TEXT("CrowdyEntity"))) return true;
#endif

	// Blueprint-added components live in the construction script of each
	// Blueprint class in the parent chain; native components live on the first
	// native ancestor's CDO. Never force CDO creation classes may still be
	// loading when this runs.
	for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
	{
		if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Current))
		{
			if (!BPClass->SimpleConstructionScript) continue;

			for (const USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf<UCrowdyEntityComponent>())
					return true;
			}
		}
		else
		{
			const AActor* CDO = Cast<AActor>(Current->GetDefaultObject(false));
			return CDO && CDO->FindComponentByClass<UCrowdyEntityComponent>() != nullptr;
		}
	}

	return false;
}

const UCrowdyEntityComponent* UCrowdyAutoRegistry::ResolveDefaultEntityComponent(const UClass* ActorClass)
{
	if (!ActorClass || !ActorClass->IsChildOf<AActor>())
	{
		return nullptr;
	}

	// Mirror HasEntityComponent's chain walk, but reach the component INSTANCE rather than just a bool.
	// Never force CDO creation (GetDefaultObject(false)) classes may still be compiling when this runs
	// during the world-init sweep.
	for (const UClass* Current = ActorClass; Current; Current = Current->GetSuperClass())
	{
		if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Current))
		{
			if (!BPClass->SimpleConstructionScript)
			{
				continue;
			}

			for (const USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf<UCrowdyEntityComponent>())
				{
					// The SCS template carries the component's Instanced defaults (Ownership, HostOverride,
					// StateExecutor, ...), so the Blueprint's configured defaults are visible here without
					// spawning. A matched node with no live template (Component still null) falls through to
					// walk the superclass chain rather than stopping, matching the pre-refactor behavior.
					if (const UCrowdyEntityComponent* Component = Cast<UCrowdyEntityComponent>(Node->ComponentTemplate))
					{
						return Component;
					}
					break;
				}
			}
		}
		else
		{
			// The first native ancestor owns any native component; stop the chain walk here whether or not
			// one was found, matching HasEntityComponent's native-branch early return.
			const AActor* CDO = Cast<AActor>(Current->GetDefaultObject(false));
			return CDO ? CDO->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
		}
	}

	return nullptr;
}

UScriptStruct* UCrowdyAutoRegistry::ResolveExecutorStateStruct(const UClass* ActorClass) const
{
	// Only a Dynamic-mode component actually runs the continuous channel that writes the executor state
	// struct, so a Static component (even one carrying a leftover StateExecutor) resolves to null and never
	// suppresses an otherwise-safe CrowdyState property.
	const UCrowdyEntityComponent* Component = ResolveDefaultEntityComponent(ActorClass);
	if (!Component || Component->GetMode() != ECrowdyEntityMode::Dynamic)
	{
		return nullptr;
	}

	const UActorUpdateExecutor* Executor = Component->StateExecutor;
	return Executor ? Executor->GetStateStruct() : nullptr;
}

void UCrowdyAutoRegistry::ApplyExecutorOverlapFilter(FCrowdyRepLayout& Layout) const
{
	const UClass* OwnerClass = Layout.OwnerClass.Get();
	if (!OwnerClass)
	{
		return;
	}

	UScriptStruct* StateStruct = ResolveExecutorStateStruct(OwnerClass);
	if (!StateStruct)
	{
		return;
	}

	bool bRemovedAny = false;
	for (int32 Index = Layout.Properties.Num() - 1; Index >= 0; --Index)
	{
		const FProperty* StateProp = Layout.Properties[Index].Property;
		if (!StateProp)
		{
			continue;
		}

		const FName StateName = StateProp->GetFName();
		const FString StateCanonical = FCrowdyRPC::CanonicalParamType(StateProp);

		bool bOverlaps = false;
		for (TFieldIterator<FProperty> It(StateStruct); It; ++It)
		{
			const FProperty* Field = *It;
			// Name AND canonical-type equality so a same-name-different-type field is not a false overlap.
			if (Field->GetFName() == StateName
				&& FCrowdyRPC::CanonicalParamType(Field) == StateCanonical)
			{
				bOverlaps = true;
				break;
			}
		}

		if (bOverlaps)
		{
			UE_LOG(LogCrowdyReplication, Error,
				TEXT("CrowdyState: property '%s' on '%s' also lives in its executor state struct '%s'; it would be written by both the continuous channel and CrowdyState. Dropping it from the CrowdyState layout."),
				*StateProp->GetName(), *OwnerClass->GetPathName(), *StateStruct->GetName());
			Layout.Properties.RemoveAt(Index);
			bRemovedAny = true;
		}
	}

	if (bRemovedAny)
	{
		// Removal shifted positional indices, so recompute the hash: a keep-P build must hash-differ from a
		// drop-P build so a drifted peer drops cleanly instead of misparsing by position.
		Layout.LayoutHash = FCrowdyStateLayoutBuilder::ComputeLayoutHash(Layout.Properties);
	}
}

void UCrowdyAutoRegistry::ValidateOnRepSignatures(FCrowdyRepLayout& Layout) const
{
	const UClass* OwnerClass = Layout.OwnerClass.Get();
	if (!OwnerClass)
	{
		return;
	}

	for (FCrowdyRepProperty& RepProp : Layout.Properties)
	{
		if (RepProp.OnRepFunctionName == NAME_None)
		{
			continue;
		}

		const FString PropName = RepProp.Property ? RepProp.Property->GetName() : TEXT("<unresolved>");

		UFunction* OnRepFn = OwnerClass->FindFunctionByName(RepProp.OnRepFunctionName);
		if (!OnRepFn)
		{
			UE_LOG(LogCrowdyReplication, Error,
				TEXT("CrowdyState: property '%s' on '%s' names CrowdyOnRep '%s', which is not a valid CrowdyOnRep notify (no such function). Dropping the notify; the property still replicates."),
				*PropName, *OwnerClass->GetPathName(), *RepProp.OnRepFunctionName.ToString());
			RepProp.OnRepFunctionName = NAME_None;
			continue;
		}

		if (OnRepFn->NumParms != 0)
		{
			UE_LOG(LogCrowdyReplication, Error,
				TEXT("CrowdyState: property '%s' on '%s' names CrowdyOnRep '%s', which is not a valid CrowdyOnRep notify (must be parameterless; it takes %d parameter(s) or returns a value). Dropping the notify; the property still replicates."),
				*PropName, *OwnerClass->GetPathName(), *RepProp.OnRepFunctionName.ToString(), OnRepFn->NumParms);
			RepProp.OnRepFunctionName = NAME_None;
		}
	}
}

FCrowdyClassID UCrowdyAutoRegistry::ResolveClassID(const UClass* Class) const
{
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	const FSoftObjectPath ClassPath(Class);

	for (const FCrowdyClassIDOverride& Override : Settings->ClassIDOverrides)
	{
		if (Override.Class.ToSoftObjectPath() == ClassPath)
		{
			UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log,
				TEXT("[CrowdyAutoRegistry] Using override ClassID=%u for '%s'"),
				Override.OverrideID, *Class->GetName());
			return Override.OverrideID;
		}
	}

	return FCrowdyTypeIDGenerator::GenerateFromClass(Class);
}
