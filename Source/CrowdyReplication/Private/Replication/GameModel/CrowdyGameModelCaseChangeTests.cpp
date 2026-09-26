#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

// WITH_METADATA as well as the test flag: the entity cases apply through the test target's CrowdyModel metadata,
// which a game target does not carry.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h" // FCrowdyMutationApplied

namespace
{
	constexpr EAutomationTestFlags CrowdyCaseChangeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FJsonObject> CaseChangeTitleState(const FString& Title)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("title"), Title);
		return Object;
	}

	TArray<FCrowdyMutationApplied> CaseChangeTitleMutation(const FString& Title)
	{
		FCrowdyMutationApplied Mutation;
		Mutation.Key = TEXT("title");
		Mutation.NewValueJson = FString::Printf(TEXT("\"%s\""), *Title);
		return { Mutation };
	}
}

// FString::operator== folds case, so a string attribute that changes only in letter case must still read as changed
// on a pull: the member is written, the change delegate fires, and the cache takes the new spelling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelApplyStateCaseOnlyChangeTest,
	"CrowdySDK.GameModel.ApplyStateCaseOnlyChange", CrowdyCaseChangeTestFlags)
bool FCrowdyGameModelApplyStateCaseOnlyChangeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Container = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Observer = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("case-state"));
	Model->ApplyStateToContainer(NetID, Container, CaseChangeTitleState(TEXT("alice")));
	TestEqual(TEXT("first spelling broadcasts"), Observer->AttributeChangedCount, 1);

	Model->ApplyStateToContainer(NetID, Container, CaseChangeTitleState(TEXT("Alice")));
	TestEqual(TEXT("a case-only change broadcasts"), Observer->AttributeChangedCount, 2);
	TestTrue(TEXT("and carries the old spelling"), Observer->LastAttrOldJson.Equals(TEXT("\"alice\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and the new spelling"), Observer->LastAttrNewJson.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the member takes the new spelling"), Container->Title.Equals(TEXT("Alice"), ESearchCase::CaseSensitive));
	FString Cached;
	Model->TryGetCachedValueJson(NetID, FName(TEXT("title")), Cached);
	TestTrue(TEXT("the cache takes the new spelling"), Cached.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));

	Model->ApplyStateToContainer(NetID, Container, CaseChangeTitleState(TEXT("Alice")));
	TestEqual(TEXT("an identical re-pull is still silent"), Observer->AttributeChangedCount, 2);
	return true;
}

// The confirmed-invoke echo path applies a case-only string change the same way the pull path does.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelApplyMutationsCaseOnlyChangeTest,
	"CrowdySDK.GameModel.ApplyMutationsCaseOnlyChange", CrowdyCaseChangeTestFlags)
bool FCrowdyGameModelApplyMutationsCaseOnlyChangeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Container = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Observer = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("case-mutation"));
	Model->ApplyMutationsToContainer(NetID, Container, CaseChangeTitleMutation(TEXT("alice")));
	TestEqual(TEXT("first spelling broadcasts"), Observer->AttributeChangedCount, 1);

	Model->ApplyMutationsToContainer(NetID, Container, CaseChangeTitleMutation(TEXT("Alice")));
	TestEqual(TEXT("a case-only change broadcasts"), Observer->AttributeChangedCount, 2);
	TestTrue(TEXT("and carries the old spelling"), Observer->LastAttrOldJson.Equals(TEXT("\"alice\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and the new spelling"), Observer->LastAttrNewJson.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the member takes the new spelling"), Container->Title.Equals(TEXT("Alice"), ESearchCase::CaseSensitive));
	FString Cached;
	Model->TryGetCachedValueJson(NetID, FName(TEXT("title")), Cached);
	TestTrue(TEXT("the cache takes the new spelling"), Cached.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));

	Model->ApplyMutationsToContainer(NetID, Container, CaseChangeTitleMutation(TEXT("Alice")));
	TestEqual(TEXT("an identical echo is still silent"), Observer->AttributeChangedCount, 2);
	return true;
}

// A free/data container's pulled state treats a case-only string change as a change too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDataContainerStateCaseOnlyChangeTest,
	"CrowdySDK.GameModel.DataContainerStateCaseOnlyChange", CrowdyCaseChangeTestFlags)
bool FCrowdyGameModelDataContainerStateCaseOnlyChangeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Observer = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	Model->OnDataContainerChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleDataContainerChanged);
	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FString ContainerId = TEXT("case-data-state");
	Model->ApplyDataContainerState(ContainerId, CaseChangeTitleState(TEXT("alice")));
	TestEqual(TEXT("first spelling fires the container delegate"), Observer->DataChangedCount, 1);

	Model->ApplyDataContainerState(ContainerId, CaseChangeTitleState(TEXT("Alice")));
	TestEqual(TEXT("a case-only change fires the container delegate"), Observer->DataChangedCount, 2);
	TestEqual(TEXT("and the attribute delegate"), Observer->AttributeChangedCount, 2);
	TestTrue(TEXT("with the new spelling"), Observer->LastAttrNewJson.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));
	FString Cached;
	Model->TryGetContainerValueJson(ContainerId, FName(TEXT("title")), Cached);
	TestTrue(TEXT("the cache takes the new spelling"), Cached.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));

	Model->ApplyDataContainerState(ContainerId, CaseChangeTitleState(TEXT("Alice")));
	TestEqual(TEXT("an identical re-pull is still silent"), Observer->DataChangedCount, 2);
	return true;
}

// A free/data container's confirmed-invoke merge treats a case-only string change as a change too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDataContainerMutationsCaseOnlyChangeTest,
	"CrowdySDK.GameModel.DataContainerMutationsCaseOnlyChange", CrowdyCaseChangeTestFlags)
bool FCrowdyGameModelDataContainerMutationsCaseOnlyChangeTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Observer = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	Model->OnDataContainerChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleDataContainerChanged);
	Model->OnModelAttributeChanged.AddDynamic(Observer, &UCrowdyGameModelTestTarget::HandleModelAttributeChanged);

	const FString ContainerId = TEXT("case-data-mutation");
	Model->ApplyDataContainerMutations(ContainerId, CaseChangeTitleMutation(TEXT("alice")));
	TestEqual(TEXT("first spelling fires the container delegate"), Observer->DataChangedCount, 1);

	Model->ApplyDataContainerMutations(ContainerId, CaseChangeTitleMutation(TEXT("Alice")));
	TestEqual(TEXT("a case-only change fires the container delegate"), Observer->DataChangedCount, 2);
	TestEqual(TEXT("and the attribute delegate"), Observer->AttributeChangedCount, 2);
	TestTrue(TEXT("with the new spelling"), Observer->LastAttrNewJson.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));
	FString Cached;
	Model->TryGetContainerValueJson(ContainerId, FName(TEXT("title")), Cached);
	TestTrue(TEXT("the cache takes the new spelling"), Cached.Equals(TEXT("\"Alice\""), ESearchCase::CaseSensitive));

	Model->ApplyDataContainerMutations(ContainerId, CaseChangeTitleMutation(TEXT("Alice")));
	TestEqual(TEXT("an identical echo is still silent"), Observer->DataChangedCount, 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
