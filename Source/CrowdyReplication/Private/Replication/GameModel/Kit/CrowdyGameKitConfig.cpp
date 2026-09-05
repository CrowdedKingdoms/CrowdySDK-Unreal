#include "Replication/GameModel/Kit/CrowdyGameKitConfig.h"

#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#include "Replication/GameModel/Kit/CrowdyGameKitPreview.h"

#define LOCTEXT_NAMESPACE "CrowdyKit"

EDataValidationResult UCrowdyGameKitConfig::IsDataValid(FDataValidationContext& ValidationContext) const
{
	const EDataValidationResult Base = Super::IsDataValid(ValidationContext);

	// Count only configured layers: an empty config, or one with only unfilled slots, is a not-yet-authored
	// asset, not an error. The emit itself rejects an empty layer list, so guard it here.
	int32 ConfiguredLayers = 0;
	for (const TObjectPtr<UCrowdyKitLayerPreset>& Layer : Layers)
	{
		if (Layer)
		{
			++ConfiguredLayers;
		}
	}
	if (ConfiguredLayers == 0)
	{
		return Base;
	}

	// Validation is independent of the app id (it checks names, group ids, and section flags), so a fixed
	// placeholder is enough; the real app id is sourced at deploy time.
	const FCrowdyKitPreview Preview = CrowdyKitPreviewLayers(Layers, 1, SessionId);
	if (!Preview.bOk)
	{
		ValidationContext.AddError(FText::Format(
			LOCTEXT("KitConfigEmitFailed", "Game Kit cannot be deployed: {0}"),
			FText::FromString(Preview.Error)));
		return EDataValidationResult::Invalid;
	}

	return Base == EDataValidationResult::NotValidated ? EDataValidationResult::Valid : Base;
}

#undef LOCTEXT_NAMESPACE
#endif // WITH_EDITOR
