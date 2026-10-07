#pragma once

#include "CoreMinimal.h"

/** What the asset editor's Server Object panel has selected, which its Details tab shows. */
struct FCrowdyServerObjectSelection
{
	enum class EKind : uint8
	{
		/** The object itself: its settings. */
		None,
		Variable,
		Function
	};

	EKind Kind = EKind::None;
	/** The variable's name. */
	FName Variable;
	/** The function's index in the definition's Functions. */
	int32 Function = INDEX_NONE;

	FSimpleMulticastDelegate OnChanged;

	void SelectNone()
	{
		Set(EKind::None, NAME_None, INDEX_NONE);
	}

	void SelectVariable(FName Name)
	{
		Set(EKind::Variable, Name, INDEX_NONE);
	}

	void SelectFunction(int32 Index)
	{
		Set(EKind::Function, NAME_None, Index);
	}

private:
	void Set(EKind InKind, FName InVariable, int32 InFunction)
	{
		if (Kind == InKind && Variable == InVariable && Function == InFunction)
		{
			return;
		}
		Kind = InKind;
		Variable = InVariable;
		Function = InFunction;
		OnChanged.Broadcast();
	}
};
