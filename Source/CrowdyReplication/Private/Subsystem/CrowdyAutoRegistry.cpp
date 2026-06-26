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
	// before it runs. Nothing past this point registers classes — only structs,
	// whose registries stay open — so sealing here is safe.
	ScanAndRegisterEntityClasses();
	UCrowdyClassRegistry::Get()->Seal();

	RegisterLoadedPayloadTypes();

#if WITH_EDITOR
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddWeakLambda(
		this, [this](EReloadCompleteReason) { ScanAndRegisterRpcFunctions(); });
#endif
}

void UCrowdyAutoRegistry::RegisterLoadedPayloadTypes()
{
	// FCrowdyRpcCall is the fixed wire payload every RPC-style CrowdyEvent rides.
	// Registering it here gives it a stable EventType on both ends, so the dispatch
	// path can resolve the type when sending and the router can recognize it on receipt.
	UEventPayloadRegistry::Get()->RegisterStructAuto(FCrowdyRpcCall::StaticStruct());

	ScanAndRegisterExecutorStateStructs();
	ScanAndRegisterRpcFunctions();
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
		// world init) — never force CDO creation on a class that is still
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

	// The id is per-class, not per-function — resolve it once. It also stamps each entry's
	// OwnerClassID so an incremental rescan can later evict this class without touching its keys.
	const FCrowdyClassID ClassID = UCrowdyClassRegistry::Get()->GetID(Class);

	// ExcludeSuper visits each function once, on the class that declares it —
	// which is its owner class. That keeps the resolver key (declaring ClassID +
	// FunctionID) aligned with what the sender stamps and with the class the
	// FunctionID hash embeds, so inherited handlers resolve without an entry per
	// subclass (FindFunctionByName returns the same owner UFunction either way).
	for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Function = *FuncIt;
		if (!UCrowdyBakedRegistry::IsEventHandlerFunction(Function)) continue;

		// An unsupported signature is the earliest place a mistake surfaces — fail it
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
		// parameters can never fit, registering it would only let it drop at send time — reject
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
	// reinstance, so it is never dereferenced — only used as a hash key by RemoveCurrent.
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
	// native ancestor's CDO. Never force CDO creation — classes may still be
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
