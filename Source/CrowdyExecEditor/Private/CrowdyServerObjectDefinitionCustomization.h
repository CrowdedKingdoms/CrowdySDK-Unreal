#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "IDetailCustomization.h"
#include "Layout/Visibility.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/Input/NumericTypeInterface.h"

class FCrowdyServerComputeService;
class IDetailCategoryBuilder;
class IDetailLayoutBuilder;
class IDetailPropertyRow;
class IPropertyHandle;
class SWidget;
class SWrapBox;
class UCrowdyServerObjectDefinition;
struct FCrowdyServerFunction;
struct FCrowdyServerObjectSelection;
struct FCrowdyServerTimer;
struct FPropertyBagPropertyDesc;
struct FPropertyChangedEvent;

/** How the Server Object editor writes a definition's functions, timers and times. */
namespace CrowdyServerObjectText
{
	/** Reads and writes seconds as seconds, with no trailing zeros: 2 s, 0.25 s. */
	TSharedRef<INumericTypeInterface<float>> MakeSecondsInterface();

	/** A function's inputs and outputs named as its Call node's pins: (Amount) returns Total. With bFieldNames a struct's fields are listed instead of its name. */
	FText Signature(const FCrowdyServerFunction& Function, bool bFieldNames);

	/** A timer's name and what it does: Respawn: Once After 10 s; New Timer while it has no name. */
	FText TimerTitle(const FCrowdyServerTimer& Timer);
}

/** Rows the Server Object editor's details share. */
namespace CrowdyServerObjectRows
{
	/** Shows a float seconds property as MakeSecondsInterface writes it, in a plain box with no slider. */
	void ShowSeconds(IDetailPropertyRow& Row, const TSharedRef<IPropertyHandle>& Seconds);

	/** Shows a Variables From, Inputs From or Outputs From property as a check box named Name, checked for a struct of your own. */
	void ShowUseStruct(IDetailPropertyRow& Row, const TSharedRef<IPropertyHandle>& Form, const FText& Name);
}

/** A List input's Value Range, kept as its ClampMin and ClampMax metadata, which the generated server code checks. */
namespace CrowdyServerValueRange
{
	inline const TCHAR* const MinKey = TEXT("ClampMin");
	inline const TCHAR* const MaxKey = TEXT("ClampMax");

	/** Whether the input is a single number (not a bool, enum or container), the only kind a Value Range fits. */
	bool CanHaveRange(const FPropertyBagPropertyDesc& Desc);

	/** Sets one bound from what was typed, or clears it when empty. False when nothing changes or the text is not a fitting number. */
	bool SetBound(FPropertyBagPropertyDesc& Desc, FName Key, const FString& Text);
}

/**
 * The Server Object definition's details. In its asset editor they follow the Server Object panel's selection: the
 * object's settings, one variable, or one function's inputs and outputs. Elsewhere every setting is shown at once.
 */
class FCrowdyServerObjectDefinitionCustomization : public IDetailCustomization
{
public:
	explicit FCrowdyServerObjectDefinitionCustomization(TSharedPtr<FCrowdyServerObjectSelection> InSelection = nullptr);

	static TSharedRef<IDetailCustomization> MakeInstance();
	static TSharedRef<IDetailCustomization> MakeForEditor(TSharedPtr<FCrowdyServerObjectSelection> InSelection);

	virtual ~FCrowdyServerObjectDefinitionCustomization() override;
	virtual void PendingDelete() override;
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	void CustomizeFunction(IDetailLayoutBuilder& DetailBuilder, int32 Index);
	void AddPins(IDetailLayoutBuilder& DetailBuilder, const TSharedRef<IPropertyHandle>& Function, int32 Index, bool bOutputs);
	void AddValueRanges(IDetailCategoryBuilder& Category, int32 Index, const TSharedPtr<IPropertyHandle>& List);
	void AddObjectCategories(IDetailLayoutBuilder& DetailBuilder);
	void CustomizeVariable(IDetailLayoutBuilder& DetailBuilder, FName Name);
	void RenameVariable(TSharedPtr<IPropertyHandle> Variables, FName Name, const FText& NewText);
	void SetVisibleToPlayers(FName Name, bool bVisible);
	void AddAdvancedCategory(IDetailLayoutBuilder& DetailBuilder);
	TSharedRef<SWidget> MakeTypeNameValue();
	TSharedRef<SWidget> MakeFieldCheckBox(FName Field, const FText& Label, bool bWatched);
	TSharedRef<SWidget> MakeMissingField(FName Field);

	void Disconnect();
	void RefreshTypeName();
	void RefreshServiceTypes();
	void RebuildPlayersSee();
	void SetWatched(FName Field, bool bWatched);
	FText ProblemFor(const FString& TypeName) const;

	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	bool HandleDeferredRefresh(float DeltaTime);
	void HandleServiceChanged();
	void HandleTypeNameTyped(const FText& NewText);
	void HandleTypeNameCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void HandleUseSuggestion();
	void HandleWatchedChanged(ECheckBoxState NewState, FName Field);
	void HandleRemoveWatched(FName Field);

	FText GetTypeNameText() const { return TypeNameText; }
	FText GetTypeNameProblem() const { return TypeNameProblem; }
	FText GetSuggestionText() const { return SuggestionText; }
	EVisibility GetProblemVisibility() const;
	EVisibility GetSuggestionVisibility() const;

	TSharedPtr<FCrowdyServerObjectSelection> Selection;
	TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition;
	TWeakPtr<FCrowdyServerComputeService> Service;
	TSharedPtr<IPropertyHandle> TypeNameHandle;
	TSharedPtr<IPropertyHandle> WatchedHandle;
	TSharedPtr<SWrapBox> PlayersSeeBox;

	/** The Type Name as stored on the definition, which may differ from what is being typed. */
	FString StoredTypeName;
	FString SuggestedTypeName;
	FText SuggestionText;
	FText TypeNameText;
	FText TypeNameProblem;

	FDelegateHandle PropertyChangedHandle;
	FDelegateHandle ServiceChangedHandle;
	FTSTicker::FDelegateHandle RefreshTicker;
	/** The service's data generation last checked for a shared Type Name. */
	uint32 SeenGeneration = 0;
	/** Another of the project's definitions has this Type Name, so both would write one crate folder. */
	bool bSharedTypeName = false;
};
