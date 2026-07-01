// Fill out your copyright notice in the Description page of Project Settings.

#include "Gql/CrowdyStudioQueries.h"

#include "Dom/JsonObject.h"

namespace
{
	// IDs are the GraphQL BigInt scalar; the server may serialize one as a JSON string or a
	// number, so read both and don't care which it chose.
	int64 ReadId(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid())
		{
			return 0;
		}

		FString AsString;
		if (Object->TryGetStringField(Field, AsString))
		{
			return FCString::Atoi64(*AsString);
		}

		int64 AsNumber = 0;
		if (Object->TryGetNumberField(Field, AsNumber))
		{
			return AsNumber;
		}

		return 0;
	}

	TSharedPtr<FJsonObject> GetData(const TSharedPtr<FJsonObject>& Envelope)
	{
		if (!Envelope.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Envelope->TryGetObjectField(TEXT("data"), Data) && Data->IsValid())
		{
			return *Data;
		}

		return nullptr;
	}

	void ReadApp(const TSharedPtr<FJsonObject>& Node, FStudioApp& Out)
	{
		Out.AppId = ReadId(Node, TEXT("appId"));
		Out.OrgId = ReadId(Node, TEXT("orgId"));
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("slug"), Out.Slug);
		Node->TryGetStringField(TEXT("status"), Out.Status);
		Node->TryGetStringField(TEXT("visibility"), Out.Visibility);
		Node->TryGetStringField(TEXT("gameApiUrl"), Out.GameApiUrl);
		Node->TryGetStringField(TEXT("deploymentTarget"), Out.DeploymentTarget);

		bool bSplitMode = false;
		if (Node->TryGetBoolField(TEXT("splitMode"), bSplitMode))
		{
			Out.SplitMode = bSplitMode ? TEXT("true") : TEXT("false");
		}
		// App has no gameApiWsUrl — the WS endpoint comes from platformConfig.
	}

	void ReadEnvironment(const TSharedPtr<FJsonObject>& Node, FStudioEnvironment& Out)
	{
		Node->TryGetStringField(TEXT("id"), Out.EnvironmentId);
		Node->TryGetStringField(TEXT("slug"), Out.Slug);
		Node->TryGetStringField(TEXT("displayName"), Out.DisplayName);
		Node->TryGetStringField(TEXT("status"), Out.Status);
		Node->TryGetStringField(TEXT("environmentClass"), Out.EnvironmentClass);
	}

	void ReadStringArray(const TSharedPtr<FJsonObject>& Node, const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(Field, Array))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			FString Value;
			if (Entry->TryGetString(Value))
			{
				Out.Add(Value);
			}
		}
	}

	void ReadGroup(const TSharedPtr<FJsonObject>& Node, FStudioGroup& Out)
	{
		Out.GroupId = ReadId(Node, TEXT("groupId"));
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("groupType"), Out.GroupType);
		Node->TryGetStringField(TEXT("membershipPolicy"), Out.MembershipPolicy);
		Node->TryGetStringField(TEXT("status"), Out.Status);
	}

	void ReadGroupPolicy(const TSharedPtr<FJsonObject>& Node, FStudioGroupPolicy& Out)
	{
		Out.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetStringField(TEXT("groupType"), Out.GroupType);
		Node->TryGetStringField(TEXT("creationPolicy"), Out.CreationPolicy);
		Node->TryGetStringField(TEXT("defaultMembershipPolicy"), Out.DefaultMembershipPolicy);
		Node->TryGetNumberField(TEXT("maxMembers"), Out.MaxMembers);
		Node->TryGetNumberField(TEXT("maxGroupsPerUser"), Out.MaxGroupsPerUser);
	}

	// A BigInt field that may be null. Returns false when the field is absent or null so callers
	// can tell "no cap configured" from a real zero.
	bool TryReadBigInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		FString AsString;
		if (Object->TryGetStringField(Field, AsString))
		{
			OutValue = FCString::Atoi64(*AsString);
			return true;
		}

		int64 AsNumber = 0;
		if (Object->TryGetNumberField(Field, AsNumber))
		{
			OutValue = AsNumber;
			return true;
		}

		return false;
	}

	TSharedPtr<FJsonObject> GetDataNode(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (Data->TryGetObjectField(OpName, Node) && Node->IsValid())
		{
			return *Node;
		}
		return nullptr;
	}

	// login / devLogin / socialLoginComplete / completeLoginLink all return the same AuthResponse shape
	// ({ token, user { userId } }). One reader keeps the four sign-in parsers from drifting. Reads
	// data.<OpName>.token and data.<OpName>.user.userId; false when the token is absent or empty.
	bool ReadAuthResponse(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FString& OutToken, int64& OutUserId)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		if (!Node->TryGetStringField(TEXT("token"), OutToken) || OutToken.IsEmpty())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* User = nullptr;
		if (Node->TryGetObjectField(TEXT("user"), User) && User->IsValid())
		{
			OutUserId = ReadId(*User, TEXT("userId"));
		}
		return true;
	}

	void ReadChunkField(const TSharedPtr<FJsonObject>& Node, const TCHAR* Field, FStudioChunk& Out)
	{
		const TSharedPtr<FJsonObject>* ChunkNode = nullptr;
		if (Node.IsValid() && Node->TryGetObjectField(Field, ChunkNode) && ChunkNode->IsValid())
		{
			Out.X = ReadId(*ChunkNode, TEXT("x"));
			Out.Y = ReadId(*ChunkNode, TEXT("y"));
			Out.Z = ReadId(*ChunkNode, TEXT("z"));
		}
	}

	void ReadGridGroupGrant(const TSharedPtr<FJsonObject>& Node, FStudioGridGroupGrant& Out)
	{
		Out.GridId = ReadId(Node, TEXT("gridId"));
		Out.GroupId = ReadId(Node, TEXT("groupId"));
		int64 RoleId = 0;
		Out.bHasRole = TryReadBigInt(Node, TEXT("groupRoleId"), RoleId);
		Out.GroupRoleId = RoleId;
		Node->TryGetStringField(TEXT("permissionKey"), Out.PermissionKey);
		Node->TryGetStringField(TEXT("expiresAt"), Out.ExpiresAt);
	}

	void ReadContainerType(const TSharedPtr<FJsonObject>& Node, FStudioContainerType& Out)
	{
		Out.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetStringField(TEXT("typeName"), Out.TypeName);
		Node->TryGetStringField(TEXT("displayName"), Out.DisplayName);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("instantiableBy"), Out.InstantiableBy);
		Node->TryGetStringField(TEXT("defaultPropertyVisibility"), Out.DefaultPropertyVisibility);
		Node->TryGetStringField(TEXT("metadataJson"), Out.MetadataJson);
	}

	void ReadPropertyDef(const TSharedPtr<FJsonObject>& Node, FStudioPropertyDef& Out)
	{
		Node->TryGetStringField(TEXT("containerTypeName"), Out.ContainerTypeName);
		Node->TryGetStringField(TEXT("key"), Out.Key);
		Node->TryGetStringField(TEXT("valueType"), Out.ValueType);
		Node->TryGetStringField(TEXT("defaultValueJson"), Out.DefaultValueJson);
		Node->TryGetStringField(TEXT("visibility"), Out.Visibility);
		Node->TryGetStringField(TEXT("writable"), Out.Writable);
		Node->TryGetStringField(TEXT("description"), Out.Description);
	}

	void ReadFunction(const TSharedPtr<FJsonObject>& Node, FStudioFunction& Out)
	{
		Node->TryGetStringField(TEXT("functionId"), Out.FunctionId);
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("containerTypeName"), Out.ContainerTypeName);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("returnType"), Out.ReturnType);
		Node->TryGetStringField(TEXT("returnExpression"), Out.ReturnExpression);
		Node->TryGetStringField(TEXT("invokeScope"), Out.InvokeScope);
		Node->TryGetStringField(TEXT("invokePolicyJson"), Out.InvokePolicyJson);
		ReadStringArray(Node, TEXT("warnings"), Out.Warnings);

		const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
		if (Node->TryGetArrayField(TEXT("parameters"), Params))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Params)
			{
				const TSharedPtr<FJsonObject>* P = nullptr;
				if (Entry->TryGetObject(P) && P->IsValid())
				{
					FStudioFunctionParam Param;
					(*P)->TryGetStringField(TEXT("name"), Param.Name);
					(*P)->TryGetStringField(TEXT("valueType"), Param.ValueType);
					(*P)->TryGetBoolField(TEXT("required"), Param.bRequired);
					(*P)->TryGetStringField(TEXT("defaultValueJson"), Param.DefaultValueJson);
					(*P)->TryGetStringField(TEXT("description"), Param.Description);
					(*P)->TryGetNumberField(TEXT("sortOrder"), Param.SortOrder);
					Out.Parameters.Add(Param);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Muts = nullptr;
		if (Node->TryGetArrayField(TEXT("mutations"), Muts))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Muts)
			{
				const TSharedPtr<FJsonObject>* M = nullptr;
				if (Entry->TryGetObject(M) && M->IsValid())
				{
					FStudioFunctionMutation Mut;
					(*M)->TryGetStringField(TEXT("target"), Mut.Target);
					(*M)->TryGetStringField(TEXT("property"), Mut.Property);
					(*M)->TryGetStringField(TEXT("expression"), Mut.Expression);
					Out.Mutations.Add(Mut);
				}
			}
		}
	}

	void ReadFeature(const TSharedPtr<FJsonObject>& Node, FStudioAppFeature& Out)
	{
		Node->TryGetStringField(TEXT("featureKey"), Out.FeatureKey);
		Node->TryGetStringField(TEXT("description"), Out.Description);
	}

	void ReadTierFeature(const TSharedPtr<FJsonObject>& Node, FStudioTierFeature& Out)
	{
		Out.TierId = ReadId(Node, TEXT("tierId"));
		Node->TryGetStringField(TEXT("featureKey"), Out.FeatureKey);
	}
}

namespace CrowdyStudioGql
{
	FString LoginMutation()
	{
		return TEXT(
			"mutation StudioLogin($loginUserInput: LoginUserInput!) {"
			"  login(loginUserInput: $loginUserInput) {"
			"    token"
			"    gameTokenId"
			"    user { userId }"
			"  }"
			"}");
	}

	FString DevLoginMutation()
	{
		// The variable wrapper is `input` (DevLoginInput), not `loginUserInput`.
		return TEXT(
			"mutation StudioDevLogin($input: DevLoginInput!) {"
			"  devLogin(input: $input) {"
			"    token"
			"    gameTokenId"
			"    user { userId }"
			"  }"
			"}");
	}

	FString MintAppTokenMutation()
	{
		// appId is the BigInt scalar, interpolated as a JSON string into the input object. Field shape
		// follows the runtime mint (CrowdyNet FMintAppTokenRequest) — minus launchUrl, which Studio
		// doesn't consume — so the same server contract is exercised.
		return TEXT(
			"mutation StudioMintAppToken($appId: BigInt!) {"
			"  mintAppToken(input: { appId: $appId }) {"
			"    token"
			"    gameTokenId"
			"    appId"
			"    expiresAt"
			"    gameApiUrl"
			"    gameApiWsUrl"
			"  }"
			"}");
	}

	FString SocialLoginStartMutation()
	{
		// Flat vars with the input built inline (the runtime FSocialLoginStartRequest shape), so the same
		// server contract is exercised. Returns the provider consent URL and the CSRF state to arm the
		// loopback with. PUBLIC — sent with no bearer.
		return TEXT(
			"mutation StudioSocialLoginStart($provider: String!, $redirectUri: String!) {"
			"  socialLoginStart(input: { provider: $provider, redirectUri: $redirectUri }) {"
			"    authorizeUrl"
			"    state"
			"  }"
			"}");
	}

	FString SocialLoginCompleteMutation()
	{
		// The provider redirect's code + the round-tripped state complete sign-in and return the identity
		// SESSION token (same AuthResponse shape as login). PUBLIC — the one-time code authorizes it.
		return TEXT(
			"mutation StudioSocialLoginComplete($provider: String!, $code: String!, $state: String!) {"
			"  socialLoginComplete(input: { provider: $provider, code: $code, state: $state }) {"
			"    token"
			"    gameTokenId"
			"    user { userId email }"
			"  }"
			"}");
	}

	FString AvailableLoginProvidersQuery()
	{
		// The enabled federated providers (e.g. ["google"]). PUBLIC, no args; drives the sign-in buttons
		// so they are not hard-coded. The dev mock provider appears only under the server dev bypass.
		return TEXT(
			"query StudioAvailableLoginProviders {"
			"  availableLoginProviders"
			"}");
	}

	FString RequestLoginLinkMutation()
	{
		// Magic-link step 1: email a one-time link whose redirect lands on the loopback. redirectUri is a
		// nullable String (omitted server-side when null). In dev the response carries a devToken that
		// short-circuits the email round-trip. PUBLIC.
		return TEXT(
			"mutation StudioRequestLoginLink($email: String!, $redirectUri: String) {"
			"  requestLoginLink(input: { email: $email, redirectUri: $redirectUri }) {"
			"    sent"
			"    devToken"
			"  }"
			"}");
	}

	FString CompleteLoginLinkMutation()
	{
		// Magic-link step 2: the one-time token from the link (or the devToken) yields the SESSION token
		// (same AuthResponse shape as login). PUBLIC — the token authorizes it.
		return TEXT(
			"mutation StudioCompleteLoginLink($token: String!) {"
			"  completeLoginLink(input: { token: $token }) {"
			"    token"
			"    gameTokenId"
			"    user { userId email }"
			"  }"
			"}");
	}

	FString MyOrganizationsQuery()
	{
		return TEXT(
			"query StudioMyOrganizations {"
			"  myOrganizations {"
			"    org { orgId name slug }"
			"    permissions"
			"  }"
			"}");
	}

	FString CreateOrganizationMutation()
	{
		return TEXT(
			"mutation StudioCreateOrganization($input: CreateOrganizationInput!) {"
			"  createOrganization(input: $input) {"
			"    orgId"
			"    name"
			"    slug"
			"  }"
			"}");
	}

	FString MyAppsQuery()
	{
		// No args — returns every app the signed-in token can see, across orgs, so app ids
		// never have to be typed by hand.
		return TEXT(
			"query StudioMyApps {"
			"  myApps {"
			"    appId"
			"    orgId"
			"    name"
			"    slug"
			"    status"
			"    visibility"
			"    gameApiUrl"
			"    splitMode"
			"    deploymentTarget"
			"  }"
			"}");
	}

	FString CreateAppMutation()
	{
		return TEXT(
			"mutation StudioCreateApp($input: CreateAppInput!) {"
			"  createApp(input: $input) {"
			"    appId"
			"    name"
			"    slug"
			"    status"
			"    visibility"
			"  }"
			"}");
	}

	FString UpdateAppMutation()
	{
		return TEXT(
			"mutation StudioUpdateApp($appId: BigInt!, $input: UpdateAppInput!) {"
			"  updateApp(appId: $appId, input: $input) {"
			"    appId"
			"    name"
			"    slug"
			"    status"
			"    visibility"
			"  }"
			"}");
	}

	FString ArchiveAppMutation()
	{
		return TEXT(
			"mutation StudioArchiveApp($appId: BigInt!) {"
			"  archiveApp(appId: $appId) {"
			"    appId"
			"    status"
			"  }"
			"}");
	}

	FString AppQuery()
	{
		// orgId must be selected: FetchApp merges this detail over the myApps list entry (*Existing =
		// *Detail), so omitting orgId here would overwrite the good value with 0 and break the org-scoped
		// fetches (environments) on a re-click.
		return TEXT(
			"query StudioApp($appId: BigInt!) {"
			"  app(appId: $appId) {"
			"    appId"
			"    orgId"
			"    name"
			"    slug"
			"    status"
			"    visibility"
			"    gameApiUrl"
			"    splitMode"
			"    deploymentTarget"
			"  }"
			"}");
	}

	FString OrgEnvironmentsQuery()
	{
		return TEXT(
			"query StudioOrgEnvironments($orgId: BigInt!) {"
			"  orgEnvironments(orgId: $orgId) {"
			"    id"
			"    slug"
			"    displayName"
			"    status"
			"    environmentClass"
			"  }"
			"}");
	}

	FString LinkAppToEnvironmentMutation()
	{
		return TEXT(
			"mutation StudioLinkAppToEnvironment($input: LinkAppToEnvironmentInput!) {"
			"  linkAppToEnvironment(input: $input) {"
			"    appId"
			"    gameApiUrl"
			"  }"
			"}");
	}

	bool ParseLogin(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Login = nullptr;
		if (!Data->TryGetObjectField(TEXT("login"), Login) || !Login->IsValid())
		{
			return false;
		}

		if (!(*Login)->TryGetStringField(TEXT("token"), OutToken) || OutToken.IsEmpty())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* User = nullptr;
		if ((*Login)->TryGetObjectField(TEXT("user"), User) && User->IsValid())
		{
			OutUserId = ReadId(*User, TEXT("userId"));
		}

		return true;
	}

	bool ParseDevLogin(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* DevLogin = nullptr;
		if (!Data->TryGetObjectField(TEXT("devLogin"), DevLogin) || !DevLogin->IsValid())
		{
			return false;
		}

		if (!(*DevLogin)->TryGetStringField(TEXT("token"), OutToken) || OutToken.IsEmpty())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* User = nullptr;
		if ((*DevLogin)->TryGetObjectField(TEXT("user"), User) && User->IsValid())
		{
			OutUserId = ReadId(*User, TEXT("userId"));
		}

		return true;
	}

	bool ParseAppToken(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, FString& OutGameApiUrl,
	                   FString& OutGameApiWsUrl, FString& OutExpiresAt)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Mint = nullptr;
		if (!Data->TryGetObjectField(TEXT("mintAppToken"), Mint) || !Mint->IsValid())
		{
			return false;
		}

		if (!(*Mint)->TryGetStringField(TEXT("token"), OutToken) || OutToken.IsEmpty())
		{
			return false;
		}

		(*Mint)->TryGetStringField(TEXT("gameApiUrl"), OutGameApiUrl);
		(*Mint)->TryGetStringField(TEXT("gameApiWsUrl"), OutGameApiWsUrl);
		(*Mint)->TryGetStringField(TEXT("expiresAt"), OutExpiresAt);
		return true;
	}

	bool ParseSocialLoginStart(const TSharedPtr<FJsonObject>& Envelope, FString& OutAuthorizeUrl, FString& OutState)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, TEXT("socialLoginStart"));
		if (!Node.IsValid())
		{
			return false;
		}
		if (!Node->TryGetStringField(TEXT("authorizeUrl"), OutAuthorizeUrl) || OutAuthorizeUrl.IsEmpty())
		{
			return false;
		}
		Node->TryGetStringField(TEXT("state"), OutState);
		return true;
	}

	bool ParseSocialLoginComplete(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId)
	{
		return ReadAuthResponse(Envelope, TEXT("socialLoginComplete"), OutToken, OutUserId);
	}

	bool ParseCompleteLoginLink(const TSharedPtr<FJsonObject>& Envelope, FString& OutToken, int64& OutUserId)
	{
		return ReadAuthResponse(Envelope, TEXT("completeLoginLink"), OutToken, OutUserId);
	}

	bool ParseRequestLoginLink(const TSharedPtr<FJsonObject>& Envelope, bool& OutSent, FString& OutDevToken)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, TEXT("requestLoginLink"));
		if (!Node.IsValid())
		{
			return false;
		}
		Node->TryGetBoolField(TEXT("sent"), OutSent);
		Node->TryGetStringField(TEXT("devToken"), OutDevToken);
		return true;
	}

	void ParseProviders(const TSharedPtr<FJsonObject>& Envelope, TArray<FString>& OutProviders)
	{
		OutProviders.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}
		// availableLoginProviders is a [String] sitting directly under data (not a named object node).
		ReadStringArray(Data, TEXT("availableLoginProviders"), OutProviders);
	}

	void ParseOrganizations(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioOrg>>& OutOrgs)
	{
		OutOrgs.Reset();

		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("myOrganizations"), Array))
		{
			return;
		}

		// Each entry is an OrgMembership; the organization itself sits under "org".
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Membership = nullptr;
			if (!Entry->TryGetObject(Membership) || !Membership->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* OrgNode = nullptr;
			if (!(*Membership)->TryGetObjectField(TEXT("org"), OrgNode) || !OrgNode->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioOrg> Org = MakeShared<FStudioOrg>();
			Org->OrgId = ReadId(*OrgNode, TEXT("orgId"));
			(*OrgNode)->TryGetStringField(TEXT("name"), Org->Name);
			(*OrgNode)->TryGetStringField(TEXT("slug"), Org->Slug);

			const TArray<TSharedPtr<FJsonValue>>* PermissionValues = nullptr;
			if ((*Membership)->TryGetArrayField(TEXT("permissions"), PermissionValues))
			{
				for (const TSharedPtr<FJsonValue>& PermissionValue : *PermissionValues)
				{
					FString Key;
					if (PermissionValue->TryGetString(Key))
					{
						Org->Permissions.Add(Key);
					}
				}
			}

			OutOrgs.Add(Org);
		}
	}

	TSharedPtr<FStudioOrg> ParseOrganization(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(OpName, Node) || !Node->IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FStudioOrg> Org = MakeShared<FStudioOrg>();
		Org->OrgId = ReadId(*Node, TEXT("orgId"));
		(*Node)->TryGetStringField(TEXT("name"), Org->Name);
		(*Node)->TryGetStringField(TEXT("slug"), Org->Slug);
		return Org;
	}

	void ParseApps(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioApp>>& OutApps)
	{
		OutApps.Reset();

		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("myApps"), Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioApp> App = MakeShared<FStudioApp>();
			ReadApp(*Node, *App);
			OutApps.Add(App);
		}
	}

	TSharedPtr<FStudioApp> ParseApp(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(OpName, Node) || !Node->IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FStudioApp> App = MakeShared<FStudioApp>();
		ReadApp(*Node, *App);
		return App;
	}

	void ParseEnvironments(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioEnvironment>>& OutEnvs)
	{
		OutEnvs.Reset();

		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("orgEnvironments"), Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioEnvironment> Env = MakeShared<FStudioEnvironment>();
			ReadEnvironment(*Node, *Env);
			OutEnvs.Add(Env);
		}
	}

	// ── Teams & channels (game plane) ───────────────────────────────────────────────

	FString TeamsQuery()
	{
		return TEXT(
			"query StudioTeams($appId: BigInt!) {"
			"  teams(appId: $appId) { groupId name description groupType membershipPolicy status }"
			"}");
	}

	FString TeamPolicyQuery()
	{
		return TEXT(
			"query StudioTeamPolicy($appId: BigInt!) {"
			"  teamPolicy(appId: $appId) { appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser }"
			"}");
	}

	FString SetTeamPolicyMutation()
	{
		return TEXT(
			"mutation StudioSetTeamPolicy($appId: BigInt!, $creationPolicy: String!, $defaultMembershipPolicy: String!, $maxMembers: Int, $maxGroupsPerUser: Int) {"
			"  setTeamPolicy(input: { appId: $appId, creationPolicy: $creationPolicy, defaultMembershipPolicy: $defaultMembershipPolicy, maxMembers: $maxMembers, maxGroupsPerUser: $maxGroupsPerUser }) {"
			"    appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser"
			"  }"
			"}");
	}

	FString ChannelsQuery()
	{
		return TEXT(
			"query StudioChannels($appId: BigInt!) {"
			"  channels(appId: $appId) { groupId name description groupType membershipPolicy status }"
			"}");
	}

	FString ChannelPolicyQuery()
	{
		return TEXT(
			"query StudioChannelPolicy($appId: BigInt!) {"
			"  channelPolicy(appId: $appId) { appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser }"
			"}");
	}

	FString SetChannelPolicyMutation()
	{
		return TEXT(
			"mutation StudioSetChannelPolicy($appId: BigInt!, $creationPolicy: String!, $defaultMembershipPolicy: String!, $maxMembers: Int, $maxGroupsPerUser: Int) {"
			"  setChannelPolicy(input: { appId: $appId, creationPolicy: $creationPolicy, defaultMembershipPolicy: $defaultMembershipPolicy, maxMembers: $maxMembers, maxGroupsPerUser: $maxGroupsPerUser }) {"
			"    appId groupType creationPolicy defaultMembershipPolicy maxMembers maxGroupsPerUser"
			"  }"
			"}");
	}

	FString CreateChannelMutation()
	{
		return TEXT(
			"mutation StudioCreateChannel($appId: BigInt!, $name: String!, $description: String, $membersCanSend: Boolean, $membershipPolicy: String) {"
			"  createChannel(input: { appId: $appId, name: $name, description: $description, membersCanSend: $membersCanSend, membershipPolicy: $membershipPolicy }) {"
			"    groupId name description groupType membershipPolicy status"
			"  }"
			"}");
	}

	FString CreateTeamMutation()
	{
		return TEXT(
			"mutation StudioCreateTeam($appId: BigInt!, $name: String!, $description: String, $membershipPolicy: String) {"
			"  createTeam(input: { appId: $appId, name: $name, description: $description, membershipPolicy: $membershipPolicy }) {"
			"    groupId name description groupType membershipPolicy status"
			"  }"
			"}");
	}

	FString GroupMembersQuery(bool bChannel)
	{
		return bChannel
			? TEXT(
				"query StudioChannelMembers($groupId: BigInt!) {"
				"  channelMembers(groupId: $groupId) { groupMemberId userId status roles { groupRoleId roleName } }"
				"}")
			: TEXT(
				"query StudioTeamMembers($groupId: BigInt!) {"
				"  teamMembers(groupId: $groupId) { groupMemberId userId status roles { groupRoleId roleName } }"
				"}");
	}

	FString GroupRolesQuery(bool bChannel)
	{
		return bChannel
			? TEXT(
				"query StudioChannelRoles($groupId: BigInt!) {"
				"  channelRoles(groupId: $groupId) { groupRoleId roleName rank isSystem permissions }"
				"}")
			: TEXT(
				"query StudioTeamRoles($groupId: BigInt!) {"
				"  teamRoles(groupId: $groupId) { groupRoleId roleName rank isSystem permissions }"
				"}");
	}

	FString AddGroupMemberMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioAddChannelMember($groupId: BigInt!, $userId: BigInt!) {"
				"  addChannelMember(groupId: $groupId, userId: $userId) { groupMemberId userId status }"
				"}")
			: TEXT(
				"mutation StudioAddTeamMember($groupId: BigInt!, $userId: BigInt!) {"
				"  addTeamMember(groupId: $groupId, userId: $userId) { groupMemberId userId status }"
				"}");
	}

	FString RemoveGroupMemberMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioRemoveChannelMember($groupId: BigInt!, $userId: BigInt!) {"
				"  removeChannelMember(groupId: $groupId, userId: $userId)"
				"}")
			: TEXT(
				"mutation StudioRemoveTeamMember($groupId: BigInt!, $userId: BigInt!) {"
				"  removeTeamMember(groupId: $groupId, userId: $userId)"
				"}");
	}

	FString SetGroupMemberRolesMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioSetChannelMemberRoles($input: SetMemberRolesInput!) {"
				"  setChannelMemberRoles(input: $input) { groupMemberId userId status roles { groupRoleId roleName } }"
				"}")
			: TEXT(
				"mutation StudioSetTeamMemberRoles($input: SetMemberRolesInput!) {"
				"  setTeamMemberRoles(input: $input) { groupMemberId userId status roles { groupRoleId roleName } }"
				"}");
	}

	FString CreateGroupRoleMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioCreateChannelRole($input: CreateGroupRoleInput!) {"
				"  createChannelRole(input: $input) { groupRoleId roleName rank isSystem permissions }"
				"}")
			: TEXT(
				"mutation StudioCreateTeamRole($input: CreateGroupRoleInput!) {"
				"  createTeamRole(input: $input) { groupRoleId roleName rank isSystem permissions }"
				"}");
	}

	FString UpdateGroupRoleMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioUpdateChannelRole($input: UpdateGroupRoleInput!) {"
				"  updateChannelRole(input: $input) { groupRoleId roleName rank isSystem permissions }"
				"}")
			: TEXT(
				"mutation StudioUpdateTeamRole($input: UpdateGroupRoleInput!) {"
				"  updateTeamRole(input: $input) { groupRoleId roleName rank isSystem permissions }"
				"}");
	}

	FString DeleteGroupRoleMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioDeleteChannelRole($groupRoleId: BigInt!) {"
				"  deleteChannelRole(groupRoleId: $groupRoleId)"
				"}")
			: TEXT(
				"mutation StudioDeleteTeamRole($groupRoleId: BigInt!) {"
				"  deleteTeamRole(groupRoleId: $groupRoleId)"
				"}");
	}

	FString DeleteGroupMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioDeleteChannel($groupId: BigInt!) {"
				"  deleteChannel(groupId: $groupId)"
				"}")
			: TEXT(
				"mutation StudioDeleteTeam($groupId: BigInt!) {"
				"  deleteTeam(groupId: $groupId)"
				"}");
	}

	FString UpdateGroupMutation(bool bChannel)
	{
		return bChannel
			? TEXT(
				"mutation StudioUpdateChannel($input: UpdateChannelInput!) {"
				"  updateChannel(input: $input) { groupId name description membershipPolicy }"
				"}")
			: TEXT(
				"mutation StudioUpdateTeam($input: UpdateTeamInput!) {"
				"  updateTeam(input: $input) { groupId name description membershipPolicy }"
				"}");
	}


	bool ParseGroupPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGroupPolicy& OutPolicy)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(OpName, Node) || !Node->IsValid())
		{
			return false;
		}

		ReadGroupPolicy(*Node, OutPolicy);
		return true;
	}

	void ParseGroups(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                 TArray<TSharedPtr<FStudioGroup>>& OutGroups)
	{
		OutGroups.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioGroup> Group = MakeShared<FStudioGroup>();
				ReadGroup(*Node, *Group);
				OutGroups.Add(Group);
			}
		}
	}

	void ParseGroupMembers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioGroupMember>>& OutMembers)
	{
		OutMembers.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioGroupMember> Member = MakeShared<FStudioGroupMember>();
			Member->GroupMemberId = ReadId(*Node, TEXT("groupMemberId"));
			Member->UserId = ReadId(*Node, TEXT("userId"));
			(*Node)->TryGetStringField(TEXT("status"), Member->Status);

			const TArray<TSharedPtr<FJsonValue>>* Roles = nullptr;
			if ((*Node)->TryGetArrayField(TEXT("roles"), Roles))
			{
				for (const TSharedPtr<FJsonValue>& RoleEntry : *Roles)
				{
					const TSharedPtr<FJsonObject>* RoleNode = nullptr;
					if (RoleEntry->TryGetObject(RoleNode) && RoleNode->IsValid())
					{
						FString RoleName;
						(*RoleNode)->TryGetStringField(TEXT("roleName"), RoleName);
						Member->RoleNames.Add(RoleName);
						Member->RoleIds.Add(ReadId(*RoleNode, TEXT("groupRoleId")));
					}
				}
			}

			OutMembers.Add(Member);
		}
	}

	void ParseGroupRoles(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioGroupRole>>& OutRoles)
	{
		OutRoles.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioGroupRole> Role = MakeShared<FStudioGroupRole>();
			Role->GroupRoleId = ReadId(*Node, TEXT("groupRoleId"));
			(*Node)->TryGetStringField(TEXT("roleName"), Role->RoleName);
			(*Node)->TryGetBoolField(TEXT("isSystem"), Role->bIsSystem);
			int32 Rank = 0;
			if ((*Node)->TryGetNumberField(TEXT("rank"), Rank))
			{
				Role->Rank = Rank;
			}
			ReadStringArray(*Node, TEXT("permissions"), Role->Permissions);
			OutRoles.Add(Role);
		}
	}

	// Spatial grid (game plane)

	FString NearbyGridPermissionsQuery()
	{
		return TEXT(
			"query StudioNearbyGrids($input: NearbyGridPermissionsInput!) {"
			"  nearbyGridPermissions(input: $input) {"
			"    appId gridId userId lowChunk { x y z } highChunk { x y z } permissionKeys"
			"  }"
			"}");
	}

	FString GridPermissionLimitsQuery()
	{
		return TEXT(
			"query StudioGridLimits($appId: BigInt!, $gridId: BigInt!) {"
			"  gridPermissionLimits(appId: $appId, gridId: $gridId) { appId gridId permissionKeys }"
			"}");
	}

	FString GridGroupGrantsQuery()
	{
		return TEXT(
			"query StudioGridGroupGrants($appId: BigInt!, $gridId: BigInt!, $groupId: BigInt!) {"
			"  gridGroupGrants(appId: $appId, gridId: $gridId, groupId: $groupId) {"
			"    appId gridId groupId groupRoleId permissionKey expiresAt"
			"  }"
			"}");
	}

	FString GridUserPermissionsQuery()
	{
		return TEXT(
			"query StudioGridUserPerms($appId: BigInt!, $gridId: BigInt!, $userId: BigInt!) {"
			"  gridUserPermissions(appId: $appId, gridId: $gridId, userId: $userId) { appId gridId userId permissionKeys }"
			"}");
	}

	FString CreateGridMutation()
	{
		return TEXT(
			"mutation StudioCreateGrid($input: CreateGridInput!) {"
			"  createGrid(input: $input) {"
			"    grid { grid_id app_id low_chunk { x y z } high_chunk { x y z } } error"
			"  }"
			"}");
	}

	FString GrantGridPermissionsMutation()
	{
		return TEXT(
			"mutation StudioGrantGrid($input: GrantGridPermissionsInput!) {"
			"  grantGridPermissions(input: $input) { appId gridId userId permissionKeys }"
			"}");
	}

	FString RevokeGridPermissionsMutation()
	{
		return TEXT(
			"mutation StudioRevokeGrid($input: RevokeGridPermissionsInput!) {"
			"  revokeGridPermissions(input: $input) { appId gridId userId permissionKeys }"
			"}");
	}

	FString SetGridPermissionLimitsMutation()
	{
		return TEXT(
			"mutation StudioSetGridLimits($input: SetGridPermissionLimitsInput!) {"
			"  setGridPermissionLimits(input: $input) { appId gridId permissionKeys }"
			"}");
	}

	FString AssignGroupToGridMutation()
	{
		return TEXT(
			"mutation StudioAssignGroupToGrid($input: AssignGroupToGridInput!) {"
			"  assignGroupToGrid(input: $input) { appId gridId groupId groupRoleId permissionKey expiresAt }"
			"}");
	}

	FString RevokeGroupFromGridMutation()
	{
		return TEXT(
			"mutation StudioRevokeGroupFromGrid($input: RevokeGroupFromGridInput!) {"
			"  revokeGroupFromGrid(input: $input) { appId gridId groupId groupRoleId permissionKey expiresAt }"
			"}");
	}

	FString RuntimePermissionsQuery()
	{
		// PUBLIC and global: no app id, no arguments. Returns the flat list of valid permission keys
		// ordered by bit index. Lives on the management plane even though grids are a game-plane concept.
		return TEXT(
			"query StudioRuntimePermissions {"
			"  runtimePermissions"
			"}");
	}

	void ParseNearbyGrids(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioGrid>>& OutGrids)
	{
		OutGrids.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("nearbyGridPermissions"), Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioGrid> Grid = MakeShared<FStudioGrid>();
				Grid->AppId = ReadId(*Node, TEXT("appId"));
				Grid->GridId = ReadId(*Node, TEXT("gridId"));
				ReadChunkField(*Node, TEXT("lowChunk"), Grid->Low);
				ReadChunkField(*Node, TEXT("highChunk"), Grid->High);
				ReadStringArray(*Node, TEXT("permissionKeys"), Grid->EffectivePermissionKeys);
				OutGrids.Add(Grid);
			}
		}
	}

	bool ParseCreateGrid(const TSharedPtr<FJsonObject>& Envelope, FStudioGrid& OutGrid, FString& OutError)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, TEXT("createGrid"));
		if (!Node.IsValid())
		{
			return false;
		}

		Node->TryGetStringField(TEXT("error"), OutError);

		const TSharedPtr<FJsonObject>* GridNode = nullptr;
		if (Node->TryGetObjectField(TEXT("grid"), GridNode) && GridNode->IsValid())
		{
			OutGrid.GridId = ReadId(*GridNode, TEXT("grid_id"));
			OutGrid.AppId = ReadId(*GridNode, TEXT("app_id"));
			ReadChunkField(*GridNode, TEXT("low_chunk"), OutGrid.Low);
			ReadChunkField(*GridNode, TEXT("high_chunk"), OutGrid.High);
			return true;
		}
		return false;
	}

	void ParseGridPermissionKeys(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, TArray<FString>& OutKeys)
	{
		OutKeys.Reset();
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (Node.IsValid())
		{
			ReadStringArray(Node, TEXT("permissionKeys"), OutKeys);
		}
	}

	void ParseGridGroupGrants(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                          TArray<TSharedPtr<FStudioGridGroupGrant>>& OutGrants)
	{
		OutGrants.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioGridGroupGrant> Grant = MakeShared<FStudioGridGroupGrant>();
				ReadGridGroupGrant(*Node, *Grant);
				OutGrants.Add(Grant);
			}
		}
	}

	void ParseRuntimePermissions(const TSharedPtr<FJsonObject>& Envelope, TArray<FString>& OutKeys)
	{
		// runtimePermissions is a flat [String!]! straight off the data node, not wrapped in an op object.
		OutKeys.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (Data.IsValid())
		{
			ReadStringArray(Data, TEXT("runtimePermissions"), OutKeys);
		}
	}

	// Game model (game plane)

	FString ContainerTypesQuery()
	{
		return TEXT(
			"query StudioContainerTypes($appId: BigInt!) {"
			"  gameModelContainerTypes(appId: $appId) {"
			"    appId typeName displayName description instantiableBy defaultPropertyVisibility metadataJson"
			"  }"
			"}");
	}

	FString PropertyDefsQuery()
	{
		return TEXT(
			"query StudioPropertyDefs($appId: BigInt!, $typeName: String!) {"
			"  gameModelPropertyDefs(appId: $appId, typeName: $typeName) {"
			"    containerTypeName key valueType defaultValueJson visibility writable description"
			"  }"
			"}");
	}

	FString FunctionsQuery()
	{
		return TEXT(
			"query StudioFunctions($appId: BigInt!, $containerTypeName: String) {"
			"  gameModelFunctions(appId: $appId, containerTypeName: $containerTypeName) {"
			"    functionId name containerTypeName description returnType returnExpression invokeScope invokePolicyJson"
			"    parameters { name valueType required defaultValueJson description sortOrder }"
			"    mutations { target property expression }"
			"    warnings"
			"  }"
			"}");
	}

	FString FeaturesQuery()
	{
		return TEXT(
			"query StudioFeatures($appId: BigInt!) {"
			"  gameModelFeatures(appId: $appId) { appId featureKey description }"
			"}");
	}

	FString TierFeaturesQuery()
	{
		return TEXT(
			"query StudioTierFeatures($appId: BigInt!) {"
			"  gameModelTierFeatures(appId: $appId) { appId tierId featureKey }"
			"}");
	}

	FString AppAccessTiersQuery()
	{
		return TEXT(
			"query StudioAppAccessTiers($appId: BigInt!) {"
			"  appAccessTiers(appId: $appId) { tierId name isFree isDefault permissionKeys status }"
			"}");
	}

	FString GameModelPolicyQuery()
	{
		return TEXT(
			"query StudioGameModelPolicy($appId: BigInt!) {"
			"  gameModelPolicy(appId: $appId) { appId sessionCreationPolicy defaultParticipantRole }"
			"}");
	}

	FString ContainersQuery()
	{
		return TEXT(
			"query StudioGmContainers($appId: BigInt!, $typeName: String, $sessionId: String) {"
			"  gameModelContainers(appId: $appId, typeName: $typeName, sessionId: $sessionId) {"
			"    containerId sessionId typeName displayName ownerUserId"
			"  }"
			"}");
	}

	FString ContainerStateQuery()
	{
		return TEXT(
			"query StudioGmContainerState($appId: BigInt!, $containerId: String!) {"
			"  gameModelContainerState(appId: $appId, containerId: $containerId) {"
			"    containerId typeName displayName ownerUserId propertiesJson"
			"  }"
			"}");
	}

	FString UpsertContainerTypeMutation()
	{
		return TEXT(
			"mutation StudioUpsertContainerType($input: UpsertContainerTypeInput!) {"
			"  gameModelUpsertContainerType(input: $input) {"
			"    appId typeName displayName description instantiableBy defaultPropertyVisibility metadataJson"
			"  }"
			"}");
	}

	FString UpsertPropertyDefMutation()
	{
		return TEXT(
			"mutation StudioUpsertPropertyDef($input: UpsertPropertyDefInput!) {"
			"  gameModelUpsertPropertyDef(input: $input) {"
			"    containerTypeName key valueType defaultValueJson visibility writable description"
			"  }"
			"}");
	}

	FString UpsertFunctionMutation()
	{
		return TEXT(
			"mutation StudioUpsertFunction($input: UpsertFunctionInput!) {"
			"  gameModelUpsertFunction(input: $input) {"
			"    functionId name containerTypeName description returnType returnExpression invokeScope invokePolicyJson"
			"    parameters { name valueType required defaultValueJson description sortOrder }"
			"    mutations { target property expression }"
			"    warnings"
			"  }"
			"}");
	}

	FString DeleteFunctionMutation()
	{
		return TEXT(
			"mutation StudioDeleteFunction($appId: BigInt!, $name: String!) {"
			"  gameModelDeleteFunction(appId: $appId, name: $name)"
			"}");
	}

	FString DefineFeatureMutation()
	{
		return TEXT(
			"mutation StudioDefineFeature($input: DefineAppFeatureInput!) {"
			"  gameModelDefineFeature(input: $input) { appId featureKey description }"
			"}");
	}

	FString GrantTierFeatureMutation()
	{
		return TEXT(
			"mutation StudioGrantTierFeature($input: GrantTierFeatureInput!) {"
			"  gameModelGrantTierFeature(input: $input) { appId tierId featureKey }"
			"}");
	}

	FString RevokeTierFeatureMutation()
	{
		return TEXT(
			"mutation StudioRevokeTierFeature($input: GrantTierFeatureInput!) {"
			"  gameModelRevokeTierFeature(input: $input)"
			"}");
	}

	FString SetGameModelPolicyMutation()
	{
		return TEXT(
			"mutation StudioSetGameModelPolicy($input: SetGameModelPolicyInput!) {"
			"  gameModelSetPolicy(input: $input) { appId sessionCreationPolicy defaultParticipantRole }"
			"}");
	}

	FString SeedGameModelMutation()
	{
		return TEXT(
			"mutation StudioSeedGameModel($input: SeedGameModelInput!) {"
			"  gameModelSeed(input: $input) {"
			"    containerTypesCreated propertyDefinitionsCreated functionsCreated containersCreated edgesCreated warnings"
			"  }"
			"}");
	}

	void ParseContainerTypes(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                         TArray<TSharedPtr<FStudioContainerType>>& OutTypes)
	{
		OutTypes.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
				ReadContainerType(*Node, *Type);
				OutTypes.Add(Type);
			}
		}
	}

	void ParsePropertyDefs(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioPropertyDef>>& OutDefs)
	{
		OutDefs.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
				ReadPropertyDef(*Node, *Def);
				OutDefs.Add(Def);
			}
		}
	}

	void ParseFunctions(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                    TArray<TSharedPtr<FStudioFunction>>& OutFns)
	{
		OutFns.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioFunction> Fn = MakeShared<FStudioFunction>();
				ReadFunction(*Node, *Fn);
				OutFns.Add(Fn);
			}
		}
	}

	bool ParseFunction(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioFunction& OutFn)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		ReadFunction(Node, OutFn);
		return true;
	}

	void ParseFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                   TArray<TSharedPtr<FStudioAppFeature>>& OutFeatures)
	{
		OutFeatures.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioAppFeature> Feature = MakeShared<FStudioAppFeature>();
				ReadFeature(*Node, *Feature);
				OutFeatures.Add(Feature);
			}
		}
	}

	void ParseTierFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioTierFeature>>& OutGrants)
	{
		OutGrants.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioTierFeature> Grant = MakeShared<FStudioTierFeature>();
				ReadTierFeature(*Node, *Grant);
				OutGrants.Add(Grant);
			}
		}
	}

	void ParseAccessTiers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<TSharedPtr<FStudioAccessTier>>& OutTiers)
	{
		OutTiers.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioAccessTier> Tier = MakeShared<FStudioAccessTier>();
				Tier->TierId = ReadId(*Node, TEXT("tierId"));
				(*Node)->TryGetStringField(TEXT("name"), Tier->Name);
				(*Node)->TryGetBoolField(TEXT("isFree"), Tier->bIsFree);
				(*Node)->TryGetBoolField(TEXT("isDefault"), Tier->bIsDefault);
				ReadStringArray(*Node, TEXT("permissionKeys"), Tier->PermissionKeys);
				(*Node)->TryGetStringField(TEXT("status"), Tier->Status);
				OutTiers.Add(Tier);
			}
		}
	}

	bool ParseGameModelPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGameModelPolicy& OutPolicy)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		OutPolicy.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetStringField(TEXT("sessionCreationPolicy"), OutPolicy.SessionCreationPolicy);
		Node->TryGetStringField(TEXT("defaultParticipantRole"), OutPolicy.DefaultParticipantRole);
		OutPolicy.bValid = true;
		return true;
	}

	bool ParseSeedResult(const TSharedPtr<FJsonObject>& Envelope, FString& OutSummary)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, TEXT("gameModelSeed"));
		if (!Node.IsValid())
		{
			return false;
		}

		int32 Types = 0, Props = 0, Fns = 0, Containers = 0, Edges = 0;
		Node->TryGetNumberField(TEXT("containerTypesCreated"), Types);
		Node->TryGetNumberField(TEXT("propertyDefinitionsCreated"), Props);
		Node->TryGetNumberField(TEXT("functionsCreated"), Fns);
		Node->TryGetNumberField(TEXT("containersCreated"), Containers);
		Node->TryGetNumberField(TEXT("edgesCreated"), Edges);
		OutSummary = FString::Printf(
			TEXT("Seeded %d type(s), %d property def(s), %d function(s), %d container(s), %d edge(s)."),
			Types, Props, Fns, Containers, Edges);
		return true;
	}

	void ParseContainers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioContainer>>& OutContainers)
	{
		OutContainers.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioContainer> Container = MakeShared<FStudioContainer>();
			(*Node)->TryGetStringField(TEXT("containerId"), Container->ContainerId);
			(*Node)->TryGetStringField(TEXT("sessionId"), Container->SessionId);
			(*Node)->TryGetStringField(TEXT("typeName"), Container->TypeName);
			(*Node)->TryGetStringField(TEXT("displayName"), Container->DisplayName);
			Container->OwnerUserId = ReadId(*Node, TEXT("ownerUserId"));
			OutContainers.Add(Container);
		}
	}

	bool ParseContainerState(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioContainerState& OutState)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		Node->TryGetStringField(TEXT("containerId"), OutState.ContainerId);
		Node->TryGetStringField(TEXT("typeName"), OutState.TypeName);
		Node->TryGetStringField(TEXT("displayName"), OutState.DisplayName);
		OutState.OwnerUserId = ReadId(Node, TEXT("ownerUserId"));
		Node->TryGetStringField(TEXT("propertiesJson"), OutState.PropertiesJson);
		OutState.bValid = true;
		return true;
	}
}
