// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class ISlateStyle;
class FSlateStyleSet;

/**
 * The Crowded Kingdoms dark-theme palette and the few widget styles built from it, usable in a game
 * and in the editor. Built from the core style only, so nothing here needs the editor's app style.
 * Get() needs Slate running; the palette accessors do not.
 */
class CROWDYSERVICES_API FCrowdyBrandStyle
{
public:
	static const ISlateStyle& Get();
	static FName StyleName();
	static void Shutdown();

	static FLinearColor Panel();        // window background
	static FLinearColor Rail();         // navigation rail background
	static FLinearColor Surface();      // card / platter fill
	static FLinearColor SurfaceHover(); // raised / hovered surface
	static FLinearColor Line();         // hairline borders
	static FLinearColor TextPrimary();
	static FLinearColor TextSecondary();
	static FLinearColor TextSubtle();
	static FLinearColor Gold();         // brand accent (matches the wordmark's KINGDOMS gold)
	static FLinearColor GoldBright();
	static FLinearColor Success();
	static FLinearColor Warning();
	static FLinearColor Danger();
	static FLinearColor Info();

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> Instance;
};
