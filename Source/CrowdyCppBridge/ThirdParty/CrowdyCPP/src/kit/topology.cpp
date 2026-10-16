#include "crowdy/kit/topology.hpp"

#include <algorithm>
#include <regex>
#include <set>

namespace crowdy::kit {

namespace {

using graphql::JVal;

/// `value` as a mutable JVal, numbers kept integral when they are.
JVal jvalOf(const graphql::Json& value) {
  if (!value.ok() || value.isNull()) return JVal();
  if (value.isBool()) return JVal(value.asBool());
  if (value.isString()) return JVal(value.asString());
  if (value.isNumber()) {
    const std::string text = value.dump();
    if (text.find_first_of(".eE") == std::string::npos) return JVal(value.asInt64());
    return JVal(value.asDouble());
  }
  if (value.isArray()) {
    graphql::JArray out;
    value.forEach([&](const graphql::Json& e) { out.push_back(jvalOf(e)); });
    return JVal(std::move(out));
  }
  graphql::JObject out;
  value.forEachMember([&](std::string_view k, const graphql::Json& v) { out.emplace(std::string(k), jvalOf(v)); });
  return JVal(std::move(out));
}

bool validName(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

std::string joined(const std::vector<std::string>& items, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += sep;
    out += items[i];
  }
  return out;
}

}  // namespace

std::vector<std::string> kitManifestProblems(const graphql::Json& manifest) {
  std::vector<std::string> out;
  const std::string root = manifest["root"].asString();
  const graphql::Json types = manifest["types"];
  const graphql::Json rootType = types[root];
  if (!rootType.isObject()) {
    out.push_back("root type `" + root + "` is not defined");
  } else if (rootType["kind"].asStringView() != "hub") {
    out.push_back("the root type must be a hub");
  } else if (!rootType["parent"].asStringView().empty()) {
    out.push_back("the root type has no parent");
  }
  std::vector<std::string> scopes;
  for (auto s : kKitNodeApiScopes) scopes.emplace_back(s);
  types.forEachMember([&](std::string_view nameView, const graphql::Json& t) {
    const std::string name(nameView);
    if (!validName(name)) out.push_back("type name `" + name + "` must be 1-64 letters, digits, `-` or `_`");
    if (name != root) {
      const std::string parent = t["parent"].asString();
      if (parent.empty()) {
        out.push_back("`" + name + "` needs a parent");
      } else if (!types[parent].isObject()) {
        out.push_back("`" + name + "`: parent `" + parent + "` is not defined");
      }
    }
    t["calls"].forEach([&](const graphql::Json& c) {
      const std::string callee = c.asString();
      if (callee != "*" && !types[callee].isObject()) out.push_back("`" + name + "` calls `" + callee + "`, which is not defined");
    });
    if (t["kind"].asStringView() == "hub" && (t["replicas"].ok() || t["max_replicas"].ok() || t["concurrency"].ok())) {
      out.push_back("`" + name + "`: replicas, max_replicas and concurrency are for spokes");
    }
    t["scopes"].forEach([&](const graphql::Json& s) {
      const std::string scope = s.asString();
      if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
        out.push_back("`" + name + "`: unknown node API scope `" + scope + "` (allowed: " + joined(scopes, ", ") + ")");
      }
    });
  });
  return out;
}

graphql::Json KitTopologyPlan::manifest() const {
  JVal m = JVal::object({{"root", JVal(root)}});
  graphql::JObject t;
  for (const auto& [name, spec] : types) t.emplace(name, spec);
  m["types"] = JVal(std::move(t));
  return graphql::Json::parse(m.dump());
}

graphql::Json KitTopology::pack() {
  if (!pack_ && exec_) {
    graphql::Json fetched = exec_->starters(appId_);
    if (fetched.isObject()) pack_ = fetched;
    return fetched;
  }
  return pack_.value_or(graphql::Json());
}

std::vector<std::string> KitTopology::types() {
  std::vector<std::string> out;
  const graphql::Json manifest = graphql::Json::parse(pack()["manifestJson"].asStringView());
  manifest["types"].forEachMember([&](std::string_view name, const graphql::Json&) { out.emplace_back(name); });
  return out;
}

KitTopologyPlan KitTopology::plan(const std::vector<std::string>& select, const std::map<std::string, JVal>& overrides) {
  KitTopologyPlan plan;
  const graphql::Json p = pack();
  if (!p.isObject()) {
    plan.problems.push_back("the starter packs could not be read");
    return plan;
  }
  const graphql::Json manifest = graphql::Json::parse(p["manifestJson"].asStringView());
  const graphql::Json all = manifest["types"];
  plan.root = manifest["root"].asString();
  std::vector<std::string> offered;
  all.forEachMember([&](std::string_view name, const graphql::Json&) { offered.emplace_back(name); });

  std::vector<std::string> chosen;
  std::function<void(const std::string&)> add = [&](const std::string& name) {
    if (std::find(chosen.begin(), chosen.end(), name) != chosen.end()) return;
    const graphql::Json t = all[name];
    if (!t.isObject()) {
      plan.problems.push_back("the starter packs have no node type '" + name + "' (they have " + joined(offered, ", ") + ")");
      return;
    }
    chosen.push_back(name);
    const std::string parent = t["parent"].asString();
    if (!parent.empty()) add(parent);
    t["calls"].forEach([&](const graphql::Json& c) {
      if (c.asStringView() != "*") add(c.asString());
    });
  };
  add(plan.root);
  for (const auto& name : select) add(name);
  if (!plan.problems.empty()) return plan;

  std::set<std::string> crates;
  for (const auto& name : chosen) {
    JVal merged = jvalOf(all[name]);
    if (auto o = overrides.find(name); o != overrides.end() && o->second.isObject()) {
      for (const auto& [k, v] : o->second.obj()) merged[k] = v;
    }
    // A type calls only what the plan holds.
    if (merged.isObject() && merged.obj().count("calls") && merged["calls"].isArray()) {
      graphql::JArray kept;
      for (const auto& c : merged["calls"].arr()) {
        const std::string* callee = c.asStringPtr();
        if (callee && (*callee == "*" || std::find(chosen.begin(), chosen.end(), *callee) != chosen.end())) kept.push_back(c);
      }
      merged["calls"] = JVal(std::move(kept));
    }
    if (const std::string* crate = merged.isObject() && merged.obj().count("crate") ? merged["crate"].asStringPtr() : nullptr) {
      if (crates.insert(*crate).second) plan.crates.push_back(*crate);
    }
    plan.types.emplace(name, std::move(merged));
  }
  plan.problems = kitManifestProblems(plan.manifest());
  if (plan.crates.size() > kKitBuildMaxCrates) {
    plan.problems.push_back(std::to_string(plan.crates.size()) + " crates are more than one build takes (" +
                            std::to_string(kKitBuildMaxCrates) + ")");
  }
  return plan;
}

KitDeployment KitTopology::deploy(const KitTopologyPlan& plan, int timeoutMs) {
  KitDeployment out;
  if (!exec_) {
    out.error = "the kit has no exec client";
    return out;
  }
  if (!plan.ok()) {
    out.error = "the plan would be refused: " + joined(plan.problems, "; ");
    return out;
  }
  const graphql::Json p = pack();
  std::vector<domains::ExecCrate> crates;
  for (const auto& name : plan.crates) {
    domains::ExecCrate crate{name, {}};
    bool found = false;
    p["starters"].forEach([&](const graphql::Json& s) {
      if (found || s["crate"].asStringView() != name) return;
      found = true;
      s["files"].forEach([&](const graphql::Json& f) { crate.files.emplace_back(f["path"].asString(), f["content"].asString()); });
    });
    if (!found) {
      out.error = "the starter packs have no crate '" + name + "'";
      return out;
    }
    crates.push_back(std::move(crate));
  }
  const graphql::Json queued = exec_->build(appId_, crates);
  out.buildId = queued["buildId"].asString();
  if (out.buildId.empty()) {
    out.error = "the build was not queued";
    return out;
  }
  out.build = exec_->waitForBuild(appId_, out.buildId, 2000, timeoutMs);
  const std::string status = out.build["status"].asString();
  if (status != "succeeded") {
    std::string log = out.build["log"].asString();
    if (log.size() > 2000) log = log.substr(log.size() - 2000);
    out.error = "build " + out.buildId + " " + (status.empty() ? std::string("unknown") : status) + ": " + log;
    return out;
  }
  std::vector<domains::ExecNodeType> types;
  for (const auto& [name, spec] : plan.types) {
    domains::ExecNodeType t;
    t.name = name;
    const graphql::Json s = graphql::Json::parse(spec.dump());
    t.kind = s["kind"].asString();
    t.crate = s["crate"].asString();
    t.parent = s["parent"].asString();
    t.client = s["client"].asBool(false);
    s["calls"].forEach([&](const graphql::Json& c) { t.calls.push_back(c.asString()); });
    t.extra = spec;
    types.push_back(std::move(t));
  }
  const graphql::Json deployed = exec_->deploy(appId_, plan.root, types, out.buildId);
  out.version = deployed["version"].asInt64();
  out.ok = out.version > 0;
  if (!out.ok) out.error = "the deploy was not accepted";
  return out;
}

}  // namespace crowdy::kit
