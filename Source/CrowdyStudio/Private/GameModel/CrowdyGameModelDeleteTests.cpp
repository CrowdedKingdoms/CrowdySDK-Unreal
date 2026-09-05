// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "GameModel/CrowdyGameModelDelete.h"
#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelSnapshot.h"
#include "GameModel/CrowdySchemaSync.h"
#include "Model/CrowdyStudioTypes.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelDeleteTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every fixture here carries the DeleteTest prefix. Adaptive unity merges this module's .cpp files into shared
	// translation units, so two anonymous-namespace helpers of one name in two files redefine each other.

	TSharedPtr<FStudioContainerType> DeleteTestType(const FString& TypeName, const FString& DisplayName = FString())
	{
		TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
		Type->TypeName = TypeName;
		Type->DisplayName = DisplayName;
		return Type;
	}

	TSharedPtr<FStudioPropertyDef> DeleteTestAttribute(const FString& TypeName, const FString& Key)
	{
		TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
		Def->ContainerTypeName = TypeName;
		Def->Key = Key;
		Def->ValueType = TEXT("int");
		return Def;
	}

	TSharedPtr<FStudioFunction> DeleteTestFunction(
		const FString& TypeName, const FString& Name, const FString& ReturnExpression = FString())
	{
		TSharedPtr<FStudioFunction> Function = MakeShared<FStudioFunction>();
		Function->ContainerTypeName = TypeName;
		Function->Name = Name;
		Function->ReturnExpression = ReturnExpression;
		return Function;
	}

	TSharedPtr<FStudioFunction> DeleteTestWritingFunction(
		const FString& TypeName, const FString& Name, const FString& Target, const FString& Property,
		const FString& Expression)
	{
		TSharedPtr<FStudioFunction> Function = DeleteTestFunction(TypeName, Name);
		FStudioFunctionMutation Mutation;
		Mutation.Target = Target;
		Mutation.Property = Property;
		Mutation.Expression = Expression;
		Function->Mutations.Add(MoveTemp(Mutation));
		return Function;
	}

	TSharedPtr<FStudioAutomation> DeleteTestAutomation(
		const FString& Name, const FString& TargetTypeName = FString(), const FString& FunctionName = FString())
	{
		TSharedPtr<FStudioAutomation> Automation = MakeShared<FStudioAutomation>();
		Automation->Name = Name;
		Automation->TargetTypeName = TargetTypeName;
		Automation->FunctionName = FunctionName;
		Automation->TriggerType = TEXT("schedule");
		Automation->ScheduleKind = TEXT("interval");
		Automation->IntervalMs = 60000;
		return Automation;
	}

	TSharedPtr<FStudioAutomationTrigger> DeleteTestTrigger(
		const FString& AutomationName, const FString& OnEvent, const FString& ContainerTypeName = FString(),
		const FString& FunctionName = FString())
	{
		TSharedPtr<FStudioAutomationTrigger> Trigger = MakeShared<FStudioAutomationTrigger>();
		Trigger->AutomationName = AutomationName;
		Trigger->OnEvent = OnEvent;
		Trigger->ContainerTypeName = ContainerTypeName;
		Trigger->FunctionName = FunctionName;
		return Trigger;
	}

	// Evidence whose three lists have all been read, which is the state a review opens in. Tests that want an unread
	// list clear the flag by hand, because an empty array and an unread one must never read the same.
	FCrowdyDeleteEvidence DeleteTestEvidence(int64 AppId = 7)
	{
		FCrowdyDeleteEvidence Evidence;
		Evidence.AppId = AppId;
		Evidence.bTypesRead = true;
		Evidence.bFunctionsRead = true;
		Evidence.bAutomationsRead = true;
		return Evidence;
	}

	void DeleteTestSetLiveCount(
		FCrowdyDeleteEvidence& Evidence, const FString& TypeName, ECrowdyLiveCountState State, int32 Count,
		const TArray<FString>& SampleIds = {})
	{
		FCrowdyDeleteLiveCount Entry;
		Entry.TypeName = TypeName;
		Entry.State = State;
		Entry.Count = Count;
		Entry.SampleIds = SampleIds;
		Evidence.LiveCounts.Add(MoveTemp(Entry));
	}

	void DeleteTestSetAttributes(
		FCrowdyDeleteEvidence& Evidence, const FString& TypeName, const TArray<FString>& Keys)
	{
		FCrowdyDeleteModelAttributes Entry;
		Entry.TypeName = TypeName;
		for (const FString& Key : Keys)
		{
			Entry.Attributes.Add(DeleteTestAttribute(TypeName, Key));
		}
		Evidence.AttributesByModel.Add(MoveTemp(Entry));
	}

	// One model declared in code, as a finished plan would have captured it.
	void DeleteTestDeclareType(
		FCrowdyModelSnapshot& Snapshot, const FString& TypeName, const TArray<FString>& AttributeKeys,
		const FString& ClassPath = TEXT("/Game/Models/BP_Hero.BP_Hero_C"))
	{
		FCrowdyModelSnapshotType Type;
		Type.TypeName = TypeName;
		Type.DisplayName = TypeName;
		Type.OwningClassPath = ClassPath;
		for (const FString& Key : AttributeKeys)
		{
			Type.Attributes.Add({ Key, TEXT("int") });
		}
		Snapshot.Types.Add(MoveTemp(Type));
	}

	void DeleteTestDeclareFunction(
		FCrowdyModelSnapshot& Snapshot, const FString& TypeName, const FString& Name,
		const FString& AssetPath = TEXT("/Game/Effects/FX_Damage.FX_Damage"))
	{
		FCrowdyModelSnapshotFunction Function;
		Function.TypeName = TypeName;
		Function.Name = Name;
		Function.ReturnType = TEXT("int");
		Function.AssetPath = AssetPath;
		Snapshot.Functions.Add(MoveTemp(Function));
		Snapshot.FunctionAuthors.Add({ TypeName, Name, AssetPath });
	}

	void DeleteTestDeclareAutomation(
		FCrowdyModelSnapshot& Snapshot, const FString& Name,
		const FString& AssetPath = TEXT("/Game/Effects/FX_Regen.FX_Regen"))
	{
		FCrowdyModelSnapshotAutomation Automation;
		Automation.Name = Name;
		Automation.AssetPath = AssetPath;
		Snapshot.Automations.Add(MoveTemp(Automation));
		Snapshot.AutomationAuthors.Add({ FString(), Name, AssetPath });
	}

	FString DeleteTestScopedKey(const FString& TypeName, const FString& Name)
	{
		return FCrowdySchemaSync::ScopedNameKey(TypeName, Name);
	}

	// The { "data": { <field>: bool } } shape a delete mutation answers with.
	TSharedPtr<FJsonObject> DeleteTestReply(const FString& Field, bool bValue)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(Field, bValue);
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetObjectField(TEXT("data"), Data);
		return Envelope;
	}

	TSharedPtr<FJsonObject> DeleteTestEmptyDataReply()
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
		return Envelope;
	}

	const FCrowdyDeleteFinding* DeleteTestFindFinding(
		const TArray<FCrowdyDeleteFinding>& Findings, ECrowdyDeleteFindingKind Kind)
	{
		for (const FCrowdyDeleteFinding& Finding : Findings)
		{
			if (Finding.Kind == Kind)
			{
				return &Finding;
			}
		}
		return nullptr;
	}

	int32 DeleteTestCountFindings(const TArray<FCrowdyDeleteFinding>& Findings, ECrowdyDeleteFindingKind Kind)
	{
		int32 Count = 0;
		for (const FCrowdyDeleteFinding& Finding : Findings)
		{
			if (Finding.Kind == Kind)
			{
				++Count;
			}
		}
		return Count;
	}

	// Where an operation carrying this argument sits in the walk, or INDEX_NONE when the plan issues none.
	int32 DeleteTestIndexOfArg(const TArray<FCrowdyDeleteOp>& Ops, const FString& ArgKey, const FString& ArgValue)
	{
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			for (const TPair<FString, FString>& Arg : Ops[Index].StringArgs)
			{
				if (Arg.Key.Equals(ArgKey, ESearchCase::CaseSensitive)
					&& Arg.Value.Equals(ArgValue, ESearchCase::CaseSensitive))
				{
					return Index;
				}
			}
		}
		return INDEX_NONE;
	}

	const FCrowdyDeleteOp* DeleteTestFindOp(const TArray<FCrowdyDeleteOp>& Ops, ECrowdyDeleteKind Kind, const FString& Name)
	{
		for (const FCrowdyDeleteOp& Op : Ops)
		{
			if (Op.Kind == Kind && Op.Subject.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return &Op;
			}
		}
		return nullptr;
	}

	bool DeleteTestIsWordChar(TCHAR Character)
	{
		return FChar::IsAlnum(Character) || Character == TEXT('_');
	}

	// Whether Text uses Word as a whole word. Needed because several of the nouns this page must not use are
	// substrings of words it legitimately does use, "edge" inside "acknowledge" among them.
	bool DeleteTestNamesWord(const FString& Text, const FString& Word)
	{
		const FString Haystack = Text.ToLower();
		const FString Needle = Word.ToLower();
		int32 SearchFrom = 0;
		while (SearchFrom <= Haystack.Len() - Needle.Len())
		{
			const int32 Found = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Found == INDEX_NONE)
			{
				return false;
			}
			const int32 After = Found + Needle.Len();
			const bool bStarts = Found == 0 || !DeleteTestIsWordChar(Haystack[Found - 1]);
			const bool bEnds = After >= Haystack.Len() || !DeleteTestIsWordChar(Haystack[After]);
			if (bStarts && bEnds)
			{
				return true;
			}
			SearchFrom = Found + 1;
		}
		return false;
	}

	// The raw server nouns this page is not allowed to show. They belong to the Advanced tab, which exists to show
	// them; a reader here is looking at models, attributes and live models.
	bool DeleteTestUsesARawServerNoun(const FString& Text)
	{
		static const TArray<FString> Banned = {
			TEXT("container"), TEXT("containers"), TEXT("invoke"), TEXT("edge"), TEXT("edges"),
			TEXT("traverse"), TEXT("digest")
		};
		for (const FString& Word : Banned)
		{
			if (DeleteTestNamesWord(Text, Word))
			{
				return true;
			}
		}
		return false;
	}

	FCrowdyModelRow DeleteTestRow(
		ECrowdyModelRowKind Kind, const FString& OwningType, const FString& Name, ECrowdyModelProvenance Provenance)
	{
		FCrowdyModelRow Row;
		Row.Kind = Kind;
		Row.OwningType = OwningType;
		Row.Name = Name;
		Row.Primary = Name;
		Row.Provenance = Provenance;
		return Row;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteReplyFalseIsAlreadyGoneTest,
	"CrowdySDK.CrowdyStudio.DeleteReplyFalseIsAlreadyGone", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteReplyFalseIsAlreadyGoneTest::RunTest(const FString& Parameters)
{
	// A false boolean means the entity was not there. That is an idempotent no-op and a success: reading it as a
	// failure stops a walk on the one operation that needed no work, which is exactly what a second press after a
	// partial commit produces on everything the first press already finished. The job could then never be finished.
	const FString Field = TEXT("gameModelDeleteFunction");

	TestTrue(TEXT("A true reply says the entity existed and is gone"),
		CrowdyGameModelDelete::ReadDeleteReply(DeleteTestReply(Field, true), Field) == ECrowdyDeleteReply::Removed);
	TestTrue(TEXT("A false reply says it was not there"),
		CrowdyGameModelDelete::ReadDeleteReply(DeleteTestReply(Field, false), Field) == ECrowdyDeleteReply::AlreadyGone);

	TestTrue(TEXT("Removed lets the walk carry on"),
		CrowdyGameModelDelete::IsDeleteReplySuccess(ECrowdyDeleteReply::Removed));
	TestTrue(TEXT("And so does an entity that was already gone"),
		CrowdyGameModelDelete::IsDeleteReplySuccess(ECrowdyDeleteReply::AlreadyGone));

	// A walk reads the reply and then decides. Both halves are asserted because getting either one wrong produces
	// the same symptom: a second press after a partial commit stops on the first operation the first press
	// already finished, and the job can never be finished.
	TestTrue(TEXT("A false reply reaches the walk as a completion"),
		CrowdyGameModelDelete::IsDeleteReplySuccess(
			CrowdyGameModelDelete::ReadDeleteReply(DeleteTestReply(Field, false), Field)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteReplyUnrecognizedIsNotSuccessTest,
	"CrowdySDK.CrowdyStudio.DeleteReplyUnrecognizedIsNotSuccess", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteReplyUnrecognizedIsNotSuccessTest::RunTest(const FString& Parameters)
{
	const FString Field = TEXT("gameModelDeleteContainerType");

	TestTrue(TEXT("A reply with no data object says nothing about the entity"),
		CrowdyGameModelDelete::ReadDeleteReply(MakeShared<FJsonObject>(), Field) == ECrowdyDeleteReply::Unrecognized);
	TestTrue(TEXT("Nor does a data object without the field the operation declares"),
		CrowdyGameModelDelete::ReadDeleteReply(DeleteTestEmptyDataReply(), Field) == ECrowdyDeleteReply::Unrecognized);
	TestTrue(TEXT("Nor does no envelope at all"),
		CrowdyGameModelDelete::ReadDeleteReply(TSharedPtr<FJsonObject>(), Field) == ECrowdyDeleteReply::Unrecognized);
	TestTrue(TEXT("Nor does asking for no field"),
		CrowdyGameModelDelete::ReadDeleteReply(DeleteTestReply(Field, true), FString()) == ECrowdyDeleteReply::Unrecognized);

	// A different operation's field in the same envelope is the mis-typed-field case, and it must not read as a
	// delete that happened.
	TestTrue(TEXT("A field the reply does not carry is not a delete"),
		CrowdyGameModelDelete::ReadDeleteReply(DeleteTestReply(Field, true), TEXT("gameModelDeleteFunction"))
			== ECrowdyDeleteReply::Unrecognized);

	TestFalse(TEXT("An unrecognized reply stops the walk"),
		CrowdyGameModelDelete::IsDeleteReplySuccess(ECrowdyDeleteReply::Unrecognized));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteCommitOrderIsThePruneOrderTest,
	"CrowdySDK.CrowdyStudio.DeleteCommitOrderIsThePruneOrder", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteCommitOrderIsThePruneOrderTest::RunTest(const FString& Parameters)
{
	const TArray<ECrowdyDeleteKind>& Order = CrowdyGameModelDelete::CommitOrder();

	const TArray<ECrowdyDeleteKind> Expected = {
		ECrowdyDeleteKind::Automation,
		ECrowdyDeleteKind::LiveInstance,
		ECrowdyDeleteKind::Attribute,
		ECrowdyDeleteKind::Function,
		ECrowdyDeleteKind::Model
	};

	TestEqual(TEXT("Every kind has a slot and none has two"), Order.Num(), Expected.Num());
	if (Order.Num() == Expected.Num())
	{
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			TestTrue(*FString::Printf(TEXT("Slot %d is the one the server's refusals force"), Index),
				Order[Index] == Expected[Index]);
		}
	}

	// The schema prune already runs automations, then attributes, then functions, then models, and every one of
	// those positions is there because the server refuses the next one otherwise. Take the live models back out and
	// what is left has to be that order, byte for byte.
	TArray<ECrowdyDeleteKind> WithoutLiveModels = Order;
	WithoutLiveModels.Remove(ECrowdyDeleteKind::LiveInstance);

	const TArray<ECrowdyDeleteKind> PruneOrder = {
		ECrowdyDeleteKind::Automation,
		ECrowdyDeleteKind::Attribute,
		ECrowdyDeleteKind::Function,
		ECrowdyDeleteKind::Model
	};

	TestEqual(TEXT("Only the live models were inserted"), WithoutLiveModels.Num(), PruneOrder.Num());
	if (WithoutLiveModels.Num() == PruneOrder.Num())
	{
		for (int32 Index = 0; Index < PruneOrder.Num(); ++Index)
		{
			TestTrue(*FString::Printf(TEXT("Prune slot %d is unchanged"), Index),
				WithoutLiveModels[Index] == PruneOrder[Index]);
		}
	}

	// Live models go before attributes, not after, so everything a model delete is refused over is cleared before
	// anything else touches that model.
	TestTrue(TEXT("Live models are cleared before anything else touches the model"),
		Order.IndexOfByKey(ECrowdyDeleteKind::LiveInstance) < Order.IndexOfByKey(ECrowdyDeleteKind::Attribute));
	TestTrue(TEXT("A bound function goes before the model it is bound to"),
		Order.IndexOfByKey(ECrowdyDeleteKind::Function) < Order.IndexOfByKey(ECrowdyDeleteKind::Model));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteOpsFollowTheCommitOrderTest,
	"CrowdySDK.CrowdyStudio.DeleteOpsFollowTheCommitOrder", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteOpsFollowTheCommitOrderTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Ghost")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));

	// Ticked in the reverse of the order they have to run in, so an implementation that kept the caller's order
	// would issue the model delete first and be refused by the server.
	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Ghost"), TEXT("Ghost")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health")));
	Marks.Add(CrowdyGameModelDelete::MarkLiveModel(TEXT("Hero"), TEXT("c-1"), TEXT("c-1")));
	Marks.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));

	const TArray<FCrowdyDeleteOp> Ops = CrowdyGameModelDelete::BuildOps(Marks, Evidence);
	TestEqual(TEXT("Every marked entity gets exactly one operation"), Ops.Num(), 5);
	if (Ops.Num() == 5)
	{
		const TArray<ECrowdyDeleteKind>& Order = CrowdyGameModelDelete::CommitOrder();
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			TestTrue(*FString::Printf(TEXT("Operation %d runs in the order the server forces"), Index),
				Ops[Index].Kind == Order[Index]);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteOpForEachKindTest,
	"CrowdySDK.CrowdyStudio.DeleteOpForEachKind", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteOpForEachKindTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));

	auto SingleOp = [&Evidence](const FCrowdyDeleteMark& Mark)
	{
		TArray<FCrowdyDeleteMark> Marks;
		Marks.Add(Mark);
		return CrowdyGameModelDelete::BuildOps(Marks, Evidence);
	};

	const TArray<FCrowdyDeleteOp> ModelOps =
		SingleOp(CrowdyGameModelDelete::MarkModel(TEXT("Ghost"), TEXT("Ghost")));
	TestEqual(TEXT("A marked model is one operation"), ModelOps.Num(), 1);
	if (ModelOps.Num() == 1)
	{
		TestEqual(TEXT("It deletes a container type"), ModelOps[0].OperationName, FString(TEXT("GameModelDeleteContainerType")));
		TestEqual(TEXT("And reads its answer from that field"), ModelOps[0].ResultField, FString(TEXT("gameModelDeleteContainerType")));
		TestEqual(TEXT("It names the type"), DeleteTestIndexOfArg(ModelOps, TEXT("typeName"), TEXT("Ghost")), 0);
		TestEqual(TEXT("It is phrased in the words of this page"), ModelOps[0].Describe, FString(TEXT("the model Ghost")));
	}

	const TArray<FCrowdyDeleteOp> AttributeOps =
		SingleOp(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health")));
	TestEqual(TEXT("A marked attribute is one operation"), AttributeOps.Num(), 1);
	if (AttributeOps.Num() == 1)
	{
		TestEqual(TEXT("It deletes a property definition"), AttributeOps[0].OperationName, FString(TEXT("GameModelDeletePropertyDef")));
		TestEqual(TEXT("An attribute key is only unique inside its model, so the model is an argument"),
			AttributeOps[0].StringArgs.Num(), 2);
		TestEqual(TEXT("It names the model"),
			DeleteTestIndexOfArg(AttributeOps, TEXT("containerTypeName"), TEXT("Hero")), 0);
		TestEqual(TEXT("And the key"), DeleteTestIndexOfArg(AttributeOps, TEXT("key"), TEXT("Health")), 0);
		TestEqual(TEXT("And the line says which model"), AttributeOps[0].Describe,
			FString(TEXT("the attribute Health on Hero")));
	}

	const TArray<FCrowdyDeleteOp> FunctionOps =
		SingleOp(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	TestEqual(TEXT("A marked function is one operation"), FunctionOps.Num(), 1);
	if (FunctionOps.Num() == 1)
	{
		TestEqual(TEXT("It deletes a function"), FunctionOps[0].OperationName, FString(TEXT("GameModelDeleteFunction")));
		// The mutation takes the name alone. The model rides on the mark so nothing downstream has to guess it.
		TestEqual(TEXT("The name is the only argument"), FunctionOps[0].StringArgs.Num(), 1);
		TestEqual(TEXT("And it is the function's"), DeleteTestIndexOfArg(FunctionOps, TEXT("name"), TEXT("heal")), 0);
		TestEqual(TEXT("The mark still carries the model it came from"), FunctionOps[0].Subject.OwningType,
			FString(TEXT("Hero")));
		TestFalse(TEXT("One model carries this name, so nothing is ambiguous"), FunctionOps[0].bNameNotUniqueInApp);
	}

	const TArray<FCrowdyDeleteOp> AutomationOps =
		SingleOp(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));
	TestEqual(TEXT("A marked automation is one operation"), AutomationOps.Num(), 1);
	if (AutomationOps.Num() == 1)
	{
		TestEqual(TEXT("It deletes an automation"), AutomationOps[0].OperationName, FString(TEXT("GameModelDeleteAutomation")));
		TestEqual(TEXT("An automation name is unique app-wide, so it needs no scope"), AutomationOps[0].StringArgs.Num(), 1);
		TestEqual(TEXT("It names the automation"), DeleteTestIndexOfArg(AutomationOps, TEXT("name"), TEXT("tick")), 0);
		TestTrue(TEXT("And carries no model"), AutomationOps[0].Subject.OwningType.IsEmpty());
	}

	const TArray<FCrowdyDeleteOp> LiveOps =
		SingleOp(CrowdyGameModelDelete::MarkLiveModel(TEXT("Hero"), TEXT("c-1"), TEXT("c-1")));
	TestEqual(TEXT("A marked live model is one operation"), LiveOps.Num(), 1);
	if (LiveOps.Num() == 1)
	{
		TestEqual(TEXT("It deletes the live instance"), LiveOps[0].OperationName, FString(TEXT("GameModelDeleteContainer")));
		TestEqual(TEXT("By its id"), DeleteTestIndexOfArg(LiveOps, TEXT("containerId"), TEXT("c-1")), 0);
		TestEqual(TEXT("And says which model it is one of"), LiveOps[0].Describe,
			FString(TEXT("the live model c-1 of Hero")));
	}

	// An attribute with no model resolves to no entity at all, so it must produce nothing rather than a guess at
	// which model's key of that name to delete.
	TArray<FCrowdyDeleteMark> Unscoped;
	Unscoped.Add(CrowdyGameModelDelete::MarkAttribute(FString(), TEXT("Health"), TEXT("Health")));
	TestEqual(TEXT("An attribute with no model produces no operation"),
		CrowdyGameModelDelete::BuildOps(Unscoped, Evidence).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteRowKindMapsToOneDeleteKindTest,
	"CrowdySDK.CrowdyStudio.DeleteRowKindMapsToOneDeleteKind", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteRowKindMapsToOneDeleteKindTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelDelete;

	TestTrue(TEXT("A model row deletes a model"),
		DeleteKindForRowKind(ECrowdyModelRowKind::Model) == ECrowdyDeleteKind::Model);
	TestTrue(TEXT("An attribute row deletes an attribute"),
		DeleteKindForRowKind(ECrowdyModelRowKind::Attribute) == ECrowdyDeleteKind::Attribute);
	TestTrue(TEXT("A function row deletes a function"),
		DeleteKindForRowKind(ECrowdyModelRowKind::Function) == ECrowdyDeleteKind::Function);
	TestTrue(TEXT("An automation row deletes an automation"),
		DeleteKindForRowKind(ECrowdyModelRowKind::Automation) == ECrowdyDeleteKind::Automation);
	TestTrue(TEXT("A live row deletes a live model"),
		DeleteKindForRowKind(ECrowdyModelRowKind::LiveInstance) == ECrowdyDeleteKind::LiveInstance);

	// The row is handed over whole, so its fields are never re-typed between the widget and the mutation.
	const FCrowdyModelRow AttributeRow =
		DeleteTestRow(ECrowdyModelRowKind::Attribute, TEXT("Hero"), TEXT("Health"), ECrowdyModelProvenance::ServerOnly);
	const FCrowdyDeleteMark FromAttribute = MarkFromRow(AttributeRow);
	TestTrue(TEXT("An attribute row becomes an attribute mark"), FromAttribute.Kind == ECrowdyDeleteKind::Attribute);
	TestEqual(TEXT("Carrying the model that scopes it"), FromAttribute.OwningType, FString(TEXT("Hero")));
	TestEqual(TEXT("And the key verbatim"), FromAttribute.Name, FString(TEXT("Health")));

	const FCrowdyModelRow AutomationRow =
		DeleteTestRow(ECrowdyModelRowKind::Automation, TEXT("Hero"), TEXT("tick"), ECrowdyModelProvenance::ServerOnly);
	const FCrowdyDeleteMark FromAutomation = MarkFromRow(AutomationRow);
	TestTrue(TEXT("An automation name is unique app-wide, so its mark carries no model"),
		FromAutomation.OwningType.IsEmpty());

	FCrowdyModelSummary Summary;
	Summary.TypeName = TEXT("Ghost");
	Summary.Display = TEXT("Ghost");
	const FCrowdyDeleteMark FromModel = MarkFromModel(Summary);
	TestTrue(TEXT("A model in the list becomes a model mark"), FromModel.Kind == ECrowdyDeleteKind::Model);
	TestTrue(TEXT("A model scopes itself, so it carries no owner"), FromModel.OwningType.IsEmpty());
	TestEqual(TEXT("And it is its own owning model"), FromModel.OwningModelName(), FString(TEXT("Ghost")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteModelSubsumesItsAttributesTest,
	"CrowdySDK.CrowdyStudio.DeleteModelSubsumesItsAttributes", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteModelSubsumesItsAttributesTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("__crowdy_touch_hero")));
	DeleteTestSetAttributes(Evidence, TEXT("Hero"), { TEXT("Health") });
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	// Ticked by hand, and the plan implies the same delete for itself. Issuing it twice would report the second as
	// already gone for no reason.
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("__crowdy_touch_hero"), TEXT("__crowdy_touch_hero")));

	TestTrue(TEXT("An attribute of a marked model goes with the model"),
		CrowdyGameModelDelete::IsSubsumed(Marks[1], Marks));
	// A model delete is refused while a function is bound to it, so the function's own operation is what clears the
	// refusal. Folding it into the model would leave nothing to clear it with.
	TestFalse(TEXT("A function of a marked model is not folded into it"),
		CrowdyGameModelDelete::IsSubsumed(Marks[2], Marks));
	TestFalse(TEXT("Nor is the model itself"), CrowdyGameModelDelete::IsSubsumed(Marks[0], Marks));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	TestEqual(TEXT("The subsumed attribute is still accounted for"), Plan.SubsumedMarks.Num(), 1);
	TestEqual(TEXT("It issues no operation of its own"), Plan.CountOps(ECrowdyDeleteKind::Attribute), 0);
	TestEqual(TEXT("The two functions are one operation each, with no duplicate for the implied one"),
		Plan.CountOps(ECrowdyDeleteKind::Function), 2);
	TestEqual(TEXT("And the model is one"), Plan.CountOps(ECrowdyDeleteKind::Model), 1);
	TestEqual(TEXT("Three operations in all"), Plan.Ops.Num(), 3);

	const int32 HealIndex = DeleteTestIndexOfArg(Plan.Ops, TEXT("name"), TEXT("heal"));
	const int32 ModelIndex = DeleteTestIndexOfArg(Plan.Ops, TEXT("typeName"), TEXT("Hero"));
	TestTrue(TEXT("Both were found"), HealIndex != INDEX_NONE && ModelIndex != INDEX_NONE);
	TestTrue(TEXT("Every function goes before the model it is bound to"), HealIndex < ModelIndex);

	TestTrue(TEXT("The sheet says the subsumed entry is not counted twice"), !Plan.Sheet.SubsumedLine.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteLiveModelsAreOrderedFirstTest,
	"CrowdySDK.CrowdyStudio.DeleteLiveModelsAreOrderedFirst", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteLiveModelsAreOrderedFirstTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 1, { TEXT("c-1") });

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));
	Marks.Add(CrowdyGameModelDelete::MarkLiveModel(TEXT("Hero"), TEXT("c-1"), TEXT("c-1")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	// A live model is never folded into its model: the model delete is refused while it exists, so its own
	// operation has to run, and it has to run first.
	TestFalse(TEXT("A live model of a marked model is not folded into it"),
		CrowdyGameModelDelete::IsSubsumed(Marks[1], Marks));
	TestEqual(TEXT("Both operations are issued"), Plan.Ops.Num(), 2);
	if (Plan.Ops.Num() == 2)
	{
		TestTrue(TEXT("The live model goes first"), Plan.Ops[0].Kind == ECrowdyDeleteKind::LiveInstance);
		TestTrue(TEXT("The model goes last"), Plan.Ops[1].Kind == ECrowdyDeleteKind::Model);
	}

	// A live model this plan deletes FIRST is not what the server refuses over, exactly as a marked function is
	// not. Anything else makes the slot the live models occupy in the commit order unreachable: no plan holding a
	// model and its live models could ever be committed, so the ordering would exist for a case that cannot arise.
	TestTrue(TEXT("A live model this plan deletes first is no longer in the way"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelHasLiveModels) == nullptr);
	TestTrue(TEXT("So the plan can run"), Plan.bCommittable);

	// Marking SOME of them clears nothing: the ones left are still there when the model delete runs.
	FCrowdyDeleteEvidence Several = DeleteTestEvidence();
	Several.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Several, TEXT("Hero"), ECrowdyLiveCountState::Exact, 3,
		{ TEXT("c-1"), TEXT("c-2"), TEXT("c-3") });

	const FCrowdyDeletePlan Partial = CrowdyGameModelDelete::BuildPlan(Marks, Several);
	const FCrowdyDeleteFinding* StillBlocked =
		DeleteTestFindFinding(Partial.Findings, ECrowdyDeleteFindingKind::ModelHasLiveModels);
	TestTrue(TEXT("One of three marked leaves the refusal standing"), StillBlocked != nullptr);
	if (StillBlocked)
	{
		TestTrue(TEXT("And the count names only what is left"), StillBlocked->Headline.Contains(TEXT("2 live models")));
		TestFalse(TEXT("The one being deleted is not listed as what stops its own delete"),
			StillBlocked->References.Contains(TEXT("c-1")));
	}
	TestFalse(TEXT("So that plan cannot be committed"), Partial.bCommittable);

	// A count nobody took is still its own blocker: a mark cannot talk an unknown down to zero.
	FCrowdyDeleteEvidence Unprobed = DeleteTestEvidence();
	Unprobed.Types.Add(DeleteTestType(TEXT("Hero")));
	TestEqual(TEXT("An unknown count yields no number to subtract from"),
		CrowdyGameModelDelete::LiveModelsBlockingModelDelete(TEXT("Hero"), Marks, Unprobed), 0);
	TestTrue(TEXT("And the unknown itself still blocks"),
		DeleteTestFindFinding(CrowdyGameModelDelete::BuildPlan(Marks, Unprobed).Findings,
			ECrowdyDeleteFindingKind::ModelLiveCountUnknown) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteBlocksOnLiveModelsTest,
	"CrowdySDK.CrowdyStudio.DeleteBlocksOnLiveModels", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteBlocksOnLiveModelsTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 3,
		{ TEXT("c-1"), TEXT("c-2"), TEXT("c-3") });

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Blocker =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelHasLiveModels);

	TestTrue(TEXT("The server's refusal is predicted before the write"), Blocker != nullptr);
	if (Blocker)
	{
		TestTrue(TEXT("A predicted refusal blocks rather than cautions"),
			Blocker->Severity == ECrowdyDeleteSeverity::Blocker);
		TestTrue(TEXT("It says how many there are"), Blocker->Headline.Contains(TEXT("3 live models")));
		TestEqual(TEXT("The count is the real one"), Blocker->ReferenceCount, 3);
		TestEqual(TEXT("And the ids are there to look at"), Blocker->References.Num(), 3);
		TestFalse(TEXT("Its remedy leads somewhere"), Blocker->Remedy.IsEmpty());
	}

	TestTrue(TEXT("The worst finding is what the ladder is scaled to"), Plan.Ladder == ECrowdyDeleteLadder::Blocked);
	TestFalse(TEXT("A plan holding a blocker cannot be committed"), Plan.bCommittable);
	TestFalse(TEXT("And the sheet says what has to change"), Plan.Sheet.BlockedReason.IsEmpty());
	TestTrue(TEXT("Findings are sorted worst first"),
		Plan.Findings.Num() > 0 && Plan.Findings[0].Severity == ECrowdyDeleteSeverity::Blocker);

	// A probe that stopped at its limit knows a floor, not a total, and the sentence has to say so.
	FCrowdyDeleteEvidence Capped = DeleteTestEvidence();
	Capped.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Capped, TEXT("Hero"), ECrowdyLiveCountState::AtLeast, CrowdyDeleteLiveModelProbeLimit);
	const FCrowdyDeletePlan CappedPlan = CrowdyGameModelDelete::BuildPlan(Marks, Capped);
	const FCrowdyDeleteFinding* CappedBlocker =
		DeleteTestFindFinding(CappedPlan.Findings, ECrowdyDeleteFindingKind::ModelHasLiveModels);
	TestTrue(TEXT("A capped probe still blocks"), CappedBlocker != nullptr);
	if (CappedBlocker)
	{
		TestTrue(TEXT("And says its count is a floor"), CappedBlocker->Headline.Contains(TEXT("at least")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteBlocksWhenTheLiveCountIsUnknownTest,
	"CrowdySDK.CrowdyStudio.DeleteBlocksWhenTheLiveCountIsUnknown", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteBlocksWhenTheLiveCountIsUnknownTest::RunTest(const FString& Parameters)
{
	// The missing-scope answer. A model nobody counted has no entry, and reading an absent count as zero turns a
	// refusal nobody was warned about into a green light. It clears with a refresh, so it strands nobody.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	TestTrue(TEXT("An uncounted model blocks"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelLiveCountUnknown) != nullptr);
	TestTrue(TEXT("And it is not reported as having none"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelHasLiveModels) == nullptr);
	TestFalse(TEXT("Nothing can be committed on an answer nobody has"), Plan.bCommittable);
	TestTrue(TEXT("The ladder is blocked, not merely a stronger confirmation"),
		Plan.Ladder == ECrowdyDeleteLadder::Blocked);

	// A read that landed with a count of none is a fact, and a fact does not block.
	FCrowdyDeleteEvidence Counted = DeleteTestEvidence();
	Counted.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Counted, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);
	const FCrowdyDeletePlan CountedPlan = CrowdyGameModelDelete::BuildPlan(Marks, Counted);

	TestTrue(TEXT("A counted model with none does not block"),
		DeleteTestFindFinding(CountedPlan.Findings, ECrowdyDeleteFindingKind::ModelLiveCountUnknown) == nullptr);
	TestTrue(TEXT("And can be committed"), CountedPlan.bCommittable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteLiveCountUnknownIsNotZeroTest,
	"CrowdySDK.CrowdyStudio.DeleteLiveCountUnknownIsNotZero", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteLiveCountUnknownIsNotZeroTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 4);
	DeleteTestSetLiveCount(Evidence, TEXT("Wraith"), ECrowdyLiveCountState::AtLeast, CrowdyDeleteLiveModelProbeLimit);
	// A read that failed leaves an entry with no answer in it. Whatever number happens to be sitting in the count
	// is not an answer, and handing it back would be worse than saying nothing.
	DeleteTestSetLiveCount(Evidence, TEXT("Ghost"), ECrowdyLiveCountState::Unknown, 5);

	int32 Count = -1;
	TestTrue(TEXT("A model that was read reports a fact"),
		Evidence.FindLiveCount(TEXT("Hero"), Count) == ECrowdyLiveCountState::Exact);
	TestEqual(TEXT("With its total"), Count, 4);

	Count = -1;
	TestTrue(TEXT("A capped probe reports a floor"),
		Evidence.FindLiveCount(TEXT("Wraith"), Count) == ECrowdyLiveCountState::AtLeast);
	TestEqual(TEXT("At the limit it stopped on"), Count, CrowdyDeleteLiveModelProbeLimit);

	Count = -1;
	TestTrue(TEXT("A model nobody probed has no answer"),
		Evidence.FindLiveCount(TEXT("Never"), Count) == ECrowdyLiveCountState::Unknown);
	TestEqual(TEXT("And its count is left at nothing rather than claimed as none"), Count, 0);

	Count = -1;
	TestTrue(TEXT("A failed read has no answer either"),
		Evidence.FindLiveCount(TEXT("Ghost"), Count) == ECrowdyLiveCountState::Unknown);
	TestEqual(TEXT("And whatever number came with it is not handed back"), Count, 0);

	TestTrue(TEXT("A model with no attributes read is not a model with none"),
		Evidence.FindAttributes(TEXT("Hero")) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteBlocksOnABoundFunctionTest,
	"CrowdySDK.CrowdyStudio.DeleteBlocksOnABoundFunction", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteBlocksOnABoundFunctionTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("__crowdy_touch_hero")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Goblin"), TEXT("bite")));
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));

	const TArray<FString> Blocking =
		CrowdyGameModelDelete::FunctionsBlockingModelDelete(TEXT("Hero"), Marks, Evidence);
	TestEqual(TEXT("Only the designer function bound to this model is in the way"), Blocking.Num(), 1);
	if (Blocking.Num() == 1)
	{
		TestEqual(TEXT("And it is named"), Blocking[0], FString(TEXT("heal")));
	}

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Blocker =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelHasBoundFunctions);

	TestTrue(TEXT("A bound function is a refusal, so it blocks"), Blocker != nullptr);
	if (Blocker)
	{
		TestTrue(TEXT("It blocks rather than cautions"), Blocker->Severity == ECrowdyDeleteSeverity::Blocker);
		TestEqual(TEXT("One function is in the way"), Blocker->ReferenceCount, 1);
		TestFalse(TEXT("And the remedy is the one that clears it"), Blocker->Remedy.IsEmpty());
	}
	TestFalse(TEXT("The plan cannot run"), Plan.bCommittable);

	// A function on another model is somebody else's problem, and the SDK's own wiring is the plan's, not the
	// reader's, so neither may raise a refusal the reader cannot act on.
	TestFalse(TEXT("A function on another model does not block this one"),
		Blocking.Contains(TEXT("bite")));
	TestFalse(TEXT("Nor does the wiring the plan deletes itself"),
		Blocking.Contains(TEXT("__crowdy_touch_hero")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteMarkedFunctionClearsTheBlockTest,
	"CrowdySDK.CrowdyStudio.DeleteMarkedFunctionClearsTheBlock", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteMarkedFunctionClearsTheBlockTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	TestTrue(TEXT("A function this plan deletes first is no longer in the way"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelHasBoundFunctions) == nullptr);
	TestTrue(TEXT("So the plan can run"), Plan.bCommittable);
	TestTrue(TEXT("One press is enough for a plan that only cascades"), Plan.Ladder == ECrowdyDeleteLadder::Confirm);

	const int32 FunctionIndex = DeleteTestIndexOfArg(Plan.Ops, TEXT("name"), TEXT("heal"));
	const int32 ModelIndex = DeleteTestIndexOfArg(Plan.Ops, TEXT("typeName"), TEXT("Hero"));
	TestTrue(TEXT("Both operations are issued"), FunctionIndex != INDEX_NONE && ModelIndex != INDEX_NONE);
	TestTrue(TEXT("And the function runs before the model"), FunctionIndex < ModelIndex);

	// A mark whose model was never worked out is not a mark that clears THIS model's refusal. Reading it as one
	// would clear a refusal nothing has actually cleared.
	TArray<FCrowdyDeleteMark> Unscoped;
	Unscoped.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));
	Unscoped.Add(CrowdyGameModelDelete::MarkFunction(FString(), TEXT("heal"), TEXT("heal")));
	TestEqual(TEXT("A function mark with no model clears nothing"),
		CrowdyGameModelDelete::FunctionsBlockingModelDelete(TEXT("Hero"), Unscoped, Evidence).Num(), 1);

	// Nor is a mark naming a different model. The name matches; the entity does not.
	TArray<FCrowdyDeleteMark> Elsewhere;
	Elsewhere.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));
	Elsewhere.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Goblin"), TEXT("heal"), TEXT("heal")));
	TestEqual(TEXT("A function marked on another model clears nothing here"),
		CrowdyGameModelDelete::FunctionsBlockingModelDelete(TEXT("Hero"), Elsewhere, Evidence).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteAttributeCautionsComeFromTheFetchedFunctionsTest,
	"CrowdySDK.CrowdyStudio.DeleteAttributeCautionsComeFromTheFetchedFunctions", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteAttributeCautionsComeFromTheFetchedFunctionsTest::RunTest(const FString& Parameters)
{
	// Deleting an attribute refuses nothing and cascades nothing on the server. The values already stored for it
	// stay behind on every live model and every expression keeps naming a key that has gone, so this pre-flight is
	// the only guard that exists anywhere.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Functions.Add(DeleteTestWritingFunction(TEXT("Hero"), TEXT("heal"), TEXT("self"), TEXT("hp"), TEXT("1")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Goblin"), TEXT("bite"), TEXT("target.hp - 5")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("rest"), TEXT("self.stamina")));

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("hp"), TEXT("hp")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	const FCrowdyDeleteFinding* Orphans =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::AttributeOrphansStoredValues);
	TestTrue(TEXT("The stored values left behind are always said"), Orphans != nullptr);
	if (Orphans)
	{
		TestTrue(TEXT("It succeeds and breaks something, so it cautions rather than blocks"),
			Orphans->Severity == ECrowdyDeleteSeverity::Caution);
		TestFalse(TEXT("And it says nothing undoes it"), Orphans->Remedy.IsEmpty());
	}

	const FCrowdyDeleteFinding* Readers =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::AttributeReadByFunctions);
	TestTrue(TEXT("The functions naming the key are listed"), Readers != nullptr);
	if (Readers)
	{
		// A function bound to another model still reaches this key through a link, so narrowing the scan to the
		// attribute's own model would miss exactly the ones a reader most needs to see.
		TestEqual(TEXT("Both functions naming it are found, whichever model they are bound to"),
			Readers->ReferenceCount, 2);
		TestTrue(TEXT("The plan must be acknowledged, not merely confirmed"),
			Plan.Ladder == ECrowdyDeleteLadder::Acknowledge);
	}

	// The references come from the functions the server answered with. Empty that list, with the read still having
	// happened, and the finding has nothing to say: it was never reading a plan's own change list.
	FCrowdyDeleteEvidence NoFunctions = DeleteTestEvidence();
	NoFunctions.Types.Add(DeleteTestType(TEXT("Hero")));
	const FCrowdyDeletePlan NoReaders = CrowdyGameModelDelete::BuildPlan(Marks, NoFunctions);
	TestTrue(TEXT("With no functions read back, none are named"),
		DeleteTestFindFinding(NoReaders.Findings, ECrowdyDeleteFindingKind::AttributeReadByFunctions) == nullptr);
	TestTrue(TEXT("But the stored values are still said"),
		DeleteTestFindFinding(NoReaders.Findings, ECrowdyDeleteFindingKind::AttributeOrphansStoredValues) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteFindingsGroupByKindTest,
	"CrowdySDK.CrowdyStudio.DeleteFindingsGroupByKind", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteFindingsGroupByKindTest::RunTest(const FString& Parameters)
{
	// Fourteen functions reading one key is one consequence, not fourteen. Fourteen lines saying the same sentence
	// is a wall the reader skips; one line with fourteen entries behind it is something they read.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	for (int32 Index = 1; Index <= 14; ++Index)
	{
		Evidence.Functions.Add(DeleteTestFunction(
			TEXT("Hero"), FString::Printf(TEXT("reader_%02d"), Index), TEXT("self.hp")));
	}

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("hp"), TEXT("hp")));

	const TArray<FCrowdyDeleteFinding> Findings = CrowdyGameModelDelete::BuildFindings(Marks, Evidence);

	TestEqual(TEXT("Fourteen references are one finding"),
		DeleteTestCountFindings(Findings, ECrowdyDeleteFindingKind::AttributeReadByFunctions), 1);

	const FCrowdyDeleteFinding* Readers =
		DeleteTestFindFinding(Findings, ECrowdyDeleteFindingKind::AttributeReadByFunctions);
	if (Readers)
	{
		TestEqual(TEXT("With all fourteen behind it"), Readers->ReferenceCount, 14);
		TestEqual(TEXT("And all fourteen listed, since that fits"), Readers->References.Num(), 14);
		TestFalse(TEXT("So nothing was cut short"), Readers->bReferencesTruncated);
		TestTrue(TEXT("The one line names the count itself"), Readers->Headline.Contains(TEXT("14")));
	}

	// Past the disclosure limit the list stops and says how many more there are, so the sentence is still true and
	// the panel still has a bounded height.
	FCrowdyDeleteEvidence Many = DeleteTestEvidence();
	Many.Types.Add(DeleteTestType(TEXT("Hero")));
	const int32 TooMany = CrowdyDeleteMaxDisclosureEntries + 5;
	for (int32 Index = 1; Index <= TooMany; ++Index)
	{
		Many.Functions.Add(DeleteTestFunction(
			TEXT("Hero"), FString::Printf(TEXT("reader_%02d"), Index), TEXT("self.hp")));
	}

	const TArray<FCrowdyDeleteFinding> ManyFindings = CrowdyGameModelDelete::BuildFindings(Marks, Many);
	TestEqual(TEXT("Still one finding"),
		DeleteTestCountFindings(ManyFindings, ECrowdyDeleteFindingKind::AttributeReadByFunctions), 1);

	const FCrowdyDeleteFinding* ManyReaders =
		DeleteTestFindFinding(ManyFindings, ECrowdyDeleteFindingKind::AttributeReadByFunctions);
	if (ManyReaders)
	{
		TestEqual(TEXT("The real total is kept"), ManyReaders->ReferenceCount, TooMany);
		TestEqual(TEXT("The list stops at the disclosure limit"),
			ManyReaders->References.Num(), CrowdyDeleteMaxDisclosureEntries);
		TestTrue(TEXT("And says it had to stop"), ManyReaders->bReferencesTruncated);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteLadderScalesToTheWorstFindingTest,
	"CrowdySDK.CrowdyStudio.DeleteLadderScalesToTheWorstFinding", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteLadderScalesToTheWorstFindingTest::RunTest(const FString& Parameters)
{
	// Info only: one line, one button, nothing to tick. A plan whose whole content is "this is what you asked for"
	// must not cost more to confirm than it is worth.
	FCrowdyDeleteEvidence Info = DeleteTestEvidence();
	Info.Automations.Add(DeleteTestAutomation(TEXT("tick")));
	Info.AutomationTriggers.Add(DeleteTestTrigger(TEXT("tick"), TEXT("property_changed"), TEXT("Hero"), FString()));

	TArray<FCrowdyDeleteMark> AutomationMark;
	AutomationMark.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));

	const FCrowdyDeletePlan InfoPlan = CrowdyGameModelDelete::BuildPlan(AutomationMark, Info);
	TestTrue(TEXT("A documented cascade is only informational"),
		CrowdyGameModelDelete::WorstSeverity(InfoPlan.Findings) == ECrowdyDeleteSeverity::Info);
	TestTrue(TEXT("So one press is enough"), InfoPlan.Ladder == ECrowdyDeleteLadder::Confirm);
	TestTrue(TEXT("It can be committed"), InfoPlan.bCommittable);
	TestTrue(TEXT("And there is nothing to tick"), InfoPlan.Sheet.AcknowledgeLabel.IsEmpty());
	TestTrue(TEXT("Nothing is in the way"), InfoPlan.Sheet.BlockedReason.IsEmpty());
	TestTrue(TEXT("The button names the count it acts on"), InfoPlan.Sheet.ActionLabel.Contains(TEXT("1")));

	// A caution succeeds and breaks something, so it needs an explicit acknowledgement.
	FCrowdyDeleteEvidence Caution = DeleteTestEvidence();
	Caution.Types.Add(DeleteTestType(TEXT("Hero")));

	TArray<FCrowdyDeleteMark> AttributeMark;
	AttributeMark.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("hp"), TEXT("hp")));

	const FCrowdyDeletePlan CautionPlan = CrowdyGameModelDelete::BuildPlan(AttributeMark, Caution);
	TestTrue(TEXT("A caution is the worst finding here"),
		CrowdyGameModelDelete::WorstSeverity(CautionPlan.Findings) == ECrowdyDeleteSeverity::Caution);
	TestTrue(TEXT("So consent has to be given explicitly"), CautionPlan.Ladder == ECrowdyDeleteLadder::Acknowledge);
	TestFalse(TEXT("And the acknowledgement is worded"), CautionPlan.Sheet.AcknowledgeLabel.IsEmpty());
	TestTrue(TEXT("It is still a plan that can run once consented to"), CautionPlan.bCommittable);

	// A blocker never arms the button. Clearing it is a change to the marked set or a refresh, never a stronger
	// confirmation, so no amount of ticking gets past it.
	FCrowdyDeleteEvidence Blocked = DeleteTestEvidence();
	Blocked.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Blocked, TEXT("Hero"), ECrowdyLiveCountState::Exact, 2, { TEXT("c-1"), TEXT("c-2") });

	TArray<FCrowdyDeleteMark> ModelMark;
	ModelMark.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));

	const FCrowdyDeletePlan BlockedPlan = CrowdyGameModelDelete::BuildPlan(ModelMark, Blocked);
	TestTrue(TEXT("A blocker is the worst finding"),
		CrowdyGameModelDelete::WorstSeverity(BlockedPlan.Findings) == ECrowdyDeleteSeverity::Blocker);
	TestTrue(TEXT("The ladder says so"), BlockedPlan.Ladder == ECrowdyDeleteLadder::Blocked);
	TestFalse(TEXT("And nothing can commit it"), BlockedPlan.bCommittable);
	TestTrue(TEXT("There is nothing to tick, because ticking would not help"),
		BlockedPlan.Sheet.AcknowledgeLabel.IsEmpty());

	// The ladder is scaled to the worst finding, never to how much is being deleted.
	TestTrue(TEXT("Nothing to do is never a confirmation"),
		CrowdyGameModelDelete::LadderFor(BlockedPlan.Findings, 0) == ECrowdyDeleteLadder::Empty);
	TestTrue(TEXT("An empty set of findings is informational"),
		CrowdyGameModelDelete::WorstSeverity({}) == ECrowdyDeleteSeverity::Info);
	TestTrue(TEXT("So a hundred operations with nothing to say still need one press"),
		CrowdyGameModelDelete::LadderFor({}, 100) == ECrowdyDeleteLadder::Confirm);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteRemainderStartsAtTheFailedOpTest,
	"CrowdySDK.CrowdyStudio.DeleteRemainderStartsAtTheFailedOp", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteRemainderStartsAtTheFailedOpTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Ghost")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));
	Marks.Add(CrowdyGameModelDelete::MarkLiveModel(TEXT("Hero"), TEXT("c-1"), TEXT("c-1")));
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("hp"), TEXT("hp")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Ghost"), TEXT("Ghost")));

	const TArray<FCrowdyDeleteOp> Ops = CrowdyGameModelDelete::BuildOps(Marks, Evidence);
	TestEqual(TEXT("Five operations to run"), Ops.Num(), 5);

	// The operation AT the stopped index did not complete, so it is the first thing a second press has to run.
	// Starting the remainder after it would silently skip the one thing that failed.
	const TArray<FCrowdyDeleteOp> Rest = CrowdyGameModelDelete::Remainder(Ops, 2);
	TestEqual(TEXT("Everything from the failure onwards is left"), Rest.Num(), 3);
	if (Rest.Num() == 3 && Ops.Num() == 5)
	{
		TestEqual(TEXT("Starting with the one that did not complete"), Rest[0].Describe, Ops[2].Describe);
		TestEqual(TEXT("In the same order as before"), Rest[1].Describe, Ops[3].Describe);
		TestEqual(TEXT("All the way to the end"), Rest[2].Describe, Ops[4].Describe);
	}

	TestEqual(TEXT("A walk that finished leaves nothing"),
		CrowdyGameModelDelete::Remainder(Ops, INDEX_NONE).Num(), 0);
	TestEqual(TEXT("Nor does one that stopped past the end"),
		CrowdyGameModelDelete::Remainder(Ops, Ops.Num()).Num(), 0);
	TestEqual(TEXT("A negative index is not an index"),
		CrowdyGameModelDelete::Remainder(Ops, -2).Num(), 0);

	// Pressing again runs the remainder. The operations the first press finished answer that they are already
	// gone, which is a success, so the second walk reaches the end and leaves nothing behind.
	TestEqual(TEXT("Running the remainder to the end finishes the job"),
		CrowdyGameModelDelete::Remainder(Rest, INDEX_NONE).Num(), 0);

	FCrowdyDeleteOutcome Finished;
	Finished.AppId = 7;
	Finished.Total = Rest.Num();
	Finished.Completed = Rest.Num();
	Finished.AlreadyGone = 0;
	Finished.StoppedAtIndex = INDEX_NONE;
	TestEqual(TEXT("And nothing is left after it"),
		CrowdyGameModelDelete::Remainder(Rest, Finished.StoppedAtIndex).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteStopAndCompletionTextTest,
	"CrowdySDK.CrowdyStudio.DeleteStopAndCompletionText", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteStopAndCompletionTextTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteOutcome Stopped;
	Stopped.AppId = 7;
	Stopped.Total = 5;
	Stopped.Completed = 2;
	Stopped.bStopped = true;
	Stopped.StoppedAtIndex = 2;
	Stopped.StoppedOnDescription = TEXT("the model Ghost");

	const FString StopLine = CrowdyGameModelDelete::StopText(Stopped);
	TestTrue(TEXT("It says what it stopped on"), StopLine.Contains(TEXT("the model Ghost")));
	TestTrue(TEXT("And how far it got"), StopLine.Contains(TEXT("2 of 5")));
	TestTrue(TEXT("And how much is left"), StopLine.Contains(TEXT("3")));
	// The deletes already made are gone and re-running them is a no-op, so the instruction is to finish rather
	// than to start over.
	TestTrue(TEXT("It says to press again to finish"), StopLine.Contains(TEXT("finish")));

	// A cancellation is this editor rebuilding its own connection, not the server refusing anything. Saying
	// otherwise sends the reader hunting for a blocker that was never there.
	FCrowdyDeleteOutcome Canceled = Stopped;
	Canceled.bStoppedByCancel = true;
	const FString CancelLine = CrowdyGameModelDelete::StopText(Canceled);
	TestNotEqual(TEXT("A cancellation reads differently from a refusal"), CancelLine, StopLine);
	TestTrue(TEXT("And says the server refused nothing"), CancelLine.Contains(TEXT("refused nothing")));
	TestTrue(TEXT("While still saying how far it got"), CancelLine.Contains(TEXT("2 of 5")));

	FCrowdyDeleteOutcome Ran;
	Ran.bStopped = false;
	TestEqual(TEXT("A walk that finished leaves no stop line"),
		CrowdyGameModelDelete::StopText(Ran), FString());

	// Completed counts the ones that were already gone, so claiming them all as deletions would be untrue.
	FCrowdyDeleteOutcome Done;
	Done.Total = 18;
	Done.Completed = 18;
	Done.AlreadyGone = 2;
	Done.StoppedAtIndex = INDEX_NONE;
	const FString DoneLine = CrowdyGameModelDelete::CompletionText(Done);
	TestTrue(TEXT("Only what was really deleted is claimed"), DoneLine.Contains(TEXT("16")));
	TestTrue(TEXT("And the rest is reported honestly"), DoneLine.Contains(TEXT("2 were already gone")));
	TestFalse(TEXT("It never claims eighteen deletions"), DoneLine.Contains(TEXT("Deleted 18")));

	FCrowdyDeleteOutcome Clean = Done;
	Clean.AlreadyGone = 0;
	const FString CleanLine = CrowdyGameModelDelete::CompletionText(Clean);
	TestTrue(TEXT("With nothing missing it just says what went"), CleanLine.Contains(TEXT("18")));
	TestFalse(TEXT("And mentions nothing that was already gone"), CleanLine.Contains(TEXT("already gone")));

	FCrowdyDeleteOutcome Nothing;
	TestEqual(TEXT("Nothing to do says so"),
		CrowdyGameModelDelete::CompletionText(Nothing), FString(TEXT("Nothing to delete.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeletePrimaryViewRefusesACodeDeclaredRowTest,
	"CrowdySDK.CrowdyStudio.DeletePrimaryViewRefusesACodeDeclaredRow", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeletePrimaryViewRefusesACodeDeclaredRowTest::RunTest(const FString& Parameters)
{
	// The page invariant: the primary view never offers an action that will silently undo itself. An entity the
	// project declares comes straight back on the next sync, so deleting it spends a destructive server write to
	// achieve nothing.
	const FString ClassPath = TEXT("/Game/Models/BP_Hero.BP_Hero_C");

	const ECrowdyModelProvenance Declared[] = {
		ECrowdyModelProvenance::CodeSynced,
		ECrowdyModelProvenance::CodeNotPushed,
		ECrowdyModelProvenance::CodeDrifted
	};

	for (const ECrowdyModelProvenance Provenance : Declared)
	{
		FCrowdyModelRow Row =
			DeleteTestRow(ECrowdyModelRowKind::Function, TEXT("Hero"), TEXT("DealDamage"), Provenance);
		Row.CodePath = ClassPath;

		const FCrowdyDeleteGate Gate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Row);
		TestFalse(TEXT("A row the project declares offers no delete"), Gate.bAllowed);
		TestTrue(TEXT("And the reason says the project declares it in code"),
			Gate.Reason.Contains(TEXT("declares")) && Gate.Reason.Contains(TEXT("in code")));
		TestTrue(TEXT("It names what the row is, not just that something is wrong"),
			Gate.Reason.Contains(TEXT("DealDamage")));
		TestEqual(TEXT("And there is somewhere to go instead"), Gate.CodePath, ClassPath);
		TestFalse(TEXT("Nothing mistakes this for a kit's doing"), Gate.Reason.Contains(TEXT("Game Kit")));
	}

	// The same rule for a model in the left-hand list, decided in the same place so two surfaces cannot disagree.
	FCrowdyModelSummary Summary;
	Summary.TypeName = TEXT("Hero");
	Summary.Display = TEXT("Hero");
	Summary.Provenance = ECrowdyModelProvenance::CodeSynced;
	Summary.CodePath = ClassPath;

	const FCrowdyDeleteGate ModelGate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Summary);
	TestFalse(TEXT("A model the project declares offers no delete either"), ModelGate.bAllowed);
	TestEqual(TEXT("And names the class that declares it"), ModelGate.CodePath, ClassPath);

	// A row the server has never had has nothing on the server to delete, which is a different sentence.
	FCrowdyModelRow Pending =
		DeleteTestRow(ECrowdyModelRowKind::Function, TEXT("Hero"), TEXT("Heal"), ECrowdyModelProvenance::CodeNotPushed);
	Pending.bCodeOnly = true;
	const FCrowdyDeleteGate PendingGate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Pending);
	TestFalse(TEXT("A row the server does not have offers no delete"), PendingGate.bAllowed);
	TestTrue(TEXT("And says there is nothing on the server to delete"),
		PendingGate.Reason.Contains(TEXT("nothing on it to delete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeletePrimaryViewRefusesAKitOwnedRowTest,
	"CrowdySDK.CrowdyStudio.DeletePrimaryViewRefusesAKitOwnedRow", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeletePrimaryViewRefusesAKitOwnedRowTest::RunTest(const FString& Parameters)
{
	// A kit owns its namespace. Deploying the kit again recreates what was deleted, so the row is refused for the
	// same reason a code-declared one is, and the reason has to say which of the two it is: what a reader has to
	// change is the kit, not their project.
	FCrowdyModelRow Row =
		DeleteTestRow(ECrowdyModelRowKind::Attribute, TEXT("CKArena"), TEXT("Rounds"), ECrowdyModelProvenance::KitOwned);

	const FCrowdyDeleteGate Gate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Row);
	TestFalse(TEXT("A kit's attribute offers no delete"), Gate.bAllowed);
	TestTrue(TEXT("And the reason names the kit"), Gate.Reason.Contains(TEXT("Game Kit")));
	TestTrue(TEXT("And says changing the kit is the fix"), Gate.Reason.Contains(TEXT("kit deploys")));
	TestFalse(TEXT("It is not blamed on the project's own code"), Gate.Reason.Contains(TEXT("in code")));

	FCrowdyModelSummary Summary;
	Summary.TypeName = TEXT("CKArena");
	Summary.Display = TEXT("CKArena");
	Summary.Provenance = ECrowdyModelProvenance::KitOwned;

	const FCrowdyDeleteGate ModelGate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Summary);
	TestFalse(TEXT("Nor does a kit's model"), ModelGate.bAllowed);
	TestTrue(TEXT("With the same reason"), ModelGate.Reason.Contains(TEXT("Game Kit")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeletePrimaryViewAnswersAMissingScopeTest,
	"CrowdySDK.CrowdyStudio.DeletePrimaryViewAnswersAMissingScope", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeletePrimaryViewAnswersAMissingScopeTest::RunTest(const FString& Parameters)
{
	// Three different ways a row's scope can fail to be an answer. Each needs its own, and none of them may fall
	// into the branch that offers the delete.
	FCrowdyModelSnapshot Snapshot;
	DeleteTestDeclareType(Snapshot, TEXT("Knight"), { TEXT("Health") });
	// Moved in code: the effect that authors "regen" now targets Knight, while the server still has it on Hero.
	DeleteTestDeclareFunction(Snapshot, TEXT("Knight"), TEXT("regen"), TEXT("/Game/Effects/FX_Regen.FX_Regen"));
	Snapshot.FunctionUpdates.Add(DeleteTestScopedKey(TEXT("Knight"), TEXT("regen")));
	// Never determined: an effect that would not compile claims this name, so nothing can be said about it.
	Snapshot.UncheckableFunctionKeys.Add(DeleteTestScopedKey(TEXT("Hero"), TEXT("brokenfx")));
	Snapshot.FunctionAuthors.Add({ TEXT("Hero"), TEXT("brokenfx"), TEXT("/Game/Effects/FX_Broken.FX_Broken") });

	TArray<TSharedPtr<FStudioFunction>> OnHero;
	OnHero.Add(DeleteTestFunction(TEXT("Hero"), TEXT("regen")));
	OnHero.Add(DeleteTestFunction(TEXT("Hero"), TEXT("brokenfx")));

	const TArray<FCrowdyModelRow> HeroRows =
		CrowdyModelLedger::BuildFunctionRows(OnHero, TEXT("Hero"), &Snapshot);
	TestEqual(TEXT("Both server functions become rows"), HeroRows.Num(), 2);

	for (const FCrowdyModelRow& Row : HeroRows)
	{
		const FCrowdyDeleteGate Gate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Row);
		TestFalse(*FString::Printf(TEXT("%s offers no delete"), *Row.Name), Gate.bAllowed);

		if (Row.Name == TEXT("regen"))
		{
			// The declaration moved to another model, so the project still declares this name and the next sync
			// puts it back where it now belongs.
			TestTrue(TEXT("A declaration that moved is still a declaration"),
				Gate.Reason.Contains(TEXT("in code")));
			TestEqual(TEXT("And the asset that moved it is what to open"),
				Gate.CodePath, FString(TEXT("/Game/Effects/FX_Regen.FX_Regen")));
		}
		else
		{
			// Nobody has been able to answer whether the project declares this. Offering the delete would answer
			// it by assumption, in the direction that loses somebody's work.
			TestTrue(TEXT("An unanswered question is refused, not guessed at"),
				Gate.Reason.Contains(TEXT("Nothing has checked yet")));
			TestEqual(TEXT("And the asset that cannot be checked is named"),
				Gate.CodePath, FString(TEXT("/Game/Effects/FX_Broken.FX_Broken")));
		}
	}

	// Empty: a function or attribute is resolved inside a model, and a row with no model names no entity at all.
	// Deleting on the name alone could act on another model's entry of the same spelling.
	FCrowdyModelRow Unscoped =
		DeleteTestRow(ECrowdyModelRowKind::Attribute, FString(), TEXT("Health"), ECrowdyModelProvenance::ServerOnly);
	const FCrowdyDeleteGate UnscopedGate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Unscoped);
	TestFalse(TEXT("A row with no model offers no delete"), UnscopedGate.bAllowed);
	TestTrue(TEXT("And says which model it belongs to is not known"),
		UnscopedGate.Reason.Contains(TEXT("not known here")));

	FCrowdyModelRow UnscopedFunction =
		DeleteTestRow(ECrowdyModelRowKind::Function, FString(), TEXT("regen"), ECrowdyModelProvenance::ServerOnly);
	TestFalse(TEXT("The same holds for a function"),
		CrowdyGameModelDelete::CanDeleteFromPrimaryView(UnscopedFunction).bAllowed);

	// With no plan captured at all, nothing is known about any row, which is the same unanswered question.
	FCrowdyModelRow NoPlan =
		DeleteTestRow(ECrowdyModelRowKind::Function, TEXT("Hero"), TEXT("regen"), ECrowdyModelProvenance::Unknown);
	const FCrowdyDeleteGate NoPlanGate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(NoPlan);
	TestFalse(TEXT("An app nobody has planned offers no schema delete"), NoPlanGate.bAllowed);
	TestTrue(TEXT("And the reason names the one press that settles it"),
		NoPlanGate.Reason.Contains(TEXT("Preview changes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeletePrimaryViewAllowsWhatOnlyTheServerHasTest,
	"CrowdySDK.CrowdyStudio.DeletePrimaryViewAllowsWhatOnlyTheServerHas", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeletePrimaryViewAllowsWhatOnlyTheServerHasTest::RunTest(const FString& Parameters)
{
	const FCrowdyModelRow ServerOnly =
		DeleteTestRow(ECrowdyModelRowKind::Attribute, TEXT("Hero"), TEXT("LegacyMana"), ECrowdyModelProvenance::ServerOnly);
	const FCrowdyDeleteGate Gate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(ServerOnly);
	TestTrue(TEXT("Nothing declares it, so deleting it is final and is offered"), Gate.bAllowed);
	TestTrue(TEXT("With nothing to explain away"), Gate.Reason.IsEmpty());

	// A live model is runtime state. No class declares one and no sync recreates one, so the trap the gate exists
	// for cannot apply, and it is allowed even on an app nobody has planned.
	const FCrowdyModelRow Live =
		DeleteTestRow(ECrowdyModelRowKind::LiveInstance, TEXT("Hero"), TEXT("c-1"), ECrowdyModelProvenance::Unknown);
	TestTrue(TEXT("A live model is always deletable"),
		CrowdyGameModelDelete::CanDeleteFromPrimaryView(Live).bAllowed);

	// A row with no name names no entity, whatever kind it is.
	const FCrowdyModelRow Nameless =
		DeleteTestRow(ECrowdyModelRowKind::LiveInstance, TEXT("Hero"), FString(), ECrowdyModelProvenance::Unknown);
	const FCrowdyDeleteGate NamelessGate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(Nameless);
	TestFalse(TEXT("A live row with no id offers nothing"), NamelessGate.bAllowed);
	TestFalse(TEXT("And says why"), NamelessGate.Reason.IsEmpty());

	FCrowdyModelSummary Summary;
	Summary.TypeName = TEXT("LegacyChest");
	Summary.Display = TEXT("LegacyChest");
	Summary.Provenance = ECrowdyModelProvenance::ServerOnly;
	TestTrue(TEXT("A model only the server has is deletable"),
		CrowdyGameModelDelete::CanDeleteFromPrimaryView(Summary).bAllowed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteFunctionNameOnTwoModelsPromisesNothingTest,
	"CrowdySDK.CrowdyStudio.DeleteFunctionNameOnTwoModelsPromisesNothing", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteFunctionNameOnTwoModelsPromisesNothingTest::RunTest(const FString& Parameters)
{
	// The delete takes a bare name. Whether the server confines it to one model is not something this side can
	// know, so a confirm reading "remove regen from Knight" would be a promise the operation may not keep.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Types.Add(DeleteTestType(TEXT("Mage")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Mage"), TEXT("regen")));

	const TArray<FString> Carriers = CrowdyGameModelDelete::ModelsCarryingFunctionName(Evidence, TEXT("regen"));
	TestEqual(TEXT("Both models carrying the name are found"), Carriers.Num(), 2);
	if (Carriers.Num() == 2)
	{
		TestEqual(TEXT("In name order"), Carriers[0], FString(TEXT("Knight")));
		TestEqual(TEXT("Both of them"), Carriers[1], FString(TEXT("Mage")));
	}

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Ambiguous =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp);

	TestTrue(TEXT("The reader is told the name is not unique"), Ambiguous != nullptr);
	if (Ambiguous)
	{
		TestTrue(TEXT("It cautions, because the delete succeeds and may take more than was asked for"),
			Ambiguous->Severity == ECrowdyDeleteSeverity::Caution);
		TestEqual(TEXT("Every model carrying the name is listed"), Ambiguous->ReferenceCount, 2);
		TestTrue(TEXT("Including the one that was not marked"), Ambiguous->References.Contains(TEXT("Mage")));
		TestTrue(TEXT("And it says every one of them may go"), Ambiguous->Headline.Contains(TEXT("Every one of them")));
	}

	TestEqual(TEXT("One operation is issued"), Plan.Ops.Num(), 1);
	if (Plan.Ops.Num() == 1)
	{
		TestTrue(TEXT("The operation knows the name is shared"), Plan.Ops[0].bNameNotUniqueInApp);
		// The line the sheet and the progress text show must not claim a scoping the operation may not have.
		TestEqual(TEXT("So it claims only what is certain"), Plan.Ops[0].Describe, FString(TEXT("the function regen")));
		TestFalse(TEXT("And promises nothing about which model"), Plan.Ops[0].Describe.Contains(TEXT("Knight")));
		// The mark still carries the model, so nothing downstream has to guess which row this came from.
		TestEqual(TEXT("While the mark still knows where it came from"), Plan.Ops[0].Subject.OwningType,
			FString(TEXT("Knight")));
	}
	TestTrue(TEXT("A caution has to be acknowledged"), Plan.Ladder == ECrowdyDeleteLadder::Acknowledge);

	// One model carrying the name is not ambiguous, so nothing is withheld.
	FCrowdyDeleteEvidence Unique = DeleteTestEvidence();
	Unique.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	const FCrowdyDeletePlan UniquePlan = CrowdyGameModelDelete::BuildPlan(Marks, Unique);
	TestTrue(TEXT("With one carrier there is no ambiguity to report"),
		DeleteTestFindFinding(UniquePlan.Findings, ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp) == nullptr);
	if (UniquePlan.Ops.Num() == 1)
	{
		TestEqual(TEXT("And the line can say which model"), UniquePlan.Ops[0].Describe,
			FString(TEXT("the function regen on Knight")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteIdentityIsCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.DeleteIdentityIsCaseSensitive", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteIdentityIsCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// A model name, an attribute key and a function name are server keys, and FString comparison folds case by
	// default. Two entities differing only in case are two entities, and folding them is how a delete lands on the
	// wrong one.
	const FCrowdyDeleteMark Upper = CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health"));
	const FCrowdyDeleteMark Lower = CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("health"), TEXT("health"));

	TestTrue(TEXT("Two keys differing only in case are two entities"), Upper != Lower);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(Upper);
	TestTrue(TEXT("A marked set holds the one that was marked"),
		CrowdyGameModelDelete::ContainsMark(Marks, Upper));
	TestFalse(TEXT("And not its differently-cased neighbour"),
		CrowdyGameModelDelete::ContainsMark(Marks, Lower));

	// The same for the model that scopes it: an attribute of "Hero" is not folded into a marked "hero".
	TArray<FCrowdyDeleteMark> WrongCaseModel;
	WrongCaseModel.Add(CrowdyGameModelDelete::MarkModel(TEXT("hero"), TEXT("hero")));
	WrongCaseModel.Add(Upper);
	TestFalse(TEXT("A differently-cased model does not absorb this attribute"),
		CrowdyGameModelDelete::IsSubsumed(Upper, WrongCaseModel));

	TArray<FCrowdyDeleteMark> RightCaseModel;
	RightCaseModel.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));
	RightCaseModel.Add(Upper);
	TestTrue(TEXT("The model it really belongs to does"),
		CrowdyGameModelDelete::IsSubsumed(Upper, RightCaseModel));

	// An attribute and a function on one model can share a name and are still two entities, so the kind is part
	// of what identifies one.
	const FCrowdyDeleteMark SameNameFunction =
		CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("Health"), TEXT("Health"));
	TestTrue(TEXT("An attribute and a function of one name are two entities"), SameNameFunction != Upper);
	TestNotEqual(TEXT("And their identity keys differ"), SameNameFunction.IdentityKey(), Upper.IdentityKey());

	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Mage"), TEXT("Regen")));
	const TArray<FString> Carriers = CrowdyGameModelDelete::ModelsCarryingFunctionName(Evidence, TEXT("regen"));
	TestEqual(TEXT("Only the exactly-named function's model carries the name"), Carriers.Num(), 1);
	if (Carriers.Num() == 1)
	{
		TestEqual(TEXT("And it is the right one"), Carriers[0], FString(TEXT("Knight")));
	}

	// Two differently-cased keys are two deletes, not one.
	TArray<FCrowdyDeleteMark> Both;
	Both.Add(Upper);
	Both.Add(Lower);
	TestEqual(TEXT("Two differently-cased attributes are two operations"),
		CrowdyGameModelDelete::BuildOps(Both, Evidence).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteEmptyMarkSetIsAnEmptyPlanTest,
	"CrowdySDK.CrowdyStudio.DeleteEmptyMarkSetIsAnEmptyPlan", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteEmptyMarkSetIsAnEmptyPlanTest::RunTest(const FString& Parameters)
{
	const FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan({}, Evidence);

	TestEqual(TEXT("Nothing marked is nothing to run"), Plan.Ops.Num(), 0);
	TestEqual(TEXT("And nothing to say"), Plan.Findings.Num(), 0);
	TestEqual(TEXT("And nothing subsumed"), Plan.SubsumedMarks.Num(), 0);
	TestTrue(TEXT("There is nothing to consent to"), Plan.Ladder == ECrowdyDeleteLadder::Empty);
	TestFalse(TEXT("So there is nothing to commit"), Plan.bCommittable);

	TestEqual(TEXT("The sheet says so plainly"), Plan.Sheet.Headline, FString(TEXT("Nothing to delete.")));
	TestEqual(TEXT("It counts nothing"), Plan.Sheet.CountLines.Num(), 0);
	TestTrue(TEXT("It adds nothing of its own"), Plan.Sheet.ImpliedLine.IsEmpty());
	TestTrue(TEXT("It folds nothing away"), Plan.Sheet.SubsumedLine.IsEmpty());
	TestTrue(TEXT("There is nothing to acknowledge"), Plan.Sheet.AcknowledgeLabel.IsEmpty());
	TestFalse(TEXT("And the button explains why it does nothing"), Plan.Sheet.BlockedReason.IsEmpty());

	// A plan is a value, so an empty one still carries a token. A commit whose plan changed to this one must fail
	// its comparison rather than find nothing to compare against.
	TestFalse(TEXT("Even an empty plan is fingerprinted"), Plan.ConsentToken.IsEmpty());
	TestEqual(TEXT("The app is still named"), Plan.AppId, Evidence.AppId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteFunctionReferencesWholeIdentifiersTest,
	"CrowdySDK.CrowdyStudio.DeleteFunctionReferencesWholeIdentifiers", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteFunctionReferencesWholeIdentifiersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyGameModelDelete;

	const TSharedPtr<FStudioFunction> Writes =
		DeleteTestWritingFunction(TEXT("Hero"), TEXT("heal"), TEXT("self"), TEXT("hp"), TEXT("1"));
	TestTrue(TEXT("A mutation writing the key names it"),
		FunctionReferencesAttribute(*Writes, TEXT("Hero"), TEXT("hp")));
	// Deleting "hp" must not report every function that merely mentions a longer key beginning the same way.
	TestFalse(TEXT("A longer key that starts the same is a different key"),
		FunctionReferencesAttribute(*Writes, TEXT("Hero"), TEXT("hp_max")));

	const TSharedPtr<FStudioFunction> Reads =
		DeleteTestWritingFunction(TEXT("Hero"), TEXT("cap"), TEXT("self"), TEXT("armour"), TEXT("self.hp_max - 1"));
	TestTrue(TEXT("An expression naming the key counts"),
		FunctionReferencesAttribute(*Reads, TEXT("Hero"), TEXT("hp_max")));
	TestFalse(TEXT("And the shorter key inside it does not"),
		FunctionReferencesAttribute(*Reads, TEXT("Hero"), TEXT("hp")));

	const TSharedPtr<FStudioFunction> Targets =
		DeleteTestWritingFunction(TEXT("Hero"), TEXT("drain"), TEXT("target.hp"), TEXT("other"), TEXT("0"));
	TestTrue(TEXT("A mutation target naming the key counts"),
		FunctionReferencesAttribute(*Targets, TEXT("Hero"), TEXT("hp")));

	const TSharedPtr<FStudioFunction> Returns = DeleteTestFunction(TEXT("Hero"), TEXT("peek"), TEXT("self.hp + 1"));
	TestTrue(TEXT("A return expression naming the key counts"),
		FunctionReferencesAttribute(*Returns, TEXT("Hero"), TEXT("hp")));

	const TSharedPtr<FStudioFunction> Quiet = DeleteTestFunction(TEXT("Hero"), TEXT("noop"), TEXT("1 + 1"));
	TestFalse(TEXT("A function naming nothing references nothing"),
		FunctionReferencesAttribute(*Quiet, TEXT("Hero"), TEXT("hp")));
	TestFalse(TEXT("And an empty key matches nothing at all"),
		FunctionReferencesAttribute(*Returns, TEXT("Hero"), FString()));

	// An attribute key is a server key, so a differently-cased spelling is a different key.
	TestFalse(TEXT("A differently-cased key is a different key"),
		FunctionReferencesAttribute(*Returns, TEXT("Hero"), TEXT("HP")));

	// The automation scans, likewise on exact names.
	const TSharedPtr<FStudioAutomation> Runner =
		DeleteTestAutomation(TEXT("tick"), TEXT("Hero"), TEXT("regen"));
	TestTrue(TEXT("An automation running the function names it"),
		AutomationReferencesFunction(*Runner, TEXT("regen")));
	TestFalse(TEXT("But not a differently-cased name"),
		AutomationReferencesFunction(*Runner, TEXT("Regen")));
	TestTrue(TEXT("An automation targeting the model names it"),
		AutomationTargetsModel(*Runner, TEXT("Hero")));
	// An app-wide automation names no target, and matching it against an empty type would report it as targeting
	// whichever model happened to be asked about.
	TestFalse(TEXT("An empty model name matches nothing"), AutomationTargetsModel(*Runner, FString()));

	const TSharedPtr<FStudioAutomationTrigger> Trigger =
		DeleteTestTrigger(TEXT("tick"), TEXT("function_invoked"), TEXT("Hero"), TEXT("regen"));
	TestTrue(TEXT("A trigger naming the function counts"), TriggerReferencesFunction(*Trigger, TEXT("regen")));
	TestFalse(TEXT("And an empty name matches nothing"), TriggerReferencesFunction(*Trigger, FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteConsentTokenIsStableAndSensitiveTest,
	"CrowdySDK.CrowdyStudio.DeleteConsentTokenIsStableAndSensitive", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteConsentTokenIsStableAndSensitiveTest::RunTest(const FString& Parameters)
{
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Ghost")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));

	TArray<FCrowdyDeleteMark> OneOrder;
	OneOrder.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health")));
	OneOrder.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	OneOrder.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));

	TArray<FCrowdyDeleteMark> AnotherOrder;
	AnotherOrder.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));
	AnotherOrder.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	AnotherOrder.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health")));

	const TArray<FCrowdyDeleteOp> First = CrowdyGameModelDelete::BuildOps(OneOrder, Evidence);
	const TArray<FCrowdyDeleteOp> Second = CrowdyGameModelDelete::BuildOps(AnotherOrder, Evidence);

	TestEqual(TEXT("The same marked set is the same work"), First.Num(), Second.Num());
	// The order the boxes were ticked in is not part of the plan, so the same set has to produce the same list.
	const FString FirstToken =
		CrowdyGameModelDelete::MakeConsentToken(7, First, ECrowdyDeleteLadder::Acknowledge);
	const FString SecondToken =
		CrowdyGameModelDelete::MakeConsentToken(7, Second, ECrowdyDeleteLadder::Acknowledge);
	TestEqual(TEXT("So the fingerprint is the same whatever order it was ticked in"), FirstToken, SecondToken);

	// One argument changing while the count stays the same is a different plan, and the sheet on screen described
	// the other one.
	TArray<FCrowdyDeleteMark> DifferentKey;
	DifferentKey.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Mana"), TEXT("Mana")));
	DifferentKey.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	DifferentKey.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));

	const TArray<FCrowdyDeleteOp> Changed = CrowdyGameModelDelete::BuildOps(DifferentKey, Evidence);
	TestEqual(TEXT("The same number of operations"), Changed.Num(), First.Num());
	TestNotEqual(TEXT("But a different fingerprint"),
		CrowdyGameModelDelete::MakeConsentToken(7, Changed, ECrowdyDeleteLadder::Acknowledge), FirstToken);

	// Consent given to a one-line confirm is not consent to a sheet that has since grown a caution.
	TestNotEqual(TEXT("A different ladder is a different consent"),
		CrowdyGameModelDelete::MakeConsentToken(7, First, ECrowdyDeleteLadder::Confirm), FirstToken);
	TestNotEqual(TEXT("And a different app is a different plan entirely"),
		CrowdyGameModelDelete::MakeConsentToken(8, First, ECrowdyDeleteLadder::Acknowledge), FirstToken);

	// The panel compares the token at click time against the one the sheet was drawn from, so the plan has to
	// fingerprint exactly the operations it will run and nothing else.
	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(OneOrder, Evidence);
	TestTrue(TEXT("Deleting an attribute is a caution, so this plan needs acknowledging"),
		Plan.Ladder == ECrowdyDeleteLadder::Acknowledge);
	TestEqual(TEXT("And its fingerprint is the one those operations and that ladder produce"),
		Plan.ConsentToken, FirstToken);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteEvidenceReadFlagsAreNotEmptinessTest,
	"CrowdySDK.CrowdyStudio.DeleteEvidenceReadFlagsAreNotEmptiness", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteEvidenceReadFlagsAreNotEmptinessTest::RunTest(const FString& Parameters)
{
	// An empty list means "this app has none" only once its read has happened. Folding the two would let an unread
	// function list clear the bound-function refusal for every model in the app at once.
	FCrowdyDeleteEvidence Unread = DeleteTestEvidence();
	Unread.bFunctionsRead = false;
	Unread.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(Unread, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));

	const FCrowdyDeletePlan UnreadPlan = CrowdyGameModelDelete::BuildPlan(Marks, Unread);
	TestTrue(TEXT("An unread function list does not clear the bound-function refusal"),
		DeleteTestFindFinding(UnreadPlan.Findings, ECrowdyDeleteFindingKind::ModelHasBoundFunctions) != nullptr);
	TestFalse(TEXT("And nothing can be committed against lists nobody read"), UnreadPlan.bCommittable);

	FString Reason;
	TestFalse(TEXT("The evidence is not good enough to judge a plan"),
		CrowdyGameModelDelete::IsEvidenceSufficient(Unread, Reason));
	TestTrue(TEXT("And it says which read is missing"), Reason.Contains(TEXT("functions")));
	TestEqual(TEXT("The sheet shows that as its reason"), UnreadPlan.Sheet.BlockedReason, Reason);

	// The same evidence with the read having happened and finding none is a different answer entirely.
	FCrowdyDeleteEvidence Read = Unread;
	Read.bFunctionsRead = true;
	const FCrowdyDeletePlan ReadPlan = CrowdyGameModelDelete::BuildPlan(Marks, Read);
	TestTrue(TEXT("An app with no functions raises no bound-function refusal"),
		DeleteTestFindFinding(ReadPlan.Findings, ECrowdyDeleteFindingKind::ModelHasBoundFunctions) == nullptr);
	TestTrue(TEXT("And its plan can run"), ReadPlan.bCommittable);

	FString ReadReason;
	TestTrue(TEXT("Its evidence is sufficient"), CrowdyGameModelDelete::IsEvidenceSufficient(Read, ReadReason));
	TestTrue(TEXT("With nothing to report"), ReadReason.IsEmpty());

	// No app selected is the same kind of answer: there is nothing to delete from.
	FCrowdyDeleteEvidence NoApp = Read;
	NoApp.AppId = 0;
	FString NoAppReason;
	TestFalse(TEXT("No app means nothing to judge"),
		CrowdyGameModelDelete::IsEvidenceSufficient(NoApp, NoAppReason));
	TestFalse(TEXT("And it says so"), NoAppReason.IsEmpty());

	FCrowdyDeleteEvidence NoTypes = Read;
	NoTypes.bTypesRead = false;
	FString TypesReason;
	TestFalse(TEXT("Unread models are not judged either"),
		CrowdyGameModelDelete::IsEvidenceSufficient(NoTypes, TypesReason));

	FCrowdyDeleteEvidence NoAutomations = Read;
	NoAutomations.bAutomationsRead = false;
	FString AutomationsReason;
	TestFalse(TEXT("Nor are unread automations"),
		CrowdyGameModelDelete::IsEvidenceSufficient(NoAutomations, AutomationsReason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteImpliedPlumbingKeepsCountsHonestTest,
	"CrowdySDK.CrowdyStudio.DeleteImpliedPlumbingKeepsCountsHonest", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteImpliedPlumbingKeepsCountsHonestTest::RunTest(const FString& Parameters)
{
	// The SDK keeps a touch function on every model. It is never a row, so nobody can mark it, and the server
	// refuses the model delete while it is there. The plan therefore adds its delete itself and says so, rather
	// than being refused over something the reader was never shown.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("__crowdy_touch_hero")));
	DeleteTestSetLiveCount(Evidence, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);

	const TArray<FString> Plumbing = CrowdyGameModelDelete::PlumbingFunctionsFor(TEXT("Hero"), Evidence);
	TestEqual(TEXT("The wiring the server actually holds is found"), Plumbing.Num(), 1);
	if (Plumbing.Num() == 1)
	{
		TestEqual(TEXT("By name"), Plumbing[0], FString(TEXT("__crowdy_touch_hero")));
	}

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	TestEqual(TEXT("The model and its wiring are two operations"), Plan.Ops.Num(), 2);
	TestEqual(TEXT("One of which the plan added itself"), Plan.CountImpliedOps(), 1);

	const int32 PlumbingIndex = DeleteTestIndexOfArg(Plan.Ops, TEXT("name"), TEXT("__crowdy_touch_hero"));
	const int32 ModelIndex = DeleteTestIndexOfArg(Plan.Ops, TEXT("typeName"), TEXT("Hero"));
	TestTrue(TEXT("Both are issued"), PlumbingIndex != INDEX_NONE && ModelIndex != INDEX_NONE);
	TestTrue(TEXT("And the wiring goes first, as any bound function must"), PlumbingIndex < ModelIndex);

	// The numbers on the sheet are the rows the reader ticked. What the plan added for itself is disclosed on its
	// own line rather than folded into a count nothing on screen accounts for.
	TestEqual(TEXT("The headline counts only what was marked"), Plan.Sheet.Headline, FString(TEXT("Delete 1 model?")));
	TestEqual(TEXT("So does the button"), Plan.Sheet.ActionLabel, FString(TEXT("Delete 1 model")));
	TestFalse(TEXT("And the extra work is stated"), Plan.Sheet.ImpliedLine.IsEmpty());
	TestTrue(TEXT("The reader is told the wiring goes too"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelCascadesItsPlumbing) != nullptr);
	// It is the plan's own work, not a refusal the reader has to clear.
	TestTrue(TEXT("It is not reported as something in the way"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelHasBoundFunctions) == nullptr);
	TestTrue(TEXT("So the plan can run"), Plan.bCommittable);

	// The names are read from what the server answered with, never composed from the naming rule, so a plan never
	// issues a delete for something that is not there.
	FCrowdyDeleteEvidence NoPlumbing = DeleteTestEvidence();
	NoPlumbing.Types.Add(DeleteTestType(TEXT("Hero")));
	DeleteTestSetLiveCount(NoPlumbing, TEXT("Hero"), ECrowdyLiveCountState::Exact, 0);

	const FCrowdyDeletePlan Bare = CrowdyGameModelDelete::BuildPlan(Marks, NoPlumbing);
	TestEqual(TEXT("A model the server holds no wiring for gets one operation"), Bare.Ops.Num(), 1);
	TestEqual(TEXT("And nothing is implied"), Bare.CountImpliedOps(), 0);
	TestTrue(TEXT("So nothing extra is claimed"), Bare.Sheet.ImpliedLine.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteAuthorshipComesFromTheDesiredSideTest,
	"CrowdySDK.CrowdyStudio.DeleteAuthorshipComesFromTheDesiredSide", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteAuthorshipComesFromTheDesiredSideTest::RunTest(const FString& Parameters)
{
	// Whether an entity is authored in code is answered from the captured desired schema, never by inverting a
	// plan's server-only lists. Those lists already have kit protection and skipped-author exclusions folded into
	// them, so the inversion is wrong in opposite directions for a kit type and for a type owned by an effect that
	// will not compile.
	FCrowdyModelSnapshot Snapshot;
	DeleteTestDeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });
	DeleteTestDeclareFunction(Snapshot, TEXT("Hero"), TEXT("DealDamage"));
	DeleteTestDeclareAutomation(Snapshot, TEXT("HeroRegen"));

	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Snapshot = MakeShared<FCrowdyModelSnapshot>(Snapshot);
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Types.Add(DeleteTestType(TEXT("LegacyChest")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("DealDamage")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("legacy_heal")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("__crowdy_touch_hero")));
	Evidence.Automations.Add(DeleteTestAutomation(TEXT("HeroRegen"), TEXT("Hero")));
	Evidence.Automations.Add(DeleteTestAutomation(TEXT("legacy_tick"), TEXT("Hero")));
	DeleteTestSetAttributes(Evidence, TEXT("Hero"), { TEXT("Health"), TEXT("LegacyMana"), TEXT("crowdy_rev") });

	const TArray<FCrowdyDeleteMark> Marks = CrowdyGameModelDelete::MarkEverythingServerOnly(Evidence);

	auto Holds = [&Marks](const FCrowdyDeleteMark& Mark)
	{
		return CrowdyGameModelDelete::ContainsMark(Marks, Mark);
	};

	TestTrue(TEXT("A model no class declares is offered"),
		Holds(CrowdyGameModelDelete::MarkModel(TEXT("LegacyChest"), TEXT("LegacyChest"))));
	TestTrue(TEXT("An attribute no class declares is offered, even on a model that is in code"),
		Holds(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("LegacyMana"), TEXT("LegacyMana"))));
	TestTrue(TEXT("A function no effect authors is offered"),
		Holds(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("legacy_heal"), TEXT("legacy_heal"))));
	TestTrue(TEXT("An automation no effect authors is offered"),
		Holds(CrowdyGameModelDelete::MarkAutomation(TEXT("legacy_tick"), TEXT("legacy_tick"))));

	// Everything the desired schema declares is left alone, because deleting it would be undone by the next sync.
	TestFalse(TEXT("A model the project declares is not offered"),
		Holds(CrowdyGameModelDelete::MarkModel(TEXT("Hero"), TEXT("Hero"))));
	TestFalse(TEXT("Nor is an attribute it declares"),
		Holds(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health"))));
	TestFalse(TEXT("Nor a function an effect authors"),
		Holds(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("DealDamage"), TEXT("DealDamage"))));
	TestFalse(TEXT("Nor an automation an effect authors"),
		Holds(CrowdyGameModelDelete::MarkAutomation(TEXT("HeroRegen"), TEXT("HeroRegen"))));

	// The SDK's own wiring is nobody's design surface, so it is never offered as a row to tick.
	TestFalse(TEXT("The revision attribute is never offered"),
		Holds(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("crowdy_rev"), TEXT("crowdy_rev"))));
	TestFalse(TEXT("Nor is the touch function"),
		Holds(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("__crowdy_touch_hero"), TEXT("__crowdy_touch_hero"))));

	TestEqual(TEXT("Exactly the four orphans are offered"), Marks.Num(), 4);

	// A function whose model was never determined is not an orphan. Offering it would answer the missing scope by
	// assuming nobody owns it, which is the assumption that deletes somebody's work.
	FCrowdyDeleteEvidence Unscoped = Evidence;
	Unscoped.Functions.Add(DeleteTestFunction(FString(), TEXT("floating")));
	const TArray<FCrowdyDeleteMark> UnscopedMarks = CrowdyGameModelDelete::MarkEverythingServerOnly(Unscoped);
	TestEqual(TEXT("A function with no model is not offered"), UnscopedMarks.Num(), Marks.Num());

	// With no plan captured there is no classification at all, so there is nothing that can be called server-only.
	FCrowdyDeleteEvidence NoPlan = Evidence;
	NoPlan.Snapshot.Reset();
	TestEqual(TEXT("With no plan captured, nothing is claimed to be server-only"),
		CrowdyGameModelDelete::MarkEverythingServerOnly(NoPlan).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteBulkMarkIsRefusedOnAKitAppTest,
	"CrowdySDK.CrowdyStudio.DeleteBulkMarkIsRefusedOnAKitApp", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteBulkMarkIsRefusedOnAKitAppTest::RunTest(const FString& Parameters)
{
	FCrowdyModelSnapshot Snapshot;
	DeleteTestDeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });

	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Snapshot = MakeShared<FCrowdyModelSnapshot>(Snapshot);
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));
	Evidence.Types.Add(DeleteTestType(TEXT("LegacyChest")));

	FString Reason;
	TestTrue(TEXT("An app with no kit deployed can be marked in bulk"),
		CrowdyGameModelDelete::CanMarkEverythingServerOnly(Evidence, Reason));
	TestTrue(TEXT("With nothing to explain"), Reason.IsEmpty());

	// Only a kit's FUNCTIONS carry prune protection today, so a bulk mark on a kit app would offer the kit's own
	// models and attributes for deletion. One at a time is the honest answer until that gap is closed.
	FCrowdyModelSnapshot KitSnapshot = Snapshot;
	KitSnapshot.RecognizedKitTypePrefixes.Add(TEXT("CK"));

	FCrowdyDeleteEvidence KitApp = Evidence;
	KitApp.Snapshot = MakeShared<FCrowdyModelSnapshot>(KitSnapshot);
	KitApp.Types.Add(DeleteTestType(TEXT("CKArena")));

	FString KitReason;
	TestFalse(TEXT("An app a kit was deployed to refuses the bulk mark"),
		CrowdyGameModelDelete::CanMarkEverythingServerOnly(KitApp, KitReason));
	TestTrue(TEXT("And says why"), KitReason.Contains(TEXT("Game Kit")));
	TestTrue(TEXT("And what to do instead"), KitReason.Contains(TEXT("one at a time")));

	// The classification itself still knows the kit's type is not an orphan, which is the half that has to be
	// answered from the desired side rather than by inverting a prune list.
	TestFalse(TEXT("A kit's own model is never marked as server-only"),
		CrowdyGameModelDelete::ContainsMark(
			CrowdyGameModelDelete::MarkEverythingServerOnly(KitApp),
			CrowdyGameModelDelete::MarkModel(TEXT("CKArena"), TEXT("CKArena"))));

	// With no plan captured, server-only is not a question anything here can answer.
	FCrowdyDeleteEvidence NoPlan = Evidence;
	NoPlan.Snapshot.Reset();
	FString NoPlanReason;
	TestFalse(TEXT("With no plan captured the bulk mark is refused"),
		CrowdyGameModelDelete::CanMarkEverythingServerOnly(NoPlan, NoPlanReason));
	TestTrue(TEXT("And names the press that settles it"), NoPlanReason.Contains(TEXT("Preview changes")));

	// Half-read lists would mark less than everything while claiming to mark everything.
	FCrowdyDeleteEvidence HalfRead = Evidence;
	HalfRead.bFunctionsRead = false;
	FString HalfReason;
	TestFalse(TEXT("Half-read lists refuse the bulk mark"),
		CrowdyGameModelDelete::CanMarkEverythingServerOnly(HalfRead, HalfReason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteWarnsWhenSomethingWillPutItBackTest,
	"CrowdySDK.CrowdyStudio.DeleteWarnsWhenSomethingWillPutItBack", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteWarnsWhenSomethingWillPutItBackTest::RunTest(const FString& Parameters)
{
	// The primary view offers no such delete at all. This finding is what catches a mark that reached the plan by
	// some other route, so the last thing before the write still says the work will undo itself.
	FCrowdyModelSnapshot Snapshot;
	DeleteTestDeclareType(Snapshot, TEXT("Hero"), { TEXT("Health") });

	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Snapshot = MakeShared<FCrowdyModelSnapshot>(Snapshot);
	Evidence.Types.Add(DeleteTestType(TEXT("Hero")));

	TArray<FCrowdyDeleteMark> Declared;
	Declared.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("Health"), TEXT("Health")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Declared, Evidence);
	const FCrowdyDeleteFinding* Restored =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::RecreatedByTheNextSync);

	TestTrue(TEXT("A delete that will be undone says so"), Restored != nullptr);
	if (Restored)
	{
		TestTrue(TEXT("It cautions"), Restored->Severity == ECrowdyDeleteSeverity::Caution);
		TestFalse(TEXT("And says where the real fix is"), Restored->Remedy.IsEmpty());
	}

	// An entity nothing declares is deleted for good, so there is nothing to warn about.
	TArray<FCrowdyDeleteMark> Orphan;
	Orphan.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("LegacyMana"), TEXT("LegacyMana")));
	const FCrowdyDeletePlan OrphanPlan = CrowdyGameModelDelete::BuildPlan(Orphan, Evidence);
	TestTrue(TEXT("An orphan is not claimed to come back"),
		DeleteTestFindFinding(OrphanPlan.Findings, ECrowdyDeleteFindingKind::RecreatedByTheNextSync) == nullptr);

	// With no plan captured, nothing is known. Repeating that as a caution would fire on every mark in the app and
	// drown the cautions that are about real consequences.
	FCrowdyDeleteEvidence NoPlan = Evidence;
	NoPlan.Snapshot.Reset();
	const FCrowdyDeletePlan Unknown = CrowdyGameModelDelete::BuildPlan(Declared, NoPlan);
	TestTrue(TEXT("An unanswered question is not a claim that it comes back"),
		DeleteTestFindFinding(Unknown.Findings, ECrowdyDeleteFindingKind::RecreatedByTheNextSync) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteAutomationCascadesAreInformationalTest,
	"CrowdySDK.CrowdyStudio.DeleteAutomationCascadesAreInformational", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteAutomationCascadesAreInformationalTest::RunTest(const FString& Parameters)
{
	// A documented cascade is what the reader asked for, so it is stated and nothing more is demanded of them.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Automations.Add(DeleteTestAutomation(TEXT("tick"), TEXT("Hero"), TEXT("regen")));
	Evidence.AutomationTriggers.Add(DeleteTestTrigger(TEXT("tick"), TEXT("property_changed"), TEXT("Hero")));
	Evidence.AutomationTriggers.Add(DeleteTestTrigger(TEXT("tick"), TEXT("container_created"), TEXT("Hero")));

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Cascade =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::AutomationCascadesItsTriggers);

	TestTrue(TEXT("The triggers going with it is stated"), Cascade != nullptr);
	if (Cascade)
	{
		TestTrue(TEXT("As what was asked for, not as a warning"), Cascade->Severity == ECrowdyDeleteSeverity::Info);
		TestEqual(TEXT("Both of them"), Cascade->ReferenceCount, 2);
		TestTrue(TEXT("An intended cascade needs no remedy"), Cascade->Remedy.IsEmpty());
	}
	TestTrue(TEXT("So one press is enough"), Plan.Ladder == ECrowdyDeleteLadder::Confirm);
	// The triggers go with the automation, so they need no operation of their own.
	TestEqual(TEXT("Only the automation is deleted"), Plan.Ops.Num(), 1);

	// An automation that survives a function it runs is a different matter: it keeps running and what it runs no
	// longer resolves.
	FCrowdyDeleteEvidence WithFunction = DeleteTestEvidence();
	WithFunction.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("regen")));
	WithFunction.Automations.Add(DeleteTestAutomation(TEXT("tick"), TEXT("Hero"), TEXT("regen")));

	TArray<FCrowdyDeleteMark> FunctionMark;
	FunctionMark.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("regen"), TEXT("regen")));

	const FCrowdyDeletePlan Orphaned = CrowdyGameModelDelete::BuildPlan(FunctionMark, WithFunction);
	const FCrowdyDeleteFinding* Runners =
		DeleteTestFindFinding(Orphaned.Findings, ECrowdyDeleteFindingKind::FunctionRunByAutomations);
	TestTrue(TEXT("An automation left running nothing is reported"), Runners != nullptr);
	if (Runners)
	{
		TestTrue(TEXT("As a caution"), Runners->Severity == ECrowdyDeleteSeverity::Caution);
		TestTrue(TEXT("Naming the automation"), Runners->References.Contains(TEXT("tick")));
	}

	// Marking the automation too means nothing survives pointing at what has gone.
	TArray<FCrowdyDeleteMark> Both = FunctionMark;
	Both.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));
	const FCrowdyDeletePlan Together = CrowdyGameModelDelete::BuildPlan(Both, WithFunction);
	TestTrue(TEXT("An automation this plan deletes is not reported as surviving"),
		DeleteTestFindFinding(Together.Findings, ECrowdyDeleteFindingKind::FunctionRunByAutomations) == nullptr);
	TestTrue(TEXT("And the automation goes before the function it runs"),
		Together.Ops.Num() == 2 && Together.Ops[0].Kind == ECrowdyDeleteKind::Automation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteSheetKeepsTheBoundVocabularyTest,
	"CrowdySDK.CrowdyStudio.DeleteSheetKeepsTheBoundVocabulary", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteSheetKeepsTheBoundVocabularyTest::RunTest(const FString& Parameters)
{
	// A container type is a model, a property definition is an attribute, a live container is a live model. The raw
	// server nouns belong to the Advanced tab, which exists to show them.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Ghost")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Hero"), TEXT("heal")));
	Evidence.Automations.Add(DeleteTestAutomation(TEXT("tick"), TEXT("Hero")));
	DeleteTestSetLiveCount(Evidence, TEXT("Ghost"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Ghost"), TEXT("Ghost")));
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Hero"), TEXT("hp"), TEXT("hp")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Hero"), TEXT("heal"), TEXT("heal")));
	Marks.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("tick"), TEXT("tick")));
	Marks.Add(CrowdyGameModelDelete::MarkLiveModel(TEXT("Hero"), TEXT("c-1"), TEXT("c-1")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	TArray<FString> Shown;
	Shown.Add(Plan.Sheet.Headline);
	Shown.Add(Plan.Sheet.AppLine);
	Shown.Add(Plan.Sheet.ImpliedLine);
	Shown.Add(Plan.Sheet.SubsumedLine);
	Shown.Add(Plan.Sheet.ActionLabel);
	Shown.Add(Plan.Sheet.AcknowledgeLabel);
	Shown.Add(Plan.Sheet.BlockedReason);
	Shown.Append(Plan.Sheet.CountLines);
	for (const FCrowdyDeleteFinding& Finding : Plan.Findings)
	{
		Shown.Add(Finding.Headline);
		Shown.Add(Finding.Remedy);
	}
	for (const FCrowdyDeleteOp& Op : Plan.Ops)
	{
		// The operation name and its arguments are what goes on the wire. Only the line a reader sees is checked.
		Shown.Add(Op.Describe);
	}

	for (const FString& Text : Shown)
	{
		TestFalse(*FString::Printf(TEXT("No raw server noun reaches the reader: %s"), *Text),
			DeleteTestUsesARawServerNoun(Text));
	}

	TestTrue(TEXT("A container type is called a model"), Plan.Sheet.Headline.Contains(TEXT("model")));
	TestTrue(TEXT("A property definition is called an attribute"), Plan.Sheet.Headline.Contains(TEXT("attribute")));
	TestEqual(TEXT("One line per kind present"), Plan.Sheet.CountLines.Num(), 5);
	TestFalse(TEXT("The app being written to is named, because there is no undo"), Plan.Sheet.AppLine.IsEmpty());
	TestTrue(TEXT("And the button names the count it acts on"), Plan.Sheet.ActionLabel.Contains(TEXT("5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteAttributeScanCoversEveryExpressionTest,
	"CrowdySDK.CrowdyStudio.DeleteAttributeScanCoversEveryExpression", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteAttributeScanCoversEveryExpressionTest::RunTest(const FString& Parameters)
{
	// A function is not only its writes. Its authority gate, the arguments of what it announces and the delay,
	// dedupe key and bound parameters of what it schedules are all expressions over model state, and deleting an
	// attribute breaks every one of them exactly as it breaks a mutation. A scan that reads mutations and the
	// returned value alone reports no dependants at all for these, and the delete then goes out having shown the
	// reader nothing.

	// Only the authority gate names the key: no mutation, no returned value.
	TSharedPtr<FStudioFunction> Gated = DeleteTestFunction(TEXT("Knight"), TEXT("attack"));
	Gated->InvokePolicyJson = TEXT("{\"type\":\"condition\",\"expression\":\"self.stamina > 0\"}");

	// Only a timer names it, and each of the three places a timer carries an expression is checked separately.
	TSharedPtr<FStudioFunction> Delayed = DeleteTestFunction(TEXT("Knight"), TEXT("tick"));
	{
		FCrowdyGameModelTimer Timer;
		Timer.FunctionName = TEXT("tick");
		Timer.DelayMsExpression = TEXT("self.stamina * 10");
		Delayed->Timers.Add(MoveTemp(Timer));
	}

	TSharedPtr<FStudioFunction> Deduped = DeleteTestFunction(TEXT("Knight"), TEXT("charge"));
	{
		FCrowdyGameModelTimer Timer;
		Timer.FunctionName = TEXT("charge");
		Timer.DedupeKeyExpression = TEXT("self.stamina");
		Deduped->Timers.Add(MoveTemp(Timer));
	}

	TSharedPtr<FStudioFunction> Bound = DeleteTestFunction(TEXT("Knight"), TEXT("rally"));
	{
		FCrowdyGameModelTimer Timer;
		Timer.FunctionName = TEXT("rally");
		FCrowdyGameModelTimerParam Param;
		Param.Name = TEXT("amount");
		Param.Expression = TEXT("self.stamina");
		Timer.Params.Add(MoveTemp(Param));
		Bound->Timers.Add(MoveTemp(Timer));
	}

	// Only a notification argument names it.
	TSharedPtr<FStudioFunction> Announcing = DeleteTestFunction(TEXT("Knight"), TEXT("shout"));
	{
		FCrowdyGameModelNotification Notification;
		Notification.Kind = TEXT("channel");
		FCrowdyGameModelNotificationArg Arg;
		Arg.Name = TEXT("left");
		Arg.Expression = TEXT("self.stamina");
		Notification.Args.Add(MoveTemp(Arg));
		Announcing->Notifications.Add(MoveTemp(Notification));
	}

	TestTrue(TEXT("An authority gate naming the key is a reference"),
		CrowdyGameModelDelete::FunctionReferencesAttribute(*Gated, TEXT("Knight"), TEXT("stamina")));
	TestTrue(TEXT("A timer's delay naming the key is a reference"),
		CrowdyGameModelDelete::FunctionReferencesAttribute(*Delayed, TEXT("Knight"), TEXT("stamina")));
	TestTrue(TEXT("A timer's dedupe key naming the key is a reference"),
		CrowdyGameModelDelete::FunctionReferencesAttribute(*Deduped, TEXT("Knight"), TEXT("stamina")));
	TestTrue(TEXT("A timer's bound parameter naming the key is a reference"),
		CrowdyGameModelDelete::FunctionReferencesAttribute(*Bound, TEXT("Knight"), TEXT("stamina")));
	TestTrue(TEXT("A notification argument naming the key is a reference"),
		CrowdyGameModelDelete::FunctionReferencesAttribute(*Announcing, TEXT("Knight"), TEXT("stamina")));

	// Still whole identifiers, wherever they are found: a longer key starting with the same letters is a different
	// key, and reporting it would train the reader to skip the disclosure.
	TestFalse(TEXT("A longer key sharing a prefix is not this key"),
		CrowdyGameModelDelete::FunctionReferencesAttribute(*Gated, TEXT("Knight"), TEXT("stam")));

	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Functions.Add(Gated);
	Evidence.Functions.Add(Delayed);
	Evidence.Functions.Add(Deduped);
	Evidence.Functions.Add(Bound);
	Evidence.Functions.Add(Announcing);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Knight"), TEXT("stamina"), TEXT("stamina")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Readers =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::AttributeReadByFunctions);
	TestTrue(TEXT("The plan reports them"), Readers != nullptr);
	if (Readers)
	{
		TestEqual(TEXT("All five are named, none of them a mutation"), Readers->ReferenceCount, 5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteAttributeScanCoversAutomationsTest,
	"CrowdySDK.CrowdyStudio.DeleteAttributeScanCoversAutomations", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteAttributeScanCoversAutomationsTest::RunTest(const FString& Parameters)
{
	// An automation picks what it runs against by naming attribute keys, and an event trigger waits on one by name.
	// Both arrays are already in the evidence, and a scan that read only the functions would report a key fourteen
	// automations depend on as having no dependants at all.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));

	TSharedPtr<FStudioAutomation> Healer = DeleteTestAutomation(TEXT("healer_ai"), TEXT("Knight"), TEXT("heal"));
	Healer->SelectorJson = TEXT("{\"pick\":\"lowest\",\"ofType\":\"Knight\",\"by\":{\"property\":\"hp\"}}");
	Evidence.Automations.Add(Healer);

	TSharedPtr<FStudioAutomation> Paid = DeleteTestAutomation(TEXT("payout"), TEXT("Knight"), TEXT("pay"));
	Paid->ParamsJson = TEXT("{\"amount\":\"self.hp\"}");
	Evidence.Automations.Add(Paid);

	TSharedPtr<FStudioAutomation> Alerting = DeleteTestAutomation(TEXT("low_hp_alert"), TEXT("Knight"), TEXT("warn"));
	Evidence.Automations.Add(Alerting);
	TSharedPtr<FStudioAutomationTrigger> Trigger =
		DeleteTestTrigger(TEXT("low_hp_alert"), TEXT("property_changed"), TEXT("Knight"), FString());
	Trigger->PropertyKey = TEXT("hp");
	Evidence.AutomationTriggers.Add(Trigger);

	// A trigger whose model the read never carried is a scope nobody determined. Reading it as somebody else's is
	// how a real dependant goes unreported.
	TSharedPtr<FStudioAutomation> Unscoped = DeleteTestAutomation(TEXT("watcher"), FString(), TEXT("watch"));
	Evidence.Automations.Add(Unscoped);
	TSharedPtr<FStudioAutomationTrigger> UnscopedTrigger =
		DeleteTestTrigger(TEXT("watcher"), TEXT("property_changed"), FString(), FString());
	UnscopedTrigger->PropertyKey = TEXT("hp");
	Evidence.AutomationTriggers.Add(UnscopedTrigger);

	// A trigger on another model waiting on another key is nobody's business here.
	TSharedPtr<FStudioAutomation> Elsewhere = DeleteTestAutomation(TEXT("mage_ai"), TEXT("Mage"), TEXT("cast"));
	Evidence.Automations.Add(Elsewhere);
	TSharedPtr<FStudioAutomationTrigger> ElsewhereTrigger =
		DeleteTestTrigger(TEXT("mage_ai"), TEXT("property_changed"), TEXT("Mage"), FString());
	ElsewhereTrigger->PropertyKey = TEXT("mana");
	Evidence.AutomationTriggers.Add(ElsewhereTrigger);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Knight"), TEXT("hp"), TEXT("hp")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Users =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::AttributeUsedByAutomations);

	TestTrue(TEXT("The automations naming the key are reported"), Users != nullptr);
	if (Users)
	{
		TestTrue(TEXT("It succeeds and breaks something, so it cautions"),
			Users->Severity == ECrowdyDeleteSeverity::Caution);
		TestEqual(TEXT("Selector, params, a scoped trigger and an unscoped one"), Users->ReferenceCount, 4);
		TestTrue(TEXT("The selector's automation is named"), Users->References.Contains(TEXT("healer_ai")));
		TestTrue(TEXT("So is the one whose static parameters name it"), Users->References.Contains(TEXT("payout")));
		TestTrue(TEXT("So is the one waiting on the key"), Users->References.Contains(TEXT("low_hp_alert")));
		TestTrue(TEXT("And the one whose model nobody determined"), Users->References.Contains(TEXT("watcher")));
		TestFalse(TEXT("An automation on another model waiting on another key is not"),
			Users->References.Contains(TEXT("mage_ai")));
	}

	// One this plan deletes anyway cannot be broken by the key going with it.
	TArray<FCrowdyDeleteMark> WithAutomation = Marks;
	WithAutomation.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("healer_ai"), TEXT("healer_ai")));
	const FCrowdyDeletePlan Together = CrowdyGameModelDelete::BuildPlan(WithAutomation, Evidence);
	const FCrowdyDeleteFinding* Fewer =
		DeleteTestFindFinding(Together.Findings, ECrowdyDeleteFindingKind::AttributeUsedByAutomations);
	if (TestNotNull(TEXT("The rest are still reported"), Fewer))
	{
		TestFalse(TEXT("An automation this plan deletes is not one it breaks"),
			Fewer->References.Contains(TEXT("healer_ai")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteModelCascadeWarnsAboutAttributeReadersTest,
	"CrowdySDK.CrowdyStudio.DeleteModelCascadeWarnsAboutAttributeReaders", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteModelCascadeWarnsAboutAttributeReadersTest::RunTest(const FString& Parameters)
{
	// Deleting a model takes its attributes with it, and an expression on another model reaches them through a
	// link. The consequence is identical to marking the attribute directly, so arriving at it through the model
	// must not soften it into an informational cascade: that would take the plan from an acknowledgement down to a
	// single unguarded press.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	DeleteTestSetAttributes(Evidence, TEXT("Knight"), { TEXT("hp"), TEXT("level") });
	DeleteTestSetLiveCount(Evidence, TEXT("Knight"), ECrowdyLiveCountState::Exact, 0);
	Evidence.Functions.Add(DeleteTestWritingFunction(
		TEXT("Cleric"), TEXT("heal"), TEXT("ref($target_id)"), TEXT("mana"), TEXT("ref($target_id).hp + $amount")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Cleric"), TEXT("rank_up"), TEXT("self.level + 1")));

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Knight"), TEXT("Knight")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	TestTrue(TEXT("The cascade itself is still stated as what was asked for"),
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelCascadesItsAttributes) != nullptr);

	const FCrowdyDeleteFinding* Orphaned =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelCascadeOrphansAttributeReaders);
	TestTrue(TEXT("What still names those attributes is reported too"), Orphaned != nullptr);
	if (Orphaned)
	{
		TestTrue(TEXT("At the same severity as marking the attribute would carry"),
			Orphaned->Severity == ECrowdyDeleteSeverity::Caution);
		TestEqual(TEXT("Both dependants are named"), Orphaned->ReferenceCount, 2);
	}
	TestTrue(TEXT("So the plan needs an explicit acknowledgement, not one press"),
		Plan.Ladder == ECrowdyDeleteLadder::Acknowledge);

	// A model whose attributes nothing else names is the informational case it always was.
	FCrowdyDeleteEvidence Alone = DeleteTestEvidence();
	Alone.Types.Add(DeleteTestType(TEXT("Knight")));
	DeleteTestSetAttributes(Alone, TEXT("Knight"), { TEXT("hp") });
	DeleteTestSetLiveCount(Alone, TEXT("Knight"), ECrowdyLiveCountState::Exact, 0);

	const FCrowdyDeletePlan Quiet = CrowdyGameModelDelete::BuildPlan(Marks, Alone);
	TestTrue(TEXT("Nothing is invented where nothing depends on it"),
		DeleteTestFindFinding(Quiet.Findings, ECrowdyDeleteFindingKind::ModelCascadeOrphansAttributeReaders) == nullptr);
	TestTrue(TEXT("And one press is still enough"), Quiet.Ladder == ECrowdyDeleteLadder::Confirm);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteOneOperationPerMarkedScopeTest,
	"CrowdySDK.CrowdyStudio.DeleteOneOperationPerMarkedScope", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteOneOperationPerMarkedScopeTest::RunTest(const FString& Parameters)
{
	// The mutation takes the bare name, so two functions of one name on two models produce byte-identical
	// arguments. Folding them into one call decides the unsettled scoping question in the direction that loses
	// one of them silently: under a type-scoped reading the second survives while the sheet says the job is done.
	// Under the app-wide reading the second call answers that there was nothing there, which this walk already
	// counts as a success, so keeping both costs a round trip and nothing else.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Types.Add(DeleteTestType(TEXT("Mage")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Mage"), TEXT("regen")));

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Mage"), TEXT("regen"), TEXT("regen")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	TestEqual(TEXT("Two marked entities are two operations"), Plan.Ops.Num(), 2);
	TestEqual(TEXT("Both are the function delete"), Plan.CountOps(ECrowdyDeleteKind::Function), 2);
	if (Plan.Ops.Num() == 2)
	{
		TestEqual(TEXT("The first carries the model it was marked on"), Plan.Ops[0].Subject.OwningType,
			FString(TEXT("Knight")));
		TestEqual(TEXT("And so does the second"), Plan.Ops[1].Subject.OwningType, FString(TEXT("Mage")));
		TestTrue(TEXT("Both know the name is shared"),
			Plan.Ops[0].bNameNotUniqueInApp && Plan.Ops[1].bNameNotUniqueInApp);
	}

	// The sheet counts operations, so folding one away would have left it one short of the rows the reader ticked
	// with nothing on screen accounting for the difference.
	TestEqual(TEXT("The sheet counts both"), Plan.Sheet.ActionLabel, FString(TEXT("Delete 2 functions")));
	TestTrue(TEXT("And nothing was quietly absorbed"), Plan.SubsumedMarks.Num() == 0);

	// One entity marked twice is still one call: the same name on the SAME model is the same entity, and a marked
	// function that its own model also implies must not be issued twice.
	TArray<FCrowdyDeleteMark> Doubled;
	Doubled.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));
	Doubled.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));
	TestEqual(TEXT("The same entity marked twice is one operation"),
		CrowdyGameModelDelete::BuildOps(Doubled, Evidence).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteAmbiguityDisclosureNamesEveryScopeTest,
	"CrowdySDK.CrowdyStudio.DeleteAmbiguityDisclosureNamesEveryScope", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteAmbiguityDisclosureNamesEveryScopeTest::RunTest(const FString& Parameters)
{
	// The caution is raised because the name sits in more than one scope, and a record whose model the read never
	// carried is its own scope. Disclosing only the models would omit exactly the entity that raised it, so the
	// reader opens the list expecting to see what else is at risk and is shown the one row they already knew about.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	Evidence.Functions.Add(DeleteTestFunction(FString(), TEXT("regen")));

	const TArray<FString> Scopes = CrowdyGameModelDelete::ScopesCarryingFunctionName(Evidence, TEXT("regen"));
	TestEqual(TEXT("A record with no model is its own scope"), Scopes.Num(), 2);
	TestTrue(TEXT("The named model is one of them"), Scopes.Contains(TEXT("Knight")));

	// The models-only list cannot name it, which is why the finding is built from the scopes instead.
	TestEqual(TEXT("The models-only list has nothing to call it"),
		CrowdyGameModelDelete::ModelsCarryingFunctionName(Evidence, TEXT("regen")).Num(), 1);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Ambiguous =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp);

	TestTrue(TEXT("The ambiguity is still raised"), Ambiguous != nullptr);
	if (Ambiguous)
	{
		TestEqual(TEXT("And the disclosure covers every scope that raised it"), Ambiguous->ReferenceCount, 2);
		TestEqual(TEXT("Listing both"), Ambiguous->References.Num(), 2);
		TestFalse(TEXT("Nothing was left out"), Ambiguous->bReferencesTruncated);

		const bool bNamesTheUndetermined = Ambiguous->References.ContainsByPredicate(
			[](const FString& Line) { return Line.Contains(TEXT("did not report")); });
		TestTrue(TEXT("The one that cannot be named is still accounted for"), bNamesTheUndetermined);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteImpliedWiringCarriesItsAmbiguityTest,
	"CrowdySDK.CrowdyStudio.DeleteImpliedWiringCarriesItsAmbiguity", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteImpliedWiringCarriesItsAmbiguityTest::RunTest(const FString& Parameters)
{
	// The SDK's wiring is named from a lowercased model name while server model names are case-sensitive, so two
	// models that differ only in case legitimately share one wiring name. Nobody marked it, so if the plan does not
	// raise the ambiguity itself the reader is never told that deleting one model may unwire the other.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Types.Add(DeleteTestType(TEXT("KNIGHT")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("__crowdy_touch_knight")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("KNIGHT"), TEXT("__crowdy_touch_knight")));
	DeleteTestSetLiveCount(Evidence, TEXT("Knight"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Knight"), TEXT("Knight")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);

	const FCrowdyDeleteOp* Implied = Plan.Ops.FindByPredicate(
		[](const FCrowdyDeleteOp& Op) { return Op.bImplied; });
	TestTrue(TEXT("The wiring is still deleted"), Implied != nullptr);
	if (Implied)
	{
		TestTrue(TEXT("And the operation knows its name is shared"), Implied->bNameNotUniqueInApp);
	}

	const FCrowdyDeleteFinding* Ambiguous =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp);
	TestTrue(TEXT("The reader is told, even though nobody marked it"), Ambiguous != nullptr);
	if (Ambiguous)
	{
		TestTrue(TEXT("It cautions"), Ambiguous->Severity == ECrowdyDeleteSeverity::Caution);
		TestTrue(TEXT("Naming the other model"), Ambiguous->References.Contains(TEXT("KNIGHT")));
	}
	TestTrue(TEXT("So the plan asks for an acknowledgement"), Plan.Ladder == ECrowdyDeleteLadder::Acknowledge);

	// A wiring name only one model carries is not ambiguous, so nothing extra is claimed.
	FCrowdyDeleteEvidence Alone = DeleteTestEvidence();
	Alone.Types.Add(DeleteTestType(TEXT("Knight")));
	Alone.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("__crowdy_touch_knight")));
	DeleteTestSetLiveCount(Alone, TEXT("Knight"), ECrowdyLiveCountState::Exact, 0);

	const FCrowdyDeletePlan Quiet = CrowdyGameModelDelete::BuildPlan(Marks, Alone);
	TestTrue(TEXT("Nothing is claimed where there is no collision"),
		DeleteTestFindFinding(Quiet.Findings, ECrowdyDeleteFindingKind::FunctionNameNotUniqueInApp) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteDisclosureCountsAreCaseSensitiveTest,
	"CrowdySDK.CrowdyStudio.DeleteDisclosureCountsAreCaseSensitive", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteDisclosureCountsAreCaseSensitiveTest::RunTest(const FString& Parameters)
{
	// The names in a disclosure are server keys, and TArray::AddUnique compares with the case-folding operator,
	// so two entities differing only in case would collapse into one line and one off the count printed beside it.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Automations.Add(DeleteTestAutomation(TEXT("Regen"), TEXT("Knight")));
	Evidence.Automations.Add(DeleteTestAutomation(TEXT("regen"), TEXT("Knight")));
	DeleteTestSetLiveCount(Evidence, TEXT("Knight"), ECrowdyLiveCountState::Exact, 0);

	TArray<FCrowdyDeleteMark> Marks;
	Marks.Add(CrowdyGameModelDelete::MarkModel(TEXT("Knight"), TEXT("Knight")));

	const FCrowdyDeletePlan Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	const FCrowdyDeleteFinding* Targeting =
		DeleteTestFindFinding(Plan.Findings, ECrowdyDeleteFindingKind::ModelTargetedByAutomations);

	TestTrue(TEXT("The automations left pointing at a model that has gone are reported"), Targeting != nullptr);
	if (Targeting)
	{
		TestEqual(TEXT("Two names differing only in case are two automations"), Targeting->ReferenceCount, 2);
		TestTrue(TEXT("Both are listed"),
			Targeting->References.Contains(TEXT("Regen")) && Targeting->References.Contains(TEXT("regen")));
		TestTrue(TEXT("And the sentence names the same number"), Targeting->Headline.Contains(TEXT("2 automations")));
	}

	// The same rule on the cascade line, whose count is what the reader is told goes with the model.
	FCrowdyDeleteEvidence Keys = DeleteTestEvidence();
	Keys.Types.Add(DeleteTestType(TEXT("Knight")));
	DeleteTestSetAttributes(Keys, TEXT("Knight"), { TEXT("Hp"), TEXT("hp") });
	DeleteTestSetLiveCount(Keys, TEXT("Knight"), ECrowdyLiveCountState::Exact, 0);

	const FCrowdyDeletePlan KeyPlan = CrowdyGameModelDelete::BuildPlan(Marks, Keys);
	const FCrowdyDeleteFinding* Cascade =
		DeleteTestFindFinding(KeyPlan.Findings, ECrowdyDeleteFindingKind::ModelCascadesItsAttributes);
	if (TestNotNull(TEXT("The cascade is stated"), Cascade))
	{
		TestEqual(TEXT("Two keys differing only in case are two attributes"), Cascade->ReferenceCount, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteMarkLineNeverPromisesAScopingTest,
	"CrowdySDK.CrowdyStudio.DeleteMarkLineNeverPromisesAScoping", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteMarkLineNeverPromisesAScopingTest::RunTest(const FString& Parameters)
{
	// The marked list is the only place the reader is told what they picked before they open the review, so it
	// must not be the one surface that promises a scoping the mutation may not honour while the caution that
	// contradicts it sits on another page behind a button press.
	FCrowdyDeleteEvidence Shared = DeleteTestEvidence();
	Shared.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	Shared.Functions.Add(DeleteTestFunction(TEXT("Mage"), TEXT("regen")));

	const FCrowdyDeleteMark Mark =
		CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen"));

	const FString SharedLine = CrowdyGameModelDelete::DescribeMark(Mark, Shared);
	TestFalse(TEXT("It does not claim the delete stays inside one model"), SharedLine.Contains(TEXT("on Knight")));
	TestTrue(TEXT("And says why it cannot"), SharedLine.Contains(TEXT("more than one model")));

	// One carrier, and the line can say which model, because there is only one it could be.
	FCrowdyDeleteEvidence Unique = DeleteTestEvidence();
	Unique.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));
	TestEqual(TEXT("With one carrier the model is named"),
		CrowdyGameModelDelete::DescribeMark(Mark, Unique), FString(TEXT("function regen on Knight")));

	// The other kinds are resolved by their scope, so they always name it.
	TestEqual(TEXT("An attribute names its model"),
		CrowdyGameModelDelete::DescribeMark(
			CrowdyGameModelDelete::MarkAttribute(TEXT("Knight"), TEXT("hp"), TEXT("hp")), Shared),
		FString(TEXT("attribute hp on Knight")));
	TestEqual(TEXT("A model names itself"),
		CrowdyGameModelDelete::DescribeMark(
			CrowdyGameModelDelete::MarkModel(TEXT("Knight"), TEXT("Knight")), Shared),
		FString(TEXT("model Knight")));
	TestEqual(TEXT("A live model says which model it is one of"),
		CrowdyGameModelDelete::DescribeMark(
			CrowdyGameModelDelete::MarkLiveModel(TEXT("Knight"), TEXT("c-1"), TEXT("c-1")), Shared),
		FString(TEXT("live model c-1 of Knight")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDeleteRemainderBelongsToItsOwnPlanTest,
	"CrowdySDK.CrowdyStudio.DeleteRemainderBelongsToItsOwnPlan", CrowdyGameModelDeleteTestFlags)

bool FCrowdyDeleteRemainderBelongsToItsOwnPlanTest::RunTest(const FString& Parameters)
{
	// A stopped walk's leftovers outlive the plan that produced them: nothing clears them when the marked set
	// changes. "Press Delete again to finish the remaining 3" printed above a button that would run something else
	// entirely is worse than saying nothing, so the banner is shown only while it still describes that button.
	FCrowdyDeleteEvidence Evidence = DeleteTestEvidence();
	Evidence.Types.Add(DeleteTestType(TEXT("Knight")));
	Evidence.Functions.Add(DeleteTestFunction(TEXT("Knight"), TEXT("regen")));

	TArray<FCrowdyDeleteMark> Original;
	Original.Add(CrowdyGameModelDelete::MarkAttribute(TEXT("Knight"), TEXT("hp"), TEXT("hp")));
	Original.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Knight"), TEXT("regen"), TEXT("regen")));

	const TArray<FCrowdyDeleteOp> Ops = CrowdyGameModelDelete::BuildOps(Original, Evidence);
	TestEqual(TEXT("Two operations to walk"), Ops.Num(), 2);

	const TArray<FCrowdyDeleteOp> Leftover = CrowdyGameModelDelete::Remainder(Ops, 1);
	TestEqual(TEXT("One did not run"), Leftover.Num(), 1);

	TestTrue(TEXT("The banner stands while the plan still holds what is left"),
		CrowdyGameModelDelete::RemainderAppliesTo(Leftover, Ops));

	// The marked set moved on. The leftover names an entity this plan would not touch.
	TArray<FCrowdyDeleteMark> Different;
	Different.Add(CrowdyGameModelDelete::MarkAutomation(TEXT("daily_reset"), TEXT("daily_reset")));
	const TArray<FCrowdyDeleteOp> Unrelated = CrowdyGameModelDelete::BuildOps(Different, Evidence);
	TestFalse(TEXT("And is withheld once the plan no longer holds it"),
		CrowdyGameModelDelete::RemainderAppliesTo(Leftover, Unrelated));

	// Nothing left over is nothing to say.
	const TArray<FCrowdyDeleteOp> Nothing;
	TestFalse(TEXT("An empty remainder shows no banner"),
		CrowdyGameModelDelete::RemainderAppliesTo(Nothing, Ops));

	// One entity of the same name on another model is not the same operation, so a plan that swapped models does
	// not adopt the previous one's leftovers.
	TArray<FCrowdyDeleteMark> Elsewhere;
	Elsewhere.Add(CrowdyGameModelDelete::MarkFunction(TEXT("Mage"), TEXT("regen"), TEXT("regen")));
	TestFalse(TEXT("A same-named entity on another model is not the one left over"),
		CrowdyGameModelDelete::RemainderAppliesTo(
			CrowdyGameModelDelete::Remainder(Ops, 1), CrowdyGameModelDelete::BuildOps(Elsewhere, Evidence)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
