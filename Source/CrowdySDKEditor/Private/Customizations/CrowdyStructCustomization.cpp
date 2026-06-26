#include "Customizations/CrowdyStructCustomization.h"

#include "CrowdySDKEditor.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Text/STextBlock.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

// ─────────────────────────────────────────────────────────────────────────────
// IDetailCustomization
// ─────────────────────────────────────────────────────────────────────────────
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

	// ── Crowdy SDK category ──────────────────────────────────────────────────
	//
	// ECategoryPriority::TypeSpecific keeps us out of the way of the
	// EditorData's existing categories (Variables, Defaults). Unedited
	// categories remain visible per IDetailLayoutBuilder semantics, so the
	// struct's variable rows are not affected.
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(
			"CrowdySDK",
			FText::FromString(TEXT("Crowdy SDK")),
			ECategoryPriority::TypeSpecific);

	// Formula MUST match the runtime CrowdySDK module exactly.
	// uint16 range: 0 is reserved for "invalid", so add 1 after modulo.
	const FString Path  = OwningStruct->GetPathName();
	const uint32  Hash  = FCrc::MemCrc32(*Path, Path.Len() * sizeof(TCHAR));
	const uint16  TypeID = static_cast<uint16>((Hash % 65535u) + 1u);

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
			     "Derived from its asset path — stable unless the asset\n"
			     "is moved or renamed.")))
	]
	.ValueContent()
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::FromInt(TypeID)))
		.Font(DetailBuilder.GetDetailFont())
		.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
	];
}

// ─────────────────────────────────────────────────────────────────────────────
// Owning struct resolution
//
// UUserDefinedStructEditorData lives as a sub-object of the struct asset.
// GetOuter() returns the UUserDefinedStruct that owns it.
// ─────────────────────────────────────────────────────────────────────────────
UUserDefinedStruct* FCrowdyStructCustomization::GetOwningStruct() const
{
	if (!EditedEditorData.IsValid()) return nullptr;
	return Cast<UUserDefinedStruct>(EditedEditorData->GetOuter());
}
