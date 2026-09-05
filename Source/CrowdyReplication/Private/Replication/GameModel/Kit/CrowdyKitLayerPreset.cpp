#include "Replication/GameModel/Kit/CrowdyKitLayerPreset.h"

#define LOCTEXT_NAMESPACE "CrowdyKit"

namespace
{
	// A genre label with its type prefix appended when one is set, so a config with
	// several layers of the same genre is still legible in the deploy preview.
	FText LayerLabel(const FText& Genre, const FString& Prefix)
	{
		if (Prefix.IsEmpty())
		{
			return Genre;
		}
		return FText::Format(LOCTEXT("KitLayerLabelFmt", "{0}: {1}"), Genre, FText::FromString(Prefix));
	}
}

FText UCrowdyKitLayerPreset::GetLayerDisplayName() const
{
	return LOCTEXT("KitLayerGeneric", "Kit Layer");
}

FText UCrowdyCombatPreset::GetLayerDisplayName() const
{
	return LayerLabel(LOCTEXT("KitLayerCombat", "Combat"), TypePrefix);
}

FText UCrowdyLeaderboardsPreset::GetLayerDisplayName() const
{
	return LayerLabel(LOCTEXT("KitLayerLeaderboards", "Leaderboards"), TypePrefix);
}

FText UCrowdyGuildPreset::GetLayerDisplayName() const
{
	return LayerLabel(LOCTEXT("KitLayerGuild", "Guild"), TypePrefix);
}

FText UCrowdyLivingWorldPreset::GetLayerDisplayName() const
{
	return LayerLabel(LOCTEXT("KitLayerLivingWorld", "Living World"), TypePrefix);
}

#undef LOCTEXT_NAMESPACE
