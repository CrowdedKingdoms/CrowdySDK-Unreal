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
		Function,
		Message
	};

	EKind Kind = EKind::None;
	/** The variable's name. */
	FName Variable;
	/** The function's index in the definition's Functions. */
	int32 Function = INDEX_NONE;
	/** The message's index in the definition's Messages. */
	int32 Message = INDEX_NONE;

	FSimpleMulticastDelegate OnChanged;

	void SelectNone()
	{
		Set(EKind::None, NAME_None, INDEX_NONE, INDEX_NONE);
	}

	void SelectVariable(FName Name)
	{
		Set(EKind::Variable, Name, INDEX_NONE, INDEX_NONE);
	}

	void SelectFunction(int32 Index)
	{
		Set(EKind::Function, NAME_None, Index, INDEX_NONE);
	}

	void SelectMessage(int32 Index)
	{
		Set(EKind::Message, NAME_None, INDEX_NONE, Index);
	}

private:
	void Set(EKind InKind, FName InVariable, int32 InFunction, int32 InMessage)
	{
		if (Kind == InKind && Variable == InVariable && Function == InFunction && Message == InMessage)
		{
			return;
		}
		Kind = InKind;
		Variable = InVariable;
		Function = InFunction;
		Message = InMessage;
		OnChanged.Broadcast();
	}
};
