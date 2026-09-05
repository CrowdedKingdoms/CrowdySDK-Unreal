// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectCompileSummary.h"

#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectGraphCompileHook.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectParser.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpecBuilder.h"

namespace CrowdyEffectCompileSummary
{
	TArray<FString> MissingMagnitudes(const UCrowdyEffect* Effect)
	{
		if (!Effect)
		{
			return {};
		}

		FCrowdyEffectProgram Program;
		TArray<FCrowdyEffectDiagnostic> Ignored;
		if (Effect->Source == ECrowdyEffectSource::Graph)
		{
			const FCrowdyEffectSpec GraphSpec = CrowdyEffectGraphCompile::CompileGraphToSpec(Effect->EffectGraph, Ignored);
			Program = FCrowdyEffectSpecBuilder::BuildProgram(GraphSpec, Ignored);
		}
		else
		{
			Program = FCrowdyEffectParser::Parse(Effect->EffectScript).Program;
		}

		TArray<FString> Declared;
		Declared.Reserve(Effect->Magnitudes.Num());
		for (const FCrowdyEffectMagnitude& Magnitude : Effect->Magnitudes)
		{
			Declared.Add(Magnitude.Name);
		}
		return FCrowdyEffectLowering::CollectUndeclaredParams(Program, Declared);
	}

	FString BuildSummary(const UCrowdyEffect* Effect)
	{
		if (!Effect)
		{
			return FString();
		}

		const FCrowdyEffectLoweringResult Result = Effect->Compile();

		FString Text = FString::Printf(TEXT("Function: %s"), *Effect->GetEffectiveFunctionName());
		Text += FString::Printf(TEXT("\nRequires a Source object: %s"), Result.bSourceReferenced ? TEXT("yes") : TEXT("no"));

		const TArray<FString> Missing = MissingMagnitudes(Effect);
		if (Missing.Num() > 0)
		{
			Text += FString::Printf(TEXT("\nUndeclared tuning parameters: %s (use Missing Parameters to declare them)"),
				*FString::Join(Missing, TEXT(", ")));
		}

		if (Result.Diagnostics.IsEmpty())
		{
			Text += TEXT("\nNo diagnostics.");
		}
		else
		{
			for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
			{
				Text += TEXT("\n") + Diagnostic.ToString();
			}
		}
		return Text;
	}
}
