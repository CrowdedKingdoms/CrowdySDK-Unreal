#include "Customizations/CrowdyStructCustomization.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "CrowdySDKEditor.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Text/STextBlock.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
	// Mirrors the resolver the registries are given at startup: an explicit entry in the project's
	// ID overrides replaces the path-derived value, so a struct listed there goes on the wire under
	// the ID from the list and not under its hash.
	uint16 ResolveDisplayedStructTypeID(const UScriptStruct* Struct)
	{
		for (const FCrowdyIDOverride& Override : GetDefault<UCrowdySDKDeveloperSettings>()->IDOverrides)
		{
			if (Override.Struct.Get() == Struct)
				return static_cast<uint16>(Override.OverrideID);
		}

		return FCrowdyTypeIDGenerator::GenerateFromStruct(Struct);
	}
}

void FCrowdyStructCustomization::CustomizeDetails(
	IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.IsEmpty()) return;

	EditedEditorData = Cast<UUserDefinedStructEditorData>(Objects[0].Get());
	if (!EditedEditorData.IsValid()) return;

	UUserDefinedStruct* OwningStruct = GetOwningStruct();
	if (!OwningStruct) return;

	// ECategoryPriority::TypeSpecific keeps us out of the way of the
	// EditorData's existing categories (Variables, Defaults). Unedited
	// categories remain visible per IDetailLayoutBuilder semantics, so the
	// struct's variable rows are not affected.
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(
			"CrowdySDK",
			FText::FromString(TEXT("Crowdy SDK")),
			ECategoryPriority::TypeSpecific);

	// Resolved the same way registration resolves it: an entry in the project's ID overrides wins
	// outright, and only a struct with no entry falls back to the path hash. Calling the runtime
	// generator for that fallback rather than restating its formula keeps the two in step.
	const uint16 TypeID = ResolveDisplayedStructTypeID(OwningStruct);

	Category.AddCustomRow(FText::FromString(TEXT("Auto TypeID")))
	.NameContent()
	[
		SNew(STextBlock)
		.Text(FText::FromString(TEXT("Auto TypeID")))
		.Font(DetailBuilder.GetDetailFont())
		.ToolTipText(FText::FromString(
			TEXT("The TypeID assigned to this struct when it is used as a Crowdy\n"
			     "payload (a CrowdyEvent handler parameter, a reception layer's\n"
			     "supported event, or an executor's state struct).\n"
			     "Derived from its asset path, so it is stable unless the asset is\n"
			     "moved or renamed. An entry for this struct in the ID Collision\n"
			     "Overrides list (Project Settings, Plugins, Crowdy SDK) replaces\n"
			     "that derived value, and is what is shown here when present.")))
	]
	.ValueContent()
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::FromInt(TypeID)))
		.Font(DetailBuilder.GetDetailFont())
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
	];
}

// UUserDefinedStructEditorData lives as a sub-object of the struct asset, so
// GetOuter() returns the UUserDefinedStruct that owns it.
UUserDefinedStruct* FCrowdyStructCustomization::GetOwningStruct() const
{
	if (!EditedEditorData.IsValid()) return nullptr;
	return Cast<UUserDefinedStruct>(EditedEditorData->GetOuter());
}
