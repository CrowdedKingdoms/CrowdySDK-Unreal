// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Model/CrowdyStudioTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/SharedPointer.h"

/**
 * Conversions between the Game Model function editor's in-memory rows and the JSON the Game API
 * stores for a function's parameters, mutations, and invoke policy. These live in one header so
 * the several views that edit a function share a single definition of the wire shape.
 */
namespace CrowdyStudioFunctionJson
{
	inline FString ParamsToJson(const TArray<FStudioFunctionParam>& Params)
	{
		if (Params.Num() == 0)
		{
			return FString();
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FStudioFunctionParam& Param : Params)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Param.Name);
			Object->SetStringField(TEXT("valueType"), Param.ValueType);
			Object->SetBoolField(TEXT("required"), Param.bRequired);
			if (!Param.DefaultValueJson.IsEmpty())
			{
				Object->SetStringField(TEXT("defaultValueJson"), Param.DefaultValueJson);
			}
			if (!Param.Description.IsEmpty())
			{
				Object->SetStringField(TEXT("description"), Param.Description);
			}
			Object->SetNumberField(TEXT("sortOrder"), Param.SortOrder);
			Items.Add(MakeShared<FJsonValueObject>(Object));
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		return Out;
	}

	inline FString MutationsToJson(const TArray<FStudioFunctionMutation>& Mutations)
	{
		if (Mutations.Num() == 0)
		{
			return FString();
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FStudioFunctionMutation& Mutation : Mutations)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("target"), Mutation.Target);
			Object->SetStringField(TEXT("property"), Mutation.Property);
			Object->SetStringField(TEXT("expression"), Mutation.Expression);
			Items.Add(MakeShared<FJsonValueObject>(Object));
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		return Out;
	}

	inline bool IsPolicyLeafType(const FString& Type)
	{
		return Type == TEXT("owner_of_self") || Type == TEXT("is_current_turn") || Type == TEXT("is_host")
			|| Type == TEXT("is_participant") || Type == TEXT("tier_feature") || Type == TEXT("group_permission")
			|| Type == TEXT("grid_permission") || Type == TEXT("condition");
	}

	// Read one leaf node's fields. Ids are BigInt-as-string on the wire, but tolerate a raw number so a
	// hand-written policy with numeric ids still loads into the builder rather than silently losing them.
	inline TSharedPtr<FStudioPolicyRule> ReadPolicyLeaf(const TSharedPtr<FJsonObject>& Node)
	{
		auto ReadId = [&Node](const TCHAR* Field, FString& Out)
		{
			if (!Node->TryGetStringField(Field, Out))
			{
				double Num = 0.0;
				if (Node->TryGetNumberField(Field, Num))
				{
					Out = FString::Printf(TEXT("%lld"), static_cast<int64>(Num));
				}
			}
		};

		TSharedPtr<FStudioPolicyRule> Rule = MakeShared<FStudioPolicyRule>();
		Node->TryGetStringField(TEXT("type"), Rule->Type);
		Node->TryGetStringField(TEXT("feature"), Rule->Feature);
		ReadId(TEXT("groupId"), Rule->GroupId);
		Node->TryGetStringField(TEXT("permission"), Rule->Permission);
		Node->TryGetStringField(TEXT("key"), Rule->Key);
		ReadId(TEXT("gridId"), Rule->GridId);
		Node->TryGetStringField(TEXT("expression"), Rule->Expression);
		return Rule;
	}

	// Parse a stored invoke policy into a flat rule list + top-level connector. Returns true when the
	// builder can represent it: an empty policy, a single bare leaf, or one and/or of leaves. Returns
	// false for a nested group, a "not", or any unknown type, which stays as raw JSON.
	inline bool ParsePolicyJson(const FString& Json, TArray<TSharedPtr<FStudioPolicyRule>>& OutRules, FString& OutConnector)
	{
		OutRules.Reset();
		OutConnector = TEXT("and");

		const FString Trimmed = Json.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return false;
		}

		FString Type;
		if (!Root->TryGetStringField(TEXT("type"), Type))
		{
			return false;
		}

		if (Type == TEXT("and") || Type == TEXT("or"))
		{
			OutConnector = Type;
			const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
			if (!Root->TryGetArrayField(TEXT("rules"), Rules))
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Entry : *Rules)
			{
				const TSharedPtr<FJsonObject>* Node = nullptr;
				if (!Entry->TryGetObject(Node) || !Node->IsValid())
				{
					return false;
				}
				FString LeafType;
				(*Node)->TryGetStringField(TEXT("type"), LeafType);
				if (!IsPolicyLeafType(LeafType))
				{
					return false;
				}
				OutRules.Add(ReadPolicyLeaf(*Node));
			}
			return true;
		}

		if (IsPolicyLeafType(Type))
		{
			OutRules.Add(ReadPolicyLeaf(Root));
			return true;
		}

		return false;
	}

	// Serialize the flat builder state back to the stored policy JSON. No rules -> empty string (no
	// policy). A single rule is still wrapped in the connector for one stable shape. Only the fields a
	// rule kind uses are emitted; optional ids/permissions are omitted when blank.
	inline FString PolicyToJson(const FString& Connector, const TArray<TSharedPtr<FStudioPolicyRule>>& Rules)
	{
		TArray<TSharedPtr<FStudioPolicyRule>> Valid;
		for (const TSharedPtr<FStudioPolicyRule>& Rule : Rules)
		{
			if (Rule.IsValid() && !Rule->Type.IsEmpty())
			{
				Valid.Add(Rule);
			}
		}
		if (Valid.Num() == 0)
		{
			return FString();
		}

		auto LeafObject = [](const TSharedPtr<FStudioPolicyRule>& Rule) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("type"), Rule->Type);
			if (Rule->Type == TEXT("tier_feature"))
			{
				Object->SetStringField(TEXT("feature"), Rule->Feature);
			}
			else if (Rule->Type == TEXT("group_permission"))
			{
				Object->SetStringField(TEXT("groupId"), Rule->GroupId);
				if (!Rule->Permission.IsEmpty())
				{
					Object->SetStringField(TEXT("permission"), Rule->Permission);
				}
			}
			else if (Rule->Type == TEXT("grid_permission"))
			{
				Object->SetStringField(TEXT("key"), Rule->Key);
				if (!Rule->GridId.IsEmpty())
				{
					Object->SetStringField(TEXT("gridId"), Rule->GridId);
				}
			}
			else if (Rule->Type == TEXT("condition"))
			{
				Object->SetStringField(TEXT("expression"), Rule->Expression);
			}
			return Object;
		};

		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), Connector.IsEmpty() ? FString(TEXT("and")) : Connector);
		TArray<TSharedPtr<FJsonValue>> RuleValues;
		for (const TSharedPtr<FStudioPolicyRule>& Rule : Valid)
		{
			RuleValues.Add(MakeShared<FJsonValueObject>(LeafObject(Rule)));
		}
		Root->SetArrayField(TEXT("rules"), RuleValues);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Out;
	}
}
