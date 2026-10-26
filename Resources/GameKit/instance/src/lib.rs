//! A run, keyed by its id: one party enters, plays down floors the hub generates from the run's
//! seed, and the run ends cleared, wiped, abandoned or expired. The dungeon and roguelike shape:
//! a floor's rooms come from `ckx_sdk::kit::rng` seeded by the run and the floor, so a seed
//! replays its layout; the hub resolves each room itself (a fight from the party's strength and
//! a seeded roll, a treasure, a rest; replace [`Run::resolve`] with your combat, or play the
//! room out in an `arena` or a `session`); a party with nobody standing is wiped and the run is
//! over (permadeath); a run nobody touches for `idle_ms` expires. Only its party plays it.
//!
//! With a `volume` in the config, opening a run reserves a grid of the world for it (scope
//! `grids.write`) and grants the party's members the config's permission keys on it (scope
//! `permissions.write`); the run's end deletes the grid. With a `board`, a run that is cleared
//! or wiped posts each member's depth to that `leaderboard` (the manifest lets `instance` call
//! `leaderboard`).
//!
//! Endpoints: `open { seed }` (a player, whose run it is; or a developer, for `{ party }`),
//! `invite { player }` and `descend` (the owner), `join` (an invited player), `enter` (a member
//! standing: resolves the room the party is at), `abandon` (the owner, or a developer), `state`
//! (anyone). It speaks the kit protocol (`ckx_sdk::kit`): the `run` topic carries the run as
//! `snapshot` messages; `snapshot { topic: "run" }` answers it; changes may carry an
//! `action_id`; refusals carry `kit:<code>:` codes.
//!
//! This is the Unreal SDK's copy of the platform's starter: a seed is 0 to 2^63-1 so it fits an
//! int64 exactly; an `open` with a larger one is refused `bad_request`.

use ckx_sdk::kit::rng::Rng;
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;
use serde_json::json;

pub const TOPIC: &str = "run";
const IDLE_TIMER: &str = "idle";
const IDLE_CHECK_MS: u64 = 60_000;
/// Entries the run's log keeps.
const LOG_MAX: usize = 20;
/// The largest seed: one an int64 holds exactly.
const SEED_MAX: u64 = i64::MAX as u64;

/// Where runs reserve their grids: `slots` volumes of `size` chunks, `stride` chunks apart
/// along x from `origin`. A run takes the slot its key hashes to.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Volume {
    pub origin: (i64, i64, i64),
    pub size: (i64, i64, i64),
    pub slots: u32,
    pub stride: i64,
}

impl Volume {
    pub fn slot_of(&self, key: &str) -> u32 {
        (Rng::seed_from(key) % u64::from(self.slots.max(1))) as u32
    }

    /// The chunks slot `slot` covers, both corners included.
    pub fn bounds(&self, slot: u32) -> (ChunkPos, ChunkPos) {
        let x = self.origin.0 + i64::from(slot) * self.stride;
        let low = ChunkPos::new(x, self.origin.1, self.origin.2);
        let high =
            ChunkPos::new(x + self.size.0.max(1) - 1, self.origin.1 + self.size.1.max(1) - 1, self.origin.2 + self.size.2.max(1) - 1);
        (low, high)
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    pub floors: u32,
    pub rooms_per_floor: u32,
    pub party_max: usize,
    pub idle_ms: u64,
    pub start_hp: i64,
    pub start_power: i64,
    /// The `leaderboard` key cleared and wiped runs post their depth to.
    pub board: Option<String>,
    pub volume: Option<Volume>,
    /// The permission keys members get on the run's grid.
    pub member_permissions: Vec<String>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            floors: 3,
            rooms_per_floor: 4,
            party_max: 4,
            idle_ms: 30 * 60_000,
            start_hp: 30,
            start_power: 6,
            board: None,
            volume: None,
            member_permissions: vec!["enter".into(), "build".into()],
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RoomKind {
    Fight,
    Treasure,
    Rest,
    Boss,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Room {
    pub kind: RoomKind,
    /// What a fight here takes.
    pub power: i64,
    pub done: bool,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Unopened,
    Running,
    Cleared,
    Wiped,
    Abandoned,
    Expired,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Member {
    pub player: u64,
    pub hp: i64,
    pub max_hp: i64,
    pub power: i64,
}

/// The run as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct RunView {
    pub phase: Phase,
    pub owner: Option<u64>,
    pub seed: u64,
    pub floor: u32,
    pub floors: u32,
    pub room: usize,
    pub rooms: Vec<Room>,
    pub members: Vec<Member>,
    pub invited: Vec<u64>,
    pub gold: i64,
    pub relics: Vec<String>,
    pub grid: Option<u64>,
    pub log: Vec<String>,
}

#[derive(Deserialize, Default)]
#[serde(default)]
struct Open {
    seed: Option<u64>,
    party: Vec<u64>,
}

#[derive(Deserialize)]
struct Invite {
    player: u64,
}

/// The rooms of `floor` in a run seeded `seed`; the last floor ends with its boss.
pub fn floor_rooms(seed: u64, floor: u32, count: u32, last: bool) -> Vec<Room> {
    let mut rng = Rng::seeded(seed ^ u64::from(floor).wrapping_mul(0x9e37_79b9_7f4a_7c15));
    let base = i64::from(floor) * 8;
    let mut rooms: Vec<Room> = (0..count.max(1))
        .map(|_| {
            let kind = match rng.weighted(&[5, 2, 1]) {
                Some(1) => RoomKind::Treasure,
                Some(2) => RoomKind::Rest,
                _ => RoomKind::Fight,
            };
            Room { kind, power: base + rng.range(0, 6), done: false }
        })
        .collect();
    if last && let Some(end) = rooms.last_mut() {
        *end = Room { kind: RoomKind::Boss, power: base * 3, done: false };
    }
    rooms
}

const RELICS: [&str; 4] = ["ember charm", "frost sigil", "old map", "lucky coin"];

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Run {
    pub config: Config,
    pub phase: Phase,
    pub owner: Option<u64>,
    pub seed: u64,
    pub floor: u32,
    pub room: usize,
    pub rooms: Vec<Room>,
    pub members: Vec<Member>,
    pub invited: Vec<u64>,
    pub gold: i64,
    pub relics: Vec<String>,
    pub rng: Rng,
    pub grid: Option<u64>,
    pub log: Vec<String>,
    pub last_touch: u64,
}

impl Run {
    pub fn view(&self) -> RunView {
        RunView {
            phase: self.phase,
            owner: self.owner,
            seed: self.seed,
            floor: self.floor,
            floors: self.config.floors,
            room: self.room,
            rooms: self.rooms.clone(),
            members: self.members.clone(),
            invited: self.invited.clone(),
            gold: self.gold,
            relics: self.relics.clone(),
            grid: self.grid,
            log: self.log.clone(),
        }
    }

    fn note(&mut self, line: String) {
        self.log.push(line);
        if self.log.len() > LOG_MAX {
            self.log.remove(0);
        }
    }

    fn member(&self, player: u64) -> Result<&Member> {
        self.members.iter().find(|m| m.player == player).ok_or_else(|| kit::error(codes::NOT_MEMBER, "you are not in this run's party"))
    }

    fn running(&self) -> Result<()> {
        match self.phase {
            Phase::Running => Ok(()),
            Phase::Unopened => Err(kit::error(codes::NOT_READY, "the run is not open")),
            _ => Err(kit::error(codes::FINISHED, "the run is over")),
        }
    }

    fn new_member(&self, player: u64) -> Member {
        Member { player, hp: self.config.start_hp, max_hp: self.config.start_hp, power: self.config.start_power }
    }

    pub fn open(&mut self, party: &[u64], seed: u64, now: u64) -> Result<()> {
        if self.phase != Phase::Unopened {
            return Err(kit::error(codes::STARTED, "this run was opened already"));
        }
        if party.is_empty() || party.len() > self.config.party_max {
            return Err(kit::error(codes::BAD_REQUEST, format!("a party is 1 to {} players", self.config.party_max)));
        }
        self.phase = Phase::Running;
        self.owner = party.first().copied();
        self.members = party.iter().map(|&p| self.new_member(p)).collect();
        self.seed = seed;
        self.rng = Rng::seeded(seed);
        self.floor = 1;
        self.room = 0;
        self.rooms = floor_rooms(seed, 1, self.config.rooms_per_floor, self.config.floors <= 1);
        self.last_touch = now;
        self.note(format!("the run opened with seed {seed}"));
        Ok(())
    }

    pub fn invite(&mut self, by: u64, player: u64) -> Result<()> {
        self.running()?;
        if self.owner != Some(by) {
            return Err(kit::error(codes::FORBIDDEN, "the run's owner invites"));
        }
        if self.members.len() + self.invited.len() >= self.config.party_max {
            return Err(kit::error(codes::FULL, "the party is full"));
        }
        if !self.invited.contains(&player) && self.members.iter().all(|m| m.player != player) {
            self.invited.push(player);
        }
        Ok(())
    }

    pub fn join(&mut self, player: u64) -> Result<()> {
        self.running()?;
        if self.members.iter().any(|m| m.player == player) {
            return Ok(());
        }
        let Some(i) = self.invited.iter().position(|&p| p == player) else {
            return Err(kit::error(codes::FORBIDDEN, "only an invited player joins this run"));
        };
        self.invited.remove(i);
        self.members.push(self.new_member(player));
        self.note(format!("player {player} joined"));
        Ok(())
    }

    /// Resolves the room the party is at. Returns what happened, as the log has it.
    pub fn resolve(&mut self, player: u64, now: u64) -> Result<String> {
        self.running()?;
        if self.member(player)?.hp <= 0 {
            return Err(kit::error(codes::FORBIDDEN, "you are down"));
        }
        let Some(room) = self.rooms.get(self.room).cloned() else {
            return Err(kit::error(codes::CONFLICT, "this floor is cleared; the owner descends"));
        };
        self.last_touch = now;
        let standing: Vec<usize> = (0..self.members.len()).filter(|&i| self.members[i].hp > 0).collect();
        let n = standing.len().max(1) as i64;
        let line = match room.kind {
            RoomKind::Treasure => {
                let gold = i64::from(self.floor) * self.rng.range(5, 15);
                self.gold += gold;
                let mut line = format!("treasure: {gold} gold");
                if self.rng.chance(0.3)
                    && let Some(relic) = self.rng.pick(&RELICS)
                {
                    self.relics.push((*relic).to_string());
                    line.push_str(&format!(" and the {relic}"));
                }
                self.rooms[self.room].done = true;
                line
            }
            RoomKind::Rest => {
                for &i in &standing {
                    let m = &mut self.members[i];
                    m.hp = (m.hp + m.max_hp / 2).min(m.max_hp);
                }
                self.rooms[self.room].done = true;
                "a rest: the party heals".to_string()
            }
            RoomKind::Fight | RoomKind::Boss => {
                let strength: i64 = standing.iter().map(|&i| self.members[i].power).sum();
                let roll = self.rng.range(0, strength);
                let won = roll + strength / 2 >= room.power;
                let hurt = if won { (room.power + 2 * n - 1) / (2 * n) } else { (room.power + n - 1) / n };
                for &i in &standing {
                    let m = &mut self.members[i];
                    m.hp = (m.hp - hurt).max(0);
                    if won && m.hp > 0 {
                        m.power += 1;
                    }
                }
                if won {
                    self.rooms[self.room].done = true;
                    let gold = room.power / 2;
                    self.gold += gold;
                    format!(
                        "{}: won ({roll} + {} against {}), {hurt} damage each, {gold} gold",
                        kind_name(room.kind),
                        strength / 2,
                        room.power
                    )
                } else {
                    format!("{}: lost ({roll} + {} against {}), {hurt} damage each", kind_name(room.kind), strength / 2, room.power)
                }
            }
        };
        self.note(line.clone());
        if self.members.iter().all(|m| m.hp <= 0) {
            self.phase = Phase::Wiped;
            self.note("the party fell".into());
        } else if self.rooms[self.room].done {
            self.room += 1;
            if self.room == self.rooms.len() && self.floor >= self.config.floors {
                self.phase = Phase::Cleared;
                self.note("the run is cleared".into());
            }
        }
        Ok(line)
    }

    pub fn descend(&mut self, by: u64, now: u64) -> Result<u32> {
        self.running()?;
        if self.owner != Some(by) {
            return Err(kit::error(codes::FORBIDDEN, "the run's owner leads the way down"));
        }
        if self.room < self.rooms.len() {
            return Err(kit::error(codes::NOT_READY, "this floor has rooms left"));
        }
        self.floor += 1;
        self.room = 0;
        self.rooms = floor_rooms(self.seed, self.floor, self.config.rooms_per_floor, self.floor >= self.config.floors);
        self.last_touch = now;
        self.note(format!("down to floor {}", self.floor));
        Ok(self.floor)
    }

    pub fn end(&mut self, phase: Phase) {
        if self.phase == Phase::Running {
            self.phase = phase;
            self.note(format!("the run {}", if phase == Phase::Expired { "expired" } else { "was abandoned" }));
        }
    }

    /// Each member's depth: floors reached and rooms done, as one score.
    pub fn depth(&self) -> i64 {
        i64::from(self.floor) * 100 + self.room as i64
    }
}

fn kind_name(kind: RoomKind) -> &'static str {
    match kind {
        RoomKind::Fight => "a fight",
        RoomKind::Boss => "the boss",
        RoomKind::Treasure => "treasure",
        RoomKind::Rest => "a rest",
    }
}

/// The hub: the run, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct RunHub {
    #[serde(flatten)]
    pub run: Run,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl RunHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let out = match call.method {
            "open" => {
                let o: Open = kit::args_or_default(call)?;
                let party = match call.caller {
                    Caller::Player(p) => vec![p],
                    Caller::Developer(_) => o.party,
                    _ => return Err(kit::error(codes::FORBIDDEN, "a player or a developer opens a run")),
                };
                if o.seed.is_some_and(|s| s > SEED_MAX) {
                    return Err(kit::error(codes::BAD_REQUEST, "a seed is 0 to 2^63-1"));
                }
                let seed = o.seed.unwrap_or_else(|| (Rng::seed_from(&ctx.key) ^ now) & SEED_MAX);
                self.run.open(&party, seed, now)?;
                self.reserve(ctx, &party)?;
                ctx.timer_every(IDLE_TIMER, IDLE_CHECK_MS)?;
                encode(&self.run.seed)
            }
            "invite" => {
                let i: Invite = kit::args(call)?;
                self.run.invite(call.player()?, i.player)?;
                encode(&true)
            }
            "join" => {
                let player = call.player()?;
                self.run.join(player)?;
                self.grant(ctx, player)?;
                encode(&true)
            }
            "enter" => encode(&self.run.resolve(call.player()?, now)?),
            "descend" => encode(&self.run.descend(call.player()?, now)?),
            "abandon" => {
                let allowed = match call.caller {
                    Caller::Developer(_) => true,
                    Caller::Player(p) => self.run.owner == Some(p),
                    _ => false,
                };
                if !allowed {
                    return Err(kit::error(codes::FORBIDDEN, "the run's owner, or a developer, abandons it"));
                }
                self.run.running()?;
                self.run.end(Phase::Abandoned);
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.after(ctx)?;
        out
    }

    /// Reserves the run's grid and grants the party on it, when the config has a volume.
    fn reserve(&mut self, ctx: &Ctx, party: &[u64]) -> Result<()> {
        let Some(volume) = self.run.config.volume.clone() else { return Ok(()) };
        let slot = volume.slot_of(&ctx.key);
        let (low, high) = volume.bounds(slot);
        let grid = ctx.grids().create(low, high).map_err(|e| {
            self.run.phase = Phase::Unopened;
            kit::error(codes::CONFLICT, format!("the run's volume (slot {slot}) could not be reserved: {}", e.message))
        })?;
        self.run.grid = Some(grid.id);
        for &p in party {
            self.grant(ctx, p)?;
        }
        Ok(())
    }

    fn grant(&self, ctx: &Ctx, player: u64) -> Result<()> {
        if let Some(grid) = self.run.grid {
            let keys: Vec<&str> = self.run.config.member_permissions.iter().map(String::as_str).collect();
            ctx.permissions()
                .grant(grid, player, &keys, None)
                .map_err(|e| kit::error(codes::CONFLICT, format!("granting the run's grid: {}", e.message)))?;
        }
        Ok(())
    }

    /// Once the run is over: the grid goes, the board hears of it, the idle timer stops.
    fn after(&mut self, ctx: &Ctx) -> Result<()> {
        if !matches!(self.run.phase, Phase::Running | Phase::Unopened) {
            ctx.cancel_timer(IDLE_TIMER);
            if let Some(grid) = self.run.grid.take()
                && let Err(e) = ctx.grids().delete(grid)
            {
                ctx.log(Level::Warn, &format!("the run's grid {grid} was not deleted: {}", e.message));
            }
            if let (Some(board), Phase::Cleared | Phase::Wiped) = (&self.run.config.board, self.run.phase) {
                for m in &self.run.members {
                    let submit = json!({ "player": m.player, "score": self.run.depth(), "action_id": format!("{}:{}", ctx.key, m.player) });
                    if let Err(e) = ctx.send("leaderboard", board, "submit", &encode(&submit)?) {
                        ctx.log(Level::Warn, &format!("the run's depth did not reach board {board}: {}", e.message));
                    }
                }
            }
            ctx.persist_now();
        }
        self.kit.snapshot(ctx, TOPIC, &self.run.view())
    }
}

impl Hub for RunHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut hub = Self::default();
        if !seed.is_empty() {
            hub.run.config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
        }
        Ok(hub)
    }

    fn load(_ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        decode(snapshot)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        match call.method {
            "state" => encode(&self.run.view()),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.run.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer == IDLE_TIMER
            && self.run.phase == Phase::Running
            && ctx.now_ms().saturating_sub(self.run.last_touch) >= self.run.config.idle_ms
        {
            self.run.end(Phase::Expired);
            self.after(ctx)?;
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(RunHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::error_code;
    use ckx_sdk::testing::TestHub;

    fn code(e: &CallError) -> Option<&str> {
        error_code(&e.message).map(|c| c.0)
    }

    #[test]
    fn a_seed_replays_its_floors_and_the_last_ends_with_the_boss() {
        assert_eq!(floor_rooms(42, 1, 4, false), floor_rooms(42, 1, 4, false));
        assert_ne!(floor_rooms(42, 1, 4, false), floor_rooms(43, 1, 4, false));
        let last = floor_rooms(42, 3, 4, true);
        assert_eq!(last.last().map(|r| r.kind), Some(RoomKind::Boss));
    }

    #[test]
    fn only_the_party_plays_and_a_strong_party_clears_the_run() {
        let mut hub = TestHub::<RunHub>::spawn_with(
            "instance",
            "run-a",
            br#"{"floors":2,"rooms_per_floor":3,"start_hp":500,"start_power":40,"board":"deepest"}"#,
        )
        .unwrap();
        let seed: u64 = hub.call_typed(Caller::Player(1), "open", &json!({ "seed": 7 })).unwrap();
        assert_eq!(seed, 7);
        let e = hub.call(Caller::Player(2), "join", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN));
        let e = hub.call(Caller::Player(2), "invite", &encode(&json!({ "player": 3 })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN));
        let _: bool = hub.call_typed(Caller::Player(1), "invite", &json!({ "player": 2 })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let e = hub.call(Caller::Player(3), "enter", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::NOT_MEMBER));
        let mut guard = 0;
        loop {
            let view: RunView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
            if view.phase != Phase::Running {
                assert_eq!(view.phase, Phase::Cleared, "{:?}", view.log);
                assert_eq!(view.floor, 2);
                break;
            }
            if view.room == view.rooms.len() {
                let e = hub.call(Caller::Player(2), "descend", &encode(&()).unwrap()).unwrap_err();
                assert_eq!(code(&e), Some(codes::FORBIDDEN));
                let _: u32 = hub.call_typed(Caller::Player(1), "descend", &()).unwrap();
            } else {
                let _: String = hub.call_typed(Caller::Player(2), "enter", &()).unwrap();
            }
            guard += 1;
            assert!(guard < 100);
        }
        let sent = hub.sent();
        assert_eq!(sent.len(), 2, "each member's depth goes to the board");
        assert!(sent.iter().all(|m| m.node_type == "leaderboard" && m.key == "deepest" && m.method == "submit"));
        let e = hub.call(Caller::Player(1), "enter", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FINISHED));
    }

    #[test]
    fn a_weak_party_is_wiped_and_the_run_is_over() {
        let mut run = Run { config: Config { start_hp: 5, start_power: 1, ..Config::default() }, ..Default::default() };
        run.open(&[1], 3, 0).unwrap();
        for _ in 0..50 {
            if run.phase != Phase::Running {
                break;
            }
            if run.room == run.rooms.len() {
                run.descend(1, 0).unwrap();
            } else {
                run.resolve(1, 0).unwrap();
            }
        }
        assert_eq!(run.phase, Phase::Wiped, "{:?}", run.log);
    }

    #[test]
    fn a_seed_stays_below_2_pow_63_and_a_larger_one_is_refused() {
        let mut masked = 0;
        for n in 0..64 {
            let key = format!("seed-{n}");
            masked += usize::from(Rng::seed_from(&key) > SEED_MAX);
            let mut hub = TestHub::<RunHub>::spawn("instance", &key).unwrap();
            let seed: u64 = hub.call_typed(Caller::Player(1), "open", &()).unwrap();
            let view: RunView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
            assert!(seed <= SEED_MAX && view.seed == seed, "{key}: {seed}");
        }
        assert!(masked > 0, "some key's hash has the top bit set, so the mask was exercised");

        let mut hub = TestHub::<RunHub>::spawn("instance", "run-d").unwrap();
        let e = hub.call(Caller::Player(1), "open", &encode(&json!({ "seed": SEED_MAX + 1 })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::BAD_REQUEST));
        let e = hub.call(Caller::Player(1), "open", &encode(&json!({ "seed": u64::MAX })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::BAD_REQUEST));
        let view: RunView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.phase, Phase::Unopened, "a refused open leaves the run unopened");
        let seed: u64 = hub.call_typed(Caller::Player(1), "open", &json!({ "seed": SEED_MAX })).unwrap();
        assert_eq!(seed, SEED_MAX);
    }

    #[test]
    fn a_run_nobody_touches_expires() {
        let mut hub = TestHub::<RunHub>::spawn_with("instance", "run-b", br#"{"idle_ms":120000}"#).unwrap();
        let _: u64 = hub.call_typed(Caller::Player(1), "open", &()).unwrap();
        hub.advance(3 * IDLE_CHECK_MS).unwrap();
        let view: RunView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.phase, Phase::Expired);
        assert!(hub.timers().is_empty());
    }

    #[test]
    fn a_volume_is_reserved_for_the_run_granted_to_its_party_and_deleted_at_its_end() {
        let mut hub =
            TestHub::<RunHub>::spawn_with("instance", "run-c", br#"{"volume":{"origin":[1000,0,0],"size":[4,2,4],"slots":8,"stride":8}}"#)
                .unwrap();
        hub.answer_node("grids.create", |body| Ok(json!({ "grid": { "gridId": "55", "low": body["low"], "high": body["high"] } })));
        hub.answer_node("permissions.grant", |_| Ok(json!({ "expiresAt": null })));
        hub.answer_node("grids.delete", |_| Ok(json!({})));
        let _: u64 = hub.call_typed(Caller::Player(1), "open", &()).unwrap();
        let view: RunView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.grid, Some(55));
        let _: bool = hub.call_typed(Caller::Player(1), "abandon", &()).unwrap();
        let ops: Vec<String> = hub.node_requests().into_iter().map(|(op, _)| op).collect();
        assert_eq!(ops, vec!["grids.create", "permissions.grant", "grids.delete"]);
        let create = &hub.node_requests()[0].1;
        let slot = i64::from(Volume { origin: (1000, 0, 0), size: (4, 2, 4), slots: 8, stride: 8 }.slot_of("run-c"));
        assert_eq!(create["low"]["x"], json!((1000 + slot * 8).to_string()));
        assert_eq!(create["high"]["y"], json!("1"));
    }
}
