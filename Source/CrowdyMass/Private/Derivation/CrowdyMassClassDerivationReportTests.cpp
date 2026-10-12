#include "Derivation/CrowdyMassClassDerivationReport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/CapsuleComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The report is what a person reads instead of running a crowd, so what is checked here is that every
// question it claims to answer is actually answered, and that the ones it cannot answer are left visibly
// unanswered rather than answered with a default.
static constexpr EAutomationTestFlags CrowdyMassClassDerivationReportTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

// The Character default object is shared by the whole process, so a case that puts a mesh on it has to
// put it back afterwards or every later case sees the change.
struct FCrowdyMassReportTestCharacterMeshScope
{
	explicit FCrowdyMassReportTestCharacterMeshScope(USkeletalMesh* Mesh)
	{
		ACharacter* DefaultCharacter = ACharacter::StaticClass()->GetDefaultObject<ACharacter>();
		Component = DefaultCharacter ? DefaultCharacter->GetMesh() : nullptr;
		if (!Component)
		{
			return;
		}

		Previous = Component->GetSkinnedAsset();
		Component->SetSkinnedAssetAndUpdate(Mesh);
	}

	~FCrowdyMassReportTestCharacterMeshScope()
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassClassDerivationReportResolvedTest,
	"CrowdySDK.Mass.ClassDerivationReportDescribesAClassThatWouldDerive", CrowdyMassClassDerivationReportTestFlags)
bool FCrowdyMassClassDerivationReportResolvedTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient));

	const FCrowdyMassReportTestCharacterMeshScope Scope(Mesh.Get());
	if (!TestNotNull(TEXT("A Character carries a mesh component to put the mesh on"), Scope.Component))
	{
		return false;
	}

	const FCrowdyMassClassDerivationReport Report =
		FCrowdyMassClassDerivationReport::ForClass(ACharacter::StaticClass(), nullptr);

	TestTrue(TEXT("A class carrying a mesh is reported as resolving"), Report.bMeshResolved);
	TestEqual(TEXT("The mesh is the one on the class"), Report.MeshAssetPath, Mesh->GetPathName());
	TestEqual(TEXT("And the route it came from is the native one"),
		static_cast<int32>(Report.MeshRoute), static_cast<int32>(ECrowdyMassMeshRoute::NativeComponent));
	TestTrue(TEXT("A resolved class has nothing to explain"), Report.Reason.IsEmpty());

	// Compared against the capsule's own object type rather than a channel named here, so this pins that
	// the report carries the class's answer across and not that any particular engine default holds.
	const ACharacter* CharacterDefaults = GetDefault<ACharacter>();
	const UCapsuleComponent* Capsule = CharacterDefaults ? CharacterDefaults->GetCapsuleComponent() : nullptr;
	if (!TestNotNull(TEXT("A Character carries a capsule to read an object type from"), Capsule))
	{
		return false;
	}

	const FName ExpectedObjectType =
		UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(static_cast<int32>(Capsule->GetCollisionObjectType()));

	TestTrue(TEXT("A class with a capsule has its object type read from the class"), Report.bObjectTypeResolved);
	TestEqual(TEXT("And it is the capsule's own"), Report.ObjectTypeName.ToString(), ExpectedObjectType.ToString());

	// The one line is what a console command prints, so it has to carry the parts separately checked
	// above rather than being a summary that drops one of them.
	const FString Line = Report.ToLine();
	TestTrue(TEXT("The line names the class"), Line.Contains(ACharacter::StaticClass()->GetPathName()));
	TestTrue(TEXT("The line names the mesh"), Line.Contains(Mesh->GetName()));
	TestTrue(TEXT("The line names the object type"), Line.Contains(ExpectedObjectType.ToString()));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassClassDerivationReportMissingMeshTest,
	"CrowdySDK.Mass.ClassDerivationReportSaysWhyAClassWouldNotDerive", CrowdyMassClassDerivationReportTestFlags)
bool FCrowdyMassClassDerivationReportMissingMeshTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassClassDerivationReport Report =
		FCrowdyMassClassDerivationReport::ForClass(AActor::StaticClass(), nullptr);

	TestFalse(TEXT("A class with no mesh anywhere is reported as not resolving"), Report.bMeshResolved);
	TestTrue(TEXT("The reason names the class"), Report.Reason.Contains(AActor::StaticClass()->GetPathName()));
	TestTrue(TEXT("The reason says what to add"), Report.Reason.Contains(TEXT("Skeletal Mesh Component")));

	// A failing report whose line reads like a successful one is worse than no line: the whole point is
	// that a person can tell the two apart at a glance in a log a crowd is already filling.
	const FString Line = Report.ToLine();
	TestTrue(TEXT("The line says outright that there is no mesh"), Line.Contains(TEXT("NO mesh")));
	TestTrue(TEXT("And carries the reason with it"), Line.Contains(TEXT("Skeletal Mesh Component")));

	// An object type is still reported, because a derived entity would still get one; what must not
	// happen is it being presented as something the class said.
	TestFalse(TEXT("Nothing on this class describes a collision shape"), Report.bObjectTypeResolved);
	TestTrue(TEXT("So the object type says it is a default rather than the class's own"),
		Report.ObjectTypeSourceDescription.Contains(TEXT("default")));

	return true;
}

// Without a world there is no subsystem to ask, and "nothing is authored for this class" is the single
// most misleading thing this report could say wrongly: it is the exact condition under which deriving
// happens at all. So the unanswerable case has to be visibly unanswered.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassClassDerivationReportNoWorldTest,
	"CrowdySDK.Mass.ClassDerivationReportLeavesTheConfigQuestionOpenWithNoWorld", CrowdyMassClassDerivationReportTestFlags)
bool FCrowdyMassClassDerivationReportNoWorldTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassClassDerivationReport Report =
		FCrowdyMassClassDerivationReport::ForClass(AActor::StaticClass(), nullptr);

	TestFalse(TEXT("Nothing is claimed to be authored when nothing was asked"), Report.bAuthoredConfigWins);
	TestTrue(TEXT("And the description says the question went unanswered"),
		Report.ConfigSourceDescription.Contains(TEXT("unknown")));
	TestTrue(TEXT("The line carries that caveat rather than dropping it"),
		Report.ToLine().Contains(TEXT("unknown")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassClassDerivationReportResolveClassTest,
	"CrowdySDK.Mass.ClassDerivationReportResolvesAClassAPersonTyped", CrowdyMassClassDerivationReportTestFlags)
bool FCrowdyMassClassDerivationReportResolveClassTest::RunTest(const FString& Parameters)
{
	FString Why;

	TestSamePtr(TEXT("A full object path resolves"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(TEXT("/Script/Engine.Character"), Why),
		ACharacter::StaticClass());

	// The form everyone actually types. A native class is reflected without its type prefix, so this only
	// works if the prefix is stripped and retried rather than the name being taken literally.
	TestSamePtr(TEXT("A native class typed with its type prefix resolves"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(TEXT("ACharacter"), Why),
		ACharacter::StaticClass());

	TestSamePtr(TEXT("And typed the way it is reflected"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(TEXT("Character"), Why),
		ACharacter::StaticClass());

	// Surrounding space is what a paste leaves behind and is not part of anybody's intent.
	TestSamePtr(TEXT("Surrounding whitespace is not part of the name"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(TEXT("  ACharacter  "), Why),
		ACharacter::StaticClass());

	TestNull(TEXT("An empty name resolves to nothing"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(FString(), Why));
	TestFalse(TEXT("And says so"), Why.IsEmpty());

	TestNull(TEXT("A name nothing answers to resolves to nothing"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(TEXT("CrowdyMassNoSuchClassAnywhereZZZ"), Why));
	TestTrue(TEXT("And the reason repeats what was asked for, so a typo is visible"),
		Why.Contains(TEXT("CrowdyMassNoSuchClassAnywhereZZZ")));

	TestNull(TEXT("A path nothing answers to resolves to nothing"),
		FCrowdyMassClassDerivationReport::ResolveActorClass(TEXT("/Game/CrowdyMassNoSuchPathZZZ"), Why));
	TestFalse(TEXT("And says which paths were tried"), Why.IsEmpty());

	return true;
}

/**
 * The report answers whether the class would look right, not only whether it would appear.
 *
 * A material missing the instanced skinned usage flag is the one thing about a crowd that every other
 * question here is blind to: the entity spawns, moves and is hittable, and renders as the default
 * material. This is also the surface a person comes back to after the log has said its piece once, so
 * the report has to answer in full however often it is asked and must never mark anything as said.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassClassDerivationReportMaterialsTest,
	"CrowdySDK.Mass.ClassDerivationReportCarriesTheMaterialVerdict", CrowdyMassClassDerivationReportTestFlags)
bool FCrowdyMassClassDerivationReportMaterialsTest::RunTest(const FString& Parameters)
{
	FCrowdyMassMaterialUsageReports::ResetForTest();
	ON_SCOPE_EXIT
	{
		FCrowdyMassMaterialUsageReports::ResetForTest();
	};

	UMaterial* Unflagged = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Unflagged->MaterialDomain = MD_Surface;
	Unflagged->SetUsageByFlag(MATUSAGE_InstancedSkinnedMesh, false);
	const TStrongObjectPtr<UMaterial> UnflaggedHolder(Unflagged);

	TArray<FSkeletalMaterial> Slots;
	Slots.Add(FSkeletalMaterial(Unflagged, FName(TEXT("Body"))));

	const TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient));
	Mesh->SetMaterials(Slots);

	const FCrowdyMassReportTestCharacterMeshScope Scope(Mesh.Get());
	if (!TestNotNull(TEXT("A Character carries a mesh component to put the mesh on"), Scope.Component))
	{
		return false;
	}

	const FCrowdyMassClassDerivationReport Report =
		FCrowdyMassClassDerivationReport::ForClass(ACharacter::StaticClass(), nullptr);

	if (!TestEqual(TEXT("The report carries the slot that would not draw"), Report.Materials.Unflagged.Num(), 1))
	{
		return false;
	}

	TestEqual(TEXT("And names the material the flag goes on"),
		Report.Materials.Unflagged[0].FlagOwnerPath, Unflagged->GetPathName());

	// The one line is what the console command prints, so the verdict has to survive into it rather than
	// only into the struct behind it.
	const FString Line = Report.ToLine();
	TestTrue(TEXT("The line carries the material verdict"), Line.Contains(TEXT("Materials:")));
	TestTrue(TEXT("It says outright that a slot is not flagged"), Line.Contains(TEXT("NOT flagged")));
	TestTrue(TEXT("And names the material, so the line is actionable on its own"),
		Line.Contains(*Unflagged->GetPathName()));

	// Asking is not reporting. If the report marked what it saw, the first person to run the command
	// would silence the warning for everybody who came after them.
	FCrowdyMassClassDerivationReport::ForClass(ACharacter::StaticClass(), nullptr);

	TArray<FString> Messages;
	FCrowdyMassMaterialUsageReports::TakeUnreported(Report.Materials, Mesh.Get(), ACharacter::StaticClass(), Messages);
	TestEqual(TEXT("The warning is still owed after the report has been asked twice"), Messages.Num(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
