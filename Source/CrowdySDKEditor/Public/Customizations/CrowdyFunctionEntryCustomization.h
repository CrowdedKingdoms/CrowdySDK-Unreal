#pragma once

#include "CoreMinimal.h"
#include "Customizations/CrowdyHandlerCustomizationBase.h"
#include "K2Node_FunctionEntry.h"

// ─────────────────────────────────────────────────────────────────────────────
// FCrowdyFunctionEntryCustomization
//
// Details panel customization for UK2Node_FunctionEntry (Blueprint functions).
// Registered against "K2Node_FunctionEntry" by FCrowdySDKEditorModule.
//
// Metadata storage: UK2Node_FunctionEntry::MetaData
//   (FKismetUserDeclaredFunctionMetadata) — persists with the Blueprint asset.
//
// The pre-compile hook in FCrowdySDKEditorModule::OnBlueprintPreCompile reads
// this field and transfers CrowdyEvent onto the generated UFunction so
// UCrowdyEventManager's TFieldIterator scan finds it at runtime.
// ─────────────────────────────────────────────────────────────────────────────
class FCrowdyFunctionEntryCustomization : public FCrowdyHandlerCustomizationBase
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance()
	{
		return MakeShared<FCrowdyFunctionEntryCustomization>();
	}

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

protected:
	virtual bool                        IsStamped()        const override;
	virtual void                        SetStamped(bool bStamped) override;
	virtual const TArray<UEdGraphPin*>& GetNodePins()      const override;
	virtual int32                       GetNodeFunctionFlags() const override;
	virtual FString                     GetNodeDisplayName() const override;

private:
	TWeakObjectPtr<UK2Node_FunctionEntry> EditedNode;
};
