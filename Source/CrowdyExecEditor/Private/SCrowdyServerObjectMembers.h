#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EditorUndoClient.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

struct FCrowdyServerObjectSelection;
class ITableRow;
class STableViewBase;
class UCrowdyServerObjectDefinition;
struct FPropertyChangedEvent;

DECLARE_DELEGATE_OneParam(FOnCrowdyOpenServerFunction, const FString& /*Method*/);

/** The definition's Type Settings, variables and functions, listed the way My Blueprint lists a Blueprint's; the selection drives the Details tab. */
class SCrowdyServerObjectMembers : public SCompoundWidget, public FSelfRegisteringEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SCrowdyServerObjectMembers)
		: _Definition(nullptr)
	{}
		SLATE_ARGUMENT(UCrowdyServerObjectDefinition*, Definition)
		SLATE_ARGUMENT(TSharedPtr<FCrowdyServerObjectSelection>, Selection)
		/** A function was double-clicked. */
		SLATE_EVENT(FOnCrowdyOpenServerFunction, OnOpenFunction)
	SLATE_END_ARGS()

	virtual ~SCrowdyServerObjectMembers() override;
	void Construct(const FArguments& InArgs);

	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** Selects the Type Settings row, even when a section heading already stands for the type. */
	void ShowTypeSettings();

	struct FItem
	{
		enum class EType : uint8
		{
			/** The type's own settings, selected when no variable or function is. */
			TypeSettings,
			Variables,
			Functions,
			/** The member functions Members From This Object adds, listed but not editable. */
			BuiltIns,
			Variable,
			Function,
			BuiltIn
		};

		EType Type = EType::Variable;
		FName Name;
		int32 Index = INDEX_NONE;
		FText Label;
		/** A variable's type, or a function's pins: (Amount) -> Total. */
		FText Detail;
		FSlateColor TypeColor;
		FText Problem;
		/** A function's Callable By tag, such as Server Only; empty shows none. */
		FText Tag;
		bool bVisibleToPlayers = false;
		/** Marked visible to players but no longer a variable. */
		bool bStale = false;
		TArray<TSharedPtr<FItem>> Children;
	};
	using FItemPtr = TSharedPtr<FItem>;

	/** The row that shows Selection: Type Settings when nothing is selected, null when the selected member has no row. */
	static FItemPtr FindRow(TConstArrayView<FItemPtr> Roots, const FCrowdyServerObjectSelection& Selection);

	/** Selects what Item stands for: its variable or function, or the type's settings for any other row or none. */
	static void SelectItem(const FItemPtr& Item, FCrowdyServerObjectSelection& Selection);

private:
	void Rebuild();
	void RestoreSelection(bool bDropMissing);
	TSharedRef<ITableRow> MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeTypeSettingsRow(const FItemPtr& Item);
	TSharedRef<SWidget> MakeSectionRow(const FItemPtr& Item);
	TSharedRef<SWidget> MakeMemberRow(const FItemPtr& Item);
	TSharedPtr<SWidget> MakeContextMenu();

	void EditDefinition(const FText& Description, TFunctionRef<void(UCrowdyServerObjectDefinition&)> Edit);
	void AddVariable();
	void RemoveVariable(FName Name);
	void SetVisibleToPlayers(FName Name, bool bVisible);
	void SetVariablesUseStruct(bool bUseStruct);
	void AddFunction();
	void DuplicateFunction(int32 Index);
	void RemoveFunction(int32 Index);

	void HandleSelectionChanged(FItemPtr Item, ESelectInfo::Type SelectInfo);
	void HandleDoubleClick(FItemPtr Item);
	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	bool HandleDeferredRebuild(float DeltaTime);
	void HandleExternalSelection();

	TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition;
	TSharedPtr<FCrowdyServerObjectSelection> Selection;
	FOnCrowdyOpenServerFunction OnOpenFunction;
	TSharedPtr<STreeView<FItemPtr>> Tree;
	TArray<FItemPtr> Roots;
	/** Kept across rebuilds, so the tree keeps it selected. */
	FItemPtr TypeSettings;
	FDelegateHandle PropertyChangedHandle;
	FDelegateHandle SelectionChangedHandle;
	FTSTicker::FDelegateHandle RebuildTicker;
	/** Set while the tree itself changes the selection, so the echo is not applied back to the tree. */
	bool bSelecting = false;
};
