#include "Compiler/CrowdyAuthoringContributions.h"

#include "Data/CrowdyMapProfile.h"
#include "Engine/World.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyAuthoringContributions
{
	namespace
	{
		TFunction<FString(const UFunction*)> GActionEventValidator;
		TFunction<TArray<FCrowdyActionParameterSpec>()> GActionParameterProvider;
	}

	bool IsCrowdRepresentationSelected()
	{
		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		if (!Settings) return false;

		// Loaded rather than skipped when not already in memory: a profile asset nothing has opened yet is
		// the ordinary state of a freshly started editor, and answering "no crowd" for it would hide these
		// surfaces from exactly the project that needs them. Profiles are small data assets and the object
		// system holds them after the first ask.
		for (const TPair<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>>& Entry : Settings->MapProfiles)
		{
			// An entry naming no map is skipped when a world resolves its profile, so no map can be drawn
			// through it and it does not count here either.
			if (Entry.Key.IsNull()) continue;

			if (UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(Entry.Value.LoadSynchronous())) return true;
		}

		// A map with no entry of its own falls back to Default Profile, and only a project that has not set
		// one can ever reach the profile a plugin ships. Counting the shipped profile regardless would keep
		// these surfaces on for a project that has deliberately chosen another backend for every map it can
		// open, which is the case this whole query exists to answer.
		if (!Settings->DefaultProfile.IsNull())
		{
			return UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(Settings->DefaultProfile.LoadSynchronous());
		}

		// The reason a shipped profile could not be honoured is dropped here on purpose. This is a question
		// about which authoring options to show, not a diagnostic, and the same resolve reports the reason
		// once per world where a map is actually starting up and something is actually broken.
		FString UnavailableReason;

		return UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(UCrowdySDKDeveloperSettings::ResolveShippedDefaultProfile(UnavailableReason));
	}

	TFunction<FString(const UFunction*)> SetActionEventValidator(TFunction<FString(const UFunction*)> Validator)
	{
		TFunction<FString(const UFunction*)> Displaced = MoveTemp(GActionEventValidator);
		GActionEventValidator = MoveTemp(Validator);
		return Displaced;
	}

	FString DescribeActionEventProblem(const UFunction* Function)
	{
		if (!Function || !GActionEventValidator) return FString();

		return GActionEventValidator(Function);
	}

	TFunction<TArray<FCrowdyActionParameterSpec>()> SetActionParameterProvider(
		TFunction<TArray<FCrowdyActionParameterSpec>()> Provider)
	{
		TFunction<TArray<FCrowdyActionParameterSpec>()> Displaced = MoveTemp(GActionParameterProvider);
		GActionParameterProvider = MoveTemp(Provider);
		return Displaced;
	}

	TArray<FCrowdyActionParameterSpec> GetActionParameters()
	{
		if (!GActionParameterProvider) return TArray<FCrowdyActionParameterSpec>();

		return GActionParameterProvider();
	}
}
