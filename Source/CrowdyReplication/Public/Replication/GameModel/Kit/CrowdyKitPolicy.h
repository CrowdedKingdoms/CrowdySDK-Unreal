// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Invoke policies, authority helpers, and naming/expression fragments for the GameKit preset layer.
 *
 * A policy is a plain JSON object with a "type" field, serialized to the wire invokePolicyJson through the
 * canonical serializer. The authority helpers compile the anti-cheat convention (reward-granting mutations must
 * never be plain player calls) into the function-level fields a model function definition carries.
 */
namespace CrowdyKit
{
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> Policy(const FString& Type);
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> AllowPolicy();
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> OwnerOfSelfPolicy();
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> IsHostPolicy();
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> IsCurrentTurnPolicy();
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> IsParticipantPolicy();
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> IsAutomationPolicy();
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> ConditionPolicy(const FString& Expression);

	// Monetization gate: the caller's access tier must hold the named feature.
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> FeatureGate(const FString& Feature);

	CROWDYREPLICATION_API TSharedPtr<FJsonObject> GridPermissionPolicy(const FString& Key, const FString& GridId = FString());
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> GroupPermissionPolicy(const FString& GroupId, const FString& Permission = FString());

	CROWDYREPLICATION_API TSharedPtr<FJsonObject> AndPolicy(const TArray<TSharedPtr<FJsonObject>>& Rules);
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> OrPolicy(const TArray<TSharedPtr<FJsonObject>>& Rules);
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> NotPolicy(const TSharedPtr<FJsonObject>& Rule);

	// AND extra rules into a base policy: null extras are skipped, and a single surviving rule collapses to itself
	// (no wrapping "and"). The composition point for the *policyExtra options.
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> AndPolicies(const TSharedPtr<FJsonObject>& Base,
		const TArray<TSharedPtr<FJsonObject>>& Extra = TArray<TSharedPtr<FJsonObject>>());

	// Serialize a policy tree to the wire invokePolicyJson.
	CROWDYREPLICATION_API FString KitPolicyJson(const TSharedPtr<FJsonObject>& Policy);

	/**
	 * Who may call a TRUSTED mutation (XP grants, score submits, loot rolls, currency mints). Reward-granting
	 * functions must never be plain player calls (anti-cheat convention).
	 */
	struct CROWDYREPLICATION_API FCrowdyTrustedAuthority
	{
		enum class EKind : uint8
		{
			Server,
			Host,
			Automation,
			Owner,
			Custom
		};

		EKind Kind = EKind::Server;
		TSharedPtr<FJsonObject> Custom;

		static FCrowdyTrustedAuthority Server();
		static FCrowdyTrustedAuthority Host();
		static FCrowdyTrustedAuthority Automation();
		static FCrowdyTrustedAuthority Owner();
		static FCrowdyTrustedAuthority CustomRule(const TSharedPtr<FJsonObject>& Rule);
	};

	// Merge the function-level fields a trusted authority compiles to into a function definition (invokePolicyJson,
	// invokeScope, autonomousInvocable). A non-empty ExtraCondition is AND'ed into the policy.
	CROWDYREPLICATION_API void ApplyTrustedAuthority(TSharedPtr<FJsonObject>& Fn,
		const FCrowdyTrustedAuthority& Authority, const FString& ExtraCondition = FString());

	// How kit types mirror their owner's user id into an owner_user_id property (expressions cannot read
	// container ownership directly). Kit standard is Int; use String only for models that mirrored the owner as a
	// string.
	enum class EOwnerIdKind : uint8
	{
		Int,
		String
	};

	// Convert PascalCase/camelCase to snake_case for derived function names.
	CROWDYREPLICATION_API FString ToSnakeCase(const FString& Name);

	// Expression fragment: OwnerExpr equals a user-id expression.
	CROWDYREPLICATION_API FString OwnerEquals(const FString& OwnerExpr, const FString& UserExpr,
		EOwnerIdKind Kind = EOwnerIdKind::Int);

	// Expression fragment: OwnerExpr equals the calling user.
	CROWDYREPLICATION_API FString OwnerEqualsCaller(const FString& OwnerExpr, EOwnerIdKind Kind = EOwnerIdKind::Int);

	// The property def for a kit-standard owner mirror property.
	CROWDYREPLICATION_API TSharedPtr<FJsonObject> OwnerMirrorProperty(const FString& ContainerTypeName,
		EOwnerIdKind Kind = EOwnerIdKind::Int);
}
