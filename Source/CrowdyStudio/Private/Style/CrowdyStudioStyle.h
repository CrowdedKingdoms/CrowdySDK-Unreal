// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class ISlateStyle;
class FSlateStyleSet;
struct FSlateBrush;

/**
 * The CrowdyStudio design language. One FSlateStyleSet holding the dark-theme palette,
 * rounded-box surfaces (cards, platters, inputs), status-badge brushes, button/input
 * styles, and the line-art icon brushes loaded from the plugin's Resources/Icons folder.
 *
 * Registered once at editor-module startup so every view can pull brushes and text styles
 * by name (e.g. .TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Title")) and tint text
 * consistently from the shared palette accessors.
 */
class FCrowdyStudioStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static const ISlateStyle& Get();
	static FName StyleName();

	// "login" -> the "Crowdy.Icon.login" vector brush, or nullptr if not registered.
	static const FSlateBrush* IconBrush(const TCHAR* IconName);

	// Shared palette (tuned for the editor's dark theme). Centralised so badges, text tints,
	// and dot indicators stay consistent across views.
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
