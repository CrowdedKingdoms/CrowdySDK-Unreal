#include "Derivation/CrowdyMassNearTierAuthoredConfig.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/CrowdyMassNearTierActor.h"
#include "Derivation/CrowdyMassNearTierAuthoredConfigTestTypes.h"
#include "Derivation/CrowdyMassNearTierDerivation.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassMovableVisualizationTrait.h"
#include "MassProcessingTypes.h"
#include "MassRepresentationActorManagement.h"
#include "MassRepresentationTypes.h"
#include "MassVisualizationTrait.h"
#include "Misc/AutomationTest.h"
#include "Representation/CrowdyMassRepresentationActorManagement.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The near tier is declared on a CLASS, and until this work it was read only where a Mass entity config is
// built FROM a class, which happens exactly when nothing authored answers for that class. A project with an
// authored config therefore declared a shell that nothing read, silently, and whether the documented opt-in
// did anything depended on a fact the declaration does not mention.
//
// What is pinned here is the wiring that closes that, and above all its boundaries: what it refuses to
// touch, and when it declines to act at all.
static constexpr EAutomationTestFlags CrowdyMassNearTierAuthoredTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassNearTierAuthoredConfigTestFixture
{
	static UMassEntityConfigAsset* MakeConfig()
	{
		return NewObject<UMassEntityConfigAsset>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	/**
	 * A visualization trait of a SUBCLASS, which is what a project actually authors: the traits that ship
	 * with the engine and its plugins are all subclasses of the base, and a lookup written against the exact
	 * base class would find none of them while passing a case that used one.
	 *
	 * Every level is set explicitly rather than left at the trait's own defaults, and that is load-bearing
	 * twice over. Several traits set the nearest level to a spawned actor in their constructor, so a fixture
	 * that inherited it would be asserting about the constructor. And the base constructor leaves a level at
	 * StaticMeshInstance with no static mesh description, which makes the engine's own editor validation log
	 * an ERROR while a trait is being duplicated, and an error-level log fails a running automation test.
	 */
	static UMassMovableVisualizationTrait* AddVisualizationTrait(UMassEntityConfigAsset* Config,
		UClass* HighResTemplateActor, const EMassRepresentationType HighRepresentation)
	{
		UMassMovableVisualizationTrait* Trait =
			NewObject<UMassMovableVisualizationTrait>(Config, NAME_None, RF_Transient);

		Trait->HighResTemplateActor = HighResTemplateActor;

		for (int32 LODIndex = 0; LODIndex < EMassLOD::Max; ++LODIndex)
		{
			Trait->Params.LODRepresentation[LODIndex] = EMassRepresentationType::SkinnedMeshInstance;
		}

		Trait->Params.LODRepresentation[EMassLOD::High] = HighRepresentation;

		Config->GetMutableConfig().AddTrait(*Trait);
		return Trait;
	}

	/** The visualization trait a patched config carries of its own, or null when it carries none. */
	static const UMassVisualizationTrait* GetOwnVisualizationTrait(const UMassEntityConfigAsset* Config)
	{
		if (!Config || Config->GetConfig().GetTraits().Num() == 0)
		{
			return nullptr;
		}

		return Cast<UMassVisualizationTrait>(Config->GetConfig().GetTraits()[0]);
	}

	static UMassEntityConfigAsset* Patch(const UMassEntityConfigAsset& Authored,
		const UMassVisualizationTrait& SourceTrait, const UClass** OutReplacedManagementClass = nullptr)
	{
		const UClass* Replaced = nullptr;
		UMassEntityConfigAsset* Patched = FCrowdyMassNearTierAuthoredConfig::BuildPatchedConfig(
			*GetTransientPackage(), Authored, SourceTrait, ACrowdyMassNearTierActor::StaticClass(), Replaced);

		if (OutReplacedManagementClass)
		{
			*OutReplacedManagementClass = Replaced;
		}

		return Patched;
	}

	/** The trait a patch may be written into, for a world that runs everything. */
	static const UMassVisualizationTrait* FindPatchable(const UMassEntityConfigAsset& Config, FString& OutRefusal)
	{
		return FCrowdyMassNearTierAuthoredConfig::FindPatchableVisualizationTrait(Config,
			EProcessorExecutionFlags::All, OutRefusal);
	}
};

/**
 * Whether an authored config already promotes takes BOTH halves of the answer.
 *
 * Naming a high resolution actor while the nearest level is still drawn as an instance promotes nothing,
 * and traits that ship with the engine set that level to a spawned actor in their own constructor with no
 * actor named. So each half read alone answers a different question wrongly: the template actor alone calls
 * a half-wired config finished and leaves promotion silently off, and the level alone calls an untouched
 * trait a deliberate decision and refuses to wire a class that asked to be promoted.
 *
 * All four cells are asserted because a rule that answered true always, or false always, passes any one of
 * them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredPromotionNeedsBothHalvesTest,
	"CrowdySDK.Mass.NearTierAuthoredPromotionNeedsBothHalves", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredPromotionNeedsBothHalvesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* Trait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Config.Get(), ACrowdyMassNearTierActor::StaticClass(), EMassRepresentationType::HighResSpawnedActor);

	TestTrue(TEXT("a config naming an actor with its nearest level spawning one already promotes"),
		FCrowdyMassNearTierAuthoredConfig::AlreadyPromotes(*Trait));

	Trait->Params.LODRepresentation[EMassLOD::High] = EMassRepresentationType::SkinnedMeshInstance;
	TestFalse(TEXT("naming an actor while the nearest level is still an instance promotes nothing, so it does not count"),
		FCrowdyMassNearTierAuthoredConfig::AlreadyPromotes(*Trait));

	Trait->HighResTemplateActor = nullptr;
	Trait->Params.LODRepresentation[EMassLOD::High] = EMassRepresentationType::HighResSpawnedActor;
	TestFalse(TEXT("a nearest level asking for an actor with no actor named promotes nothing either"),
		FCrowdyMassNearTierAuthoredConfig::AlreadyPromotes(*Trait));

	Trait->Params.LODRepresentation[EMassLOD::High] = EMassRepresentationType::SkinnedMeshInstance;
	TestFalse(TEXT("and a config that says neither certainly does not"),
		FCrowdyMassNearTierAuthoredConfig::AlreadyPromotes(*Trait));

	return true;
}

/**
 * A config whose own promotion this plugin cannot drive is reported, even though nothing changes it.
 *
 * This is the branch where an authored config wins and the class's declaration is shadowed, so it is the
 * one branch that looks at a promotion it did not build. Both of the things a DECLARED shell must satisfy
 * are checked here too, because an actor with no view component is placed once and then stands still, and a
 * management class that cannot hide one leaves the entity drawn twice. From the outside both are
 * indistinguishable from a promotion that works, and the line above them says it works.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredPromotionIsCheckedForUsabilityTest,
	"CrowdySDK.Mass.NearTierAuthoredPromotionIsCheckedForUsability", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredPromotionIsCheckedForUsabilityTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* Trait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Config.Get(), ACrowdyMassNearTierActor::StaticClass(), EMassRepresentationType::HighResSpawnedActor);

	FString Problem;

	// The stock management class is what every visualization trait carries out of its own constructor, so
	// this is the state an author reaches by filling in a template actor and nothing else.
	TestFalse(TEXT("a promotion under the stock management class is refused"),
		FCrowdyMassNearTierAuthoredConfig::PromotesUsably(*Trait, Problem));
	TestTrue(TEXT("and says the actor would be drawn twice, which is the visible symptom"),
		Problem.Contains(TEXT("drawn twice")));

	Trait->Params.RepresentationActorManagementClass = UCrowdyMassNearTierAuthoredTestManagement::StaticClass();
	TestTrue(TEXT("with a management class deriving from this plugin's, the same promotion is usable"),
		FCrowdyMassNearTierAuthoredConfig::PromotesUsably(*Trait, Problem));
	TestTrue(TEXT("and an answer that succeeded has nothing to explain"), Problem.IsEmpty());

	// The other half: a management class that CAN hide, promoting to an actor nothing drives. An ordinary
	// Actor is what an author reaches for, and it passes every class check there is.
	Trait->HighResTemplateActor = AActor::StaticClass();
	TestFalse(TEXT("a promotion to an actor with no near tier component is refused"),
		FCrowdyMassNearTierAuthoredConfig::PromotesUsably(*Trait, Problem));
	TestTrue(TEXT("and says the component is what is missing"),
		Problem.Contains(TEXT("no Crowdy Mass Near Tier component")));

	Trait->HighResTemplateActor = nullptr;
	TestFalse(TEXT("and a trait naming no actor at all is not a usable promotion either"),
		FCrowdyMassNearTierAuthoredConfig::PromotesUsably(*Trait, Problem));

	return true;
}

/**
 * The patch is a per-class config that names the authored one as its parent, and the authored asset comes
 * out of it exactly as it went in.
 *
 * The authored asset can be shared by several classes and is a file on disk besides, so writing to it would
 * give every class the last one's shell and dirty content nobody edited. That claim is asserted directly
 * rather than inferred from the patched config being correct: a patch that wrote through to the source
 * would produce an identical patched config and pass every other case here.
 *
 * The guid is asserted for a reason that only shows up in a cooked build. Mass keys its template registry
 * on the config's guid and duplicating a config asset carries the source's guid along with it, with only an
 * editor-only hook to regenerate it. A patch built by duplicating the ASSET would therefore hand back the
 * authored template in a packaged game and do nothing at all, while working perfectly in the editor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredPatchLeavesTheAuthoredAssetAloneTest,
	"CrowdySDK.Mass.NearTierAuthoredPatchLeavesTheAuthoredAssetAlone", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredPatchLeavesTheAuthoredAssetAloneTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Authored(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* AuthoredTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Authored.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	// Read before the patch rather than asserted against a constant afterwards. Every visualization trait
	// arrives with the stock management class already in this slot from its own constructor, so a case that
	// expected an empty one would be asserting about the engine's defaults instead of about what was
	// written through.
	const UClass* AuthoredManagementBefore = AuthoredTrait->Params.RepresentationActorManagementClass;

	const TStrongObjectPtr<UMassEntityConfigAsset> Patched(
		FCrowdyMassNearTierAuthoredConfigTestFixture::Patch(*Authored.Get(), *AuthoredTrait));

	if (!Patched.IsValid())
	{
		AddError(TEXT("a config carrying a visualization trait produced no patched config"));
		return false;
	}

	TestNotEqual(TEXT("the patch is its own config and not the authored one handed back"),
		static_cast<const UMassEntityConfigAsset*>(Patched.Get()),
		static_cast<const UMassEntityConfigAsset*>(Authored.Get()));
	TestEqual(TEXT("and it names the authored config as its parent, which is what carries everything not patched"),
		Patched->GetConfig().GetParent(), static_cast<const UMassEntityConfigAsset*>(Authored.Get()));
	TestNotEqual(TEXT("its guid is its own, so the template registry cannot answer it with the authored template"),
		Patched->GetConfig().GetGuid(), Authored->GetConfig().GetGuid());

	const UMassVisualizationTrait* PatchedTrait =
		FCrowdyMassNearTierAuthoredConfigTestFixture::GetOwnVisualizationTrait(Patched.Get());

	if (!PatchedTrait)
	{
		AddError(TEXT("the patched config carries no visualization trait of its own"));
		return false;
	}

	TestEqual(TEXT("the patched trait names the declared shell"),
		static_cast<const UClass*>(PatchedTrait->HighResTemplateActor),
		static_cast<const UClass*>(ACrowdyMassNearTierActor::StaticClass()));
	TestEqual(TEXT("and its nearest level spawns an actor, without which naming one changes nothing"),
		static_cast<int32>(PatchedTrait->Params.LODRepresentation[EMassLOD::High]),
		static_cast<int32>(EMassRepresentationType::HighResSpawnedActor));
	TestEqual(TEXT("and it is managed by the class that hides, places and readies a promoted actor"),
		static_cast<const UClass*>(PatchedTrait->Params.RepresentationActorManagementClass),
		static_cast<const UClass*>(UCrowdyMassRepresentationActorManagement::StaticClass()));

	// The three levels BELOW the nearest are the far crowd and are none of the near tier's business. The
	// derived config path writes all four on purpose; doing the same here would change how a promoted
	// class's distant entities are drawn, and no other assertion in this file would notice.
	for (int32 LODIndex = EMassLOD::High + 1; LODIndex < EMassLOD::Max; ++LODIndex)
	{
		TestEqual(TEXT("a level below the nearest is left exactly as the config authored it"),
			static_cast<int32>(PatchedTrait->Params.LODRepresentation[LODIndex]),
			static_cast<int32>(EMassRepresentationType::SkinnedMeshInstance));
	}

	TestNull(TEXT("the authored trait still names no actor, so nothing was written through to the shared asset"),
		static_cast<const UClass*>(AuthoredTrait->HighResTemplateActor));
	TestEqual(TEXT("its nearest level is still drawn as an instance"),
		static_cast<int32>(AuthoredTrait->Params.LODRepresentation[EMassLOD::High]),
		static_cast<int32>(EMassRepresentationType::SkinnedMeshInstance));
	TestEqual(TEXT("and its management class is the one it had, not the one the patch installs"),
		static_cast<const UClass*>(AuthoredTrait->Params.RepresentationActorManagementClass), AuthoredManagementBefore);
	TestNotEqual(TEXT("which is a claim worth making because the patch does install a different one"),
		AuthoredManagementBefore, static_cast<const UClass*>(UCrowdyMassRepresentationActorManagement::StaticClass()));

	return true;
}

/**
 * The patched trait OVERRIDES the authored one rather than joining it.
 *
 * Mass combines traits down a parent chain and replaces a parent's trait with a child's when the two match
 * on exact class and on the net modes they are valid for. Miss either and the template is built from BOTH
 * traits: two visualization traits, one of which still says the nearest level is an instance, which is a
 * config that reads as patched and promotes nothing.
 *
 * The pair is asserted rather than the outcome because building the template needs a world, and the pair is
 * exactly what the engine compares.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredPatchOverridesRatherThanAddsTest,
	"CrowdySDK.Mass.NearTierAuthoredPatchOverridesRatherThanAdds", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredPatchOverridesRatherThanAddsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Authored(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* AuthoredTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Authored.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	// Deliberately not the default. A duplicate copies whatever the source holds, so a fixture left at the
	// default would pass even if the patch built a fresh trait that happened to start there.
	AuthoredTrait->ValidTargetConfig = EProcessorExecutionFlags::Client;

	const TStrongObjectPtr<UMassEntityConfigAsset> Patched(
		FCrowdyMassNearTierAuthoredConfigTestFixture::Patch(*Authored.Get(), *AuthoredTrait));

	if (!Patched.IsValid() || Patched->GetConfig().GetTraits().Num() != 1)
	{
		AddError(TEXT("the patched config should carry exactly one trait of its own, the visualization override"));
		return false;
	}

	const UMassEntityTraitBase* PatchedTrait = Patched->GetConfig().GetTraits()[0];

	TestEqual(TEXT("the patched trait has the authored trait's exact class, which is half of what makes it an override"),
		PatchedTrait->GetClass(), AuthoredTrait->GetClass());
	TestEqual(TEXT("and the same valid net modes, which is the other half"),
		static_cast<int32>(PatchedTrait->ValidTargetConfig), static_cast<int32>(AuthoredTrait->ValidTargetConfig));

	return true;
}

/**
 * A config carrying two visualization traits that BOTH build is refused rather than half-patched.
 *
 * The patched trait wins only because it writes the representation parameters last. Mass keeps the FIRST
 * const shared fragment offered for a type and discards every later one, with nothing but an ensure to say
 * so, and an ensure is compiled out of Shipping. So a second visualization trait of a DIFFERENT class also
 * builds, writes those parameters first, and the patch is thrown away: the entity ends up with a valid high
 * resolution actor index that its level table never selects. Nothing promotes and nothing is logged.
 *
 * A parent and a child holding traits of the same class is the ordinary way to author this and must stay
 * allowed, because Mass keeps one of them and the patch overrides whichever it kept. Both cells are here
 * because a rule that refused everything would pass the first one alone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredSecondVisualizationTraitIsRefusedTest,
	"CrowdySDK.Mass.NearTierAuthoredSecondVisualizationTraitIsRefused", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredSecondVisualizationTraitIsRefusedTest::RunTest(const FString& Parameters)
{
	FString Refusal;

	// One trait, in the config's own list: the ordinary case, and the control for everything below.
	const TStrongObjectPtr<UMassEntityConfigAsset> Simple(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	const UMassVisualizationTrait* SimpleTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Simple.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	TestEqual(TEXT("a config with one visualization trait answers with it"),
		FCrowdyMassNearTierAuthoredConfigTestFixture::FindPatchable(*Simple.Get(), Refusal), SimpleTrait);
	TestTrue(TEXT("and has nothing to explain"), Refusal.IsEmpty());

	// A parent and a child of the SAME class. Mass keeps one, so this is one writer, not two.
	const TStrongObjectPtr<UMassEntityConfigAsset> SameClassParent(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		SameClassParent.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	const TStrongObjectPtr<UMassEntityConfigAsset> SameClassChild(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	const UMassVisualizationTrait* ChildTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		SameClassChild.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);
	SameClassChild->GetMutableConfig().SetParentAsset(*SameClassParent.Get());

	TestEqual(TEXT("a child overriding its parent's trait of the same class is not two writers"),
		FCrowdyMassNearTierAuthoredConfigTestFixture::FindPatchable(*SameClassChild.Get(), Refusal), ChildTrait);
	TestTrue(TEXT("so it is not refused"), Refusal.IsEmpty());

	// Two DIFFERENT classes, which Mass does not dedupe: both build, and the first one to write the
	// representation parameters keeps them.
	const TStrongObjectPtr<UMassEntityConfigAsset> RivalParent(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	UMassVisualizationTrait* BaseClassTrait =
		NewObject<UMassVisualizationTrait>(RivalParent.Get(), NAME_None, RF_Transient);
	for (int32 LODIndex = 0; LODIndex < EMassLOD::Max; ++LODIndex)
	{
		BaseClassTrait->Params.LODRepresentation[LODIndex] = EMassRepresentationType::SkinnedMeshInstance;
	}
	RivalParent->GetMutableConfig().AddTrait(*BaseClassTrait);

	const TStrongObjectPtr<UMassEntityConfigAsset> RivalChild(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		RivalChild.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);
	RivalChild->GetMutableConfig().SetParentAsset(*RivalParent.Get());

	TestNull(TEXT("a chain carrying visualization traits of two different classes is refused"),
		FCrowdyMassNearTierAuthoredConfigTestFixture::FindPatchable(*RivalChild.Get(), Refusal));
	TestTrue(TEXT("and names both of them, since which one to fix is the whole question"),
		Refusal.Contains(TEXT("MassMovableVisualizationTrait")) && Refusal.Contains(TEXT("MassVisualizationTrait")));

	return true;
}

/**
 * A trait Mass will not build here cannot carry a near tier, and is refused rather than patched.
 *
 * Traits are filtered by their valid net modes before they build, so a duplicate of a server-only trait
 * never runs on a client. Patching one produces a config that is cached, reported as a promotion, and
 * changes nothing at all. The cell that matters is the one where the trait exists and is simply not for
 * this world: a check written on the trait's presence alone passes it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredWrongNetModeTraitIsRefusedTest,
	"CrowdySDK.Mass.NearTierAuthoredWrongNetModeTraitIsRefused", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredWrongNetModeTraitIsRefusedTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* Trait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Config.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	Trait->ValidTargetConfig = EProcessorExecutionFlags::Server;

	FString Refusal;
	TestNull(TEXT("a server-only visualization trait is refused on a client"),
		FCrowdyMassNearTierAuthoredConfig::FindPatchableVisualizationTrait(*Config.Get(),
			EProcessorExecutionFlags::Client, Refusal));
	TestTrue(TEXT("and says the net mode is why, not that the trait is missing"),
		Refusal.Contains(TEXT("net mode")));

	TestEqual(TEXT("and the same trait is patchable on a server, where Mass does build from it"),
		FCrowdyMassNearTierAuthoredConfig::FindPatchableVisualizationTrait(*Config.Get(),
			EProcessorExecutionFlags::Server, Refusal),
		static_cast<const UMassVisualizationTrait*>(Trait));
	TestTrue(TEXT("with nothing to explain"), Refusal.IsEmpty());

	return true;
}

/**
 * Everything the author set that promotion has no opinion about survives the patch.
 *
 * A near tier changes what the nearest level DRAWS, not where that level begins or how many entities may be
 * in it: those are somebody's authored LOD table, and rewriting them from project settings would move the
 * far crowd of every class that opted in. Rebuilding the trait rather than duplicating it is the mistake
 * this rules out, and it is invisible from the promotion fields alone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredPatchKeepsTheAuthoredLodTableTest,
	"CrowdySDK.Mass.NearTierAuthoredPatchKeepsTheAuthoredLodTable", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredPatchKeepsTheAuthoredLodTableTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Authored(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* AuthoredTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Authored.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	AuthoredTrait->LODParams.BaseLODDistance[EMassLOD::Medium] = 777.0f;
	AuthoredTrait->LODParams.VisibleLODDistance[EMassLOD::Medium] = 888.0f;
	AuthoredTrait->LODParams.LODMaxCount[EMassLOD::High] = 7;
	AuthoredTrait->Params.NotVisibleUpdateRate = 0.25f;

	const TStrongObjectPtr<UMassEntityConfigAsset> Patched(
		FCrowdyMassNearTierAuthoredConfigTestFixture::Patch(*Authored.Get(), *AuthoredTrait));

	const UMassVisualizationTrait* PatchedTrait =
		FCrowdyMassNearTierAuthoredConfigTestFixture::GetOwnVisualizationTrait(Patched.Get());

	if (!PatchedTrait)
	{
		AddError(TEXT("the patched config carries no visualization trait of its own"));
		return false;
	}

	TestEqual(TEXT("the authored distance the near tier band ends at is kept"),
		PatchedTrait->LODParams.BaseLODDistance[EMassLOD::Medium], 777.0f);
	TestEqual(TEXT("and the visible one beside it"),
		PatchedTrait->LODParams.VisibleLODDistance[EMassLOD::Medium], 888.0f);
	TestEqual(TEXT("and the ceiling on how many entities may be promoted at once, which is the authored table's and not a project setting"),
		PatchedTrait->LODParams.LODMaxCount[EMassLOD::High], 7);
	TestEqual(TEXT("and a representation parameter promotion has no opinion about at all"),
		PatchedTrait->Params.NotVisibleUpdateRate, 0.25f);

	return true;
}

/**
 * The band below the near tier that asks for an actor it has no template for is found and named.
 *
 * Both engine visualization trait constructors set that band to a low resolution spawned actor and neither
 * ever names a low resolution template actor, so a config left at those defaults logs two errors per entity
 * per frame once entities reach it and draws them through a fallback. The near tier does not rewrite it,
 * because it is somebody's authored level table, but nothing else looks at that band at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredUnusableSpawnedActorBandIsFoundTest,
	"CrowdySDK.Mass.NearTierAuthoredUnusableSpawnedActorBandIsFound", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredUnusableSpawnedActorBandIsFoundTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* Trait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Config.Get(), ACrowdyMassNearTierActor::StaticClass(), EMassRepresentationType::HighResSpawnedActor);

	TestEqual(TEXT("a table whose lower bands are all instanced has no unusable band"),
		FCrowdyMassNearTierAuthoredConfig::FindUnusableSpawnedActorBand(*Trait), INDEX_NONE);

	Trait->Params.LODRepresentation[EMassLOD::Medium] = EMassRepresentationType::LowResSpawnedActor;
	TestEqual(TEXT("a band asking for a low resolution actor with none named is found, and named by level"),
		FCrowdyMassNearTierAuthoredConfig::FindUnusableSpawnedActorBand(*Trait), static_cast<int32>(EMassLOD::Medium));

	Trait->LowResTemplateActor = ACrowdyMassNearTierActor::StaticClass();
	TestEqual(TEXT("the same band is fine once the config names a low resolution template actor"),
		FCrowdyMassNearTierAuthoredConfig::FindUnusableSpawnedActorBand(*Trait), INDEX_NONE);

	// The NEAREST band is excluded on purpose: it is the one being patched, and it always has a template
	// actor by construction. Reporting it would fire on every successful patch.
	Trait->LowResTemplateActor = nullptr;
	Trait->Params.LODRepresentation[EMassLOD::Medium] = EMassRepresentationType::SkinnedMeshInstance;
	Trait->Params.LODRepresentation[EMassLOD::High] = EMassRepresentationType::LowResSpawnedActor;
	TestEqual(TEXT("the nearest band is not reported, since that is the one the near tier owns"),
		FCrowdyMassNearTierAuthoredConfig::FindUnusableSpawnedActorBand(*Trait), INDEX_NONE);

	return true;
}

/**
 * The visualization trait is looked for down the parent chain, and a parent's trait is copied rather than
 * written to.
 *
 * Sharing one base config between several classes is the ordinary way to author this, and a lookup that
 * read only a config's own traits would report such a config as carrying no visualization at all: the
 * advice that produces sends an author to add a trait they already have. The second half is the danger the
 * first half creates, and it is worse than writing to the config itself, because a parent is shared by
 * every class that derives from it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredTraitIsFoundDownTheParentChainTest,
	"CrowdySDK.Mass.NearTierAuthoredTraitIsFoundDownTheParentChain", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredTraitIsFoundDownTheParentChainTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Parent(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* ParentTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Parent.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	const TStrongObjectPtr<UMassEntityConfigAsset> Child(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	Child->GetMutableConfig().SetParentAsset(*Parent.Get());

	FString Refusal;
	TestEqual(TEXT("a config with no visualization trait of its own answers with its parent's"),
		FCrowdyMassNearTierAuthoredConfigTestFixture::FindPatchable(*Child.Get(), Refusal),
		static_cast<const UMassVisualizationTrait*>(ParentTrait));

	const TStrongObjectPtr<UMassEntityConfigAsset> Patched(
		FCrowdyMassNearTierAuthoredConfigTestFixture::Patch(*Child.Get(), *ParentTrait));

	if (!Patched.IsValid())
	{
		AddError(TEXT("a config whose parent carries a visualization trait produced no patched config"));
		return false;
	}

	TestNull(TEXT("the parent's trait still names no actor, so a shared base config was not written through"),
		static_cast<const UClass*>(ParentTrait->HighResTemplateActor));
	TestEqual(TEXT("and its nearest level is still drawn as an instance"),
		static_cast<int32>(ParentTrait->Params.LODRepresentation[EMassLOD::High]),
		static_cast<int32>(EMassRepresentationType::SkinnedMeshInstance));

	// A config carrying nothing anywhere in its chain is the other answer, and it is not a fault to be
	// fixed by guessing: representation parameters come from a visualization trait and from nothing else.
	const TStrongObjectPtr<UMassEntityConfigAsset> Bare(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());
	TestNull(TEXT("a config with no visualization trait anywhere in its chain answers with none"),
		FCrowdyMassNearTierAuthoredConfigTestFixture::FindPatchable(*Bare.Get(), Refusal));
	TestTrue(TEXT("and says so rather than staying silent, since that is a config to fix"),
		Refusal.Contains(TEXT("no visualization trait")));

	return true;
}

/**
 * A management class that already does what promotion needs is kept; anything else is replaced, and the
 * replacement is reported rather than done quietly.
 *
 * Everything a promoted entity does visually is done by its management class: it hides the actor the stock
 * one leaves drawn, answers readiness, and places the actor at the entity's transform. Leaving a stock or a
 * third-party one in that slot produces a promotion that renders the entity twice and reads as working.
 * Overwriting unconditionally is the opposite mistake and takes a game's own subclass away from it.
 *
 * What is displaced can be doing real work of its own, and another crowd plugin's management class is a
 * SIBLING of this plugin's rather than a child, so it is always the one replaced. That is why the caller is
 * told which class it was rather than just that promotion succeeded.
 *
 * The kept case uses a real subclass rather than this plugin's own class, because keeping and replacing
 * produce the same value for the class itself and that cell would then pass either way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierAuthoredPatchKeepsACapableManagementClassTest,
	"CrowdySDK.Mass.NearTierAuthoredPatchKeepsACapableManagementClass", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierAuthoredPatchKeepsACapableManagementClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Authored(FCrowdyMassNearTierAuthoredConfigTestFixture::MakeConfig());

	UMassMovableVisualizationTrait* AuthoredTrait = FCrowdyMassNearTierAuthoredConfigTestFixture::AddVisualizationTrait(
		Authored.Get(), /*HighResTemplateActor=*/nullptr, EMassRepresentationType::SkinnedMeshInstance);

	AuthoredTrait->Params.RepresentationActorManagementClass = UCrowdyMassNearTierAuthoredTestManagement::StaticClass();

	const UClass* KeptReplaced = nullptr;
	const TStrongObjectPtr<UMassEntityConfigAsset> KeptPatch(
		FCrowdyMassNearTierAuthoredConfigTestFixture::Patch(*Authored.Get(), *AuthoredTrait, &KeptReplaced));

	const UMassVisualizationTrait* KeptTrait =
		FCrowdyMassNearTierAuthoredConfigTestFixture::GetOwnVisualizationTrait(KeptPatch.Get());

	if (!KeptTrait)
	{
		AddError(TEXT("the patched config carries no visualization trait of its own"));
		return false;
	}

	TestEqual(TEXT("a game's own subclass of this plugin's management class is left alone"),
		static_cast<const UClass*>(KeptTrait->Params.RepresentationActorManagementClass),
		static_cast<const UClass*>(UCrowdyMassNearTierAuthoredTestManagement::StaticClass()));
	TestNull(TEXT("and nothing is reported as replaced, because nothing was"), KeptReplaced);

	// The stock engine management class is the other half. It is a management class in every respect and
	// still cannot hide an actor, which is the one thing a handover from a crowd row cannot do without.
	AuthoredTrait->Params.RepresentationActorManagementClass = UMassRepresentationActorManagement::StaticClass();

	const UClass* Replaced = nullptr;
	const TStrongObjectPtr<UMassEntityConfigAsset> ReplacedPatch(
		FCrowdyMassNearTierAuthoredConfigTestFixture::Patch(*Authored.Get(), *AuthoredTrait, &Replaced));

	const UMassVisualizationTrait* ReplacedTrait =
		FCrowdyMassNearTierAuthoredConfigTestFixture::GetOwnVisualizationTrait(ReplacedPatch.Get());

	if (!ReplacedTrait)
	{
		AddError(TEXT("the second patched config carries no visualization trait of its own"));
		return false;
	}

	TestEqual(TEXT("a management class that is not one of ours is replaced with the one that hides and places a promoted actor"),
		static_cast<const UClass*>(ReplacedTrait->Params.RepresentationActorManagementClass),
		static_cast<const UClass*>(UCrowdyMassRepresentationActorManagement::StaticClass()));
	TestEqual(TEXT("and the class that was displaced is named, so the caller can say what it took away"),
		Replaced, static_cast<const UClass*>(UMassRepresentationActorManagement::StaticClass()));

	return true;
}

/**
 * Declaring nothing, declaring something unusable, and not being ready to answer are three outcomes, and
 * all three arrive as a null class.
 *
 * Every class in a project that has never asked for a near tier declares nothing, so treating that as a
 * fault would print a warning per class for a project doing nothing wrong. A class that added the component
 * and left its slot empty is a fault and was silent until this told it apart: it is the one authoring
 * mistake that looks exactly like never having asked. And a class still loading has no class default object
 * to read, which answers the same null as a class with no component while needing no fix at all, so an
 * answer memoised for it would make a moment permanent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierShellRefusalIsNotADeclarationOfNothingTest,
	"CrowdySDK.Mass.NearTierShellRefusalIsNotADeclarationOfNothing", CrowdyMassNearTierAuthoredTestFlags)
bool FCrowdyMassNearTierShellRefusalIsNotADeclarationOfNothingTest::RunTest(const FString& Parameters)
{
	FString Refusal;
	bool bAskAgainLater = true;

	const FCrowdyMassDerivedNearTierShell Undeclared;
	TestNull(TEXT("a class that declared nothing has no shell to promote to"),
		FCrowdyMassNearTierDerivation::LoadUsableShellClass(AActor::StaticClass(), Undeclared, Refusal, bAskAgainLater));
	TestTrue(TEXT("and that is not a refusal, because it is what every class in a project without a near tier looks like"),
		Refusal.IsEmpty());
	TestFalse(TEXT("and it is a settled answer, so a caller may keep it"), bAskAgainLater);

	// The component added and its slot left empty. Same null class, different fault, and the one that was
	// silent on both config paths until the two were separated.
	FCrowdyMassDerivedNearTierShell EmptySlot;
	EmptySlot.bSawComponent = true;
	EmptySlot.Reason = TEXT("has a Crowdy Mass Near Tier Shell component, but no actor class is assigned to it.");

	TestNull(TEXT("a shell component with nothing assigned has no shell to promote to either"),
		FCrowdyMassNearTierDerivation::LoadUsableShellClass(AActor::StaticClass(), EmptySlot, Refusal, bAskAgainLater));
	TestFalse(TEXT("but it IS a refusal, because somebody asked for a near tier and did not finish"),
		Refusal.IsEmpty());
	TestTrue(TEXT("and it carries the reason the walk composed, rather than a second wording of it"),
		Refusal.Contains(TEXT("no actor class is assigned")));

	FCrowdyMassDerivedNearTierShell DeclaredWithoutView;
	DeclaredWithoutView.bSawComponent = true;
	DeclaredWithoutView.NearTierActorClass = AActor::StaticClass();
	DeclaredWithoutView.Route = ECrowdyMassNearTierRoute::BlueprintComponent;

	TestNull(TEXT("a shell that carries no near tier component is refused rather than promoted to"),
		FCrowdyMassNearTierDerivation::LoadUsableShellClass(AActor::StaticClass(), DeclaredWithoutView, Refusal, bAskAgainLater));
	TestTrue(TEXT("and says the component is what is missing, which is the fix"),
		Refusal.Contains(TEXT("no Crowdy Mass Near Tier component")));
	TestFalse(TEXT("and that is settled too: the class is formed and simply lacks the component"), bAskAgainLater);

	FCrowdyMassDerivedNearTierShell DeclaredAndUsable;
	DeclaredAndUsable.bSawComponent = true;
	DeclaredAndUsable.NearTierActorClass = ACrowdyMassNearTierActor::StaticClass();
	DeclaredAndUsable.Route = ECrowdyMassNearTierRoute::BlueprintComponent;

	TestEqual(TEXT("a shell that carries one answers with that class"),
		FCrowdyMassNearTierDerivation::LoadUsableShellClass(AActor::StaticClass(), DeclaredAndUsable, Refusal, bAskAgainLater),
		static_cast<UClass*>(ACrowdyMassNearTierActor::StaticClass()));
	TestTrue(TEXT("and a shell that was accepted has nothing to explain"), Refusal.IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
