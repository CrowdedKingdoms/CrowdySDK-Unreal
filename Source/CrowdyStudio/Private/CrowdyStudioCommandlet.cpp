#include "CrowdyStudioCommandlet.h"

#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "Async/TaskGraphInterfaces.h"
#include "ConfigSync/FCrowdyConfigSync.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "CrowdyStudioModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Gql/CrowdyStudioQueries.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Model/FCrowdyStudioController.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CrowdyStudioCommandletDetail
{
	const TCHAR* const Usage = TEXT("-run=CrowdyStudio -op=apps (the account's organizations and their apps, the datacenters an app can be placed in, and the project's Network settings)")
		TEXT(" | createapp -org=<id> -name=<text> -slug=<text> -datacenter=<code from apps> [-description=<text>] -yes (the datacenter is permanent)")
		TEXT(" | sync -app=<id> -yes [-editoropen] (writes the app's id, organization and game endpoints into Config/DefaultGame.ini; refused while an Unreal Editor is running unless -editoropen)")
		TEXT(" | features [-app=<id>] (the app's feature keys and each access tier with the features it grants)")
		TEXT(" | definefeature [-app=<id>] -key=<key> [-description=<text>] -yes")
		TEXT(" | grantfeature [-app=<id>] -tier=<id from features> -key=<key> -yes | revokefeature [-app=<id>] -tier=<id> -key=<key> -yes")
		TEXT(" | teams [-app=<id>] (the app's teams and team policy) | createteam [-app=<id>] -name=<text> [-description=<text>] [-policy=open|request|invite|admin, default open] -yes")
		TEXT(" | channels [-app=<id>] (the app's channels and channel policy) | createchannel [-app=<id>] -name=<text> [-description=<text>] [-policy=open|request|invite|admin, default the app's] [-memberscansend=true|false, default true] -yes")
		TEXT(" | grids [-app=<id>] -low=\"x,y,z\" -high=\"x,y,z\" [-user=<id>, default the signed-in user] (the grids in that chunk region and the permission keys a grid can grant)")
		TEXT(" | creategrid [-app=<id>] -corner1=\"x,y,z\" -corner2=\"x,y,z\" -yes")
		TEXT(" | grantgrid | revokegrid [-app=<id>] -grid=<id> -user=<id> or -group=<id> [-role=<id>] -keys=\"a,b\" (revokegrid also takes -all in place of -keys) -yes")
		TEXT(" | groupdetail [-app=<id>] -kind=team|channel -group=<id> (the group's members and roles)")
		TEXT(" | addmember | removemember [-app=<id>] -kind=team|channel -group=<id> -user=<id> -yes")
		TEXT(" | setroles [-app=<id>] -kind=team|channel -group=<id> -user=<id> -roles=\"1,2\" or -noroles -yes")
		TEXT(" | createrole [-app=<id>] -kind=team|channel -group=<id> -name=<text> [-perms=\"manage_members,manage_roles,manage_group,send_messages\"] [-rank=<number>] -yes")
		TEXT(" | updaterole [-app=<id>] -kind=team|channel -group=<id> -role=<id> [-name=<text>] [-perms=\"a,b\" or -noperms] [-rank=<number>] -yes | deleterole [-app=<id>] -kind=team|channel -group=<id> -role=<id> -yes")
		TEXT(" | updategroup [-app=<id>] -kind=team|channel -group=<id> [-name=<text>] [-description=<text>] [-policy=open|request|invite|admin] -yes | deletegroup [-app=<id>] -kind=team|channel -group=<id> -yes")
		TEXT(" | setteampolicy | setchannelpolicy [-app=<id>] [-creation=admin|member|anyone] [-membership=open|request|invite|admin] [-maxmembers=<n, 0 unlimited>] [-maxgroupsperuser=<n, 0 unlimited>] -yes")
		TEXT(" | gridlimits [-app=<id>] -grid=<id> -keys=\"a,b\" or -nolimit -yes | gridgrants [-app=<id>] -grid=<id> -group=<id> | gridpermissions [-app=<id>] -grid=<id> [-user=<id>, default the signed-in user]")
		TEXT(", -app defaulting to the project's synced AppID, and -out=<file> for a JSON result. Quote a value with spaces or commas. Every operation that changes something does nothing without -yes.");

	const TCHAR* const SignInMessage = TEXT("Sign in to Crowdy Studio on this machine first");

	constexpr double ReadTimeoutSeconds = 60.0;
	constexpr double CreateTimeoutSeconds = 180.0;

	struct FRun
	{
		FCrowdyStudioCommandRequest Request;
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TSharedPtr<FCrowdyStudioController> Controller;
		FString Error;
		/** The controller's latest error status since the last reset. */
		FString StatusError;
		int64 CreatedAppId = 0;
		bool bCreateAnswered = false;
		int64 CreatedGridId = 0;
		int64 SignedInUserId = 0;
	};

	void Step(const FString& Line)
	{
		UE_LOG(LogCrowdyStudio, Display, TEXT("Crowdy Studio: %s"), *Line);
	}

	FString IdText(int64 Id)
	{
		return FString::Printf(TEXT("%lld"), Id);
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Items)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Item : Items)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return Values;
	}

	void SetSettings(FJsonObject& Out, const FStudioSettingsSnapshot& Settings)
	{
		Out.SetStringField(TEXT("appId"), IdText(Settings.AppId));
		Out.SetStringField(TEXT("orgId"), IdText(Settings.OrgId));
		Out.SetStringField(TEXT("discoveryUrl"), Settings.DiscoveryUrl);
		Out.SetStringField(TEXT("gameApiHttpUrl"), Settings.GameApiHttpUrl);
		Out.SetStringField(TEXT("gameApiWsUrl"), Settings.GameApiWsUrl);
	}

	FString SettingsLine(const FStudioSettingsSnapshot& Settings)
	{
		return FString::Printf(TEXT("AppID=%lld OrgId=%lld DiscoveryUrl=%s GameApiHttpUrl=%s GameApiWsUrl=%s"), Settings.AppId, Settings.OrgId,
			*Settings.DiscoveryUrl, *Settings.GameApiHttpUrl, *Settings.GameApiWsUrl);
	}

	bool ReadSwitch(const FString& Params, const TCHAR* Name, FString& OutValue)
	{
		OutValue.Reset();
		FParse::Value(*Params, *FString::Printf(TEXT("-%s="), Name), OutValue);
		OutValue.TrimStartAndEndInline();
		return !OutValue.IsEmpty();
	}

	bool IsDigits(const FString& Text)
	{
		for (const TCHAR C : Text)
		{
			if (!FChar::IsDigit(C))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadId(const FString& Params, const TCHAR* Name, int64& OutId, FString& OutError, const TCHAR* Source = TEXT("-op=apps"))
	{
		FString Text;
		if (!ReadSwitch(Params, Name, Text))
		{
			OutError = FString::Printf(TEXT("-%s=<id> is required, the number shown by %s"), Name, Source);
			return false;
		}
		if (Text.Len() > 18 || !IsDigits(Text) || FCString::Atoi64(*Text) <= 0)
		{
			OutError = FString::Printf(TEXT("-%s=%s is not an id; it is the positive number shown by %s"), Name, *Text, Source);
			return false;
		}
		OutId = FCString::Atoi64(*Text);
		return true;
	}

	bool ReadOptionalId(const FString& Params, const TCHAR* Name, int64& OutId, FString& OutError, const TCHAR* Source)
	{
		FString Text;
		OutId = 0;
		return !ReadSwitch(Params, Name, Text) || ReadId(Params, Name, OutId, OutError, Source);
	}

	/** Three whole numbers; the list switches need quotes because an unquoted value stops at its first comma. */
	bool ReadChunk(const FString& Params, const TCHAR* Name, FStudioChunk& OutChunk, FString& OutError)
	{
		FString Text;
		if (!ReadSwitch(Params, Name, Text))
		{
			OutError = FString::Printf(TEXT("-%s=\"x,y,z\" is required, three chunk coordinates"), Name);
			return false;
		}
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), false);
		int64 Axes[3] = {};
		bool bValid = Parts.Num() == 3;
		for (int32 Index = 0; bValid && Index < 3; ++Index)
		{
			const FString Axis = Parts[Index].TrimStartAndEnd();
			const FString Digits = Axis.StartsWith(TEXT("-")) ? Axis.Mid(1) : Axis;
			bValid = !Digits.IsEmpty() && Digits.Len() <= 18 && IsDigits(Digits);
			Axes[Index] = bValid ? FCString::Atoi64(*Axis) : 0;
		}
		if (!bValid)
		{
			OutError = FString::Printf(TEXT("-%s=%s is not three chunk coordinates; write -%s=\"x,y,z\" with whole numbers, in quotes"), Name, *Text, Name);
			return false;
		}
		OutChunk.X = Axes[0];
		OutChunk.Y = Axes[1];
		OutChunk.Z = Axes[2];
		return true;
	}

	/** True when the switch's value was cut at an unquoted comma, so the rest of the list would be dropped silently. */
	bool IsCutAtComma(const FString& Params, const TCHAR* Name, const FString& Value)
	{
		const FString Match = FString::Printf(TEXT("-%s="), Name);
		const int32 At = Params.Find(Match, ESearchCase::IgnoreCase);
		if (At == INDEX_NONE)
		{
			return false;
		}
		const int32 ValueAt = At + Match.Len();
		return Params.IsValidIndex(ValueAt + Value.Len()) && Params[ValueAt] != TEXT('"') && Params[ValueAt + Value.Len()] == TEXT(',');
	}

	/** A comma list, trimmed and without repeats; empty when the switch is absent. */
	bool ReadList(const FString& Params, const TCHAR* Name, TArray<FString>& OutItems, FString& OutError)
	{
		FString Text;
		OutItems.Reset();
		if (!ReadSwitch(Params, Name, Text))
		{
			return true;
		}
		if (IsCutAtComma(Params, Name, Text))
		{
			OutError = FString::Printf(TEXT("-%s=%s... was cut at a comma; write -%s=\"a,b\" in quotes"), Name, *Text, Name);
			return false;
		}
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","));
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (Part.Len() > 64)
			{
				OutError = FString::Printf(TEXT("-%s entries are at most 64 characters"), Name);
				return false;
			}
			if (!Part.IsEmpty())
			{
				OutItems.AddUnique(Part);
			}
		}
		return true;
	}

	bool ReadKeys(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		return ReadList(Params, TEXT("keys"), Out.Keys, OutError);
	}

	/** A whole number from min to 999999999; OutGiven says whether the switch was there. */
	bool ReadCount(const FString& Params, const TCHAR* Name, bool bAllowNegative, int32& OutValue, bool& OutGiven, FString& OutError)
	{
		FString Text;
		OutGiven = ReadSwitch(Params, Name, Text);
		if (!OutGiven)
		{
			return true;
		}
		const FString Digits = bAllowNegative && Text.StartsWith(TEXT("-")) ? Text.Mid(1) : Text;
		if (Digits.IsEmpty() || Digits.Len() > 9 || !IsDigits(Digits))
		{
			OutError = FString::Printf(TEXT("-%s=%s is not a whole number%s"), Name, *Text, bAllowNegative ? TEXT("") : TEXT(" of 0 or more"));
			return false;
		}
		OutValue = FCString::Atoi(*Text);
		return true;
	}

	bool IsGroupPermission(const FString& Key)
	{
		static const TCHAR* const Keys[] = { TEXT("manage_members"), TEXT("manage_roles"), TEXT("manage_group"), TEXT("send_messages") };
		return Algo::AnyOf(Keys, [&Key](const TCHAR* Name) { return Key.Equals(Name, ESearchCase::CaseSensitive); });
	}

	bool IsCreationPolicy(const FString& Policy)
	{
		static const TCHAR* const Policies[] = { TEXT("admin"), TEXT("member"), TEXT("anyone") };
		return Algo::AnyOf(Policies, [&Policy](const TCHAR* Name) { return Policy.Equals(Name, ESearchCase::CaseSensitive); });
	}

	bool ReadKind(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		FString Kind;
		if (!ReadSwitch(Params, TEXT("kind"), Kind))
		{
			OutError = TEXT("-kind=team or -kind=channel is required: which kind of group -group names");
			return false;
		}
		Out.bChannel = Kind.Equals(TEXT("channel"), ESearchCase::IgnoreCase);
		if (!Out.bChannel && !Kind.Equals(TEXT("team"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("-kind=%s is not team or channel"), *Kind);
			return false;
		}
		return true;
	}

	bool ReadMembershipPolicy(const FString& Params, FCrowdyStudioCommandRequest& Out, const TCHAR* Name, FString& OutError)
	{
		ReadSwitch(Params, Name, Out.Policy);
		Out.Policy.ToLowerInline();
		if (!Out.Policy.IsEmpty() && !UCrowdyStudioCommandlet::IsMembershipPolicy(Out.Policy))
		{
			OutError = FString::Printf(TEXT("-%s=%s is not a membership policy; use open, request, invite or admin"), Name, *Out.Policy);
			return false;
		}
		return true;
	}

	bool ReadGroupPermissions(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		if (!ReadList(Params, TEXT("perms"), Out.Perms, OutError))
		{
			return false;
		}
		for (const FString& Perm : Out.Perms)
		{
			if (!IsGroupPermission(Perm))
			{
				OutError = FString::Printf(TEXT("-perms has %s, which is not a group permission; use manage_members, manage_roles, manage_group or send_messages"), *Perm);
				return false;
			}
		}
		return true;
	}

	bool ReadRoleIds(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		TArray<FString> Items;
		Out.RoleIds.Reset();
		Out.bNoRoles = FParse::Param(*Params, TEXT("noroles"));
		if (!ReadList(Params, TEXT("roles"), Items, OutError))
		{
			return false;
		}
		if (Items.IsEmpty() == !Out.bNoRoles)
		{
			OutError = Out.bNoRoles ? TEXT("give -roles=\"1,2\" or -noroles, not both")
				: TEXT("-roles=\"1,2\" or -noroles is required; -op=groupdetail lists the role ids, -noroles takes every role away");
			return false;
		}
		for (const FString& Item : Items)
		{
			if (Item.Len() > 18 || !IsDigits(Item) || FCString::Atoi64(*Item) <= 0)
			{
				OutError = FString::Printf(TEXT("-roles has %s, which is not a role id; -op=groupdetail lists them"), *Item);
				return false;
			}
			Out.RoleIds.AddUnique(FCString::Atoi64(*Item));
		}
		return true;
	}

	/** addmember, removemember, setroles, createrole, updaterole, deleterole, updategroup, deletegroup and groupdetail. */
	bool ParseGroupDetail(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		const FString& Op = Out.Op;
		if (!ReadKind(Params, Out, OutError) || !ReadId(Params, TEXT("group"), Out.GroupId, OutError, TEXT("-op=teams or -op=channels")))
		{
			return false;
		}
		if (Op == TEXT("groupdetail") || Op == TEXT("deletegroup"))
		{
			return true;
		}
		if (Op == TEXT("addmember") || Op == TEXT("removemember"))
		{
			return ReadId(Params, TEXT("user"), Out.UserId, OutError, TEXT("-op=groupdetail"));
		}
		if (Op == TEXT("setroles"))
		{
			return ReadId(Params, TEXT("user"), Out.UserId, OutError, TEXT("-op=groupdetail")) && ReadRoleIds(Params, Out, OutError);
		}
		if (Op == TEXT("updategroup"))
		{
			ReadSwitch(Params, TEXT("name"), Out.Name);
			ReadSwitch(Params, TEXT("description"), Out.Description);
			if (!ReadMembershipPolicy(Params, Out, TEXT("policy"), OutError))
			{
				return false;
			}
			if (Out.Name.IsEmpty() && Out.Description.IsEmpty() && Out.Policy.IsEmpty())
			{
				OutError = TEXT("give at least one of -name, -description or -policy to change; a switch left out stays as it is");
				return false;
			}
			return true;
		}
		const bool bCreate = Op == TEXT("createrole");
		bool bGivenRank = false;
		if (!bCreate && !ReadId(Params, TEXT("role"), Out.RoleId, OutError, TEXT("-op=groupdetail")))
		{
			return false;
		}
		ReadSwitch(Params, TEXT("name"), Out.Name);
		Out.bNoPerms = FParse::Param(*Params, TEXT("noperms"));
		if (!ReadGroupPermissions(Params, Out, OutError) || !ReadCount(Params, TEXT("rank"), true, Out.Rank, bGivenRank, OutError))
		{
			return false;
		}
		Out.bHasRank = bGivenRank;
		if (Out.bNoPerms && !Out.Perms.IsEmpty())
		{
			OutError = TEXT("give -perms=\"a,b\" or -noperms, not both");
			return false;
		}
		if (Op == TEXT("deleterole"))
		{
			return true;
		}
		if (bCreate && Out.Name.IsEmpty())
		{
			OutError = TEXT("-name=<text> is required, the new role's name");
			return false;
		}
		if (!bCreate && Out.Name.IsEmpty() && Out.Perms.IsEmpty() && !Out.bNoPerms && !Out.bHasRank)
		{
			OutError = TEXT("give at least one of -name, -perms (or -noperms) or -rank to change; a switch left out stays as it is");
			return false;
		}
		return true;
	}

	/** setteampolicy and setchannelpolicy: every switch left out keeps the app's current value. */
	bool ParseGroupPolicy(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		ReadSwitch(Params, TEXT("creation"), Out.Creation);
		Out.Creation.ToLowerInline();
		if (!Out.Creation.IsEmpty() && !IsCreationPolicy(Out.Creation))
		{
			OutError = FString::Printf(TEXT("-creation=%s is not a creation policy; use admin, member or anyone"), *Out.Creation);
			return false;
		}
		bool bMembers = false;
		bool bPerUser = false;
		if (!ReadMembershipPolicy(Params, Out, TEXT("membership"), OutError) || !ReadCount(Params, TEXT("maxmembers"), false, Out.MaxMembers, bMembers, OutError)
			|| !ReadCount(Params, TEXT("maxgroupsperuser"), false, Out.MaxGroupsPerUser, bPerUser, OutError))
		{
			return false;
		}
		if (!bMembers)
		{
			Out.MaxMembers = -1;
		}
		if (!bPerUser)
		{
			Out.MaxGroupsPerUser = -1;
		}
		if (Out.Creation.IsEmpty() && Out.Policy.IsEmpty() && !bMembers && !bPerUser)
		{
			OutError = TEXT("give at least one of -creation, -membership, -maxmembers or -maxgroupsperuser to change; 0 for a maximum means unlimited");
			return false;
		}
		return true;
	}

	/** gridlimits, gridgrants and gridpermissions. */
	bool ParseGridDetail(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		if (!ReadId(Params, TEXT("grid"), Out.GridId, OutError, TEXT("-op=grids")))
		{
			return false;
		}
		if (Out.Op == TEXT("gridgrants"))
		{
			return ReadId(Params, TEXT("group"), Out.GroupId, OutError, TEXT("-op=teams or -op=channels"));
		}
		if (Out.Op == TEXT("gridpermissions"))
		{
			return ReadOptionalId(Params, TEXT("user"), Out.UserId, OutError, TEXT("the user's account"));
		}
		Out.bAllKeys = FParse::Param(*Params, TEXT("nolimit"));
		if (!ReadKeys(Params, Out, OutError))
		{
			return false;
		}
		if (Out.bAllKeys && !Out.Keys.IsEmpty())
		{
			OutError = TEXT("give -keys=\"a,b\" or -nolimit, not both");
			return false;
		}
		if (!Out.bAllKeys && Out.Keys.IsEmpty())
		{
			OutError = TEXT("-keys=\"a,b\" or -nolimit is required: the only keys that can ever take effect on the grid, or no limit");
			return false;
		}
		return true;
	}

	bool ReadFeatureKey(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		if (!ReadSwitch(Params, TEXT("key"), Out.Key))
		{
			OutError = TEXT("-key=<key> is required, the feature key");
			return false;
		}
		if (!FCrowdyStudioController::IsFeatureKey(Out.Key))
		{
			OutError = TEXT("-key can only be 1 to 64 letters, digits, underscores, hyphens and dots");
			return false;
		}
		return true;
	}

	bool ParseGroupCreate(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		if (!ReadSwitch(Params, TEXT("name"), Out.Name))
		{
			OutError = TEXT("-name=<text> is required, the name of the new team or channel");
			return false;
		}
		ReadSwitch(Params, TEXT("description"), Out.Description);
		ReadSwitch(Params, TEXT("policy"), Out.Policy);
		Out.Policy.ToLowerInline();
		if (!Out.Policy.IsEmpty() && !UCrowdyStudioCommandlet::IsMembershipPolicy(Out.Policy))
		{
			OutError = FString::Printf(TEXT("-policy=%s is not a membership policy; use open, request, invite or admin"), *Out.Policy);
			return false;
		}
		if (Out.Op == TEXT("createteam"))
		{
			if (Out.Policy.IsEmpty())
			{
				Out.Policy = TEXT("open");
			}
			return true;
		}
		FString CanSend;
		if (!ReadSwitch(Params, TEXT("memberscansend"), CanSend))
		{
			return true;
		}
		const bool bTrue = CanSend.Equals(TEXT("true"), ESearchCase::IgnoreCase) || CanSend == TEXT("1");
		if (!bTrue && !CanSend.Equals(TEXT("false"), ESearchCase::IgnoreCase) && CanSend != TEXT("0"))
		{
			OutError = FString::Printf(TEXT("-memberscansend=%s is not true or false"), *CanSend);
			return false;
		}
		Out.bMembersCanSend = bTrue;
		return true;
	}

	bool ParseGridGrant(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		const bool bRevoke = Out.Op == TEXT("revokegrid");
		if (!ReadId(Params, TEXT("grid"), Out.GridId, OutError, TEXT("-op=grids")))
		{
			return false;
		}
		FString Unused;
		const bool bUser = ReadSwitch(Params, TEXT("user"), Unused);
		const bool bGroup = ReadSwitch(Params, TEXT("group"), Unused);
		if (bUser == bGroup)
		{
			OutError = TEXT("give exactly one of -user=<id> or -group=<id>, who the grant is for");
			return false;
		}
		if (bUser && !ReadId(Params, TEXT("user"), Out.UserId, OutError, TEXT("the user's account")))
		{
			return false;
		}
		if (bGroup && !ReadId(Params, TEXT("group"), Out.GroupId, OutError, TEXT("-op=teams or -op=channels")))
		{
			return false;
		}
		if (!ReadOptionalId(Params, TEXT("role"), Out.RoleId, OutError, TEXT("the group's roles")))
		{
			return false;
		}
		if (Out.RoleId != 0 && !bGroup)
		{
			OutError = TEXT("-role=<id> goes with -group=<id>; a user grant has no role");
			return false;
		}
		if (!ReadKeys(Params, Out, OutError))
		{
			return false;
		}
		Out.bAllKeys = bRevoke && FParse::Param(*Params, TEXT("all"));
		if (Out.bAllKeys && !Out.Keys.IsEmpty())
		{
			OutError = TEXT("give -keys=\"a,b\" or -all, not both");
			return false;
		}
		if (Out.Keys.IsEmpty() && !Out.bAllKeys)
		{
			OutError = bRevoke ? TEXT("-keys=\"a,b\" or -all is required; -all revokes every grant of the target")
				: TEXT("-keys=\"a,b\" is required, the permission keys to grant; -op=grids lists them");
			return false;
		}
		return true;
	}

	bool IsAppScopedOp(const FString& Op)
	{
		static const TCHAR* const Ops[] = { TEXT("features"), TEXT("definefeature"), TEXT("grantfeature"), TEXT("revokefeature"), TEXT("teams"),
			TEXT("createteam"), TEXT("channels"), TEXT("createchannel"), TEXT("grids"), TEXT("creategrid"), TEXT("grantgrid"), TEXT("revokegrid"),
			TEXT("groupdetail"), TEXT("addmember"), TEXT("removemember"), TEXT("setroles"), TEXT("createrole"), TEXT("updaterole"), TEXT("deleterole"),
			TEXT("updategroup"), TEXT("deletegroup"), TEXT("setteampolicy"), TEXT("setchannelpolicy"), TEXT("gridlimits"), TEXT("gridgrants"), TEXT("gridpermissions") };
		return Algo::AnyOf(Ops, [&Op](const TCHAR* Name) { return Op == Name; });
	}

	bool ParseAppScoped(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		const FString& Op = Out.Op;
		if (!ReadOptionalId(Params, TEXT("app"), Out.AppId, OutError, TEXT("-op=apps")))
		{
			return false;
		}
		Out.bChannel = Op == TEXT("channels") || Op == TEXT("createchannel") || Op == TEXT("setchannelpolicy");
		if (Op == TEXT("groupdetail") || Op == TEXT("addmember") || Op == TEXT("removemember") || Op == TEXT("setroles") || Op == TEXT("createrole")
			|| Op == TEXT("updaterole") || Op == TEXT("deleterole") || Op == TEXT("updategroup") || Op == TEXT("deletegroup"))
		{
			return ParseGroupDetail(Params, Out, OutError);
		}
		if (Op == TEXT("setteampolicy") || Op == TEXT("setchannelpolicy"))
		{
			return ParseGroupPolicy(Params, Out, OutError);
		}
		if (Op == TEXT("gridlimits") || Op == TEXT("gridgrants") || Op == TEXT("gridpermissions"))
		{
			return ParseGridDetail(Params, Out, OutError);
		}
		if (Op == TEXT("features") || Op == TEXT("teams") || Op == TEXT("channels"))
		{
			return true;
		}
		if (Op == TEXT("definefeature"))
		{
			ReadSwitch(Params, TEXT("description"), Out.Description);
			return ReadFeatureKey(Params, Out, OutError);
		}
		if (Op == TEXT("grantfeature") || Op == TEXT("revokefeature"))
		{
			return ReadId(Params, TEXT("tier"), Out.TierId, OutError, TEXT("-op=features")) && ReadFeatureKey(Params, Out, OutError);
		}
		if (Op == TEXT("createteam") || Op == TEXT("createchannel"))
		{
			return ParseGroupCreate(Params, Out, OutError);
		}
		if (Op == TEXT("grids"))
		{
			return ReadChunk(Params, TEXT("low"), Out.Low, OutError) && ReadChunk(Params, TEXT("high"), Out.High, OutError)
				&& ReadOptionalId(Params, TEXT("user"), Out.UserId, OutError, TEXT("the user's account"));
		}
		if (Op == TEXT("creategrid"))
		{
			return ReadChunk(Params, TEXT("corner1"), Out.Low, OutError) && ReadChunk(Params, TEXT("corner2"), Out.High, OutError);
		}
		return ParseGridGrant(Params, Out, OutError);
	}

	bool ParseCreateApp(const FString& Params, FCrowdyStudioCommandRequest& Out, FString& OutError)
	{
		if (!ReadId(Params, TEXT("org"), Out.OrgId, OutError))
		{
			return false;
		}
		if (!ReadSwitch(Params, TEXT("name"), Out.Name))
		{
			OutError = TEXT("-name=<text> is required, the app's display name");
			return false;
		}
		if (!ReadSwitch(Params, TEXT("slug"), Out.Slug))
		{
			OutError = TEXT("-slug=<text> is required, the app's URL identifier");
			return false;
		}
		if (!Out.Slug.Equals(CrowdyStudioGql::SlugFromName(Out.Slug), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("-slug can only use lowercase letters, numbers, and single hyphens");
			return false;
		}
		if (!ReadSwitch(Params, TEXT("datacenter"), Out.Datacenter))
		{
			OutError = TEXT("-datacenter=<code> is required, one of the placeable codes shown by -op=apps");
			return false;
		}
		ReadSwitch(Params, TEXT("description"), Out.Description);
		return true;
	}

	/** A commandlet has no engine loop, so the ticker (which also ticks the HTTP module) is driven here until the condition holds or the time runs out. */
	bool Pump(TFunctionRef<bool()> IsDone, double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		double Last = FPlatformTime::Seconds();
		while (!IsDone() && !IsEngineExitRequested())
		{
			const double Now = FPlatformTime::Seconds();
			if (Now > Deadline)
			{
				return false;
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - Last));
			Last = Now;
			FPlatformProcess::Sleep(0.01f);
		}
		return IsDone();
	}

	/** Waits for every request the controller has in flight; any error status raised meanwhile fails the step. */
	bool Settle(FRun& Run)
	{
		if (!Pump([&Run]() { return !Run.Controller->IsBusy(); }, ReadTimeoutSeconds))
		{
			Run.Error = TEXT("Crowdy did not answer in time");
			return false;
		}
		if (!Run.StatusError.IsEmpty())
		{
			Run.Error = Run.StatusError;
			return false;
		}
		return true;
	}

	bool SignIn(FRun& Run)
	{
		int32 Scope = 0;
		int64 UserId = 0;
		if (!CrowdyStudioAuth::GetSignedInIdentity(Scope, UserId))
		{
			Run.Error = SignInMessage;
			return false;
		}
		const TCHAR* ScopeName = Scope == static_cast<int32>(ECrowdyStudioAuthScope::Session) ? TEXT("session") : TEXT("organizationToken");
		const TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
		Identity->SetStringField(TEXT("scope"), ScopeName);
		Identity->SetStringField(TEXT("userId"), IdText(UserId));
		Run.Result->SetObjectField(TEXT("signedIn"), Identity);
		Run.SignedInUserId = UserId;

		Run.Controller = MakeShared<FCrowdyStudioController>();
		FString* StatusError = &Run.StatusError;
		Run.Controller->OnStatusMessage.AddLambda([StatusError](const FString& Message, bool bIsError)
		{
			if (bIsError)
			{
				*StatusError = Message;
			}
		});
		if (!Run.Controller->RestoreSavedSignIn())
		{
			Run.Error = SignInMessage;
			return false;
		}
		if (!Settle(Run))
		{
			return false;
		}
		if (!Run.Controller->IsSignedIn())
		{
			Run.Error = FString::Printf(TEXT("The stored sign-in was not accepted. %s"), SignInMessage);
			return false;
		}
		Step(FString::Printf(TEXT("signed in (%s, user %lld) with %d organization(s) and %d app(s)"), ScopeName, UserId,
			Run.Controller->GetOrganizations().Num(), Run.Controller->GetApps().Num()));
		return true;
	}

	bool LoadDatacenters(FRun& Run)
	{
		Run.StatusError.Reset();
		Run.Controller->FetchPlaceableDatacenters();
		if (!Settle(Run))
		{
			return false;
		}
		if (!Run.Controller->HasFetchedPlaceableDatacenters())
		{
			Run.Error = TEXT("Could not read the datacenters this deployment can place an app in");
			return false;
		}
		return true;
	}

	bool RunApps(FRun& Run)
	{
		if (!SignIn(Run) || !LoadDatacenters(Run))
		{
			return false;
		}
		// The list answer carries no datacenter; one discovery read for every app fills it in.
		Run.StatusError.Reset();
		TArray<int64> AppIds;
		for (const TSharedPtr<FStudioApp>& App : Run.Controller->GetApps())
		{
			AppIds.Add(App->AppId);
		}
		Run.Controller->FetchAppDiscovery(AppIds);
		if (!Settle(Run))
		{
			return false;
		}

		const FStudioSettingsSnapshot Current = FCrowdyConfigSync::ReadCurrentSettings();
		const FString Environment = FCrowdyConfigSync::GetBackendMode();
		Run.Result->SetStringField(TEXT("environment"), Environment);
		SetSettings(*Run.Result, Current);
		Step(FString::Printf(TEXT("project: Environment=%s %s"), *Environment, *SettingsLine(Current)));

		TArray<TSharedPtr<FJsonValue>> DatacenterRows;
		for (const FStudioDatacenter& Dc : Run.Controller->GetPlaceableDatacenters())
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("code"), Dc.Code);
			Row->SetBoolField(TEXT("placeable"), Dc.bPlaceable);
			Row->SetStringField(TEXT("serving"), Dc.Serving);
			Row->SetStringField(TEXT("gameApiUrl"), Dc.GameApiUrl);
			DatacenterRows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("datacenter %s: %s, %s"), *Dc.Code, Dc.bPlaceable ? TEXT("placeable") : TEXT("not placeable"), *Dc.Serving));
		}
		Run.Result->SetArrayField(TEXT("datacenters"), DatacenterRows);

		TArray<TSharedPtr<FJsonValue>> OrgRows;
		for (const TSharedPtr<FStudioOrg>& Org : Run.Controller->GetOrganizations())
		{
			const TSharedRef<FJsonObject> OrgRow = MakeShared<FJsonObject>();
			OrgRow->SetStringField(TEXT("orgId"), IdText(Org->OrgId));
			OrgRow->SetStringField(TEXT("name"), Org->Name);
			OrgRow->SetStringField(TEXT("slug"), Org->Slug);
			OrgRow->SetArrayField(TEXT("permissions"), StringValues(Org->Permissions));
			Step(FString::Printf(TEXT("organization %lld \"%s\" (%s)"), Org->OrgId, *Org->Name, *Org->Slug));

			TArray<TSharedPtr<FJsonValue>> AppRows;
			for (const TSharedPtr<FStudioApp>& App : Run.Controller->GetApps())
			{
				if (App->OrgId != Org->OrgId)
				{
					continue;
				}
				const bool bArchived = App->Status == TEXT("ARCHIVED");
				const TSharedRef<FJsonObject> AppRow = MakeShared<FJsonObject>();
				AppRow->SetStringField(TEXT("appId"), IdText(App->AppId));
				AppRow->SetStringField(TEXT("name"), App->Name);
				AppRow->SetStringField(TEXT("slug"), App->Slug);
				AppRow->SetStringField(TEXT("datacenter"), App->DatacenterCode);
				AppRow->SetStringField(TEXT("status"), App->Status);
				AppRow->SetBoolField(TEXT("archived"), bArchived);
				AppRows.Add(MakeShared<FJsonValueObject>(AppRow));
				Step(FString::Printf(TEXT("  app %lld \"%s\" (%s) datacenter=%s %s%s"), App->AppId, *App->Name, *App->Slug, *App->DatacenterCode,
					*App->Status, bArchived ? TEXT(", archived") : TEXT("")));
			}
			OrgRow->SetArrayField(TEXT("apps"), AppRows);
			OrgRows.Add(MakeShared<FJsonValueObject>(OrgRow));
		}
		Run.Result->SetArrayField(TEXT("organizations"), OrgRows);
		return true;
	}

	bool RunCreateApp(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!SignIn(Run) || !LoadDatacenters(Run))
		{
			return false;
		}
		const FString Problem = UCrowdyStudioCommandlet::FindCreateProblem(Request, Run.Controller->GetOrganizations(), Run.Controller->GetPlaceableDatacenters());
		if (!Problem.IsEmpty())
		{
			Run.Error = Problem;
			return false;
		}

		Run.Result->SetStringField(TEXT("orgId"), IdText(Request.OrgId));
		Run.Result->SetStringField(TEXT("name"), Request.Name);
		Run.Result->SetStringField(TEXT("slug"), Request.Slug);
		Run.Result->SetStringField(TEXT("datacenter"), Request.Datacenter);
		Run.Result->SetStringField(TEXT("description"), Request.Description);
		Run.Result->SetBoolField(TEXT("created"), false);
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s app \"%s\" (%s) in organization %lld, datacenter %s (permanent)"), bNeedsConfirmation ? TEXT("would create") : TEXT("creating"),
			*Request.Name, *Request.Slug, Request.OrgId, *Request.Datacenter));
		if (bNeedsConfirmation)
		{
			Run.Error = TEXT("Creating an app is permanent, its datacenter can never change; pass -yes to confirm.");
			return false;
		}

		Run.StatusError.Reset();
		Run.Controller->CreateApp(Request.OrgId, Request.Name, Request.Slug, Request.Datacenter, Request.Description,
			[&Run](int64 CreatedAppId)
			{
				Run.CreatedAppId = CreatedAppId;
				Run.bCreateAnswered = true;
			});
		if (!Pump([&Run]() { return Run.bCreateAnswered; }, CreateTimeoutSeconds))
		{
			Run.Error = TEXT("No answer in time; the app may still have been created, check -op=apps");
			return false;
		}
		if (Run.CreatedAppId == 0)
		{
			Run.Error = Run.StatusError.IsEmpty() ? FString(TEXT("The app could not be created")) : Run.StatusError;
			return false;
		}
		Run.Result->SetBoolField(TEXT("created"), true);
		Run.Result->SetStringField(TEXT("appId"), IdText(Run.CreatedAppId));
		Step(FString::Printf(TEXT("created app %lld; write it into the project with -op=sync -app=%lld -yes"), Run.CreatedAppId, Run.CreatedAppId));
		return true;
	}

	bool RunSync(FRun& Run)
	{
		const int64 AppId = Run.Request.AppId;
		if (!SignIn(Run))
		{
			return false;
		}
		if (!UCrowdyStudioCommandlet::FindApp(Run.Controller->GetApps(), AppId))
		{
			Run.Error = FString::Printf(TEXT("App %lld is not one of this account's apps; -op=apps lists them"), AppId);
			return false;
		}
		// Selecting an app in the console reads its record before Sync to project writes it; the same read is made here.
		Run.StatusError.Reset();
		Run.Controller->FetchApp(AppId);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioApp> App = UCrowdyStudioCommandlet::FindApp(Run.Controller->GetApps(), AppId);
		if (!App)
		{
			Run.Error = FString::Printf(TEXT("App %lld is no longer one of this account's apps"), AppId);
			return false;
		}

		const FStudioSettingsSnapshot Before = FCrowdyConfigSync::ReadCurrentSettings();
		const FStudioSettingsSnapshot After = UCrowdyStudioCommandlet::ProposeSync(*App);
		const TSharedRef<FJsonObject> BeforeRow = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> AfterRow = MakeShared<FJsonObject>();
		SetSettings(*BeforeRow, Before);
		SetSettings(*AfterRow, After);
		Run.Result->SetObjectField(TEXT("before"), BeforeRow);
		Run.Result->SetObjectField(TEXT("after"), AfterRow);
		Run.Result->SetBoolField(TEXT("changed"), false);
		Run.Result->SetStringField(TEXT("environment"), FCrowdyConfigSync::GetBackendMode());
		const bool bChanged = !(Before == After);
		Step(FString::Printf(TEXT("app %lld \"%s\": %s"), AppId, *App->Name, bChanged ? TEXT("the project settings change") : TEXT("the project already holds these values")));
		Step(FString::Printf(TEXT("  was: %s"), *SettingsLine(Before)));
		Step(FString::Printf(TEXT("  now: %s"), *SettingsLine(After)));
		if (App->GameApiUrl.IsEmpty())
		{
			Step(TEXT("  the app reports no game endpoint, so GameApiHttpUrl and GameApiWsUrl stay as they are"));
		}
		if (!bChanged)
		{
			Step(FString::Printf(TEXT("already synced to app %lld; nothing to write"), AppId));
			return true;
		}
		if (UCrowdyStudioCommandlet::NeedsConfirmation(Run.Request))
		{
			Run.Error = TEXT("Sync rewrites AppID, OrgId and the game endpoints in Config/DefaultGame.ini; pass -yes to confirm.");
			return false;
		}
		const FString WriteProblem = UCrowdyStudioCommandlet::FindSyncWriteProblem(Run.Request, FPlatformProcess::IsApplicationRunning(TEXT("UnrealEditor")));
		if (!WriteProblem.IsEmpty())
		{
			Run.Error = WriteProblem;
			return false;
		}

		if (!FCrowdyConfigSync::ApplyAppToSettings(*App))
		{
			Run.Error = FCrowdyConfigSync::WriteFailedMessage;
			return false;
		}
		if (!(FCrowdyConfigSync::ReadCurrentSettings() == After))
		{
			Run.Error = TEXT("The project settings did not take the app's values");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		Step(TEXT("wrote the app into Config/DefaultGame.ini"));
		return true;
	}

	/** Signs in and selects -app, or the project's synced app, as the console does when an app is picked, so the app token is in hand. */
	bool OpenApp(FRun& Run)
	{
		if (!SignIn(Run))
		{
			return false;
		}
		FString Problem;
		const int64 AppId = UCrowdyStudioCommandlet::ResolveAppId(Run.Request, FCrowdyConfigSync::ReadCurrentSettings().AppId, Problem);
		if (AppId == 0)
		{
			Run.Error = Problem;
			return false;
		}
		if (!UCrowdyStudioCommandlet::FindApp(Run.Controller->GetApps(), AppId))
		{
			Run.Error = FString::Printf(TEXT("App %lld is not one of this account's apps; -op=apps lists them"), AppId);
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->SelectApp(AppId, false);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioApp> App = UCrowdyStudioCommandlet::FindApp(Run.Controller->GetApps(), AppId);
		const bool bFromProject = Run.Request.AppId == 0;
		Run.Result->SetStringField(TEXT("appId"), IdText(AppId));
		Run.Result->SetStringField(TEXT("appName"), App ? App->Name : FString());
		Run.Result->SetStringField(TEXT("appSource"), bFromProject ? TEXT("project") : TEXT("switch"));
		Step(FString::Printf(TEXT("app %lld \"%s\" (%s)"), AppId, App ? *App->Name : TEXT(""), bFromProject ? TEXT("the project's synced app") : TEXT("from -app")));
		return true;
	}

	bool OpenGamePlane(FRun& Run)
	{
		if (!OpenApp(Run))
		{
			return false;
		}
		if (!Run.Controller->CanUseGamePlane())
		{
			Run.Error = TEXT("Teams, channels and grids need a session sign-in: sign in to Crowdy Studio with email, dev, or a magic link. An organization token only covers account settings.");
			return false;
		}
		return true;
	}

	bool OpenAccess(FRun& Run, bool bWrite)
	{
		if (!OpenApp(Run))
		{
			return false;
		}
		if (!Run.Controller->CanUseAppAccess())
		{
			Run.Error = TEXT("Features need a sign-in that can reach this app.");
			return false;
		}
		if (bWrite && !Run.Controller->CanManageApps())
		{
			Run.Error = TEXT("Changing features needs the manage_apps permission in the app's organization, which this sign-in does not hold");
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->EnsureAccessListsLoaded();
		return Settle(Run);
	}

	TSharedRef<FJsonObject> ChunkObject(const FStudioChunk& Chunk)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("x"), IdText(Chunk.X));
		Out->SetStringField(TEXT("y"), IdText(Chunk.Y));
		Out->SetStringField(TEXT("z"), IdText(Chunk.Z));
		return Out;
	}

	FString ChunkText(const FStudioChunk& Chunk)
	{
		return FString::Printf(TEXT("[%lld, %lld, %lld]"), Chunk.X, Chunk.Y, Chunk.Z);
	}

	bool RunFeatures(FRun& Run)
	{
		if (!OpenAccess(Run, false))
		{
			return false;
		}
		const FCrowdyStudioController& Controller = *Run.Controller;
		TArray<TSharedPtr<FJsonValue>> FeatureRows;
		for (const TSharedPtr<FStudioAppFeature>& Feature : Controller.GetFeatures())
		{
			if (!Feature.IsValid())
			{
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> TierIds;
			for (const TSharedPtr<FStudioTierFeature>& Grant : Controller.GetTierFeatures())
			{
				if (Grant.IsValid() && Grant->FeatureKey.Equals(Feature->FeatureKey, ESearchCase::CaseSensitive))
				{
					TierIds.Add(MakeShared<FJsonValueString>(IdText(Grant->TierId)));
				}
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("key"), Feature->FeatureKey);
			Row->SetStringField(TEXT("description"), Feature->Description);
			Row->SetArrayField(TEXT("grantedToTiers"), TierIds);
			FeatureRows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("feature %s%s%s"), *Feature->FeatureKey, Feature->Description.IsEmpty() ? TEXT("") : TEXT(": "), *Feature->Description));
		}
		Run.Result->SetArrayField(TEXT("features"), FeatureRows);

		TArray<TSharedPtr<FJsonValue>> TierRows;
		for (const TSharedPtr<FStudioAccessTier>& Tier : Controller.GetAccessTiers())
		{
			if (!Tier.IsValid())
			{
				continue;
			}
			TArray<FString> Granted;
			for (const TSharedPtr<FStudioTierFeature>& Grant : Controller.GetTierFeatures())
			{
				if (Grant.IsValid() && Grant->TierId == Tier->TierId)
				{
					Granted.Add(Grant->FeatureKey);
				}
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tierId"), IdText(Tier->TierId));
			Row->SetStringField(TEXT("name"), Tier->Name);
			Row->SetStringField(TEXT("status"), Tier->Status);
			Row->SetBoolField(TEXT("free"), Tier->bIsFree);
			Row->SetBoolField(TEXT("default"), Tier->bIsDefault);
			Row->SetArrayField(TEXT("features"), StringValues(Granted));
			TierRows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("tier %lld \"%s\" (%s%s%s) grants: %s"), Tier->TierId, *Tier->Name, *Tier->Status, Tier->bIsFree ? TEXT(", free") : TEXT(""),
				Tier->bIsDefault ? TEXT(", default") : TEXT(""), Granted.IsEmpty() ? TEXT("nothing") : *FString::Join(Granted, TEXT(", "))));
		}
		Run.Result->SetArrayField(TEXT("tiers"), TierRows);
		return true;
	}

	bool RunDefineFeature(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenAccess(Run, true))
		{
			return false;
		}
		const TSharedPtr<FStudioAppFeature>* Existing = Run.Controller->GetFeatures().FindByPredicate([&Request](const TSharedPtr<FStudioAppFeature>& Feature)
		{
			return Feature.IsValid() && Feature->FeatureKey.Equals(Request.Key, ESearchCase::CaseSensitive);
		});
		Run.Result->SetStringField(TEXT("key"), Request.Key);
		Run.Result->SetStringField(TEXT("description"), Request.Description);
		Run.Result->SetBoolField(TEXT("changed"), false);
		if (Existing && (Request.Description.IsEmpty() || (*Existing)->Description.Equals(Request.Description, ESearchCase::CaseSensitive)))
		{
			Step(FString::Printf(TEXT("feature %s is already defined; nothing to change"), *Request.Key));
			return true;
		}
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s feature %s"), bNeedsConfirmation ? TEXT("would define") : TEXT("defining"), *Request.Key));
		if (bNeedsConfirmation)
		{
			Run.Error = TEXT("Defining a feature adds a key the app's access tiers can be granted; pass -yes to confirm.");
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->DefineFeature(Request.Key, Request.Description);
		if (!Settle(Run))
		{
			return false;
		}
		const FString Problem = UCrowdyStudioCommandlet::FindDefineProblem(Request, Run.Controller->GetFeatures());
		if (!Problem.IsEmpty())
		{
			Run.Error = Problem;
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunTierFeature(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		const bool bGrant = Request.Op == TEXT("grantfeature");
		if (!OpenAccess(Run, true))
		{
			return false;
		}
		const FCrowdyStudioController& Controller = *Run.Controller;
		const FString Problem = UCrowdyStudioCommandlet::FindAccessProblem(Request, Controller.GetFeatures(), Controller.GetAccessTiers());
		if (!Problem.IsEmpty())
		{
			Run.Error = Problem;
			return false;
		}
		Run.Result->SetStringField(TEXT("tierId"), IdText(Request.TierId));
		Run.Result->SetStringField(TEXT("key"), Request.Key);
		Run.Result->SetBoolField(TEXT("changed"), false);
		if (UCrowdyStudioCommandlet::IsGranted(Controller.GetTierFeatures(), Request.TierId, Request.Key) == bGrant)
		{
			Step(FString::Printf(TEXT("tier %lld %s feature %s; nothing to change"), Request.TierId, bGrant ? TEXT("already grants") : TEXT("does not grant"), *Request.Key));
			return true;
		}
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s feature %s %s tier %lld"), bNeedsConfirmation ? (bGrant ? TEXT("would grant") : TEXT("would revoke")) : (bGrant ? TEXT("granting") : TEXT("revoking")),
			*Request.Key, bGrant ? TEXT("to") : TEXT("from"), Request.TierId));
		if (bNeedsConfirmation)
		{
			Run.Error = TEXT("This changes what the tier's players can use; pass -yes to confirm.");
			return false;
		}
		Run.StatusError.Reset();
		if (bGrant)
		{
			Run.Controller->GrantTierFeature(Request.TierId, Request.Key);
		}
		else
		{
			Run.Controller->RevokeTierFeature(Request.TierId, Request.Key);
		}
		if (!Settle(Run))
		{
			return false;
		}
		if (UCrowdyStudioCommandlet::IsGranted(Run.Controller->GetTierFeatures(), Request.TierId, Request.Key) != bGrant)
		{
			Run.Error = TEXT("The tier's feature grants did not change");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool IsTeamOp(const FRun& Run)
	{
		return !Run.Request.bChannel;
	}

	ECrowdyGroupKind KindOf(const FRun& Run)
	{
		return Run.Request.bChannel ? ECrowdyGroupKind::Channel : ECrowdyGroupKind::Team;
	}

	TSharedPtr<FStudioGroup> FindGroupById(const TArray<TSharedPtr<FStudioGroup>>& Groups, int64 GroupId)
	{
		const TSharedPtr<FStudioGroup>* Found = Groups.FindByPredicate([GroupId](const TSharedPtr<FStudioGroup>& Group) { return Group.IsValid() && Group->GroupId == GroupId; });
		return Found ? *Found : nullptr;
	}

	const TArray<TSharedPtr<FStudioGroup>>& GroupList(const FRun& Run)
	{
		return IsTeamOp(Run) ? Run.Controller->GetTeams() : Run.Controller->GetChannels();
	}

	/** The newest group of that name, so a group just created is the one found. */
	TSharedPtr<FStudioGroup> FindGroupByName(const TArray<TSharedPtr<FStudioGroup>>& Groups, const FString& Name)
	{
		TSharedPtr<FStudioGroup> Found;
		for (const TSharedPtr<FStudioGroup>& Group : Groups)
		{
			if (Group.IsValid() && Group->Name.Equals(Name, ESearchCase::CaseSensitive) && (!Found || Group->GroupId > Found->GroupId))
			{
				Found = Group;
			}
		}
		return Found;
	}

	bool LoadGroups(FRun& Run)
	{
		Run.StatusError.Reset();
		if (IsTeamOp(Run))
		{
			Run.Controller->FetchTeams();
			Run.Controller->FetchTeamPolicy();
		}
		else
		{
			Run.Controller->FetchChannels();
			Run.Controller->FetchChannelPolicy();
		}
		return Settle(Run);
	}

	bool RunGroups(FRun& Run)
	{
		if (!OpenGamePlane(Run) || !LoadGroups(Run))
		{
			return false;
		}
		const bool bTeams = IsTeamOp(Run);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TSharedPtr<FStudioGroup>& Group : GroupList(Run))
		{
			if (!Group.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("groupId"), IdText(Group->GroupId));
			Row->SetStringField(TEXT("name"), Group->Name);
			Row->SetStringField(TEXT("description"), Group->Description);
			Row->SetStringField(TEXT("membershipPolicy"), Group->MembershipPolicy);
			Row->SetStringField(TEXT("status"), Group->Status);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("%s %lld \"%s\" membership=%s %s"), bTeams ? TEXT("team") : TEXT("channel"), Group->GroupId, *Group->Name, *Group->MembershipPolicy, *Group->Status));
		}
		Run.Result->SetArrayField(bTeams ? TEXT("teams") : TEXT("channels"), Rows);

		const FStudioGroupPolicy& Policy = bTeams ? Run.Controller->GetTeamPolicy() : Run.Controller->GetChannelPolicy();
		const TSharedRef<FJsonObject> PolicyRow = MakeShared<FJsonObject>();
		PolicyRow->SetStringField(TEXT("creationPolicy"), Policy.CreationPolicy);
		PolicyRow->SetStringField(TEXT("defaultMembershipPolicy"), Policy.DefaultMembershipPolicy);
		PolicyRow->SetNumberField(TEXT("maxMembers"), Policy.MaxMembers);
		PolicyRow->SetNumberField(TEXT("maxGroupsPerUser"), Policy.MaxGroupsPerUser);
		Run.Result->SetObjectField(TEXT("policy"), PolicyRow);
		Step(FString::Printf(TEXT("policy: creation=%s default membership=%s max members=%d max per user=%d (0 is unlimited)"), *Policy.CreationPolicy,
			*Policy.DefaultMembershipPolicy, Policy.MaxMembers, Policy.MaxGroupsPerUser));
		return true;
	}

	bool RunCreateGroup(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		const bool bTeam = IsTeamOp(Run);
		const TCHAR* Noun = bTeam ? TEXT("team") : TEXT("channel");
		if (!OpenGamePlane(Run) || !LoadGroups(Run))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("name"), Request.Name);
		Run.Result->SetStringField(TEXT("description"), Request.Description);
		Run.Result->SetStringField(TEXT("policy"), Request.Policy);
		Run.Result->SetBoolField(TEXT("changed"), false);
		if (const TSharedPtr<FStudioGroup> Existing = FindGroupByName(GroupList(Run), Request.Name))
		{
			Run.Result->SetStringField(TEXT("groupId"), IdText(Existing->GroupId));
			Run.Result->SetStringField(TEXT("description"), Existing->Description);
			Run.Result->SetStringField(TEXT("policy"), Existing->MembershipPolicy);
			const FString Difference = UCrowdyStudioCommandlet::FindGroupDifference(Request, *Existing);
			if (!Difference.IsEmpty())
			{
				Run.Error = Difference;
				return false;
			}
			Step(FString::Printf(TEXT("%s %lld \"%s\" already exists; nothing to create"), Noun, Existing->GroupId, *Request.Name));
			return true;
		}
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s %s \"%s\" with membership %s"), bNeedsConfirmation ? TEXT("would create") : TEXT("creating"), Noun, *Request.Name,
			Request.Policy.IsEmpty() ? TEXT("the app's default") : *Request.Policy));
		if (bNeedsConfirmation)
		{
			Run.Error = FString::Printf(TEXT("Creating a %s adds it for the app's players; pass -yes to confirm."), Noun);
			return false;
		}
		Run.StatusError.Reset();
		if (bTeam)
		{
			Run.Controller->CreateTeam(Request.Name, Request.Description, Request.Policy);
		}
		else
		{
			Run.Controller->CreateChannel(Request.Name, Request.Description, Request.bMembersCanSend, Request.Policy);
		}
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioGroup> Created = FindGroupByName(GroupList(Run), Request.Name);
		if (!Created)
		{
			Run.Error = FString::Printf(TEXT("The new %s was not in the app's list afterwards"), Noun);
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		Run.Result->SetStringField(TEXT("groupId"), IdText(Created->GroupId));
		Step(FString::Printf(TEXT("created %s %lld"), Noun, Created->GroupId));
		return true;
	}

	bool RunGrids(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenGamePlane(Run))
		{
			return false;
		}
		const int64 UserId = Request.UserId != 0 ? Request.UserId : Run.SignedInUserId;
		if (UserId == 0)
		{
			Run.Error = TEXT("-user=<id> is required: this sign-in has no user of its own to scan as");
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->FetchRuntimePermissions();
		Run.Controller->FetchNearbyGrids(UserId, Request.Low.X, Request.Low.Y, Request.Low.Z, Request.High.X, Request.High.Y, Request.High.Z);
		if (!Settle(Run))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("userId"), IdText(UserId));
		Run.Result->SetObjectField(TEXT("low"), ChunkObject(Request.Low));
		Run.Result->SetObjectField(TEXT("high"), ChunkObject(Request.High));
		Run.Result->SetArrayField(TEXT("permissionKeys"), StringValues(Run.Controller->GetRuntimePermissions()));
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TSharedPtr<FStudioGrid>& Grid : Run.Controller->GetNearbyGrids())
		{
			if (!Grid.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("gridId"), IdText(Grid->GridId));
			Row->SetObjectField(TEXT("low"), ChunkObject(Grid->Low));
			Row->SetObjectField(TEXT("high"), ChunkObject(Grid->High));
			Row->SetArrayField(TEXT("effectivePermissionKeys"), StringValues(Grid->EffectivePermissionKeys));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("grid %lld %s to %s, user %lld holds: %s"), Grid->GridId, *ChunkText(Grid->Low), *ChunkText(Grid->High), UserId,
				Grid->EffectivePermissionKeys.IsEmpty() ? TEXT("nothing") : *FString::Join(Grid->EffectivePermissionKeys, TEXT(", "))));
		}
		Run.Result->SetArrayField(TEXT("grids"), Rows);
		Step(FString::Printf(TEXT("permission keys a grid can grant: %s"), *FString::Join(Run.Controller->GetRuntimePermissions(), TEXT(", "))));
		return true;
	}

	bool RunCreateGrid(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenGamePlane(Run))
		{
			return false;
		}
		Run.Result->SetObjectField(TEXT("corner1"), ChunkObject(Request.Low));
		Run.Result->SetObjectField(TEXT("corner2"), ChunkObject(Request.High));
		Run.Result->SetBoolField(TEXT("changed"), false);
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s a grid from chunk %s to %s"), bNeedsConfirmation ? TEXT("would create") : TEXT("creating"), *ChunkText(Request.Low), *ChunkText(Request.High)));
		if (bNeedsConfirmation)
		{
			Run.Error = TEXT("Creating a grid scopes world permissions to that region; pass -yes to confirm.");
			return false;
		}
		Run.StatusError.Reset();
		Run.CreatedGridId = 0;
		Run.Controller->CreateGrid(Request.Low.X, Request.Low.Y, Request.Low.Z, Request.High.X, Request.High.Y, Request.High.Z,
			[&Run](int64 GridId) { Run.CreatedGridId = GridId; });
		if (!Settle(Run))
		{
			return false;
		}
		if (Run.CreatedGridId == 0)
		{
			Run.Error = TEXT("The grid was not created");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		Run.Result->SetStringField(TEXT("gridId"), IdText(Run.CreatedGridId));
		Step(FString::Printf(TEXT("created grid %lld"), Run.CreatedGridId));
		return true;
	}

	/** Reads the revoked target's grants again, so a revoke the server answered but did not apply fails the run. */
	bool VerifyRevoked(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		FCrowdyStudioController& Controller = *Run.Controller;
		const bool bFromUser = Request.UserId != 0;
		Run.StatusError.Reset();
		if (bFromUser)
		{
			Controller.FetchGridUserPermissions(Request.GridId, Request.UserId);
		}
		else
		{
			Controller.FetchGridGroupGrants(Request.GridId, Request.GroupId);
		}
		if (!Settle(Run))
		{
			return false;
		}
		Run.Error = bFromUser ? UCrowdyStudioCommandlet::FindUnrevokedUserKey(Request, Controller.GetGridUserEffectiveKeys())
			: UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(Request, Controller.GetGridGroupGrants());
		return Run.Error.IsEmpty();
	}

	bool RunGridGrant(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		const bool bGrant = Request.Op == TEXT("grantgrid");
		const bool bToUser = Request.UserId != 0;
		if (!OpenGamePlane(Run))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->FetchRuntimePermissions();
		if (!Settle(Run))
		{
			return false;
		}
		const FString Problem = UCrowdyStudioCommandlet::FindUnknownPermissionKey(Request.Keys, Run.Controller->GetRuntimePermissions());
		if (!Problem.IsEmpty())
		{
			Run.Error = Problem;
			return false;
		}
		if (!bGrant && !bToUser)
		{
			Run.StatusError.Reset();
			Run.Controller->FetchTeams();
			Run.Controller->FetchChannels();
			if (!Settle(Run))
			{
				return false;
			}
			Run.Error = UCrowdyStudioCommandlet::FindGridGroupProblem(Request.GroupId, Run.Controller->GetTeams(), Run.Controller->GetChannels());
			if (!Run.Error.IsEmpty())
			{
				return false;
			}
		}
		const FString Target = bToUser ? FString::Printf(TEXT("user %lld"), Request.UserId)
			: FString::Printf(TEXT("group %lld%s"), Request.GroupId, Request.RoleId != 0 ? *FString::Printf(TEXT(" role %lld"), Request.RoleId) : TEXT(""));
		const FString KeyText = Request.bAllKeys ? FString(TEXT("every key")) : FString::Join(Request.Keys, TEXT(", "));
		Run.Result->SetStringField(TEXT("gridId"), IdText(Request.GridId));
		Run.Result->SetStringField(bToUser ? TEXT("userId") : TEXT("groupId"), IdText(bToUser ? Request.UserId : Request.GroupId));
		Run.Result->SetStringField(TEXT("roleId"), IdText(Request.RoleId));
		Run.Result->SetArrayField(TEXT("keys"), StringValues(Request.Keys));
		Run.Result->SetBoolField(TEXT("all"), Request.bAllKeys);
		Run.Result->SetBoolField(TEXT("changed"), false);
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Request);
		Step(FString::Printf(TEXT("%s %s %s grid %lld: %s"), bNeedsConfirmation ? (bGrant ? TEXT("would grant") : TEXT("would revoke")) : (bGrant ? TEXT("granting") : TEXT("revoking")),
			bGrant ? TEXT("to") : TEXT("from"), *Target, Request.GridId, *KeyText));
		if (bNeedsConfirmation)
		{
			Run.Error = TEXT("This changes who can do what in that grid; pass -yes to confirm.");
			return false;
		}
		Run.StatusError.Reset();
		FCrowdyStudioController& Controller = *Run.Controller;
		if (bToUser && bGrant)
		{
			Controller.GrantGridPermissions(Request.GridId, Request.UserId, Request.Keys);
		}
		else if (bToUser)
		{
			Controller.RevokeGridPermissions(Request.GridId, Request.UserId, Request.Keys);
		}
		else if (bGrant)
		{
			Controller.AssignGroupToGrid(Request.GridId, Request.GroupId, Request.RoleId != 0, Request.RoleId, Request.Keys);
		}
		else
		{
			Controller.RevokeGroupFromGrid(Request.GridId, Request.GroupId, Request.RoleId != 0, Request.RoleId, Request.Keys);
		}
		if (!Settle(Run) || (!bGrant && !VerifyRevoked(Run)))
		{
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		if (bToUser)
		{
			Run.Result->SetArrayField(TEXT("effectivePermissionKeys"), StringValues(Controller.GetGridUserEffectiveKeys()));
			Step(FString::Printf(TEXT("user %lld now holds on grid %lld: %s"), Request.UserId, Request.GridId, *FString::Join(Controller.GetGridUserEffectiveKeys(), TEXT(", "))));
			return true;
		}
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TSharedPtr<FStudioGridGroupGrant>& Grant : Controller.GetGridGroupGrants())
		{
			if (!Grant.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("groupId"), IdText(Grant->GroupId));
			Row->SetStringField(TEXT("roleId"), Grant->bHasRole ? IdText(Grant->GroupRoleId) : FString());
			Row->SetStringField(TEXT("permissionKey"), Grant->PermissionKey);
			Row->SetStringField(TEXT("expiresAt"), Grant->ExpiresAt);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Run.Result->SetArrayField(TEXT("groupGrants"), Rows);
		Step(FString::Printf(TEXT("group %lld has %d grant(s) on grid %lld"), Request.GroupId, Rows.Num(), Request.GridId));
		return true;
	}

	bool SameKeys(const TArray<FString>& A, const TArray<FString>& B)
	{
		return A.Num() == B.Num() && Algo::AllOf(A, [&B](const FString& Key)
		{
			return B.ContainsByPredicate([&Key](const FString& Other) { return Other.Equals(Key, ESearchCase::CaseSensitive); });
		});
	}

	bool SameIds(const TArray<int64>& A, const TArray<int64>& B)
	{
		return A.Num() == B.Num() && Algo::AllOf(A, [&B](int64 Id) { return B.Contains(Id); });
	}

	TArray<TSharedPtr<FJsonValue>> IdValues(const TArray<int64>& Ids)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const int64 Id : Ids)
		{
			Values.Add(MakeShared<FJsonValueString>(IdText(Id)));
		}
		return Values;
	}

	const TCHAR* GroupNoun(const FRun& Run)
	{
		return Run.Request.bChannel ? TEXT("channel") : TEXT("team");
	}

	TSharedPtr<FStudioGroupMember> FindMember(const FRun& Run, int64 UserId)
	{
		for (const TSharedPtr<FStudioGroupMember>& Member : Run.Controller->GetGroupMembers())
		{
			if (Member.IsValid() && Member->UserId == UserId)
			{
				return Member;
			}
		}
		return nullptr;
	}

	TSharedPtr<FStudioGroupRole> FindRole(const FRun& Run, int64 RoleId, const FString& Name)
	{
		for (const TSharedPtr<FStudioGroupRole>& Role : Run.Controller->GetGroupRoles())
		{
			if (Role.IsValid() && (RoleId != 0 ? Role->GroupRoleId == RoleId : Role->RoleName.Equals(Name, ESearchCase::CaseSensitive)))
			{
				return Role;
			}
		}
		return nullptr;
	}

	bool IsPendingMember(const FStudioGroupMember& Member)
	{
		return Member.Status.Equals(TEXT("pending"), ESearchCase::IgnoreCase);
	}

	/** Loads the app's groups and, with bDetail, the named group's members and roles, the way opening it in Teams or Channels does. */
	bool OpenGroup(FRun& Run, bool bDetail, TSharedPtr<FStudioGroup>& OutGroup)
	{
		const int64 GroupId = Run.Request.GroupId;
		if (!OpenGamePlane(Run) || !LoadGroups(Run))
		{
			return false;
		}
		OutGroup = FindGroupById(GroupList(Run), GroupId);
		if (!OutGroup)
		{
			Run.Error = FString::Printf(TEXT("-group=%lld is not one of this app's %ss; %s lists them"), GroupId, GroupNoun(Run), Run.Request.bChannel ? TEXT("-op=channels") : TEXT("-op=teams"));
			return false;
		}
		Run.Result->SetStringField(TEXT("kind"), GroupNoun(Run));
		Run.Result->SetStringField(TEXT("groupId"), IdText(GroupId));
		Run.Result->SetStringField(TEXT("groupName"), OutGroup->Name);
		Run.Result->SetBoolField(TEXT("changed"), false);
		if (!bDetail)
		{
			return true;
		}
		Run.StatusError.Reset();
		Run.Controller->SelectGroup(KindOf(Run), GroupId);
		return Settle(Run);
	}

	/** Describes the change; true when it may go ahead, false after setting the -yes refusal. */
	bool Confirm(FRun& Run, const FString& Would, const FString& Doing, const TCHAR* Consequence)
	{
		const bool bNeedsConfirmation = UCrowdyStudioCommandlet::NeedsConfirmation(Run.Request);
		Step(bNeedsConfirmation ? Would : Doing);
		if (bNeedsConfirmation)
		{
			Run.Error = FString::Printf(TEXT("%s; pass -yes to confirm."), Consequence);
		}
		return !bNeedsConfirmation;
	}

	bool RunGroupDetail(FRun& Run)
	{
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, true, Group))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("membershipPolicy"), Group->MembershipPolicy);
		TArray<TSharedPtr<FJsonValue>> MemberRows;
		for (const TSharedPtr<FStudioGroupMember>& Member : Run.Controller->GetGroupMembers())
		{
			if (!Member.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("userId"), IdText(Member->UserId));
			Row->SetStringField(TEXT("groupMemberId"), IdText(Member->GroupMemberId));
			Row->SetStringField(TEXT("status"), Member->Status);
			Row->SetArrayField(TEXT("roleIds"), IdValues(Member->RoleIds));
			Row->SetArrayField(TEXT("roleNames"), StringValues(Member->RoleNames));
			MemberRows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("member user %lld %s roles: %s"), Member->UserId, *Member->Status, Member->RoleNames.IsEmpty() ? TEXT("none") : *FString::Join(Member->RoleNames, TEXT(", "))));
		}
		Run.Result->SetArrayField(TEXT("members"), MemberRows);

		TArray<TSharedPtr<FJsonValue>> RoleRows;
		for (const TSharedPtr<FStudioGroupRole>& Role : Run.Controller->GetGroupRoles())
		{
			if (!Role.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("roleId"), IdText(Role->GroupRoleId));
			Row->SetStringField(TEXT("name"), Role->RoleName);
			Row->SetNumberField(TEXT("rank"), Role->Rank);
			Row->SetBoolField(TEXT("system"), Role->bIsSystem);
			Row->SetArrayField(TEXT("permissions"), StringValues(Role->Permissions));
			RoleRows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("role %lld \"%s\" rank %d%s: %s"), Role->GroupRoleId, *Role->RoleName, Role->Rank, Role->bIsSystem ? TEXT(", system") : TEXT(""),
				Role->Permissions.IsEmpty() ? TEXT("no permissions") : *FString::Join(Role->Permissions, TEXT(", "))));
		}
		Run.Result->SetArrayField(TEXT("roles"), RoleRows);
		return true;
	}

	bool RunMember(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		const bool bAdd = Request.Op == TEXT("addmember");
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, true, Group))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("userId"), IdText(Request.UserId));
		const TSharedPtr<FStudioGroupMember> Member = FindMember(Run, Request.UserId);
		const bool bPending = Member.IsValid() && IsPendingMember(*Member);
		if (bAdd ? (Member.IsValid() && !bPending) : !Member.IsValid())
		{
			Step(FString::Printf(TEXT("user %lld %s group %lld; nothing to change"), Request.UserId, bAdd ? TEXT("is already in") : TEXT("is not in"), Request.GroupId));
			return true;
		}
		const TCHAR* Verb = bAdd ? (bPending ? TEXT("approve") : TEXT("add")) : TEXT("remove");
		if (!Confirm(Run, FString::Printf(TEXT("would %s user %lld %s group %lld"), Verb, Request.UserId, bAdd ? TEXT("to") : TEXT("from"), Request.GroupId),
			FString::Printf(TEXT("%s user %lld %s group %lld"), Verb, Request.UserId, bAdd ? TEXT("to") : TEXT("from"), Request.GroupId),
			bAdd ? TEXT("Adding a member changes who is in the group") : TEXT("Removing a member takes them out of the group and their roles with it")))
		{
			return false;
		}
		Run.StatusError.Reset();
		if (bAdd)
		{
			Run.Controller->AddGroupMember(KindOf(Run), Request.GroupId, Request.UserId);
		}
		else
		{
			Run.Controller->RemoveGroupMember(KindOf(Run), Request.GroupId, Request.UserId);
		}
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioGroupMember> After = FindMember(Run, Request.UserId);
		if (bAdd ? (!After.IsValid() || IsPendingMember(*After)) : After.IsValid())
		{
			Run.Error = TEXT("The member list did not change as asked; the group's membership policy may hold the request as pending");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunSetRoles(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, true, Group))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("userId"), IdText(Request.UserId));
		const TSharedPtr<FStudioGroupMember> Member = FindMember(Run, Request.UserId);
		if (!Member)
		{
			Run.Error = FString::Printf(TEXT("-user=%lld is not a member of this group; add them with -op=addmember"), Request.UserId);
			return false;
		}
		for (const int64 RoleId : Request.RoleIds)
		{
			if (!FindRole(Run, RoleId, FString()))
			{
				Run.Error = FString::Printf(TEXT("-roles has %lld, which is not a role of this group; -op=groupdetail lists them"), RoleId);
				return false;
			}
		}
		const TArray<int64> Desired = Request.bNoRoles ? TArray<int64>() : Request.RoleIds;
		Run.Result->SetArrayField(TEXT("roleIds"), IdValues(Desired));
		if (SameIds(Member->RoleIds, Desired))
		{
			Step(FString::Printf(TEXT("user %lld already has exactly these roles in group %lld; nothing to change"), Request.UserId, Request.GroupId));
			return true;
		}
		if (!Confirm(Run, FString::Printf(TEXT("would give user %lld %d role(s) in group %lld, replacing theirs"), Request.UserId, Desired.Num(), Request.GroupId),
			FString::Printf(TEXT("giving user %lld %d role(s) in group %lld, replacing theirs"), Request.UserId, Desired.Num(), Request.GroupId),
			TEXT("This replaces the member's roles, and with them what they may do in the group")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->SetGroupMemberRoles(KindOf(Run), Request.GroupId, Request.UserId, Desired);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioGroupMember> After = FindMember(Run, Request.UserId);
		if (!After || !SameIds(After->RoleIds, Desired))
		{
			Run.Error = TEXT("The member's roles did not change as asked");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunCreateRole(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, true, Group))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("name"), Request.Name);
		Run.Result->SetNumberField(TEXT("rank"), Request.Rank);
		Run.Result->SetArrayField(TEXT("permissions"), StringValues(Request.Perms));
		if (const TSharedPtr<FStudioGroupRole> Existing = FindRole(Run, 0, Request.Name))
		{
			Run.Result->SetStringField(TEXT("roleId"), IdText(Existing->GroupRoleId));
			Run.Result->SetNumberField(TEXT("rank"), Existing->Rank);
			Run.Result->SetArrayField(TEXT("permissions"), StringValues(Existing->Permissions));
			const FString Difference = UCrowdyStudioCommandlet::FindRoleDifference(Request, *Existing);
			if (!Difference.IsEmpty())
			{
				Run.Error = Difference;
				return false;
			}
			Step(FString::Printf(TEXT("role %lld \"%s\" already exists in group %lld; nothing to create"), Existing->GroupRoleId, *Request.Name, Request.GroupId));
			return true;
		}
		if (!Confirm(Run, FString::Printf(TEXT("would create role \"%s\" (rank %d) in group %lld"), *Request.Name, Request.Rank, Request.GroupId),
			FString::Printf(TEXT("creating role \"%s\" (rank %d) in group %lld"), *Request.Name, Request.Rank, Request.GroupId), TEXT("Creating a role adds a set of permissions members can be given")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->CreateGroupRole(KindOf(Run), Request.GroupId, Request.Name, Request.Perms, Request.Rank);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioGroupRole> Created = FindRole(Run, 0, Request.Name);
		if (!Created)
		{
			Run.Error = TEXT("The new role was not in the group's roles afterwards");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		Run.Result->SetStringField(TEXT("roleId"), IdText(Created->GroupRoleId));
		return true;
	}

	bool RunUpdateRole(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, true, Group))
		{
			return false;
		}
		const TSharedPtr<FStudioGroupRole> Role = FindRole(Run, Request.RoleId, FString());
		if (!Role)
		{
			Run.Error = FString::Printf(TEXT("-role=%lld is not a role of this group; -op=groupdetail lists them"), Request.RoleId);
			return false;
		}
		if (Role->bIsSystem)
		{
			Run.Error = FString::Printf(TEXT("-role=%lld is a system role; only custom roles can be changed"), Request.RoleId);
			return false;
		}
		const FString NewName = Request.Name.IsEmpty() ? Role->RoleName : Request.Name;
		const TArray<FString> NewPerms = Request.bNoPerms ? TArray<FString>() : (Request.Perms.IsEmpty() ? Role->Permissions : Request.Perms);
		const int32 NewRank = Request.bHasRank ? Request.Rank : Role->Rank;
		Run.Result->SetStringField(TEXT("roleId"), IdText(Request.RoleId));
		Run.Result->SetStringField(TEXT("name"), NewName);
		Run.Result->SetNumberField(TEXT("rank"), NewRank);
		Run.Result->SetArrayField(TEXT("permissions"), StringValues(NewPerms));
		if (NewName.Equals(Role->RoleName, ESearchCase::CaseSensitive) && NewRank == Role->Rank && SameKeys(NewPerms, Role->Permissions))
		{
			Step(FString::Printf(TEXT("role %lld already has these values; nothing to change"), Request.RoleId));
			return true;
		}
		if (!Confirm(Run, FString::Printf(TEXT("would change role %lld to \"%s\", rank %d, %d permission(s)"), Request.RoleId, *NewName, NewRank, NewPerms.Num()),
			FString::Printf(TEXT("changing role %lld to \"%s\", rank %d, %d permission(s)"), Request.RoleId, *NewName, NewRank, NewPerms.Num()),
			TEXT("This changes what every member holding the role may do")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->UpdateGroupRole(KindOf(Run), Request.RoleId, NewName, NewPerms, NewRank);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioGroupRole> After = FindRole(Run, Request.RoleId, FString());
		if (!After || !After->RoleName.Equals(NewName, ESearchCase::CaseSensitive) || After->Rank != NewRank || !SameKeys(After->Permissions, NewPerms))
		{
			Run.Error = TEXT("The role did not take the new values");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunDeleteRole(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, true, Group))
		{
			return false;
		}
		const TSharedPtr<FStudioGroupRole> Role = FindRole(Run, Request.RoleId, FString());
		if (!Role)
		{
			Run.Error = FString::Printf(TEXT("-role=%lld is not a role of this group; -op=groupdetail lists them"), Request.RoleId);
			return false;
		}
		if (Role->bIsSystem)
		{
			Run.Error = FString::Printf(TEXT("-role=%lld is a system role; only custom roles can be deleted"), Request.RoleId);
			return false;
		}
		Run.Result->SetStringField(TEXT("roleId"), IdText(Request.RoleId));
		if (!Confirm(Run, FString::Printf(TEXT("would delete role %lld \"%s\""), Request.RoleId, *Role->RoleName), FString::Printf(TEXT("deleting role %lld \"%s\""), Request.RoleId, *Role->RoleName),
			TEXT("Deleting a role removes it from every member")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->DeleteGroupRole(KindOf(Run), Request.RoleId);
		if (!Settle(Run))
		{
			return false;
		}
		if (FindRole(Run, Request.RoleId, FString()))
		{
			Run.Error = TEXT("The role was still in the group's roles afterwards");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunUpdateGroup(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, false, Group))
		{
			return false;
		}
		const FString Name = Request.Name.Equals(Group->Name, ESearchCase::CaseSensitive) ? FString() : Request.Name;
		const FString Description = Request.Description.Equals(Group->Description, ESearchCase::CaseSensitive) ? FString() : Request.Description;
		const FString Policy = Request.Policy.Equals(Group->MembershipPolicy, ESearchCase::CaseSensitive) ? FString() : Request.Policy;
		Run.Result->SetStringField(TEXT("name"), Name.IsEmpty() ? Group->Name : Name);
		Run.Result->SetStringField(TEXT("description"), Description.IsEmpty() ? Group->Description : Description);
		Run.Result->SetStringField(TEXT("membershipPolicy"), Policy.IsEmpty() ? Group->MembershipPolicy : Policy);
		if (Name.IsEmpty() && Description.IsEmpty() && Policy.IsEmpty())
		{
			Step(FString::Printf(TEXT("%s %lld already has these values; nothing to change"), GroupNoun(Run), Request.GroupId));
			return true;
		}
		if (!Confirm(Run, FString::Printf(TEXT("would edit %s %lld"), GroupNoun(Run), Request.GroupId), FString::Printf(TEXT("editing %s %lld"), GroupNoun(Run), Request.GroupId),
			TEXT("This changes the group for everyone in it")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->UpdateGroup(KindOf(Run), Request.GroupId, Name, Description, Policy);
		if (!Settle(Run))
		{
			return false;
		}
		const TSharedPtr<FStudioGroup> After = FindGroupById(GroupList(Run), Request.GroupId);
		if (!After || (!Name.IsEmpty() && !After->Name.Equals(Name, ESearchCase::CaseSensitive)) || (!Description.IsEmpty() && !After->Description.Equals(Description, ESearchCase::CaseSensitive))
			|| (!Policy.IsEmpty() && !After->MembershipPolicy.Equals(Policy, ESearchCase::CaseSensitive)))
		{
			Run.Error = FString::Printf(TEXT("The %s did not take the new values"), GroupNoun(Run));
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunDeleteGroup(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		TSharedPtr<FStudioGroup> Group;
		if (!OpenGroup(Run, false, Group))
		{
			return false;
		}
		if (!Confirm(Run, FString::Printf(TEXT("would delete %s %lld \"%s\""), GroupNoun(Run), Request.GroupId, *Group->Name),
			FString::Printf(TEXT("deleting %s %lld \"%s\""), GroupNoun(Run), Request.GroupId, *Group->Name), TEXT("Deleting a group disbands it for every member and cannot be undone")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->DeleteGroup(KindOf(Run), Request.GroupId);
		if (!Settle(Run))
		{
			return false;
		}
		if (FindGroupById(GroupList(Run), Request.GroupId))
		{
			Run.Error = FString::Printf(TEXT("The %s was still in the app's list afterwards"), GroupNoun(Run));
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunSetPolicy(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenGamePlane(Run) || !LoadGroups(Run))
		{
			return false;
		}
		const FStudioGroupPolicy Current = Request.bChannel ? Run.Controller->GetChannelPolicy() : Run.Controller->GetTeamPolicy();
		const FString Creation = Request.Creation.IsEmpty() ? Current.CreationPolicy : Request.Creation;
		const FString Membership = Request.Policy.IsEmpty() ? Current.DefaultMembershipPolicy : Request.Policy;
		const int32 MaxMembers = Request.MaxMembers >= 0 ? Request.MaxMembers : Current.MaxMembers;
		const int32 MaxPerUser = Request.MaxGroupsPerUser >= 0 ? Request.MaxGroupsPerUser : Current.MaxGroupsPerUser;
		if (Creation.IsEmpty() || Membership.IsEmpty())
		{
			Run.Error = FString::Printf(TEXT("The app's current %s could not be read; give %s"), Creation.IsEmpty() ? TEXT("creation policy") : TEXT("default membership"),
				Creation.IsEmpty() ? TEXT("-creation") : TEXT("-membership"));
			return false;
		}
		Run.Result->SetStringField(TEXT("creationPolicy"), Creation);
		Run.Result->SetStringField(TEXT("defaultMembershipPolicy"), Membership);
		Run.Result->SetNumberField(TEXT("maxMembers"), MaxMembers);
		Run.Result->SetNumberField(TEXT("maxGroupsPerUser"), MaxPerUser);
		Run.Result->SetBoolField(TEXT("changed"), false);
		if (Creation.Equals(Current.CreationPolicy, ESearchCase::CaseSensitive) && Membership.Equals(Current.DefaultMembershipPolicy, ESearchCase::CaseSensitive)
			&& MaxMembers == Current.MaxMembers && MaxPerUser == Current.MaxGroupsPerUser)
		{
			Step(FString::Printf(TEXT("the %s policy already has these values; nothing to change"), GroupNoun(Run)));
			return true;
		}
		const FString Line = FString::Printf(TEXT("%s policy: creation=%s default membership=%s max members=%d max per user=%d (0 is unlimited)"), GroupNoun(Run), *Creation, *Membership, MaxMembers, MaxPerUser);
		if (!Confirm(Run, TEXT("would set ") + Line, TEXT("setting ") + Line, TEXT("This changes who may create and join groups for the whole app")))
		{
			return false;
		}
		Run.StatusError.Reset();
		if (Request.bChannel)
		{
			Run.Controller->SetChannelPolicy(Creation, Membership, MaxMembers, MaxPerUser);
		}
		else
		{
			Run.Controller->SetTeamPolicy(Creation, Membership, MaxMembers, MaxPerUser);
		}
		if (!Settle(Run))
		{
			return false;
		}
		const FStudioGroupPolicy& After = Request.bChannel ? Run.Controller->GetChannelPolicy() : Run.Controller->GetTeamPolicy();
		if (!After.CreationPolicy.Equals(Creation, ESearchCase::CaseSensitive) || !After.DefaultMembershipPolicy.Equals(Membership, ESearchCase::CaseSensitive)
			|| After.MaxMembers != MaxMembers || After.MaxGroupsPerUser != MaxPerUser)
		{
			Run.Error = FString::Printf(TEXT("The %s policy did not take the new values"), GroupNoun(Run));
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunGridLimits(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenGamePlane(Run))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->FetchRuntimePermissions();
		Run.Controller->FetchGridPermissionLimits(Request.GridId);
		if (!Settle(Run))
		{
			return false;
		}
		const FString Problem = UCrowdyStudioCommandlet::FindUnknownPermissionKey(Request.Keys, Run.Controller->GetRuntimePermissions());
		if (!Problem.IsEmpty())
		{
			Run.Error = Problem;
			return false;
		}
		const TArray<FString> Current = Run.Controller->GetGridWhitelistKeys();
		const TArray<FString>& Desired = Request.Keys;
		Run.Result->SetStringField(TEXT("gridId"), IdText(Request.GridId));
		Run.Result->SetArrayField(TEXT("previousKeys"), StringValues(Current));
		Run.Result->SetArrayField(TEXT("keys"), StringValues(Desired));
		Run.Result->SetBoolField(TEXT("limited"), !Desired.IsEmpty());
		Run.Result->SetBoolField(TEXT("changed"), false);
		if (SameKeys(Current, Desired))
		{
			Step(FString::Printf(TEXT("grid %lld already has this whitelist; nothing to change"), Request.GridId));
			return true;
		}
		const FString Line = Desired.IsEmpty() ? FString(TEXT("no limit")) : FString::Join(Desired, TEXT(", "));
		if (!Confirm(Run, FString::Printf(TEXT("would limit grid %lld to: %s"), Request.GridId, *Line), FString::Printf(TEXT("limiting grid %lld to: %s"), Request.GridId, *Line),
			TEXT("This caps which permission keys can ever take effect on the grid, whatever is granted")))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->SetGridPermissionLimits(Request.GridId, Desired);
		if (!Settle(Run))
		{
			return false;
		}
		if (!SameKeys(Run.Controller->GetGridWhitelistKeys(), Desired))
		{
			Run.Error = TEXT("The grid's whitelist did not take the new keys");
			return false;
		}
		Run.Result->SetBoolField(TEXT("changed"), true);
		return true;
	}

	bool RunGridGrants(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenGamePlane(Run))
		{
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->FetchGridGroupGrants(Request.GridId, Request.GroupId);
		if (!Settle(Run))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("gridId"), IdText(Request.GridId));
		Run.Result->SetStringField(TEXT("groupId"), IdText(Request.GroupId));
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const TSharedPtr<FStudioGridGroupGrant>& Grant : Run.Controller->GetGridGroupGrants())
		{
			if (!Grant.IsValid())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("roleId"), Grant->bHasRole ? IdText(Grant->GroupRoleId) : FString());
			Row->SetStringField(TEXT("permissionKey"), Grant->PermissionKey);
			Row->SetStringField(TEXT("expiresAt"), Grant->ExpiresAt);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			Step(FString::Printf(TEXT("group %lld%s holds %s on grid %lld%s%s"), Request.GroupId, Grant->bHasRole ? *FString::Printf(TEXT(" role %lld"), Grant->GroupRoleId) : TEXT(""),
				*Grant->PermissionKey, Request.GridId, Grant->ExpiresAt.IsEmpty() ? TEXT("") : TEXT(" until "), *Grant->ExpiresAt));
		}
		Run.Result->SetArrayField(TEXT("grants"), Rows);
		return true;
	}

	bool RunGridPermissions(FRun& Run)
	{
		const FCrowdyStudioCommandRequest& Request = Run.Request;
		if (!OpenGamePlane(Run))
		{
			return false;
		}
		const int64 UserId = Request.UserId != 0 ? Request.UserId : Run.SignedInUserId;
		if (UserId == 0)
		{
			Run.Error = TEXT("-user=<id> is required: this sign-in has no user of its own to read");
			return false;
		}
		Run.StatusError.Reset();
		Run.Controller->FetchGridUserPermissions(Request.GridId, UserId);
		if (!Settle(Run))
		{
			return false;
		}
		Run.Result->SetStringField(TEXT("gridId"), IdText(Request.GridId));
		Run.Result->SetStringField(TEXT("userId"), IdText(UserId));
		Run.Result->SetArrayField(TEXT("effectivePermissionKeys"), StringValues(Run.Controller->GetGridUserEffectiveKeys()));
		Step(FString::Printf(TEXT("user %lld holds on grid %lld: %s"), UserId, Request.GridId,
			Run.Controller->GetGridUserEffectiveKeys().IsEmpty() ? TEXT("nothing") : *FString::Join(Run.Controller->GetGridUserEffectiveKeys(), TEXT(", "))));
		return true;
	}

	bool Execute(FRun& Run)
	{
		const FString& Op = Run.Request.Op;
		if (Op == TEXT("groupdetail"))
		{
			return RunGroupDetail(Run);
		}
		if (Op == TEXT("addmember") || Op == TEXT("removemember"))
		{
			return RunMember(Run);
		}
		if (Op == TEXT("setroles"))
		{
			return RunSetRoles(Run);
		}
		if (Op == TEXT("createrole"))
		{
			return RunCreateRole(Run);
		}
		if (Op == TEXT("updaterole"))
		{
			return RunUpdateRole(Run);
		}
		if (Op == TEXT("deleterole"))
		{
			return RunDeleteRole(Run);
		}
		if (Op == TEXT("updategroup"))
		{
			return RunUpdateGroup(Run);
		}
		if (Op == TEXT("deletegroup"))
		{
			return RunDeleteGroup(Run);
		}
		if (Op == TEXT("setteampolicy") || Op == TEXT("setchannelpolicy"))
		{
			return RunSetPolicy(Run);
		}
		if (Op == TEXT("gridlimits"))
		{
			return RunGridLimits(Run);
		}
		if (Op == TEXT("gridgrants"))
		{
			return RunGridGrants(Run);
		}
		if (Op == TEXT("gridpermissions"))
		{
			return RunGridPermissions(Run);
		}
		if (Op == TEXT("apps"))
		{
			return RunApps(Run);
		}
		if (Op == TEXT("createapp"))
		{
			return RunCreateApp(Run);
		}
		if (Op == TEXT("sync"))
		{
			return RunSync(Run);
		}
		if (Op == TEXT("features"))
		{
			return RunFeatures(Run);
		}
		if (Op == TEXT("definefeature"))
		{
			return RunDefineFeature(Run);
		}
		if (Op == TEXT("grantfeature") || Op == TEXT("revokefeature"))
		{
			return RunTierFeature(Run);
		}
		if (Op == TEXT("teams") || Op == TEXT("channels"))
		{
			return RunGroups(Run);
		}
		if (Op == TEXT("createteam") || Op == TEXT("createchannel"))
		{
			return RunCreateGroup(Run);
		}
		if (Op == TEXT("grids"))
		{
			return RunGrids(Run);
		}
		if (Op == TEXT("creategrid"))
		{
			return RunCreateGrid(Run);
		}
		return RunGridGrant(Run);
	}

	bool WriteResult(const TSharedRef<FJsonObject>& Result, const FString& OutPath)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), OutPath);
		FString Text;
		FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Text));
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogCrowdyStudio, Error, TEXT("Crowdy Studio: could not write %s"), *Path);
			return false;
		}
		Step(FString::Printf(TEXT("wrote %s"), *Path));
		return true;
	}

	bool Finish(FRun& Run, const FString& Params, bool bOk)
	{
		Run.Result->SetStringField(TEXT("op"), Run.Request.Op);
		Run.Result->SetBoolField(TEXT("ok"), bOk);
		Run.Result->SetStringField(TEXT("error"), Run.Error);
		UE_CLOG(bOk, LogCrowdyStudio, Display, TEXT("Crowdy Studio: done"));
		UE_CLOG(!bOk, LogCrowdyStudio, Error, TEXT("Crowdy Studio: %s"), *Run.Error);
		FString OutPath;
		if (!FParse::Value(*Params, TEXT("-out="), OutPath))
		{
			return bOk;
		}
		return WriteResult(Run.Result, OutPath) && bOk;
	}
}

UCrowdyStudioCommandlet::UCrowdyStudioCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	// Otherwise any error line logged during the run turns a success into exit code 1.
	UseCommandletResultAsExitCode = true;
}

int32 UCrowdyStudioCommandlet::Main(const FString& Params)
{
	using namespace CrowdyStudioCommandletDetail;
	FRun Run;
	if (!ParseRequest(Params, Run.Request, Run.Error))
	{
		return Finish(Run, Params, false) ? 0 : 1;
	}
	return Finish(Run, Params, Execute(Run)) ? 0 : 1;
}

const TCHAR* UCrowdyStudioCommandlet::GetUsage()
{
	return CrowdyStudioCommandletDetail::Usage;
}

bool UCrowdyStudioCommandlet::ParseRequest(const FString& Params, FCrowdyStudioCommandRequest& OutRequest, FString& OutError)
{
	using namespace CrowdyStudioCommandletDetail;
	OutRequest = FCrowdyStudioCommandRequest();
	OutError.Reset();
	ReadSwitch(Params, TEXT("op"), OutRequest.Op);
	OutRequest.Op.ToLowerInline();
	OutRequest.bYes = FParse::Param(*Params, TEXT("yes"));
	OutRequest.bEditorOpen = FParse::Param(*Params, TEXT("editoropen"));
	if (OutRequest.Op == TEXT("apps"))
	{
		return true;
	}
	if (OutRequest.Op == TEXT("sync"))
	{
		return ReadId(Params, TEXT("app"), OutRequest.AppId, OutError);
	}
	if (OutRequest.Op == TEXT("createapp"))
	{
		return ParseCreateApp(Params, OutRequest, OutError);
	}
	if (IsAppScopedOp(OutRequest.Op))
	{
		return ParseAppScoped(Params, OutRequest, OutError);
	}
	OutError = FString::Printf(TEXT("Unknown or missing -op '%s'. Usage: %s"), *OutRequest.Op, Usage);
	return false;
}

bool UCrowdyStudioCommandlet::NeedsConfirmation(const FCrowdyStudioCommandRequest& Request)
{
	static const TCHAR* const ChangingOps[] = { TEXT("createapp"), TEXT("sync"), TEXT("definefeature"), TEXT("grantfeature"), TEXT("revokefeature"),
		TEXT("createteam"), TEXT("createchannel"), TEXT("creategrid"), TEXT("grantgrid"), TEXT("revokegrid"), TEXT("addmember"), TEXT("removemember"), TEXT("setroles"),
		TEXT("createrole"), TEXT("updaterole"), TEXT("deleterole"), TEXT("updategroup"), TEXT("deletegroup"), TEXT("setteampolicy"), TEXT("setchannelpolicy"), TEXT("gridlimits") };
	return !Request.bYes && Algo::AnyOf(ChangingOps, [&Request](const TCHAR* Name) { return Request.Op == Name; });
}

int64 UCrowdyStudioCommandlet::ResolveAppId(const FCrowdyStudioCommandRequest& Request, int64 ProjectAppId, FString& OutError)
{
	OutError.Reset();
	if (Request.AppId > 0)
	{
		return Request.AppId;
	}
	if (ProjectAppId > 0)
	{
		return ProjectAppId;
	}
	OutError = TEXT("-app=<id> is required: the project has no synced AppID to default to. -op=apps lists the apps and -op=sync writes one into the project");
	return 0;
}

FString UCrowdyStudioCommandlet::FindAccessProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioAppFeature>>& Features,
	const TArray<TSharedPtr<FStudioAccessTier>>& Tiers)
{
	if (Request.Op != TEXT("grantfeature") && Request.Op != TEXT("revokefeature"))
	{
		return FString();
	}
	if (!Tiers.ContainsByPredicate([&Request](const TSharedPtr<FStudioAccessTier>& Tier) { return Tier.IsValid() && Tier->TierId == Request.TierId; }))
	{
		return FString::Printf(TEXT("-tier=%lld is not one of this app's access tiers; -op=features lists them"), Request.TierId);
	}
	const bool bDefined = Features.ContainsByPredicate([&Request](const TSharedPtr<FStudioAppFeature>& Feature)
	{
		return Feature.IsValid() && Feature->FeatureKey.Equals(Request.Key, ESearchCase::CaseSensitive);
	});
	if (Request.Op == TEXT("grantfeature") && !bDefined)
	{
		return FString::Printf(TEXT("-key=%s is not a feature of this app; define it first with -op=definefeature"), *Request.Key);
	}
	return FString();
}

bool UCrowdyStudioCommandlet::IsGranted(const TArray<TSharedPtr<FStudioTierFeature>>& Grants, int64 TierId, const FString& Key)
{
	return Grants.ContainsByPredicate([TierId, &Key](const TSharedPtr<FStudioTierFeature>& Grant)
	{
		return Grant.IsValid() && Grant->TierId == TierId && Grant->FeatureKey.Equals(Key, ESearchCase::CaseSensitive);
	});
}

FString UCrowdyStudioCommandlet::FindDefineProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioAppFeature>>& Features)
{
	const TSharedPtr<FStudioAppFeature>* Saved = Features.FindByPredicate([&Request](const TSharedPtr<FStudioAppFeature>& Feature)
	{
		return Feature.IsValid() && Feature->FeatureKey.Equals(Request.Key, ESearchCase::CaseSensitive);
	});
	if (!Saved)
	{
		return TEXT("The feature was not in the app's feature list after it was saved");
	}
	if (!Request.Description.IsEmpty() && !(*Saved)->Description.Equals(Request.Description, ESearchCase::CaseSensitive))
	{
		return FString::Printf(TEXT("The feature's description is \"%s\" after it was saved, not the requested \"%s\""), *(*Saved)->Description, *Request.Description);
	}
	return FString();
}

FString UCrowdyStudioCommandlet::FindGroupDifference(const FCrowdyStudioCommandRequest& Request, const FStudioGroup& Existing)
{
	TArray<FString> Differences;
	if (!Request.Description.IsEmpty() && !Request.Description.Equals(Existing.Description, ESearchCase::CaseSensitive))
	{
		Differences.Add(FString::Printf(TEXT("description is \"%s\", not \"%s\""), *Existing.Description, *Request.Description));
	}
	if (!Request.Policy.IsEmpty() && !Request.Policy.Equals(Existing.MembershipPolicy, ESearchCase::CaseSensitive))
	{
		Differences.Add(FString::Printf(TEXT("membership policy is %s, not %s"), *Existing.MembershipPolicy, *Request.Policy));
	}
	if (Differences.IsEmpty())
	{
		return FString();
	}
	const TCHAR* Kind = Request.bChannel ? TEXT("channel") : TEXT("team");
	return FString::Printf(TEXT("%s %lld \"%s\" already exists, but its %s; change it with -op=updategroup -kind=%s -group=%lld"), Kind, Existing.GroupId, *Existing.Name,
		*FString::Join(Differences, TEXT(" and its ")), Kind, Existing.GroupId);
}

FString UCrowdyStudioCommandlet::FindRoleDifference(const FCrowdyStudioCommandRequest& Request, const FStudioGroupRole& Existing)
{
	using namespace CrowdyStudioCommandletDetail;
	TArray<FString> Differences;
	if ((!Request.Perms.IsEmpty() || Request.bNoPerms) && !SameKeys(Request.Perms, Existing.Permissions))
	{
		Differences.Add(FString::Printf(TEXT("permissions are \"%s\", not \"%s\""), *FString::Join(Existing.Permissions, TEXT(", ")), *FString::Join(Request.Perms, TEXT(", "))));
	}
	if (Request.bHasRank && Request.Rank != Existing.Rank)
	{
		Differences.Add(FString::Printf(TEXT("rank is %d, not %d"), Existing.Rank, Request.Rank));
	}
	if (Differences.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("role %lld \"%s\" already exists, but its %s; change it with -op=updaterole -kind=%s -group=%lld -role=%lld"), Existing.GroupRoleId, *Existing.RoleName,
		*FString::Join(Differences, TEXT(" and its ")), Request.bChannel ? TEXT("channel") : TEXT("team"), Request.GroupId, Existing.GroupRoleId);
}

FString UCrowdyStudioCommandlet::FindGridGroupProblem(int64 GroupId, const TArray<TSharedPtr<FStudioGroup>>& Teams, const TArray<TSharedPtr<FStudioGroup>>& Channels)
{
	using namespace CrowdyStudioCommandletDetail;
	if (FindGroupById(Teams, GroupId) || FindGroupById(Channels, GroupId))
	{
		return FString();
	}
	return FString::Printf(TEXT("-group=%lld is neither a team nor a channel of this app; -op=teams and -op=channels list them"), GroupId);
}

FString UCrowdyStudioCommandlet::FindUnrevokedUserKey(const FCrowdyStudioCommandRequest& Request, const TArray<FString>& EffectiveKeys)
{
	for (const FString& Key : Request.Keys)
	{
		if (EffectiveKeys.ContainsByPredicate([&Key](const FString& Held) { return Held.Equals(Key, ESearchCase::CaseSensitive); }))
		{
			return FString::Printf(TEXT("-keys has %s, which user %lld still holds on grid %lld after the revoke; a group grant may be keeping it, see -op=gridgrants"), *Key, Request.UserId, Request.GridId);
		}
	}
	return FString();
}

FString UCrowdyStudioCommandlet::FindUnrevokedGroupGrant(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioGridGroupGrant>>& Grants)
{
	const bool bRoleScope = Request.RoleId != 0;
	for (const TSharedPtr<FStudioGridGroupGrant>& Grant : Grants)
	{
		if (!Grant.IsValid() || Grant->bHasRole != bRoleScope || (bRoleScope && Grant->GroupRoleId != Request.RoleId))
		{
			continue;
		}
		const bool bNamed = Request.bAllKeys || Request.Keys.ContainsByPredicate([&Grant](const FString& Key) { return Grant->PermissionKey.Equals(Key, ESearchCase::CaseSensitive); });
		if (bNamed)
		{
			return FString::Printf(TEXT("group %lld still holds %s on grid %lld after the revoke"), Request.GroupId, *Grant->PermissionKey, Request.GridId);
		}
	}
	return FString();
}

FString UCrowdyStudioCommandlet::FindUnknownPermissionKey(const TArray<FString>& Keys, const TArray<FString>& Catalog)
{
	if (Catalog.IsEmpty())
	{
		return FString();
	}
	for (const FString& Key : Keys)
	{
		if (!Catalog.ContainsByPredicate([&Key](const FString& Known) { return Known.Equals(Key, ESearchCase::CaseSensitive); }))
		{
			return FString::Printf(TEXT("-keys has %s, which is not a runtime permission key; choose from: %s"), *Key, *FString::Join(Catalog, TEXT(", ")));
		}
	}
	return FString();
}

bool UCrowdyStudioCommandlet::IsMembershipPolicy(const FString& Policy)
{
	static const TCHAR* const Policies[] = { TEXT("open"), TEXT("request"), TEXT("invite"), TEXT("admin") };
	return Algo::AnyOf(Policies, [&Policy](const TCHAR* Name) { return Policy.Equals(Name, ESearchCase::CaseSensitive); });
}

FString UCrowdyStudioCommandlet::FindCreateProblem(const FCrowdyStudioCommandRequest& Request, const TArray<TSharedPtr<FStudioOrg>>& Organizations,
	const TArray<FStudioDatacenter>& Datacenters)
{
	const bool bKnownOrg = Organizations.ContainsByPredicate([&Request](const TSharedPtr<FStudioOrg>& Org) { return Org.IsValid() && Org->OrgId == Request.OrgId; });
	if (!bKnownOrg)
	{
		return FString::Printf(TEXT("-org=%lld is not one of this account's organizations; -op=apps lists them"), Request.OrgId);
	}
	TArray<FString> Placeable;
	for (const FStudioDatacenter& Dc : Datacenters)
	{
		if (Dc.bPlaceable)
		{
			Placeable.Add(Dc.Code);
		}
	}
	if (Placeable.IsEmpty())
	{
		return TEXT("This deployment has no datacenter that can take a new app right now, so creation will be refused. Ask the server operator.");
	}
	if (!Placeable.ContainsByPredicate([&Request](const FString& Code) { return Code.Equals(Request.Datacenter, ESearchCase::CaseSensitive); }))
	{
		return FString::Printf(TEXT("-datacenter=%s is not a datacenter an app can be placed in; choose one of: %s"), *Request.Datacenter, *FString::Join(Placeable, TEXT(", ")));
	}
	return FString();
}

FString UCrowdyStudioCommandlet::FindSyncWriteProblem(const FCrowdyStudioCommandRequest& Request, bool bEditorRunning)
{
	if (!bEditorRunning || Request.bEditorOpen)
	{
		return FString();
	}
	return TEXT("An Unreal Editor is running. Close it first: it holds this project's settings in memory and would write its own copy over the synced app. Pass -editoropen to write anyway.");
}

TSharedPtr<FStudioApp> UCrowdyStudioCommandlet::FindApp(const TArray<TSharedPtr<FStudioApp>>& Apps, int64 AppId)
{
	for (const TSharedPtr<FStudioApp>& App : Apps)
	{
		if (App.IsValid() && App->AppId == AppId)
		{
			return App;
		}
	}
	return nullptr;
}

FStudioSettingsSnapshot UCrowdyStudioCommandlet::ProposeSync(const FStudioApp& App)
{
	return FCrowdyConfigSync::BuildProposedSettings(App);
}
