#pragma once

/// Umbrella header for the CrowdyCPP SDK.
///
/// Layers:
///   crowdy::CrowdyClient        — GraphQL surface (auth, portal, world data,
///                                 game model, teams/channels, admin, operator)
///   crowdy::replication         — native UDP replication client
///   crowdy::session             — world session layer (actors, chunks, inboxes)
///   crowdy::kit                 — Game Kit (blueprints + runtime helpers)

#include "crowdy/client.hpp"
#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/agent/client_runtime.hpp"
#include "crowdy/agent/controller.hpp"
#include "crowdy/agent/native_browser_dispatcher.hpp"
#include "crowdy/agent/native_tool_dispatcher.hpp"
#include "crowdy/agent/registry.hpp"
#include "crowdy/agent/transport.hpp"
#endif
#include "crowdy/domains/admin.hpp"
#include "crowdy/domains/operator.hpp"
#include "crowdy/generated/enums.hpp"
#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/kit/kit.hpp"
#include "crowdy/player_host/player_host.hpp"
#endif
#include "crowdy/replication/connection.hpp"
#include "crowdy/session/codec.hpp"
#include "crowdy/session/durable.hpp"
#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/session/model_mirror.hpp"
#endif
#include "crowdy/session/world_session.hpp"
#include "crowdy/studio/layout.hpp"
#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/studio/agent_projection.hpp"
#include "crowdy/studio/controller.hpp"
#include "crowdy/studio/diagnostics.hpp"
#include "crowdy/studio/editor.hpp"
#include "crowdy/studio/host_adapter.hpp"
#include "crowdy/studio/integration.hpp"
#include "crowdy/studio/runtime.hpp"
#endif
#include "crowdy/world.hpp"
