#include "SCrowdyServerCodeTab.h"

#include "CrowdyExecCodegen.h"
#include "CrowdyExecCodegenMenu.h"
#include "CrowdyRustSyntaxMarshaller.h"
#include "CrowdyServerComputeService.h"
#include "DesktopPlatformModule.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/UnrealType.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SCrowdyServerCodeTab"

namespace SCrowdyServerCodeTabDetail
{
	const FString LogicFilePath(TEXT("src/logic.rs"));
	const FString TypesFilePath(TEXT("src/types.rs"));
	const FMargin CodePadding(6.0f, 4.0f, 2.0f, 2.0f);
	constexpr float GutterMargin = 8.0f;
	constexpr float DiffNumberWidth = 40.0f;

	FSlateFontInfo CodeFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Mono", 9);
	}

	FString ProjectFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	FString ShownPathOf(const FString& Path)
	{
		const FString ProjectDirectory = ProjectFolder();
		FString Shown = Path;
		if (!Path.IsEmpty() && FPaths::IsUnderDirectory(Path, ProjectDirectory))
		{
			FPaths::MakePathRelativeTo(Shown, *ProjectDirectory);
		}
		return Shown;
	}

	/** The generated logic file's path comes from the Type Name, so only a valid Type Name makes one; an own file with none chosen has none. */
	FString LogicPathOf(const UCrowdyServerObjectDefinition& Definition)
	{
		if (!CrowdyExecCodegen::HasOwnLogicFile(Definition) && !CrowdyServerCodeFiles::IsValidTypeName(Definition.TypeName))
		{
			return FString();
		}
		return CrowdyExecCodegen::GetLogicFile(Definition);
	}

	FText WriteBlockReasonOf(const UCrowdyServerObjectDefinition& Definition)
	{
		if (!CrowdyServerCodeFiles::IsValidTypeName(Definition.TypeName))
		{
			return LOCTEXT("InvalidTypeName", "Set a valid Type Name first: lowercase letters, digits and underscores, starting with a letter, at most 48 characters");
		}
		if (!CrowdyExecCodegen::HasOwnLogicFile(Definition) && !FPaths::IsUnderDirectory(CrowdyExecCodegen::GetLogicFile(Definition), ProjectFolder()))
		{
			return LOCTEXT("OutsideProject", "The logic file must be inside the project folder");
		}
		return FText::GetEmpty();
	}

	/** True when the revision's generated types are what Generate Server Code writes for the definition now. */
	bool IsBuiltFromDefinition(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecRevisions::FRevision& Revision)
	{
		const CrowdyExecDeveloper::FBuildFile* Types = Revision.FindFile(TypesFilePath);
		CrowdyExecCodegen::FGeneratedCrate Crate;
		FString Error;
		if (!Types || !CrowdyExecCodegen::Generate(Definition, Crate, Error))
		{
			return false;
		}
		const CrowdyExecCodegen::FGeneratedFile* Generated = Crate.Files.FindByPredicate([](const CrowdyExecCodegen::FGeneratedFile& File) { return File.Path == TypesFilePath; });
		return Generated && CrowdyServerCodeFiles::IsSameCode(Generated->Text, Types->Content);
	}

	FText SourceLabel(ECrowdyServerCodeSource Source)
	{
		return Source == ECrowdyServerCodeSource::OwnFile ? LOCTEXT("SourceOwnFile", "My Own File") : LOCTEXT("SourceGenerated", "Generated");
	}

	int32 CountLines(const FString& Text)
	{
		int32 Count = 1;
		for (const TCHAR Char : Text)
		{
			Count += Char == '\n' ? 1 : 0;
		}
		return Count;
	}

	FText DialogTitle()
	{
		return LOCTEXT("DialogTitle", "Server Code");
	}

	bool AskYesNo(const FText& Message)
	{
		return FMessageDialog::Open(EAppMsgCategory::Warning, EAppMsgType::YesNo, EAppReturnType::No, Message, DialogTitle()) == EAppReturnType::Yes;
	}

	TSharedRef<SWidget> Notice(FName Icon, const TSharedRef<SWidget>& Content)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(FMargin(8.0f, 5.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(Icon))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					Content
				]
			];
	}

	TSharedRef<SMultiLineEditableTextBox> CodeBox(const FEditableTextBoxStyle& Style, const TAttribute<bool>& bReadOnly, const FOnTextChanged& OnTextChanged)
	{
		return SNew(SMultiLineEditableTextBox)
			.Style(&Style)
			.Marshaller(FCrowdyRustSyntaxMarshaller::Create(CodeFont()))
			.AutoWrapText(false)
			.IsReadOnly(bReadOnly)
			.OnTextChanged(OnTextChanged);
	}
}

/** Line numbers beside a code box, scrolled with it. */
class SCrowdyServerCodeGutter : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyServerCodeGutter) {}
		SLATE_ATTRIBUTE(int32, LineCount)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SMultiLineEditableTextBox>& InTextBox)
	{
		LineCount = InArgs._LineCount;
		TextBox = InTextBox;
		Font = SCrowdyServerCodeTabDetail::CodeFont();
		DigitWidth = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(TEXT("0"), Font).X;
		SetClipping(EWidgetClipping::ClipToBounds);
	}

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		int32 Digits = 3;
		for (int32 Count = LineCount.Get(); Count >= 1000; Count /= 10)
		{
			++Digits;
		}
		return FVector2D(Digits * DigitWidth + 2.0f * SCrowdyServerCodeTabDetail::GutterMargin, 0.0f);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const TSharedPtr<SMultiLineEditableTextBox> Box = TextBox.Pin();
		const TSharedPtr<const SScrollBar> Bar = Box.IsValid() ? Box->GetVScrollBar() : nullptr;
		const float Scale = AllottedGeometry.Scale;
		if (Scale != MeasuredScale)
		{
			// The text box lays lines out in whole pixels at the current scale, so the gutter measures at that scale too.
			MeasuredScale = Scale;
			LineHeight = FMath::Max(1.0f, FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->GetMaxCharacterHeight(Font, Scale) / Scale);
		}
		const int32 Count = FMath::Max(1, LineCount.Get());
		const float Offset = Bar.IsValid() ? Bar->DistanceFromTop() * Count * LineHeight : 0.0f;
		const FVector2f Size = AllottedGeometry.GetLocalSize();
		const FLinearColor Ink = FStyleColors::White25.GetSpecifiedColor() * InWidgetStyle.GetColorAndOpacityTint();
		for (int32 Line = FMath::Max(0, FMath::FloorToInt(Offset / LineHeight)); Line < Count; ++Line)
		{
			const float Top = SCrowdyServerCodeTabDetail::CodePadding.Top + Line * LineHeight - Offset;
			if (Top > Size.Y)
			{
				break;
			}
			while (Numbers.Num() <= Line)
			{
				Numbers.Add(FString::FromInt(Numbers.Num() + 1));
			}
			const float Width = Numbers[Line].Len() * DigitWidth;
			const FVector2f Position(Size.X - SCrowdyServerCodeTabDetail::GutterMargin - Width, Top);
			FSlateDrawElement::MakeText(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(FVector2f(Width, LineHeight), FSlateLayoutTransform(Position)),
				Numbers[Line], Font, ESlateDrawEffect::None, Ink);
		}
		return LayerId;
	}

private:
	TAttribute<int32> LineCount;
	TWeakPtr<SMultiLineEditableTextBox> TextBox;
	FSlateFontInfo Font;
	float DigitWidth = 7.0f;
	mutable float MeasuredScale = 0.0f;
	mutable float LineHeight = 14.0f;
	/** Each line's number as text, made once. */
	mutable TArray<FString> Numbers;
};

SCrowdyServerCodeTab::~SCrowdyServerCodeTab()
{
	CrowdyServerCodeEdits::RemoveTab(*this);
	if (const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin())
	{
		Pinned->OnChanged().Remove(ServiceChangedHandle);
	}
	// Edits still unsaved when the editor closes come back the next time the file is shown.
	StashEdits();
}

void SCrowdyServerCodeTab::Construct(const FArguments& InArgs)
{
	Definition = InArgs._Definition;
	OnCodeWritten = InArgs._OnCodeWritten;
	Service = FCrowdyServerComputeService::Get().AsShared();
	SourceItems = {MakeShared<ECrowdyServerCodeSource>(ECrowdyServerCodeSource::Generated), MakeShared<ECrowdyServerCodeSource>(ECrowdyServerCodeSource::OwnFile)};
	CodeBoxStyle = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
	const FSlateNoResource NoBackground;
	CodeBoxStyle.SetBackgroundImageNormal(NoBackground).SetBackgroundImageHovered(NoBackground).SetBackgroundImageFocused(NoBackground).SetBackgroundImageReadOnly(NoBackground)
		.SetPadding(SCrowdyServerCodeTabDetail::CodePadding).SetFont(SCrowdyServerCodeTabDetail::CodeFont());

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeHeader()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRevisionBanner()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeMissingNotice()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeChangedNotice()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeGapsNotice()
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			MakeCodeViews()
		]
	];

	CrowdyServerCodeEdits::AddTab(*this);
	if (const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin())
	{
		ServiceChangedHandle = Pinned->OnChanged().AddSP(this, &SCrowdyServerCodeTab::HandleServiceChanged);
		SeenGeneration = Pinned->GetDataGeneration();
	}
	RefreshCached();
	Reload();
	RefreshState();
	RebuildRevisions();
	RefreshExpected();
	RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyServerCodeTab::HandleDiskTimer));
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeHeader()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SCrowdyServerCodeTab::GetPathText)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.ToolTipText(LOCTEXT("PathToolTip", "The file holding this type's Server Functions, relative to the project folder"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SCrowdyServerCodeTab::GetUnsavedText)
				.Visibility(this, &SCrowdyServerCodeTab::GetUnsavedVisibility)
				.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
				.ColorAndOpacity(FStyleColors::Warning)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(SourceCombo, SComboBox<FSourceItemPtr>)
				.OptionsSource(&SourceItems)
				.OnGenerateWidget(this, &SCrowdyServerCodeTab::MakeSourceRow)
				.OnSelectionChanged(this, &SCrowdyServerCodeTab::HandleSourcePicked)
				.ToolTipText(LOCTEXT("SourceToolTip", "Where this type's Server Functions come from: the logic.rs Generate writes once and never replaces, or a .rs file of your own"))
				[
					SNew(STextBlock)
					.Text(this, &SCrowdyServerCodeTab::GetSourceText)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility(this, &SCrowdyServerCodeTab::GetChooseFileVisibility)
				[
					MakeIconButton("Icons.FolderOpen", LOCTEXT("ChooseFileToolTip", "Chooses your .rs file"), true, FOnClicked::CreateSP(this, &SCrowdyServerCodeTab::HandleChooseFile))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(RevisionCombo, SComboBox<FRevisionItemPtr>)
				.Visibility(this, &SCrowdyServerCodeTab::GetRevisionsListVisibility)
				.OptionsSource(&RevisionItems)
				.OnGenerateWidget(this, &SCrowdyServerCodeTab::MakeRevisionRow)
				.OnSelectionChanged(this, &SCrowdyServerCodeTab::HandleRevisionPicked)
				.IsEnabled(this, &SCrowdyServerCodeTab::HasHistory)
				.ToolTipText(LOCTEXT("RevisionsToolTip", "This type's code as it was deployed, newest first: view it, compare it with yours, or restore it"))
				[
					SNew(STextBlock)
					.Text(this, &SCrowdyServerCodeTab::GetRevisionComboText)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				MakeIconButton("Icons.Save", TAttribute<FText>::CreateSP(this, &SCrowdyServerCodeTab::GetSaveToolTip), TAttribute<bool>::CreateSP(this, &SCrowdyServerCodeTab::CanSave),
					FOnClicked::CreateSP(this, &SCrowdyServerCodeTab::HandleSave))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				MakeIconButton("SourceControl.Actions.Revert", LOCTEXT("RevertToolTip", "Shows the logic file as it is on disk again"), true,
					FOnClicked::CreateSP(this, &SCrowdyServerCodeTab::HandleRevert))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				MakeIconButton("Icons.OpenInExternalEditor", TAttribute<FText>::CreateSP(this, &SCrowdyServerCodeTab::GetOpenExternalToolTip),
					TAttribute<bool>::CreateSP(this, &SCrowdyServerCodeTab::IsFilePresent), FOnClicked::CreateSP(this, &SCrowdyServerCodeTab::HandleOpenExternal))
			]
		];
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeIconButton(FName Icon, const TAttribute<FText>& ToolTip, const TAttribute<bool>& bEnabled, FOnClicked OnClicked)
{
	return SNew(SButton)
		.ButtonStyle(&FAppStyle::Get(), "SimpleButton")
		.ContentPadding(FMargin(4.0f, 2.0f))
		.ToolTipText(ToolTip)
		.IsEnabled(bEnabled)
		.OnClicked(OnClicked)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush(Icon))
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeRevisionBanner()
{
	const TSharedRef<SWidget> Content = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(this, &SCrowdyServerCodeTab::GetViewedBanner)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SCrowdyServerCodeTab::GetViewedWarning)
				.Visibility(this, &SCrowdyServerCodeTab::GetViewedWarningVisibility)
				.AutoWrapText(true)
				.ColorAndOpacity(FStyleColors::Warning)
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(this, &SCrowdyServerCodeTab::GetCompareText)
			.ToolTipText(LOCTEXT("CompareToolTip", "Shows the lines this revision has that your code does not (red) and the lines your code adds (green)"))
			.OnClicked(this, &SCrowdyServerCodeTab::HandleCompare)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Restore", "Restore"))
			.ToolTipText(this, &SCrowdyServerCodeTab::GetRestoreToolTip)
			.IsEnabled(this, &SCrowdyServerCodeTab::CanRestore)
			.OnClicked(this, &SCrowdyServerCodeTab::HandleRestore)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("BackToYourCode", "Back to your code"))
			.OnClicked(this, &SCrowdyServerCodeTab::HandleBackToYourCode)
		];
	return SNew(SBox)
		.Visibility(this, &SCrowdyServerCodeTab::GetRevisionVisibility)
		[
			SCrowdyServerCodeTabDetail::Notice("Icons.Info", Content)
		];
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeMissingNotice()
{
	const TSharedRef<SWidget> Content = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SCrowdyServerCodeTab::GetMissingText)
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("GenerateMissing", "Generate"))
			.ToolTipText(this, &SCrowdyServerCodeTab::GetGenerateToolTip)
			.IsEnabled(this, &SCrowdyServerCodeTab::CanGenerate)
			.Visibility(this, &SCrowdyServerCodeTab::GetMissingGenerateVisibility)
			.OnClicked(this, &SCrowdyServerCodeTab::HandleGenerate)
		];
	return SNew(SBox)
		.Visibility(this, &SCrowdyServerCodeTab::GetMissingVisibility)
		[
			SCrowdyServerCodeTabDetail::Notice("Icons.WarningWithColor", Content)
		];
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeChangedNotice()
{
	const TSharedRef<SWidget> Content = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SCrowdyServerCodeTab::GetChangedText)
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ReloadFromDisk", "Reload"))
			.ToolTipText(LOCTEXT("ReloadFromDiskToolTip", "Discards your unsaved changes and shows the file from disk"))
			.OnClicked(this, &SCrowdyServerCodeTab::HandleReloadFromDisk)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("KeepMine", "Keep Mine"))
			.ToolTipText(LOCTEXT("KeepMineToolTip", "Keeps your changes; Save asks before it overwrites the file"))
			.OnClicked(this, &SCrowdyServerCodeTab::HandleKeepMine)
		];
	return SNew(SBox)
		.Visibility(this, &SCrowdyServerCodeTab::GetChangedVisibility)
		[
			SCrowdyServerCodeTabDetail::Notice("Icons.WarningWithColor", Content)
		];
}

EActiveTimerReturnType SCrowdyServerCodeTab::HandleDiskTimer(double CurrentTime, float DeltaTime)
{
	FollowDisk();
	return EActiveTimerReturnType::Continue;
}

void SCrowdyServerCodeTab::FollowDisk()
{
	if (LoadedPath.IsEmpty())
	{
		return;
	}
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*LoadedPath);
	if (Stamp == DiskStamp)
	{
		return;
	}
	DiskStamp = Stamp;
	if (bDirty)
	{
		bStale = true;
		bChangedOnDisk = true;
		return;
	}
	const FTextLocation Cursor = TextBox.IsValid() ? TextBox->GetCursorLocation() : FTextLocation();
	Reload();
	RefreshState();
	if (TextBox.IsValid())
	{
		TextBox->GoTo(Cursor);
	}
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeGapsNotice()
{
	const TSharedRef<SWidget> Content = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SCrowdyServerCodeTab::GetGapsText)
			.AutoWrapText(true)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(this, &SCrowdyServerCodeTab::GetAddStubsText)
			.ToolTipText(this, &SCrowdyServerCodeTab::GetAddStubsToolTip)
			.IsEnabled(this, &SCrowdyServerCodeTab::CanAddStubs)
			.Visibility(this, &SCrowdyServerCodeTab::GetAddStubsVisibility)
			.OnClicked(this, &SCrowdyServerCodeTab::HandleAddStubs)
		];
	return SNew(SBox)
		.Visibility(this, &SCrowdyServerCodeTab::GetGapsVisibility)
		[
			SCrowdyServerCodeTabDetail::Notice("Icons.WarningWithColor", Content)
		];
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeCodeViews()
{
	using namespace SCrowdyServerCodeTabDetail;
	TextBox = CodeBox(CodeBoxStyle, TAttribute<bool>::CreateSP(this, &SCrowdyServerCodeTab::IsReadOnly), FOnTextChanged::CreateSP(this, &SCrowdyServerCodeTab::HandleTextChanged));
	RevisionTextBox = CodeBox(CodeBoxStyle, true, FOnTextChanged());
	return SNew(SWidgetSwitcher)
		.WidgetIndex(this, &SCrowdyServerCodeTab::GetCodeViewIndex)
		+ SWidgetSwitcher::Slot()
		[
			MakeCodeView(TextBox.ToSharedRef(), TAttribute<int32>::CreateSP(this, &SCrowdyServerCodeTab::GetEditedLineCount))
		]
		+ SWidgetSwitcher::Slot()
		[
			MakeCodeView(RevisionTextBox.ToSharedRef(), TAttribute<int32>::CreateSP(this, &SCrowdyServerCodeTab::GetViewedLineCount))
		]
		+ SWidgetSwitcher::Slot()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(0.0f)
			[
				SAssignNew(DiffList, SListView<FDiffRowPtr>)
				.ListItemsSource(&DiffRows)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SCrowdyServerCodeTab::MakeDiffRow)
			]
		];
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeCodeView(const TSharedRef<SMultiLineEditableTextBox>& Box, const TAttribute<int32>& LineCount)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Background"))
				.Padding(0.0f)
				[
					SNew(SCrowdyServerCodeGutter, Box)
					.LineCount(LineCount)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				Box
			]
		];
}

void SCrowdyServerCodeTab::HandleDefinitionChanged()
{
	using namespace SCrowdyServerCodeTabDetail;
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	const bool bTypeNameChanged = !Current->TypeName.Equals(CachedTypeName, ESearchCase::CaseSensitive);
	RefreshCached();
	if (bTypeNameChanged)
	{
		RebuildRevisions();
	}
	RefreshServiceTypes();
	if (LogicPathOf(*Current).Equals(LoadedPath, ESearchCase::CaseSensitive))
	{
		bOwnFile = CrowdyExecCodegen::HasOwnLogicFile(*Current);
		RefreshState();
		RefreshExpected();
		return;
	}
	const bool bSave = bDirty && AskYesNo(FText::Format(LOCTEXT("SaveBeforeSwitch",
		"Save your changes to {0}? If not, they stay unsaved and come back when this editor shows {0} again."), FText::FromString(ShownPath)));
	if (!bSave || !SaveEdits(false))
	{
		StashEdits();
	}
	Reload();
	RefreshState();
	RefreshExpected();
}

void SCrowdyServerCodeTab::Generate()
{
	using namespace SCrowdyServerCodeTabDetail;
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current || !CanGenerate())
	{
		return;
	}
	if (bDirty && AskYesNo(FText::Format(LOCTEXT("SaveBeforeGenerate", "Save your changes to {0} first? Generating never replaces an existing logic file."), FText::FromString(ShownPath))))
	{
		SaveEdits(true);
	}
	CrowdyExecCodegenMenu::GenerateServerCode(*Current);
	RefreshServiceTypes();
	if (!bDirty)
	{
		Reload();
		RefreshState();
		RefreshExpected();
		NotifyCodeWritten();
		return;
	}
	const CrowdyServerCodeFiles::FDiskText OnDisk = CrowdyServerCodeFiles::Read(LoadedPath);
	bFileExists = OnDisk.bExists;
	bFileReadable = OnDisk.bReadable;
	bStale = !CrowdyServerCodeFiles::IsSame(OnDisk, Loaded);
	RefreshState();
	RefreshExpected();
	NotifyCodeWritten();
}

FText SCrowdyServerCodeTab::GetGenerateToolTip() const
{
	return WriteBlockReason.IsEmpty() ? LOCTEXT("GenerateToolTip", "Writes this type's server code under Server/<Type Name>; your logic file is never replaced") : WriteBlockReason;
}

void SCrowdyServerCodeTab::OfferSaveBeforeDeploy()
{
	if (bDirty && SCrowdyServerCodeTabDetail::AskYesNo(FText::Format(LOCTEXT("SaveBeforeDeploy", "Save your changes to {0} first? A deploy sends the files as they are saved."), FText::FromString(ShownPath))))
	{
		SaveEdits(true);
	}
}

bool SCrowdyServerCodeTab::RevealFunction(const FString& Method)
{
	if (!TextBox.IsValid() || Method.IsEmpty())
	{
		return false;
	}
	TArray<FString> Lines;
	TextBox->GetText().ToString().ParseIntoArrayLines(Lines, false);
	const FString Needle = TEXT("fn ") + Method + TEXT("(");
	for (int32 Line = 0; Line < Lines.Num(); ++Line)
	{
		const int32 Column = Lines[Line].Find(Needle, ESearchCase::CaseSensitive);
		if (Column == INDEX_NONE)
		{
			continue;
		}
		const FTextLocation Location(Line, Column + 3);
		TextBox->GoTo(Location);
		TextBox->ScrollTo(Location);
		FSlateApplication::Get().SetKeyboardFocus(TextBox, EFocusCause::SetDirectly);
		return true;
	}
	return false;
}

FString SCrowdyServerCodeTab::GetUnsavedFile() const
{
	return bDirty ? LoadedPath : FString();
}

void SCrowdyServerCodeTab::ShowWritten(const FString& Path)
{
	if (bDirty || LoadedPath.IsEmpty() || !FPaths::IsSamePath(Path, LoadedPath))
	{
		return;
	}
	Reload();
	RefreshState();
}

void SCrowdyServerCodeTab::ForgetEditsUnder(const FString& Directory)
{
	if (LoadedPath.IsEmpty() || !FPaths::IsUnderDirectory(LoadedPath, Directory))
	{
		return;
	}
	DiscardEdits();
}

void SCrowdyServerCodeTab::EditDefinition(FName Member, const FText& Description, TFunctionRef<void(UCrowdyServerObjectDefinition&)> Edit)
{
	UCrowdyServerObjectDefinition* Current = Definition.Get();
	FProperty* Property = FindFProperty<FProperty>(UCrowdyServerObjectDefinition::StaticClass(), Member);
	if (!Current || !Property)
	{
		return;
	}
	const FScopedTransaction Transaction(Description);
	Current->PreEditChange(Property);
	Edit(*Current);
	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Current->PostEditChangeProperty(Event);
}

void SCrowdyServerCodeTab::RefreshCached()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	CachedTypeName = Current->TypeName;
	bTypeNameValid = CrowdyServerCodeFiles::IsValidTypeName(CachedTypeName);
	bSharedTypeName = IsTypeNameShared();
	bOwnFile = CrowdyExecCodegen::HasOwnLogicFile(*Current);
	// Kept in step with the definition, so picking the other source after an undo still counts as a change.
	if (SourceCombo.IsValid())
	{
		SourceCombo->SetSelectedItem(SourceItems[bOwnFile ? 1 : 0]);
	}
}

void SCrowdyServerCodeTab::RebuildRevisions()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	const FString ViewedFingerprint = History.IsValidIndex(ViewedRevision) ? History[ViewedRevision].Fingerprint : FString();
	History.Reset();
	if (Pinned.IsValid() && bTypeNameValid && !bSharedTypeName)
	{
		History = Pinned->GetHistory(CachedTypeName);
	}
	const CrowdyExecRevisions::FTypeChange* Change = Pinned.IsValid() ? Pinned->FindChange(CachedTypeName) : nullptr;
	const int32 LiveRevision = Change ? Change->LiveRevision : INDEX_NONE;
	const int32 LiveVersion = Change ? Change->LiveVersion : 0;
	const FDateTime Now = FDateTime::UtcNow();
	RevisionItems.Reset();
	RevisionItems.Add(MakeShared<FRevisionItem>(FRevisionItem{INDEX_NONE, 0, LOCTEXT("YourCode", "Your code")}));
	for (int32 Index = 0; Index < History.Num(); ++Index)
	{
		const bool bLive = Index == LiveRevision;
		const TArray<int32>& Versions = History[Index].Versions;
		const int32 Version = bLive && LiveVersion > 0 ? LiveVersion : (Versions.IsEmpty() ? 0 : Versions[0]);
		RevisionItems.Add(MakeShared<FRevisionItem>(FRevisionItem{Index, Version, CrowdyServerCodeFiles::RevisionLabel(Version, bLive, History[Index].DeployedAt, Now)}));
	}
	if (RevisionCombo.IsValid())
	{
		RevisionCombo->RefreshOptions();
	}
	const int32 Kept = ViewedFingerprint.IsEmpty() ? INDEX_NONE
		: History.IndexOfByPredicate([&ViewedFingerprint](const CrowdyExecRevisions::FRevision& Revision) { return Revision.Fingerprint == ViewedFingerprint; });
	if (Kept == INDEX_NONE)
	{
		ShowYourCode();
		return;
	}
	const bool bWasComparing = bComparing;
	ShowRevision(Kept);
	if (bWasComparing)
	{
		SetComparing(true);
	}
}

void SCrowdyServerCodeTab::ShowRevision(int32 Index)
{
	using namespace SCrowdyServerCodeTabDetail;
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current || !History.IsValidIndex(Index) || !RevisionItems.IsValidIndex(Index + 1))
	{
		ShowYourCode();
		return;
	}
	const CrowdyExecRevisions::FRevision& Revision = History[Index];
	const FRevisionItemPtr& Item = RevisionItems[Index + 1];
	const CrowdyExecDeveloper::FBuildFile* Logic = Revision.FindFile(LogicFilePath);
	ViewedRevision = Index;
	bComparing = false;
	bViewedHasLogic = Logic != nullptr;
	ViewedLogic = Logic ? CrowdyServerCodeFiles::WithLineFeeds(Logic->Content) : FString();
	ViewedBanner = FText::Format(LOCTEXT("ViewingRevision", "Viewing version {0}'s code (deployed {1})"), CrowdyServerCodeFiles::VersionText(Item->Version),
		CrowdyServerCodeFiles::TimeAgo(Revision.DeployedAt, FDateTime::UtcNow()));
	ViewedWarning = IsBuiltFromDefinition(*Current, Revision) ? FText::GetEmpty()
		: LOCTEXT("OtherDefinition", "This revision was built from a different definition, so its code may not compile with the types Generate writes now.");
	RevisionComboText = Item->Label;
	const FString Shown = bViewedHasLogic ? ViewedLogic : LOCTEXT("RevisionNoLogic", "This revision has no src/logic.rs.").ToString();
	ViewedLineCount = CountLines(Shown);
	RevisionTextBox->SetText(FText::FromString(Shown));
	RevisionCombo->SetSelectedItem(Item);
}

void SCrowdyServerCodeTab::ShowYourCode()
{
	ViewedRevision = INDEX_NONE;
	bComparing = false;
	bViewedHasLogic = false;
	ViewedLogic.Reset();
	DiffRows.Reset();
	RevisionComboText = History.IsEmpty() ? LOCTEXT("NoRevisions", "No deployed revisions yet") : LOCTEXT("YourCode", "Your code");
	if (DiffList.IsValid())
	{
		DiffList->RequestListRefresh();
	}
	if (RevisionCombo.IsValid() && !RevisionItems.IsEmpty())
	{
		RevisionCombo->SetSelectedItem(RevisionItems[0]);
	}
}

void SCrowdyServerCodeTab::SetComparing(bool bCompare)
{
	using EKind = CrowdyExecRevisions::FDiffLine::EKind;
	bComparing = bCompare;
	DiffRows.Reset();
	TArray<CrowdyExecRevisions::FDiffLine> Lines = bComparing ? CrowdyExecRevisions::DiffLines(ViewedLogic, EditedText) : TArray<CrowdyExecRevisions::FDiffLine>();
	int32 OldNumber = 0;
	int32 NewNumber = 0;
	for (CrowdyExecRevisions::FDiffLine& Line : Lines)
	{
		OldNumber += Line.Kind != EKind::Added ? 1 : 0;
		NewNumber += Line.Kind != EKind::Removed ? 1 : 0;
		const int32 Number = Line.Kind == EKind::Removed ? OldNumber : NewNumber;
		DiffRows.Add(MakeShared<FDiffRow>(FDiffRow{MoveTemp(Line), Number}));
	}
	if (DiffList.IsValid())
	{
		DiffList->RequestListRefresh();
	}
}

void SCrowdyServerCodeTab::RefreshServiceTypes()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (Pinned.IsValid() && !Pinned->IsBusy())
	{
		Pinned->RequestRefreshTypes();
	}
}

bool SCrowdyServerCodeTab::IsTypeNameShared() const
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	return bTypeNameValid && Pinned.IsValid() && Pinned->IsSharedTypeName(CachedTypeName);
}

void SCrowdyServerCodeTab::Reload()
{
	using namespace SCrowdyServerCodeTabDetail;
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	bOwnFile = CrowdyExecCodegen::HasOwnLogicFile(*Current);
	LoadedPath = LogicPathOf(*Current);
	ShownPath = ShownPathOf(LoadedPath);
	const CrowdyServerCodeFiles::FDiskText OnDisk = LoadedPath.IsEmpty() ? CrowdyServerCodeFiles::FDiskText() : CrowdyServerCodeFiles::Read(LoadedPath);
	const TOptional<CrowdyServerCodeFiles::FEdits> Stashed = LoadedPath.IsEmpty() ? TOptional<CrowdyServerCodeFiles::FEdits>() : CrowdyServerCodeEdits::Kept().Take(LoadedPath);
	CrowdyServerCodeFiles::FShownText Shown = CrowdyServerCodeFiles::ShowText(OnDisk, Stashed.GetPtrOrNull());
	bFileExists = OnDisk.bExists;
	bFileReadable = OnDisk.bReadable;
	Loaded = MoveTemp(Shown.Base);
	bStale = Shown.bStale;
	DiskStamp = LoadedPath.IsEmpty() ? FDateTime::MinValue() : IFileManager::Get().GetTimeStamp(*LoadedPath);
	bChangedOnDisk = false;
	SetEditedText(MoveTemp(Shown.Text));
}

void SCrowdyServerCodeTab::RefreshState()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (!Current)
	{
		return;
	}
	WriteBlockReason = bSharedTypeName ? CrowdyServerCodeFiles::SharedTypeNameProblem() : SCrowdyServerCodeTabDetail::WriteBlockReasonOf(*Current);
}

bool SCrowdyServerCodeTab::SaveEdits(bool bAskIfMoved)
{
	using namespace SCrowdyServerCodeTabDetail;
	if (LoadedPath.IsEmpty() || !GetSaveBlockReason().IsEmpty())
	{
		return false;
	}
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	const bool bMoved = Current && !LogicPathOf(*Current).Equals(LoadedPath, ESearchCase::CaseSensitive);
	if (bAskIfMoved && bMoved && !AskYesNo(FText::Format(LOCTEXT("SaveMoved", "This definition's logic file is no longer {0}. Save your edits to {0} anyway?"), FText::FromString(ShownPath))))
	{
		return false;
	}
	const CrowdyServerCodeFiles::FDiskText OnDisk = CrowdyServerCodeFiles::Read(LoadedPath);
	if (!CrowdyServerCodeFiles::IsSame(OnDisk, Loaded) && !ResolveOutsideChange())
	{
		return false;
	}
	const CrowdyServerCodeFiles::FDiskText& Original = OnDisk.bReadable ? OnDisk : Loaded;
	if (Original.bReadable && !Original.bUtf8
		&& !AskYesNo(FText::Format(LOCTEXT("ConvertToUtf8", "{0} is not UTF-8 text. Saving writes it as UTF-8, which can change characters that are not plain ASCII. Save anyway?"), FText::FromString(ShownPath))))
	{
		return false;
	}
	if (!CrowdyServerCodeFiles::Write(LoadedPath, EditedText, Original.bCrlf))
	{
		const FText Message = FText::Format(LOCTEXT("SaveFailed", "Could not save {0}. Close any program using it and try again."), FText::FromString(LoadedPath));
		FMessageDialog::Open(EAppMsgCategory::Error, EAppMsgType::Ok, Message, DialogTitle());
		return false;
	}
	Loaded.Text = EditedText;
	Loaded.bExists = true;
	Loaded.bReadable = true;
	Loaded.bUtf8 = true;
	Loaded.bCrlf = Original.bCrlf;
	bFileExists = true;
	bFileReadable = true;
	bDirty = false;
	bStale = false;
	DiskStamp = IFileManager::Get().GetTimeStamp(*LoadedPath);
	bChangedOnDisk = false;
	// Edits kept from an earlier close are older than what was just saved.
	CrowdyServerCodeEdits::Kept().Forget(LoadedPath);
	RefreshState();
	RefreshServiceTypes();
	NotifyCodeWritten();
	return true;
}

bool SCrowdyServerCodeTab::ResolveOutsideChange()
{
	using namespace SCrowdyServerCodeTabDetail;
	const FText Message = FText::Format(LOCTEXT("ChangedOnDisk",
		"{0} changed on disk since it was opened here. Overwrite it with your edits?\n\nYes overwrites the file. No discards your edits and shows the file from disk. Cancel keeps your edits unsaved."),
		FText::FromString(ShownPath));
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgCategory::Warning, EAppMsgType::YesNoCancel, EAppReturnType::Cancel, Message, DialogTitle());
	if (Choice == EAppReturnType::Yes)
	{
		return true;
	}
	if (Choice == EAppReturnType::No && AskYesNo(FText::Format(LOCTEXT("ConfirmDiscard", "Discard your edits to {0}? They cannot be recovered."), FText::FromString(ShownPath))))
	{
		DiscardEdits();
	}
	return false;
}

void SCrowdyServerCodeTab::StashEdits()
{
	if (!bDirty || LoadedPath.IsEmpty())
	{
		return;
	}
	CrowdyServerCodeEdits::Kept().Put(LoadedPath, CrowdyServerCodeFiles::FEdits{EditedText, Loaded});
	bDirty = false;
}

void SCrowdyServerCodeTab::DiscardEdits()
{
	CrowdyServerCodeEdits::Kept().Forget(LoadedPath);
	bDirty = false;
	Reload();
	RefreshState();
}

void SCrowdyServerCodeTab::SetEditedText(FString Text)
{
	EditedText = MoveTemp(Text);
	bDirty = !EditedText.Equals(Loaded.Text, ESearchCase::CaseSensitive);
	EditedLineCount = SCrowdyServerCodeTabDetail::CountLines(EditedText);
	if (TextBox.IsValid())
	{
		TextBox->SetText(FText::FromString(EditedText));
	}
	RefreshGaps();
}

void SCrowdyServerCodeTab::RefreshExpected()
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	FString Error;
	// A definition that cannot generate leaves Expected empty, so nothing is compared.
	if (!Current || !CrowdyExecCodegen::Generate(*Current, Expected, Error))
	{
		Expected = CrowdyExecCodegen::FGeneratedCrate();
	}
	RefreshGaps();
}

void SCrowdyServerCodeTab::RefreshGaps()
{
	const bool bCompare = !Expected.StateName.IsEmpty() && !LoadedPath.IsEmpty() && bFileExists && bFileReadable;
	Gaps = bCompare ? CrowdyExecCodegen::FindLogicGaps(Expected, EditedText) : CrowdyExecCodegen::FLogicGaps();
	const bool bShort = !Gaps.Missing.IsEmpty() || !Gaps.Leftover.IsEmpty();
	GapsText = bShort ? CrowdyExecCodegenMenu::DescribeLogicGaps(Gaps, FPaths::GetCleanFilename(LoadedPath), Expected.StateName) : FText::GetEmpty();
}

void SCrowdyServerCodeTab::NotifyCodeWritten()
{
	OnCodeWritten.ExecuteIfBound();
}

void SCrowdyServerCodeTab::HandleServiceChanged()
{
	const TSharedPtr<FCrowdyServerComputeService> Pinned = Service.Pin();
	if (!Pinned.IsValid() || Pinned->GetDataGeneration() == SeenGeneration)
	{
		return;
	}
	SeenGeneration = Pinned->GetDataGeneration();
	if (IsTypeNameShared() != bSharedTypeName)
	{
		bSharedTypeName = !bSharedTypeName;
		RefreshState();
	}
	RebuildRevisions();
}

void SCrowdyServerCodeTab::HandleTextChanged(const FText& NewText)
{
	EditedText = CrowdyServerCodeFiles::WithLineFeeds(NewText.ToString());
	bDirty = !EditedText.Equals(Loaded.Text, ESearchCase::CaseSensitive);
	EditedLineCount = SCrowdyServerCodeTabDetail::CountLines(EditedText);
	RefreshGaps();
}

void SCrowdyServerCodeTab::HandleRevisionPicked(FRevisionItemPtr Item, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct || !Item.IsValid())
	{
		return;
	}
	if (Item->Index == INDEX_NONE)
	{
		ShowYourCode();
		return;
	}
	ShowRevision(Item->Index);
}

void SCrowdyServerCodeTab::HandleSourcePicked(FSourceItemPtr Item, ESelectInfo::Type SelectInfo)
{
	const UCrowdyServerObjectDefinition* Current = Definition.Get();
	if (SelectInfo == ESelectInfo::Direct || !Item.IsValid() || !Current || Current->CodeSource == *Item)
	{
		return;
	}
	const ECrowdyServerCodeSource Source = *Item;
	EditDefinition(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, CodeSource), LOCTEXT("ChangeCodeSource", "Change Code Source"),
		[Source](UCrowdyServerObjectDefinition& Edited) { Edited.CodeSource = Source; });
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeRevisionRow(FRevisionItemPtr Item)
{
	return SNew(STextBlock)
		.Text(Item.IsValid() ? Item->Label : FText::GetEmpty())
		.Margin(FMargin(4.0f, 2.0f));
}

TSharedRef<SWidget> SCrowdyServerCodeTab::MakeSourceRow(FSourceItemPtr Item)
{
	return SNew(STextBlock)
		.Text(Item.IsValid() ? SCrowdyServerCodeTabDetail::SourceLabel(*Item) : FText::GetEmpty())
		.Margin(FMargin(4.0f, 2.0f));
}

TSharedRef<ITableRow> SCrowdyServerCodeTab::MakeDiffRow(FDiffRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace SCrowdyServerCodeTabDetail;
	using EKind = CrowdyExecRevisions::FDiffLine::EKind;
	const EKind Kind = Row.IsValid() ? Row->Line.Kind : EKind::Same;
	const FLinearColor Tint = Kind == EKind::Same ? FLinearColor::Transparent
		: (Kind == EKind::Removed ? FStyleColors::Error : FStyleColors::Success).GetSpecifiedColor().CopyWithNewOpacity(0.15f);
	const TCHAR* Mark = Kind == EKind::Removed ? TEXT("-") : (Kind == EKind::Added ? TEXT("+") : TEXT(" "));
	return SNew(STableRow<FDiffRowPtr>, OwnerTable)
		.ShowSelection(false)
		.Padding(0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Tint)
			.Padding(0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride(DiffNumberWidth)
					.Padding(FMargin(0.0f, 0.0f, GutterMargin, 0.0f))
					[
						SNew(STextBlock)
						.Text(CrowdyServerCodeFiles::VersionText(Row.IsValid() ? Row->Number : 0))
						.Justification(ETextJustify::Right)
						.Font(CodeFont())
						.ColorAndOpacity(FStyleColors::White25)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Mark))
					.Font(CodeFont())
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Row.IsValid() ? Row->Line.Text : FString()))
					.Font(CodeFont())
					.ColorAndOpacity(Kind == EKind::Same ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
				]
			]
		];
}

FReply SCrowdyServerCodeTab::HandleGenerate()
{
	Generate();
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleChooseFile()
{
	using namespace SCrowdyServerCodeTabDetail;
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop || !Definition.IsValid())
	{
		return FReply::Handled();
	}
	const FString ProjectDirectory = ProjectFolder();
	const FString StartFolder = LoadedPath.IsEmpty() ? ProjectDirectory : FPaths::GetPath(LoadedPath);
	TArray<FString> Chosen;
	const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	if (!Desktop->OpenFileDialog(Parent, LOCTEXT("ChooseFileTitle", "Choose your server code").ToString(), StartFolder, FString(), TEXT("Rust source (*.rs)|*.rs"), EFileDialogFlags::None, Chosen)
		|| Chosen.IsEmpty())
	{
		return FReply::Handled();
	}
	FString Path = FPaths::ConvertRelativePathToFull(Chosen[0]);
	if (FPaths::IsUnderDirectory(Path, ProjectDirectory))
	{
		FPaths::MakePathRelativeTo(Path, *ProjectDirectory);
	}
	EditDefinition(GET_MEMBER_NAME_CHECKED(UCrowdyServerObjectDefinition, LogicFile), LOCTEXT("ChooseLogicFile", "Choose Logic File"),
		[&Path](UCrowdyServerObjectDefinition& Edited) { Edited.LogicFile.FilePath = Path; });
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleOpenExternal()
{
	if (!LoadedPath.IsEmpty())
	{
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*LoadedPath);
	}
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleSave()
{
	SaveEdits(true);
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleRevert()
{
	const bool bUnsaved = bDirty || CrowdyServerCodeEdits::Kept().Contains(LoadedPath);
	if (bUnsaved && !SCrowdyServerCodeTabDetail::AskYesNo(FText::Format(LOCTEXT("ConfirmRevert", "Discard your unsaved changes to {0}?"), FText::FromString(ShownPath))))
	{
		return FReply::Handled();
	}
	DiscardEdits();
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleCompare()
{
	if (ViewedRevision == INDEX_NONE)
	{
		return FReply::Handled();
	}
	SetComparing(!bComparing);
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleRestore()
{
	if (!CanRestore())
	{
		return FReply::Handled();
	}
	const FText Version = CrowdyServerCodeFiles::VersionText(RevisionItems[ViewedRevision + 1]->Version);
	if (bDirty && !SCrowdyServerCodeTabDetail::AskYesNo(FText::Format(LOCTEXT("ConfirmRestore", "Replace your unsaved changes to {0} with version {1}'s code?"), FText::FromString(ShownPath), Version)))
	{
		return FReply::Handled();
	}
	FString Restored = ViewedLogic;
	ShowYourCode();
	SetEditedText(MoveTemp(Restored));
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleBackToYourCode()
{
	ShowYourCode();
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleReloadFromDisk()
{
	if (SCrowdyServerCodeTabDetail::AskYesNo(FText::Format(LOCTEXT("ConfirmReloadFromDisk", "Discard your unsaved changes to {0} and show the file from disk?"), FText::FromString(ShownPath))))
	{
		DiscardEdits();
	}
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleKeepMine()
{
	bChangedOnDisk = false;
	return FReply::Handled();
}

FReply SCrowdyServerCodeTab::HandleAddStubs()
{
	if (Gaps.Missing.IsEmpty() || !CanAddStubs())
	{
		return FReply::Handled();
	}
	if (Gaps.InsertAt != INDEX_NONE && TextBox.IsValid())
	{
		// The stubs go at the end of the block, so the cursor's line still means the same place.
		const FTextLocation Cursor = TextBox->GetCursorLocation();
		SetEditedText(CrowdyExecCodegen::AddLogicStubs(EditedText, Gaps));
		TextBox->GoTo(Cursor);
		TextBox->ScrollTo(Cursor);
		return FReply::Handled();
	}
	FPlatformApplicationMisc::ClipboardCopy(*CrowdyExecCodegenMenu::JoinStubs(Gaps));
	FNotificationInfo Info(LOCTEXT("StubsCopied", "Copied the missing functions; paste them into the block that implements Functions"));
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	return FReply::Handled();
}

FText SCrowdyServerCodeTab::GetPathText() const
{
	if (!ShownPath.IsEmpty())
	{
		return FText::FromString(ShownPath);
	}
	return bOwnFile ? LOCTEXT("ChooseOwnFile", "No file chosen: choose your .rs file with the folder button") : LOCTEXT("GeneratedPathPending", "Server/<Type Name>/src/logic.rs, once the Type Name is valid");
}

FText SCrowdyServerCodeTab::GetMissingText() const
{
	if (LoadedPath.IsEmpty() && bOwnFile)
	{
		return LOCTEXT("NoOwnFile", "Choose your .rs file above, or switch to Generated to use the generated logic.rs.");
	}
	if (LoadedPath.IsEmpty())
	{
		return LOCTEXT("NoLogicPath", "Set a valid Type Name to edit this type's server code here.");
	}
	if (IsUnreadable())
	{
		return FText::Format(LOCTEXT("Unreadable", "{0} exists but could not be read. Close any program that has it open, then Revert."), FText::FromString(ShownPath));
	}
	if (bOwnFile)
	{
		return LOCTEXT("OwnFileMissing", "This file does not exist. Choose an existing file, switch to Generated, or type code below and Save to create it.");
	}
	return LOCTEXT("LogicMissing", "logic.rs does not exist yet. Generate writes it.");
}

FText SCrowdyServerCodeTab::GetAddStubsText() const
{
	return Gaps.InsertAt == INDEX_NONE ? LOCTEXT("CopyStubs", "Copy Stubs") : LOCTEXT("AddStubs", "Add Stubs");
}

FText SCrowdyServerCodeTab::GetAddStubsToolTip() const
{
	return Gaps.InsertAt == INDEX_NONE ? LOCTEXT("CopyStubsToolTip", "Copies empty versions of the missing functions, to paste into the block that implements Functions")
		: LOCTEXT("AddStubsToolTip", "Adds empty versions of the missing functions to the code below, as unsaved changes");
}

FText SCrowdyServerCodeTab::GetUnsavedText() const
{
	return bStale ? LOCTEXT("UnsavedStale", "Unsaved changes, made before the file last changed on disk") : LOCTEXT("Unsaved", "Unsaved changes");
}

FText SCrowdyServerCodeTab::GetSaveToolTip() const
{
	const FText Reason = GetSaveBlockReason();
	if (!Reason.IsEmpty())
	{
		return Reason;
	}
	return bDirty ? LOCTEXT("SaveToolTip", "Writes the code below to the logic file") : LOCTEXT("SaveNothing", "No unsaved changes");
}

FText SCrowdyServerCodeTab::GetOpenExternalToolTip() const
{
	if (IsFilePresent())
	{
		return LOCTEXT("OpenExternalToolTip", "Opens the logic file in the program your computer uses for .rs files");
	}
	return LoadedPath.IsEmpty() ? LOCTEXT("OpenExternalNoPath", "There is no logic file to open yet") : LOCTEXT("OpenExternalMissing", "The logic file does not exist yet");
}

FText SCrowdyServerCodeTab::GetSaveBlockReason() const
{
	if (LoadedPath.IsEmpty() && bOwnFile)
	{
		return LOCTEXT("SaveNeedsOwnFile", "Choose your .rs file first");
	}
	if (!WriteBlockReason.IsEmpty())
	{
		return WriteBlockReason;
	}
	if (IsUnreadable())
	{
		return LOCTEXT("SaveUnreadable", "The logic file could not be read, so saving could replace what it holds");
	}
	return FText::GetEmpty();
}

FText SCrowdyServerCodeTab::GetRestoreBlockReason() const
{
	if (!bViewedHasLogic)
	{
		return LOCTEXT("RestoreNoLogic", "This revision has no logic.rs to restore");
	}
	if (bSharedTypeName)
	{
		return CrowdyServerCodeFiles::SharedTypeNameProblem();
	}
	if (LoadedPath.IsEmpty())
	{
		return bOwnFile ? LOCTEXT("RestoreNeedsOwnFile", "Choose your .rs file first") : LOCTEXT("RestoreNeedsTypeName", "Set a valid Type Name first");
	}
	if (IsUnreadable())
	{
		return LOCTEXT("RestoreUnreadable", "The logic file could not be read; Revert once it can be");
	}
	return FText::GetEmpty();
}

FText SCrowdyServerCodeTab::GetRestoreToolTip() const
{
	const FText Reason = GetRestoreBlockReason();
	return Reason.IsEmpty() ? LOCTEXT("RestoreToolTip", "Puts this revision's code in the editor as unsaved changes. Save writes it; Deploy makes it live.") : Reason;
}

FText SCrowdyServerCodeTab::GetCompareText() const
{
	return bComparing ? LOCTEXT("ShowRevisionCode", "Show its code") : LOCTEXT("Compare", "Compare with yours");
}

FText SCrowdyServerCodeTab::GetSourceText() const
{
	return SCrowdyServerCodeTabDetail::SourceLabel(bOwnFile ? ECrowdyServerCodeSource::OwnFile : ECrowdyServerCodeSource::Generated);
}

int32 SCrowdyServerCodeTab::GetCodeViewIndex() const
{
	if (ViewedRevision == INDEX_NONE)
	{
		return 0;
	}
	return bComparing ? 2 : 1;
}

EVisibility SCrowdyServerCodeTab::GetMissingVisibility() const
{
	const bool bPresent = !LoadedPath.IsEmpty() && bFileExists && bFileReadable;
	return bPresent || ViewedRevision != INDEX_NONE ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SCrowdyServerCodeTab::GetMissingGenerateVisibility() const
{
	return !bOwnFile && !LoadedPath.IsEmpty() && !bFileExists ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCrowdyServerCodeTab::GetGapsVisibility() const
{
	return ViewedRevision == INDEX_NONE && !GapsText.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCrowdyServerCodeTab::GetChangedVisibility() const
{
	return bChangedOnDisk && ViewedRevision == INDEX_NONE ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SCrowdyServerCodeTab::GetChangedText() const
{
	return FText::Format(LOCTEXT("ChangedOnDiskNotice", "{0} changed on disk, for example in another editor, while you have unsaved changes here."), FText::FromString(ShownPath));
}

EVisibility SCrowdyServerCodeTab::GetAddStubsVisibility() const
{
	return Gaps.Missing.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SCrowdyServerCodeTab::GetUnsavedVisibility() const
{
	return bDirty ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCrowdyServerCodeTab::GetChooseFileVisibility() const
{
	return bOwnFile ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SCrowdyServerCodeTab::GetRevisionsListVisibility() const
{
	return bSharedTypeName ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SCrowdyServerCodeTab::GetRevisionVisibility() const
{
	return ViewedRevision == INDEX_NONE ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SCrowdyServerCodeTab::GetViewedWarningVisibility() const
{
	return ViewedWarning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

bool SCrowdyServerCodeTab::IsFilePresent() const
{
	return bFileExists && !LoadedPath.IsEmpty();
}

bool SCrowdyServerCodeTab::IsUnreadable() const
{
	return bFileExists && !bFileReadable;
}

bool SCrowdyServerCodeTab::IsReadOnly() const
{
	return LoadedPath.IsEmpty() || IsUnreadable();
}

bool SCrowdyServerCodeTab::CanSave() const
{
	return bDirty && GetSaveBlockReason().IsEmpty();
}

bool SCrowdyServerCodeTab::CanRestore() const
{
	return ViewedRevision != INDEX_NONE && RevisionItems.IsValidIndex(ViewedRevision + 1) && GetRestoreBlockReason().IsEmpty();
}

#undef LOCTEXT_NAMESPACE
