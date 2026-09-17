// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h" // WrapDataEnvelope constructs one inline, so the full type is needed here
#include "Model/CrowdyStudioTypes.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h" // FCrowdyGameModelAutomationInput, ...Trigger...

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

	// Build the variables for gameModelUpsertAutomation / gameModelUpsertAutomationTrigger from a neutral input
	// (compiled from a UCrowdyEffect that opts into running automatically). Every field is emitted EXPLICITLY so the
	// server read-back matches and a follow-up plan is a genuine no-op; appId is emitted as a JSON string (BigInt).
	// The returned object is the full { input: {...} } variables wrapper.
	TSharedPtr<FJsonObject> BuildAutomationUpsertVariables(const FCrowdyGameModelAutomationInput& Automation, int64 AppId);
	TSharedPtr<FJsonObject> BuildAutomationTriggerUpsertVariables(const FCrowdyGameModelAutomationTriggerInput& Trigger, int64 AppId);

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

	// Game model.
	void ParseContainerTypes(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                         TArray<TSharedPtr<FStudioContainerType>>& OutTypes);
	void ParsePropertyDefs(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioPropertyDef>>& OutDefs);
	void ParseFunctions(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                    TArray<TSharedPtr<FStudioFunction>>& OutFns);
	bool ParseFunction(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioFunction& OutFn);

	// Read the authored fields of every GmAutomation (the circuit-breaker runtime fields are ignored: they are
	// server-owned state, not part of the desired schema the diff compares).
	void ParseAutomations(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<FStudioAutomation>& OutAutomations);
	// Read every GmAutomationTrigger. The wire references its automation by id, but the diff keys triggers by
	// automation NAME, so the automations read in the same plan are passed in to resolve automationId -> name.
	void ParseAutomationTriggers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                             const TArray<FStudioAutomation>& AutomationsForNameLookup,
	                             TArray<FStudioAutomationTrigger>& OutTriggers);
	void ParseFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                   TArray<TSharedPtr<FStudioAppFeature>>& OutFeatures);
	void ParseTierFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioTierFeature>>& OutGrants);
	void ParseAccessTiers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<TSharedPtr<FStudioAccessTier>>& OutTiers);
	bool ParseGameModelPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGameModelPolicy& OutPolicy);
	bool ParseSeedResult(const TSharedPtr<FJsonObject>& Envelope, FString& OutSummary);
	// The container half of a seed result: new rows only in OutCreated; OutIdMapJson maps every sent tempId.
	bool ParseSeedContainers(const TSharedPtr<FJsonObject>& Envelope, int32& OutCreated, FString& OutIdMapJson);
	bool ParseModelLint(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioLintReport& OutReport);

	void ParseContainers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioContainer>>& OutContainers);
	bool ParseContainerState(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioContainerState& OutState);
	void ParseSessions(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, TArray<FStudioSession>& OutSessions);
}
