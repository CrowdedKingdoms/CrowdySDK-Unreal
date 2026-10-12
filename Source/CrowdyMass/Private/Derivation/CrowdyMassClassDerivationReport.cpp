#include "Derivation/CrowdyMassClassDerivationReport.h"

#include "Animation/AnimSequenceTransformProviderData.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Derivation/CrowdyMassAnimSetDerivation.h"
#include "Derivation/CrowdyMassCollisionDerivation.h"
#include "Engine/Blueprint.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassFragments.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace CrowdyMassClassDerivationReportPrivate
{
	FName ChannelName(const ECollisionChannel Channel)
	{
		// The project's own name for the channel, not the enumerator: a game that renamed
		// GameTraceChannel3 to something meaningful gets the meaningful name back, which is the only
		// form anyone reading this recognises.
		const UCollisionProfile* Profile = UCollisionProfile::Get();
		const FName Named = Profile ? Profile->ReturnChannelNameFromContainerIndex(static_cast<int32>(Channel)) : NAME_None;
		return Named.IsNone() ? FName(*FString::Printf(TEXT("channel %d"), static_cast<int32>(Channel))) : Named;
	}

	const TCHAR* AnimSetSourceText(const ECrowdyMassAnimSetSource Source)
	{
		switch (Source)
		{
		case ECrowdyMassAnimSetSource::ConfigTrait:
			return TEXT("the animation trait on its Mass entity config");
		case ECrowdyMassAnimSetSource::ClassComponent:
			return TEXT("the Crowdy Mass Anim Set component on the class");
		case ECrowdyMassAnimSetSource::ClassComponentNotRendered:
			return TEXT("the Crowdy Mass Anim Set component on the class, but NOTHING RENDERS IT: the authored Mass entity config the entities are built from carries no animation trait naming it, so they render in their bind pose");
		case ECrowdyMassAnimSetSource::LoadFailed:
			return TEXT("a named set that did not load, so the reference is missing or renamed");
		case ECrowdyMassAnimSetSource::Undeclared:
			return TEXT("nothing, so its entities would render in their bind pose");
		case ECrowdyMassAnimSetSource::NoClass:
			return TEXT("nothing, because no class was given");
		default:
			return TEXT("nothing yet, because this world has never been asked about this class");
		}
	}

	FString BankText(const UCrowdyMassAnimSet* AnimSet, const USkinnedAsset* Mesh, FString& OutPath)
	{
		if (!AnimSet)
		{
			return FString();
		}

		if (AnimSet->Bank.IsNull())
		{
			return TEXT("none named on the set, so a derived class would render in its bind pose");
		}

		OutPath = AnimSet->Bank.ToString();

		const UAnimSequenceTransformProviderData* Bank = AnimSet->Bank.LoadSynchronous();
		if (!Bank)
		{
			return TEXT("named but did not load, so a derived class would render in its bind pose");
		}

		const USkinnedAsset* BankMesh = Bank->GetSkinnedAsset();
		if (BankMesh && Mesh && BankMesh != Mesh)
		{
			return FString::Printf(TEXT("%d sequence(s), but it animates '%s' and the class renders '%s', so the crowd would render in its bind pose"),
				Bank->GetSequences().Num(), *BankMesh->GetPathName(), *Mesh->GetPathName());
		}

		return FString::Printf(TEXT("%d sequence(s)"), Bank->GetSequences().Num());
	}

	// BoundClass is the class actually carrying the binding, which for an inherited one is not the class
	// being reported on. Named rather than described, because the whole difficulty of an inherited binding
	// is that the row an author has to edit is filed under a class they did not ask about.
	FString ConfigSourceText(const ECrowdyMassEntityConfigSource Source, const UClass* BoundClass)
	{
		switch (Source)
		{
		case ECrowdyMassEntityConfigSource::PerClassBinding:
			return TEXT("a per-class binding on the rendering backend");
		case ECrowdyMassEntityConfigSource::InheritedClassBinding:
			return FString::Printf(
				TEXT("a per-class binding on the rendering backend for the ancestor class '%s', inherited because nothing binds this class itself"),
				*GetNameSafe(BoundClass));
		case ECrowdyMassEntityConfigSource::BackendOverride:
			return TEXT("the rendering backend's Remote Entity Config override");
		case ECrowdyMassEntityConfigSource::ProjectSetting:
			return TEXT("the project-wide Remote Entity Config");
		default:
			return TEXT("nothing");
		}
	}

	// Every loaded class with exactly this reflected name. Plural on purpose: two modules can both
	// declare a Foo, and a report that silently picked one of them would answer a different question
	// from the one asked without ever saying so.
	void FindLoadedClasses(const FString& ClassName, TArray<UClass*>& OutMatches)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (Candidate->GetName() == ClassName)
			{
				OutMatches.Add(Candidate);
			}
		}
	}

	UClass* LoadClassFromObjectPath(const FString& ObjectPath)
	{
		// Quiet on purpose: several candidate paths are tried in turn and only the last miss is worth
		// reporting, so the ones in between must not each write a warning of their own.
		return LoadObject<UClass>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	// The generated class inside a Blueprint asset, which is the class a person means when they name the
	// asset. A package path carries no object name at all, so the asset's own leaf name is used.
	void AppendPathCandidates(const FString& Path, TArray<FString>& OutCandidates)
	{
		if (Path.Contains(TEXT(".")))
		{
			OutCandidates.Add(Path);
			if (!Path.EndsWith(TEXT("_C")))
			{
				OutCandidates.Add(Path + TEXT("_C"));
			}

			return;
		}

		FString Leaf = Path;
		Path.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		OutCandidates.Add(FString::Printf(TEXT("%s.%s_C"), *Path, *Leaf));
		OutCandidates.Add(FString::Printf(TEXT("%s.%s"), *Path, *Leaf));
	}

	// A Blueprint nobody has opened this session is not loaded, so no amount of looking through loaded
	// classes will find it. The registry knows about it without it being loaded, which is what makes this
	// answerable before anything has spawned.
	UClass* LoadBlueprintClassFromAssetRegistry(const FString& AssetName, bool& bOutRegistryStillScanning)
	{
		bOutRegistryStillScanning = false;

		IAssetRegistry* Registry = IAssetRegistry::Get();
		if (!Registry)
		{
			return nullptr;
		}

		bOutRegistryStillScanning = Registry->IsLoadingAssets();

		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		Registry->GetAssets(Filter, Assets);

		const FName Wanted(*AssetName);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName != Wanted)
			{
				continue;
			}

			if (UClass* Loaded = LoadClassFromObjectPath(Asset.GetObjectPathString() + TEXT("_C")))
			{
				return Loaded;
			}
		}

		return nullptr;
	}
}

FCrowdyMassClassDerivationReport FCrowdyMassClassDerivationReport::ForClass(const UClass* ActorClass, const UWorld* World)
{
	using namespace CrowdyMassClassDerivationReportPrivate;

	FCrowdyMassClassDerivationReport Report;

	if (!ActorClass)
	{
		Report.Reason = TEXT("No class was given, so there is nothing to report on.");
		return Report;
	}

	Report.ClassPath = ActorClass->GetPathName();

	const FCrowdyMassDerivedMesh Mesh = FCrowdyMassMeshDerivation::FromClass(ActorClass);
	Report.bMeshResolved = Mesh.IsValid();
	Report.MeshRoute = Mesh.Route;
	Report.MeshComponentName = Mesh.ComponentName;
	Report.MeshRouteOwnerName = Mesh.RouteOwnerName;
	Report.MeshTransform = Mesh.Transform;
	Report.MeshAssetPath = Mesh.IsValid() ? Mesh.Asset->GetPathName() : FString();
	Report.Reason = Mesh.Reason;

	// Read here rather than left to the caller, so the same command that answers "would this class appear"
	// also answers "and would it look like anything". Nothing is marked as reported from a report: this is
	// the surface a person comes back to, and it has to answer in full however often it is asked.
	Report.Materials = FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(Mesh.Asset);

	// Only the object type is taken from this. The shape a derived entity is hit with comes from the
	// physics asset on the mesh above, through the machinery that was there before any of this, so a
	// class that describes no collision shape is still hittable and only loses the answer to "what sort
	// of thing is this".
	const FCrowdyMassDerivedTraceCollision Collision = FCrowdyMassCollisionDerivation::FromActorClass(ActorClass);
	Report.bObjectTypeResolved = Collision.IsValid();
	if (Collision.IsValid())
	{
		Report.ObjectTypeName = ChannelName(Collision.Settings.ObjectType.GetValue());
		Report.ObjectTypeSourceDescription = Collision.Source == ECrowdyMassCollisionDerivationSource::CapsuleComponent
			? TEXT("a capsule component")
			: TEXT("the physics asset on a skinned mesh");
	}
	else
	{
		// The same default the trait itself carries, read off the same struct, so this cannot claim an
		// object type a derived entity would not actually get.
		Report.ObjectTypeName = ChannelName(FCrowdyMassTraceCollisionSettings().ObjectType.GetValue());
		Report.ObjectTypeSourceDescription = TEXT("the trait default, because nothing on the class describes a collision shape");
	}

	const UCrowdyMassEntitySubsystem* Mass = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	if (!Mass)
	{
		Report.ConfigSourceDescription = TEXT("unknown, because there is no Crowdy Mass entity subsystem here to ask, so everything above is what WOULD be derived and not necessarily what a spawn would use");
		return Report;
	}

	ECrowdyMassEntityConfigSource ConfigSource = ECrowdyMassEntityConfigSource::None;
	const UMassEntityConfigAsset* AuthoredConfig = Mass->FindAuthoredEntityConfig(ActorClass, ConfigSource);

	Report.bAuthoredConfigWins = AuthoredConfig != nullptr;
	// The near tier is deliberately excluded from "nothing would be derived". A class's near tier shell
	// declaration is read on BOTH config paths: when an authored config answers, the declaration can still
	// be patched into a per-class copy of it, so an authored config no longer means the class supplies
	// nothing. Saying otherwise here is the confident wrong answer this report exists to avoid.
	// Asked only for the inherited case, where the answering row is filed under a class the report was not
	// asked about. Every other source either names its own class or is not a per-class row at all.
	const UClass* BoundClass = ConfigSource == ECrowdyMassEntityConfigSource::InheritedClassBinding
		? Mass->FindEntityConfigBindingClass(ActorClass)
		: nullptr;

	Report.ConfigSourceDescription = Report.bAuthoredConfigWins
		? FString::Printf(TEXT("%s ('%s'), so nothing above would be derived except the near tier shell, which is read off the class on either path"), *ConfigSourceText(ConfigSource, BoundClass), *AuthoredConfig->GetName())
		: FString(TEXT("nothing, so a spawn would derive"));

	// Asked through the subsystem's own resolve rather than a copy of it, for the same reason the config
	// question is: a second implementation of the precedence would answer confidently and wrongly the
	// first time the precedence changed.
	ECrowdyMassAnimSetSource AnimSetSource = ECrowdyMassAnimSetSource::NoClass;
	const UCrowdyMassAnimSet* AnimSet = Mass->FindRemoteEntityAnimSet(ActorClass, AnimSetSource);
	Report.AnimSetPath = AnimSet ? AnimSet->GetPathName() : FString();
	Report.AnimSetSourceDescription = AnimSetSourceText(AnimSetSource);

	// A derived config takes its Bank from the class component's set, which an authored config's trait may not name.
	const UCrowdyMassAnimSet* DerivedAnimSet = FCrowdyMassAnimSetDerivation::FromClass(ActorClass).AnimSet.LoadSynchronous();
	Report.BankDescription = BankText(DerivedAnimSet, Mesh.Asset, Report.BankPath);

	return Report;
}

FString FCrowdyMassClassDerivationReport::ToLine() const
{
	if (ClassPath.IsEmpty())
	{
		return Reason;
	}

	// Reported even for a class with no mesh, because the two answers are independent: an author fixing the
	// mesh needs to know whether the animation is also missing rather than finding out one gap at a time.
	FString Animation = AnimSetSourceDescription.IsEmpty()
		? FString()
		: (AnimSetPath.IsEmpty()
			? FString::Printf(TEXT(" Animation: %s."), *AnimSetSourceDescription)
			: FString::Printf(TEXT(" Animation: '%s', from %s."), *AnimSetPath, *AnimSetSourceDescription));

	if (!BankDescription.IsEmpty())
	{
		Animation += BankPath.IsEmpty()
			? FString::Printf(TEXT(" Bank (used if derived): %s."), *BankDescription)
			: FString::Printf(TEXT(" Bank (used if derived): '%s', %s."), *BankPath, *BankDescription);
	}

	if (!bMeshResolved)
	{
		return FString::Printf(TEXT("%s: NO mesh, so nothing would be derived for it. Authored config: %s.%s %s"),
			*ClassPath, *ConfigSourceDescription, *Animation, *Reason);
	}

	const FString Where = MeshComponentName.IsEmpty()
		? FString()
		: FString::Printf(TEXT(", component '%s' on '%s', placed at %s turned %s"), *MeshComponentName, *MeshRouteOwnerName,
			*MeshTransform.GetLocation().ToCompactString(), *MeshTransform.Rotator().ToCompactString());

	// Said whether or not anything is wrong with them. A clear verdict is the answer to "the crowd is grey
	// and I have already checked the materials", which is worth as much as the complaint itself.
	const FString MaterialVerdict = FString::Printf(TEXT(" Materials: %s."), *Materials.Describe());

	return FString::Printf(TEXT("%s: mesh '%s' from %s%s. Object type %s, from %s. Authored config: %s.%s%s"),
		*ClassPath,
		*MeshAssetPath,
		LexToString(MeshRoute),
		*Where,
		*ObjectTypeName.ToString(),
		*ObjectTypeSourceDescription,
		*ConfigSourceDescription,
		*Animation,
		*MaterialVerdict);
}

UClass* FCrowdyMassClassDerivationReport::ResolveActorClass(const FString& ClassNameOrPath, FString& OutWhy)
{
	using namespace CrowdyMassClassDerivationReportPrivate;

	OutWhy.Reset();

	const FString Wanted = ClassNameOrPath.TrimStartAndEnd();
	if (Wanted.IsEmpty())
	{
		OutWhy = TEXT("No class was given.");
		return nullptr;
	}

	// A path names exactly one class, so it is taken at its word and never searched for.
	if (Wanted.Contains(TEXT("/")))
	{
		TArray<FString> Candidates;
		AppendPathCandidates(Wanted, Candidates);

		for (const FString& Candidate : Candidates)
		{
			if (UClass* Loaded = LoadClassFromObjectPath(Candidate))
			{
				return Loaded;
			}
		}

		OutWhy = FString::Printf(TEXT("'%s' does not name a class that could be loaded. Tried %s."),
			*Wanted, *FString::Join(Candidates, TEXT(", ")));
		return nullptr;
	}

	// A short name, in the order a person means them: what they typed, then the generated class behind a
	// Blueprint of that name, then the same two again without a type prefix, because a native class is
	// reflected without one and ACharacter is registered as Character.
	TArray<FString> Names;
	Names.Add(Wanted);
	if (!Wanted.EndsWith(TEXT("_C")))
	{
		Names.Add(Wanted + TEXT("_C"));
	}

	const TCHAR First = Wanted[0];
	if (Wanted.Len() > 1 && (First == TEXT('A') || First == TEXT('U')))
	{
		Names.Add(Wanted.RightChop(1));
	}

	for (const FString& Name : Names)
	{
		TArray<UClass*> Matches;
		FindLoadedClasses(Name, Matches);

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}

		if (Matches.Num() > 1)
		{
			TArray<FString> Paths;
			for (const UClass* Match : Matches)
			{
				Paths.Add(Match->GetPathName());
			}

			OutWhy = FString::Printf(TEXT("'%s' names %d loaded classes, so it is ambiguous. Pass one of these paths instead: %s."),
				*Name, Matches.Num(), *FString::Join(Paths, TEXT(", ")));
			return nullptr;
		}
	}

	// Nothing loaded answers, which is the ordinary state for a Blueprint nothing has spawned yet rather
	// than evidence it does not exist.
	const FString AssetName = Wanted.EndsWith(TEXT("_C")) ? Wanted.LeftChop(2) : Wanted;

	bool bRegistryStillScanning = false;
	if (UClass* FromRegistry = LoadBlueprintClassFromAssetRegistry(AssetName, bRegistryStillScanning))
	{
		return FromRegistry;
	}

	// A miss while the registry is still filling is not an answer, and saying so is the difference
	// between "there is no such class" and "nobody has looked at all of them yet".
	OutWhy = bRegistryStillScanning
		? FString::Printf(TEXT("Nothing loaded is called '%s', and the asset registry is still scanning, so this is not yet an answer. Ask again in a moment, or pass the full asset path."), *Wanted)
		: FString::Printf(TEXT("Nothing loaded is called '%s' and no Blueprint asset of that name was found. Check the spelling, or pass the full asset path."), *Wanted);

	return nullptr;
}
