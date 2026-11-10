#include "Subsystem/CrowdyClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags ClaimsTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int64 ClaimsTestApp = 31337;
	constexpr int64 ClaimsTestMe = 42;

	FString ClaimsPoint(int64 X, int64 Y, int64 Z)
	{
		return FString::Printf(TEXT("{\"x\":\"%lld\",\"y\":\"%lld\",\"z\":\"%lld\"}"), X, Y, Z);
	}

	FString ClaimAnswer(int64 GridId, int64 X, int64 Y, int64 Z)
	{
		return FString::Printf(
			TEXT("{\"data\":{\"claimGridChunk\":{\"gridId\":\"%lld\",\"lowChunk\":%s,\"highChunk\":%s,\"policy\":\"SELF_CLAIM\",")
			TEXT("\"ownership\":{\"ownerKind\":\"USER\",\"ownerRef\":\"42\",\"acquiredVia\":\"self_claim_chunk\"},\"moddable\":false,\"effectivePermissionKeys\":[]}}}"),
			GridId, *ClaimsPoint(X, Y, Z), *ClaimsPoint(X, Y, Z));
	}

	FString ClaimsError(const TCHAR* Code, const TCHAR* Reason = nullptr, const TCHAR* Message = TEXT("No."))
	{
		const FString ReasonField = Reason ? FString::Printf(TEXT(",\"reason\":\"%s\""), Reason) : FString();
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"%s\",\"extensions\":{\"code\":\"%s\"%s}}],\"data\":null}"), Message, Code, *ReasonField);
	}

	FString ClaimsGrid(int64 GridId, int64 LowX, int64 HighX)
	{
		return FString::Printf(TEXT("{\"appId\":\"31337\",\"gridId\":\"%lld\",\"lowChunk\":%s,\"highChunk\":%s}"),
			GridId, *ClaimsPoint(LowX, 0, 0), *ClaimsPoint(HighX, 0, 0));
	}

	FString ClaimsOwner(int64 GridId, const TCHAR* Via, int64 Owner)
	{
		return FString::Printf(
			TEXT("{\"data\":{\"gridOwnership\":{\"gridOwnershipId\":\"1\",\"gridId\":\"%lld\",\"appId\":\"31337\",\"ownerKind\":\"USER\",")
			TEXT("\"ownerRef\":\"%lld\",\"tenure\":\"permanent\",\"acquiredVia\":\"%s\",\"acquiredAt\":null,\"expiresAt\":null}}}"),
			GridId, Owner, Via);
	}

	struct FClaimsRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyClaims> Claims;
		TSharedPtr<FCrowdyCppClient> Client;
		int32 Answers = 0;
		FCrowdyChunkClaim Claim;
		FCrowdyClaimRelease Release;
		FCrowdyClaimSearch Search;

		explicit FClaimsRig(const FString& Body, int32 Status = 200, bool bSignedIn = true)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Claims(NewObject<UCrowdyClaims>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(Body, Status))
		{
			// Signed out, the subsystem has no stand-in client, so it asks its game instance, which has no session.
			if (Client.IsValid() && bSignedIn)
			{
				Client->SetGameToken(TEXT("app-token"));
			}
			Claims->InitializeForTest(bSignedIn ? Client : nullptr, ClaimsTestApp, ClaimsTestMe);
		}

		void Script(TArray<TPair<int32, FString>> Responses)
		{
			Client->SetTestResponseScript(MoveTemp(Responses));
		}

		void ClaimChunk(int64 X, int64 Y, int64 Z)
		{
			Claims->ClaimChunk(X, Y, Z, [this](const FCrowdyChunkClaim& Answer) { Claim = Answer; ++Answers; });
		}

		void ReleaseClaim(int64 GridId)
		{
			Claims->ReleaseClaim(GridId, [this](const FCrowdyClaimRelease& Answer) { Release = Answer; ++Answers; });
		}

		void FindClaims(int64 LowX, int64 HighX, int64 Side = 0)
		{
			Claims->FindClaims(LowX, 0, 0, HighX, Side, Side, [this](const FCrowdyClaimSearch& Answer) { Search = Answer; ++Answers; });
		}

		void Pump()
		{
			for (int32 Step = 0; Step < 400 && Answers == 0; ++Step)
			{
				Client->Poll();
				FTSTicker::GetCoreTicker().Tick(0.0f);
			}
		}

		FString SentBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsClaimAnswersGridTest, "CrowdySDK.Claims.ClaimAnswersTheNewGrid", ClaimsTestFlags)
bool FCrowdyClaimsClaimAnswersGridTest::RunTest(const FString& Parameters)
{
	FClaimsRig Rig(ClaimAnswer(901, 3, -4, 0));
	Rig.ClaimChunk(3, -4, 0);
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
	Rig.Pump();
	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestTrue(TEXT("the claim succeeded"), Rig.Claim.Result == ECrowdyClaimResult::Success);
	TestEqual(TEXT("the new grid"), Rig.Claim.GridId, static_cast<int64>(901));
	TestEqual(TEXT("the chunk asked for"), Rig.Claim.ChunkY, static_cast<int64>(-4));
	const FString Body = Rig.SentBody();
	TestTrue(TEXT("the claim operation was sent"), Body.Contains(TEXT("claimGridChunk")));
	TestTrue(TEXT("the chunk travels as BigInt strings"), Body.Contains(TEXT("\"y\":\"-4\"")));
	TestTrue(TEXT("the app travels as a BigInt string"), Body.Contains(TEXT("\"appId\":\"31337\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsMapsCodesTest, "CrowdySDK.Claims.ClaimMapsTheGridCodes", ClaimsTestFlags)
bool FCrowdyClaimsMapsCodesTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FString Body;
		int32 Status;
		ECrowdyClaimResult Expected;
		const TCHAR* What;
	};
	const FCase Cases[] = {
		{ClaimsError(TEXT("FORBIDDEN"), TEXT("GRID_NOT_CLAIMABLE")), 200, ECrowdyClaimResult::NotClaimable, TEXT("not claimable in the reason")},
		{ClaimsError(TEXT("GRID_NOT_CLAIMABLE")), 200, ECrowdyClaimResult::NotClaimable, TEXT("not claimable in the code")},
		{ClaimsError(TEXT("FORBIDDEN"), TEXT("GRID_ALREADY_CLAIMED")), 200, ECrowdyClaimResult::AlreadyClaimed, TEXT("already claimed in the reason")},
		{ClaimsError(TEXT("GRID_ALREADY_CLAIMED")), 200, ECrowdyClaimResult::AlreadyClaimed, TEXT("already claimed in the code")},
		{ClaimsError(TEXT("CONFLICT"), nullptr, TEXT("GRID_ALREADY_CLAIMED: the chunk already has a grid claim")), 200, ECrowdyClaimResult::AlreadyClaimed,
			TEXT("already claimed at the start of a conflict's message, as the live platform answers")},
		{ClaimsError(TEXT("CONFLICT"), nullptr, TEXT("not GRID_ALREADY_CLAIMED: elsewhere")), 200, ECrowdyClaimResult::Refused, TEXT("the code anywhere but the start")},
		{ClaimsError(TEXT("FORBIDDEN"), TEXT("CLAIM_POLICY")), 200, ECrowdyClaimResult::Refused, TEXT("the claim policy")},
		{ClaimsError(TEXT("CONFLICT")), 200, ECrowdyClaimResult::Refused, TEXT("a competing claim")},
		{FString(), 503, ECrowdyClaimResult::Failed, TEXT("a request that did not complete")},
		{TEXT("{\"data\":{\"claimGridChunk\":{\"gridId\":\"x\"}}}"), 200, ECrowdyClaimResult::BadReply, TEXT("a malformed answer")},
		{ClaimAnswer(901, 9, 9, 9), 200, ECrowdyClaimResult::BadReply, TEXT("a claim over another chunk")},
		{ClaimAnswer(901, 3, -4, 0).Replace(TEXT("\"901\""), TEXT("\"901abc\"")), 200, ECrowdyClaimResult::BadReply, TEXT("a grid id with trailing text")},
		{ClaimAnswer(901, 3, -4, 0).Replace(TEXT("\"901\""), TEXT("\"99999999999999999999\"")), 200, ECrowdyClaimResult::BadReply, TEXT("a grid id past int64")},
	};
	for (const FCase& Case : Cases)
	{
		FClaimsRig Rig(Case.Body, Case.Status);
		Rig.ClaimChunk(3, -4, 0);
		Rig.Pump();
		TestTrue(FString::Printf(TEXT("%s ends %d"), Case.What, static_cast<int32>(Case.Expected)), Rig.Claim.Result == Case.Expected);
		TestEqual(FString::Printf(TEXT("%s carries no grid"), Case.What), Rig.Claim.GridId, static_cast<int64>(0));
	}
	FClaimsRig Policy(ClaimsError(TEXT("FORBIDDEN"), TEXT("CLAIM_POLICY")));
	Policy.ClaimChunk(3, -4, 0);
	Policy.Pump();
	TestTrue(TEXT("a refusal's message names the code and the reason"), Policy.Claim.Message.Contains(TEXT("FORBIDDEN CLAIM_POLICY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsNotSignedInTest, "CrowdySDK.Claims.NoTokenIsNotSignedInLater", ClaimsTestFlags)
bool FCrowdyClaimsNotSignedInTest::RunTest(const FString& Parameters)
{
	FClaimsRig Rig(ClaimAnswer(901, 3, -4, 0), 200, false);
	Rig.ClaimChunk(3, -4, 0);
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
	Rig.Pump();
	TestTrue(TEXT("without an app token the claim is not signed in"), Rig.Claim.Result == ECrowdyClaimResult::NotSignedIn);
	FString Url;
	FString Authorization;
	TestFalse(TEXT("nothing was sent"), Rig.Client->GetLastTestRequest(Url, Authorization));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsClosedClientTest, "CrowdySDK.Claims.ClosedClientAnswersLater", ClaimsTestFlags)
bool FCrowdyClaimsClosedClientTest::RunTest(const FString& Parameters)
{
	// A closed client fails a request as it is made; the claim still answers on a later tick, never inside the call.
	FClaimsRig Rig(ClaimAnswer(901, 3, -4, 0));
	Rig.Client->Close();
	Rig.ClaimChunk(3, -4, 0);
	TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
	Rig.Pump();
	TestEqual(TEXT("one answer"), Rig.Answers, 1);
	TestTrue(TEXT("a request the client could not make fails"), Rig.Claim.Result == ECrowdyClaimResult::Failed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsReleaseTest, "CrowdySDK.Claims.ReleaseAnswersOnce", ClaimsTestFlags)
bool FCrowdyClaimsReleaseTest::RunTest(const FString& Parameters)
{
	const auto Released = [](int64 GridId, bool bReleased)
	{
		return FString::Printf(TEXT("{\"data\":{\"releaseClaimedGrid\":{\"gridId\":\"%lld\",\"lowChunk\":%s,\"highChunk\":%s,\"policy\":\"SELF_CLAIM\",\"released\":%s}}}"),
			GridId, *ClaimsPoint(1, 0, 0), *ClaimsPoint(1, 0, 0), bReleased ? TEXT("true") : TEXT("false"));
	};
	{
		FClaimsRig Rig(Released(77, true));
		Rig.ReleaseClaim(77);
		Rig.Pump();
		TestTrue(TEXT("a release succeeds"), Rig.Release.Result == ECrowdyClaimResult::Success);
		TestTrue(TEXT("the grid travels as a BigInt string"), Rig.SentBody().Contains(TEXT("\"gridId\":\"77\"")));
	}
	{
		FClaimsRig Rig(Released(77, false));
		Rig.ReleaseClaim(77);
		Rig.Pump();
		TestTrue(TEXT("a release the platform did not make is refused"), Rig.Release.Result == ECrowdyClaimResult::Refused);
	}
	{
		FClaimsRig Rig(Released(78, true));
		Rig.ReleaseClaim(77);
		Rig.Pump();
		TestTrue(TEXT("a release of another grid is a bad reply"), Rig.Release.Result == ECrowdyClaimResult::BadReply);
	}
	{
		FClaimsRig Rig(ClaimsError(TEXT("FORBIDDEN")));
		Rig.ReleaseClaim(77);
		Rig.Pump();
		TestTrue(TEXT("a grid not owned is refused"), Rig.Release.Result == ECrowdyClaimResult::Refused);
		TestEqual(TEXT("the answer names the grid asked for"), Rig.Release.GridId, static_cast<int64>(77));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsSearchTest, "CrowdySDK.Claims.FindClaimsKeepsOnlyClaims", ClaimsTestFlags)
bool FCrowdyClaimsSearchTest::RunTest(const FString& Parameters)
{
	const FString Grids = FString::Printf(TEXT("{\"data\":{\"nearbyGrids\":[%s]}}"), *FString::Join(TArray<FString>{
		ClaimsGrid(1, -1000, 1000),
		ClaimsGrid(9, 3, 3),
		ClaimsGrid(5, 1, 1),
		ClaimsGrid(7, 2, 2),
		ClaimsGrid(11, 40, 40),
		ClaimsGrid(13, 4, 4),
		ClaimsGrid(7, 2, 2),
	}, TEXT(",")));
	FClaimsRig Rig(TEXT("{}"));
	Rig.Script({
		{200, Grids},
		{200, ClaimsOwner(5, TEXT("studio_grant"), ClaimsTestMe)},
		{200, ClaimsOwner(7, TEXT("self_claim_chunk"), ClaimsTestMe)},
		{200, ClaimsOwner(9, TEXT("self_claim_chunk"), 77)},
		{200, TEXT("{\"data\":{\"gridOwnership\":null}}")},
	});
	Rig.FindClaims(0, 8);
	Rig.Pump();
	TestTrue(TEXT("the search succeeded"), Rig.Search.Result == ECrowdyClaimResult::Success);
	if (!TestEqual(TEXT("only the two players' claims, the world grid, plot, outside grid and unowned grid left out"), Rig.Search.Claims.Num(), 2))
	{
		return true;
	}
	TestEqual(TEXT("in grid id order: mine first"), Rig.Search.Claims[0].GridId, static_cast<int64>(7));
	TestTrue(TEXT("mine is mine"), Rig.Search.Claims[0].bOwnedByMe);
	TestEqual(TEXT("its chunk"), Rig.Search.Claims[0].ChunkX, static_cast<int64>(2));
	TestEqual(TEXT("then another player's"), Rig.Search.Claims[1].GridId, static_cast<int64>(9));
	TestEqual(TEXT("with its owner"), Rig.Search.Claims[1].OwnerUserId, static_cast<int64>(77));
	TestFalse(TEXT("which is not mine"), Rig.Search.Claims[1].bOwnedByMe);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsSearchManyTest, "CrowdySDK.Claims.FindClaimsReadsMoreOwnersThanInFlight", ClaimsTestFlags)
bool FCrowdyClaimsSearchManyTest::RunTest(const FString& Parameters)
{
	TArray<FString> Listed;
	TArray<TPair<int32, FString>> Script;
	for (int64 GridId = 10; GridId >= 3; --GridId)
	{
		Listed.Add(ClaimsGrid(GridId, GridId - 3, GridId - 3));
	}
	Script.Add({200, FString::Printf(TEXT("{\"data\":{\"nearbyGrids\":[%s]}}"), *FString::Join(Listed, TEXT(",")))});
	for (int64 GridId = 3; GridId <= 10; ++GridId)
	{
		Script.Add({200, ClaimsOwner(GridId, TEXT("self_claim_chunk"), GridId % 2 == 0 ? ClaimsTestMe : 77)});
	}
	FClaimsRig Rig(TEXT("{}"));
	Rig.Script(MoveTemp(Script));
	Rig.FindClaims(0, 8);
	Rig.Pump();
	TestTrue(TEXT("the search succeeded"), Rig.Search.Result == ECrowdyClaimResult::Success);
	if (!TestEqual(TEXT("all eight claims, past the reads kept in flight"), Rig.Search.Claims.Num(), 8))
	{
		return true;
	}
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const FCrowdyChunkClaimInfo& Claim = Rig.Search.Claims[Index];
		TestEqual(FString::Printf(TEXT("claim %d in grid id order"), Index), Claim.GridId, static_cast<int64>(Index + 3));
		TestEqual(FString::Printf(TEXT("claim %d keeps its own owner"), Index), Claim.bOwnedByMe, Claim.GridId % 2 == 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsSearchFailsWholeTest, "CrowdySDK.Claims.FindClaimsFailsWhole", ClaimsTestFlags)
bool FCrowdyClaimsSearchFailsWholeTest::RunTest(const FString& Parameters)
{
	const FString Grids = FString::Printf(TEXT("{\"data\":{\"nearbyGrids\":[%s,%s]}}"), *ClaimsGrid(7, 2, 2), *ClaimsGrid(9, 3, 3));
	FClaimsRig Rig(TEXT("{}"));
	Rig.Script({
		{200, Grids},
		{200, ClaimsOwner(7, TEXT("self_claim_chunk"), ClaimsTestMe)},
		{200, ClaimsError(TEXT("FORBIDDEN"))},
	});
	Rig.FindClaims(0, 8);
	Rig.Pump();
	TestTrue(TEXT("a failed ownership read fails the search"), Rig.Search.Result == ECrowdyClaimResult::Refused);
	TestEqual(TEXT("never a partial list"), Rig.Search.Claims.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsSearchBoxTest, "CrowdySDK.Claims.FindClaimsBoxIsBounded", ClaimsTestFlags)
bool FCrowdyClaimsSearchBoxTest::RunTest(const FString& Parameters)
{
	{
		FClaimsRig Rig(TEXT("{\"data\":{\"nearbyGrids\":[]}}"));
		Rig.FindClaims(0, 16, 15);
		TestEqual(TEXT("nothing answers inside the call"), Rig.Answers, 0);
		Rig.Pump();
		TestTrue(TEXT("a box of 17 x 16 x 16 chunks fails"), Rig.Search.Result == ECrowdyClaimResult::Failed);
		FString Url;
		FString Authorization;
		TestFalse(TEXT("without a request"), Rig.Client->GetLastTestRequest(Url, Authorization));
	}
	{
		FClaimsRig Rig(TEXT("{\"data\":{\"nearbyGrids\":[]}}"));
		Rig.FindClaims(0, 15, 15);
		Rig.Pump();
		TestTrue(TEXT("a box of 16 x 16 x 16 chunks is searched"), Rig.Search.Result == ECrowdyClaimResult::Success);
	}
	{
		FClaimsRig Rig(TEXT("{\"data\":{\"nearbyGrids\":[]}}"));
		Rig.Claims->FindClaims(MIN_int64, 0, 0, MAX_int64, 0, 0, [&Rig](const FCrowdyClaimSearch& Answer) { Rig.Search = Answer; ++Rig.Answers; });
		Rig.Pump();
		TestTrue(TEXT("a box spanning every int64 chunk fails, not overflows"), Rig.Search.Result == ECrowdyClaimResult::Failed);
	}
	{
		FClaimsRig Rig(TEXT("{\"data\":{\"nearbyGrids\":[]}}"));
		Rig.FindClaims(5, 4);
		Rig.Pump();
		TestTrue(TEXT("a box whose low corner is above its high one fails"), Rig.Search.Result == ECrowdyClaimResult::Failed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClaimsAfterEndTest, "CrowdySDK.Claims.AnswerAfterTheEndIsDropped", ClaimsTestFlags)
bool FCrowdyClaimsAfterEndTest::RunTest(const FString& Parameters)
{
	FClaimsRig Rig(ClaimAnswer(901, 3, -4, 0));
	Rig.ClaimChunk(3, -4, 0);
	Rig.Claims->Deinitialize();
	Rig.Pump();
	TestEqual(TEXT("an answer arriving after the subsystem ended runs nothing"), Rig.Answers, 0);
	return true;
}

#endif
