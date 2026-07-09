// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyRegistryInspector.h"

#include "CrowdyStudioModule.h"
#include "Framework/Text/TextLayout.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "Subsystem/CrowdyAutoRegistry.h"
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
	// Fixed column widths shared by the Table view's header and every member row, so the two fill
	// columns (name / detail) get identical slack on every row and the whole table lines up.
	constexpr float KindColWidth  = 64.0f;
	constexpr float FlagsColWidth = 118.0f;
	constexpr float IdColWidth    = 92.0f;

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

	// One "Label : value" line in an expanded member's vertical detail block.
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

	// A regular-weight chip carrying a tooltip (for the class Ownership / Host Override defaults).
	TSharedRef<SWidget> MakeMetaChip(const FText& Text, const FText& Tooltip)
	{
		return SNew(SBorder)
			.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Chip"))
			.Padding(FMargin(6.0f, 1.0f))
			[
				SNew(STextBlock).Text(Text).ToolTipText(Tooltip)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
			];
	}

	// A small tinted "{count} {noun}" pill used to summarise how many RPCs / rep props a class has.
	TSharedRef<SWidget> CountPill(int32 Count, const FText& Noun, FLinearColor Strong)
	{
		FLinearColor Fill = Strong;
		Fill.A = 0.16f;
		return SNew(SBorder)
			.BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Pill"))
			.BorderBackgroundColor(FSlateColor(Fill))
			.Padding(FMargin(8.0f, 2.0f))
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("CountPillFmt", "{0} {1}"), FText::AsNumber(Count), Noun))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor(Strong))
			];
	}

	// A subtle em-dash placeholder for an empty table cell (no flags / no RepNotify). Built from the
	// code point so the source stays ASCII-only (no BOM here, and the module is -WarningsAsErrors).
	TSharedRef<SWidget> Dash()
	{
		return SNew(STextBlock)
			.Text(FText::FromString(FString::Chr(0x2014)))
			.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle");
	}

	// The id chip for a class's layout hash, shown wherever a rep-bearing class is summarised.
	TSharedRef<SWidget> LayoutHashChip(int64 LayoutHash)
	{
		return MakeIdChip(
			FText::Format(LOCTEXT("LayoutHashChip", "layout {0}"), FText::FromString(FString::Printf(TEXT("%lld"), LayoutHash))),
			FText::FromString(FString::Printf(TEXT("Layout hash 0x%016llX"), LayoutHash)));
	}

	// The Table view's fixed 5-column skeleton (kind | name | routing/notify | flags | id). Passing the
	// same widths through both the header row and every member row is what keeps the columns aligned.
	TSharedRef<SWidget> MakeTableRowSkeleton(const TSharedRef<SWidget>& Kind, const TSharedRef<SWidget>& Name,
		const TSharedRef<SWidget>& Detail, const TSharedRef<SWidget>& Flags, const TSharedRef<SWidget>& Id,
		bool bHeader = false)
	{
		return SNew(SBox).Padding(FMargin(2.0f, bHeader ? 1.0f : 5.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(SBox).WidthOverride(KindColWidth).HAlign(HAlign_Left).VAlign(VAlign_Center)[ Kind ] ]
			+ SHorizontalBox::Slot().FillWidth(0.5f).VAlign(VAlign_Center).Padding(6.0f, 0.0f)[ Name ]
			+ SHorizontalBox::Slot().FillWidth(0.5f).VAlign(VAlign_Center).Padding(6.0f, 0.0f)[ Detail ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(SBox).WidthOverride(FlagsColWidth).HAlign(HAlign_Right).VAlign(VAlign_Center)[ Flags ] ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[ SNew(SBox).WidthOverride(IdColWidth).HAlign(HAlign_Right).VAlign(VAlign_Center)[ Id ] ]
		];
	}
}

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

		// Title + explainer.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[ SNew(STextBlock).Text(LOCTEXT("RegistryTitle", "Registry")).TextStyle(&Style, "Crowdy.Text.Title") ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
			.Text(LOCTEXT("RegistryExplainer",
				"A preview of the baked Crowdy metadata that ships in packaged builds, grouped by class - "
				"each class shows its RPC functions and its CrowdyState replicated properties together. The "
				"editor and PIE read live metadata, so if this looks stale, click Rebuild."))
		]

		// Toolbar: actions + resolved-asset status.
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
					.Text(LOCTEXT("Rebuilding", "Rebuilding registry..."))
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

		// Summary count pills.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ StatPill(TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("ClassStat", "{0} classes"), FText::AsNumber(NumClasses)); }), FCrowdyStudioStyle::GoldBright()) ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ StatPill(TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("FnStat", "{0} functions"), FText::AsNumber(AllRpcRows.Num())); }), FCrowdyStudioStyle::TextSecondary()) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[ StatPill(TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("RepStat", "{0} rep props"), FText::AsNumber(AllRepRows.Num())); }), FCrowdyStudioStyle::TextSecondary()) ]
		]

		// Persistent / singleton struct cards, side by side to save vertical space.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 5.0f, 0.0f)
			[
				StructCard(TEXT("cube"),
					TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SCrowdyRegistryInspector::GetPersistentTitle)),
					PersistentStructListView, &PersistentStructItems,
					LOCTEXT("NoPersistent", "No persistent structs."))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				StructCard(TEXT("cube"),
					TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SCrowdyRegistryInspector::GetSingletonTitle)),
					SingletonStructListView, &SingletonStructItems,
					LOCTEXT("NoSingleton", "No singleton structs."))
			]
		]

		// Search filter + Cards/Table view toggle.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(SearchBox, SEditableTextBox)
				.Style(&Style, "Crowdy.Input")
				.HintText(LOCTEXT("SearchHint", "Filter by class, function, or property name..."))
				.OnTextChanged(this, &SCrowdyRegistryInspector::OnSearchTextChanged)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(168.0f)
				[
					CrowdyStudioWidgets::SegmentedEnum(
						{ TEXT("cards"), TEXT("table") },
						{ LOCTEXT("ViewCards", "Cards"), LOCTEXT("ViewTable", "Table") },
						TAttribute<FString>::CreateLambda([this]()
						{
							return ViewMode == ECrowdyRegistryViewMode::Table ? FString(TEXT("table")) : FString(TEXT("cards"));
						}),
						[this](const FString& Value)
						{
							SetViewMode(Value == TEXT("table") ? ECrowdyRegistryViewMode::Table : ECrowdyRegistryViewMode::Cards);
						})
				]
			]
		]

		// The grouped metadata (fills the remaining space), rendered as cards or a table.
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SAssignNew(ContentContainer, SVerticalBox)
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

	// The rebuild streams its assets in asynchronously, so the editor no longer freezes on click,
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
	RebuildContent();
}

void SCrowdyRegistryInspector::SetViewMode(ECrowdyRegistryViewMode Mode)
{
	if (ViewMode != Mode)
	{
		ViewMode = Mode;
		RebuildContent();
	}
}

void SCrowdyRegistryInspector::RefreshData()
{
	AllRpcRows.Reset();
	AllRepRows.Reset();
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

		// CrowdyState replicated properties, mirrored from the baked arrays. Build a transient
		// class-path -> layout-hash map so each property row can carry its class's layout hash for the
		// card header (the baker guarantees one hash entry per class that declares any rep props).
		TMap<FString, int64> LayoutHashByClass;
		LayoutHashByClass.Reserve(Registry->RepLayoutHashes.Num());
		for (const FCrowdyBakedRepLayoutHash& Hash : Registry->RepLayoutHashes)
		{
			LayoutHashByClass.Add(Hash.ClassPath.ToString(), Hash.LayoutHash);
		}

		// Class-level Ownership/HostOverride, resolved LIVE (never baked) from each class's default
		// CrowdyEntityComponent - see UCrowdyAutoRegistry::ResolveDefaultEntityComponent. Cached per
		// class path here so a class with many properties only pays one class-load/CDO-walk, not one
		// per property row.
		struct FClassEntityDefaults
		{
			bool  bChecked = false;
			bool  bFound = false;
			FText Ownership;
			FText HostOverride;
		};
		TMap<FString, FClassEntityDefaults> EntityDefaultsByClass;

		for (const FCrowdyBakedRepProperty& Prop : Registry->RepProperties)
		{
			const FString ClassPath = Prop.OwnerClassPath.ToString();

			FClassEntityDefaults& Defaults = EntityDefaultsByClass.FindOrAdd(ClassPath);
			if (!Defaults.bChecked)
			{
				Defaults.bChecked = true;
				if (const UClass* OwnerClass = Prop.OwnerClassPath.TryLoadClass<UObject>())
				{
					if (const UCrowdyEntityComponent* Component = UCrowdyAutoRegistry::ResolveDefaultEntityComponent(OwnerClass))
					{
						Defaults.bFound = true;
						Defaults.Ownership = EnumDisplayText(Component->GetOwnership());
						if (Component->GetOwnership() == ECrowdyOwnership::LocalClient)
						{
							Defaults.HostOverride = EnumDisplayText(Component->GetHostOverridePolicy());
						}
					}
				}
			}

			FCrowdyRepPropRowItemPtr Row = MakeShared<FCrowdyRepPropRowItem>();
			Row->ClassPath    = ClassPath;
			Row->ClassShort   = ShortName(ClassPath);
			Row->LayoutHash   = LayoutHashByClass.FindRef(ClassPath);
			Row->PropertyName = Prop.PropertyName.ToString();
			Row->PropertyID   = Prop.PropertyID;
			Row->bOwnerOnly   = Prop.bOwnerOnly;
			Row->bManualDirty = Prop.bManualDirty;
			Row->OnRepFunction = Prop.OnRepFunctionName.IsNone() ? FString() : Prop.OnRepFunctionName.ToString();
			Row->LayoutOrder  = Prop.LayoutOrder;
			Row->bHasEntityDefaults = Defaults.bFound;
			Row->OwnershipText       = Defaults.Ownership;
			Row->HostOverrideText    = Defaults.HostOverride;
			AllRepRows.Add(Row);
		}

		// Tidy grouped order: declaring class, then positional layout order (matches wire order).
		AllRepRows.Sort([](const FCrowdyRepPropRowItemPtr& A, const FCrowdyRepPropRowItemPtr& B)
		{
			return A->ClassPath != B->ClassPath ? A->ClassPath < B->ClassPath : A->LayoutOrder < B->LayoutOrder;
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

	// Distinct classes across both member kinds, for the summary pill.
	TSet<FString> ClassPaths;
	for (const FCrowdyRpcRowItemPtr& Row : AllRpcRows)  { ClassPaths.Add(Row->ClassPath); }
	for (const FCrowdyRepPropRowItemPtr& Row : AllRepRows) { ClassPaths.Add(Row->ClassPath); }
	NumClasses = ClassPaths.Num();

	RebuildContent();

	if (PersistentStructListView.IsValid()) PersistentStructListView->RequestListRefresh();
	if (SingletonStructListView.IsValid())  SingletonStructListView->RequestListRefresh();
}

TArray<FCrowdyClassGroupPtr> SCrowdyRegistryInspector::BuildFilteredGroups() const
{
	const bool bHasFilter = !SearchText.IsEmpty();
	auto Matches = [&](const FString& ClassShort, const FString& ClassPath, const FString& Member)
	{
		return !bHasFilter
			|| ClassShort.Contains(SearchText)
			|| ClassPath.Contains(SearchText)
			|| Member.Contains(SearchText);
	};

	// Union the two sorted, flat row arrays into one group per declaring class. Both arrays are already
	// class-contiguous and member-sorted, so appending preserves each group's internal order.
	TMap<FString, FCrowdyClassGroupPtr> ByClass;

	for (const FCrowdyRpcRowItemPtr& Row : AllRpcRows)
	{
		if (!Matches(Row->ClassShort, Row->ClassPath, Row->Function))
		{
			continue;
		}
		FCrowdyClassGroupPtr& Group = ByClass.FindOrAdd(Row->ClassPath);
		if (!Group.IsValid())
		{
			Group = MakeShared<FCrowdyClassGroup>();
			Group->ClassPath = Row->ClassPath;
			Group->ClassShort = Row->ClassShort;
		}
		Group->Functions.Add(Row);
	}

	for (const FCrowdyRepPropRowItemPtr& Row : AllRepRows)
	{
		if (!Matches(Row->ClassShort, Row->ClassPath, Row->PropertyName))
		{
			continue;
		}
		FCrowdyClassGroupPtr& Group = ByClass.FindOrAdd(Row->ClassPath);
		if (!Group.IsValid())
		{
			Group = MakeShared<FCrowdyClassGroup>();
			Group->ClassPath = Row->ClassPath;
			Group->ClassShort = Row->ClassShort;
		}
		Group->Props.Add(Row);
		// Every rep row of a class carries the same class-level metadata; set it from whichever is shown.
		Group->bHasRepProps = true;
		Group->LayoutHash = Row->LayoutHash;
		Group->bHasEntityDefaults = Row->bHasEntityDefaults;
		Group->OwnershipText = Row->OwnershipText;
		Group->HostOverrideText = Row->HostOverrideText;
	}

	TArray<FCrowdyClassGroupPtr> Groups;
	ByClass.GenerateValueArray(Groups);
	Groups.Sort([](const FCrowdyClassGroupPtr& A, const FCrowdyClassGroupPtr& B)
	{
		return A->ClassPath < B->ClassPath;
	});
	return Groups;
}

void SCrowdyRegistryInspector::RebuildContent()
{
	if (!ContentContainer.IsValid())
	{
		return;
	}

	ContentContainer->ClearChildren();

	const TArray<FCrowdyClassGroupPtr> Groups = BuildFilteredGroups();

	if (Groups.Num() == 0)
	{
		const bool bHasFilter = !SearchText.IsEmpty();
		ContentContainer->AddSlot().AutoHeight().Padding(8.0f)
		[
			CrowdyStudioWidgets::EmptyState(TEXT("broadcast"),
				bResolved
					? (bHasFilter
						? LOCTEXT("NoMatches", "Nothing matches the filter.")
						: LOCTEXT("NoMetadata", "No Crowdy metadata is registered.\nMark a UFUNCTION meta=(CrowdyEvent) or a UPROPERTY meta=(CrowdyState), then click Rebuild."))
					: LOCTEXT("NoAsset", "No baked registry asset found.\nClick Rebuild to create it."))
		];
		return;
	}

	if (ViewMode == ECrowdyRegistryViewMode::Table)
	{
		BuildTableInto(Groups);
	}
	else
	{
		BuildCardsInto(Groups);
	}
}

void SCrowdyRegistryInspector::BuildCardsInto(const TArray<FCrowdyClassGroupPtr>& Groups)
{
	for (const FCrowdyClassGroupPtr& Group : Groups)
	{
		ContentContainer->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			MakeClassCard(Group)
		];
	}
}

void SCrowdyRegistryInspector::BuildTableInto(const TArray<FCrowdyClassGroupPtr>& Groups)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SVerticalBox> Table = SNew(SVerticalBox);

	// A single column-header row; the fixed column widths line it up with every member row below.
	Table->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)
	[
		MakeTableRowSkeleton(
			SNew(STextBlock).Text(LOCTEXT("ColType", "TYPE")).TextStyle(&Style, "Crowdy.Text.SectionLabel"),
			SNew(STextBlock).Text(LOCTEXT("ColName", "NAME")).TextStyle(&Style, "Crowdy.Text.SectionLabel"),
			SNew(STextBlock).Text(LOCTEXT("ColDetail", "ROUTING / NOTIFY")).TextStyle(&Style, "Crowdy.Text.SectionLabel"),
			SNew(STextBlock).Text(LOCTEXT("ColFlags", "FLAGS")).TextStyle(&Style, "Crowdy.Text.SectionLabel"),
			SNew(STextBlock).Text(LOCTEXT("ColId", "ID")).TextStyle(&Style, "Crowdy.Text.SectionLabel"),
			/*bHeader*/ true)
	];
	Table->AddSlot().AutoHeight()[ Hairline() ];

	for (const FCrowdyClassGroupPtr& Group : Groups)
	{
		Table->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 4.0f)
		[ MakeTableClassSubheader(Group) ];
		Table->AddSlot().AutoHeight()[ Hairline() ];

		// One row per member: RPC functions first, then replicated properties, hairline-separated.
		TArray<TSharedRef<SWidget>> MemberRows;
		for (const FCrowdyRpcRowItemPtr& Fn : Group->Functions)
		{
			MemberRows.Add(MakeTableRpcRow(Fn));
		}
		for (const FCrowdyRepPropRowItemPtr& Prop : Group->Props)
		{
			MemberRows.Add(MakeTableRepRow(Prop));
		}

		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		for (int32 Index = 0; Index < MemberRows.Num(); ++Index)
		{
			Rows->AddSlot().AutoHeight()[ MemberRows[Index] ];
			if (Index < MemberRows.Num() - 1)
			{
				Rows->AddSlot().AutoHeight()[ Hairline() ];
			}
		}
		Table->AddSlot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)[ Rows ];
	}

	ContentContainer->AddSlot().AutoHeight()
	[
		CrowdyStudioWidgets::Card(Table, FMargin(14.0f, 12.0f))
	];
}

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeClassCard(const FCrowdyClassGroupPtr& Group)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Header: class name + per-kind count pills (left), package path (right).
	TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);
	Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Group->ClassShort))
		.ToolTipText(FText::FromString(Group->ClassPath))
		.TextStyle(&Style, "Crowdy.Text.Heading")
	];
	if (Group->Functions.Num() > 0)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[ CountPill(Group->Functions.Num(), LOCTEXT("RpcNoun", "RPC"), FCrowdyStudioStyle::GoldBright()) ];
	}
	if (Group->Props.Num() > 0)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[ CountPill(Group->Props.Num(), LOCTEXT("StateNoun", "state"), FCrowdyStudioStyle::Info()) ];
	}
	Header->AddSlot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(FText::FromString(PackageOnly(Group->ClassPath)))
		.ToolTipText(FText::FromString(Group->ClassPath))
		.TextStyle(&Style, "Crowdy.Text.Subtle")
	];

	// Body: class-meta strip (if any rep props), then the RPC and replicated-property subsections.
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	if (Group->bHasRepProps)
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[ MakeClassMetaStrip(Group) ];
	}

	if (Group->Functions.Num() > 0)
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 4.0f)
		[ CrowdyStudioWidgets::GroupLabel(LOCTEXT("RpcGroupLabel", "RPC FUNCTIONS")) ];

		TSharedRef<SVerticalBox> FnBox = SNew(SVerticalBox);
		for (int32 Index = 0; Index < Group->Functions.Num(); ++Index)
		{
			FnBox->AddSlot().AutoHeight()[ MakeFunctionEntry(Group->Functions[Index]) ];
			if (Index < Group->Functions.Num() - 1)
			{
				FnBox->AddSlot().AutoHeight()[ Hairline() ];
			}
		}
		Body->AddSlot().AutoHeight()[ FnBox ];
	}

	if (Group->Props.Num() > 0)
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 4.0f)
		[ CrowdyStudioWidgets::GroupLabel(LOCTEXT("RepGroupLabel", "REPLICATED PROPERTIES")) ];

		TSharedRef<SVerticalBox> PropBox = SNew(SVerticalBox);
		for (int32 Index = 0; Index < Group->Props.Num(); ++Index)
		{
			PropBox->AddSlot().AutoHeight()[ MakeRepPropertyEntry(Group->Props[Index]) ];
			if (Index < Group->Props.Num() - 1)
			{
				PropBox->AddSlot().AutoHeight()[ Hairline() ];
			}
		}
		Body->AddSlot().AutoHeight()[ PropBox ];
	}

	return CrowdyStudioWidgets::Card(
		SNew(SExpandableArea)
		.InitiallyCollapsed(false)
		.BorderImage(FStyleDefaults::GetNoBrush())
		.HeaderPadding(FMargin(2.0f, 2.0f))
		.HeaderContent()
		[
			Header
		]
		.BodyContent()
		[
			SNew(SBox).Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))[ Body ]
		],
		FMargin(12.0f, 10.0f));
}

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeClassMetaStrip(const FCrowdyClassGroupPtr& Group) const
{
	const FText DefaultsTip = LOCTEXT("EntityDefaultsTip",
		"Resolved live from this class's default CrowdyEntityComponent (native CDO or Blueprint component "
		"defaults) when this view was built - never baked, since Ownership / Host Override are "
		"per-placed-instance settings with no single value for a class. A level-placed instance can "
		"override either one.");

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
	[ LayoutHashChip(Group->LayoutHash) ];

	if (Group->bHasEntityDefaults)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[ MakeMetaChip(FText::Format(LOCTEXT("OwnershipChip", "Ownership: {0}"), Group->OwnershipText), DefaultsTip) ];

		if (!Group->HostOverrideText.IsEmpty())
		{
			Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
			[ MakeMetaChip(FText::Format(LOCTEXT("HostOverrideChip", "Host Override: {0}"), Group->HostOverrideText), DefaultsTip) ];
		}
	}

	return Row;
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

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeRepPropertyEntry(const FCrowdyRepPropRowItemPtr& Prop)
{
	// Collapsed header: property name on the left; OnRep name + OWNER/MANUAL badges on the right.
	TSharedRef<SHorizontalBox> Header = SNew(SHorizontalBox);
	Header->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock).Text(FText::FromString(Prop->PropertyName)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong")
	];
	Header->AddSlot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(Prop->OnRepFunction.IsEmpty() ? FText::GetEmpty() : FText::FromString(Prop->OnRepFunction))
		.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
	];
	if (Prop->bOwnerOnly)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
		[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagOwnerOnly", "OWNER"), CrowdyStudioWidgets::EBadgeTone::Info) ];
	}
	if (Prop->bManualDirty)
	{
		Header->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagManual", "MANUAL"), CrowdyStudioWidgets::EBadgeTone::Warning) ];
	}

	// Expanded body: details stacked vertically as "Label : value" rows, inside an inset panel.
	TSharedRef<SVerticalBox> Details = SNew(SVerticalBox);
	auto AddDetail = [&Details](const FText& Label, const TSharedRef<SWidget>& Value)
	{
		Details->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[ MakeDetailRow(Label, Value) ];
	};

	AddDetail(LOCTEXT("DetailPropID", "Property ID"),
		MakeIdChip(
			FText::FromString(FString::Printf(TEXT("%lld"), Prop->PropertyID)),
			FText::FromString(FString::Printf(TEXT("0x%016llX"), Prop->PropertyID))));
	AddDetail(LOCTEXT("DetailLayoutOrder", "Layout order"), MakeValueText(FText::AsNumber(Prop->LayoutOrder)));
	AddDetail(LOCTEXT("DetailOwnerOnly", "Only to owner"), MakeBoolValue(Prop->bOwnerOnly));
	AddDetail(LOCTEXT("DetailManual", "Manual update"), MakeBoolValue(Prop->bManualDirty));
	AddDetail(LOCTEXT("DetailRepNotify", "RepNotify"),
		MakeValueText(Prop->OnRepFunction.IsEmpty() ? LOCTEXT("RepNotifyNone", "None") : FText::FromString(Prop->OnRepFunction)));

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

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeTableClassSubheader(const FCrowdyClassGroupPtr& Group) const
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Group->ClassShort))
		.ToolTipText(FText::FromString(Group->ClassPath))
		.TextStyle(&Style, "Crowdy.Text.Heading")
	];
	if (Group->Functions.Num() > 0)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[ CountPill(Group->Functions.Num(), LOCTEXT("RpcNoun", "RPC"), FCrowdyStudioStyle::GoldBright()) ];
	}
	if (Group->Props.Num() > 0)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[ CountPill(Group->Props.Num(), LOCTEXT("StateNoun", "state"), FCrowdyStudioStyle::Info()) ];
	}

	// Right side: the layout hash (rep-bearing classes only), then the package path.
	TSharedRef<SHorizontalBox> Right = SNew(SHorizontalBox);
	if (Group->bHasRepProps)
	{
		Right->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[ LayoutHashChip(Group->LayoutHash) ];
	}
	Right->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(STextBlock)
		.Text(FText::FromString(PackageOnly(Group->ClassPath)))
		.ToolTipText(FText::FromString(Group->ClassPath))
		.TextStyle(&Style, "Crowdy.Text.Subtle")
	];
	Row->AddSlot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
	[ Right ];

	return Row;
}

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeTableRpcRow(const FCrowdyRpcRowItemPtr& Fn) const
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> Kind = CrowdyStudioWidgets::Badge(LOCTEXT("KindRpc", "RPC"), CrowdyStudioWidgets::EBadgeTone::Brand);

	TSharedRef<SWidget> Name = SNew(STextBlock)
		.Text(FText::FromString(Fn->Function))
		.ToolTipText(FText::FromString(Fn->Function))
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		.TextStyle(&Style, "Crowdy.Text.BodyStrong");

	TSharedRef<SWidget> Detail = SNew(STextBlock)
		.Text(Fn->Recipient)
		.ToolTipText(Fn->Recipient)
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		.TextStyle(&Style, "Crowdy.Text.Subtle");

	TSharedRef<SWidget> Flags = Dash();
	if (Fn->bParamsPOD || Fn->bReplicated)
	{
		TSharedRef<SHorizontalBox> FlagBox = SNew(SHorizontalBox);
		if (Fn->bParamsPOD)
		{
			FlagBox->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagPOD", "POD"), CrowdyStudioWidgets::EBadgeTone::Success) ];
		}
		if (Fn->bReplicated)
		{
			FlagBox->AddSlot().AutoWidth()
			[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagRepl", "REPL"), CrowdyStudioWidgets::EBadgeTone::Info) ];
		}
		Flags = FlagBox;
	}

	TSharedRef<SWidget> Id = MakeIdChip(
		FText::FromString(FString::Printf(TEXT("%lld"), Fn->FunctionID)),
		FText::FromString(FString::Printf(TEXT("0x%016llX"), Fn->FunctionID)));

	return MakeTableRowSkeleton(Kind, Name, Detail, Flags, Id);
}

TSharedRef<SWidget> SCrowdyRegistryInspector::MakeTableRepRow(const FCrowdyRepPropRowItemPtr& Prop) const
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> Kind = CrowdyStudioWidgets::Badge(LOCTEXT("KindState", "STATE"), CrowdyStudioWidgets::EBadgeTone::Info);

	TSharedRef<SWidget> Name = SNew(STextBlock)
		.Text(FText::FromString(Prop->PropertyName))
		.ToolTipText(FText::FromString(Prop->PropertyName))
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		.TextStyle(&Style, "Crowdy.Text.BodyStrong");

	TSharedRef<SWidget> Detail = Dash();
	if (!Prop->OnRepFunction.IsEmpty())
	{
		Detail = SNew(STextBlock)
			.Text(FText::FromString(Prop->OnRepFunction))
			.ToolTipText(FText::FromString(Prop->OnRepFunction))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.TextStyle(&Style, "Crowdy.Text.Subtle");
	}

	TSharedRef<SWidget> Flags = Dash();
	if (Prop->bOwnerOnly || Prop->bManualDirty)
	{
		TSharedRef<SHorizontalBox> FlagBox = SNew(SHorizontalBox);
		if (Prop->bOwnerOnly)
		{
			FlagBox->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagOwnerOnly", "OWNER"), CrowdyStudioWidgets::EBadgeTone::Info) ];
		}
		if (Prop->bManualDirty)
		{
			FlagBox->AddSlot().AutoWidth()
			[ CrowdyStudioWidgets::Badge(LOCTEXT("FlagManual", "MANUAL"), CrowdyStudioWidgets::EBadgeTone::Warning) ];
		}
		Flags = FlagBox;
	}

	TSharedRef<SWidget> Id = MakeIdChip(
		FText::FromString(FString::Printf(TEXT("%lld"), Prop->PropertyID)),
		FText::FromString(FString::Printf(TEXT("0x%016llX"), Prop->PropertyID)));

	return MakeTableRowSkeleton(Kind, Name, Detail, Flags, Id);
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
