#include "CrowdyStateHeartbeatAdvisoryTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "CoreGlobals.h"
#include "CrowdyReplicationLog.h"
#include "CrowdyStateHeartbeatAdvisory.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Subsystem/CrowdyAutoRegistry.h"

// An enum-typed CrowdyState property that is not on the keyframe heartbeat replicates on change and is
// never re-sent. Nothing about it is malformed, every counter reads healthy, and the only symptom is a
// client that joined late or lost one datagram showing a stale value for as long as the value happens
// not to change.
//
// The verdict is taken once, where a class's layout is built and cached, which is also the only place
// the property's type and its heartbeat flag are visible together. These cases cover that the property
// is named where the verdict is taken, that the properties with nothing wrong with them stay unnamed,
// that the line is said once per property while a different property still speaks, that the record
// still reports the resolutions the memo silenced, and that a fixed property leaves the record.
namespace
{
	constexpr EAutomationTestFlags CrowdyStateHeartbeatAdvisoryTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The sentence every advisory line contains, and nothing else in this category does. Counting on it
	// keeps an unrelated CrowdyReplication warning from another subsystem out of these numbers.
	const TCHAR* const CrowdyStateHeartbeatAdvisoryMarker = TEXT("is not part of the keyframe heartbeat");

	// Collects LogCrowdyReplication warnings for as long as it is in scope. The lines are the deliverable,
	// so the cases read them rather than a counter kept beside them, which would still read green if the
	// line itself were dropped.
	class FCrowdyStateHeartbeatAdvisoryLogCapture : public FOutputDevice
	{
	public:

		FCrowdyStateHeartbeatAdvisoryLogCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FCrowdyStateHeartbeatAdvisoryLogCapture()
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category != LogCrowdyReplication.GetCategoryName() || Verbosity != ELogVerbosity::Warning)
			{
				return;
			}

			FScopeLock Lock(&Mutex);
			Lines.Add(Message);
		}

		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		// Advisory lines since the last take. Taking clears, so consecutive calls read consecutive
		// stretches of the run, which is what lets a case say "and then it was silent".
		TArray<FString> TakeAdvisories()
		{
			GLog->FlushThreadedLogs();

			FScopeLock Lock(&Mutex);
			TArray<FString> Taken;
			for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
			{
				if (!Lines[Index].Contains(CrowdyStateHeartbeatAdvisoryMarker))
				{
					continue;
				}

				Taken.Insert(Lines[Index], 0);
				Lines.RemoveAt(Index);
			}
			return Taken;
		}

	private:

		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	int32 CountAdvisoryLinesContaining(const TArray<FString>& Lines, const FString& Needle)
	{
		int32 Count = 0;
		for (const FString& Line : Lines)
		{
			Count += Line.Contains(Needle) ? 1 : 0;
		}
		return Count;
	}

	// Resolves a class exactly as the registry does for a class it has not seen before. A fresh registry
	// per call, because the layout cache is per instance while the advisory's memo is not: that is the
	// difference these cases have to be able to produce.
	void ResolveOnAFreshRegistry(const UClass* Class)
	{
		UCrowdyAutoRegistry* Registry = MakeStateRegistry();
		Registry->FindRepLayout(Class);
	}

	// Judges a class's layout without going through the registry, which is what the cases about the
	// advisory's CONTENT want: the fixtures they use are marked as fixtures, so the registry sweep skips
	// them on purpose.
	void ReportLayoutDirectly(const UClass* Class)
	{
		FCrowdyRepLayout Layout;
		if (FCrowdyStateLayoutBuilder::BuildLayout(Class, Layout))
		{
			CrowdyStateHeartbeatAdvisory::ReportLayout(Layout);
		}
	}
}

/**
 * The enum property outside the heartbeat is named, and every property with nothing wrong with it is not.
 *
 * Both halves are the case. Naming the property is the feature; staying quiet about the heartbeat-marked
 * one, the owner-only one (a keyframe never carries it, so the advice would be a remedy that does
 * nothing) and the non-enum one is what keeps the line worth reading.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatAdvisoryNamesThePropertyTest,
	"CrowdySDK.State.HeartbeatAdvisoryNamesTheProperty", CrowdyStateHeartbeatAdvisoryTestFlags)
bool FCrowdyStateHeartbeatAdvisoryNamesThePropertyTest::RunTest(const FString& Parameters)
{
	CrowdyStateHeartbeatAdvisory::ForgetAdvisedForTest();

	FCrowdyStateHeartbeatAdvisoryLogCapture Capture;
	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass());
	const TArray<FString> Advisories = Capture.TakeAdvisories();

	if (!TestEqual(TEXT("one line for the enum property and one for the byte-backed enum, and nothing for the other three"),
		Advisories.Num(), 2))
	{
		for (const FString& Advisory : Advisories)
		{
			AddInfo(Advisory);
		}
		return false;
	}

	TestEqual(TEXT("the enum-class property is named"), CountAdvisoryLinesContaining(Advisories, TEXT("'Stance'")), 1);
	TestEqual(TEXT("so is the TEnumAsByte property, which reflects as a byte and not as an enum property"),
		CountAdvisoryLinesContaining(Advisories, TEXT("'Mood'")), 1);
	TestEqual(TEXT("the property already on the heartbeat is not mentioned"),
		CountAdvisoryLinesContaining(Advisories, TEXT("'HeartbeatStance'")), 0);
	TestEqual(TEXT("nor is the owner-only one, which a keyframe never carries whatever it is marked"),
		CountAdvisoryLinesContaining(Advisories, TEXT("'OwnerOnlyStance'")), 0);
	TestEqual(TEXT("nor is the property that is not an enum at all"),
		CountAdvisoryLinesContaining(Advisories, TEXT("'Ticks'")), 0);

	const FString& Line = Advisories[0].Contains(TEXT("'Stance'")) ? Advisories[0] : Advisories[1];
	TestTrue(TEXT("the line names the class, which is the half a stale value on screen cannot tell you"),
		Line.Contains(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass()->GetName()));
	TestTrue(TEXT("it names the enum the property is declared as"),
		Line.Contains(TEXT("ECrowdyStateHeartbeatAdvisoryStance")));
	TestTrue(TEXT("it says what actually goes wrong rather than only that a marker is missing"),
		Line.Contains(TEXT("shows the wrong value")));
	TestTrue(TEXT("it names the C++ fix"), Line.Contains(TEXT("CrowdyHeartbeat")));
	TestTrue(TEXT("and the Blueprint one, since the property may not be C++ at all"),
		Line.Contains(TEXT("Keyframe heartbeat")));
	TestTrue(TEXT("it says the other two things that have to be true for a keyframe to go out, so the fix is not half a fix"),
		Line.Contains(TEXT("State Heartbeat must not be Off")));
	TestTrue(TEXT("it says the harmless reading out loud, so a deliberately transient enum is not a mystery"),
		Line.Contains(TEXT("meant to be transient")));
	TestTrue(TEXT("it says nothing is refused, because nothing is"),
		Line.Contains(TEXT("Advisory only")));
	TestTrue(TEXT("and it points at the command that lists the rest, since this line is said only once"),
		Line.Contains(TEXT("crowdy.state.heartbeat.advisories")));

	Capture.TakeAdvisories();
	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryCoveredTarget::StaticClass());
	TestEqual(TEXT("a class whose every property is covered says nothing at all"),
		Capture.TakeAdvisories().Num(), 0);

	return true;
}

/**
 * The line is said once per property, and every other property still speaks.
 *
 * The memo has to outlive the layout cache: that cache is reset wholesale on every world init, so a memo
 * riding on it would repeat the whole advisory on every level load. The second resolution here therefore
 * runs on a registry with an empty cache, which is exactly the shape a level load produces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatAdvisorySaidOncePerPropertyTest,
	"CrowdySDK.State.HeartbeatAdvisorySaidOncePerProperty", CrowdyStateHeartbeatAdvisoryTestFlags)
bool FCrowdyStateHeartbeatAdvisorySaidOncePerPropertyTest::RunTest(const FString& Parameters)
{
	CrowdyStateHeartbeatAdvisory::ForgetAdvisedForTest();

	FCrowdyStateHeartbeatAdvisoryLogCapture Capture;
	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass());
	if (!TestEqual(TEXT("the first resolution names both of the class's uncovered enum properties"),
		Capture.TakeAdvisories().Num(), 2))
	{
		return false;
	}

	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass());
	TestEqual(TEXT("resolving the same class again, with an empty layout cache, says nothing"),
		Capture.TakeAdvisories().Num(), 0);

	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryOtherTarget::StaticClass());
	const TArray<FString> OtherAdvisories = Capture.TakeAdvisories();
	if (!TestEqual(TEXT("a different property is a different subject and still speaks"), OtherAdvisories.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("and the line that speaks names the other class"),
		OtherAdvisories[0].Contains(UCrowdyStateHeartbeatAdvisoryOtherTarget::StaticClass()->GetName()));

	// What the memo silenced has to stay visible somewhere, or a reader who arrives after the warning
	// scrolled past is told a smaller number than the record holds.
	const FString Described = CrowdyStateHeartbeatAdvisory::DescribeAdvised();
	TestTrue(TEXT("the record still names the property the log has gone quiet about"),
		Described.Contains(TEXT("'Stance' on '")));
	TestTrue(TEXT("it says how many resolutions that one warning stands for"),
		Described.Contains(TEXT("resolved 2 time(s)")));
	TestTrue(TEXT("and it totals every property and every resolution, not only the lines that were printed"),
		Described.Contains(TEXT("3 propert(y/ies), 5 resolution(s)")));

	return true;
}

/**
 * An inherited property is advised once, on the class that declares it.
 *
 * A layout carries its super-class properties, so one UPROPERTY on a base class appears in every
 * subclass's layout. Keyed on the class the layout was built for, this is one line per subclass, every
 * one of them naming a class the author cannot edit the declaration on, and the number of lines grows
 * with the number of subclasses rather than with the number of mistakes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatAdvisoryNamesTheDeclaringClassTest,
	"CrowdySDK.State.HeartbeatAdvisoryNamesTheDeclaringClass", CrowdyStateHeartbeatAdvisoryTestFlags)
bool FCrowdyStateHeartbeatAdvisoryNamesTheDeclaringClassTest::RunTest(const FString& Parameters)
{
	CrowdyStateHeartbeatAdvisory::ForgetAdvisedForTest();

	FCrowdyStateHeartbeatAdvisoryLogCapture Capture;
	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryFirstDerivedTarget::StaticClass());
	const TArray<FString> Advisories = Capture.TakeAdvisories();

	if (!TestEqual(TEXT("resolving a subclass advises the inherited property once"), Advisories.Num(), 1))
	{
		for (const FString& Advisory : Advisories)
		{
			AddInfo(Advisory);
		}
		return false;
	}

	TestTrue(TEXT("and the line names the class the UPROPERTY is declared on"),
		Advisories[0].Contains(UCrowdyStateHeartbeatAdvisoryBaseTarget::StaticClass()->GetPathName()));
	TestFalse(TEXT("rather than the subclass, where there is nothing to edit"),
		Advisories[0].Contains(UCrowdyStateHeartbeatAdvisoryFirstDerivedTarget::StaticClass()->GetName()));

	// The second subclass is the half that fails quietly: keyed on the layout's class this speaks again,
	// saying the same thing about the same declaration.
	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisorySecondDerivedTarget::StaticClass());
	TestEqual(TEXT("a second subclass of the same base says nothing, because it is the same subject"),
		Capture.TakeAdvisories().Num(), 0);

	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryBaseTarget::StaticClass());
	TestEqual(TEXT("and neither does the base class itself"), Capture.TakeAdvisories().Num(), 0);

	const FString Described = CrowdyStateHeartbeatAdvisory::DescribeAdvised();
	TestTrue(TEXT("the record holds one subject for the three resolutions, not three"),
		Described.Contains(TEXT("1 propert(y/ies), 3 resolution(s)")));

	return true;
}

/**
 * A property that comes to carry the heartbeat leaves the record, so the remedy is confirmed by the
 * property disappearing from it rather than only by the log staying quiet, which it would do anyway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatAdvisoryClearsWhenFixedTest,
	"CrowdySDK.State.HeartbeatAdvisoryClearsWhenFixed", CrowdyStateHeartbeatAdvisoryTestFlags)
bool FCrowdyStateHeartbeatAdvisoryClearsWhenFixedTest::RunTest(const FString& Parameters)
{
	CrowdyStateHeartbeatAdvisory::ForgetAdvisedForTest();

	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("the fixture class has a CrowdyState layout"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateHeartbeatAdvisoryOtherTarget::StaticClass(), Layout)))
	{
		return false;
	}

	CrowdyStateHeartbeatAdvisory::ReportLayout(Layout);
	TestTrue(TEXT("the property is on the record to begin with"),
		CrowdyStateHeartbeatAdvisory::DescribeAdvised().Contains(TEXT("'Stance' on '")));

	for (FCrowdyRepProperty& RepProp : Layout.Properties)
	{
		RepProp.bHeartbeat = true;
	}

	CrowdyStateHeartbeatAdvisory::ReportLayout(Layout);
	TestFalse(TEXT("and it is gone once the property carries the heartbeat"),
		CrowdyStateHeartbeatAdvisory::DescribeAdvised().Contains(TEXT("'Stance' on '")));

	return true;
}

// The marker that keeps an SDK test fixture out of the startup sweep is read off the class itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateTestFixtureClassesAreRecognisedTest,
	"CrowdySDK.State.TestFixtureClassesAreRecognised", CrowdyStateHeartbeatAdvisoryTestFlags)
bool FCrowdyStateTestFixtureClassesAreRecognisedTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a class marked meta=(CrowdyTestFixture) is recognised as a fixture"),
		FCrowdyAttributeRegistry::IsTestFixture(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass()));
	TestFalse(TEXT("an ordinary class is not"),
		FCrowdyAttributeRegistry::IsTestFixture(UObject::StaticClass()));

	return true;
}

// The filter lives at the registry sweep, not inside the advisory, and both halves are the case: a fixture
// resolved the way a startup sweep resolves it says nothing, while the same class judged directly still
// says everything. Without the second half this would pass for a fixture that had nothing to advise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateHeartbeatAdvisorySkipsFixturesInTheSweepTest,
	"CrowdySDK.State.HeartbeatAdvisorySkipsTestFixturesInTheSweep", CrowdyStateHeartbeatAdvisoryTestFlags)
bool FCrowdyStateHeartbeatAdvisorySkipsFixturesInTheSweepTest::RunTest(const FString& Parameters)
{
	CrowdyStateHeartbeatAdvisory::ForgetAdvisedForTest();

	FCrowdyStateHeartbeatAdvisoryLogCapture Capture;

	ResolveOnAFreshRegistry(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass());
	TestEqual(TEXT("a fixture class resolved through the registry advises nothing"),
		Capture.TakeAdvisories().Num(), 0);

	ReportLayoutDirectly(UCrowdyStateHeartbeatAdvisoryTarget::StaticClass());
	TestEqual(TEXT("while the same class judged directly still advises, so the silence above is the filter and not an empty class"),
		Capture.TakeAdvisories().Num(), 2);

	return true;
}

#endif
