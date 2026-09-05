#include "crowdy/studio/editor.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace crowdy::studio {

struct CrowdyStudioEditorBridge::Lifetime {
  std::recursive_mutex mutex;
  CrowdyStudioEditorBridge* owner = nullptr;
};

namespace {

template <typename Lifetime, typename Fn>
void withBridge(
    const std::weak_ptr<Lifetime>& weak, Fn&& callback) {
  const auto lifetime = weak.lock();
  if (!lifetime) return;
  std::lock_guard lock(lifetime->mutex);
  if (lifetime->owner) callback(*lifetime->owner);
}

CrowdyStudioFileRef projectFileRef(const CrowdyStudioProjectFile& file) {
  return {
      CrowdyStudioFileRef::Source::Project,
      file.target,
      file.path,
      std::nullopt,
  };
}

CrowdyStudioFileRef referenceFileRef(
    const CrowdyStudioReferenceFile& file) {
  return {
      file.source == CrowdyStudioReferenceSource::PersonalLibrary
          ? CrowdyStudioFileRef::Source::PersonalLibrary
          : CrowdyStudioFileRef::Source::Common,
      file.target,
      file.path,
      file.id,
  };
}

}  // namespace

CrowdyStudioEditorBridge::CrowdyStudioEditorBridge(
    CrowdyStudioController& controller,
    std::shared_ptr<ICrowdyStudioEditorAdapter> editor,
    CrowdyStudioEditorBridgeOptions options)
    : controller_(controller),
      editor_(std::move(editor)),
      options_(std::move(options)),
      lifetime_(std::make_shared<Lifetime>()) {
  if (!editor_) {
    throw std::invalid_argument(
        "Crowdy Studio editor bridge requires an editor adapter");
  }
  lifetime_->owner = this;
  const std::weak_ptr<Lifetime> weak = lifetime_;
  editor_->setCallbacks({
      .onProjectFileChange =
          [weak](CrowdyStudioTarget target, std::string path,
                 std::string content) {
            withBridge(weak, [&](CrowdyStudioEditorBridge& owner) {
              owner.acceptProjectFileChange(
                  target, std::move(path), std::move(content));
            });
          },
      .onLocalDiagnostics =
          [weak](std::vector<CrowdyStudioEditorDiagnostic> diagnostics) {
            withBridge(weak, [&](CrowdyStudioEditorBridge& owner) {
              owner.acceptLocalDiagnostics(std::move(diagnostics));
            });
          },
      .onOpenFile =
          [weak](CrowdyStudioFileRef file) {
            withBridge(weak, [&](CrowdyStudioEditorBridge& owner) {
              owner.acceptOpenFile(std::move(file));
            });
          },
      .onCloseFile =
          [weak](CrowdyStudioFileRef file) {
            withBridge(weak, [&](CrowdyStudioEditorBridge& owner) {
              owner.acceptCloseFile(std::move(file));
            });
          },
      .onFailure =
          [weak](std::string message) {
            withBridge(weak, [&](CrowdyStudioEditorBridge& owner) {
              owner.acceptFailure(std::move(message));
            });
          },
  });
  subscription_ = controller_.subscribe(
      [weak](const CrowdyStudioState& state) {
        withBridge(weak, [&](CrowdyStudioEditorBridge& owner) {
          if (owner.disposed_) return;
          try {
            owner.editor_->synchronize(snapshot(state));
          } catch (const std::exception& error) {
            owner.acceptFailure(error.what());
          } catch (...) {
            owner.acceptFailure("Native editor synchronization failed");
          }
        });
      });
}

CrowdyStudioEditorBridge::~CrowdyStudioEditorBridge() { dispose(); }

CrowdyStudioEditorMode CrowdyStudioEditorBridge::mode() const noexcept {
  return editor_->mode();
}

void CrowdyStudioEditorBridge::synchronize() {
  if (disposed_) return;
  editor_->synchronize(snapshot(controller_.getState()));
}

void CrowdyStudioEditorBridge::relayout() {
  if (disposed_) return;
  editor_->relayout();
}

void CrowdyStudioEditorBridge::dispose() noexcept {
  if (disposed_) return;
  disposed_ = true;
  if (subscription_ != 0) {
    controller_.unsubscribe(subscription_);
    subscription_ = 0;
  }
  {
    std::lock_guard lock(lifetime_->mutex);
    lifetime_->owner = nullptr;
  }
  try {
    editor_->setCallbacks({});
  } catch (...) {
  }
  editor_->dispose();
}

CrowdyStudioEditorSnapshot CrowdyStudioEditorBridge::snapshot(
    const CrowdyStudioState& state) {
  CrowdyStudioEditorSnapshot value;
  value.openFiles = state.openFiles;
  value.selectedFile = state.activeFile;
  if (state.project) {
    value.projectId = state.project->projectId;
    value.buffers.reserve(
        state.project->files.size() +
        state.personalLibraryFiles.size() + state.commonFiles.size());
    for (const auto& file : state.project->files) {
      value.buffers.push_back(
          {projectFileRef(file), file.content, state.project->archived});
    }
  }
  for (const auto& file : state.personalLibraryFiles) {
    value.buffers.push_back({referenceFileRef(file), file.content, true});
  }
  for (const auto& file : state.commonFiles) {
    value.buffers.push_back({referenceFileRef(file), file.content, true});
  }
  return value;
}

void CrowdyStudioEditorBridge::acceptProjectFileChange(
    CrowdyStudioTarget target, std::string path, std::string content) {
  if (disposed_) return;
  try {
    controller_.updateFile(target, path, std::move(content));
  } catch (const std::exception& error) {
    acceptFailure(error.what());
  } catch (...) {
    acceptFailure("Native editor file update failed");
  }
}

void CrowdyStudioEditorBridge::acceptLocalDiagnostics(
    std::vector<CrowdyStudioEditorDiagnostic> diagnostics) {
  if (disposed_) return;
  try {
    if (diagnostics.size() > 256) {
      throw std::invalid_argument(
          "Native editor returned more than 256 diagnostics");
    }
    for (auto& diagnostic : diagnostics) {
      diagnostic.path = normalizeCrowdyStudioPath(diagnostic.path);
      if (diagnostic.line == 0 || diagnostic.column == 0 ||
          diagnostic.line > 1'000'000 ||
          diagnostic.column > 1'000'000 ||
          (diagnostic.endLine &&
           (*diagnostic.endLine == 0 ||
            *diagnostic.endLine > 1'000'000)) ||
          (diagnostic.endColumn &&
           (*diagnostic.endColumn == 0 ||
            *diagnostic.endColumn > 1'000'000)) ||
          diagnostic.message.empty() ||
          diagnostic.message.size() > 2'048 ||
          (diagnostic.code && diagnostic.code->size() > 64)) {
        throw std::invalid_argument(
            "Native editor diagnostic is outside contract bounds");
      }
    }
    localDiagnostics_ = diagnostics;
    controller_.setLocalDiagnostics(std::move(diagnostics));
  } catch (const std::exception& error) {
    acceptFailure(error.what());
  } catch (...) {
    acceptFailure("Native editor diagnostics update failed");
  }
}

void CrowdyStudioEditorBridge::acceptOpenFile(CrowdyStudioFileRef file) {
  if (disposed_) return;
  try {
    controller_.openFile(file);
  } catch (const std::exception& error) {
    acceptFailure(error.what());
  } catch (...) {
    acceptFailure("Native editor file selection failed");
  }
}

void CrowdyStudioEditorBridge::acceptCloseFile(CrowdyStudioFileRef file) {
  if (disposed_) return;
  try {
    controller_.closeFile(file);
  } catch (const std::exception& error) {
    acceptFailure(error.what());
  } catch (...) {
    acceptFailure("Native editor file close failed");
  }
}

void CrowdyStudioEditorBridge::acceptFailure(std::string message) {
  if (options_.onFailure) options_.onFailure(message);
}

}  // namespace crowdy::studio
