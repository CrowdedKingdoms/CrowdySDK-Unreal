#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class UUserDefinedStructEditorData;
class IDetailLayoutBuilder;

/**
 * Details panel customization for UUserDefinedStructEditorData, adding a read-only "Crowdy SDK"
 * category next to a user-defined struct's member variables.
 *
 * It is registered against "UserDefinedStructEditorData" and not against "UserDefinedStruct":
 * the struct editor builds its Details panel from the struct's EditorData sub-object, so a
 * customization registered against the struct itself never fires there. It would only appear in
 * the Content Browser's selection inspector, which does not show the struct's fields at all.
 *
 * The row it adds shows the TypeID this struct would carry as a Crowdy payload. It is read-only:
 * a struct is stamped from the Content Browser's right-click menu instead, and this only surfaces
 * the resulting state where the variables are edited.
 */
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