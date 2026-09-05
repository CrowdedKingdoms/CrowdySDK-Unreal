// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdySchemaSync.h"

// Turns a schema-sync plan/apply report into the words and counts the reconcile strip shows. Pure: no Slate,
// no HTTP, no UObject, no world, so it can be exercised without spinning up any of those.
namespace CrowdyReconcileSummary
{
	struct FCounts
	{
		int32 ToAdd = 0;      // every *ToCreate in the report
		int32 ToUpdate = 0;   // every *ToUpdate in the report
		int32 ServerOnly = 0; // report.ServerOnlyCount()
		bool bChecked = false; // report.bValid
		// The counts describe work already written, not work still pending. An apply keeps the counts it wrote so
		// the detailed report can say what it did, so without this flag the same numbers read as a to-do list.
		bool bWritten = false; // report.bApplied
	};

	inline FCounts BuildCounts(const FCrowdySchemaSyncReport& Report)
	{
		FCounts Counts;
		Counts.bChecked = Report.bValid;
		Counts.bWritten = Report.bApplied;
		Counts.ToAdd = Report.TypesToCreate + Report.PropsToCreate + Report.FunctionsToCreate
			+ Report.AutomationsToCreate + Report.TriggersToCreate;
		Counts.ToUpdate = Report.TypesToUpdate + Report.PropsToUpdate + Report.FunctionsToUpdate
			+ Report.AutomationsToUpdate + Report.TriggersToUpdate;
		Counts.ServerOnly = Report.ServerOnlyCount();
		return Counts;
	}

	// The one line the strip shows beside its buttons.
	inline FString BuildCountLine(const FCrowdySchemaSyncReport& Report)
	{
		const FCounts Counts = BuildCounts(Report);
		if (!Counts.bChecked)
		{
			return TEXT("Not checked yet.");
		}
		// A report that has been written still carries the counts it wrote, so reporting them in the pending tense
		// would advertise finished work as outstanding right next to a Schema indicator that has just gone green.
		if (Counts.bWritten)
		{
			return TEXT("Synced. Check again to confirm.");
		}
		if (Counts.ToAdd == 0 && Counts.ToUpdate == 0 && Counts.ServerOnly == 0)
		{
			return TEXT("Everything matches your project.");
		}

		TArray<FString> Parts;
		if (Counts.ToAdd != 0)
		{
			Parts.Add(FString::Printf(TEXT("%d to add"), Counts.ToAdd));
		}
		if (Counts.ToUpdate != 0)
		{
			Parts.Add(FString::Printf(TEXT("%d to change"), Counts.ToUpdate));
		}
		if (Counts.ServerOnly != 0)
		{
			Parts.Add(FString::Printf(TEXT("%d only on server"), Counts.ServerOnly));
		}
		return FString::Join(Parts, TEXT("  ·  "));
	}

	// Whether the Details disclosure should open itself. A failed plan or a partial apply is at least as
	// load-bearing as a warning: its banner is what tells the user what to do next, so StatusNote opens the
	// disclosure exactly like a warning does.
	inline bool ShouldOpenDetails(const FCrowdySchemaSyncReport& Report)
	{
		return Report.bValid && (Report.Warnings.Num() > 0 || !Report.StatusNote.IsEmpty());
	}
}
