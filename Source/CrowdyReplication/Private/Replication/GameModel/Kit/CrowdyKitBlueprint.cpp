// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyKitBlueprint.h"

#include "Dom/JsonObject.h"
#include "Replication/GameModel/Kit/CrowdyKitJson.h"

namespace CrowdyKit
{
	namespace
	{
		// A shallow copy of a JSON object with the app id bound as a string. Only a top-level field changes, so
		// copying the member map is enough and the source blueprint object is left untouched for reuse.
		TSharedPtr<FJsonObject> BindAppId(const TSharedPtr<FJsonObject>& Source, const FString& AppId)
		{
			TSharedPtr<FJsonObject> Bound = MakeShared<FJsonObject>();
			if (Source.IsValid())
			{
				Bound->Values = Source->Values;
			}
			Bound->SetStringField(TEXT("appId"), AppId);
			return Bound;
		}
	}

	FString BlueprintField(const TSharedPtr<FJsonObject>& Object, const FString& Key)
	{
		if (!Object.IsValid())
		{
			return FString();
		}
		FString Value;
		if (Object->TryGetStringField(Key, Value))
		{
			return Value;
		}
		return FString();
	}

	bool MergeBlueprints(const FString& AppId, const TArray<FCrowdyKitBlueprint>& Blueprints,
		const FString& SessionId, FCrowdyMergedBlueprints& Out, TArray<FString>& OutErrors)
	{
		TArray<TSharedPtr<FJsonObject>> ContainerTypes, PropertyDefinitions, Functions, Containers, Edges;
		TArray<TSharedPtr<FJsonObject>> Automations, AutomationTriggers;

		TMap<FString, FString> SeenTypes, SeenProps, SeenFunctions, SeenAutomations, SeenTempIds;

		auto Claim = [&OutErrors](TMap<FString, FString>& Seen, const FString& Key, const FString& BlueprintName,
			const TCHAR* Kind) -> bool
		{
			if (const FString* Existing = Seen.Find(Key))
			{
				OutErrors.Add(FString::Printf(
					TEXT("Blueprint '%s' redefines %s '%s' already defined by blueprint '%s'"),
					*BlueprintName, Kind, *Key, **Existing));
				return false;
			}
			Seen.Add(Key, BlueprintName);
			return true;
		};

		for (const FCrowdyKitBlueprint& Bp : Blueprints)
		{
			for (const TSharedPtr<FJsonObject>& T : Bp.ContainerTypes)
			{
				if (!Claim(SeenTypes, BlueprintField(T, TEXT("typeName")), Bp.Name, TEXT("container type")))
				{
					return false;
				}
				ContainerTypes.Add(T);
			}
			for (const TSharedPtr<FJsonObject>& P : Bp.PropertyDefinitions)
			{
				const FString Key = BlueprintField(P, TEXT("containerTypeName")) + TEXT(".") + BlueprintField(P, TEXT("key"));
				if (!Claim(SeenProps, Key, Bp.Name, TEXT("property")))
				{
					return false;
				}
				PropertyDefinitions.Add(P);
			}
			for (const TSharedPtr<FJsonObject>& F : Bp.Functions)
			{
				if (!Claim(SeenFunctions, BlueprintField(F, TEXT("name")), Bp.Name, TEXT("function")))
				{
					return false;
				}
				Functions.Add(F);
			}
			for (const TSharedPtr<FJsonObject>& C : Bp.Containers)
			{
				if (!Claim(SeenTempIds, BlueprintField(C, TEXT("tempId")), Bp.Name, TEXT("container tempId")))
				{
					return false;
				}
				Containers.Add(C);
			}
			for (const TSharedPtr<FJsonObject>& E : Bp.Edges)
			{
				Edges.Add(E);
			}
			for (const TSharedPtr<FJsonObject>& A : Bp.Automations)
			{
				if (!Claim(SeenAutomations, BlueprintField(A, TEXT("name")), Bp.Name, TEXT("automation")))
				{
					return false;
				}
				Automations.Add(BindAppId(A, AppId));
			}
			for (const TSharedPtr<FJsonObject>& T : Bp.AutomationTriggers)
			{
				AutomationTriggers.Add(BindAppId(T, AppId));
			}
		}

		TSharedPtr<FJsonObject> SeedInput = MakeShared<FJsonObject>();
		SeedInput->SetStringField(TEXT("appId"), AppId);
		if (!SessionId.IsEmpty())
		{
			SeedInput->SetStringField(TEXT("sessionId"), SessionId);
		}
		if (ContainerTypes.Num() > 0)
		{
			SeedInput->SetArrayField(TEXT("containerTypes"), CrowdyKitJson::ArrayOfObjects(ContainerTypes)->AsArray());
		}
		if (PropertyDefinitions.Num() > 0)
		{
			SeedInput->SetArrayField(TEXT("propertyDefinitions"), CrowdyKitJson::ArrayOfObjects(PropertyDefinitions)->AsArray());
		}
		if (Functions.Num() > 0)
		{
			SeedInput->SetArrayField(TEXT("functions"), CrowdyKitJson::ArrayOfObjects(Functions)->AsArray());
		}
		if (Containers.Num() > 0)
		{
			SeedInput->SetArrayField(TEXT("containers"), CrowdyKitJson::ArrayOfObjects(Containers)->AsArray());
		}
		if (Edges.Num() > 0)
		{
			SeedInput->SetArrayField(TEXT("edges"), CrowdyKitJson::ArrayOfObjects(Edges)->AsArray());
		}

		Out.SeedInput = SeedInput;
		Out.Automations = MoveTemp(Automations);
		Out.AutomationTriggers = MoveTemp(AutomationTriggers);
		return true;
	}

	FCrowdyKitBlueprint ComposeBlueprints(const FString& Name, const TArray<FCrowdyKitBlueprint>& List)
	{
		FCrowdyKitBlueprint Out;
		Out.Name = Name;
		for (const FCrowdyKitBlueprint& Bp : List)
		{
			Out.ContainerTypes.Append(Bp.ContainerTypes);
			Out.PropertyDefinitions.Append(Bp.PropertyDefinitions);
			Out.Functions.Append(Bp.Functions);
			Out.Containers.Append(Bp.Containers);
			Out.Edges.Append(Bp.Edges);
			Out.Automations.Append(Bp.Automations);
			Out.AutomationTriggers.Append(Bp.AutomationTriggers);
		}
		return Out;
	}
}
