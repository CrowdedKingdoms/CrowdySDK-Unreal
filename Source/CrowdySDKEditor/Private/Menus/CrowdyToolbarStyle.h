#pragma once

#include "CoreMinimal.h"

namespace CrowdyToolbarStyle
{
	// The shared Crowdy style set's registered name (mirrors FCrowdyStudioStyle::StyleName) and the Game Model
	// glyph the Studio's Game Model page uses. Referenced by name so these editor toolbars need no include of the
	// Studio module's private style header; the brush resolves lazily at render time, by which point the Studio
	// module (a startup dependency of this one) has registered the style.
	inline const FName StyleSetName(TEXT("CrowdyStudioStyle"));
	inline const TCHAR* const GameModelIconBrush = TEXT("Crowdy.Icon.cube");
}
