// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyKitPolicy.h"

#include "Dom/JsonObject.h"
#include "Replication/GameModel/Kit/CrowdyKitJson.h"

namespace CrowdyKit
{
	TSharedPtr<FJsonObject> Policy(const FString& Type)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		return P;
	}

	TSharedPtr<FJsonObject> AllowPolicy() { return Policy(TEXT("allow")); }
	TSharedPtr<FJsonObject> OwnerOfSelfPolicy() { return Policy(TEXT("owner_of_self")); }
	TSharedPtr<FJsonObject> IsHostPolicy() { return Policy(TEXT("is_host")); }
	TSharedPtr<FJsonObject> IsCurrentTurnPolicy() { return Policy(TEXT("is_current_turn")); }
	TSharedPtr<FJsonObject> IsParticipantPolicy() { return Policy(TEXT("is_participant")); }
	TSharedPtr<FJsonObject> IsAutomationPolicy() { return Policy(TEXT("is_automation")); }

	TSharedPtr<FJsonObject> ConditionPolicy(const FString& Expression)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("condition"));
		P->SetStringField(TEXT("expression"), Expression);
		return P;
	}

	TSharedPtr<FJsonObject> FeatureGate(const FString& Feature)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("tier_feature"));
		P->SetStringField(TEXT("feature"), Feature);
		return P;
	}

	TSharedPtr<FJsonObject> GridPermissionPolicy(const FString& Key, const FString& GridId)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("grid_permission"));
		P->SetStringField(TEXT("key"), Key);
		if (!GridId.IsEmpty())
		{
			P->SetStringField(TEXT("gridId"), GridId);
		}
		return P;
	}

	TSharedPtr<FJsonObject> GroupPermissionPolicy(const FString& GroupId, const FString& Permission)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("group_permission"));
		P->SetStringField(TEXT("groupId"), GroupId);
		if (!Permission.IsEmpty())
		{
			P->SetStringField(TEXT("permission"), Permission);
		}
		return P;
	}

	TSharedPtr<FJsonObject> AndPolicy(const TArray<TSharedPtr<FJsonObject>>& Rules)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("and"));
		P->SetArrayField(TEXT("rules"), CrowdyKitJson::ArrayOfObjects(Rules)->AsArray());
		return P;
	}

	TSharedPtr<FJsonObject> OrPolicy(const TArray<TSharedPtr<FJsonObject>>& Rules)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("or"));
		P->SetArrayField(TEXT("rules"), CrowdyKitJson::ArrayOfObjects(Rules)->AsArray());
		return P;
	}

	TSharedPtr<FJsonObject> NotPolicy(const TSharedPtr<FJsonObject>& Rule)
	{
		TSharedPtr<FJsonObject> P = Policy(TEXT("not"));
		P->SetObjectField(TEXT("rule"), Rule);
		return P;
	}

	TSharedPtr<FJsonObject> AndPolicies(const TSharedPtr<FJsonObject>& Base, const TArray<TSharedPtr<FJsonObject>>& Extra)
	{
		TArray<TSharedPtr<FJsonObject>> Rules;
		Rules.Add(Base);
		for (const TSharedPtr<FJsonObject>& E : Extra)
		{
			if (E.IsValid())
			{
				Rules.Add(E);
			}
		}
		if (Rules.Num() == 1)
		{
			return Rules[0];
		}
		return AndPolicy(Rules);
	}

	FString KitPolicyJson(const TSharedPtr<FJsonObject>& InPolicy)
	{
		return CrowdyKitJson::Canonical(InPolicy);
	}

	FCrowdyTrustedAuthority FCrowdyTrustedAuthority::Server()
	{
		FCrowdyTrustedAuthority A;
		A.Kind = EKind::Server;
		return A;
	}

	FCrowdyTrustedAuthority FCrowdyTrustedAuthority::Host()
	{
		FCrowdyTrustedAuthority A;
		A.Kind = EKind::Host;
		return A;
	}

	FCrowdyTrustedAuthority FCrowdyTrustedAuthority::Automation()
	{
		FCrowdyTrustedAuthority A;
		A.Kind = EKind::Automation;
		return A;
	}

	FCrowdyTrustedAuthority FCrowdyTrustedAuthority::Owner()
	{
		FCrowdyTrustedAuthority A;
		A.Kind = EKind::Owner;
		return A;
	}

	FCrowdyTrustedAuthority FCrowdyTrustedAuthority::CustomRule(const TSharedPtr<FJsonObject>& Rule)
	{
		FCrowdyTrustedAuthority A;
		A.Kind = EKind::Custom;
		A.Custom = Rule;
		return A;
	}

	void ApplyTrustedAuthority(TSharedPtr<FJsonObject>& Fn, const FCrowdyTrustedAuthority& Authority,
		const FString& ExtraCondition)
	{
		TSharedPtr<FJsonObject> Rule;
		switch (Authority.Kind)
		{
		case FCrowdyTrustedAuthority::EKind::Server: Rule = AllowPolicy(); break;
		case FCrowdyTrustedAuthority::EKind::Host: Rule = IsHostPolicy(); break;
		case FCrowdyTrustedAuthority::EKind::Automation: Rule = IsAutomationPolicy(); break;
		case FCrowdyTrustedAuthority::EKind::Owner: Rule = OwnerOfSelfPolicy(); break;
		case FCrowdyTrustedAuthority::EKind::Custom: Rule = Authority.Custom; break;
		}

		if (!ExtraCondition.IsEmpty())
		{
			Rule = AndPolicy({Rule, ConditionPolicy(ExtraCondition)});
		}

		Fn->SetStringField(TEXT("invokePolicyJson"), KitPolicyJson(Rule));
		if (Authority.Kind == FCrowdyTrustedAuthority::EKind::Server)
		{
			Fn->SetStringField(TEXT("invokeScope"), TEXT("server"));
		}
		if (Authority.Kind == FCrowdyTrustedAuthority::EKind::Automation)
		{
			Fn->SetBoolField(TEXT("autonomousInvocable"), true);
		}
	}

	FString ToSnakeCase(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len() + 4);
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR C = Name[Index];
			if (C == TEXT(' ') || C == TEXT('-'))
			{
				if (Out.Len() > 0 && Out[Out.Len() - 1] != TEXT('_'))
				{
					Out.AppendChar(TEXT('_'));
				}
				continue;
			}
			if (FChar::IsUpper(C))
			{
				if (Index > 0 && (FChar::IsLower(Name[Index - 1]) || FChar::IsDigit(Name[Index - 1])))
				{
					Out.AppendChar(TEXT('_'));
				}
				Out.AppendChar(FChar::ToLower(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	FString OwnerEquals(const FString& OwnerExpr, const FString& UserExpr, EOwnerIdKind Kind)
	{
		FString Out = OwnerExpr;
		Out += TEXT(" == ");
		if (Kind == EOwnerIdKind::String)
		{
			Out += TEXT("to_string(");
			Out += UserExpr;
			Out += TEXT(")");
		}
		else
		{
			Out += UserExpr;
		}
		return Out;
	}

	FString OwnerEqualsCaller(const FString& OwnerExpr, EOwnerIdKind Kind)
	{
		return OwnerEquals(OwnerExpr, TEXT("$caller_user_id"), Kind);
	}

	TSharedPtr<FJsonObject> OwnerMirrorProperty(const FString& ContainerTypeName, EOwnerIdKind Kind)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("containerTypeName"), ContainerTypeName);
		P->SetStringField(TEXT("key"), TEXT("owner_user_id"));
		P->SetStringField(TEXT("valueType"), Kind == EOwnerIdKind::String ? TEXT("string") : TEXT("int"));
		P->SetStringField(TEXT("defaultValueJson"), Kind == EOwnerIdKind::String ? TEXT("\"\"") : TEXT("0"));
		P->SetStringField(TEXT("description"),
			TEXT("Mirror of the container owner's user id (kit convention: expressions cannot read container ownership)."));
		return P;
	}
}
