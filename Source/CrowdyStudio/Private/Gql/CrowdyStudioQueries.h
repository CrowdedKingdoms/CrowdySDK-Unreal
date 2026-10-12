// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h" // WrapDataEnvelope constructs one inline, so the full type is needed here
#include "Model/CrowdyStudioTypes.h"

// Response parsing and variables-building for the studio operations. The GraphQL query/mutation
// documents themselves live in the shared API client's generated operation set; this file only
// turns their responses back into studio types and builds the input variables the few remaining
// hand-built payloads need. Field/argument names match the live management schema: ids are the
// BigInt scalar, list/create ops take input objects, appsForOrg keys on the org slug, and status /
// visibility are the AppStatus / AppVisibility enums.
namespace CrowdyStudioGql
{
	// The API client hands back a response's bare `data` object, while every Parse* function below reads a whole
	// { "data": ... } envelope. Wrapping it back up is what lets those parsers stay exactly as they were when the
	// transport underneath them changed. Lives here, in the one header both callers already include, because two
	// file-local copies in the same module would be merged into a single translation unit by a unity build and
	// redefine each other.
	inline TSharedPtr<FJsonObject> WrapDataEnvelope(const TSharedPtr<FJsonObject>& DataObject)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		if (DataObject.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), DataObject);
		}
		return Envelope;
	}

	// Each parser takes the full response envelope ({ data, errors }) and digs into
	// `data` itself, returning false when the expected node is missing.

	void ParseOrganizations(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioOrg>>& OutOrgs);
	TSharedPtr<FStudioOrg> ParseOrganization(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName);
	void ParseApps(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioApp>>& OutApps);
	TSharedPtr<FStudioApp> ParseApp(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName);

	// Folds the appDiscovery entry for AppId (datacenter code, WS endpoint) into an app record. False when the
	// reply carries no entry for that id.
	bool ParseAppDiscovery(const TSharedPtr<FJsonObject>& Envelope, int64 AppId, FStudioApp& InOutApp);
	void ParsePlaceableDatacenters(const TSharedPtr<FJsonObject>& Envelope, TArray<FStudioDatacenter>& OutDatacenters, bool& bOutPlacementEnforced);
	// platformConfig.freeAppsPerOrg, or 0 when the reply has no platformConfig node.
	int32 ParseFreeAppsPerOrg(const TSharedPtr<FJsonObject>& Envelope);

	// The { input: CreateAppInput } variables for createApp. The datacenter is a required, permanent placement;
	// the server refuses a create without one. Status and visibility are left to the server's defaults.
	TSharedPtr<FJsonObject> BuildCreateAppVariables(int64 OrgId, const FString& Name, const FString& Slug,
		const FString& Datacenter, const FString& Description);

	// The URL identifier the server accepts for a display name: lowercase letters, digits and single dashes.
	FString SlugFromName(const FString& Name);

	bool ParseGroupPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGroupPolicy& OutPolicy);
	void ParseGroups(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                 TArray<TSharedPtr<FStudioGroup>>& OutGroups);
	void ParseGroupMembers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioGroupMember>>& OutMembers);
	void ParseGroupRoles(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioGroupRole>>& OutRoles);

	// Grid. Grant/revoke-user and limits results all expose a flat permissionKeys array, so one
	// key reader serves them; group grants and nearby scans get their own shapes.
	void ParseNearbyGrids(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioGrid>>& OutGrids);
	bool ParseCreateGrid(const TSharedPtr<FJsonObject>& Envelope, FStudioGrid& OutGrid, FString& OutError);
	void ParseGridPermissionKeys(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, TArray<FString>& OutKeys);
	void ParseGridGroupGrants(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                          TArray<TSharedPtr<FStudioGridGroupGrant>>& OutGrants);
	void ParseRuntimePermissions(const TSharedPtr<FJsonObject>& Envelope, TArray<FString>& OutKeys);

	// App access: feature keys, the tier-feature grants and the access tiers.
	void ParseFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                   TArray<TSharedPtr<FStudioAppFeature>>& OutFeatures);
	void ParseTierFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioTierFeature>>& OutGrants);
	void ParseAccessTiers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<TSharedPtr<FStudioAccessTier>>& OutTiers);
}
