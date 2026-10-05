#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FCrowdyCppAdminClientHost;
class FCrowdyCppClient;

/** The platform's server-code operations for one app, as the signed-in developer. Game thread only; every callback runs on the game thread. */
namespace CrowdyExecDeveloper
{
	struct FBuildFile
	{
		FString Path;
		FString Content;
	};

	struct FBuildCrate
	{
		FString Name;
		TArray<FBuildFile> Files;
	};

	struct FBuildArtifact
	{
		FString Crate;
		FString Digest;
		int64 SizeBytes = 0;
	};

	struct FBuild
	{
		FString BuildId;
		/** queued, building, succeeded or failed. */
		FString Status;
		/** The compiler's output once finished. */
		FString Log;
		TArray<FBuildArtifact> Artifacts;

		bool IsFinished() const { return Status == TEXT("succeeded") || Status == TEXT("failed"); }
		bool IsSucceeded() const { return Status == TEXT("succeeded"); }
	};

	struct FVersion
	{
		int32 Version = 0;
		FString CreatedBy;
		FDateTime CreatedAt;
		int32 Types = 0;
		bool bActive = false;
		FString ManifestJson;
	};

	struct FAppStatus
	{
		TOptional<int32> ActiveVersion;
		bool bDisabled = false;
		TArray<FString> DisabledTypes;
		bool bBudgetPaused = false;
	};

	struct FInstance
	{
		FString InstanceId;
		FString NodeType;
		FString Key;
		FString Kind;
		FString Phase;
		FString Host;
		FString HeldBack;
		int64 Epoch = 0;
		double SinceMs = 0.0;
	};

	struct FLogLine
	{
		FString Id;
		FString NodeType;
		FString Key;
		FString Host;
		/** The call this line belongs to (32 hex digits); empty for a line written outside a call. */
		FString Flow;
		FString Text;
		/** 0 error, 1 warning, 2 info, 3 debug. */
		int32 Level = 3;
		FDateTime At;
	};

	/** Empty strings and negative numbers leave a filter unset. Lines come newest first; pass the oldest Id held as Before to page back. */
	struct FLogQuery
	{
		FString NodeType;
		FString Key;
		FString Flow;
		FString Before;
		int32 MaxLevel = -1;
		int32 Limit = -1;
	};

	struct FEndpointStat
	{
		FString NodeType;
		FString Method;
		double Calls = 0.0;
		double AppErrors = 0.0;
		double Busy = 0.0;
		double Denied = 0.0;
		double DeadlineExceeded = 0.0;
		double OtherErrors = 0.0;
		TOptional<double> LatencyMsAvg;
		TOptional<double> LatencyMsMax;
	};

	struct FStarter
	{
		FString NodeType;
		FString Description;
		FBuildCrate Crate;
	};

	struct FStarterPack
	{
		/** Deployable as it is, together with the build of every starter's crate. */
		FString ManifestJson;
		TArray<FStarter> Starters;
	};

	struct FError
	{
		FString Message;
		/** The platform's error code (for example FORBIDDEN), empty for a transport failure. */
		FString Code;
		/** True when the call failed with no code from the platform (a network failure or a timeout), so asking again may work. */
		bool bNoAnswer = false;
	};

	template <typename T>
	struct TResult
	{
		bool bOk = false;
		T Value;
		FError Error;
	};

	class FClient : public TSharedFromThis<FClient>
	{
	public:
		/** A client for the app the project is set up for, as the developer signed in to Crowdy Studio. Null with OutError when there is no sign-in or no app. */
		static TSharedPtr<FClient> Create(FString& OutError);

		/** A client over an existing bridge client, for tests that script its answers. */
		static TSharedRef<FClient> CreateForClient(TSharedRef<FCrowdyCppClient> Client, int64 AppId);

		int64 GetAppId() const { return AppId; }

		void Starters(TFunction<void(const TResult<FStarterPack>&)> OnDone);
		void Build(const TArray<FBuildCrate>& Crates, TFunction<void(const TResult<FBuild>&)> OnDone);
		void BuildStatus(const FString& BuildId, TFunction<void(const TResult<FBuild>&)> OnDone);

		/** Asks for the build's status every PollSeconds until it finishes or TimeoutSeconds pass, asking again after a busy or unanswered read; OnProgress sees each answer. */
		void WaitForBuild(const FString& BuildId, TFunction<void(const TResult<FBuild>&)> OnDone,
			TFunction<void(const FBuild&)> OnProgress = nullptr, float PollSeconds = 3.f, float TimeoutSeconds = 600.f);

		/** Makes ManifestJson, naming crates of the build BuildId, the app's active version. Answers the new version number. */
		void Deploy(const FString& ManifestJson, const FString& BuildId, TFunction<void(const TResult<int32>&)> OnDone);
		void Versions(TFunction<void(const TResult<TArray<FVersion>>&)> OnDone);
		void ActivateVersion(int32 Version, TFunction<void(const TResult<FAppStatus>&)> OnDone);
		/** An empty NodeType switches the whole app. */
		void SetEnabled(bool bEnabled, const FString& NodeType, TFunction<void(const TResult<FAppStatus>&)> OnDone);
		void Status(TFunction<void(const TResult<FAppStatus>&)> OnDone);
		void Instances(TFunction<void(const TResult<TArray<FInstance>>&)> OnDone);
		void Logs(const FLogQuery& Query, TFunction<void(const TResult<TArray<FLogLine>>&)> OnDone);
		/** An empty NodeType covers every type; SinceMinutes below 1 uses the platform's default hour. */
		void EndpointStats(const FString& NodeType, int32 SinceMinutes, TFunction<void(const TResult<TArray<FEndpointStat>>&)> OnDone);

		/** Waits between retries of a change the platform refused as busy, so tests can shorten them. */
		void SetBusyWaitsForTest(TArray<float> Seconds) { BusyWaitSeconds = MoveTemp(Seconds); }

	private:
		FClient() = default;

		TSharedPtr<FCrowdyCppAdminClientHost> Host;
		TSharedPtr<FCrowdyCppClient> Client;
		int64 AppId = 0;
		TArray<float> BusyWaitSeconds = {2.f, 4.f, 8.f, 15.f, 30.f};
	};
}
