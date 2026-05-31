#pragma once

#include "CoreMinimal.h"

class FExtender;
class FToolBarBuilder;
class FUICommandList;
class UUserDefinedStruct;

class FCrowdyStructEditorToolbar
{
public:
	static void Register();
	static void Unregister();

private:
	static TSharedRef<FExtender> CreateToolbarExtender(
		const TSharedRef<FUICommandList> CommandList,
		const TArray<UObject*> EditingObjects);

	static void FillToolbar(
		FToolBarBuilder& ToolbarBuilder,
		TWeakObjectPtr<UUserDefinedStruct> WeakStruct);

	static FDelegateHandle ToolbarExtenderHandle;
};
