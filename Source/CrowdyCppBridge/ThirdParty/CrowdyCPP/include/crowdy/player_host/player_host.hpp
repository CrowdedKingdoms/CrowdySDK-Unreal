#pragma once

// The player-host observation contract (`crowdy.player-host/1`): what a game
// exposes about the player's surroundings, its schemas and the error
// vocabulary. The control gate and lease manager that used to live beside it
// went with the Crowdy Agent orchestrator in 0.34.0; the in-browser agent only
// observes, so a native engine has nothing to gate.
#include "crowdy/player_host/adapter.hpp"
#include "crowdy/player_host/schemas.hpp"
#include "crowdy/player_host/types.hpp"
