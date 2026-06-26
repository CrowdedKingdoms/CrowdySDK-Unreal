// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyRegistryInspector.h"

#include "CrowdyStudioModule.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// Trailing token of an object/class path: the part after the last '.', or the last
	// '/', whichever exists - e.g. "/Game/BP/BP_Door.BP_Door_C" -> "BP_Door_C".
	FString ShortName(const FString& Path)
	{
		int32 Index = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Index) || Path.FindLastChar(TEXT('/'), Index))
		{
			return Path.RightChop(Index + 1);
		}
		return Path;
	}

	// Drops the trailing ".AssetName" so a class path reads as just its package path in
	// the card header - e.g. "/Game/BP/BP_Door.BP_Door_C" -> "/Game/BP/BP_Door".
	FString PackageOnly(const FString& Path)
	{
		int32 Index = INDEX_NONE;
		return Path.FindLastChar(TEXT('.'), Index) ? Path.Left(Index) : Path;
	}

	// Enum value -> its UMETA(DisplayName) text (all three Crowdy routing enums define them).
	template <typename TEnum>
	FText EnumDisplayText(TEnum Value)
	{
		if (const UEnum* EnumPtr = StaticEnum<TEnum>())
		{
			return EnumPtr->GetDisplayNameTextByValue(static_cast<int64>(Value));
		}
		return FText::FromString(TEXT("?"));
	}

	// A 1px hairline using the shared separator brush (matches the rest of the console).
	TSharedRef<SWidget> Hairline()
	{
		return SNew(SBox).HeightOverride(1.0f)
			[ SNew(SImage).Image(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Separator")) ];
	}

	// One "Label : value" line in an expanded function's vertical detail block.
	TSharedRef<SWidget> MakeDetailRow(const FText& Label, const TSharedRef<SWidget>& Value)
	{
		const ISlateStyle& S = FCrowdyStudioStyle::Get();
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 14.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(104.0f)
				[ SNew(STextBlock).Text(Label).TextStyle(&S, "Crowdy.Text.Subtle") ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Value
			];
	}

	TSharedRef<SWidget> MakeValueText(const FText& Text, const FText& Tooltip = FText::GetEmpty())
	{
		return SNew(STextBlock).Text(Text).ToolTipText(Tooltip)
			.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Body");
	}

	TSharedRef<SWidget> MakeBoolValue(bool bValue)
	{
		return SNew(STextBlock)
			.Text(bValue ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.ColorAndOpacity(bValue
				? FSlateColor(FCrowdyStudioStyle::Success())
				: FSlateColor(FCrowdyStudioStyle::TextSubtle()));
	}

	// A monospace id chip carrying a tooltip (Chip helper can't take one).
	TSharedRef<SWidget> MakeIdChip(const FText& Text, const FText& Tooltip)
	{
		return SNew(SBorder)
			.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Chip"))
			.Padding(FMargin(6.0f, 1.0f))
			[
				SNew(STextBlock).Text(Text).ToolTipText(Tooltip)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
			];
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// SCrowdyRegistryInspector
// ─────────────────────────────────────────────────────────────────────────────
void SCrowdyRegistryInspector::Construct(const FArguments& InArgs)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// A small count pill whose text stays live (re-evaluated each paint) across Rebuild/Refresh.
	auto StatPill = [](TAttribute<FText> Text, FLinearColor Strong) -> TSharedRef<SWidget>
	{
		FLinearColor Fill = Strong;
		Fill.A = 0.16f;
		return SNew(SBorder)
			.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Pill"))
			.BorderBackgroundColor(FSlateColor(Fill))
			.Padding(FMargin(9.0f, 3.0f))
			[
				SNew(STextBlock).Text(Text)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor(Strong))
			];
	};

	// One struct list section as a card: a bound title header over a bounded, scrolling list with an
	// empty-state shown when there are no entries.
	auto StructCard = [this](const TCHAR* Icon, TAttribute<FText> Title,
		TSharedPtr<SListView<TSharedPtr<FString>>>& OutList, TArray<TSharedPtr<FString>>* Source,
		const FText& EmptyMessage) -> TSharedRef<SWidget>
	{
		return CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(Icon, 16.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(Title).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Heading") ]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).MaxDesiredHeight(120.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(OutList, SListView<TSharedPtr<FString>>)
						.ListItemsSource(Source)
						.OnGenerateRow(this, &SCrowdyRegistryInspector::OnGenerateStructRow)
						.SelectionMode(ESelectionMode::None)
					]
					+ SOverlay::Slot()
					[
						SNew(SBox)
						.Visibility_Lambda([Source]() { return Source->Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed; })
						[ CrowdyStudioWidgets::EmptyState(TEXT("cube"), EmptyMessage) ]
					]
				]
			],
			FMargin(14.0f, 12.0f));
	};

	ChildSlot
	[
		SNew(SVerticalBox)

		// ── Title + explainer ──────────────────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[ SNew(STextBlock).Text(LOCTEXT("RegistryTitle", "Registry")).TextStyle(&Style, "Crowdy.Text.Title") ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
			.Text(LOCTEXT("RegistryExplainer",
				"A preview of the baked Crowdy metadata - RPC functions, their routing, and the "
				"persistent / singleton structs - that ships in packaged builds. The editor and PIE "
				"read live metadata, so if this looks stale, click Rebuild."))
		]

		// ── Toolbar: actions + resolved-asset status ───────────────────────────
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Primary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.IsEnabled_Lambda([this]() { return !bRebuildInProgress && CrowdyStudioRegistry::HasRebuildHook(); })
				.ToolTipText_Lambda([]()
				{
					return CrowdyStudioRegistry::HasRebuildHook()
						? LOCTEXT("RebuildTip", "Regenerate the registry from every C++ and Blueprint metadata in the project, then refresh this view. This is the authoritative bake - packaging runs the same thing automatically at cook time.")
						: LOCTEXT("RebuildDisabledTip", "Rebuilding needs the CrowdySDK editor module, which isn't loaded.");
				})
				.OnClicked(this, &SCrowdyRegistryInspector::OnRebuildClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
					[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 15.0f, FSlateColor::UseForeground()) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(STextBlock).Text(LOCTEXT("Rebuild", "Rebuild (Deep Scan)")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.ToolTipText(LOCTEXT("RefreshTip", "Re-read the baked registry asset and update this view, without re-baking from metadata."))
				.OnClicked(this, &SCrowdyRegistryInspector::OnRefreshClicked)
				[ SNew(STextBlock).Text(LOCTEXT("Refresh", "Refresh View")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]

			// In-progress spinner: visible only while the async deep scan is streaming + baking, so the
			// user gets a clue the work is happening (the editor stays responsive, so nothing else moves).
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return bRebuildInProgress ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
				[ SNew(SCircularThrobber).Radius(9.0f) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Rebuilding", "Rebuilding registry…"))
					.TextStyle(&Style, "Crowdy.Text.Subtle")
				]
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SCrowdyRegistryInspector::GetStatusText)
				.TextStyle(&Style, "Crowdy.Text.Subtle")
			]
		]

		// ── Summary count pills ─────────────────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ StatPill(TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("FnStat", "{0} functions"), FText::AsNumber(AllRpcRows.Num())); }), FCrowdyStudioStyle::GoldBright()) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ StatPill(TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("PersistStat", "{0} persistent"), FText::AsNumber(PersistentStructItems.Num())); }), FCrowdyStudioStyle::TextSecondary()) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[ StatPill(TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("SingleStat", "{0} singleton"), FText::AsNumber(SingletonStructItems.Num())); }), FCrowdyStudioStyle::TextSecondary()) ]
		]

		// ── Persistent / singleton struct cards ─────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			StructCard(TEXT("cube"),
				TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SCrowdyRegistryInspector::GetPersistentTitle)),
				PersistentStructListView, &PersistentStructItems,
				LOCTEXT("NoPersistent", "No persistent structs."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			StructCard(TEXT("cube"),
				TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SCrowdyRegistryInspector::GetSingletonTitle)),
				SingletonStructListView, &SingletonStructItems,
				LOCTEXT("NoSingleton", "No singleton structs."))
		]

		// ── RPC search filter ────────────────────────────────────────────────────
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SAssignNew(SearchBox, SEditableTextBox)
			.Style(&Style, "Crowdy.Input")
			.HintText(LOCTEXT("SearchHint", "Filter RPC functions by class or function name..."))
			.OnTextChanged(this, &SCrowdyRegistryInspector::OnSearchTextChanged)
		]

		// ── RPC functions: one card per class (fills remaining space) ────────────
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(CardContainer, SVerticalBox)
			]
		]
	];

	RefreshData();
}

FReply SCrowdyRegistryInspector::OnRebuildClicked()
{
	if (bRebuildInProgress)
	{
		return FReply::Handled();
	}

	// The rebuild streams its assets in asynchronously, so the editor no longer freezes on click —
	// which also means nothing visibly happens until it finishes. Flip the in-progress flag so the
	// toolbar spinner appears, and clear it + refresh from the completion callback (game thread), not
	// synchronously (that would show the pre-rebuild data). Weak-pin so a panel closed mid-rebuild is
	// a no-op. A synchronous completion (e.g. nothing to stream) just flips it straight back off.
	bRebuildInProgress = true;

	TWeakPtr<SCrowdyRegistryInspector> WeakSelf = SharedThis(this);
	CrowdyStudioRegistry::RequestRebuild([WeakSelf]()
	{
		if (TSharedPtr<SCrowdyRegistryInspector> Self = WeakSelf.Pin())
		{
			Self->bRebuildInProgress = false;
			Self->RefreshData();
		}
	});
	return FReply::Handled();
}

FReply SCrowdyRegistryInspector::OnRefreshClicked()
{
	UCrowdyBakedRegistry::InvalidateCache();
	RefreshData();
	return FReply::Handled();
}

void SCrowdyRegistryInspector::OnSearchTextChanged(const FText& NewText)
{
	SearchText = NewText.ToString();
	BuildCardList();
}

void SCrowdyRegistryInspector::RefreshData()
{
	AllRpcRows.Reset();
	PersistentStructItems.Reset();
	SingletonStructItems.Reset();

	const UCrowdyBakedRegistry* Registry = UCrowdyBakedRegistry::Get();
	bResolved = (Registry != nullptr);
	ResolvedAssetPath = Registry ? Registry->GetPathName() : FString();

	if (Registry)
	{
		for (const FCrowdyBakedRpcFunction& Fn : Registry->RpcFunctions)
		{
			const FString ClassPath = Fn.ClassPath.ToString();

			FCrowdyRpcRowItemPtr Row = MakeShared<FCrowdyRpcRowItem>();
			Row->ClassPath   = ClassPath;
			Row->ClassShort  = ShortName(ClassPath);
			Row->Function    = Fn.FunctionName.ToString();
			Row->FunctionID  = Fn.FunctionID;
			Row->Recipient   = EnumDisplayText(Fn.Recipient);
			Row->Decay       = EnumDisplayText(Fn.DecayRate);
			Row->Distance    = EnumDisplayText(Fn.Distance);
			Row->bParamsPOD  = Fn.bParamsPOD;
			Row->bReplicated = Fn.bIsReplicated;
			AllRpcRows.Add(Row);
		}

		// Tidy grouped order: declaring class, then function name.
		AllRpcRows.Sort([](const FCrowdyRpcRowItemPtr& A, const FCrowdyRpcRowItemPtr& B)
		{
			return A->ClassPath != B->ClassPath ? A->ClassPath < B->ClassPath : A->Function < B->Function;
		});

		for (const FSoftObjectPath& Path : Registry->PersistentStructs)
		{
			PersistentStructItems.Add(MakeShared<FString>(Path.ToString()));
		}
		for (const FSoftObjectPath& Path : Registry->SingletonStructs)
		{
			SingletonStructItems.Add(MakeShared<FString>(Path.ToString()));
		}
	}

	BuildCardList();

	if (PersistentStructListView.IsValid()) PersistentStructListView->RequestListRefresh();
	if (SingletonStructListView.IsValid())  SingletonStructListView->RequestListRefresh();
}

void SCrowdyRegistryInspector::BuildCardList()
{
	if (!CardContainer.IsValid())
	{
		return;
	}

	CardContainer->ClearChildren();

	const bool bHasFilter = !SearchText.IsEmpty();
	int32 ShownClasses = 0;

	// AllRpcRows is class-sorted, so a class's functions are contiguous.
	int32 Start = 0;
	while (Start < AllRpcRows.Num())
	{
		const FString ClassPath = AllRpcRows[Start]->ClassPath;

		TArray<FCrowdyRpcRowItemPtr> Matching;
		int32 End = Start;
		for (; End < AllRpcRows.Num() && AllRpcRows[End]->ClassPath == ClassPath; ++End)
		{
			const FCrowdyRpcRowItemPtr& Row = AllRpcRows[End];
			if (!bHasFilter
				|| Row->ClassShort.Contains(SearchText)
				|| Row->ClassPath.Contains(SearchText)
				|| Row->Function.Contains(SearchText))
			{
				Matching.Add(Row);
			}
		}

		if (Matching.Num() > 0)
		{
			CardContainer->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				MakeClassCard(AllRpcRows[Start]->ClassShort, ClassPath, Matching)
			];
			++ShownClasses;
		}

		Start = End;
	}

	if (ShownClasses == 0)
	{
		CardContainer->AddSlot().AutoHeight().Padding(8.0f)
		[
			CrowdyStudioWidgets::EmptyState(TEXT("broadcast"),
				bResolved
					? (bHasFilter
						? LOCTEXT("NoMatches", "No functions match the filter.")
						: LOCTEXT("NoFunctions", "No RPC functions are registered.\nClick Rebuild to scan the project."))
					: LOCTEXT("NoAsset", "No baked registry asset found.\nClick Rebuild to create it."))
		];
	}
}

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeClassCard(
	const FString& ClassShort, const FString& ClassPath, const TArray<FCrowdyRpcRowItemPtr>& Functions)
{
	// Build the function rows, with a thin separator between them.
	TSharedRef<SVerticalBox> FnBox = SNew(SVerticalBox);
	for (int32 Index = 0; Index < Functions.Num(); ++Index)
	{
		FnBox->AddSlot().AutoHeight()
		[
			MakeFunctionEntry(Functions[Index])
		];

		if (Index < Functions.Num() - 1)
		{
			FnBox->AddSlot().AutoHeight()[ Hairline() ];
		}
	}

	const int32 FunctionCount = Functions.Num();
	const FText CountText = FText::Format(
		LOCTEXT("FnCount", "{0} {1}"),
		FText::AsNumber(FunctionCount),
		FunctionCount == 1 ? LOCTEXT("FnWordOne", "function") : LOCTEXT("FnWordMany", "functions"));

	return CrowdyStudioWidgets::Card(
		SNew(SExpandableArea)
		.InitiallyCollapsed(false)
		.BorderImage(FStyleDefaults::GetNoBrush())
		.HeaderPadding(FMargin(2.0f, 2.0f))
		.HeaderContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(ClassShort))
				.ToolTipText(FText::FromString(ClassPath))
				.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Heading")
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[ CrowdyStudioWidgets::Chip(CountText) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(PackageOnly(ClassPath)))
				.ToolTipText(FText::FromString(ClassPath))
				.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
			]
		]
		.BodyContent()
		[
			SNew(SBox).Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))[ FnBox ]
		],
		FMargin(12.0f, 10.0f));
}

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeFunctionEntry(const FCrowdyRpcRowItemPtr& Fn)
{
	// Collapsed header: function name on the left; recipient + POD/REPL badges on the right.
	TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);
	Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock).Text(FText::FromString(Fn->Function)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong")
	];
	Header->AddSlot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock).Text(Fn->Recipient).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
	];
	if (Fn->bParamsPOD)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
		[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagPOD", "POD"), CrowdyStudioWidgets::EBadgeTone::Success) ];
	}
	if (Fn->bReplicated)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagRepl", "REPL"), CrowdyStudioWidgets::EBadgeTone::Info) ];
	}

	// Expanded body: details stacked vertically as "Label : value" rows, inside an inset panel.
	TSharedRef<SVerticalBox> Details = SNew(SVerticalBox);
	auto AddDetail = [&Details](const FText& Label, const TSharedRef<SWidget>& Value)
	{
		Details->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[ MakeDetailRow(Label, Value) ];
	};

	AddDetail(LOCTEXT("DetailID", "Function ID"),
		MakeIdChip(
			FText::FromString(FString::Printf(TEXT("%lld"), Fn->FunctionID)),
			FText::FromString(FString::Printf(TEXT("0x%016llX"), Fn->FunctionID))));
	AddDetail(LOCTEXT("DetailRecipient", "Recipient"), MakeValueText(Fn->Recipient));
	AddDetail(LOCTEXT("DetailDecay", "Decay rate"), MakeValueText(Fn->Decay));
	AddDetail(LOCTEXT("DetailDistance", "Distance"), MakeValueText(Fn->Distance));
	AddDetail(LOCTEXT("DetailPOD", "Params POD"), MakeBoolValue(Fn->bParamsPOD));
	AddDetail(LOCTEXT("DetailRepl", "Replicated"), MakeBoolValue(Fn->bReplicated));

	return SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.BorderImage(FStyleDefaults::GetNoBrush())
		.Padding(FMargin(0.0f))
		.HeaderPadding(FMargin(2.0f, 6.0f))
		.HeaderContent()
		[
			Header
		]
		.BodyContent()
		[
			SNew(SBox).Padding(FMargin(8.0f, 2.0f, 0.0f, 8.0f))
			[
				SNew(SBorder).BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Inset")).Padding(FMargin(12.0f, 10.0f))
				[ Details ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyRegistryInspector::OnGenerateStructRow(
	TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Path = Item.IsValid() ? *Item : FString();
	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		[
			SNew(SBox).Padding(FMargin(8.0f, 3.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(ShortName(Path)))
				.ToolTipText(FText::FromString(Path))
				.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Body")
			]
		];
}

FText SCrowdyRegistryInspector::GetStatusText() const
{
	if (!bResolved)
	{
		return LOCTEXT("StatusNone", "No baked registry asset found - click Rebuild to create it.");
	}
	return FText::Format(LOCTEXT("StatusFmt", "Asset: {0}"), FText::FromString(ResolvedAssetPath));
}

FText SCrowdyRegistryInspector::GetPersistentTitle() const
{
	return FText::Format(LOCTEXT("PersistentTitle", "Persistent Structs ({0})"),
		FText::AsNumber(PersistentStructItems.Num()));
}

FText SCrowdyRegistryInspector::GetSingletonTitle() const
{
	return FText::Format(LOCTEXT("SingletonTitle", "Singleton Structs ({0})"),
		FText::AsNumber(SingletonStructItems.Num()));
}

#undef LOCTEXT_NAMESPACE
