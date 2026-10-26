#pragma once

#include "CoreMinimal.h"

class UCrowdyServerObjectDefinition;
struct FCrowdyServerFunction;

/** The Game Kit starters the Crowdy SDK ships, and Add starter, which makes one an ordinary Server Object type of the project. */
namespace CrowdyGameKitStarters
{
	struct FStarter
	{
		/** The node type, which is also the Type Name of the definition Add starter makes. */
		const TCHAR* TypeName = nullptr;
		const TCHAR* AssetName = nullptr;
		const TCHAR* Description = nullptr;
		/** The starter it runs under; empty for the app's root. */
		const TCHAR* Parent = TEXT("");
		/** The starters its server code calls. */
		TArray<const TCHAR*> Calls;
		/** The methods its server code refuses to anyone but the app's developers. */
		TArray<const TCHAR*> DeveloperMethods;
		/** The starters its server code calls only when its Config says so: in its Calls, but Add starter does not bring them and removing them leaves it. */
		TArray<const TCHAR*> OptionalCalls;
		/** Its server code mixes a Config secret into what it shuffles or rolls, so Add starter given no Config writes a fresh one: players can see everything else it seeds from. */
		bool bMakesSecret = false;
	};

	/** Where Add starter saves the definitions it makes. */
	inline const TCHAR* const ContentPath = TEXT("/Game/ServerObjects/GameKit");

	/** world, matchmaker, session, leaderboard, player, market, npcs, minigame, tactics, director, instance, territory, zone, deck, arena, race and ball. */
	TConstArrayView<FStarter> GetStarters();

	const FStarter* FindStarter(const FString& TypeName);

	/** The Config Add starter saves: Config trimmed, or for a starter that makes a secret and was given none, a fresh random one. */
	FString ConfigToAdd(const FStarter& Starter, const FString& Config);

	/** What in Config, the JSON text of the starter TypeName's Config object, its server code would refuse to start on: a key that is not a whole number within its range, or a secret that is not text. Keys match with case, as the server code reads them. Empty when nothing is wrong or the starter checks nothing. */
	FString FindConfigProblem(const FString& TypeName, const FString& Config);

	/** Whether Config, a starter's Config text, has a secret its server code reads: non-empty text under exactly the key "secret". */
	bool HasConfigSecret(const FString& Config);

	/** Whether Definition is a starter whose server code takes Function from the app's developers only, whatever Callable By its asset was saved with. */
	bool IsDeveloperOnly(const UCrowdyServerObjectDefinition& Definition, const FCrowdyServerFunction& Function);

	/** A new definition of the starter, named Name in Outer and baked; null for a name the SDK has no starter for. */
	UCrowdyServerObjectDefinition* MakeDefinition(const FStarter& Starter, UObject* Outer, FName Name, EObjectFlags Flags);

	/** The Crowdy SDK's copy of the starter's crate: Cargo.toml and src/lib.rs, its package named after its type. */
	FString GetCrateSource(const FStarter& Starter);

	/** The starters adding TypeName brings, in the order to add them: its parents up to the root first, then it, then the types it calls; those the project has are left out. Empty, with OutProblem, when one cannot be added. */
	TArray<const FStarter*> PlanAdd(const FString& TypeName, TConstArrayView<const UCrowdyServerObjectDefinition*> Project, FString& OutProblem);

	/** Adds the starters PlanAdd names: each crate copied to ServerDirectory/<Type Name>, each definition saved under ContentPath, and the Game Kit World setting pointed at world when it is added. Config, a JSON object, becomes TypeName's own Config. Nothing is written when a folder or asset it needs is taken. */
	bool AddStarter(const FString& TypeName, TConstArrayView<const UCrowdyServerObjectDefinition*> Project, const FString& ServerDirectory,
		TArray<FString>& OutAdded, FString& OutError, const FString& Config = FString());

	/** The project's starter definitions removing TypeName takes, in the order to remove them: those that run under it or call it (other than by one of their OptionalCalls) first, then it. Empty, with OutProblem, when the project has no such starter. */
	TArray<UCrowdyServerObjectDefinition*> PlanRemove(const FString& TypeName, TConstArrayView<UCrowdyServerObjectDefinition*> Project, FString& OutProblem);

	/** Removes the starters PlanRemove names: each definition deleted, its folder in ServerDirectory deleted with any edits to its code, and the Game Kit World setting cleared when it named one. */
	bool RemoveStarter(const FString& TypeName, TConstArrayView<UCrowdyServerObjectDefinition*> Project, const FString& ServerDirectory,
		TArray<FString>& OutRemoved, FString& OutError);
}
