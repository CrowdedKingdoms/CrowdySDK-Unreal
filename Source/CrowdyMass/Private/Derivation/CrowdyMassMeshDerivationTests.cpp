#include "Derivation/CrowdyMassMeshDerivation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Every case here asks the same question, "what would this class render with", of a class sitting on one
// of the routes a mesh can reach an actor by.
//
// Real engine classes are used wherever one exists that has the shape being tested, because a class built
// here is only ever an imitation of a compiled one and the ways it differs are exactly the ways this code
// is sensitive: a class assembled at runtime has no default object until something builds one, and asking
// an unfinished class for a default object asserts rather than returning null. Only the construction
// script case has no real class to borrow, and it is the one case that builds its own.
static constexpr EAutomationTestFlags CrowdyMassMeshDerivationTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassMeshDerivationTestFixture
{
	// A mesh asset with nothing in it. Derivation only ever compares and returns the pointer, so an empty
	// asset is indistinguishable from an imported one here.
	static USkeletalMesh* MakeMesh()
	{
		return NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	// Stands in for a COMPILED Blueprint class, which is the only kind derivation answers for. Three
	// things make it one rather than a half-formed class: it carries the parent's constructor, without
	// which a default object cannot be built at all; it is linked against that parent, which is what
	// gives it the parent's size; and it then has that default object built. A class still waiting for
	// any of those is what a class mid-compilation looks like, and derivation refuses those on purpose
	// rather than reaching into them.
	//
	// The link is not optional and its absence is silent. A class assembled with NewObject has a
	// properties size of zero, and SetSuperStruct does not change that: only linking copies the parent's
	// size across. Building a default object before that allocates a zero-sized object and then runs the
	// parent's constructor over it, which writes an actor's worth of members past the end of the
	// allocation and corrupts the heap. Nothing reports it, and the crash lands in whatever allocates
	// next, which can be an unrelated test or the engine's own message pump.
	static UBlueprintGeneratedClass* MakeCompiledClass(UClass* ParentClass)
	{
		UBlueprintGeneratedClass* GeneratedClass =
			NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient);
		GeneratedClass->ClassWithin = ParentClass->ClassWithin;
		GeneratedClass->ClassConstructor = ParentClass->ClassConstructor;
		GeneratedClass->ClassCastFlags |= ParentClass->ClassCastFlags;
		GeneratedClass->SetSuperStruct(ParentClass);
		GeneratedClass->Bind();
		GeneratedClass->StaticLink(/*bRelinkExistingProperties=*/true);
		GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
		return GeneratedClass;
	}

	// The shape a mesh added in the Blueprint editor takes: a construction script node holding a component
	// template, with no matching component anywhere on a default object.
	static void AddMeshNode(UBlueprintGeneratedClass* GeneratedClass, USkeletalMesh* Mesh)
	{
		USimpleConstructionScript* Script =
			NewObject<USimpleConstructionScript>(GeneratedClass, NAME_None, RF_Transient);
		GeneratedClass->SimpleConstructionScript = Script;

		USkeletalMeshComponent* Template =
			NewObject<USkeletalMeshComponent>(GeneratedClass, NAME_None, RF_Transient);
		SetMesh(Template, Mesh);

		USCS_Node* Node = NewObject<USCS_Node>(Script, NAME_None, RF_Transient);
		Node->ComponentClass = USkeletalMeshComponent::StaticClass();
		Node->ComponentTemplate = Template;
		Script->AddNode(Node);
	}

	// Assigning the property directly would leave the component's own change tracking behind and trip an
	// ensure the next time anything read the mesh back, so the assignment goes through the setter that
	// keeps the two in step.
	static void SetMesh(USkinnedMeshComponent* Component, USkeletalMesh* Mesh)
	{
		Component->SetSkinnedAssetAndUpdate(Mesh);
	}

	// A surface material with the instanced skinned usage flag either ticked or not. The flag is set
	// through the accessor rather than the member behind it, which is the accessor a real material's own
	// details panel goes through, so a case cannot pass by writing a bit the engine reads somewhere else.
	static UMaterial* MakeMaterial(const bool bFlagged)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		Material->MaterialDomain = MD_Surface;
		Material->SetUsageByFlag(MATUSAGE_InstancedSkinnedMesh, bFlagged);
		return Material;
	}

	static USkeletalMesh* MakeMeshWithSlots(const TArray<FSkeletalMaterial>& Slots)
	{
		USkeletalMesh* Mesh = MakeMesh();
		Mesh->SetMaterials(Slots);
		return Mesh;
	}
};

// The character default object is shared by the whole process, so a case that puts a mesh on it has to put
// it back afterwards or every later case sees the change.
struct FCrowdyMassMeshDerivationTestCharacterMeshScope
{
	explicit FCrowdyMassMeshDerivationTestCharacterMeshScope(USkeletalMesh* Mesh)
	{
		ACharacter* DefaultCharacter = ACharacter::StaticClass()->GetDefaultObject<ACharacter>();
		Component = DefaultCharacter ? DefaultCharacter->GetMesh() : nullptr;
		if (!Component)
		{
			return;
		}

		Previous = Component->GetSkinnedAsset();
		FCrowdyMassMeshDerivationTestFixture::SetMesh(Component, Mesh);
	}

	~FCrowdyMassMeshDerivationTestCharacterMeshScope()
	{
		if (!Component)
		{
			return;
		}

		Component->SetSkinnedAssetAndUpdate(Previous);
	}

	USkinnedMeshComponent* Component = nullptr;
	USkinnedAsset* Previous = nullptr;
};

// The authoring case a Character class is built around: the mesh component is native and the mesh is on it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationCharacterTest,
	"CrowdySDK.Mass.MeshDerivationFindsACharacterClassMesh", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationCharacterTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());

	const FCrowdyMassMeshDerivationTestCharacterMeshScope Scope(Mesh.Get());
	if (!TestNotNull(TEXT("A Character carries a mesh component to put the mesh on"), Scope.Component))
	{
		return false;
	}

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());

	TestTrue(TEXT("A class whose native mesh component carries a mesh resolves"), Derived.IsValid());
	TestSamePtr(TEXT("The mesh is the one on the native component"), Derived.Asset, static_cast<USkinnedAsset*>(Mesh.Get()));
	TestTrue(TEXT("A resolved class has nothing to explain"), Derived.Reason.IsEmpty());

	return true;
}

// A Character's mesh hangs below the capsule, and the row has to be drawn where the actor draws it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationCharacterPlacementTest,
	"CrowdySDK.Mass.MeshDerivationKeepsACharacterMeshPlacement", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationCharacterPlacementTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());
	const FCrowdyMassMeshDerivationTestCharacterMeshScope Scope(Mesh.Get());
	if (!TestNotNull(TEXT("A Character carries a mesh component to put the mesh on"), Scope.Component))
	{
		return false;
	}

	const FTransform Previous = Scope.Component->GetRelativeTransform();
	ON_SCOPE_EXIT { Scope.Component->SetRelativeTransform(Previous); };
	const FTransform Placement(FRotator(0.0, -90.0, 0.0), FVector(0.0, 0.0, -90.0));
	Scope.Component->SetRelativeTransform(Placement);

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());

	TestTrue(TEXT("The derived placement is the mesh's placement under the capsule"), Derived.Transform.Equals(Placement));

	return true;
}

// A mesh that is the actor's root has no placement of its own: the engine folds a root's transform into the actor's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationRootMeshTest,
	"CrowdySDK.Mass.MeshDerivationGivesARootMeshNoPlacement", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationRootMeshTest::RunTest(const FString& Parameters)
{
	USkeletalMeshComponent* Component = ASkeletalMeshActor::StaticClass()->GetDefaultObject<ASkeletalMeshActor>()->GetSkeletalMeshComponent();
	if (!TestNotNull(TEXT("A skeletal mesh actor's root is its mesh"), Component))
	{
		return false;
	}

	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());
	USkinnedAsset* PreviousAsset = Component->GetSkinnedAsset();
	const FTransform PreviousTransform = Component->GetRelativeTransform();
	ON_SCOPE_EXIT
	{
		Component->SetSkinnedAssetAndUpdate(PreviousAsset);
		Component->SetRelativeTransform(PreviousTransform);
	};
	FCrowdyMassMeshDerivationTestFixture::SetMesh(Component, Mesh.Get());
	Component->SetRelativeTransform(FTransform(FRotator(0.0, -90.0, 0.0), FVector(0.0, 0.0, -90.0)));

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ASkeletalMeshActor::StaticClass());

	TestSamePtr(TEXT("The root mesh is the one found"), Derived.Asset, static_cast<USkinnedAsset*>(Mesh.Get()));
	TestTrue(TEXT("and it is given no placement"), Derived.Transform.Equals(FTransform::Identity));

	return true;
}

// The failure the whole reason string exists for: nothing anywhere on the class draws anything, and the
// caller has to be able to say which class and what to do about it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationBareActorTest,
	"CrowdySDK.Mass.MeshDerivationReportsAnActorWithNoMeshComponent", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationBareActorTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(AActor::StaticClass());

	TestFalse(TEXT("An Actor with no mesh component anywhere resolves to nothing"), Derived.IsValid());
	TestNull(TEXT("Nothing is returned rather than an arbitrary mesh"), Derived.Asset);
	TestTrue(TEXT("The reason names the class that could not be resolved"),
		Derived.Reason.Contains(AActor::StaticClass()->GetPathName()));
	TestTrue(TEXT("The reason says a mesh component is what is missing"),
		Derived.Reason.Contains(TEXT("no skinned mesh component")));

	return true;
}

// A component added in the Blueprint editor never reaches a default object, so this is the route a
// default-object read alone would miss, and it is the one "just add a mesh and it works" depends on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationConstructionScriptTest,
	"CrowdySDK.Mass.MeshDerivationFindsAMeshAddedInABlueprint", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationConstructionScriptTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());

	// Parented to Actor rather than Character so the construction script is the ONLY place a mesh could
	// come from. A Character parent would carry a native mesh component and the case would pass without
	// the construction script ever being read.
	const TStrongObjectPtr<UBlueprintGeneratedClass> ActorClass(
		FCrowdyMassMeshDerivationTestFixture::MakeCompiledClass(AActor::StaticClass()));

	FCrowdyMassMeshDerivationTestFixture::AddMeshNode(ActorClass.Get(), Mesh.Get());

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ActorClass.Get());

	TestTrue(TEXT("A mesh added in the Blueprint resolves"), Derived.IsValid());
	TestSamePtr(TEXT("The mesh is the one on the construction script template"), Derived.Asset, static_cast<USkinnedAsset*>(Mesh.Get()));
	TestTrue(TEXT("A resolved class has nothing to explain"), Derived.Reason.IsEmpty());

	return true;
}

// A mesh nested under another component in a Blueprint is placed by the whole chain up to the root, not by its own
// offset alone; the root's own transform is the actor's and adds nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationNestedPlacementTest,
	"CrowdySDK.Mass.MeshDerivationComposesANestedMeshPlacement", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationNestedPlacementTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());
	const TStrongObjectPtr<UBlueprintGeneratedClass> ActorClass(
		FCrowdyMassMeshDerivationTestFixture::MakeCompiledClass(AActor::StaticClass()));

	USimpleConstructionScript* Script = NewObject<USimpleConstructionScript>(ActorClass.Get(), NAME_None, RF_Transient);
	ActorClass->SimpleConstructionScript = Script;
	const auto AddNode = [&Script, &ActorClass](USceneComponent* Template, const FTransform& Placement, USCS_Node* Parent)
	{
		Template->SetRelativeTransform(Placement);
		USCS_Node* Node = NewObject<USCS_Node>(Script, NAME_None, RF_Transient);
		Node->ComponentClass = Template->GetClass();
		Node->ComponentTemplate = Template;
		if (Parent)
		{
			Parent->AddChildNode(Node);
		}
		else
		{
			Script->AddNode(Node);
		}
		return Node;
	};

	USCS_Node* Root = AddNode(NewObject<USceneComponent>(ActorClass.Get(), NAME_None, RF_Transient),
		FTransform(FVector(0.0, 0.0, 500.0)), nullptr);
	USCS_Node* Hip = AddNode(NewObject<USceneComponent>(ActorClass.Get(), NAME_None, RF_Transient),
		FTransform(FVector(0.0, 0.0, -50.0)), Root);
	USkeletalMeshComponent* MeshTemplate = NewObject<USkeletalMeshComponent>(ActorClass.Get(), NAME_None, RF_Transient);
	FCrowdyMassMeshDerivationTestFixture::SetMesh(MeshTemplate, Mesh.Get());
	AddNode(MeshTemplate, FTransform(FRotator(0.0, -90.0, 0.0), FVector(0.0, 0.0, -40.0)), Hip);

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ActorClass.Get());

	TestSamePtr(TEXT("The nested mesh is found"), Derived.Asset, static_cast<USkinnedAsset*>(Mesh.Get()));
	TestTrue(TEXT("and placed by both offsets under the root, the root's own left out"),
		Derived.Transform.Equals(FTransform(FRotator(0.0, -90.0, 0.0), FVector(0.0, 0.0, -90.0))));

	return true;
}

// A mesh that is the Blueprint's own root has no placement of its own, as for a native root.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationBlueprintRootMeshTest,
	"CrowdySDK.Mass.MeshDerivationGivesABlueprintRootMeshNoPlacement", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationBlueprintRootMeshTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());
	const TStrongObjectPtr<UBlueprintGeneratedClass> ActorClass(
		FCrowdyMassMeshDerivationTestFixture::MakeCompiledClass(AActor::StaticClass()));

	FCrowdyMassMeshDerivationTestFixture::AddMeshNode(ActorClass.Get(), Mesh.Get());
	USceneComponent* Template = Cast<USceneComponent>(ActorClass->SimpleConstructionScript->GetRootNodes()[0]->ComponentTemplate);
	Template->SetRelativeScale3D(FVector(1.2));

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ActorClass.Get());

	TestSamePtr(TEXT("The root mesh is found"), Derived.Asset, static_cast<USkinnedAsset*>(Mesh.Get()));
	TestTrue(TEXT("and given no placement, so its scale is not applied twice"), Derived.Transform.Equals(FTransform::Identity));

	return true;
}

// Callers are meant to work this out once per class and keep it, which is only sound if the answer does not
// depend on how many times or in what order it was asked.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationStabilityTest,
	"CrowdySDK.Mass.MeshDerivationAnswersTheSameClassTheSameWay", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationStabilityTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());

	const FCrowdyMassMeshDerivationTestCharacterMeshScope Scope(Mesh.Get());
	if (!TestNotNull(TEXT("A Character carries a mesh component to put the mesh on"), Scope.Component))
	{
		return false;
	}

	const FCrowdyMassDerivedMesh First = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());
	const FCrowdyMassDerivedMesh Second = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());

	TestSamePtr(TEXT("Asking twice returns the same mesh"), Second.Asset, First.Asset);
	TestSamePtr(TEXT("The mesh is still the one that was assigned"), Second.Asset, static_cast<USkinnedAsset*>(Mesh.Get()));

	// A second, differently answered class in between would catch an answer that leaked from one call to the
	// next rather than being read from the class each time.
	const FCrowdyMassDerivedMesh Bare = FCrowdyMassMeshDerivation::FromClass(AActor::StaticClass());
	const FCrowdyMassDerivedMesh Third = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());

	TestNull(TEXT("A class with no mesh is unaffected by the class asked about before it"), Bare.Asset);
	TestSamePtr(TEXT("The resolvable class still answers the same afterwards"), Third.Asset, First.Asset);
	TestEqual(TEXT("And explains itself the same way"), Third.Reason, First.Reason);

	return true;
}

// Splits the one failure into two, because an empty mesh slot and a missing component need different advice,
// and because it pins that the default object read reaches the native component at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationEmptyMeshSlotTest,
	"CrowdySDK.Mass.MeshDerivationReportsAMeshComponentWithNoMesh", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationEmptyMeshSlotTest::RunTest(const FString& Parameters)
{
	// No mesh scope here, so the Character's own mesh component is in the state it ships in: present, and
	// with nothing assigned to it.
	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());

	TestFalse(TEXT("An unassigned mesh slot resolves to nothing"), Derived.IsValid());
	TestTrue(TEXT("The reason says the component is there and the mesh is not"),
		Derived.Reason.Contains(TEXT("no mesh is assigned")));

	return true;
}

// A class that cannot yet answer for itself is refused rather than reached into. Building a default object
// for a class that has not finished forming runs its constructor at the wrong moment, and a class without
// one asserts rather than returning null, so the refusal is what keeps that from being reachable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationUnfinishedClassTest,
	"CrowdySDK.Mass.MeshDerivationRefusesAClassWithNoDefaultObject", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationUnfinishedClassTest::RunTest(const FString& Parameters)
{
	// Deliberately NOT given a constructor or a default object, which is what a class part way through
	// compilation looks like.
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnfinishedClass(
		NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient));
	UnfinishedClass->ClassWithin = AActor::StaticClass()->ClassWithin;
	UnfinishedClass->SetSuperStruct(AActor::StaticClass());

	const FCrowdyMassDerivedMesh Derived = FCrowdyMassMeshDerivation::FromClass(UnfinishedClass.Get());

	TestFalse(TEXT("An unfinished class resolves to nothing"), Derived.IsValid());
	TestTrue(TEXT("The reason says the class is not ready rather than that it has no mesh"),
		Derived.Reason.Contains(TEXT("no class default object")));

	return true;
}

// The three routes are indistinguishable from the outside once a mesh has been found, which is exactly
// when an author is asking why the mesh is not the one they set. Telling them apart is the only reason
// the route is recorded, so the two routes that can be built here are pinned against each other rather
// than each on its own: an implementation that answered one label for everything would satisfy either
// half alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMeshDerivationRouteTest,
	"CrowdySDK.Mass.MeshDerivationNamesTheRouteAMeshCameFrom", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMeshDerivationRouteTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> NativeMesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());

	const FCrowdyMassMeshDerivationTestCharacterMeshScope Scope(NativeMesh.Get());
	if (!TestNotNull(TEXT("A Character carries a mesh component to put the mesh on"), Scope.Component))
	{
		return false;
	}

	const FCrowdyMassDerivedMesh FromNative = FCrowdyMassMeshDerivation::FromClass(ACharacter::StaticClass());
	TestEqual(TEXT("A mesh on a component declared in C++ is reported as coming from one"),
		static_cast<int32>(FromNative.Route), static_cast<int32>(ECrowdyMassMeshRoute::NativeComponent));
	TestEqual(TEXT("And it names the component it was read off"), FromNative.ComponentName, Scope.Component->GetName());
	TestFalse(TEXT("And what holds that component"), FromNative.RouteOwnerName.IsEmpty());

	const TStrongObjectPtr<USkeletalMesh> BlueprintMesh(FCrowdyMassMeshDerivationTestFixture::MakeMesh());

	// Parented to Actor so the construction script is the only place a mesh could come from; a Character
	// parent would supply the native route instead and this half would agree for the wrong reason.
	const TStrongObjectPtr<UBlueprintGeneratedClass> ActorClass(
		FCrowdyMassMeshDerivationTestFixture::MakeCompiledClass(AActor::StaticClass()));
	FCrowdyMassMeshDerivationTestFixture::AddMeshNode(ActorClass.Get(), BlueprintMesh.Get());

	const FCrowdyMassDerivedMesh FromBlueprint = FCrowdyMassMeshDerivation::FromClass(ActorClass.Get());
	TestEqual(TEXT("A mesh on a construction script template is reported as added in the Blueprint"),
		static_cast<int32>(FromBlueprint.Route), static_cast<int32>(ECrowdyMassMeshRoute::BlueprintComponent));

	// A route recorded when nothing was found would name a component that supplied no mesh, which is
	// worse than saying nothing.
	const FCrowdyMassDerivedMesh FromNothing = FCrowdyMassMeshDerivation::FromClass(AActor::StaticClass());
	TestEqual(TEXT("A class with no mesh names no route"),
		static_cast<int32>(FromNothing.Route), static_cast<int32>(ECrowdyMassMeshRoute::None));
	TestTrue(TEXT("And no component"), FromNothing.ComponentName.IsEmpty());

	return true;
}

/**
 * The usage flag is read off each material slot, and off the slots the flag can act on.
 *
 * A crowd whose material is missing this flag spawns, moves and is hittable, and only renders as the
 * default material, so nothing else in the system has anything to report. That is exactly why the flag
 * itself is read: the one answer that must never be produced here is one inferred from whether the crowd
 * turned up. The counted slots are pinned as well as the offending one, because a check that quietly
 * looked at nothing would agree with every clear verdict there is.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMaterialUsageFlagTest,
	"CrowdySDK.Mass.MaterialUsageFlagIsReadOffEachMaterialSlot", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMaterialUsageFlagTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMaterial> Flagged(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/true));
	const TStrongObjectPtr<UMaterial> Unflagged(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/false));

	// A material outside the three domains the engine gates usage flags on. Reporting one would be a
	// sentence about something that has no bearing on what the crowd looks like.
	const TStrongObjectPtr<UMaterial> Interface(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/false));
	Interface->MaterialDomain = MD_UI;

	TArray<FSkeletalMaterial> Slots;
	Slots.Add(FSkeletalMaterial(Flagged.Get(), FName(TEXT("Body"))));
	Slots.Add(FSkeletalMaterial(Unflagged.Get(), FName(TEXT("Hair"))));
	Slots.Add(FSkeletalMaterial(nullptr, FName(TEXT("Empty"))));
	Slots.Add(FSkeletalMaterial(Interface.Get(), FName(TEXT("Overlay"))));

	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(Slots));

	const FCrowdyMassInstancedSkinnedMaterials Verdict =
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(Mesh.Get());

	TestEqual(TEXT("Every slot the mesh declares is counted"), Verdict.SlotCount, 4);
	TestEqual(TEXT("An empty slot and a domain the flag cannot act on are counted out of what was checked"),
		Verdict.SlotsChecked, 2);

	if (!TestEqual(TEXT("Only the slot whose material is missing the flag is reported"), Verdict.Unflagged.Num(), 1))
	{
		return false;
	}

	const FCrowdyMassUnflaggedMaterialSlot& Slot = Verdict.Unflagged[0];
	TestEqual(TEXT("It is named by its position on the mesh"), Slot.SlotIndex, 1);
	TestEqual(TEXT("And by the slot name an author sees beside it"), Slot.SlotName.ToString(), FString(TEXT("Hair")));
	TestEqual(TEXT("And it names the material actually assigned there"), Slot.MaterialPath, Unflagged->GetPathName());
	TestEqual(TEXT("A material with no parent is the material the flag goes on"), Slot.FlagOwnerPath, Unflagged->GetPathName());
	TestFalse(TEXT("A verdict with an unflagged slot is not clear"), Verdict.IsClear());

	// The sentence is the whole deliverable for the console command, so it is read rather than a count
	// beside it, which would still agree if the sentence said nothing.
	const FString Line = Verdict.Describe();
	TestTrue(TEXT("The sentence says how many of how many"), Line.Contains(TEXT("1 of 2")));
	TestTrue(TEXT("It names the slot"), Line.Contains(TEXT("slot 1 'Hair'")));
	TestTrue(TEXT("It names the material to tick the flag on"), Line.Contains(*Unflagged->GetPathName()));
	TestTrue(TEXT("And the checkbox to tick, in the words the material's own panel uses"),
		Line.Contains(TEXT("Used with Instanced Skinned Meshes")));

	// A clear mesh has to be clear for the right reason: it counted the slots and they passed, rather
	// than skipping them.
	TArray<FSkeletalMaterial> ClearSlots;
	ClearSlots.Add(FSkeletalMaterial(Flagged.Get(), FName(TEXT("Body"))));
	ClearSlots.Add(FSkeletalMaterial(Flagged.Get(), FName(TEXT("Hair"))));

	const TStrongObjectPtr<USkeletalMesh> ClearMesh(FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(ClearSlots));
	const FCrowdyMassInstancedSkinnedMaterials ClearVerdict =
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(ClearMesh.Get());

	TestTrue(TEXT("A mesh whose materials all carry the flag has nothing to report"), ClearVerdict.IsClear());
	TestEqual(TEXT("And it says so having checked both slots"), ClearVerdict.SlotsChecked, 2);
	TestTrue(TEXT("Which the sentence states rather than implying by saying nothing"),
		ClearVerdict.Describe().Contains(TEXT("all 2")));

	const FCrowdyMassInstancedSkinnedMaterials NoAsset =
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(nullptr);
	TestTrue(TEXT("With no mesh there is nothing to report"), NoAsset.IsClear());
	TestEqual(TEXT("And nothing is claimed to have been checked"), NoAsset.SlotsChecked, 0);

	return true;
}

/**
 * A slot holding an instance names the material at the root of its chain, which is where the flag goes.
 *
 * Naming the instance would send an author to a checkbox that is not there. Both halves are the case: an
 * instance over an unflagged material is reported against its parent, and an instance over a flagged one
 * is not reported at all, because the flag is inherited and a check that read the instance's own
 * authored state would call a correctly set up crowd broken.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMaterialUsageFlagBaseMaterialTest,
	"CrowdySDK.Mass.MaterialUsageFlagNamesTheMaterialToTickItOn", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMaterialUsageFlagBaseMaterialTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMaterial> Unflagged(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/false));
	const TStrongObjectPtr<UMaterialInstanceDynamic> Instance(
		UMaterialInstanceDynamic::Create(Unflagged.Get(), GetTransientPackage()));

	if (!TestNotNull(TEXT("An instance can be built over the material"), Instance.Get()))
	{
		return false;
	}

	TArray<FSkeletalMaterial> Slots;
	Slots.Add(FSkeletalMaterial(Instance.Get(), FName(TEXT("Body"))));

	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(Slots));

	const FCrowdyMassInstancedSkinnedMaterials Verdict =
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(Mesh.Get());

	if (!TestEqual(TEXT("An instance over an unflagged material is reported"), Verdict.Unflagged.Num(), 1))
	{
		return false;
	}

	TestEqual(TEXT("The slot still names what is actually assigned to it"),
		Verdict.Unflagged[0].MaterialPath, Instance->GetPathName());
	TestEqual(TEXT("And separately names the material the flag has to be ticked on"),
		Verdict.Unflagged[0].FlagOwnerPath, Unflagged->GetPathName());

	const TStrongObjectPtr<UMaterial> Flagged(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/true));
	const TStrongObjectPtr<UMaterialInstanceDynamic> FlaggedInstance(
		UMaterialInstanceDynamic::Create(Flagged.Get(), GetTransientPackage()));

	if (!TestNotNull(TEXT("An instance can be built over the flagged material too"), FlaggedInstance.Get()))
	{
		return false;
	}

	TArray<FSkeletalMaterial> FlaggedSlots;
	FlaggedSlots.Add(FSkeletalMaterial(FlaggedInstance.Get(), FName(TEXT("Body"))));

	const TStrongObjectPtr<USkeletalMesh> FlaggedMesh(
		FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(FlaggedSlots));

	TestTrue(TEXT("An instance inherits the flag, so it has nothing to report"),
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(FlaggedMesh.Get()).IsClear());

	return true;
}

/**
 * The warning is said once per material, and every other material still speaks.
 *
 * The subject is the material the flag is ticked on rather than the mesh or the class, because that is
 * the unit of the fix: a project whose characters share one parent material has one thing to change, and
 * a line per class would bury it. Both directions are the case. Too loose and it is a line per class
 * again; too broad and a second material is silenced, which is a separate fix that would then never be
 * asked for, and a diagnostic that never fires looks exactly like a system with nothing to report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassMaterialUsageFlagOncePerMaterialTest,
	"CrowdySDK.Mass.MaterialUsageFlagIsSaidOncePerMaterial", CrowdyMassMeshDerivationTestFlags)
bool FCrowdyMassMaterialUsageFlagOncePerMaterialTest::RunTest(const FString& Parameters)
{
	FCrowdyMassMaterialUsageReports::ResetForTest();
	ON_SCOPE_EXIT
	{
		FCrowdyMassMaterialUsageReports::ResetForTest();
	};

	const TStrongObjectPtr<UMaterial> First(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/false));
	const TStrongObjectPtr<UMaterial> Second(FCrowdyMassMeshDerivationTestFixture::MakeMaterial(/*bFlagged=*/false));

	// Two slots over one material, so the same verdict can show that the record is per material and not
	// per slot within a single mesh either.
	TArray<FSkeletalMaterial> Slots;
	Slots.Add(FSkeletalMaterial(First.Get(), FName(TEXT("Body"))));
	Slots.Add(FSkeletalMaterial(First.Get(), FName(TEXT("Hair"))));

	const TStrongObjectPtr<USkeletalMesh> Mesh(FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(Slots));
	const FCrowdyMassInstancedSkinnedMaterials Verdict =
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(Mesh.Get());

	if (!TestEqual(TEXT("Both slots are in the verdict"), Verdict.Unflagged.Num(), 2))
	{
		return false;
	}

	TArray<FString> Messages;
	FCrowdyMassMaterialUsageReports::TakeUnreported(Verdict, Mesh.Get(), ACharacter::StaticClass(), Messages);

	if (!TestEqual(TEXT("Two slots over one material are one thing to say"), Messages.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("The message names the material the flag is ticked on"), Messages[0].Contains(*First->GetPathName()));
	TestTrue(TEXT("It says what the crowd looks like instead"), Messages[0].Contains(TEXT("default material")));
	TestTrue(TEXT("It says the fix has to be saved, which is what makes it come back tomorrow"),
		Messages[0].Contains(TEXT("RESAVE")));
	TestTrue(TEXT("It names the mesh it was run into on"), Messages[0].Contains(*Mesh->GetPathName()));
	TestTrue(TEXT("And the class that was being worked out at the time"),
		Messages[0].Contains(*ACharacter::StaticClass()->GetPathName()));
	TestTrue(TEXT("And it points at the command that answers in full, since this line is said only once"),
		Messages[0].Contains(TEXT("crowdy.mass.derive")));

	Messages.Reset();
	FCrowdyMassMaterialUsageReports::TakeUnreported(Verdict, Mesh.Get(), ACharacter::StaticClass(), Messages);
	TestEqual(TEXT("Asked a second time about the same material it says nothing"), Messages.Num(), 0);

	// A different mesh entirely, over the same material. Keyed on the mesh this would speak again and the
	// once-per-material claim would be false.
	TArray<FString> SharedMessages;
	TArray<FSkeletalMaterial> SharedSlots;
	SharedSlots.Add(FSkeletalMaterial(First.Get(), FName(TEXT("Body"))));

	const TStrongObjectPtr<USkeletalMesh> SharedMesh(
		FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(SharedSlots));

	FCrowdyMassMaterialUsageReports::TakeUnreported(
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(SharedMesh.Get()),
		SharedMesh.Get(), AActor::StaticClass(), SharedMessages);
	TestEqual(TEXT("Nor about a second mesh sharing that material"), SharedMessages.Num(), 0);

	// And the half that fails silently: a different material is a different fix and has to be asked for.
	TArray<FString> SecondMessages;
	TArray<FSkeletalMaterial> SecondSlots;
	SecondSlots.Add(FSkeletalMaterial(Second.Get(), FName(TEXT("Body"))));

	const TStrongObjectPtr<USkeletalMesh> SecondMesh(
		FCrowdyMassMeshDerivationTestFixture::MakeMeshWithSlots(SecondSlots));

	FCrowdyMassMaterialUsageReports::TakeUnreported(
		FCrowdyMassMeshDerivation::CheckInstancedSkinnedMaterials(SecondMesh.Get()),
		SecondMesh.Get(), AActor::StaticClass(), SecondMessages);

	if (!TestEqual(TEXT("A second material is a second thing to fix and still speaks"), SecondMessages.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("And it names itself rather than the first"), SecondMessages[0].Contains(*Second->GetPathName()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
