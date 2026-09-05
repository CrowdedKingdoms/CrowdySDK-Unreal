#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crowdy::studio {

enum class CrowdyStudioTarget { Server, Client };
enum class CrowdyStudioProjectKind { Server, Client, FullStack };
enum class CrowdyStudioPairingPreference { None, Optional, Required };
enum class CrowdyStudioApiPairing {
  Paired,
  Independent,
  ServerOnly,
  ClientOnly,
};
enum class CrowdyStudioFileProvenance { Authored, Library, Common };
enum class CrowdyStudioCommonStatus { Draft, Published, Archived };
enum class CrowdyStudioReferenceSource { PersonalLibrary, Common };
enum class CrowdyStudioPatchOperation { Create, Replace };
enum class CrowdyStudioSynchronizationCapability {
  AtomicPatch,
  CheckpointList,
  CheckpointEvents,
  ApprovedRestore,
};

inline constexpr std::string_view toString(CrowdyStudioTarget value) {
  return value == CrowdyStudioTarget::Server ? "SERVER" : "CLIENT";
}

inline constexpr std::string_view toString(CrowdyStudioApiPairing value) {
  switch (value) {
    case CrowdyStudioApiPairing::Paired: return "PAIRED";
    case CrowdyStudioApiPairing::Independent: return "INDEPENDENT";
    case CrowdyStudioApiPairing::ServerOnly: return "SERVER_ONLY";
    case CrowdyStudioApiPairing::ClientOnly: return "CLIENT_ONLY";
  }
  return "";
}

inline constexpr std::string_view toString(
    CrowdyStudioPairingPreference value) {
  switch (value) {
    case CrowdyStudioPairingPreference::None: return "NONE";
    case CrowdyStudioPairingPreference::Optional: return "OPTIONAL";
    case CrowdyStudioPairingPreference::Required: return "REQUIRED";
  }
  return "";
}

inline constexpr std::string_view toString(CrowdyStudioFileProvenance value) {
  switch (value) {
    case CrowdyStudioFileProvenance::Authored: return "AUTHORED";
    case CrowdyStudioFileProvenance::Library: return "LIBRARY";
    case CrowdyStudioFileProvenance::Common: return "COMMON";
  }
  return "";
}

inline constexpr std::string_view toString(CrowdyStudioCommonStatus value) {
  switch (value) {
    case CrowdyStudioCommonStatus::Draft: return "DRAFT";
    case CrowdyStudioCommonStatus::Published: return "PUBLISHED";
    case CrowdyStudioCommonStatus::Archived: return "ARCHIVED";
  }
  return "";
}

inline constexpr std::string_view toString(CrowdyStudioReferenceSource value) {
  return value == CrowdyStudioReferenceSource::PersonalLibrary ? "LIBRARY"
                                                               : "COMMON";
}

inline constexpr std::string_view toString(
    CrowdyStudioSynchronizationCapability value) {
  switch (value) {
    case CrowdyStudioSynchronizationCapability::AtomicPatch:
      return "ATOMIC_PATCH";
    case CrowdyStudioSynchronizationCapability::CheckpointList:
      return "CHECKPOINT_LIST";
    case CrowdyStudioSynchronizationCapability::CheckpointEvents:
      return "CHECKPOINT_EVENTS";
    case CrowdyStudioSynchronizationCapability::ApprovedRestore:
      return "APPROVED_RESTORE";
  }
  return "";
}

inline constexpr CrowdyStudioProjectKind projectKind(
    CrowdyStudioApiPairing value) {
  if (value == CrowdyStudioApiPairing::ServerOnly) {
    return CrowdyStudioProjectKind::Server;
  }
  if (value == CrowdyStudioApiPairing::ClientOnly) {
    return CrowdyStudioProjectKind::Client;
  }
  return CrowdyStudioProjectKind::FullStack;
}

inline constexpr CrowdyStudioPairingPreference pairingPreference(
    CrowdyStudioApiPairing value) {
  if (value == CrowdyStudioApiPairing::Paired) {
    return CrowdyStudioPairingPreference::Required;
  }
  if (value == CrowdyStudioApiPairing::Independent) {
    return CrowdyStudioPairingPreference::Optional;
  }
  return CrowdyStudioPairingPreference::None;
}

inline constexpr CrowdyStudioApiPairing apiPairing(
    CrowdyStudioProjectKind kind, CrowdyStudioPairingPreference preference) {
  if (kind == CrowdyStudioProjectKind::Server) {
    return CrowdyStudioApiPairing::ServerOnly;
  }
  if (kind == CrowdyStudioProjectKind::Client) {
    return CrowdyStudioApiPairing::ClientOnly;
  }
  return preference == CrowdyStudioPairingPreference::Required
             ? CrowdyStudioApiPairing::Paired
             : CrowdyStudioApiPairing::Independent;
}

inline std::optional<CrowdyStudioTarget> targetFromString(
    std::string_view value) {
  if (value == "SERVER" || value == "server") {
    return CrowdyStudioTarget::Server;
  }
  if (value == "CLIENT" || value == "client") {
    return CrowdyStudioTarget::Client;
  }
  return std::nullopt;
}

inline std::optional<CrowdyStudioApiPairing> apiPairingFromString(
    std::string_view value) {
  if (value == "PAIRED" || value == "paired") {
    return CrowdyStudioApiPairing::Paired;
  }
  if (value == "INDEPENDENT" || value == "independent") {
    return CrowdyStudioApiPairing::Independent;
  }
  if (value == "SERVER_ONLY" || value == "server_only") {
    return CrowdyStudioApiPairing::ServerOnly;
  }
  if (value == "CLIENT_ONLY" || value == "client_only") {
    return CrowdyStudioApiPairing::ClientOnly;
  }
  return std::nullopt;
}

inline CrowdyStudioFileProvenance provenanceFromString(
    std::string_view value) {
  if (value == "LIBRARY" || value == "library") {
    return CrowdyStudioFileProvenance::Library;
  }
  if (value == "COMMON" || value == "common") {
    return CrowdyStudioFileProvenance::Common;
  }
  return CrowdyStudioFileProvenance::Authored;
}

inline CrowdyStudioCommonStatus commonStatusFromString(
    std::string_view value) {
  if (value == "DRAFT" || value == "draft") {
    return CrowdyStudioCommonStatus::Draft;
  }
  if (value == "ARCHIVED" || value == "archived") {
    return CrowdyStudioCommonStatus::Archived;
  }
  return CrowdyStudioCommonStatus::Published;
}

enum class CrowdyStudioPatchState { Omitted, Null, Value };

/// Three-state GraphQL input field. `std::optional` cannot distinguish
/// omission ("leave unchanged") from explicit null ("clear this field").
template <typename T>
class CrowdyStudioPatchField {
 public:
  CrowdyStudioPatchField() = default;

  static CrowdyStudioPatchField omitted() { return {}; }

  static CrowdyStudioPatchField null() {
    CrowdyStudioPatchField field;
    field.state_ = CrowdyStudioPatchState::Null;
    return field;
  }

  static CrowdyStudioPatchField value(T value) {
    CrowdyStudioPatchField field;
    field.state_ = CrowdyStudioPatchState::Value;
    field.value_ = std::move(value);
    return field;
  }

  CrowdyStudioPatchState state() const { return state_; }
  bool isOmitted() const {
    return state_ == CrowdyStudioPatchState::Omitted;
  }
  bool isNull() const { return state_ == CrowdyStudioPatchState::Null; }
  bool hasValue() const { return state_ == CrowdyStudioPatchState::Value; }

  const T& get() const {
    if (!value_) throw std::logic_error("Crowdy Studio patch field has no value");
    return *value_;
  }

 private:
  CrowdyStudioPatchState state_ = CrowdyStudioPatchState::Omitted;
  std::optional<T> value_;
};

struct CrowdyStudioProjectScope {
  std::string appId;
  std::string gridId;
};

struct CrowdyStudioProjectFile {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
  std::string content;
  std::string revision;
  CrowdyStudioFileProvenance provenance =
      CrowdyStudioFileProvenance::Authored;
  std::optional<std::string> provenanceLibraryFileId;
  std::optional<std::string> provenanceLibraryRevision;
  std::optional<std::string> provenanceCommonVersionId;
  std::string createdAt;
  std::string updatedAt;
};

struct CrowdyStudioProjectMetadata {
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> serverModuleName;
  std::optional<std::string> clientModuleName;
  CrowdyStudioPairingPreference pairingPreference =
      CrowdyStudioPairingPreference::None;
};

struct CrowdyStudioProjectRevision {
  std::string id;
  std::string savedAt;
};

struct CrowdyStudioProject {
  std::string projectId;
  std::string appId;
  std::string ownerUserId;
  std::optional<std::string> gridId;
  CrowdyStudioProjectKind kind = CrowdyStudioProjectKind::Server;
  CrowdyStudioProjectMetadata metadata;
  std::vector<CrowdyStudioProjectFile> files;
  std::string sdkVersion;
  int abiVersion = 0;
  CrowdyStudioProjectRevision revision;
  bool archived = false;
  std::optional<std::string> archivedAt;
  int fileCount = 0;
  std::string totalBytes;
  std::string createdAt;
  std::string updatedAt;
};

struct CrowdyStudioProjectSummary {
  std::string projectId;
  std::optional<std::string> gridId;
  std::string name;
  CrowdyStudioProjectKind kind = CrowdyStudioProjectKind::Server;
  std::string revisionId;
  std::optional<std::string> serverModuleName;
  std::optional<std::string> clientModuleName;
  bool archived = false;
  std::string updatedAt;
};

struct CrowdyStudioReferenceFile {
  std::string id;
  CrowdyStudioReferenceSource source =
      CrowdyStudioReferenceSource::PersonalLibrary;
  std::string appId;
  std::optional<std::string> ownerUserId;
  std::optional<std::string> commonFileId;
  std::optional<std::string> slug;
  std::string title;
  std::optional<std::string> description;
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
  std::string content;
  std::vector<std::string> tags;
  std::string revision;
  bool archived = false;
  std::optional<std::string> archivedAt;
  CrowdyStudioCommonStatus commonStatus =
      CrowdyStudioCommonStatus::Published;
  std::optional<std::string> contentSha256;
  std::optional<std::string> publishedByUserId;
  std::optional<std::string> publishedAt;
  std::string createdAt;
  std::string updatedAt;
};

struct CreateCrowdyStudioProjectInput {
  std::string appId;
  std::optional<std::string> gridId;
  CrowdyStudioProjectKind kind = CrowdyStudioProjectKind::Server;
  CrowdyStudioProjectMetadata metadata;
  std::vector<CrowdyStudioProjectFile> files;
  std::string sdkVersion = "0.1.5";
  int abiVersion = 0;
  std::optional<std::string> idempotencyKey;
};

struct CrowdyStudioProjectMetadataPatch {
  CrowdyStudioPatchField<std::string> gridId;
  std::optional<std::string> name;
  CrowdyStudioPatchField<std::string> description;
  CrowdyStudioPatchField<std::string> serverModuleName;
  CrowdyStudioPatchField<std::string> clientModuleName;
  std::optional<CrowdyStudioApiPairing> pairingPreference;
  std::optional<std::string> sdkVersion;
  std::optional<int> abiVersion;
};

struct SaveCrowdyStudioProjectMetadataInput {
  std::string appId;
  std::string projectId;
  std::string expectedRevisionId;
  CrowdyStudioProjectMetadataPatch patch;
  std::optional<std::string> idempotencyKey;
};

struct CrowdyStudioProjectFileDelete {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
};

struct SaveCrowdyStudioProjectFilesInput {
  std::string appId;
  std::string projectId;
  std::string expectedRevisionId;
  std::vector<CrowdyStudioProjectFile> upserts;
  std::vector<CrowdyStudioProjectFileDelete> deletes;
  std::optional<std::string> idempotencyKey;
};

/// Full controller snapshot. CrowdyStudioAPI computes a target/path delta
/// against its remembered server baseline before calling the atomic save root.
struct SaveCrowdyStudioProjectInput {
  std::string appId;
  std::string gridId;
  std::string projectId;
  std::string expectedRevisionId;
  CrowdyStudioProjectMetadata metadata;
  std::vector<CrowdyStudioProjectFile> files;
  std::string sdkVersion = "0.1.5";
  int abiVersion = 0;
  std::optional<std::string> idempotencyKey;
};

struct SetCrowdyStudioProjectArchivedInput {
  std::string appId;
  std::string projectId;
  std::string expectedRevisionId;
  bool archived = true;
  std::optional<std::string> idempotencyKey;
};

struct SaveCrowdyStudioLibraryFileInput {
  std::string appId;
  std::optional<std::string> libraryFileId;
  std::optional<std::string> expectedRevisionId;
  std::string title;
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
  std::string content;
  std::vector<std::string> tags;
  std::optional<std::string> idempotencyKey;
};

struct SetCrowdyStudioLibraryFileArchivedInput {
  std::string appId;
  std::string libraryFileId;
  std::string expectedRevisionId;
  bool archived = true;
  std::optional<std::string> idempotencyKey;
};

struct ImportCrowdyStudioReferenceFileInput {
  std::string appId;
  std::string gridId;
  std::string projectId;
  std::string expectedRevisionId;
  CrowdyStudioReferenceSource source =
      CrowdyStudioReferenceSource::PersonalLibrary;
  std::string referenceId;
  std::optional<std::string> destinationPath;
  std::optional<std::string> idempotencyKey;
};

struct PublishCrowdyStudioCommonFileInput {
  std::string appId;
  std::optional<std::string> commonFileId;
  std::string slug;
  std::string title;
  CrowdyStudioPatchField<std::string> description;
  std::string path;
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::vector<std::string> tags;
  std::string content;
  std::optional<std::string> idempotencyKey;
};

struct CreateCrowdyStudioProjectFromModulesInput {
  std::string appId;
  std::string gridId;
  std::optional<std::string> serverModuleName;
  std::optional<std::string> clientModuleName;
  std::optional<std::string> projectName;
  std::optional<std::string> idempotencyKey;
};

struct CrowdyStudioListOptions {
  bool includeArchived = false;
  int limit = 50;
  int offset = 0;
};

struct CrowdyStudioCommonListOptions {
  std::optional<CrowdyStudioTarget> target;
  int limit = 100;
  int offset = 0;
};

class CrowdyStudioError : public std::runtime_error {
 public:
  CrowdyStudioError(std::string code, const std::string& message)
      : std::runtime_error(message), code_(std::move(code)) {}

  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

class CrowdyStudioRevisionConflictError : public CrowdyStudioError {
 public:
  explicit CrowdyStudioRevisionConflictError(
      const std::string& message = "The Crowdy Studio project changed",
      std::optional<CrowdyStudioProject> remoteProject = std::nullopt)
      : CrowdyStudioError("CROWDY_STUDIO_REVISION_CONFLICT", message),
        remoteProject_(std::move(remoteProject)) {}

  const std::optional<CrowdyStudioProject>& remoteProject() const {
    return remoteProject_;
  }

 private:
  std::optional<CrowdyStudioProject> remoteProject_;
};

class CrowdyStudioIdempotencyConflictError : public CrowdyStudioError {
 public:
  explicit CrowdyStudioIdempotencyConflictError(const std::string& message)
      : CrowdyStudioError("IDEMPOTENCY_CONFLICT", message) {}
};

class CrowdyStudioCapabilityUnavailableError : public CrowdyStudioError {
 public:
  explicit CrowdyStudioCapabilityUnavailableError(
      CrowdyStudioSynchronizationCapability capability,
      std::string message = {})
      : CrowdyStudioError(
            "CROWDY_STUDIO_CAPABILITY_UNAVAILABLE",
            message.empty()
                ? "Crowdy Studio synchronization capability " +
                      std::string(toString(capability)) +
                      " is unavailable"
                : message),
        capability_(capability) {}

  CrowdyStudioSynchronizationCapability capability() const {
    return capability_;
  }

 private:
  CrowdyStudioSynchronizationCapability capability_;
};

class CrowdyStudioOfflineError : public CrowdyStudioError {
 public:
  explicit CrowdyStudioOfflineError(
      const std::string& message = "The Crowdy Studio project service is offline")
      : CrowdyStudioError("PROJECT_OFFLINE", message) {}
};

class ICrowdyStudioProjectProvider {
 public:
  virtual ~ICrowdyStudioProjectProvider() = default;

  virtual std::vector<CrowdyStudioProjectSummary> listProjects(
      const CrowdyStudioProjectScope& scope) = 0;
  virtual CrowdyStudioProject getProject(
      const CrowdyStudioProjectScope& scope, std::string_view projectId) = 0;
  virtual CrowdyStudioProject createProject(
      const CreateCrowdyStudioProjectInput& input) = 0;
  virtual CrowdyStudioProject saveProject(
      const SaveCrowdyStudioProjectInput& input) = 0;
  virtual std::vector<CrowdyStudioReferenceFile> listPersonalLibraryFiles(
      const CrowdyStudioProjectScope& scope) = 0;
  virtual CrowdyStudioReferenceFile savePersonalLibraryFile(
      const SaveCrowdyStudioLibraryFileInput& input) = 0;
  virtual std::vector<CrowdyStudioReferenceFile> listCommonFiles(
      const CrowdyStudioProjectScope& scope) = 0;
  virtual CrowdyStudioProject importReferenceFile(
      const ImportCrowdyStudioReferenceFileInput& input) = 0;
};

struct CrowdyStudioAtomicFileChange {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
  CrowdyStudioPatchOperation operation = CrowdyStudioPatchOperation::Replace;
  std::string content;
  std::string expectedContentHash;
};

struct CrowdyStudioAtomicPatchInput {
  std::string expectedRevisionId;
  std::vector<CrowdyStudioAtomicFileChange> changes;
};

struct CrowdyStudioCheckpointFile {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
  std::string contentHash;
  std::size_t byteLength = 0;
};

struct CrowdyStudioCheckpointMetadata {
  enum class Reason { AgentWrite, RestorePreimage, Manual };

  std::string checkpointId;
  std::string projectRevisionId;
  std::string contentHash;
  Reason reason = Reason::Manual;
  std::vector<CrowdyStudioCheckpointFile> files;
  std::string createdAt;
  std::optional<std::string> restoredAt;
};

/// Scope supplied by an authenticated agent integration when forwarding one
/// durable CHECKPOINT_CREATED/CHECKPOINT_RESTORED event to Studio. The event
/// contains metadata only; source content and restore authority are absent.
struct CrowdyStudioCheckpointEvent {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  CrowdyStudioCheckpointMetadata checkpoint;
};

struct CrowdyStudioAtomicPatchResult {
  CrowdyStudioProject project;
  CrowdyStudioCheckpointMetadata checkpoint;
  std::vector<CrowdyStudioCheckpointFile> changedFiles;
};

struct CrowdyStudioCheckpointRestoreInput {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  std::string checkpointId;
  std::string expectedRevisionId;
  std::string approvalGrant;
};

struct CrowdyStudioCheckpointRestoreResult {
  CrowdyStudioProject project;
  CrowdyStudioCheckpointMetadata preRestoreCheckpoint;
};

/**
 * Explicit host/orchestrator bridge for durable synchronization capabilities.
 *
 * The published Game and Management GraphQL schemas do not expose generic
 * checkpoint, atomic-patch, or approved-restore roots. Implementations must
 * bridge an independently authorized durable service; they must not infer this
 * authority from CrowdyStudioAPI::saveProject or a caller-supplied grant.
 */
class ICrowdyStudioSynchronizationProvider {
 public:
  virtual ~ICrowdyStudioSynchronizationProvider() = default;

  virtual CrowdyStudioAtomicPatchResult applyAtomicPatch(
      const CrowdyStudioProjectScope& scope, std::string_view projectId,
      const CrowdyStudioAtomicPatchInput& input) = 0;
  virtual std::vector<CrowdyStudioCheckpointMetadata> listCheckpoints(
      const CrowdyStudioProjectScope& scope, std::string_view projectId) = 0;
  virtual CrowdyStudioCheckpointRestoreResult restoreCheckpoint(
      const CrowdyStudioCheckpointRestoreInput& input) = 0;
};

struct CrowdyStudioProjectSynchronization {
  enum class Source { Agent, Remote };

  Source source = Source::Remote;
  std::optional<std::string> expectedPreviousRevisionId;
  std::optional<CrowdyStudioCheckpointMetadata> checkpoint;
};

inline std::string normalizeCrowdyStudioPath(std::string_view path) {
  std::size_t first = 0;
  while (first < path.size() &&
         std::isspace(static_cast<unsigned char>(path[first])) != 0) {
    ++first;
  }
  std::size_t last = path.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(path[last - 1])) != 0) {
    --last;
  }
  std::string normalized(path.substr(first, last - first));
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
  bool invalid = normalized.empty() || normalized.size() > 240 ||
                 normalized.front() == '/' || normalized.back() == '/';
  std::size_t start = 0;
  while (!invalid && start <= normalized.size()) {
    const std::size_t slash = normalized.find('/', start);
    const std::size_t end =
        slash == std::string::npos ? normalized.size() : slash;
    const std::string_view part(normalized.data() + start, end - start);
    invalid = part.empty() || part == "." || part == "..";
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  for (unsigned char character : normalized) {
    if (character < 0x20 || character == 0x7f) {
      invalid = true;
      break;
    }
  }
  if (invalid) {
    throw std::invalid_argument("Invalid Crowdy Studio project file path");
  }
  return normalized;
}

inline std::string crowdyStudioFileKey(CrowdyStudioTarget target,
                                       std::string_view path) {
  return std::string(toString(target)) + ":" +
         normalizeCrowdyStudioPath(path);
}

inline std::vector<CrowdyStudioTarget> projectTargets(
    CrowdyStudioProjectKind kind) {
  if (kind == CrowdyStudioProjectKind::Server) {
    return {CrowdyStudioTarget::Server};
  }
  if (kind == CrowdyStudioProjectKind::Client) {
    return {CrowdyStudioTarget::Client};
  }
  return {CrowdyStudioTarget::Server, CrowdyStudioTarget::Client};
}

}  // namespace crowdy::studio
