#include "SCrowdyExecDeveloperCall.h"

#include "CrowdyExecDeveloperCall.h"
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
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("InstanceId", "Instance Id"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(KeyBox, SEditableTextBox).HintText(LOCTEXT("InstanceIdHint", "the Server Object's Instance Id, such as boss-1"))
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
	return Found && CrowdyExecDeveloperCall::IsForDevelopers(*Found) ? Found : nullptr;
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
	bCalling = true;
	Answer = LOCTEXT("Calling", "Calling...");
	AnswerColor = FSlateColor::UseSubduedForeground();
	const TWeakPtr<SCrowdyExecDeveloperCall> WeakThis = SharedThis(this);
	CrowdyExecDeveloperCall::Call(*Client, *Current, Function, KeyBox->GetText().ToString().TrimStartAndEnd(), Values,
		[WeakThis](const CrowdyExecDeveloperCall::FResult& Result)
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
				Self->Answer = Result.ReplyJson.IsEmpty() ? LOCTEXT("OkNoReply", "Ok") : FText::Format(LOCTEXT("OkReply", "Ok\n{0}"), FText::FromString(Result.ReplyJson));
				return;
			}
			Self->Answer = FText::Format(LOCTEXT("Refused", "{0}: {1}"), FText::FromString(Result.Status), FText::FromString(Result.Message));
		});
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
