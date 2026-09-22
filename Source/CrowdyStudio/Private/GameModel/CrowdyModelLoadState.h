// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * What a read of one family of model data is doing, and what an empty surface says while it is doing it.
 *
 * PURE and header-only: no Slate, no HTTP, no UObject, no world. Every function here is `inline` and this is
 * their only home, because the unity build merges this module's .cpp files into fewer translation units, so a
 * second definition of any of these in a .cpp would redefine this one. For the same reason there is no
 * LOCTEXT_NAMESPACE here; a caller that needs FText wraps with FText::FromString.
 *
 * THE RULE THIS FILE EXISTS FOR: an empty list is not an answer about the app unless the read that would have
 * filled it actually landed. A list nobody asked for, a list still being read, a list that came back empty and
 * a list whose read failed all look identical at the call site, and saying "this app has none" over the last
 * three is how a page reports a server error as a design fact. Every string below names which of the four it
 * is in, and no string claims a state the code cannot distinguish.
 *
 * These are FString rather than FText deliberately: the strings are compared byte for byte in tests, a
 * LOCTEXT_NAMESPACE in a header is the unity hazard above, and this is an editor tool with no shipped
 * localization.
 */
enum class ECrowdyModelLoadState : uint8
{
	// Nobody has asked. Not a failure, and not an answer about the app.
	NeverRequested,

	// A read is out. The only honest thing to show is that it is out.
	Loading,

	// The read landed. An empty list now means the app genuinely has none.
	Loaded,

	// The read came back an error, or never came back. An empty list here says nothing about the app.
	Failed
};

/**
 * Which read a state belongs to.
 *
 * Automations and their event triggers are TWO reads chained in one call, and they are two families here for
 * exactly that reason: folding them makes a failed trigger read indistinguishable from an app whose automations
 * have no triggers, which is the one state a reader can do nothing about because nothing tells them about it.
 */
enum class ECrowdyModelFamily : uint8
{
	Models,
	Attributes,
	Functions,
	Automations,
	AutomationTriggers,
	LiveModels,
	Count
};

/** One family's read state, pinned to the app and the request it belongs to. */
struct FCrowdyFamilyLoad
{
	ECrowdyModelLoadState State = ECrowdyModelLoadState::NeverRequested;

	// The app the outstanding or last-landed read was issued for.
	int64 AppId = 0;

	// The serial of the most recently ISSUED read. A reply carrying an older serial is discarded: it was
	// superseded, and an app switched away from and back to compares EQUAL on app id alone, which is the one
	// case the app pin cannot cover.
	uint64 Serial = 0;

	// Whether a reply may be acted on. All three terms are required: the app the read was issued for, the
	// request it was issued as, and the app the user is looking at now.
	bool Accepts(int64 ReplyAppId, uint64 ReplySerial, int64 SelectedAppId) const
	{
		return AppId == ReplyAppId && Serial == ReplySerial && SelectedAppId == ReplyAppId;
	}
};

/**
 * The empty-state strings. One function per surface, keyed by state, and the ONE place any of them is spelled.
 *
 * A surface that shows a filtered-to-nothing message passes its own query, because that message names what was
 * typed and no load state can produce it.
 */
namespace CrowdyModelEmptyState
{
	/**
	 * Which state one model's attributes are in, from the three things the caller knows about them.
	 *
	 * The order is load-bearing. A cached entry wins over a recorded failure, because a successful re-read must
	 * beat a stale failure from before it; a recorded failure wins over a read being in flight, because the
	 * failure is what the last completed read said; and only then is an outstanding read reported as loading.
	 * Reading the failure first is how a model that has just been re-read successfully keeps showing an error.
	 */
	inline ECrowdyModelLoadState AttributeLoadState(bool bCached, bool bFailed, bool bReadInFlight)
	{
		if (bCached)
		{
			return ECrowdyModelLoadState::Loaded;
		}
		if (bFailed)
		{
			return ECrowdyModelLoadState::Failed;
		}
		if (bReadInFlight)
		{
			return ECrowdyModelLoadState::Loading;
		}
		return ECrowdyModelLoadState::NeverRequested;
	}

	/** The left-hand model list on the Models tab. */
	inline FString ModelRail(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this app's models...");
		case ECrowdyModelLoadState::Loaded:
			return TEXT("This app has no models yet.\nDeclare one in code and press Sync to Server.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This app's models could not be read. The server returned an error or did not respond.")
				TEXT("\nPress Refresh to try again.");
		default:
			return TEXT("Nothing has been read for this app yet.\nPress Refresh to read its models.");
		}
	}

	inline FString ModelRailFilteredBySource()
	{
		return TEXT("No model comes from this source.\nChoose All to see the rest.");
	}

	inline FString ModelRailFilteredBySearch(const FString& Query)
	{
		return FString::Printf(TEXT("No model matches \"%s\"."), *Query);
	}

	/**
	 * One model's attributes. The failure remedy is re-selecting the model rather than Refresh, because
	 * attributes are read per model when it is selected and that is the press that issues the read again.
	 */
	inline FString AttributeTable(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this model's attributes...");
		case ECrowdyModelLoadState::Loaded:
			return TEXT("This model has no attributes yet.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This model's attributes could not be read.\nSelect the model again to try.");
		default:
			return TEXT("This model's attributes have not been read yet.");
		}
	}

	// What the attributes section says for the app itself, which owns no attributes. A "not applicable"
	// answer rather than a load state, so it takes none.
	inline FString AttributeTableAppWide()
	{
		return TEXT("Attributes belong to a model. What is listed here belongs to the app itself.");
	}

	inline FString FunctionTable(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this app's functions...");
		case ECrowdyModelLoadState::Loaded:
			return TEXT("This model has no functions.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This app's functions could not be read.\nPress Refresh to try again.");
		default:
			return TEXT("This app's functions have not been read yet.");
		}
	}

	/**
	 * The functions section for the app entry, which belongs to no model and lists what the app owns directly.
	 *
	 * Only the landed-read answer differs from a model's. The other three describe the read itself, which is the
	 * same app-wide read either way, so restating them here would be two copies of one sentence that can drift.
	 * Naming a model on this pane is the defect: its own subtitle says it is attached to none.
	 */
	inline FString FunctionTableAppWide(ECrowdyModelLoadState State)
	{
		if (State == ECrowdyModelLoadState::Loaded)
		{
			return TEXT("This app has no functions outside a model.");
		}
		return FunctionTable(State);
	}

	inline FString AutomationTable(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this app's automations...");
		case ECrowdyModelLoadState::Loaded:
			return TEXT("This model has no automations.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This app's automations could not be read.\nPress Refresh to try again.");
		default:
			return TEXT("This app's automations have not been read yet.");
		}
	}

	/** The automations section for the app entry. Same rule as the functions one: only the landed read differs. */
	inline FString AutomationTableAppWide(ECrowdyModelLoadState State)
	{
		if (State == ECrowdyModelLoadState::Loaded)
		{
			return TEXT("This app has no automations outside a model.");
		}
		return AutomationTable(State);
	}

	/**
	 * The line shown under the section strip when the automations arrived and their triggers did not.
	 *
	 * It is a line and not a placeholder because the automations themselves rendered, so the table has rows and
	 * shows no placeholder at all. Empty for every other combination: a trigger read that failed while the
	 * automations also failed is already said by the automations table, and there is nothing to add to a read
	 * that has not finished.
	 */
	inline FString AutomationTriggerNote(
		ECrowdyModelLoadState AutomationsState, ECrowdyModelLoadState TriggersState)
	{
		if (AutomationsState == ECrowdyModelLoadState::Loaded && TriggersState == ECrowdyModelLoadState::Failed)
		{
			return TEXT("Automations are shown, but their triggers could not be read, so when each one runs may be missing.");
		}
		return FString();
	}

	// The Live tab's own model list. The same read behind the Models rail, so the two answer with the same
	// words rather than prescribing two different remedies for one empty list.
	inline FString LiveModelRail(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this app's models...");
		case ECrowdyModelLoadState::Loaded:
			return TEXT("This app has no models yet.\nDeclare one in code and press Sync to Server.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This app's models could not be read.\nPress Refresh to try again.");
		default:
			return TEXT("Nothing has been read for this app yet.\nPress Refresh to read its models.");
		}
	}

	inline FString LiveInstanceTable(ECrowdyModelLoadState State)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this model's live models...");
		case ECrowdyModelLoadState::Loaded:
			return TEXT("The server is holding no live models of this type.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This model's live models could not be read.\nPress Refresh to try again.");
		default:
			return TEXT("Press Refresh to read this app's live models.");
		}
	}

	inline FString LiveInstanceTableFiltered()
	{
		return TEXT("No live model matches the filters.");
	}

	/**
	 * One live model's stored values. A live model is read when it is selected, so the remedy for a failure is
	 * selecting it again rather than a Refresh, which re-reads the list instead.
	 *
	 * bAttributesKnown is whether the model's declared attributes have been read. Without them, a landed read that
	 * found no values cannot go on to say the model declares none: that is a claim about a schema nobody has
	 * looked at, and it is wrong exactly when the reader most needs it to be right.
	 */
	inline FString PropertyTable(ECrowdyModelLoadState State, bool bAttributesKnown)
	{
		switch (State)
		{
		case ECrowdyModelLoadState::Loading:
			return TEXT("Reading this live model's values...");
		case ECrowdyModelLoadState::Loaded:
			return bAttributesKnown
				? TEXT("This live model holds no values, and its model declares no attributes.")
				: TEXT("This live model holds no values you can see.");
		case ECrowdyModelLoadState::Failed:
			return TEXT("This live model's values could not be read.\nSelect it again to try.");
		default:
			return TEXT("Select a live model above to see the values it holds.");
		}
	}

	// The values arrived in a shape this editor cannot parse, or nested deeper than its guard allows. Neither an
	// empty instance nor a failed read: the server answered, and what it sent cannot be laid out as rows.
	inline FString PropertyTableUnreadable()
	{
		return TEXT("The server sent values this editor could not read.\nCopy values copies them as they arrived.");
	}

	// The read landed and the server named no such live model. Not a failure and not an empty instance: the row
	// the reader clicked describes something the server will no longer answer for.
	inline FString PropertyTableMissing()
	{
		return TEXT("The server returned nothing for this live model.\nIt may have been deleted. Press Refresh.");
	}

	inline FString PropertyTableFilteredBySearch(const FString& Query)
	{
		return FString::Printf(TEXT("No attribute matches \"%s\"."), *Query);
	}
}
