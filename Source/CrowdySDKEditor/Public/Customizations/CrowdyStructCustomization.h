#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class UUserDefinedStructEditorData;
class IDetailLayoutBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// FCrowdyStructCustomization
//
// Details panel customization for UUserDefinedStructEditorData.
//
// Registered against "UserDefinedStructEditorData" — NOT "UserDefinedStruct".
//
// ── Why EditorData and not the struct itself ─────────────────────────────────
// FUserDefinedStructEditor (the window opened when a user double-clicks a
// struct asset) builds its Details panel against the struct's EditorData
// sub-object (UUserDefinedStructEditorData), not against the struct UObject
// directly. Registering a customization against "UserDefinedStruct" never
// fires inside that editor — only against the Content Browser's selection
// inspector, which doesn't show struct fields anyway.
//
// Registering against "UserDefinedStructEditorData" makes the customization
// fire in the same Details panel where the user sees the member variables,
// which is what the user actually wants.
//
// ── What this adds ──────────────────────────────────────────────────────────
// One read-only row inside a new "Crowdy SDK" category:
//
//   • Status   — "Not a Crowdy payload" / "Event" / "ActorUpdate"
//   • TypeID   — auto-derived from the struct's asset path, shown only
//                when a category is active.
//
// Stamp changes are done via the Content Browser right-click on the struct
// asset (FCrowdyStructAssetActions). This customization is read-only — it
// surfaces the state inline next to the variables, so the user can see what
// they've stamped without leaving the struct editor.
//
// (Read-only avoids the need for a dropdown in a panel that already houses
// the variable editing UI, and avoids any chance of the customization being
// blamed for properties disappearing.)
// ─────────────────────────────────────────────────────────────────────────────
class FCrowdyStructCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FCrowdyStructCustomization>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TWeakObjectPtr<UUserDefinedStructEditorData> EditedEditorData;

	// Resolves the owning UUserDefinedStruct from the EditorData's outer.
	// Returns nullptr if the relationship isn't intact for any reason.
	class UUserDefinedStruct* GetOwningStruct() const;
};