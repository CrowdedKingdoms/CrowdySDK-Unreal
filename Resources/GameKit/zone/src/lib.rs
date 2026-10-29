//! A circle that holds, then shrinks, phase by phase (`ckx_sdk::kit::sim::zones`), and wears down
//! every player outside it until one is left, keyed by its match. The battle royale's circle; pair
//! it with an `arena` for the fighting. This is the Unreal SDK's z-up copy of the platform's
//! `territory` starter, its `zone` type alone (the `territory` type is the `territory` crate).
//! What differs: chunks are z up, as the Unreal SDK sends them (Unreal X, Y, Z over 1600), so the
//! config's `center` is ground `[x, y]` and `z` its height, the probes are laid in rows along
//! ground y and read only the rows the circle's radius needs now, realtime event 98 carries
//! `centerX`, `centerY` and `centerZ` (the last two both the ground y, for either reader), and
//! snapshots from the platform's y-up starter start again.
//!
//! The hub reads where players are with the node API (scope `world.read`): a client never says
//! it is inside the circle. A player's position is the chunk the platform tracks, to one chunk
//! (the middle of that chunk on every axis), never the state the player's client writes. Players
//! more than 1 chunk above or below the circle's height, or more than 10 chunks from its centre on
//! either ground axis, count as outside, so its first circle is at most 10.
//!
//! A scan costs one read per probe: 21 at a radius of 10, falling to 1 as the circle shrinks, and
//! runs every `scan_ms` (2 seconds by default, never under 250 ms). A scan whose reads fail
//! changes nothing; the next good one wears players down for the time since the last good one,
//! at most 5 scan periods.
//!
//! Endpoints: `join`, `start` (the first to join, or a developer), `state` (anyone). Topic
//! `zone`: `snapshot` messages each scan, `event` messages (`type`: `warning`, `shrinking`,
//! `settled`, `final`, `eliminated`, `won`); the circle's moves are also realtime event 98 at its
//! centre. It speaks the kit protocol (`ckx_sdk::kit`): `snapshot { topic }` answers the topic,
//! changes may carry an `action_id`, and refusals carry `kit:<code>:` codes. A seed whose `mode`
//! is not `zone` is refused.

use std::collections::BTreeMap;

use ckx_sdk::kit::presence::Seen;
use ckx_sdk::kit::sim::zones::{ShrinkPhase, ShrinkingZone, ZoneEvent};
use ckx_sdk::kit::wire;
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::node::Actor;
use ckx_sdk::prelude::*;
use serde::de::IgnoredAny;
use serde_json::{Value, json};

pub const TOPIC: &str = "zone";
const SCAN: &str = "scan";
/// Ground chunks either side of the centre the probes reach.
const REACH: i64 = 10;
/// A probe's reach on ground x (also on height, which the hub cuts to 1) and on ground y.
const PROBE_X: i64 = 3;
const PROBE_Y: i64 = 1;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct ZoneConfig {
    /// The circle's centre on the ground, `[x, y]` in chunks.
    pub center: (f64, f64),
    /// The circle's height in chunks: players within 1 of it count.
    pub z: i64,
    /// The first circle's radius, in chunks (at most 10).
    pub radius: f64,
    pub phases: Vec<ShrinkPhase>,
    pub hp: i64,
    /// What a second outside the circle costs.
    pub damage_per_sec: i64,
    /// Milliseconds between scans, which also wear down the players outside (at least 250).
    pub scan_ms: u64,
    pub players_max: usize,
}

impl Default for ZoneConfig {
    fn default() -> Self {
        Self {
            center: (0.5, 0.5),
            z: 0,
            radius: 10.0,
            phases: vec![
                ShrinkPhase { radius: 6.0, hold_ms: 60_000, shrink_ms: 30_000 },
                ShrinkPhase { radius: 3.0, hold_ms: 45_000, shrink_ms: 20_000 },
                ShrinkPhase { radius: 1.0, hold_ms: 30_000, shrink_ms: 15_000 },
            ],
            hp: 100,
            damage_per_sec: 5,
            scan_ms: 2_000,
            players_max: 64,
        }
    }
}

fn config(seed: &[u8]) -> Result<ZoneConfig> {
    #[derive(Deserialize)]
    struct Named {
        #[serde(default)]
        mode: Option<String>,
    }
    if seed.is_empty() {
        return Ok(ZoneConfig::default());
    }
    let bad = |e: serde_json::Error| Error::new(format!("seed is not a JSON config: {e}"));
    let named: Named = serde_json::from_slice(seed).map_err(bad)?;
    if let Some(mode) = named.mode.filter(|m| m != "zone") {
        return Err(Error::new(format!("this crate runs a `zone`, not a `{mode}`; a `territory` is the `territory` crate")));
    }
    serde_json::from_slice(seed).map_err(bad)
}

fn with_type(kind: &str, extra: Value) -> Value {
    let mut e = json!({ "type": kind });
    if let (Some(e), Some(x)) = (e.as_object_mut(), extra.as_object()) {
        e.extend(x.clone());
    }
    e
}

/// Where a player's actor is, in chunks, z up: x and y on the ground, z the height. The middle of
/// the chunk the platform tracks it in. The actor's own state is never read: a client writes it,
/// so it could say anything.
pub fn position_of(actor: &Actor) -> (f64, f64, f64, bool) {
    (actor.chunk.x as f64 + 0.5, actor.chunk.y as f64 + 0.5, actor.chunk.z as f64 + 0.5, false)
}

/// The ground chunks, lowest and highest, a circle of `radius` about `center` touches on one
/// axis, cut at `REACH` of the centre's chunk.
fn span(center: f64, radius: f64) -> (i64, i64) {
    let c = center.floor() as i64;
    (((center - radius).floor() as i64).max(c - REACH), ((center + radius).floor() as i64).min(c + REACH))
}

/// The circle's realtime event 98, z up: its centre on the ground is `centerX`, `centerY`, and `centerZ` repeats `centerY` for readers that name the ground axis z.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct ZoneChange {
    /// `hold`, `shrink` or `closed`.
    pub kind: String,
    pub phase: Option<u32>,
    pub radius_now: f64,
    pub center_x: f64,
    pub center_y: f64,
    pub center_z: f64,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum MatchPhase {
    #[default]
    Lobby,
    Running,
    Finished,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Contender {
    pub player: u64,
    pub hp: i64,
    /// Outside the circle at the last scan (or nowhere the probes saw).
    pub outside: bool,
    /// 1 for the winner; set as each player is eliminated.
    pub place: Option<u32>,
}

/// The match as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct ZoneView {
    pub phase: MatchPhase,
    /// Ground `[x, y]`.
    pub center: (f64, f64),
    pub radius: f64,
    /// The phase under way, and the radius it shrinks to.
    pub stage: Option<usize>,
    pub next_radius: Option<f64>,
    pub players: Vec<Contender>,
    pub alive: u32,
    pub winner: Option<u64>,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Zone {
    pub config: ZoneConfig,
    /// Snapshots from the y-up starter lack this and start again.
    #[serde(default)]
    pub z_up: bool,
    pub phase: MatchPhase,
    pub circle: ShrinkingZone,
    pub players: Vec<Contender>,
    pub stage: Option<usize>,
    pub winner: Option<u64>,
    /// When the last good scan ran; 0 in a snapshot from before this field, set again on load.
    #[serde(default)]
    pub last_scan_ms: u64,
    #[serde(skip)]
    pub events: Vec<Value>,
}

impl Zone {
    pub fn new(config: ZoneConfig) -> Self {
        Self { config, z_up: true, ..Self::default() }
    }

    pub fn radius(&self, now: u64) -> f64 {
        if self.phase == MatchPhase::Lobby { self.config.radius } else { f64::from(self.circle.radius(now)) }
    }

    pub fn view(&self, now: u64) -> ZoneView {
        ZoneView {
            phase: self.phase,
            center: self.config.center,
            radius: self.radius(now),
            stage: self.stage,
            next_radius: self.stage.and_then(|s| self.config.phases.get(s)).map(|p| f64::from(p.radius)),
            players: self.players.clone(),
            alive: self.alive() as u32,
            winner: self.winner,
        }
    }

    fn alive(&self) -> usize {
        self.players.iter().filter(|p| p.place.is_none()).count()
    }

    pub fn join(&mut self, player: u64) -> Result<()> {
        if self.phase != MatchPhase::Lobby {
            return Err(kit::error(codes::STARTED, "the match is under way"));
        }
        if self.players.iter().any(|p| p.player == player) {
            return Ok(());
        }
        if self.players.len() >= self.config.players_max {
            return Err(kit::error(codes::FULL, "the match is full"));
        }
        self.players.push(Contender { player, hp: self.config.hp, outside: false, place: None });
        Ok(())
    }

    pub fn start(&mut self, now: u64) -> Result<()> {
        if self.phase != MatchPhase::Lobby {
            return Err(kit::error(codes::STARTED, "the match is under way"));
        }
        if self.players.len() < 2 {
            return Err(kit::error(codes::NOT_READY, "a match takes two"));
        }
        let c = &self.config;
        self.circle = ShrinkingZone::start((c.center.0 as f32, c.center.1 as f32), c.radius as f32, c.phases.clone(), now);
        self.phase = MatchPhase::Running;
        self.last_scan_ms = now;
        Ok(())
    }

    /// Where the probes go: columns 7 chunks apart on ground x by rows 3 apart on ground y,
    /// covering the chunks the circle touches now. A radius of r whole chunks takes
    /// ceil((2r+1)/7) by ceil((2r+1)/3) reads: 21 at 10, 10 at 6, 3 at 3, 1 at 1.
    pub fn probes(&self, now: u64) -> Vec<ChunkPos> {
        let radius = self.radius(now);
        let (x0, x1) = span(self.config.center.0, radius);
        let (y0, y1) = span(self.config.center.1, radius);
        let mut out = Vec::new();
        for x in (x0 + PROBE_X..=x1 + PROBE_X).step_by(2 * PROBE_X as usize + 1) {
            for y in (y0 + PROBE_Y..=y1 + PROBE_Y).step_by(2 * PROBE_Y as usize + 1) {
                out.push(ChunkPos::new(x, y, self.config.z));
            }
        }
        out
    }

    /// One scan: who the probes saw. Players outside wear down for the time since the last scan,
    /// at most 5 scan periods, so a scan that could not read carries its damage to the next.
    pub fn scan(&mut self, seen: &[Seen], now: u64) {
        if self.phase != MatchPhase::Running {
            return;
        }
        let dt_ms = now.saturating_sub(self.last_scan_ms).min(5 * self.config.scan_ms.max(250));
        self.last_scan_ms = now;
        for e in self.circle.poll(now) {
            let (kind, body) = match e {
                ZoneEvent::Warning { phase, to_radius, shrink_starts_at_ms } => {
                    self.stage = Some(phase);
                    ("warning", json!({ "phase": phase, "to_radius": to_radius, "shrinks_at": shrink_starts_at_ms }))
                }
                ZoneEvent::Shrinking { phase, to_radius } => ("shrinking", json!({ "phase": phase, "to_radius": to_radius })),
                ZoneEvent::Settled { phase, radius } => ("settled", json!({ "phase": phase, "radius": radius })),
                ZoneEvent::Final { radius } => ("final", json!({ "radius": radius })),
            };
            self.events.push(with_type(kind, body));
        }
        let cost = (self.config.damage_per_sec * dt_ms as i64 + 999) / 1000;
        let before: BTreeMap<u64, i64> = self.players.iter().map(|p| (p.player, p.hp)).collect();
        let mut fallen = Vec::new();
        for p in self.players.iter_mut().filter(|p| p.place.is_none()) {
            let inside = seen.iter().find(|s| s.player == p.player).is_some_and(|s| self.circle.contains((s.x as f32, s.y as f32), now));
            p.outside = !inside;
            if !inside {
                p.hp = (p.hp - cost).max(0);
                if p.hp == 0 {
                    fallen.push(p.player);
                }
            }
        }
        let alive_before = self.alive();
        if fallen.len() == alive_before {
            // Everyone left fell at once: the one who had the most stands.
            let best = fallen.iter().map(|p| before[p]).max().unwrap_or(0);
            let top: Vec<u64> = fallen.iter().copied().filter(|p| before[p] == best).collect();
            if top.len() == 1 {
                fallen.retain(|p| *p != top[0]);
                if let Some(c) = self.players.iter_mut().find(|c| c.player == top[0]) {
                    c.hp = 1;
                }
            }
        }
        let mut place = alive_before as u32;
        for player in fallen {
            if let Some(c) = self.players.iter_mut().find(|c| c.player == player) {
                c.place = Some(place);
                self.events.push(with_type("eliminated", json!({ "player": player, "place": place })));
                place -= 1;
            }
        }
        if self.alive() <= 1 {
            self.phase = MatchPhase::Finished;
            self.winner = self.players.iter().find(|p| p.place.is_none()).map(|p| p.player);
            if let Some(w) = self.players.iter_mut().find(|p| p.place.is_none()) {
                w.place = Some(1);
            }
            self.events.push(with_type("won", json!({ "player": self.winner })));
        }
    }
}

/// The platform's y-up starter persisted its hub inside one of these.
#[derive(Deserialize)]
enum YUpNode {
    Territory(IgnoredAny),
    Zone(IgnoredAny),
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct ZoneHub {
    #[serde(flatten)]
    pub zone: Zone,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl ZoneHub {
    /// A new lobby on the default circle, for a y-up snapshot: its centre cannot be read z up.
    fn start_again(ctx: &Ctx) -> Self {
        ctx.cancel_timer(SCAN);
        ctx.log(Level::Warn, "a y-up snapshot: the match starts again on the default circle");
        Self { zone: Zone::new(ZoneConfig::default()), ..Self::default() }
    }

    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let out = match call.method {
            "join" => {
                self.zone.join(call.player()?)?;
                encode(&true)
            }
            "start" => {
                let allowed = match call.caller {
                    Caller::Developer(_) => true,
                    Caller::Player(p) => self.zone.players.first().map(|c| c.player) == Some(p),
                    _ => false,
                };
                if !allowed {
                    return Err(kit::error(codes::FORBIDDEN, "the first to join, or a developer, starts it"));
                }
                self.zone.start(now)?;
                ctx.timer_every(SCAN, self.zone.config.scan_ms.max(250))?;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.flush(ctx, now)?;
        out
    }

    fn flush(&mut self, ctx: &Ctx, now: u64) -> Result<()> {
        for e in std::mem::take(&mut self.zone.events) {
            self.kit.event(ctx, TOPIC, &e)?;
            self.realtime(ctx, &e, now);
        }
        self.kit.snapshot(ctx, TOPIC, &self.zone.view(now))
    }

    fn realtime(&self, ctx: &Ctx, e: &Value, now: u64) {
        let kind = match e["type"].as_str() {
            Some("warning" | "settled") => "hold",
            Some("shrinking") => "shrink",
            Some("final") => "closed",
            _ => return,
        };
        let c = &self.zone.config;
        let body = ZoneChange {
            kind: kind.into(),
            phase: e["phase"].as_u64().map(|p| p as u32),
            radius_now: f64::from(self.zone.circle.radius(now)),
            center_x: c.center.0,
            center_y: c.center.1,
            center_z: c.center.1,
        };
        let at = ChunkPos::new(c.center.0.floor() as i64, c.center.1.floor() as i64, c.z);
        let actor = actor_id(&format!("zone:{}", ctx.key));
        if let Err(Error(why)) = wire::emit_event(ctx, at, 8, &actor, wire::ZONE_CHANGE, &body) {
            ctx.log(Level::Warn, &format!("the circle's realtime event did not go out: {why}"));
        }
    }

    fn on_scan(&mut self, ctx: &Ctx) -> Result<()> {
        let now = ctx.now_ms();
        let mut actors = Vec::new();
        for at in self.zone.probes(now) {
            match ctx.world().actors_near(at, PROBE_X as u8, PROBE_Y as u8) {
                Ok(found) => actors.extend(found),
                Err(e) => {
                    ctx.log(Level::Warn, &format!("the circle was not scanned: {}", e.message));
                    return Ok(());
                }
            }
        }
        let z = self.zone.config.z;
        actors.retain(|a| (a.chunk.z - z).abs() <= 1);
        let mut seen: Vec<Seen> = Vec::new();
        for a in &actors {
            let Some(player) = a.player else { continue };
            if seen.iter().any(|s| s.player == player) {
                continue;
            }
            let (x, y, z, precise) = position_of(a);
            seen.push(Seen { player, uuid: a.uuid.clone(), x, y, z, precise });
        }
        self.zone.scan(&seen, now);
        if self.zone.phase == MatchPhase::Finished {
            ctx.cancel_timer(SCAN);
            ctx.persist_now();
        }
        self.flush(ctx, now)
    }
}

impl Hub for ZoneHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        Ok(Self { zone: Zone::new(config(seed)?), ..Self::default() })
    }

    fn load(ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        let mut hub: Self = match decode(snapshot) {
            Ok(hub) => hub,
            Err(_) if decode::<YUpNode>(snapshot).is_ok() => return Ok(Self::start_again(ctx)),
            Err(e) => return Err(e),
        };
        if !hub.zone.z_up {
            return Ok(Self::start_again(ctx));
        }
        if hub.zone.last_scan_ms == 0 {
            hub.zone.last_scan_ms = ctx.now_ms();
        }
        Ok(hub)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        match call.method {
            "state" => encode(&self.zone.view(now)),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.zone.view(now)),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        match timer {
            SCAN => self.on_scan(ctx),
            _ => Ok(()),
        }
    }
}

ckx_sdk::export_hub!(ZoneHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::MessageKind;
    use ckx_sdk::kit::wire::ActorState;
    use ckx_sdk::testing::TestHub;
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;

    /// A player at ground `x`, `y`, standing on height 0.
    fn seen(player: u64, x: f64, y: f64) -> Seen {
        Seen { player, uuid: format!("u{player}"), x, y, z: 0.5, precise: true }
    }

    /// The default seed, a scan each second: the tests below count seconds.
    const EVERY_SECOND: &[u8] = br#"{"scan_ms":1000}"#;

    type World = Rc<RefCell<Vec<(u64, i64, i64, i64)>>>;

    /// Answers `world.actors_radius` with the players in `world` (player, x, y, z, z up) within
    /// the read's box: ground x and height z within its xz radius, ground y within its y radius.
    fn world_of(hub: &TestHub<ZoneHub>) -> World {
        world_failing(hub, Rc::default())
    }

    /// Like [`world_of`], but a read fails while `fail` is set.
    fn world_failing(hub: &TestHub<ZoneHub>, fail: Rc<Cell<bool>>) -> World {
        let world: World = Rc::default();
        let w = world.clone();
        hub.answer_node("world.actors_radius", move |body| {
            if fail.get() {
                return Err(CallError { status: ckx_sdk::Status::Unavailable, message: "the world is away".into() });
            }
            let n =|k: &str| body[k].as_str().and_then(|v| v.parse::<i64>().ok()).unwrap_or(0);
            let (rxz, ry) = (body["radiusXz"].as_i64().unwrap_or(0), body["radiusY"].as_i64().unwrap_or(0));
            let actors: Vec<Value> = w
                .borrow()
                .iter()
                .filter(|(_, x, y, z)| (x - n("x")).abs() <= rxz && (y - n("y")).abs() <= ry && (z - n("z")).abs() <= rxz)
                .map(|(p, x, y, z)| {
                    json!({ "uuid": format!("u{p}"), "userId": p.to_string(), "chunkX": x.to_string(), "chunkY": y.to_string(), "chunkZ": z.to_string() })
                })
                .collect();
            Ok(json!({ "actors": actors }))
        });
        world
    }

    fn reads(hub: &TestHub<ZoneHub>) -> usize {
        hub.node_requests().iter().filter(|(op, _)| op == "world.actors_radius").count()
    }

    /// A hub with players 1 and 2 joined and started, the world answering.
    fn started(seed: &[u8]) -> (TestHub<ZoneHub>, World) {
        let mut hub = TestHub::<ZoneHub>::spawn_with("zone", "match-1", seed).unwrap();
        let world = world_of(&hub);
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        (hub, world)
    }

    #[test]
    fn a_seed_mode_other_than_zone_is_refused() {
        assert!(TestHub::<ZoneHub>::spawn_with("zone", "match-1", br#"{"mode":"territory"}"#).is_err());
        assert!(TestHub::<ZoneHub>::spawn_with("br_circle", "match-1", br#"{"mode":"zone","hp":50}"#).is_ok());
        assert!(TestHub::<ZoneHub>::spawn("br_circle", "match-1").is_ok(), "the crate runs one type, whatever its name");
    }

    #[test]
    fn the_circle_wears_down_the_players_outside_it_until_one_is_left() {
        let config = ZoneConfig {
            radius: 6.0,
            phases: vec![ShrinkPhase { radius: 2.0, hold_ms: 2_000, shrink_ms: 2_000 }],
            hp: 10,
            damage_per_sec: 5,
            ..ZoneConfig::default()
        };
        let mut zone = Zone::new(config);
        zone.join(1).unwrap();
        zone.join(2).unwrap();
        zone.join(3).unwrap();
        zone.start(0).unwrap();
        let inner = seen(1, 0.5, 0.5);
        let edge = seen(2, 0.5, 4.5);
        // 3 is nowhere the probes see: outside.
        zone.scan(&[inner.clone(), edge.clone()], 1_000);
        assert_eq!(zone.players.iter().map(|p| p.hp).collect::<Vec<_>>(), vec![10, 10, 5]);
        zone.scan(&[inner.clone(), edge.clone()], 2_000);
        assert_eq!(zone.players[2].place, Some(3));
        // The circle shrinks to 2: the edge is outside now.
        for t in [3_000, 4_000, 5_000] {
            zone.scan(&[inner.clone(), edge.clone()], t);
        }
        assert_eq!((zone.phase, zone.winner), (MatchPhase::Finished, Some(1)));
        assert_eq!(zone.players[1].place, Some(2));
        let kinds: Vec<&str> = zone.events.iter().filter_map(|e| e["type"].as_str()).collect();
        assert!(
            kinds.contains(&"warning") && kinds.contains(&"shrinking") && kinds.contains(&"eliminated") && kinds.contains(&"won"),
            "{kinds:?}"
        );
    }

    #[test]
    fn the_zone_hub_probes_around_its_centre_and_publishes_each_scan() {
        let (mut hub, world) = started(EVERY_SECOND);
        world.borrow_mut().extend([(1, 0, 0, 0), (2, 9, 9, 0)]);
        hub.advance(1_000).unwrap();
        assert_eq!(reads(&hub), 21, "3 columns by 7 rows at the default radius of 10");
        let view: ZoneView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.players.iter().map(|p| p.outside).collect::<Vec<_>>(), vec![false, true]);
        #[derive(Deserialize)]
        struct K {
            kind: MessageKind,
        }
        assert!(hub.published().iter().any(|(_, b)| decode::<K>(b).unwrap().kind == MessageKind::Event), "the first warning");
        assert!(!hub.emitted().is_empty(), "the circle's warning went out as event 98");
    }

    #[test]
    fn a_scan_runs_every_two_seconds_unless_the_seed_says_otherwise() {
        assert_eq!(ZoneConfig::default().scan_ms, 2_000);
        let (mut hub, _world) = started(b"");
        hub.advance(1_000).unwrap();
        assert_eq!(reads(&hub), 0, "no scan in the first second");
        hub.advance(1_000).unwrap();
        assert_eq!(reads(&hub), 21, "one scan's reads after two");
    }

    #[test]
    fn the_scan_period_does_not_change_what_a_second_outside_costs() {
        let end_hp = |scan_ms: u64| {
            let mut zone = Zone::new(ZoneConfig { scan_ms, ..ZoneConfig::default() });
            zone.join(1).unwrap();
            zone.join(2).unwrap();
            zone.start(0).unwrap();
            for now in (scan_ms..=10_000).step_by(scan_ms as usize) {
                zone.scan(&[seen(1, 0.5, 0.5)], now);
            }
            zone.players[1].hp
        };
        assert_eq!((end_hp(1_000), end_hp(2_000)), (50, 50), "10 seconds outside at 5 a second");
    }

    #[test]
    fn a_player_above_or_below_the_height_band_is_outside() {
        let (mut hub, world) = started(br#"{"z":5,"scan_ms":1000}"#);
        // A probe's xz reach of 3 also reaches heights 2 to 8; the hub keeps only 4 to 6.
        world.borrow_mut().extend([(1, 0, 0, 6), (2, 0, 0, 7)]);
        hub.advance(1_000).unwrap();
        let view: ZoneView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.players.iter().map(|p| p.outside).collect::<Vec<_>>(), vec![false, true]);
        world.borrow_mut().clear();
        world.borrow_mut().extend([(1, 0, 0, 3), (2, 0, 0, 4)]);
        hub.advance(1_000).unwrap();
        let view: ZoneView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.players.iter().map(|p| p.outside).collect::<Vec<_>>(), vec![true, false]);
    }

    #[test]
    fn the_probes_cover_the_circle_to_its_edge_on_both_ground_axes() {
        // Centre (0.5, 0.5), radius 10: a chunk's centre 10 away on either axis is on the edge.
        for (x, y) in [(0, 10), (0, -10), (10, 0), (-10, 0)] {
            let (mut hub, world) = started(EVERY_SECOND);
            world.borrow_mut().extend([(1, 0, 0, 0), (2, x, y, 0)]);
            hub.advance(1_000).unwrap();
            let view: ZoneView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
            assert_eq!(view.players.iter().map(|p| p.outside).collect::<Vec<_>>(), vec![false, false], "at ({x}, {y})");
        }
    }

    #[test]
    fn a_scan_reads_only_the_rows_the_circle_needs_now() {
        let mut zone = Zone::new(ZoneConfig::default());
        zone.join(1).unwrap();
        zone.join(2).unwrap();
        zone.start(0).unwrap();
        // Holding at 10, then settled at 6, 3 and 1.
        for (now, reads) in [(0, 21), (90_000, 10), (155_000, 3), (200_000, 1)] {
            assert_eq!(zone.probes(now).len(), reads, "at {} chunks", zone.radius(now));
        }
        let mid = zone.probes(155_000);
        assert_eq!(mid.iter().map(|p| p.y).collect::<Vec<_>>(), vec![-2, 1, 4], "rows 3 apart along ground y");
        assert!(mid.iter().all(|p| p.x == 0 && p.z == 0));
    }

    #[test]
    fn the_circles_realtime_event_is_at_its_centre_with_z_its_height() {
        let (mut hub, _world) = started(br#"{"center":[3.5,-7.5],"z":2,"scan_ms":1000}"#);
        hub.advance(1_000).unwrap();
        let emitted = hub.emitted();
        assert!(!emitted.is_empty());
        let n = |i: usize| i64::from_le_bytes(emitted[0][i..i + 8].try_into().unwrap());
        assert_eq!(ChunkPos::new(n(10), n(18), n(26)), ChunkPos::new(3, -8, 2));
        let body: Value = serde_json::from_slice(&emitted[0][72..]).unwrap();
        assert_eq!((body["centerX"].as_f64(), body["centerY"].as_f64()), (Some(3.5), Some(-7.5)));
        assert_eq!(body["centerZ"].as_f64(), Some(-7.5));
        let view: ZoneView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.center, (3.5, -7.5));
    }

    fn base64(bytes: &[u8]) -> String {
        const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let mut out = String::new();
        for c in bytes.chunks(3) {
            let n = (u32::from(c[0]) << 16) | (u32::from(*c.get(1).unwrap_or(&0)) << 8) | u32::from(*c.get(2).unwrap_or(&0));
            for i in 0..4 {
                if i <= c.len() {
                    out.push(A[(n >> (18 - 6 * i) & 63) as usize] as char);
                } else {
                    out.push('=');
                }
            }
        }
        out
    }

    #[test]
    fn a_players_own_state_never_places_it() {
        let mut pose = vec![0u8; 48];
        pose[..4].copy_from_slice(&8.0f32.to_le_bytes());
        for state in [encode(&ActorState::new("player", 1, 0.5, 0.5)).unwrap(), pose] {
            let actor = Actor { uuid: "u1".into(), player: Some(1), chunk: ChunkPos::new(40, -30, 2), state: Some(state) };
            assert_eq!(position_of(&actor), (40.5, -29.5, 2.5, false));
        }
    }

    #[test]
    fn a_client_written_state_cannot_put_a_player_inside_the_circle() {
        let (mut hub, _world) = started(EVERY_SECOND);
        let forged = base64(&encode(&ActorState::new("player", 1, 0.5, 0.5)).unwrap());
        hub.answer_node("world.actors_radius", move |_| {
            Ok(json!({ "actors": [{ "uuid": "u1", "userId": "1", "chunkX": "9", "chunkY": "9", "chunkZ": "0", "stateBase64": forged }] }))
        });
        hub.advance(1_000).unwrap();
        let view: ZoneView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert!(view.players[0].outside, "chunk 9, 9 is past the radius of 10, whatever its state says");
    }

    #[test]
    fn a_scan_that_could_not_read_carries_its_damage_to_the_next_good_one() {
        let mut hub = TestHub::<ZoneHub>::spawn_with("zone", "match-1", br#"{"hp":100,"damage_per_sec":10,"scan_ms":1000}"#).unwrap();
        let fail = Rc::new(Cell::new(false));
        let world = world_failing(&hub, fail.clone());
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        world.borrow_mut().push((1, 0, 0, 0));
        let hp2 = |hub: &mut TestHub<ZoneHub>| hub.call_typed::<_, ZoneView>(Caller::Player(1), "state", &()).unwrap().players[1].hp;
        hub.advance(1_000).unwrap();
        assert_eq!(hp2(&mut hub), 90);
        fail.set(true);
        hub.advance(2_000).unwrap();
        assert_eq!(hp2(&mut hub), 90, "a scan that could not read wears nobody down");
        fail.set(false);
        hub.advance(1_000).unwrap();
        assert_eq!(hp2(&mut hub), 60, "the good scan wears down for the 3 seconds since the last good one");
        fail.set(true);
        hub.advance(10_000).unwrap();
        fail.set(false);
        hub.advance(1_000).unwrap();
        assert_eq!(hp2(&mut hub), 10, "an outage costs at most 5 scan periods");
    }

    #[test]
    fn a_snapshot_from_before_the_last_scan_time_starts_counting_when_it_loads() {
        let snapshot = {
            let (mut hub, _world) = started(b"");
            hub.hub_mut().zone.last_scan_ms = 0;
            hub.persist().unwrap()
        };
        let kept = TestHub::<ZoneHub>::load("zone", "match-1", &snapshot, 1).unwrap();
        assert!(kept.hub().zone.last_scan_ms > 0);
    }

    #[test]
    fn a_snapshot_from_the_y_up_starter_starts_again() {
        #[derive(Serialize)]
        enum Platform<'a> {
            Zone(&'a ZoneHub),
        }
        let (ours, unmarked, platform) = {
            let (mut hub, _world) = started(br#"{"center":[3.5,-7.5],"z":2}"#);
            let ours = hub.persist().unwrap();
            hub.hub_mut().zone.z_up = false;
            (ours, hub.persist().unwrap(), encode(&Platform::Zone(hub.hub())).unwrap())
        };
        {
            let kept = TestHub::<ZoneHub>::load("zone", "match-1", &ours, 1).unwrap();
            assert_eq!((kept.hub().zone.phase, kept.hub().zone.config.z), (MatchPhase::Running, 2), "a z-up snapshot loads");
        }
        {
            let again = TestHub::<ZoneHub>::load("zone", "match-1", &unmarked, 1).unwrap();
            assert!(again.hub().zone.z_up);
            assert_eq!((again.hub().zone.phase, &again.hub().zone.config), (MatchPhase::Lobby, &ZoneConfig::default()));
        }
        {
            let again = TestHub::<ZoneHub>::load("zone", "match-1", &platform, 1).unwrap();
            assert!(again.hub().zone.z_up && again.hub().zone.players.is_empty(), "the platform's snapshot starts again too");
        }
        assert!(TestHub::<ZoneHub>::load("zone", "match-1", b"\x01\x02", 1).is_err(), "a broken snapshot is still an error");
    }
}
