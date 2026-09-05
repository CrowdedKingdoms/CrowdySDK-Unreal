#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/studio/controller.hpp"

namespace crowdy::studio {

enum class CrowdyStudioEditorMode { Native, Text };
using CrowdyStudioEditorDiagnosticSource = CrowdyStudioDiagnosticSource;
using CrowdyStudioEditorDiagnosticSeverity = CrowdyStudioDiagnosticSeverity;
using CrowdyStudioEditorDiagnostic = CrowdyStudioDiagnostic;

struct CrowdyStudioEditorBuffer {
  CrowdyStudioFileRef file;
  std::string content;
  bool readOnly = false;
};

struct CrowdyStudioEditorSnapshot {
  std::optional<std::string> projectId;
  std::vector<CrowdyStudioEditorBuffer> buffers;
  std::vector<CrowdyStudioFileRef> openFiles;
  std::optional<CrowdyStudioFileRef> selectedFile;
};

/**
 * The only callbacks a native editor receives. They deliberately expose no
 * project provider, CrowdyClient, GraphQL, filesystem, shell, or network
 * capability.
 */
struct CrowdyStudioEditorCallbacks {
  std::function<void(CrowdyStudioTarget, std::string, std::string)>
      onProjectFileChange;
  std::function<void(std::vector<CrowdyStudioEditorDiagnostic>)>
      onLocalDiagnostics;
  std::function<void(CrowdyStudioFileRef)> onOpenFile;
  std::function<void(CrowdyStudioFileRef)> onCloseFile;
  std::function<void(std::string)> onFailure;
};

/**
 * Engine-owned editor surface. Implementations may wrap an engine text editor,
 * an LSP client, or a basic text area, but receive only synchronized in-memory
 * buffers and narrow callbacks into CrowdyStudioController.
 */
class ICrowdyStudioEditorAdapter {
 public:
  virtual ~ICrowdyStudioEditorAdapter() = default;
  virtual CrowdyStudioEditorMode mode() const noexcept = 0;
  virtual void setCallbacks(CrowdyStudioEditorCallbacks callbacks) = 0;
  virtual void synchronize(const CrowdyStudioEditorSnapshot& snapshot) = 0;
  virtual void relayout() = 0;
  virtual void dispose() noexcept = 0;
};

struct CrowdyStudioEditorBridgeOptions {
  std::function<void(std::string_view)> onFailure;
};

/**
 * Binds an engine editor to the same controller methods used by a human Studio
 * shell. Callback delivery is lifetime-fenced; dispose() disconnects both
 * directions before asking the engine editor to release its resources.
 */
class CrowdyStudioEditorBridge {
 public:
  CrowdyStudioEditorBridge(
      CrowdyStudioController& controller,
      std::shared_ptr<ICrowdyStudioEditorAdapter> editor,
      CrowdyStudioEditorBridgeOptions options = {});
  ~CrowdyStudioEditorBridge();

  CrowdyStudioEditorBridge(const CrowdyStudioEditorBridge&) = delete;
  CrowdyStudioEditorBridge& operator=(const CrowdyStudioEditorBridge&) =
      delete;

  CrowdyStudioEditorMode mode() const noexcept;
  const std::vector<CrowdyStudioEditorDiagnostic>& localDiagnostics() const {
    return localDiagnostics_;
  }
  void synchronize();
  void relayout();
  void dispose() noexcept;

 private:
  struct Lifetime;

  static CrowdyStudioEditorSnapshot snapshot(
      const CrowdyStudioState& state);
  void acceptProjectFileChange(CrowdyStudioTarget target, std::string path,
                               std::string content);
  void acceptLocalDiagnostics(
      std::vector<CrowdyStudioEditorDiagnostic> diagnostics);
  void acceptOpenFile(CrowdyStudioFileRef file);
  void acceptCloseFile(CrowdyStudioFileRef file);
  void acceptFailure(std::string message);

  CrowdyStudioController& controller_;
  std::shared_ptr<ICrowdyStudioEditorAdapter> editor_;
  CrowdyStudioEditorBridgeOptions options_;
  std::shared_ptr<Lifetime> lifetime_;
  CrowdyStudioController::ListenerId subscription_ = 0;
  std::vector<CrowdyStudioEditorDiagnostic> localDiagnostics_;
  bool disposed_ = false;
};

}  // namespace crowdy::studio
