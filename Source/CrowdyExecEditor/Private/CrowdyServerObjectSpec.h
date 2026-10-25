#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/StrongObjectPtr.h"

class FJsonObject;
class UCrowdyServerObjectDefinition;

/** A Server Object type as a JSON spec: every authored setting of its definition by its C++ name, and nothing the bake rebuilds. */
namespace CrowdyServerObjectSpec
{
	/** The project's type called Name, or null. */
	using FFindType = TFunctionRef<UCrowdyServerObjectDefinition*(const FString&)>;

	/** A List value that kept its Id and changed its Name. */
	struct FRenamedListValue
	{
		FString List;
		FGuid Id;
		FString From;
		FString To;
	};

	/** The names of the settings a spec holds, in the definition's order. */
	TArray<FString> GetAuthoredKeys();

	/** Definition's authored settings. */
	TSharedRef<FJsonObject> ToJson(const UCrowdyServerObjectDefinition& Definition);

	/** Fills Out, a fresh definition, from Spec, which may leave out any setting (it keeps its default). Every problem is added to OutErrors; false if there was one. */
	bool FromJson(const FJsonObject& Spec, UCrowdyServerObjectDefinition& Out, FFindType FindType, TArray<FString>& OutErrors);

	/** The baked definition a spec for a new type describes, or null with every problem in OutErrors. Creates no asset. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeNew(const FJsonObject& Spec, TConstArrayView<UCrowdyServerObjectDefinition*> Project, TArray<FString>& OutErrors);

	/** The baked definition Existing becomes under Spec, which must hold every authored setting, or null with every problem in OutErrors. Changes nothing. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeEdited(const FJsonObject& Spec, const UCrowdyServerObjectDefinition& Existing, TConstArrayView<UCrowdyServerObjectDefinition*> Project,
		TArray<FString>& OutErrors);

	/** The settings whose values differ between A and B. */
	TArray<FString> FindChangedKeys(const UCrowdyServerObjectDefinition& A, const UCrowdyServerObjectDefinition& B);

	/** The List values After names differently from Before. */
	TArray<FRenamedListValue> FindRenamedListValues(const UCrowdyServerObjectDefinition& Before, const UCrowdyServerObjectDefinition& After);

	/** Gives To every authored setting of From. */
	void CopyAuthored(const UCrowdyServerObjectDefinition& From, UCrowdyServerObjectDefinition& To);

	/** Gives Existing every authored setting of Candidate, bakes it and saves it. On a failure, OutError says why and Existing is put back as it was. */
	bool ApplyEdit(const UCrowdyServerObjectDefinition& Candidate, UCrowdyServerObjectDefinition& Existing, FString& OutError);

	/** The package ContentPath/CSO_<TypeName in Pascal case> that CreateAsset would make. */
	FString MakePackageName(const FString& ContentPath, const UCrowdyServerObjectDefinition& Definition);

	/** Creates, bakes and saves a new asset holding Candidate's settings. Null, with OutError and nothing saved or left in memory, when ContentPath is not under /Game/, the package name is invalid or taken, or the asset does not bake or save. */
	UCrowdyServerObjectDefinition* CreateAsset(const UCrowdyServerObjectDefinition& Candidate, const FString& ContentPath, FString& OutError);

	/** Saves the package Definition lives in. */
	bool SaveDefinitionPackage(UCrowdyServerObjectDefinition& Definition, FString& OutError);

	/** Lists or unlists Definition in the Game Kit Actor Starters project setting as its Kit Actors ask; true if the setting changed. */
	bool SyncActorStarters(const UCrowdyServerObjectDefinition& Definition);
}
