#include "Nodes/CrowdyMassEventCatalog.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Backend/CrowdyMassRenderingBackendConfig.h"
#include "CrowdyMassNodesLog.h"
#include "Data/CrowdyActorManagementConfig.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Mass/CrowdyMassEntity.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#define LOCTEXT_NAMESPACE "CrowdyMassEventCatalog"

namespace CrowdyMassEventCatalogPrivate
{
	// The universal marker any Crowdy function carries. A C++ receiver declares it in its UFUNCTION,
	// and the Blueprint compiler stamps it onto a custom event marked "Crowdy Replicates".
	const TCHAR* CrowdyEventMeta = TEXT("CrowdyEvent");

	// Present only on a Blueprint event marked "Crowdy Replicates", which is what separates an
	// RPC-style event from a struct handler that merely carries the CrowdyEvent marker.
	const TCHAR* CrowdyReplicatesMeta = TEXT("CrowdyReplicates");

	// A C++ CROWDY_EVENT call site sends to <Event>_Implementation, so the receiver always ends this
	// way and the suffix is what identifies a C++ event among the functions carrying the marker.
	const TCHAR* ImplementationSuffix = TEXT("_Implementation");

	/**
	 * Whether the AUTHOR marked this event as replicated, read from the event node rather than from the
	 * compiled function.
	 *
	 * The markers below are stamped onto a Blueprint's UFunction in ProcessBlueprintCompiled, which runs
	 * after node validation. So while the Blueprint that DECLARES an event is being compiled, that event's
	 * function does not carry them yet, and the skeleton copy never carries them at all. The author's own
	 * marker lives on the event node, is saved with the asset, and is true from the moment they set it.
	 */
	bool HasAuthoredReplicatesMarker(const UFunction* Function)
	{
		if (!Function)
		{
			return false;
		}

		UClass* Owner = Function->GetOwnerClass();
		UClass* Authoritative = Owner ? Owner->GetAuthoritativeClass() : nullptr;
		UBlueprint* Blueprint = Authoritative ? Cast<UBlueprint>(Authoritative->ClassGeneratedBy) : nullptr;
		if (!Blueprint)
		{
			return false;
		}

		TArray<UK2Node_CustomEvent*> EventNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, EventNodes);
		for (const UK2Node_CustomEvent* EventNode : EventNodes)
		{
			if (!EventNode || EventNode->CustomFunctionName != Function->GetFName())
			{
				continue;
			}

			const FKismetUserDeclaredFunctionMetadata& Meta =
				const_cast<UK2Node_CustomEvent*>(EventNode)->GetUserDefinedMetaData();
			return Meta.HasMetaData(FName(CrowdyRpcMetaKeys::Replicates))
				|| Meta.HasMetaData(FName(CrowdyRpcMetaKeys::LegacyReplicate));
		}

		return false;
	}

	bool IsTransientClassName(const FString& Name)
	{
		return Name.StartsWith(TEXT("SKEL_"))
			|| Name.StartsWith(TEXT("REINST_"))
			|| Name.StartsWith(TEXT("TRASHCLASS_"))
			|| Name.StartsWith(TEXT("PLACEHOLDER-"));
	}

	// The engine's own answer to "should a picker offer this class", which every other class picker already
	// honours. Used here rather than a Crowdy-specific marker so a class hidden from one picker is hidden
	// from all of them, and so an SDK test fixture and a game's own dev-only entity opt out the same way.
	bool IsHiddenFromPickers(const UClass* Class)
	{
		if (Class->HasAnyClassFlags(CLASS_Hidden | CLASS_HideDropDown))
		{
			return true;
		}

#if WITH_METADATA
		// A Blueprint sets this rather than a class flag, and the catalog only ever runs in the editor.
		return Class->GetBoolMetaData(TEXT("HideDropdown"));
#else
		return false;
#endif
	}

	bool IsUsableEntityClass(const UClass* Class)
	{
		return CrowdyMassEventCatalog::IsSelectableEntityClass(Class);
	}

	FString MakeClassDisplayName(const UClass* Class)
	{
		// The SDK's own group is named for what it means to an author rather than for the C++ class, which
		// is not a class anybody authors against: its events go to any crowd entity whatever its class.
		if (Class == UCrowdyMassEntity::StaticClass())
		{
			return TEXT("Any Crowd Entity");
		}

		FString Name = Class ? Class->GetName() : FString();
		Name.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
		return Name;
	}

	// The Mass backend config embedded in a profile, or null when that profile uses another backend.
	// The config is an instanced sub-object of the profile asset rather than an asset of its own, so
	// it is only reachable by loading the profile that owns it.
	const UCrowdyMassRenderingBackendConfig* AsMassBackendConfig(const UCrowdyRenderingBackendConfig* Config)
	{
		return Cast<UCrowdyMassRenderingBackendConfig>(Config);
	}

	void AppendBoundClasses(const UCrowdyMassRenderingBackendConfig* Config, TArray<UClass*>& OutClasses)
	{
		if (!Config)
		{
			return;
		}

		for (const FCrowdyMassEntityConfigBinding& Binding : Config->PerClassEntityConfigs)
		{
			if (Binding.EntityClass.IsNull())
			{
				continue;
			}

			// A bound class is normally a Blueprint nothing has loaded yet, which is exactly the case
			// this dropdown has to answer for, so the class is loaded here rather than skipped.
			UClass* Loaded = Binding.EntityClass.LoadSynchronous();
			if (IsUsableEntityClass(Loaded))
			{
				OutClasses.AddUnique(Loaded);
			}
		}
	}

	void AppendClassesFromProfileObject(const UObject* ProfileObject, TArray<UClass*>& OutClasses)
	{
		if (const UCrowdyMapProfile* Profile = Cast<UCrowdyMapProfile>(ProfileObject))
		{
			AppendBoundClasses(AsMassBackendConfig(Profile->ActorManagement.BackendConfig), OutClasses);
			return;
		}

		if (const UCrowdyActorManagementConfig* Config = Cast<UCrowdyActorManagementConfig>(ProfileObject))
		{
			AppendBoundClasses(AsMassBackendConfig(Config->Config.BackendConfig), OutClasses);
		}
	}

	// The profiles the project settings name, which is the set the game actually runs with, and then
	// every profile asset the registry knows about, so a profile authored but not yet assigned still
	// contributes its classes.
	void AppendDataAssetBoundClasses(TArray<UClass*>& OutClasses, bool& bOutRegistryStillScanning)
	{
		if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
		{
			AppendClassesFromProfileObject(Settings->DefaultProfile.LoadSynchronous(), OutClasses);

			for (const TPair<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>>& Pair : Settings->MapProfiles)
			{
				AppendClassesFromProfileObject(Pair.Value.LoadSynchronous(), OutClasses);
			}
		}

		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (!Registry)
		{
			return;
		}

		// Reported rather than waited on: blocking the editor on a full scan to open a dropdown is
		// worse than saying the list may still be incomplete.
		bOutRegistryStillScanning = Registry->IsLoadingAssets();

		FARFilter Filter;
		Filter.bRecursiveClasses = true;
		Filter.ClassPaths.Add(UCrowdyMapProfile::StaticClass()->GetClassPathName());
		Filter.ClassPaths.Add(UCrowdyActorManagementConfig::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		Registry->GetAssets(Filter, Assets);

		for (const FAssetData& Asset : Assets)
		{
			AppendClassesFromProfileObject(Asset.GetAsset(), OutClasses);
		}
	}

	// A class nothing binds still renders as a Mass entity when it derives its configuration, so any
	// loaded class carrying a Crowdy entity component is offered too. Only loaded classes can be
	// tested this way: whether a class carries a component is not something the asset registry knows.
	void AppendEntityComponentClasses(TArray<UClass*>& OutClasses)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!IsUsableEntityClass(Class))
			{
				continue;
			}

			// The class metadata is the editor's own record that a class carries the component, and
			// the component walk answers for a C++ class that adds one without declaring the marker.
			const bool bCarriesComponent = Class->HasMetaData(TEXT("CrowdyEntity"))
				|| UCrowdyAutoRegistry::ResolveDefaultEntityComponent(Class) != nullptr;

			if (bCarriesComponent)
			{
				OutClasses.AddUnique(Class);
			}
		}
	}

	// The events the SDK declares itself, which can be sent to any Mass-rendered entity whatever class it
	// is: they are declared on the stand-in base every entity is given rather than on a game's own class.
	//
	// Added directly rather than through IsUsableEntityClass, which asks whether a class is an ENTITY class
	// worth listing and correctly requires an Actor. Relaxing that filter would re-open the whole
	// non-actor authoring surface; this adds exactly one group holding exactly the SDK's own events.
	//
	// Sending only. These are the events the crowd plane applies itself, so there is nothing for a graph to
	// receive; see CrowdyMassEventCatalog::IsAppliedByTheCrowdPlane.
	void AppendSdkDeclaredClass(TArray<UClass*>& OutClasses)
	{
		OutClasses.AddUnique(UCrowdyMassEntity::StaticClass());
	}

	FText DescribeParameters(const UFunction* Function)
	{
		TArray<FString> Parts;
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm) || FCrowdyRPC::IsTrueOutputParam(Property))
			{
				continue;
			}
			Parts.Add(FString::Printf(TEXT("%s %s"), *Property->GetCPPType(), *Property->GetName()));
		}

		if (Parts.Num() == 0)
		{
			return LOCTEXT("NoParameters", "Takes no parameters.");
		}
		return FText::FromString(FString::Join(Parts, TEXT(", ")));
	}
}

bool CrowdyMassEventCatalog::IsSelectableEntityClass(const UClass* Class)
{
	using namespace CrowdyMassEventCatalogPrivate;

	return Class
		&& Class->IsChildOf(AActor::StaticClass())
		&& !Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)
		&& !IsHiddenFromPickers(Class)
		&& !IsTransientClassName(Class->GetName());
}

bool CrowdyMassEventCatalog::IsCallableCrowdyEvent(const UFunction* Function)
{
	return DescribeCallProblem(Function).IsEmpty();
}

FString CrowdyMassEventCatalog::DescribeCallProblem(const UFunction* Function)
{
	using namespace CrowdyMassEventCatalogPrivate;

	if (!Function)
	{
		return TEXT("no such function exists");
	}

	// Checked before the stamped metadata, because a Blueprint being compiled has not been stamped yet.
	const bool bAuthoredAsReplicated = HasAuthoredReplicatesMarker(Function);

	if (!bAuthoredAsReplicated && !Function->HasMetaData(CrowdyEventMeta))
	{
		return TEXT("is not a Crowdy event");
	}

	const bool bIsBlueprintReplicatedEvent = bAuthoredAsReplicated || Function->HasMetaData(CrowdyReplicatesMeta);
	const bool bIsNativeReceiver = Function->GetName().EndsWith(ImplementationSuffix, ESearchCase::CaseSensitive);
	if (!bIsBlueprintReplicatedEvent && !bIsNativeReceiver)
	{
		return TEXT("is a Crowdy event handler, which is dispatched with a payload rather than called on an entity");
	}

	const FString SignatureProblem = FCrowdyRPC::DescribeSignatureProblem(Function);
	if (!SignatureProblem.IsEmpty())
	{
		return SignatureProblem;
	}

	return FString();
}

bool CrowdyMassEventCatalog::IsAppliedByTheCrowdPlane(const UFunction* Function)
{
	const UClass* DeclaringClass = Function ? Function->GetOwnerClass() : nullptr;
	return DeclaringClass && DeclaringClass->IsChildOf(UCrowdyMassEntity::StaticClass());
}

FString CrowdyMassEventCatalog::MakeDisplayName(FName FunctionName)
{
	FString Name = FunctionName.ToString();
	Name.RemoveFromEnd(CrowdyMassEventCatalogPrivate::ImplementationSuffix, ESearchCase::CaseSensitive);
	return Name;
}

void CrowdyMassEventCatalog::Build(FCrowdyMassEventCatalog& OutCatalog, const ECrowdyMassEventCatalogPurpose Purpose)
{
	using namespace CrowdyMassEventCatalogPrivate;

	OutCatalog.Groups.Reset();
	OutCatalog.bAssetRegistryStillScanning = false;

	TArray<UClass*> EntityClasses;
	AppendDataAssetBoundClasses(EntityClasses, OutCatalog.bAssetRegistryStillScanning);
	AppendEntityComponentClasses(EntityClasses);

	if (Purpose == ECrowdyMassEventCatalogPurpose::Send)
	{
		AppendSdkDeclaredClass(EntityClasses);
	}

	OutCatalog.NumEntityClassesConsidered = EntityClasses.Num();

	for (UClass* EntityClass : EntityClasses)
	{
		FCrowdyMassEventClassGroup Group;
		Group.EntityClass = EntityClass;
		Group.DisplayName = MakeClassDisplayName(EntityClass);

		// Supers included: an event a base class declares can be sent to this entity too, and it is
		// still addressed by the class that declares it, which is what DeclaringClass records. The walk
		// starts at the entity class, so where a subclass redeclares a name the derived one is kept and
		// the shadowed base one is dropped rather than listed twice under one class.
		TSet<FName> SeenNames;
		for (TFieldIterator<UFunction> It(EntityClass); It; ++It)
		{
			UFunction* Function = *It;
			if (!IsCallableCrowdyEvent(Function))
			{
				continue;
			}

			bool bAlreadySeen = false;
			SeenNames.Add(Function->GetFName(), &bAlreadySeen);
			if (bAlreadySeen)
			{
				continue;
			}

			UClass* DeclaringClass = Function->GetOwnerClass();
			DeclaringClass = DeclaringClass ? DeclaringClass->GetAuthoritativeClass() : nullptr;
			if (!DeclaringClass)
			{
				continue;
			}

			FCrowdyMassEventEntry Entry;
			Entry.DeclaringClass = DeclaringClass;
			Entry.FunctionName = Function->GetFName();
			Entry.DisplayName = MakeDisplayName(Function->GetFName());
			Entry.Tooltip = DescribeParameters(Function);
			Group.Events.Add(MoveTemp(Entry));
		}

		if (Group.Events.Num() == 0)
		{
			continue;
		}

		Group.Events.Sort([](const FCrowdyMassEventEntry& A, const FCrowdyMassEventEntry& B)
		{
			return A.DisplayName < B.DisplayName;
		});

		OutCatalog.Groups.Add(MoveTemp(Group));
	}

	OutCatalog.Groups.Sort([](const FCrowdyMassEventClassGroup& A, const FCrowdyMassEventClassGroup& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	UE_CLOG(CrowdyMassNodesTrace::IsEnabled(), LogCrowdyMassNodes, Log,
		TEXT("[CrowdyMassEventCatalog] %s: %d entity class(es) considered, %d offering events%s."),
		Purpose == ECrowdyMassEventCatalogPurpose::Send ? TEXT("send") : TEXT("receive"),
		OutCatalog.NumEntityClassesConsidered, OutCatalog.Groups.Num(),
		OutCatalog.bAssetRegistryStillScanning ? TEXT(" (asset registry still scanning)") : TEXT(""));
}

#undef LOCTEXT_NAMESPACE
