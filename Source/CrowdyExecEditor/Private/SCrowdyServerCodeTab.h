#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyExecRevisions.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerObjectDefinition.h"
#include "Framework/SlateDelegates.h"
#include "Input/Reply.h"
#include "Layout/Visibility.h"
#include "Styling/SlateTypes.h"
#include "Templates/Function.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyServerComputeService;
class ITableRow;
class SMultiLineEditableTextBox;
class STableViewBase;

/** A Server Object definition's server code: its logic file in an editor, and the type's deployed revisions to view, compare with it and restore. */
class SCrowdyServerCodeTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyServerCodeTab)
		: _Definition(nullptr)
	{}
		SLATE_ARGUMENT(UCrowdyServerObjectDefinition*, Definition)
		/** After this tab wrote the logic file or generated the server code. */
		SLATE_EVENT(FSimpleDelegate, OnCodeWritten)
	SLATE_END_ARGS()

	virtual ~SCrowdyServerCodeTab() override;
	void Construct(const FArguments& InArgs);

	/** Follows an edit of the definition: a new Type Name, Code Source or logic file shows another file, offering to save the edits first. */
	void HandleDefinitionChanged();

	/** Offers to save unsaved edits, then writes the type's server code. Never replaces the logic file. */
	void Generate();
	bool CanGenerate() const { return WriteBlockReason.IsEmpty(); }
	FText GetGenerateToolTip() const;
	/** Why Save and Generate are off, empty when they are allowed. */
	const FText& GetWriteBlockReason() const { return WriteBlockReason; }

	/** Offers to save unsaved edits, since a deploy sends the files as they are saved. */
	void OfferSaveBeforeDeploy();

	/** Moves the cursor to the Rust function named Method, `fn method(`, and scrolls it into view. False when the code has none. */
	bool RevealFunction(const FString& Method);

	/** The logic file with edits not saved in this tab, or empty. */
	FString GetUnsavedFile() const;
	/** Drops this tab's unsaved edits and shows the file from disk again when it is in Directory or below it. */
	void ForgetEditsUnder(const FString& Directory);
	/** Keeps unsaved edits for the next time this file is shown; the editor calls it as it closes. */
	void KeepUnsavedEdits() { StashEdits(); }
	/** Shows the file from disk again when it is Path and has no unsaved edits here. */
	void ShowWritten(const FString& Path);

	/** Follows a change to the file on disk, such as a save in another editor: shows it at once, or with unsaved edits here says so instead. Runs every second while the tab is visible. */
	void FollowDisk();
	/** The code in the editor, with LF line breaks. */
	const FString& GetEditedText() const { return EditedText; }
	/** The file changed on disk while this tab had unsaved edits, and neither Reload nor Keep Mine was chosen yet. */
	bool HasChangedOnDisk() const { return bChangedOnDisk; }

private:
	struct FRevisionItem
	{
		/** The revision's place in the history, INDEX_NONE for the code in the editor. */
		int32 Index = INDEX_NONE;
		int32 Version = 0;
		FText Label;
	};

	struct FDiffRow
	{
		CrowdyExecRevisions::FDiffLine Line;
		/** The line's number in your code, or in the revision for a removed line. */
		int32 Number = 0;
	};

	using FRevisionItemPtr = TSharedPtr<FRevisionItem>;
	using FDiffRowPtr = TSharedPtr<FDiffRow>;
	using FSourceItemPtr = TSharedPtr<ECrowdyServerCodeSource>;

	TSharedRef<SWidget> MakeHeader();
	TSharedRef<SWidget> MakeIconButton(FName Icon, const TAttribute<FText>& ToolTip, const TAttribute<bool>& bEnabled, FOnClicked OnClicked);
	TSharedRef<SWidget> MakeRevisionBanner();
	TSharedRef<SWidget> MakeMissingNotice();
	TSharedRef<SWidget> MakeGapsNotice();
	TSharedRef<SWidget> MakeChangedNotice();
	EActiveTimerReturnType HandleDiskTimer(double CurrentTime, float DeltaTime);
	TSharedRef<SWidget> MakeCodeViews();
	TSharedRef<SWidget> MakeCodeView(const TSharedRef<SMultiLineEditableTextBox>& Box, const TAttribute<int32>& LineCount);

	/** Changes one of the definition's properties as an undoable edit, notifying like an edit in the details panel. */
	void EditDefinition(FName Member, const FText& Description, TFunctionRef<void(UCrowdyServerObjectDefinition&)> Edit);

	void RefreshCached();
	void RebuildRevisions();
	void ShowRevision(int32 Index);
	void ShowYourCode();
	void SetComparing(bool bCompare);
	void RefreshServiceTypes();
	bool IsTypeNameShared() const;

	void Reload();
	void RefreshState();
	/** Works out again what the definition's Functions trait asks the logic file for. */
	void RefreshExpected();
	/** Compares the code in the editor with it. */
	void RefreshGaps();
	bool SaveEdits(bool bAskIfMoved);
	bool ResolveOutsideChange();
	void StashEdits();
	void DiscardEdits();
	void SetEditedText(FString Text);
	void NotifyCodeWritten();
	void HandleServiceChanged();
	void HandleTextChanged(const FText& NewText);
	void HandleRevisionPicked(FRevisionItemPtr Item, ESelectInfo::Type SelectInfo);
	void HandleSourcePicked(FSourceItemPtr Item, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> MakeRevisionRow(FRevisionItemPtr Item);
	TSharedRef<SWidget> MakeSourceRow(FSourceItemPtr Item);
	TSharedRef<ITableRow> MakeDiffRow(FDiffRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable);

	FReply HandleGenerate();
	FReply HandleChooseFile();
	FReply HandleOpenExternal();
	FReply HandleSave();
	FReply HandleRevert();
	FReply HandleCompare();
	FReply HandleRestore();
	FReply HandleBackToYourCode();
	FReply HandleAddStubs();
	FReply HandleReloadFromDisk();
	FReply HandleKeepMine();

	FText GetPathText() const;
	FText GetGapsText() const { return GapsText; }
	FText GetAddStubsText() const;
	FText GetAddStubsToolTip() const;
	FText GetMissingText() const;
	FText GetUnsavedText() const;
	FText GetSaveToolTip() const;
	FText GetOpenExternalToolTip() const;
	FText GetSaveBlockReason() const;
	FText GetRestoreBlockReason() const;
	FText GetRestoreToolTip() const;
	FText GetCompareText() const;
	FText GetSourceText() const;
	FText GetRevisionComboText() const { return RevisionComboText; }
	FText GetViewedBanner() const { return ViewedBanner; }
	FText GetViewedWarning() const { return ViewedWarning; }
	int32 GetEditedLineCount() const { return EditedLineCount; }
	int32 GetViewedLineCount() const { return ViewedLineCount; }
	int32 GetCodeViewIndex() const;
	EVisibility GetMissingVisibility() const;
	EVisibility GetMissingGenerateVisibility() const;
	EVisibility GetGapsVisibility() const;
	EVisibility GetChangedVisibility() const;
	FText GetChangedText() const;
	EVisibility GetAddStubsVisibility() const;
	EVisibility GetUnsavedVisibility() const;
	EVisibility GetChooseFileVisibility() const;
	EVisibility GetRevisionsListVisibility() const;
	EVisibility GetRevisionVisibility() const;
	EVisibility GetViewedWarningVisibility() const;
	bool IsFilePresent() const;
	bool IsUnreadable() const;
	bool IsReadOnly() const;
	bool HasHistory() const { return !History.IsEmpty(); }
	bool CanSave() const;
	bool CanRestore() const;
	bool CanAddStubs() const { return !IsReadOnly(); }

	TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition;
	TWeakPtr<FCrowdyServerComputeService> Service;
	FSimpleDelegate OnCodeWritten;

	FEditableTextBoxStyle CodeBoxStyle;
	TSharedPtr<SMultiLineEditableTextBox> TextBox;
	TSharedPtr<SMultiLineEditableTextBox> RevisionTextBox;
	TSharedPtr<SComboBox<FRevisionItemPtr>> RevisionCombo;
	TSharedPtr<SComboBox<FSourceItemPtr>> SourceCombo;
	TSharedPtr<SListView<FDiffRowPtr>> DiffList;
	TArray<FSourceItemPtr> SourceItems;

	/** The logic file the editor shows, absolute; empty when the definition names none that can be written. */
	FString LoadedPath;
	FString ShownPath;
	/** The file as the edits started from it: what a save checks the disk against. */
	CrowdyServerCodeFiles::FDiskText Loaded;
	FString EditedText;
	/** Why Save and Generate are off, empty when they are allowed. */
	FText WriteBlockReason;
	FString CachedTypeName;
	/** The crate the definition generates now, whose Functions trait the code is compared with; empty while it cannot generate. */
	CrowdyExecCodegen::FGeneratedCrate Expected;
	/** How the code in the editor falls short of it. */
	CrowdyExecCodegen::FLogicGaps Gaps;
	FText GapsText;

	/** The type's deployed revisions, newest first, as last read. */
	TArray<CrowdyExecRevisions::FRevision> History;
	TArray<FRevisionItemPtr> RevisionItems;
	TArray<FDiffRowPtr> DiffRows;
	FText RevisionComboText;
	FText ViewedBanner;
	FText ViewedWarning;
	FString ViewedLogic;
	/** The revision shown instead of the editor, INDEX_NONE while editing. */
	int32 ViewedRevision = INDEX_NONE;
	int32 EditedLineCount = 1;
	int32 ViewedLineCount = 1;

	FDelegateHandle ServiceChangedHandle;
	/** The service's data generation this tab last showed. */
	uint32 SeenGeneration = 0;
	bool bFileExists = false;
	bool bFileReadable = false;
	bool bOwnFile = false;
	bool bDirty = false;
	/** Restored unsaved edits whose file changed on disk after they were made. */
	bool bStale = false;
	/** The file's time stamp when last read, saved or seen changed, so a later change on disk is noticed. */
	FDateTime DiskStamp;
	bool bChangedOnDisk = false;
	bool bTypeNameValid = false;
	/** Another of the project's definitions has this Type Name, so both would write one crate folder. */
	bool bSharedTypeName = false;
	bool bComparing = false;
	bool bViewedHasLogic = false;
};
