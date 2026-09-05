#pragma once

#include "CoreMinimal.h"

struct FCrowdyRepLayout;

/**
 * An advisory over a resolved CrowdyState rep layout: an enum-typed property that is not part of the
 * periodic keyframe heartbeat.
 *
 * An enum field is held state. It is diffed and sent when it changes and never again, so an observer
 * that starts watching after the last change, or that loses the one unreliable datagram carrying it,
 * holds the wrong value until the value happens to change. Adding the property to the keyframe
 * heartbeat re-sends it on the keyframe interval, which is the only thing on this plane that makes a
 * held value converge on its own. C++ leaves the opt-in off by default while the Blueprint variable
 * panel ticks it on, so the case this catches is almost always a C++-declared enum.
 *
 * Advisory only. Nothing is refused, no default changes, and a transient enum that is meant to be
 * missed is a legitimate reason to ignore the line.
 *
 * WHAT IT MEASURES, exactly: the property's type is an enum (an enum class, or a byte property with an
 * enum), it does not carry the heartbeat flag, and it is not owner-only. Owner-only properties are
 * excluded because a keyframe never carries them whatever they are marked, so the advice would be a
 * remedy that does nothing. The verdict is taken off the assembled layout rather than off UPROPERTY
 * metadata, so it reads the same in a cooked build, where the layout comes from the baked table and
 * metadata is gone.
 *
 * WHO IT NAMES: the class the property is DECLARED on, not the class whose layout it was found in. A
 * layout includes its super-class properties, so a property declared once on a base class appears in
 * every subclass's layout, and naming those would say the same thing once per subclass while pointing at
 * classes the UPROPERTY cannot be edited on.
 */
namespace CrowdyStateHeartbeatAdvisory
{
	/**
	 * Judges every property of a freshly resolved layout and warns about each qualifying one, once per
	 * property for the life of the process. Later resolutions of the same property are counted and stay
	 * silent; DescribeAdvised reports them.
	 *
	 * Called where a layout is built and cached, which happens once per class per registration sweep,
	 * never on the per-entity or per-delta path.
	 */
	void ReportLayout(const FCrowdyRepLayout& Layout);

	/**
	 * Every property currently advised, one line each, with the number of resolutions that property's
	 * single warning stands for, and a total. This is what stops the once-per-property memo from hiding
	 * the size of the problem from anyone who arrives after the warning scrolled past.
	 *
	 * A property that comes to carry the heartbeat flag (or to be owner-only, or to stop being an enum)
	 * is dropped from the record the next time its class resolves, so the remedy shows up as the
	 * property leaving this list.
	 */
	FString DescribeAdvised();

	// Forgets every advised property, so a test can start from silence and read exact totals back. A
	// registration sweep resolves the test fixtures before any case runs, and without this a case would be
	// measuring the memo rather than the advisory. Deliberately wholesale rather than per class: the totals
	// DescribeAdvised reports are over everything, so a case can only assert them from an empty record. The
	// price is that any property advised earlier in the same process speaks once more afterwards.
	void ForgetAdvisedForTest();
}
