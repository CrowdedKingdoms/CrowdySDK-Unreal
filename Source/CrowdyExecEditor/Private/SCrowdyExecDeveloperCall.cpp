#include "SCrowdyExecDeveloperCall.h"

#include "CrowdyExecDeveloperCall.h"
#include "CrowdyGameKitTypes.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerObjectDefinition.h"
#include "Framework/Application/SlateApplication.h"
#include "IStructureDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/StructOnScope.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCrowdyExecDeveloperCall"

void SCrowdyExecDeveloperCall::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;
	AnswerColor = FSlateColor::UseSubduedForeground();
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const FCrowdyServerFunction* Function = Current ? Current->FindFunction(InArgs._FunctionName) : nullptr;
	FunctionName = Function ? Function->Name : NAME_None;

	TSharedRef<SWidget> InputsWidget = SNew(STextBlock).Text(LOCTEXT("NoInputs", "It takes no inputs.")).ColorAndOpacity(FSlateColor::UseSubduedForeground());
	FInstancedStruct Start;
	if (Function)
	{
		Function->InitializeParams(Start);
	}
	if (Start.IsValid())
	{
		InputsStruct.Reset(const_cast<UScriptStruct*>(Start.GetScriptStruct()));
		Inputs = MakeShared<FStructOnScope>(Start.GetScriptStruct());
		Start.GetScriptStruct()->CopyScriptStruct(Inputs->GetStructMemory(), Start.GetMemory());
		FDetailsViewArgs ViewArgs;
		ViewArgs.bAllowSearch = false;
		ViewArgs.bHideSelectionTip = true;
		ViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		InputsView = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").CreateStructureDetailView(ViewArgs, FStructureDetailsViewArgs(), Inputs);
		InputsWidget = InputsView->GetWidget().ToSharedRef();
	}

	const FText Title = Function ? FText::Format(LOCTEXT("Heading", "Call {0} on {1} as the developer"), FText::FromName(Function->Name), FText::FromString(Current->TypeName))
		: LOCTEXT("NoFunction", "The function is no longer in the definition.");
	const UScriptStruct* InputsType = Start.GetScriptStruct();
	const bool bKit = Current && Current->IsGameKitStarter();
	const FStructProperty* BundleField = nullptr;
	bool bTakesRef = false;
	if (InputsType && bKit)
	{
		for (TFieldIterator<FProperty> It(InputsType); It; ++It)
		{
			bTakesRef |= It->IsA<FStrProperty>() && It->GetAuthoredName().Equals(TEXT("Ref"), ESearchCase::IgnoreCase);
			const FStructProperty* Field = CastField<FStructProperty>(*It);
			BundleField = Field && Field->Struct == FCrowdyKitBundle::StaticStruct() ? Field : BundleField;
		}
	}
	const bool bKeyedByPlayer = Current && Current->IsKeyedByPlayer();
	FText Notes;
	if (BundleField)
	{
		Notes = LOCTEXT("SoftCurrency", "In-game soft currency and items, which are game state like a score: never real money.");
	}
	if (bTakesRef)
	{
		Notes = FText::Format(LOCTEXT("FreshRef", "{0}{1}Leave Ref empty and each press makes a fresh one, so each press gives again. A Ref sent before answers as it did then and gives nothing more, which makes it safe to send again after a lost answer."),
			Notes, Notes.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT(" ")));
	}
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock).Text(Title).Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("Explain", "Runs it on the live app now, as a developer tool: the server code sees the call come from you, not a player, and whatever it changes stays changed."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock).AutoWrapText(true).Visibility(Notes.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible).Text(Notes)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).Text(bKeyedByPlayer ? LOCTEXT("Player", "Player") : LOCTEXT("InstanceId", "Instance Id"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(KeyBox, SEditableTextBox).HintText(bKeyedByPlayer ? LOCTEXT("PlayerHint", "the player's user id, which is their player hub's Instance Id")
						: LOCTEXT("InstanceIdHint", "the Server Object's Instance Id, such as boss-1"))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				InputsWidget
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Call", "Call"))
				.IsEnabled(this, &SCrowdyExecDeveloperCall::CanCall)
				.OnClicked(this, &SCrowdyExecDeveloperCall::OnCallClicked)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.Text_Lambda([this]() { return Answer; })
				.ForegroundColor_Lambda([this]() { return AnswerColor; })
			]
		]
	];
}

void SCrowdyExecDeveloperCall::Open(UCrowdyServerObjectDefinition& InDefinition, FName InFunctionName)
{
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Call as Developer"))
		.ClientSize(FVector2D(520.0f, 480.0f))
		[
			SNew(SCrowdyExecDeveloperCall).Definition(&InDefinition).FunctionName(InFunctionName)
		];
	FSlateApplication::Get().AddWindow(Window);
}

const FCrowdyServerFunction* SCrowdyExecDeveloperCall::FindFunction() const
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const FCrowdyServerFunction* Found = Current ? Current->FindFunction(FunctionName) : nullptr;
	return Found && CrowdyExecDeveloperCall::IsForDevelopers(*Current, *Found) ? Found : nullptr;
}

bool SCrowdyExecDeveloperCall::CanCall() const
{
	return !bCalling && FindFunction() != nullptr;
}

FReply SCrowdyExecDeveloperCall::OnCallClicked()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const FCrowdyServerFunction* Found = FindFunction();
	if (bCalling || !Current || !Found)
	{
		return FReply::Handled();
	}
	const FCrowdyServerFunction& Function = *Found;
	if (Function.GetParamsStruct() != InputsStruct.Get())
	{
		Answer = LOCTEXT("InputsChanged", "The function's inputs changed since this window opened; close it and open it again.");
		AnswerColor = FStyleColors::Error;
		return FReply::Handled();
	}
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	Compute.RenewClientIfStale();
	const TSharedPtr<CrowdyExecDeveloper::FClient> Client = Compute.GetClient();
	if (!Client)
	{
		Answer = FText::FromString(Compute.GetClientError());
		AnswerColor = FStyleColors::Error;
		return FReply::Handled();
	}
	FInstancedStruct Values;
	if (Inputs.IsValid())
	{
		Values.InitializeAs(Function.GetParamsStruct(), Inputs->GetStructMemory());
	}
	const FString Key = KeyBox->GetText().ToString().TrimStartAndEnd();
	const FString Pointless = CrowdyExecDeveloperCall::FindPointlessCall(*Current, Key, Values);
	if (!Pointless.IsEmpty())
	{
		Answer = FText::FromString(Pointless);
		AnswerColor = FStyleColors::Error;
		return FReply::Handled();
	}
	// The inputs view keeps an empty Ref, so the next press makes another.
	FString Ref;
	CrowdyExecDeveloperCall::FillFreshRef(*Current, Function, Values, Ref);
	const FText RefLine = Ref.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("RefLine", "Ref {0}\n"), FText::FromString(Ref));
	bCalling = true;
	Answer = FText::Format(LOCTEXT("Calling", "{0}Calling..."), RefLine);
	AnswerColor = FSlateColor::UseSubduedForeground();
	const TWeakPtr<SCrowdyExecDeveloperCall> WeakThis = SharedThis(this);
	CrowdyExecDeveloperCall::Call(*Client, *Current, Function, Key, Values,
		[WeakThis, RefLine](const CrowdyExecDeveloperCall::FResult& Result)
		{
			const TSharedPtr<SCrowdyExecDeveloperCall> Self = WeakThis.Pin();
			if (!Self)
			{
				return;
			}
			Self->bCalling = false;
			Self->AnswerColor = Result.bOk ? FStyleColors::Success : FStyleColors::Error;
			if (Result.bOk)
			{
				Self->Answer = Result.ReplyJson.IsEmpty() ? FText::Format(LOCTEXT("OkNoReply", "{0}Ok"), RefLine)
					: FText::Format(LOCTEXT("OkReply", "{0}Ok\n{1}"), RefLine, FText::FromString(Result.ReplyJson));
				return;
			}
			Self->Answer = FText::Format(LOCTEXT("Refused", "{0}{1}: {2}"), RefLine, FText::FromString(Result.Status), FText::FromString(Result.Message));
		});
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
