// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/CrowdyEffectGraphNodeOptions.h"

#include "Customizations/CrowdyEffectPickerOptions.h"
#include "EdGraph/EdGraphNode.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

namespace CrowdyEffectGraphNodeOptions
{
	const UCrowdyEffect* OwningEffect(const UEdGraphNode* Node)
	{
		return Node ? Node->GetTypedOuter<UCrowdyEffect>() : nullptr;
	}

	TArray<FString> AttributeOptions(const UCrowdyEffect* Effect, ECrowdyEffectRole Role)
	{
		TArray<FString> Keys;
		if (!Effect)
		{
			return Keys;
		}

		const UClass* Class = nullptr;
		if (Role == ECrowdyEffectRole::Source && !Effect->SourceContainerType.TrimStartAndEnd().IsEmpty())
		{
			// A declared Source Container Type means the source is a different kind of container from the target, so
			// its attributes are not the target's. Resolve through the same lookup Compile() uses; a name that does
			// not resolve returns nothing rather than falling back to the target's class, since offering the wrong
			// container's keys is the defect being fixed here.
			Class = Effect->ResolveSourceContainerClass();
			if (!Class)
			{
				return Keys;
			}
		}
		else
		{
			// Target, or a Source with no declared type of its own, which means the source shares the target's schema.
			Class = Effect->ContainerClass.LoadSynchronous();
		}

		// The graph emits attributes as their server key (self.<key>), the same string the schema sync creates on the
		// server, so the picker offers keys and not the raw C++ property name. Def.Key is always derived, but fall back
		// to a lowercased property name defensively.
		for (const FCrowdyAttributeDef& Def : CrowdyEffectPickerOptions::AttributesForClass(Class))
		{
			const FString Key = Def.Key.IsEmpty() ? Def.PropertyName.ToString().ToLower() : Def.Key;
			Keys.AddUnique(Key);
		}
		Keys.Sort([](const FString& A, const FString& B) { return A < B; });
		return Keys;
	}

	TArray<FString> MagnitudeOptions(const UCrowdyEffect* Effect)
	{
		TArray<FString> Names;
		if (Effect)
		{
			for (const FCrowdyEffectMagnitude& Magnitude : Effect->Magnitudes)
			{
				const FString Name = Magnitude.Name.TrimStartAndEnd();
				if (!Name.IsEmpty())
				{
					Names.AddUnique(Name);
				}
			}
		}
		return Names;
	}

	const TArray<FCrowdyEffectBuiltinCall>& BuiltinCalls()
	{
		// The documented effect-language builtins a Call node can invoke, alphabetized by callee. Intentionally broader
		// than the text editor's syntax-highlight subset (CrowdyExpressionEditorModel::BuiltinNames, which classifies
		// only a handful). The ternary is offered by the dedicated Select node, not here. These are functions the
		// language itself provides; a function authored on the effect is the separate fn: server call.
		//
		// The variadic ones (array, coalesce, concat, max, min) are the only ones whose argument count an author sets;
		// every other builtin has one legal arity, so its node offers no count at all.
		//
		// The list family (append, array, at, index_of, remove_at, set_at) is pure: each returns a new list (or item,
		// or index) and never modifies the list passed in. A property that was never written reads as nothing, and
		// every one of these treats that as the empty list, EXCEPT remove_at: removing index 0 of nothing has no
		// answer, so remove_at on an empty list is an out-of-range error like any other bad index. at is the mirror
		// case: on an unset or empty list it returns null rather than erroring, the same way index_of answers -1
		// instead of failing. An out-of-range index on a non-empty list is still an error for both at and remove_at,
		// rolling back the whole invocation.
		//
		// The idiom shapes documented on Append and Remove At are not what makes a write correct: any conditional
		// write that reads the property it changes is already protected by a lock, so a plain if() guard is just as
		// correct, only serialized behind that lock rather than lock-free. The reason to reach for the idiom shapes
		// is contention on a hot container, and (for Append) exactly-once semantics, not correctness.
		static const TArray<FCrowdyEffectBuiltinCall> Calls = {
			{ TEXT("abs"), TEXT("Abs"), 1, 1,
				TEXT("The value without its sign, so -7 becomes 7."),
				{ TEXT("Value") } },
			{ TEXT("append"), TEXT("Append"), 2, 2,
				TEXT("List with Value added at the end, returned as a new list; List itself is unchanged.\n\n"
					"A property that was never written reads as the empty list, so appending onto a brand-new list "
					"attribute stores a one-item list rather than failing. Lists are capped at 1000 items: appending "
					"past the cap is a rejected call, even when the result is never stored.\n\n"
					"A bare append is not exactly-once: it will happily add the same value twice if the call arrives "
					"twice, for example a player whose enter fires again after a connection flap. Guard it when "
					"'already there?' matters: p = if(index_of(p, X) < 0, append(p, X), p), or the other ordering, "
					"p = if(index_of(p, X) >= 0, p, append(p, X)). Any of < 0, == -1, or <= -1 tests absence, and any "
					"of >= 0, != -1, or > -1 tests presence; the server treats them the same. This shape needs no "
					"lock: two callers racing both recompute against the value the winner just committed. The reason "
					"to reach for it is exactly-once behavior and, on a hot container, contention: any other guard "
					"that reads the list before writing it is correct too, just serialized behind a short lock."),
				{ TEXT("List"), TEXT("Value") } },
			{ TEXT("array"), TEXT("Array"), 0, 16,
				TEXT("A new list built from its arguments, in order; array() with no arguments is the empty list.\n\n"
					"Lists are capped at 1000 items, enforced the same way as Append."),
				{} },
			{ TEXT("at"), TEXT("At"), 2, 2,
				TEXT("The item at Index in List.\n\n"
					"A property that was never written reads as the empty list, so At on an unset or empty List "
					"returns nothing rather than failing, the same way Index Of returns -1. An out-of-range Index on "
					"a non-empty List is an error that rolls back the whole invocation, so bound-check with Length "
					"before indexing a position you are not already sure of."),
				{ TEXT("List"), TEXT("Index") } },
			{ TEXT("ceil"), TEXT("Ceil"), 1, 1,
				TEXT("Rounds up to the next whole number, so 2.1 becomes 3."),
				{ TEXT("Value") } },
			{ TEXT("clamp"), TEXT("Clamp"), 3, 3,
				TEXT("Constrains a value so it never falls below a minimum or rises above a maximum.\n\n"
					"An attribute with clamp bounds is already clamped on write, so reach for this when you need to bound "
					"an intermediate result instead."),
				{ TEXT("Value"), TEXT("Min"), TEXT("Max") } },
			{ TEXT("coalesce"), TEXT("Coalesce"), 2, 16,
				TEXT("The first argument that is not null.\n\n"
					"Use it to give an unset attribute a safe default before doing maths on it, since arithmetic on null "
					"fails the whole invocation."),
				{ TEXT("Value"), TEXT("Fallback") } },
			{ TEXT("concat"), TEXT("Concat"), 2, 16,
				TEXT("Joins its arguments into one string."),
				{ TEXT("A"), TEXT("B") } },
			{ TEXT("floor"), TEXT("Floor"), 1, 1,
				TEXT("Rounds down to the previous whole number, so 2.9 becomes 2."),
				{ TEXT("Value") } },
			{ TEXT("index_of"), TEXT("Index Of"), 2, 2,
				TEXT("The index of the first item in List equal to Value, or -1 when it is not present.\n\n"
					"Compares exactly as == does, so index_of(list, x) >= 0 and an == test over the same values can "
					"never disagree. A property that was never written reads as the empty list, so Index Of on an "
					"unset attribute is always -1."),
				{ TEXT("List"), TEXT("Value") } },
			{ TEXT("is_null"), TEXT("Is Null"), 1, 1,
				TEXT("True when the value is null.\n\nUse it in a condition to branch on an attribute that was never set."),
				{ TEXT("Value") } },
			{ TEXT("len"), TEXT("Length"), 1, 1,
				TEXT("The length of a string or list."),
				{ TEXT("Value") } },
			{ TEXT("max"), TEXT("Max"), 2, 16,
				TEXT("The largest of its arguments."),
				{ TEXT("A"), TEXT("B") } },
			{ TEXT("min"), TEXT("Min"), 2, 16,
				TEXT("The smallest of its arguments."),
				{ TEXT("A"), TEXT("B") } },
			{ TEXT("not"), TEXT("Not"), 1, 1,
				TEXT("Logical negation, as a function call.\n\nThe Not operator node does the same thing and reads better "
					"in a graph; this form exists for parity with the text language."),
				{ TEXT("Condition") } },
			{ TEXT("pow"), TEXT("Pow"), 2, 2,
				TEXT("Raises the base to a power, so Pow(2, 10) is 1024."),
				{ TEXT("Base"), TEXT("Exponent") } },
			{ TEXT("rand"), TEXT("Rand"), 0, 0,
				TEXT("A random number between 0 and 1.\n\nEvaluated on the server, so every client sees the same result "
					"for one invocation."),
				{} },
			{ TEXT("rand_int"), TEXT("Rand Int"), 2, 2,
				TEXT("A random whole number between a minimum and a maximum.\n\nEvaluated on the server, so every client "
					"sees the same result for one invocation."),
				{ TEXT("Min"), TEXT("Max") } },
			{ TEXT("remove_at"), TEXT("Remove At"), 2, 2,
				TEXT("List with the item at Index removed, returned as a new list; List itself is unchanged.\n\n"
					"An out-of-range Index is an error that rolls back the whole invocation, and that includes index "
					"0 of an empty list: unlike the other list builtins, an unset List has no index to remove.\n\n"
					"Guard it for an idempotent remove: p = if(index_of(p, X) >= 0, remove_at(p, index_of(p, X)), p), "
					"or the other ordering, p = if(index_of(p, X) < 0, p, remove_at(p, index_of(p, X))). Any of "
					">= 0, != -1, or > -1 tests presence, and any of < 0, == -1, or <= -1 tests absence; the server "
					"treats them the same. This shape needs no lock: two callers racing both recompute against the "
					"value the winner just committed, and removing something already gone does nothing rather than "
					"erroring. The reason to reach for it is idempotence and, on a hot container, contention: any "
					"other guard that reads the list before writing it is correct too, just serialized behind a "
					"short lock."),
				{ TEXT("List"), TEXT("Index") } },
			{ TEXT("round"), TEXT("Round"), 1, 1,
				TEXT("Rounds to the nearest whole number."),
				{ TEXT("Value") } },
			{ TEXT("set_at"), TEXT("Set At"), 3, 3,
				TEXT("List with the item at Index replaced by Value, returned as a new list; List itself is "
					"unchanged.\n\nAn out-of-range Index is an error that rolls back the whole invocation.\n\n"
					"Lists are capped at 1000 items, enforced the same way as Append: replacing an item does not "
					"change List's length, but the cap is still checked."),
				{ TEXT("List"), TEXT("Index"), TEXT("Value") } },
			{ TEXT("sqrt"), TEXT("Sqrt"), 1, 1,
				TEXT("Square root."),
				{ TEXT("Value") } },
			{ TEXT("to_float"), TEXT("To Float"), 1, 1,
				TEXT("Converts a value to a float."),
				{ TEXT("Value") } },
			{ TEXT("to_int"), TEXT("To Int"), 1, 1,
				TEXT("Converts a value to a whole number."),
				{ TEXT("Value") } },
			{ TEXT("to_string"), TEXT("To String"), 1, 1,
				TEXT("Converts a value to a string.\n\nUseful before Concat, since joining a number without converting it "
					"is rejected."),
				{ TEXT("Value") } },

			// The database-backed grid and permission reads. Each carries a "use it when" line, because unlike the pure
			// maths these are only meaningful in a world that uses grids, and their cost is not free: each uncached
			// lookup is metered on the server.
			//
			// Chunk coordinates are CHUNK indices, not world positions. Getting that wrong is the likeliest mistake with
			// this family, so every chunk argument says so in its own description.
			{ TEXT("has_grid_permission"), TEXT("Has Grid Permission"), 2, 3,
				TEXT("True when a user holds an unexpired runtime permission on a grid.\n\n"
					"Use it when an action should depend on what a player is allowed to do in the world rather than on "
					"who owns the container: opening a claimed door, editing voxels inside a plot, teleporting into a "
					"region. Pass $caller_user_id for the player making the call.\n\n"
					"Leave Grid Id unset to ask whether they hold the permission on any grid at all.\n\n"
					"Reads the live permission table. Each uncached lookup is metered on the server."),
				{ TEXT("User Id"), TEXT("Permission Key"), TEXT("Grid Id") },
				TEXT("Grid & Permissions") },
			{ TEXT("has_chunk_permission"), TEXT("Has Chunk Permission"), 5, 6,
				TEXT("True when a user holds a permission on whichever grid covers a chunk. False when no grid covers it.\n\n"
					"Use it when the question is about a place rather than a grid: may this player act HERE. It is "
					"shorthand for looking up the grid at those coordinates and then testing the permission on it.\n\n"
					"Chunk X, Y and Z are chunk indices, not world coordinates. Overlap Mode decides which grid wins when "
					"several cover the chunk: \"first\" (default, matches how replication picks the enforcing grid), "
					"\"smallest\" (the innermost, natural for plots), or \"largest\".\n\n"
					"Reads the live permission table. Each uncached lookup is metered on the server."),
				{ TEXT("User Id"), TEXT("Permission Key"), TEXT("Chunk X"), TEXT("Chunk Y"), TEXT("Chunk Z"),
					TEXT("Overlap Mode") },
				TEXT("Grid & Permissions") },
			{ TEXT("grid_at"), TEXT("Grid At"), 3, 4,
				TEXT("The id of the grid covering a chunk, or null when none does.\n\n"
					"Use it when you need the grid itself rather than a yes or no answer, for example to store which plot "
					"something was built in, or to feed Grid Min and Grid Max.\n\n"
					"Chunk X, Y and Z are chunk indices, not world coordinates. Overlap Mode decides which grid wins when "
					"several cover the chunk: \"first\" (default), \"smallest\" (the innermost), or \"largest\".\n\n"
					"Guard the result with Is Null before using it. Metered per uncached lookup."),
				{ TEXT("Chunk X"), TEXT("Chunk Y"), TEXT("Chunk Z"), TEXT("Overlap Mode") },
				TEXT("Grid & Permissions") },
			{ TEXT("grid_contains"), TEXT("Grid Contains"), 4, 4,
				TEXT("True when a grid's box covers a chunk.\n\n"
					"Use it when you already know which grid you care about and want to test a position against it, "
					"rather than asking which grid covers that position.\n\n"
					"Chunk X, Y and Z are chunk indices, not world coordinates.\n\n"
					"Each uncached lookup is metered on the server."),
				{ TEXT("Grid Id"), TEXT("Chunk X"), TEXT("Chunk Y"), TEXT("Chunk Z") },
				TEXT("Grid & Permissions") },
			{ TEXT("grid_min"), TEXT("Grid Min"), 2, 2,
				TEXT("A grid's inclusive lower chunk bound on one axis.\n\n"
					"Use it with Grid Max to reason about a region's size or to place something relative to its edge.\n\n"
					"Axis is \"x\", \"y\", or \"z\". Each uncached lookup is metered on the server."),
				{ TEXT("Grid Id"), TEXT("Axis") },
				TEXT("Grid & Permissions") },
			{ TEXT("grid_max"), TEXT("Grid Max"), 2, 2,
				TEXT("A grid's inclusive upper chunk bound on one axis.\n\n"
					"Use it with Grid Min to reason about a region's size or to place something relative to its edge.\n\n"
					"Axis is \"x\", \"y\", or \"z\". Each uncached lookup is metered on the server."),
				{ TEXT("Grid Id"), TEXT("Axis") },
				TEXT("Grid & Permissions") }
		};
		return Calls;
	}

	const FCrowdyEffectBuiltinCall* FindBuiltin(const FString& Callee)
	{
		const FString Trimmed = Callee.TrimStartAndEnd();
		return BuiltinCalls().FindByPredicate([&Trimmed](const FCrowdyEffectBuiltinCall& Call)
		{
			return Call.Callee.Equals(Trimmed, ESearchCase::IgnoreCase);
		});
	}

	int32 ClampArgCount(const FString& Callee, int32 Requested)
	{
		const FCrowdyEffectBuiltinCall* Builtin = FindBuiltin(Callee);
		return Builtin ? FMath::Clamp(Requested, Builtin->MinArgs, Builtin->MaxArgs) : Requested;
	}

	FString ArgDisplayName(const FString& Callee, int32 ArgIndex)
	{
		const FCrowdyEffectBuiltinCall* Builtin = FindBuiltin(Callee);
		if (!Builtin || ArgIndex < 0)
		{
			return FString();
		}

		// A variadic builtin names only its first few arguments, so the rest read as numbered values rather than as
		// nothing at all.
		const FString Name = Builtin->ArgNames.IsValidIndex(ArgIndex)
			? Builtin->ArgNames[ArgIndex]
			: FString::Printf(TEXT("Value %d"), ArgIndex + 1);

		return (ArgIndex >= Builtin->MinArgs) ? Name + TEXT(" (optional)") : Name;
	}

	FString BuiltinDisplayName(const FString& Callee)
	{
		const FString Trimmed = Callee.TrimStartAndEnd();
		for (const FCrowdyEffectBuiltinCall& Call : BuiltinCalls())
		{
			if (Call.Callee.Equals(Trimmed, ESearchCase::IgnoreCase))
			{
				return Call.DisplayName;
			}
		}
		return Trimmed;
	}

	FString BoolLiteralDisplayName(const FString& Literal)
	{
		const FString Trimmed = Literal.TrimStartAndEnd();
		if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			return TEXT("True");
		}
		if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return TEXT("False");
		}
		return Trimmed;
	}

	TArray<FString> BuiltinCallOptions()
	{
		TArray<FString> Names;
		Names.Reserve(BuiltinCalls().Num());
		for (const FCrowdyEffectBuiltinCall& Call : BuiltinCalls())
		{
			Names.Add(Call.Callee);
		}
		return Names;
	}

	TArray<FString> FnCallOptions(const UCrowdyEffect* Effect)
	{
		TArray<FString> Names;
		if (!Effect)
		{
			return Names;
		}

		const FString ContainerTypeName = Effect->GetContainerTypeName();
		if (ContainerTypeName.IsEmpty())
		{
			// No container class means this effect declares no server function and can call no fn: callee that
			// shares its (nonexistent) type. Querying the catalog with an empty key would only turn up whatever
			// else got bucketed under "no container", which is unrelated to Effect and would be a wrong suggestion.
			return Names;
		}

		if (CrowdyEffectFunctionCatalog::IsCatalogAvailable())
		{
			const FString OwnFunctionName = Effect->GetEffectiveFunctionName();
			for (const CrowdyEffectFunctionCatalog::FDeclaredFunction& Declared :
				CrowdyEffectFunctionCatalog::FindFunctionsOnContainerType(ContainerTypeName))
			{
				// bAuthorsReturn, and not a non-empty ReturnType, is what makes a function a valid fn: callee: an
				// effect that returns a bare attribute legitimately declares no type.
				if (!Declared.bAuthorsReturn)
				{
					continue;
				}
				// Defensive re-check even though the catalog is already scoped by ContainerTypeName: the picker
				// must never aggregate a name declared on a different container type into this effect's list.
				if (!Declared.ContainerTypeName.Equals(ContainerTypeName, ESearchCase::CaseSensitive))
				{
					continue;
				}
				if (Declared.FunctionName.Equals(OwnFunctionName, ESearchCase::CaseSensitive))
				{
					// Offering an effect as its own callee is a cycle the server would reject.
					continue;
				}
				Names.AddUnique(Declared.FunctionName);
			}
		}

		Names.Sort([](const FString& A, const FString& B) { return A < B; });
		return Names;
	}
}
