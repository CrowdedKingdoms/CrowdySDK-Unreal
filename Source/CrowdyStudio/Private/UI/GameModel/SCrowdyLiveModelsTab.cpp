// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyLiveModelsTab.h"

#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelLoadState.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/MessageDialog.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/SCrowdyModelSectionTable.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// How many live instances one read asks for. The query reports no count of any kind, so a full page is the
	// only evidence that another one exists; asking for everything at once would take that signal away as well as
	// pulling an unbounded list into the editor.
	constexpr int32 CrowdyLivePageSize = 100;
}

TArray<FCrowdyModelColumn> SCrowdyLiveModelsTab::LiveInstanceColumns()
{
	TArray<FCrowdyModelColumn> Columns;
	Columns.Reserve(5);

	FCrowdyModelColumn& TitleColumn = Columns.AddDefaulted_GetRef();
	TitleColumn.ColumnId = CrowdyModelTableColumns::Title;
	TitleColumn.Label = LOCTEXT("LiveColumnTitle", "Name");
	TitleColumn.FillWidth = 0.26f;
	TitleColumn.TextStyle = TEXT("Crowdy.Text.BodyStrong");
	TitleColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Primary; };

	FCrowdyModelColumn& IdColumn = Columns.AddDefaulted_GetRef();
	IdColumn.ColumnId = CrowdyModelTableColumns::Id;
	IdColumn.Label = LOCTEXT("LiveColumnId", "Id");
	IdColumn.FillWidth = 0.24f;
	IdColumn.bMonospace = true;
	IdColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
	IdColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Secondary; };

	FCrowdyModelColumn& OwnerColumn = Columns.AddDefaulted_GetRef();
	OwnerColumn.ColumnId = CrowdyModelTableColumns::Owner;
	OwnerColumn.Label = LOCTEXT("LiveColumnOwner", "Owner");
	OwnerColumn.FillWidth = 0.16f;
	OwnerColumn.TextStyle = TEXT("Crowdy.Text.Body");
	OwnerColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Owner; };

	// The two columns that identify a row are its name and its id, so these are the ones that give way once the
	// pane is too narrow to give every column a readable width.
	FCrowdyModelColumn& SessionColumn = Columns.AddDefaulted_GetRef();
	SessionColumn.ColumnId = CrowdyModelTableColumns::Session;
	SessionColumn.Label = LOCTEXT("LiveColumnSession", "Session");
	SessionColumn.FillWidth = 0.17f;
	SessionColumn.bMonospace = true;
	SessionColumn.bDropWhenNarrow = true;
	SessionColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
	SessionColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Session; };

	FCrowdyModelColumn& BindingColumn = Columns.AddDefaulted_GetRef();
	BindingColumn.ColumnId = CrowdyModelTableColumns::Binding;
	BindingColumn.Label = LOCTEXT("LiveColumnBinding", "Binding");
	BindingColumn.FillWidth = 0.17f;
	BindingColumn.bMonospace = true;
	BindingColumn.bDropWhenNarrow = true;
	BindingColumn.TextStyle = TEXT("Crowdy.Text.Subtle");
	BindingColumn.Cell = [](const FCrowdyModelRow& Row) -> const FString& { return Row.Binding; };

	return Columns;
}

FString SCrowdyLiveModelsTab::CountPhrase(int32 Count, const TCHAR* Singular, const TCHAR* Plural)
{
	return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? Singular : Plural);
}

void SCrowdyLiveModelsTab::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	ShowModelInBrowser = InArgs._OnShowModelInBrowser;

	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyLiveModelsTab::HandleAppChanged);
		Controller->OnContainerTypesChanged.AddSP(this, &SCrowdyLiveModelsTab::HandleContainerTypesChanged);
		Controller->OnContainersChanged.AddSP(this, &SCrowdyLiveModelsTab::HandleContainersChanged);
		Controller->OnContainerStateChanged.AddSP(this, &SCrowdyLiveModelsTab::HandleContainerStateChanged);
		Controller->OnContainerPurgeProgress.AddSP(this, &SCrowdyLiveModelsTab::HandleContainerPurgeProgress);
		Controller->OnContainerPurgeFinished.AddSP(this, &SCrowdyLiveModelsTab::HandleContainerPurgeFinished);
	}

	// The app this tab's contents belong to, as of construction. The page is built long after a remembered app was
	// restored and selected, so without this the first list to arrive reads as an app switch and empties the
	// filters the user has just typed.
	EditorAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> ModelRail =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(ContainerTypeFilterBox, SEditableTextBox)
			.Style(&Style, "Crowdy.Input")
			.HintText(LOCTEXT("LiveTypeFilterHint", "type name (optional)"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(ContainerSessionFilterBox, SEditableTextBox)
			.Style(&Style, "Crowdy.Input")
			.HintText(LOCTEXT("LiveSessionFilterHint", "session id (optional)"))
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(13.0f, 7.0f))
			.HAlign(HAlign_Center)
			.ToolTipText(LOCTEXT("LiveRefreshTip", "Read the first page of live models under the filters above, replacing what is listed."))
			.IsEnabled_Lambda([this]() { return !bReadPending && !IsPurgeRunning(); })
			.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnRefreshClicked))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("LiveRefresh", "Refresh"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			CrowdyStudioWidgets::Card(
				SNew(SOverlay)

				+ SOverlay::Slot()
				[
					SAssignNew(ModelListView, SListView<TSharedPtr<FCrowdyLiveModelEntry>>)
					.ListViewStyle(&Style, "Crowdy.TableView")
					.ListItemsSource(&ModelEntries)
					.OnGenerateRow(this, &SCrowdyLiveModelsTab::MakeModelRow)
					.OnSelectionChanged(this, &SCrowdyLiveModelsTab::OnModelSelectionChanged)
					.SelectionMode(ESelectionMode::Single)
				]

				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox)
					.Padding(18.0f)
					// Hit-test invisible so the message never swallows a scroll aimed at the list underneath it.
					.Visibility_Lambda([this]() { return ModelEntries.Num() == 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					[
						// The same read as the Models rail, so the two answer an empty list with the same words
						// rather than prescribing two different remedies for one situation. Written when the list
						// is rebuilt: an empty list means four different things and only the read knows which.
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
						[ CrowdyStudioWidgets::Icon(TEXT("cube"), 26.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
						[
							SAssignNew(ModelListPlaceholderText, STextBlock)
							.TextStyle(&Style, "Crowdy.Text.Subtle")
							.Justification(ETextJustify::Center)
							.AutoWrapText(true)
						]
					]
				],
				FMargin(4.0f), /*bFlat*/ true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[ SAssignNew(ModelListFooterText, STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle") ];

	TSharedRef<SWidget> DetailHeader = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SAssignNew(DetailTitleText, STextBlock).TextStyle(&Style, "Crowdy.Text.Heading").OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[ SAssignNew(DetailBadgeBox, SBox) ]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(11.0f, 5.0f))
				.ToolTipText(LOCTEXT("LiveShowInModelsTip", "Open this model on the Models tab."))
				.IsEnabled_Lambda([this]() { return SelectedTypeName.IsSet(); })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnShowInModelsClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveShowInModels", "Show in Models"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.Text(LOCTEXT("LiveSubtitle", "Read from the server, so this is live even outside Play."))
		],
		FMargin(14.0f, 12.0f));

	TSharedRef<SWidget> DetailPane =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[ DetailHeader ]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			CrowdyStudioWidgets::Card(
				SAssignNew(InstanceTable, SCrowdyModelSectionTable)
				.Controller(Controller)
				.Columns(LiveInstanceColumns())
				// The same sentence the table gets whenever nobody has asked for this model yet, from the same
				// place, so the message a reader sees before a model is even selected and the one they see after
				// cannot be two spellings of one answer that drift apart.
				.Placeholder(FText::FromString(
					CrowdyModelEmptyState::LiveInstanceTable(ECrowdyModelLoadState::NeverRequested)))
				.PlaceholderIcon(TEXT("inspector"))
				.OnSelectionChanged(this, &SCrowdyLiveModelsTab::OnInstanceSelectionChanged),
				FMargin(4.0f), /*bFlat*/ true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			// A summary is one line however much the instance carries, but a line longer than the strip is still
			// drawn in full unless it is clipped: a height on its own only reserves less room. The page area does
			// not scroll, so an unclipped line paints straight over the action bar and the status line below it.
			SNew(SBox)
			.HeightOverride(34.0f)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				SNew(SBorder)
				.BorderImage(Style.GetBrush("Crowdy.Inset"))
				.Padding(FMargin(10.0f, 7.0f))
				[
					SAssignNew(InspectorText, STextBlock)
					.TextStyle(&Style, "Crowdy.Text.Body")
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SAssignNew(SelectionCountText, STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle") ]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				// The Id column is an opaque uuid that clips, so the only way to get one out of here was to retype it.
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(11.0f, 7.0f))
				.ToolTipText(LOCTEXT("LiveCopyIdTip", "Copy the highlighted live model's id to the clipboard."))
				.IsEnabled_Lambda([this]() { return InstanceTable.IsValid() && InstanceTable->NumSelectedRows() > 0; })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnCopyInstanceIdClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveCopyId", "Copy id"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.ToolTipText(LOCTEXT("LiveDeleteInstanceTip", "Delete the highlighted live model from the running app. There is no undo and no soft delete."))
				// Enabled exactly when a row is highlighted, so the button can never be armed at something the
				// table is not showing. Counting is cheap enough to ask every paint; copying the selection out is
				// not, so that waits until the click.
				.IsEnabled_Lambda([this]()
					{ return !IsPurgeRunning() && InstanceTable.IsValid() && InstanceTable->NumSelectedRows() > 0; })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnDeleteInstanceClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveDeleteInstance", "Delete live model"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.ToolTipText(LOCTEXT("LiveDeleteAllOfModelTip", "Delete every live model of the selected model, not only the ones read so far. It reads its own pages until none are left."))
				.IsEnabled_Lambda([this]() { return !IsPurgeRunning() && !bReadPending && SelectedTypeName.IsSet(); })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnDeleteAllOfModelClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveDeleteAllOfModel", "Delete all of this model"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.ToolTipText(LOCTEXT("LiveDeleteAllInAppTip", "Delete every live model in this app, of every model and every session. It reads its own pages until none are left."))
				// Also gated on the tab's own outstanding read: the purge issues a container read of its own, and
				// whichever landed first would be consumed as the answer to the other one's query.
				.IsEnabled_Lambda([this]() { return !IsPurgeRunning() && !bReadPending && EditorAppId != 0; })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnDeleteAllInAppClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveDeleteAllInApp", "Delete all in app"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
				]
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.Visibility_Lambda([this]() { return IsPurgeRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnCancelPurgeClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveCancelPurge", "Stop"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SAssignNew(StatusLineText, STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(11.0f, 5.0f))
				.ToolTipText(LOCTEXT("LiveLoadMoreTip", "Read the next page of live models under these filters."))
				// The last page coming back full is the only evidence that another one exists. It is not a count,
				// so this goes quiet as soon as a page comes back short.
				.IsEnabled_Lambda([this]() { return Controller.IsValid() && Controller->ContainersMayHaveMore(); })
				.OnClicked(FOnClicked::CreateSP(this, &SCrowdyLiveModelsTab::OnLoadMoreClicked))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LiveLoadMore", "Load more"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];

	ChildSlot
	[
		SNew(SSplitter)
		.Style(&Style, "Crowdy.Splitter")
		.Orientation(Orient_Horizontal)
		.PhysicalSplitterHandleSize(3.0f)

		+ SSplitter::Slot().Value(0.33f).MinSize(200.0f)
		[
			SNew(SBox).Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[ ModelRail ]
		]

		+ SSplitter::Slot().Value(0.67f).MinSize(300.0f)
		[
			SNew(SBox).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SAssignNew(DetailSwitcher, SWidgetSwitcher)

				+ SWidgetSwitcher::Slot()
				[
					CrowdyStudioWidgets::EmptyState(TEXT("inspector"), LOCTEXT("LiveNoModelSelected",
						"Select a model on the left to see the instances the server is holding for it."))
				]

				+ SWidgetSwitcher::Slot()
				[ DetailPane ]
			]
		]
	];

	DetailSwitcher->SetActiveWidgetIndex(0);
	SetInspectorLine(LOCTEXT("LiveInspectorHint", "Select a live model to see its properties."));
	UpdateActionBar();
	RebuildModelList();
}

SCrowdyLiveModelsTab::~SCrowdyLiveModelsTab()
{
	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.RemoveAll(this);
		Controller->OnContainerTypesChanged.RemoveAll(this);
		Controller->OnContainersChanged.RemoveAll(this);
		Controller->OnContainerStateChanged.RemoveAll(this);
		Controller->OnContainerPurgeProgress.RemoveAll(this);
		Controller->OnContainerPurgeFinished.RemoveAll(this);
	}
}

void SCrowdyLiveModelsTab::RequestContainers(bool bAppend)
{
	if (!Controller.IsValid())
	{
		return;
	}

	const FString TypeFilter = ContainerTypeFilterBox.IsValid()
		? ContainerTypeFilterBox->GetText().ToString().TrimStartAndEnd()
		: FString();
	const FString SessionFilter = ContainerSessionFilterBox.IsValid()
		? ContainerSessionFilterBox->GetText().ToString().TrimStartAndEnd()
		: FString();

	// A page only continues the ones on screen while it is asked for under the same filters. A box edited without
	// a Refresh describes a different query, and appending its second page would both mix it with rows read for
	// the old one and skip its first page, which nothing on this page could then reach. Filters are server keys,
	// so the comparison has to be spelled out as case-sensitive.
	if (bAppend
		&& !(TypeFilter.Equals(LoadedTypeFilter, ESearchCase::CaseSensitive)
			&& SessionFilter.Equals(LoadedSessionFilter, ESearchCase::CaseSensitive)))
	{
		bAppend = false;
	}

	// Where the next page starts is what is already held, not a count kept here. A page that failed added nothing
	// and has to be asked for again rather than stepped over, and a delete between two pages shortens the list
	// under any offset remembered from before it.
	const int32 Offset = bAppend ? Controller->GetContainers().Num() : 0;

	// What the answer will describe is decided by the filters this read was issued with, not by whatever is in the
	// boxes when it lands: the user is free to type on while it is in flight.
	// A new read is the reader asking for something else, so the last purge's line stops standing over it.
	PurgeOutcomeLine = FText::GetEmpty();

	PendingTypeFilter = TypeFilter;
	PendingSessionFilter = SessionFilter;
	bPendingAppend = bAppend;
	bReadPending = true;

	Controller->FetchContainers(TypeFilter, SessionFilter, CrowdyLivePageSize, Offset, bAppend);
}

FReply SCrowdyLiveModelsTab::OnRefreshClicked()
{
	RequestContainers(/*bAppend*/ false);
	return FReply::Handled();
}

FReply SCrowdyLiveModelsTab::OnLoadMoreClicked()
{
	RequestContainers(/*bAppend*/ true);
	return FReply::Handled();
}

FReply SCrowdyLiveModelsTab::OnShowInModelsClicked()
{
	if (SelectedTypeName.IsSet())
	{
		ShowModelInBrowser.ExecuteIfBound(SelectedTypeName.GetValue());
	}
	return FReply::Handled();
}

FReply SCrowdyLiveModelsTab::OnDeleteInstanceClicked()
{
	const TSharedPtr<FCrowdyModelRow> Row = InstanceTable.IsValid() ? InstanceTable->GetSelectedRow() : nullptr;
	if (!Controller.IsValid() || !Row.IsValid())
	{
		return FReply::Handled();
	}

	// An instance with no name of its own shows its id in the name column, so naming both would repeat it back.
	const FString Named = Row->Primary.Equals(Row->Name, ESearchCase::CaseSensitive)
		? Row->Name
		: FString::Printf(TEXT("%s (%s)"), *Row->Primary, *Row->Name);

	// The app named and passed is the one this tab's list was loaded for, not whatever is selected at this
	// instant: reading the selection here would agree with itself no matter how stale the list on screen is,
	// and the controller's refusal on a mismatch would never fire. It falls back to the selection only when
	// nothing has been loaded yet.
	const int64 AppId = EditorAppId != 0 ? EditorAppId : Controller->GetSelectedAppId();
	const FText Message = FText::Format(
		LOCTEXT("LiveDeleteConfirm", "DELETE the live model {0} of type \"{1}\" from the live server for app {2}?\n\nThis is DESTRUCTIVE and cannot be undone. There is no soft delete: the instance and its stored property values are removed from the running app immediately."),
		FText::FromString(Named),
		FText::FromString(Row->OwningType),
		FText::FromString(FString::Printf(TEXT("%lld"), AppId)));

	if (FMessageDialog::Open(EAppMsgType::YesNo, Message) == EAppReturnType::Yes)
	{
		Controller->DeleteContainer(Row->Name, AppId);
	}
	return FReply::Handled();
}

FReply SCrowdyLiveModelsTab::OnCopyInstanceIdClicked()
{
	const TSharedPtr<FCrowdyModelRow> Row = InstanceTable.IsValid() ? InstanceTable->GetSelectedRow() : nullptr;
	if (Row.IsValid() && !Row->Name.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*Row->Name);
		SetInspectorLine(FText::Format(
			LOCTEXT("LiveCopiedId", "Copied {0} to the clipboard."), FText::FromString(Row->Name)));
	}
	return FReply::Handled();
}

bool SCrowdyLiveModelsTab::IsPurgeRunning() const
{
	return Controller.IsValid() && Controller->IsContainerPurgeInFlight();
}

FReply SCrowdyLiveModelsTab::OnDeleteAllOfModelClicked()
{
	// Read at click time, like the single delete: a background read can drop the selection between the paint that
	// enabled this and the click. Bailing rather than falling back to an empty name, which means the WHOLE APP.
	if (!SelectedTypeName.IsSet())
	{
		return FReply::Handled();
	}

	ConfirmAndPurge(SelectedTypeName.GetValue());
	return FReply::Handled();
}

FReply SCrowdyLiveModelsTab::OnDeleteAllInAppClicked()
{
	ConfirmAndPurge(FString());
	return FReply::Handled();
}

FReply SCrowdyLiveModelsTab::OnCancelPurgeClicked()
{
	if (Controller.IsValid())
	{
		Controller->CancelContainerPurge();
	}
	return FReply::Handled();
}

void SCrowdyLiveModelsTab::ConfirmAndPurge(const FString& TypeName)
{
	if (!Controller.IsValid())
	{
		return;
	}

	const int64 AppId = EditorAppId != 0 ? EditorAppId : Controller->GetSelectedAppId();
	if (AppId == 0)
	{
		return;
	}

	// The confirm promises no count. The server sends no total, so the only honest thing to say is that this
	// reaches past what the table has read rather than naming a number the page never had.
	const FText Message = TypeName.IsEmpty()
		? FText::Format(
			LOCTEXT("LivePurgeAppConfirm", "DELETE EVERY live model in app {0}, of every model and every session?\n\nThis is DESTRUCTIVE and cannot be undone. There is no soft delete. It is not limited to the live models listed here: it reads its own pages and keeps deleting until none are left, so it will also remove ones these filters hide. A live model held by a binding key is recreated when the runtime next ensures that key."),
			FText::FromString(FString::Printf(TEXT("%lld"), AppId)))
		: FText::Format(
			LOCTEXT("LivePurgeModelConfirm", "DELETE EVERY live model of type \"{0}\" from app {1}?\n\nThis is DESTRUCTIVE and cannot be undone. There is no soft delete. It is not limited to the live models listed here: it reads its own pages and keeps deleting until none are left, so it will also remove ones these filters hide. A live model held by a binding key is recreated when the runtime next ensures that key."),
			FText::FromString(TypeName),
			FText::FromString(FString::Printf(TEXT("%lld"), AppId)));

	if (FMessageDialog::Open(EAppMsgType::YesNo, Message) == EAppReturnType::Yes)
	{
		Controller->PurgeContainers(TypeName, AppId);
	}
}

void SCrowdyLiveModelsTab::HandleContainerPurgeProgress()
{
	SyncEditorAppScope();
	UpdateStatusLine();
}

void SCrowdyLiveModelsTab::HandleContainerPurgeFinished()
{
	SyncEditorAppScope();

	// Held on the tab because the controller's own status line does not survive: the purge's re-list writes
	// "N live container(s)." over it a moment later, taking the count and, on a stopped run, the sentence saying
	// how to finish it. It stays until the reader asks for something else.
	if (Controller.IsValid())
	{
		PurgeOutcomeLine = FText::FromString(
			CrowdyGameModelDelete::PurgeText(Controller->GetLastContainerPurgeOutcome()));
	}

	UpdateActionBar();
	UpdateStatusLine();
}

void SCrowdyLiveModelsTab::HandleAppChanged()
{
	SyncEditorAppScope();
	RebuildModelList();
}

void SCrowdyLiveModelsTab::HandleContainerTypesChanged()
{
	// An app switch empties this list before anything else happens, so this is one of the two earliest points at
	// which the switch is visible to this tab.
	SyncEditorAppScope();
	RebuildModelList();
}

void SCrowdyLiveModelsTab::HandleContainersChanged()
{
	SyncEditorAppScope();

	// A read of this tab's own is the only one that says which models the answer covers. A read another view
	// issued fills the same list, but under filters this tab never saw.
	//
	// The latch is released by the read that set it whether that read landed or failed, because a failed read now
	// announces itself too. What it must not do on a failure is claim coverage: nothing came back, so the pages in
	// hand are still the ones the previous read delivered, the next page is still the one this read did not, and a
	// model marked read here would show "showing 0" over a list that was never fetched.
	if (bReadPending)
	{
		bReadPending = false;

		const bool bFailed = Controller.IsValid()
			&& Controller->GetFamilyLoadState(ECrowdyModelFamily::LiveModels) == ECrowdyModelLoadState::Failed;

		if (!bFailed)
		{
			// The pages held are now the ones this read asked for, so the next page continues from these filters.
			LoadedTypeFilter = PendingTypeFilter;
			LoadedSessionFilter = PendingSessionFilter;
			MarkTypesRead(PendingTypeFilter, PendingSessionFilter, bPendingAppend);
		}
	}

	RebuildModelList();

	// A re-list is a whole new set of objects, so the instance that was open may simply be gone: a narrower
	// filter, or the one that was just deleted. The table drops its selection as its rows are replaced, but the
	// inspector line would go on describing that instance until something replaced it, which reads as a
	// selection that is still there. Decide it from the ids instead, which is true immediately.
	if (Controller.IsValid())
	{
		const FString& ShownId = Controller->GetSelectedContainerId();
		if (!ShownId.IsEmpty())
		{
			bool bStillListed = false;
			for (const TSharedPtr<FStudioContainer>& Container : Controller->GetContainers())
			{
				// Container ids are opaque and case-significant, and the default FString comparison is not.
				if (Container.IsValid() && Container->ContainerId.Equals(ShownId, ESearchCase::CaseSensitive))
				{
					bStillListed = true;
					break;
				}
			}
			if (!bStillListed)
			{
				SetInspectorLine(LOCTEXT("LiveInspectorHint", "Select a live model to see its properties."));
			}
		}
	}
}

void SCrowdyLiveModelsTab::HandleContainerStateChanged()
{
	if (!Controller.IsValid())
	{
		return;
	}

	const FStudioContainerState& State = Controller->GetContainerState();
	if (!State.bValid)
	{
		SetInspectorLine(LOCTEXT("LiveInspectorHint", "Select a live model to see its properties."));
		return;
	}

	const FString DisplayName = State.DisplayName.TrimStartAndEnd();
	const FText Named = FText::FromString(DisplayName.IsEmpty() ? State.ContainerId : DisplayName);
	const FString Summary = CrowdyModelLedger::FormatPropertySummary(State.PropertiesJson);

	SetInspectorLine(Summary.IsEmpty()
		? FText::Format(LOCTEXT("LiveInspectorNoProperties", "{0} - no properties this token may see."), Named)
		: FText::Format(LOCTEXT("LiveInspectorLine", "{0} - {1}"), Named, FText::FromString(Summary)));
}

void SCrowdyLiveModelsTab::OnInstanceSelectionChanged(TSharedPtr<FCrowdyModelRow> Row, ESelectInfo::Type SelectInfo)
{
	// The count changes whichever way the selection moved, including the clear the table performs when its rows
	// are replaced, so the action bar follows every one of them.
	UpdateActionBar();

	// Ignore the programmatic clear the table emits when its rows are rebuilt on refresh. The instance it was
	// showing may well still be there, and the inspector line is decided from the ids on the refresh itself.
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	// Read what the table is showing as selected right now rather than trusting the argument: it is null on a
	// deselect, and it points into a set of rows that any refresh replaces.
	const TSharedPtr<FCrowdyModelRow> Selected = InstanceTable.IsValid() ? InstanceTable->GetSelectedRow() : nullptr;
	if (!Selected.IsValid())
	{
		SetInspectorLine(LOCTEXT("LiveInspectorHint", "Select a live model to see its properties."));
		return;
	}

	SetInspectorLine(FText::Format(
		LOCTEXT("LiveInspectorReading", "{0} - reading properties..."), FText::FromString(Selected->Primary)));

	if (Controller.IsValid())
	{
		Controller->FetchContainerState(Selected->Name);
	}
}

void SCrowdyLiveModelsTab::OnModelSelectionChanged(TSharedPtr<FCrowdyLiveModelEntry> Entry, ESelectInfo::Type SelectInfo)
{
	// The list announces the clear it performs when its source is replaced as a direct change. That is not the
	// user choosing anything, and following it would close the open model on every refresh.
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	const TArray<TSharedPtr<FCrowdyLiveModelEntry>> Selected =
		ModelListView.IsValid() ? ModelListView->GetSelectedItems() : TArray<TSharedPtr<FCrowdyLiveModelEntry>>();

	if (Selected.Num() == 0 || !Selected[0].IsValid())
	{
		SelectedTypeName.Reset();
	}
	else
	{
		SelectedTypeName = Selected[0]->TypeName;
	}

	RefreshInstanceTable();
}

TSharedRef<ITableRow> SCrowdyLiveModelsTab::MakeModelRow(TSharedPtr<FCrowdyLiveModelEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FText Title = FText::FromString(Entry.IsValid() ? Entry->Display : FString());
	const FText Status = FText::FromString(Entry.IsValid() ? Entry->Status : FString());

	return SNew(STableRow<TSharedPtr<FCrowdyLiveModelEntry>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(Title).ToolTipText(Title).TextStyle(&Style, "Crowdy.Text.BodyStrong").OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[ SNew(STextBlock).Text(Status).TextStyle(&Style, "Crowdy.Text.Subtle") ]
			]
		];
}

void SCrowdyLiveModelsTab::AddLoadedType(const FString& TypeName)
{
	if (TypeName.IsEmpty())
	{
		return;
	}

	// Type names are server keys and the default string comparison is not case-sensitive.
	for (const FString& Loaded : LoadedTypeNames)
	{
		if (Loaded.Equals(TypeName, ESearchCase::CaseSensitive))
		{
			return;
		}
	}
	LoadedTypeNames.Add(TypeName);
}

void SCrowdyLiveModelsTab::MarkTypesRead(const FString& TypeFilter, const FString& SessionFilter, bool bAppend)
{
	// A read that replaced the list no longer holds anything the read before it returned, so what is claimed as
	// read is rebuilt from this one alone. Left standing, an earlier wide read would keep every other model
	// reading "showing 0" over a list that simply does not cover it any more.
	if (!bAppend)
	{
		LoadedTypeNames.Reset();
		bLoadedEveryType = false;
	}

	if (!TypeFilter.IsEmpty())
	{
		AddLoadedType(TypeFilter);
		return;
	}

	// With no filters at all, one answer covers every model, but only once the read reached the end of the list.
	// A page that came back full is the edge of what was asked for, so a model beyond it was never looked at and
	// must not be reported as one the server holds none of.
	const bool bReachedTheEnd = Controller.IsValid() && !Controller->ContainersMayHaveMore();
	if (SessionFilter.IsEmpty() && bReachedTheEnd)
	{
		bLoadedEveryType = true;
		return;
	}

	// Otherwise only the models the read actually returned have an answer to show. Every other one stays unread,
	// which is what keeps a model with instances nobody has looked for off a count of zero.
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioContainer>& Container : Controller->GetContainers())
		{
			if (Container.IsValid())
			{
				AddLoadedType(Container->TypeName);
			}
		}
	}
}

bool SCrowdyLiveModelsTab::IsModelLoaded(const FString& TypeName) const
{
	if (bLoadedEveryType)
	{
		return true;
	}

	for (const FString& Loaded : LoadedTypeNames)
	{
		if (Loaded.Equals(TypeName, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

int32 SCrowdyLiveModelsTab::CountLiveInstances(const FString& TypeName) const
{
	if (!Controller.IsValid())
	{
		return 0;
	}

	int32 Count = 0;
	for (const TSharedPtr<FStudioContainer>& Container : Controller->GetContainers())
	{
		if (Container.IsValid() && Container->TypeName.Equals(TypeName, ESearchCase::CaseSensitive))
		{
			++Count;
		}
	}
	return Count;
}

void SCrowdyLiveModelsTab::RebuildModelList()
{
	ModelEntries.Reset();

	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioContainerType>& Type : Controller->GetContainerTypes())
		{
			if (!Type.IsValid())
			{
				continue;
			}

			TSharedRef<FCrowdyLiveModelEntry> Entry = MakeShared<FCrowdyLiveModelEntry>();
			Entry->TypeName = Type->TypeName;

			const FString DisplayName = Type->DisplayName.TrimStartAndEnd();
			Entry->Display = DisplayName.IsEmpty() ? Type->TypeName : DisplayName;

			// "Nothing came back" and "nobody has asked yet" are different answers, and a model that reads zero
			// when it has never been read invites a delete decision taken on a list that was never fetched.
			Entry->Status = IsModelLoaded(Entry->TypeName)
				? FString::Printf(TEXT("showing %d"), CountLiveInstances(Entry->TypeName))
				: FString(TEXT("not loaded"));

			ModelEntries.Add(Entry);
		}
	}

	if (ModelListPlaceholderText.IsValid())
	{
		ModelListPlaceholderText->SetText(FText::FromString(CrowdyModelEmptyState::LiveModelRail(
			Controller.IsValid()
				? Controller->GetFamilyLoadState(ECrowdyModelFamily::Models)
				: ECrowdyModelLoadState::NeverRequested)));
	}

	if (ModelListFooterText.IsValid())
	{
		ModelListFooterText->SetText(FText::FromString(CountPhrase(ModelEntries.Num(), TEXT("model"), TEXT("models"))));
	}

	if (ModelListView.IsValid())
	{
		ModelListView->RequestListRefresh();
	}

	// Every row object was rebuilt, so the highlight has to be placed on the new row carrying the same name.
	if (SelectedTypeName.IsSet())
	{
		const FString& OpenType = SelectedTypeName.GetValue();
		TSharedPtr<FCrowdyLiveModelEntry> StillListed;
		for (const TSharedPtr<FCrowdyLiveModelEntry>& Entry : ModelEntries)
		{
			if (Entry.IsValid() && Entry->TypeName.Equals(OpenType, ESearchCase::CaseSensitive))
			{
				StillListed = Entry;
				break;
			}
		}

		if (StillListed.IsValid())
		{
			if (ModelListView.IsValid())
			{
				// Announced as a direct change, which the selection handler ignores by design, so the pane is
				// told what to show explicitly rather than through the list.
				ModelListView->SetSelection(StillListed, ESelectInfo::Direct);
			}
		}
		else
		{
			// The model that was open is not in this app's list any more. Its instances describe something that
			// is not there, so the pane goes back to showing nothing.
			SelectedTypeName.Reset();
			if (ModelListView.IsValid())
			{
				ModelListView->ClearSelection();
			}
		}
	}

	RefreshInstanceTable();
}

void SCrowdyLiveModelsTab::RefreshInstanceTable()
{
	if (!InstanceTable.IsValid() || !DetailSwitcher.IsValid())
	{
		return;
	}

	if (!SelectedTypeName.IsSet())
	{
		DetailSwitcher->SetActiveWidgetIndex(0);
		InstanceTable->SetRows(TArray<TSharedPtr<FCrowdyModelRow>>());
		UpdateStatusLine();
		return;
	}

	DetailSwitcher->SetActiveWidgetIndex(1);

	const FString& TypeName = SelectedTypeName.GetValue();
	const bool bLoaded = IsModelLoaded(TypeName);

	// Five situations, not two. A model a landed read covered has a real answer; one it did not covers whichever of
	// "nobody asked", "a read is out" and "the read failed" the app-wide read is actually in, and a read that
	// landed but never covered this model is still nobody having asked about it.
	ECrowdyModelLoadState InstanceState = ECrowdyModelLoadState::Loaded;
	if (!bLoaded)
	{
		const ECrowdyModelLoadState ReadState = Controller.IsValid()
			? Controller->GetFamilyLoadState(ECrowdyModelFamily::LiveModels)
			: ECrowdyModelLoadState::NeverRequested;

		InstanceState = (ReadState == ECrowdyModelLoadState::Failed || ReadState == ECrowdyModelLoadState::Loading)
			? ReadState
			: ECrowdyModelLoadState::NeverRequested;
	}

	// A read taken under a filter says nothing about what this model has, only about what matched. The filters
	// compared are the ones the pages in hand were read under, never the boxes, which are free to be typed into
	// after a read has landed.
	const bool bFiltered = !LoadedTypeFilter.IsEmpty() || !LoadedSessionFilter.IsEmpty();

	InstanceTable->SetPlaceholder(FText::FromString(
		(InstanceState == ECrowdyModelLoadState::Loaded && bFiltered)
			? CrowdyModelEmptyState::LiveInstanceTableFiltered()
			: CrowdyModelEmptyState::LiveInstanceTable(InstanceState)));

	InstanceTable->SetRows(Controller.IsValid()
		? CrowdyModelListItems(CrowdyModelLedger::BuildLiveRows(Controller->GetContainers(), TypeName))
		: TArray<TSharedPtr<FCrowdyModelRow>>());

	UpdateDetailHeader();
	UpdateStatusLine();
}

void SCrowdyLiveModelsTab::UpdateDetailHeader()
{
	if (!SelectedTypeName.IsSet() || !DetailTitleText.IsValid() || !DetailBadgeBox.IsValid())
	{
		return;
	}

	const FString& TypeName = SelectedTypeName.GetValue();

	FString Display = TypeName;
	FString Status;
	for (const TSharedPtr<FCrowdyLiveModelEntry>& Entry : ModelEntries)
	{
		if (Entry.IsValid() && Entry->TypeName.Equals(TypeName, ESearchCase::CaseSensitive))
		{
			Display = Entry->Display;
			Status = Entry->Status;
			break;
		}
	}

	// Set here, on the events that can change either of them, rather than through a text binding: a bound lambda
	// runs on every frame the pane is on screen and none of this changes between frames.
	DetailTitleText->SetText(FText::FromString(Display));

	// The badge repeats the model's status word rather than stating a total, which the server cannot supply.
	DetailBadgeBox->SetContent(CrowdyStudioWidgets::Badge(
		FText::FromString(Status),
		IsModelLoaded(TypeName) ? CrowdyStudioWidgets::EBadgeTone::Info : CrowdyStudioWidgets::EBadgeTone::Neutral));
}

void SCrowdyLiveModelsTab::UpdateActionBar()
{
	if (!SelectionCountText.IsValid())
	{
		return;
	}

	const int32 Selected = InstanceTable.IsValid() ? InstanceTable->NumSelectedRows() : 0;
	SelectionCountText->SetText(FText::Format(LOCTEXT("LiveSelectedCount", "{0} selected"), FText::AsNumber(Selected)));
}

void SCrowdyLiveModelsTab::UpdateStatusLine()
{
	if (!StatusLineText.IsValid())
	{
		return;
	}

	// A running purge outranks everything below, which all describes a list it is in the middle of emptying. The
	// count has no total beside it because nothing ever knew one.
	if (IsPurgeRunning())
	{
		StatusLineText->SetText(FText::Format(
			LOCTEXT("LivePurgeProgress", "Deleting live models: {0} so far. Press Stop to leave the rest."),
			FText::AsNumber(Controller->GetContainerPurgeCompleted())));
		return;
	}

	// What the last purge did, until the reader asks for something else. It outranks the row count, which would
	// otherwise be taken from rows the purge deleted and the re-list has not replaced yet.
	if (!PurgeOutcomeLine.IsEmpty())
	{
		StatusLineText->SetText(PurgeOutcomeLine);
		return;
	}

	if (!SelectedTypeName.IsSet())
	{
		StatusLineText->SetText(FText::GetEmpty());
		return;
	}

	if (!IsModelLoaded(SelectedTypeName.GetValue()))
	{
		StatusLineText->SetText(LOCTEXT("LiveStatusNotRead", "Nothing read yet for this model."));
		return;
	}

	// Never a total: the query answers with a page and no count of any kind, so the only honest statement is
	// what is on screen and how to see more of it.
	const int32 Shown = InstanceTable.IsValid() ? InstanceTable->NumRows() : 0;
	StatusLineText->SetText(FText::Format(
		LOCTEXT("LiveStatusShowing", "Showing the first {0}. Narrow the filters or load more."), FText::AsNumber(Shown)));
}

void SCrowdyLiveModelsTab::SetInspectorLine(const FText& Line)
{
	if (InspectorText.IsValid())
	{
		InspectorText->SetText(Line);
	}
}

void SCrowdyLiveModelsTab::SyncEditorAppScope()
{
	const int64 CurrentAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;
	if (CurrentAppId == EditorAppId)
	{
		return;
	}

	EditorAppId = CurrentAppId;
	ClearAppScopedEditors();
}

void SCrowdyLiveModelsTab::ClearAppScopedEditors()
{
	// The filters name the previous app's types and sessions, the open model and the highlighted instance are
	// both entities of that app, and the read this page was left in the middle of describes a list that no longer
	// exists, so nothing here may continue it or report anything as read. A row left
	// highlighted would arm Delete against the new app while describing an entity that was only ever read from
	// the old one. There is no undo and no soft delete on the Game Model API, so clear all of it.
	auto ClearBox = [](const TSharedPtr<SEditableTextBox>& Box)
	{
		if (Box.IsValid())
		{
			Box->SetText(FText::GetEmpty());
		}
	};

	ClearBox(ContainerTypeFilterBox);
	ClearBox(ContainerSessionFilterBox);

	SelectedTypeName.Reset();
	if (ModelListView.IsValid())
	{
		ModelListView->ClearSelection();
	}

	if (InstanceTable.IsValid())
	{
		InstanceTable->SetRows(TArray<TSharedPtr<FCrowdyModelRow>>());
	}
	if (DetailSwitcher.IsValid())
	{
		DetailSwitcher->SetActiveWidgetIndex(0);
	}

	SetInspectorLine(LOCTEXT("LiveInspectorHint", "Select a live model to see its properties."));
	UpdateActionBar();
	UpdateStatusLine();

	LoadedTypeNames.Reset();
	bLoadedEveryType = false;
	bReadPending = false;
	PendingTypeFilter.Reset();
	PendingSessionFilter.Reset();
	bPendingAppend = false;
	LoadedTypeFilter.Reset();
	LoadedSessionFilter.Reset();
}

#undef LOCTEXT_NAMESPACE
