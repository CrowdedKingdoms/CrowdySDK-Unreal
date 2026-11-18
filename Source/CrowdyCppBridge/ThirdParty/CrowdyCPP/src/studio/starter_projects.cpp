#include "crowdy/studio/starter_projects.hpp"

#include <stdexcept>
#include <utility>

#include "crowdy/domains/crowdy_studio.hpp"
#include "crowdy/domains/exec.hpp"

namespace crowdy::studio {

namespace {

/// A mod's name is at most 48 characters, and the SERVER module name is `<base>-server`.
constexpr std::size_t kModBaseMax = 48 - 7;

// The en dash is escaped so the source reads the same to every compiler.
constexpr std::string_view kClientCargoHead = R"toml([package]
name = ")toml";
constexpr std::string_view kClientCargoTail =
    R"toml("
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

# How often visitors' browsers call tick (clamped 16)toml"
    "\xE2\x80\x93"
    R"toml(1000 ms).
# 1000 = HUD/text. 50 = physics minigames (pool). 16 = shooters, if a tick stays cheap.
[package.metadata.crowdy]
tick_interval_ms = 1000

[dependencies]
crowdy-client-sdk = ")toml";
constexpr std::string_view kClientCargoEnd = R"toml("
serde_json = "1"
)toml";

constexpr std::string_view kClientLib = R"rs(use crowdy_client_sdk as crowdy;

fn init() {
    // Runs once in each visitor's browser, after they consent to this CLIENT half
    // (or trust you) and while they stand in this grid.
    crowdy::log(1, "ready");
}

fn tick(_dt_ms: u32) {
    // Every tick_interval_ms (Cargo.toml). The state blob lasts as long as the page's worker.
    let ticks = u32::from_le_bytes(crowdy::state_get().try_into().unwrap_or([0; 4])).wrapping_add(1);
    crowdy::state_set(&ticks.to_le_bytes());
    // Host calls cross the page's broker as the visiting player, inside this grid: the HUD and
    // overlay, chunk and actor reads, voxel writes, spatial and channel sends, grid events.
    // Type "crowdy::api::" for the rest. The page's DOM and tokens are never reachable.
    let _ = crowdy::api::hud_set(serde_json::json!({ "text": format!("Ticks here: {ticks}") }));
    //
    // Mouse (holodeck canvas only; Studio chrome is omitted). Drain every tick:
    //   let data = crowdy::api::pointer_clicks().unwrap_or(serde_json::json!({}));
    // data["clicks"] = [{ "t": "down"|"up", "button": 0, "atMs", "heldMs", "nx", "ny" }]
}

fn invoke(payload: &[u8]) -> Vec<u8> {
    payload.to_vec()
}

fn event(_payload: &[u8]) {
    // Grid events from the other CLIENT halves on this page (crowdy::api::emit_event).
}

crowdy::register_module!(init: init, tick: tick, invoke: invoke, event: event);
)rs";

/// The length of the whitespace character at `at`, as JavaScript's `\s` and trim() read
/// UTF-8 text; 0 when it is not one.
std::size_t whitespaceAt(std::string_view text, std::size_t at) {
  const auto byte = [&](std::size_t i) -> unsigned {
    return i < text.size() ? static_cast<unsigned char>(text[i]) : 0u;
  };
  const unsigned c = byte(at);
  if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return 1;
  if (c == 0xC2 && byte(at + 1) == 0xA0) return 2;                            // U+00A0
  if (c == 0xE1 && byte(at + 1) == 0x9A && byte(at + 2) == 0x80) return 3;  // U+1680
  if (c == 0xE2 && byte(at + 1) == 0x80) {
    const unsigned d = byte(at + 2);
    // U+2000-U+200A, U+2028, U+2029, U+202F
    if ((d >= 0x80 && d <= 0x8A) || d == 0xA8 || d == 0xA9 || d == 0xAF) return 3;
  }
  if (c == 0xE2 && byte(at + 1) == 0x81 && byte(at + 2) == 0x9F) return 3;  // U+205F
  if (c == 0xE3 && byte(at + 1) == 0x80 && byte(at + 2) == 0x80) return 3;  // U+3000
  if (c == 0xEF && byte(at + 1) == 0xBB && byte(at + 2) == 0xBF) return 3;  // U+FEFF
  return 0;
}

std::size_t skipWhitespace(std::string_view text, std::size_t at) {
  while (at < text.size()) {
    const std::size_t width = whitespaceAt(text, at);
    if (width == 0) break;
    at += width;
  }
  return at;
}

/// JavaScript's String.prototype.trim().
std::string trimmed(std::string_view text) {
  const std::size_t begin = skipWhitespace(text, 0);
  std::size_t end = begin;
  for (std::size_t at = begin; at < text.size();) {
    const std::size_t width = whitespaceAt(text, at);
    if (width > 0) {
      at += width;
    } else {
      end = ++at;
    }
  }
  return std::string(text.substr(begin, end - begin));
}

/// CrowdyJS's slugModuleName: trim, lower-case, every run of characters outside a-z and 0-9
/// one dash, none at either end. Lower-casing turns two non-ASCII letters into ASCII ones,
/// which the slug keeps: U+0130 becomes `i` and a combining dot, U+212A (KELVIN SIGN) `k`.
std::string slugModuleName(std::string_view value) {
  const std::string text = trimmed(value);
  std::string dashed;
  bool pendingDash = false;
  bool started = false;
  const auto keep = [&](char c) {
    if (pendingDash && started) dashed.push_back('-');
    dashed.push_back(c);
    started = true;
    pendingDash = false;
  };
  for (std::size_t at = 0; at < text.size(); ++at) {
    const auto c = static_cast<unsigned char>(text[at]);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      keep(static_cast<char>(c));
    } else if (c >= 'A' && c <= 'Z') {
      keep(static_cast<char>(c - 'A' + 'a'));
    } else if (c == 0xC4 && at + 1 < text.size() &&
               static_cast<unsigned char>(text[at + 1]) == 0xB0) {
      keep('i');
      pendingDash = true;
      at += 1;
    } else if (c == 0xE2 && at + 2 < text.size() &&
               static_cast<unsigned char>(text[at + 1]) == 0x84 &&
               static_cast<unsigned char>(text[at + 2]) == 0xAA) {
      keep('k');
      at += 2;
    } else {
      pendingDash = true;
    }
  }
  return dashed;
}

/// A mod module base: short enough for `<base>-server` and `<base>-client` to be mod
/// names, and starting with a letter, as a build's crate names must.
std::string modModuleBase(std::string_view value) {
  const std::string slug = slugModuleName(value);
  if (slug.empty()) return "player-mod";
  std::string cut = slug.front() >= 'a' && slug.front() <= 'z' ? slug : "mod-" + slug;
  if (cut.size() > kModBaseMax) cut.resize(kModBaseMax);
  // The slug has no runs of dashes, so the cut leaves at most one at the end.
  if (!cut.empty() && cut.back() == '-') cut.pop_back();
  return cut;
}

/// The section a `[name]` line opens (`/^\s*\[([^\]]+)\]\s*$/`), trimmed.
std::optional<std::string> sectionOf(std::string_view line) {
  const std::size_t open = skipWhitespace(line, 0);
  if (open >= line.size() || line[open] != '[') return std::nullopt;
  const std::size_t close = line.find(']', open + 1);
  if (close == std::string_view::npos || close == open + 1) return std::nullopt;
  if (skipWhitespace(line, close + 1) != line.size()) return std::nullopt;
  return trimmed(line.substr(open + 1, close - open - 1));
}

/// `/^\s*name\s*=/`
bool namesPackage(std::string_view line) {
  std::size_t at = skipWhitespace(line, 0);
  if (line.substr(at, 4) != "name") return false;
  at = skipWhitespace(line, at + 4);
  return at < line.size() && line[at] == '=';
}

/// The first `name` in `[package]`, set to `name`; every other line as it was.
std::string renamePackage(std::string_view manifest, std::string_view name) {
  std::string out;
  out.reserve(manifest.size() + name.size());
  std::string section;
  bool renamed = false;
  std::size_t start = 0;
  for (;;) {
    const std::size_t newline = manifest.find('\n', start);
    const std::string_view line = manifest.substr(
        start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
    if (const auto header = sectionOf(line)) {
      section = *header;
      out += line;
    } else if (section == "package" && !renamed && namesPackage(line)) {
      renamed = true;
      out += "name = \"";
      out += name;
      out += '"';
    } else {
      out += line;
    }
    if (newline == std::string_view::npos) break;
    out.push_back('\n');
    start = newline + 1;
  }
  return out;
}

std::vector<CrowdyStudioProjectFile> starterFiles(const CrowdyStudioModStarter& starter,
                                                  std::string_view name, CrowdyStudioTarget target) {
  bool manifest = false;
  bool library = false;
  for (const auto& file : starter.files) {
    manifest = manifest || file.path == "Cargo.toml";
    library = library || file.path == "src/lib.rs";
  }
  if (!manifest || !library) {
    throw std::invalid_argument(target == CrowdyStudioTarget::Server
                                    ? "The mod starter has no Cargo.toml or src/lib.rs"
                                    : "The CLIENT half starter has no Cargo.toml or src/lib.rs");
  }
  std::vector<CrowdyStudioProjectFile> files;
  files.reserve(starter.files.size());
  for (const auto& source : starter.files) {
    CrowdyStudioProjectFile file;
    file.target = target;
    file.path = source.path;
    file.content = source.path == "Cargo.toml" ? renamePackage(source.content, name)
                                               : source.content;
    files.push_back(std::move(file));
  }
  return files;
}

bool hasTarget(CrowdyStudioProjectKind kind, CrowdyStudioTarget target) {
  for (const auto candidate : projectTargets(kind)) {
    if (candidate == target) return true;
  }
  return false;
}

graphql::GraphQLOutcome refusal(std::string message) {
  graphql::GraphQLOutcome out;
  out.status = Errc::Malformed;
  out.kind = graphql::GraphQLErrorKind::Protocol;
  out.errorMessage = std::move(message);
  return out;
}

/// An API host whose toolchain predates the CLIENT half starter answers it with 503.
bool starterUnavailable(const graphql::GraphQLOutcome& outcome) {
  return outcome.kind == graphql::GraphQLErrorKind::GraphQL && !outcome.errors.empty() &&
         outcome.errors.front().httpStatus == 503;
}

/// Fetches what `options` lacks, one starter at a time, then creates the project. `fetched`:
/// a starter came from the platform, so one it cannot start from is an outcome, not a throw.
/// `sdkClient`: the CLIENT half starter is unavailable, so the SDK's copy stands.
void createStarterProject(domains::ExecAPI& exec, domains::CrowdyStudioAPI& projects,
                          CrowdyStudioNewProjectOptions options, CrowdyStudioCreatedCallback done,
                          bool fetched, bool sdkClient) {
  if (hasTarget(options.kind, CrowdyStudioTarget::Server) && !options.modStarter) {
    const std::string appId = options.appId;
    exec.modStarterAsync(appId, [&exec, &projects, options = std::move(options), done = std::move(done),
                                 sdkClient](graphql::GraphQLOutcome outcome) mutable {
      if (!outcome.ok()) {
        done(std::move(outcome), {});
        return;
      }
      options.modStarter = crowdyStudioModStarterOf(outcome.data);
      createStarterProject(exec, projects, std::move(options), std::move(done), true, sdkClient);
    });
    return;
  }
  if (hasTarget(options.kind, CrowdyStudioTarget::Client) && !options.clientStarter && !sdkClient) {
    const std::string appId = options.appId;
    exec.modClientStarterAsync(appId, [&exec, &projects, options = std::move(options),
                                       done = std::move(done)](graphql::GraphQLOutcome outcome) mutable {
      if (outcome.ok()) {
        options.clientStarter = crowdyStudioModStarterOf(outcome.data);
      } else if (!starterUnavailable(outcome)) {
        done(std::move(outcome), {});
        return;
      }
      createStarterProject(exec, projects, std::move(options), std::move(done), true, !outcome.ok());
    });
    return;
  }
  if (!fetched) {
    projects.createProjectAsync(createCrowdyStudioStarterProject(options), std::move(done));
    return;
  }
  CreateCrowdyStudioProjectInput input;
  try {
    input = createCrowdyStudioStarterProject(options);
  } catch (const std::invalid_argument& error) {
    done(refusal(error.what()), {});
    return;
  }
  projects.createProjectAsync(input, std::move(done));
}

}  // namespace

CrowdyStudioModStarter crowdyStudioModStarterOf(const graphql::Json& starter) {
  CrowdyStudioModStarter mod;
  const graphql::Json files = starter["files"];
  if (!files.isArray()) return mod;
  files.forEach([&](const graphql::Json& file) {
    mod.files.push_back({file["path"].asString(), file["content"].asString()});
  });
  return mod;
}

std::vector<CrowdyStudioProjectFile> crowdyStudioClientStarterFiles(
    std::string_view moduleName) {
  CrowdyStudioProjectFile manifest;
  manifest.target = CrowdyStudioTarget::Client;
  manifest.path = "Cargo.toml";
  manifest.content.reserve(kClientCargoHead.size() + moduleName.size() +
                           kClientCargoTail.size() + kCrowdyStudioClientSdkVersion.size() +
                           kClientCargoEnd.size());
  manifest.content += kClientCargoHead;
  manifest.content += moduleName;
  manifest.content += kClientCargoTail;
  manifest.content += kCrowdyStudioClientSdkVersion;
  manifest.content += kClientCargoEnd;
  CrowdyStudioProjectFile library;
  library.target = CrowdyStudioTarget::Client;
  library.path = "src/lib.rs";
  library.content = std::string(kClientLib);
  std::vector<CrowdyStudioProjectFile> files;
  files.push_back(std::move(manifest));
  files.push_back(std::move(library));
  return files;
}

CreateCrowdyStudioProjectInput createCrowdyStudioStarterProject(
    const CrowdyStudioNewProjectOptions& options) {
  const bool server = hasTarget(options.kind, CrowdyStudioTarget::Server);
  const bool client = hasTarget(options.kind, CrowdyStudioTarget::Client);
  if (server && !options.modStarter) {
    throw std::invalid_argument(
        "A SERVER target starts from the mod starter (client.exec().modStarter)");
  }
  const std::string base = modModuleBase(options.name);
  CreateCrowdyStudioProjectInput input;
  input.appId = options.appId;
  if (!options.gridId.empty()) input.gridId = options.gridId;
  input.kind = options.kind;
  const std::string name = trimmed(options.name);
  input.metadata.name = name.empty() ? "Untitled mod" : name;
  if (options.description) {
    std::string description = trimmed(*options.description);
    if (!description.empty()) input.metadata.description = std::move(description);
  }
  if (server) input.metadata.serverModuleName = base + "-server";
  if (client) input.metadata.clientModuleName = base + "-client";
  input.metadata.pairingPreference = CrowdyStudioPairingPreference::None;
  for (const auto target : projectTargets(options.kind)) {
    auto files = target == CrowdyStudioTarget::Server
                     ? starterFiles(*options.modStarter, *input.metadata.serverModuleName, target)
                 : options.clientStarter
                     ? starterFiles(*options.clientStarter, *input.metadata.clientModuleName, target)
                     : crowdyStudioClientStarterFiles(*input.metadata.clientModuleName);
    for (auto& file : files) input.files.push_back(std::move(file));
  }
  return input;
}

void createCrowdyStudioStarterProjectAsync(domains::ExecAPI& exec,
                                           domains::CrowdyStudioAPI& projects,
                                           CrowdyStudioNewProjectOptions options,
                                           CrowdyStudioCreatedCallback done) {
  createStarterProject(exec, projects, std::move(options), std::move(done), false, false);
}

}  // namespace crowdy::studio
