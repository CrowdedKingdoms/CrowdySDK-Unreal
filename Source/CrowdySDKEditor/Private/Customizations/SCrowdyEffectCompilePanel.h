// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Customizations/CrowdyEffectPayloadPreview.h"
#include "Widgets/SCompoundWidget.h"

class UCrowdyEffect;

/**
 * The compile readout for one effect: what it compiles to and any diagnostics, plus the deploy payload it would send,
 * toggled between a readable summary and the exact wire JSON. Both panes scroll inside a bounded height, so a long
 * payload never stretches whatever hosts this widget.
 *
 * It sits under the authoring canvas in both effect editors, so the effect being edited and what it compiles to are
 * visible at once. Refresh() recomputes from the effect; the hosting toolkit calls it after every edit.
 */
class SCrowdyEffectCompilePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyEffectCompilePanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCrowdyEffect>, Effect)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Recomputes both readouts from the effect. Cheap enough to call on every graph or script edit: it is one Compile()
	// plus two string builds, the same work the details panel already did on each refresh.
	void Refresh();

private:
	FText GetSummaryText() const;
	FText GetPayloadText() const;

	TWeakObjectPtr<UCrowdyEffect> Effect;

	// Both payload views are built from the same compile, so the Summary / Wire JSON toggle switches instantly.
	CrowdyEffectPayloadPreview::EMode PayloadMode = CrowdyEffectPayloadPreview::EMode::Summary;
	FString CachedSummary;
	FString CachedPayloadSummary;
	FString CachedPayloadWireJson;
};
