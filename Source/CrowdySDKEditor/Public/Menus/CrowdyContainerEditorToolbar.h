#pragma once

#include "CoreMinimal.h"

class FExtender;
class FToolBarBuilder;
class FUICommandList;
class SWidget;
class UBlueprint;

/**
 * Adds a "Crowdy" menu to the Blueprint editor toolbar for marking a Blueprint as a Game Model container. Clones
 * FCrowdyStructEditorToolbar's shared-toolbar-extensibility pattern, but targets a UBlueprint editing object and
 * drives the persisted UCrowdyContainerBlueprintExtension (mark/un-mark + type name) instead of struct metadata.
 * The menu offers a checkable "Game Model Class" toggle, a container-type-name field, and the container type's
 * pull-on-start toggle, so a class is marked and configured in one place.
 */
class FCrowdyContainerEditorToolbar
{
public:
	static void Register();
	static void Unregister();

private:
	static TSharedRef<FExtender> CreateToolbarExtender(
		const TSharedRef<FUICommandList> CommandList,
		const TArray<UObject*> EditingObjects);

	static void FillToolbar(FToolBarBuilder& ToolbarBuilder, TWeakObjectPtr<UBlueprint> WeakBlueprint);
	static TSharedRef<SWidget> BuildMenu(TWeakObjectPtr<UBlueprint> WeakBlueprint);

	static FDelegateHandle ToolbarExtenderHandle;
};
