#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class IStructureDetailsView;
class SEditableTextBox;
class UCrowdyServerObjectDefinition;
class FStructOnScope;
struct FCrowdyServerFunction;

/** Calls one Server Only or Developers Only function on a live instance as the signed-in developer: its Instance Id, its inputs, and the answer. */
class SCrowdyExecDeveloperCall : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyExecDeveloperCall) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCrowdyServerObjectDefinition>, Definition)
		SLATE_ARGUMENT(int32, FunctionIndex)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** A window for the definition's function at FunctionIndex. */
	static void Open(UCrowdyServerObjectDefinition& Definition, int32 FunctionIndex);

private:
	FReply OnCallClicked();
	bool CanCall() const;
	/** The function the window was opened for, if the definition still has it and it is still for developers. */
	const FCrowdyServerFunction* FindFunction() const;

	TWeakObjectPtr<UCrowdyServerObjectDefinition> Definition;
	/** Found again by name on every call, since functions may be added, removed or reordered while the window is open. */
	FName FunctionName;
	/** The function's struct when the window opened, kept alive for the inputs view and compared on every call. */
	TStrongObjectPtr<UScriptStruct> InputsStruct;
	TSharedPtr<FStructOnScope> Inputs;
	TSharedPtr<IStructureDetailsView> InputsView;
	TSharedPtr<SEditableTextBox> KeyBox;
	FText Answer;
	FSlateColor AnswerColor;
	bool bCalling = false;
};
