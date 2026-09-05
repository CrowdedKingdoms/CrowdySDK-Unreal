// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "SourceCodeNavigation.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SoftObjectPath.h"

// Opening the thing in the project that declares a row. A model, an attribute, a function or an automation that came
// from code carries the path of whatever declares it: a container class for the first two, the authoring effect asset
// for the other two. Two shapes of path arrive here and they are opened in different places.
//
//   /Game/Effects/E_Heal.E_Heal        an asset; open its asset editor
//   /Game/Models/BP_Hero.BP_Hero_C     a Blueprint class; open the Blueprint asset the class was generated from
//   /Script/CrowdedKingdoms.AHero      a native class; there is no asset, so go to the header that declares it
//
// Whether a path leads anywhere is answered separately from following it, so a control with nothing to open can be
// shown disabled rather than doing nothing when it is clicked.
//
// Every function here is inline and this is their only home, because the unity build merges this module's .cpp files
// into fewer translation units and a second definition of any of these would redefine this one.
namespace CrowdyModelCodeLink
{
	enum class ETargetKind : uint8
	{
		None,
		Asset,
		NativeClass
	};

	struct FTarget
	{
		ETargetKind Kind = ETargetKind::None;
		FSoftObjectPath AssetPath;
		const UClass* NativeClass = nullptr;
	};

	// Whether a path names a class compiled into a module rather than an object saved in a package. Those are the
	// only two shapes that reach here, so this decides which half of the work applies.
	inline bool IsNativeClassPath(const FString& CodePath)
	{
		return CodePath.TrimStartAndEnd().StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive);
	}

	// The object path an ASSET would be at, or empty when the path names no asset (it is blank, or it names a native
	// class). A Blueprint class path ends in the generated class's "_C" and the asset is the same path without it;
	// an asset path is already the answer.
	//
	// The suffix is matched case-sensitively, because FString::EndsWith folds case by default and a package
	// genuinely ending in "_c" is a different asset: chopping that would ask for a name nothing is saved under, and
	// the control would go quiet with no way to tell that from an asset that had been deleted.
	inline FString AssetObjectPath(const FString& CodePath)
	{
		const FString Trimmed = CodePath.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || IsNativeClassPath(Trimmed))
		{
			return FString();
		}

		FString ObjectPath = Trimmed;
		if (ObjectPath.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
		{
			ObjectPath.LeftChopInline(2);
		}
		return ObjectPath;
	}

	// Work out what a path leads to WITHOUT loading anything. A native class must already be in memory to be one at
	// all, and an asset is answered from the asset registry, so this is cheap enough to ask about a selection.
	inline FTarget Resolve(const FString& CodePath)
	{
		FTarget Target;

		const FString Trimmed = CodePath.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return Target;
		}

		if (IsNativeClassPath(Trimmed))
		{
			// A native class lives in a compiled module rather than in a package, so nothing can be opened for it.
			// Its declaration can be, but only where the engine can find the source: a class from a module shipped
			// without sources answers no here, and the control that offered it goes quiet.
			const UClass* Class = FindObject<UClass>(nullptr, *Trimmed);
			if (Class != nullptr && FSourceCodeNavigation::CanNavigateToClass(Class))
			{
				Target.Kind = ETargetKind::NativeClass;
				Target.NativeClass = Class;
			}
			return Target;
		}

		const FSoftObjectPath Candidate(AssetObjectPath(Trimmed));
		if (Candidate.IsNull())
		{
			return Target;
		}

		// Already in memory is already an answer. Otherwise the registry knows what is on disk without reading it.
		if (Candidate.ResolveObject() == nullptr)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			if (!AssetRegistryModule.Get().GetAssetByObjectPath(Candidate).IsValid())
			{
				// Renamed, deleted, or never saved since the plan named it.
				return Target;
			}
		}

		Target.Kind = ETargetKind::Asset;
		Target.AssetPath = Candidate;
		return Target;
	}

	inline bool CanOpen(const FString& CodePath)
	{
		return Resolve(CodePath).Kind != ETargetKind::None;
	}

	// Follow the path. Resolved again here rather than from a remembered target: what a path leads to can change
	// between the moment a control was drawn and the moment it was clicked.
	inline bool Open(const FString& CodePath)
	{
		const FTarget Target = Resolve(CodePath);

		if (Target.Kind == ETargetKind::NativeClass)
		{
			return FSourceCodeNavigation::NavigateToClass(Target.NativeClass);
		}

		if (Target.Kind == ETargetKind::Asset)
		{
			// The one load this page ever performs, and only in answer to a click asking for exactly this asset.
			UObject* Asset = Target.AssetPath.TryLoad();
			if (Asset == nullptr || GEditor == nullptr)
			{
				return false;
			}

			UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			return AssetEditors != nullptr && AssetEditors->OpenEditorForAsset(Asset);
		}

		return false;
	}
}
