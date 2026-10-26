//! Control points the factions of one map capture by standing on them, keyed by the map. This is
//! the Unreal SDK's z-up copy of the platform's `territory` starter, its `territory` type alone
//! (the `zone` type is the `zone` crate). What differs: chunks are z up, as the Unreal SDK sends
//! them (Unreal X, Y, Z over 1600), so a point is at ground `x`, `y` with `z` its height, players
//! on it are read row by row along ground y, and snapshots from the platform's y-up starter start
//! again.
//!
//! The hub reads where players are with the node API (scope `world.read`): a client never says
//! it is on a point. A player's position is the chunk the platform tracks, to one chunk (the
//! middle of that chunk on every axis), never the state the player's client writes. Once a faction
//! has won, a scan reads nothing until `reset`. A faction captures a point by having more members on it than any other
//! faction has, with capture progress that decays when nobody presses it, income for each point
//! held, and a winning score. The shape of a conquest mode, a guild war, a king-of-the-hill.
//!
//! Endpoints: `join { faction }` (once; a player keeps their faction), `reset` (developers: a
//! new war), `state` (anyone). Topic `territory`: `snapshot` messages when a point moves, `event`
//! messages (`type`: `captured`, `income`, `won`); a point changing hands is also realtime event
//! 96 at the point. It speaks the kit protocol (`ckx_sdk::kit`): `snapshot { topic }` answers the
//! topic, changes may carry an `action_id`, and refusals carry `kit:<code>:` codes. A seed whose
//! `mode` is not `territory` is refused.

use std::collections::BTreeMap;

use ckx_sdk::kit::presence::Seen;
use ckx_sdk::kit::wire;
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::node::Actor;
use ckx_sdk::prelude::*;
use serde::de::IgnoredAny;
use serde_json::{Value, json};

pub const TOPIC: &str = "territory";
const SCAN: &str = "scan";
const INCOME: &str = "income";
/// The largest radius a point may have, the node API's largest ground-x reach.
const MAX_RADIUS: u8 = 3;
/// Ground-y offsets of a point's reads, by its radius: each read reaches 1 chunk of ground y
/// either side, so a point takes 1 read a scan at radius 0 or 1, 2 at radius 2 and 3 at radius 3.
const ROWS: [&[i64]; 4] = [&[0], &[0], &[-1, 1], &[-2, 0, 2]];

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Point {
    pub id: String,
    /// Ground x and y, in chunks.
    pub x: i64,
    pub y: i64,
    /// Height, in chunks; players within 1 of it count.
    #[serde(default)]
    pub z: i64,
    /// Chunks around it on the ground that count as on it (at most 3).
    #[serde(default = "default_radius")]
    pub radius: u8,
}

fn default_radius() -> u8 {
    1
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct TerritoryConfig {
    pub points: Vec<Point>,
    pub factions: Vec<String>,
    /// Capture progress a second for each member more than the next faction has there.
    pub capture_per_sec: f64,
    /// Progress lost a second when nobody presses the capture.
    pub decay_per_sec: f64,
    pub scan_ms: u64,
    pub income_ms: u64,
    pub income_per_point: i64,
    /// The score that wins the war; none for a war without end.
    pub win_score: Option<i64>,
    pub members_max: usize,
}

impl Default for TerritoryConfig {
    fn default() -> Self {
        let point = |id: &str, x: i64| Point { id: id.into(), x, y: 0, z: 0, radius: 1 };
        Self {
            points: vec![point("mill", -8), point("bridge", 0), point("tower", 8)],
            factions: vec!["red".into(), "blue".into()],
            capture_per_sec: 0.1,
            decay_per_sec: 0.05,
            scan_ms: 2_000,
            income_ms: 60_000,
            income_per_point: 10,
            win_score: Some(500),
            members_max: 500,
        }
    }
}

fn config(seed: &[u8]) -> Result<TerritoryConfig> {
    #[derive(Deserialize)]
    struct Named {
        #[serde(default)]
        mode: Option<String>,
    }
    if seed.is_empty() {
        return Ok(TerritoryConfig::default());
    }
    let bad = |e: serde_json::Error| Error::new(format!("seed is not a JSON config: {e}"));
    let named: Named = serde_json::from_slice(seed).map_err(bad)?;
    if let Some(mode) = named.mode.filter(|m| m != "territory") {
        return Err(Error::new(format!("this crate runs a `territory`, not a `{mode}`; a `zone` is the `zone` crate")));
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

/// Every player among `actors`, once each (the first actor a player has), z up.
fn players_among(actors: &[Actor]) -> Vec<Seen> {
    let mut out: Vec<Seen> = Vec::new();
    for a in actors {
        let Some(player) = a.player else { continue };
        if out.iter().any(|s| s.player == player) {
            continue;
        }
        let (x, y, z, precise) = position_of(a);
        out.push(Seen { player, uuid: a.uuid.clone(), x, y, z, precise });
    }
    out
}

/// The players on `p`: within its radius on ground x and y, and 1 chunk of its height.
fn on_point(ctx: &Ctx, p: &Point) -> Result<Vec<Seen>, CallError> {
    let r = p.radius.min(MAX_RADIUS);
    let reach = i64::from(r);
    let mut actors = Vec::new();
    for dy in ROWS[usize::from(r)] {
        // The ground-x reach is also the height reach, so it is at least the height band's 1.
        actors.extend(ctx.world().actors_near(ChunkPos::new(p.x, p.y + dy, p.z), r.max(1), 1)?);
    }
    actors.retain(|a| (a.chunk.x - p.x).abs() <= reach && (a.chunk.y - p.y).abs() <= reach && (a.chunk.z - p.z).abs() <= 1);
    Ok(players_among(&actors))
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct PointState {
    pub id: String,
    pub owner: Option<String>,
    /// The faction pressing a capture, and how far it is, 0 to 1.
    pub capturing: Option<String>,
    pub progress: f64,
    /// Members on the point at the last scan, by faction.
    pub present: BTreeMap<String, u32>,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum WarPhase {
    #[default]
    Open,
    Won,
}

/// The war as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct TerritoryView {
    pub phase: WarPhase,
    pub points: Vec<PointState>,
    pub scores: BTreeMap<String, i64>,
    pub members: BTreeMap<String, u32>,
    pub winner: Option<String>,
}

#[derive(Deserialize)]
struct Join {
    faction: String,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Territory {
    pub config: TerritoryConfig,
    /// Snapshots from the y-up starter lack this and start again.
    #[serde(default)]
    pub z_up: bool,
    pub phase: WarPhase,
    pub members: BTreeMap<u64, String>,
    pub points: Vec<PointState>,
    pub scores: BTreeMap<String, i64>,
    pub winner: Option<String>,
    pub armed: bool,
    #[serde(skip)]
    pub events: Vec<Value>,
    #[serde(skip)]
    pub moved: bool,
}

impl Territory {
    pub fn new(config: TerritoryConfig) -> Self {
        let mut t = Self { config, z_up: true, ..Self::default() };
        t.reset();
        t
    }

    pub fn reset(&mut self) {
        self.phase = WarPhase::Open;
        self.winner = None;
        self.points = self.config.points.iter().map(|p| PointState { id: p.id.clone(), ..PointState::default() }).collect();
        self.scores = self.config.factions.iter().map(|f| (f.clone(), 0)).collect();
        self.moved = true;
    }

    pub fn view(&self) -> TerritoryView {
        let mut members: BTreeMap<String, u32> = self.config.factions.iter().map(|f| (f.clone(), 0)).collect();
        for f in self.members.values() {
            *members.entry(f.clone()).or_insert(0) += 1;
        }
        TerritoryView { phase: self.phase, points: self.points.clone(), scores: self.scores.clone(), members, winner: self.winner.clone() }
    }

    pub fn join(&mut self, player: u64, faction: &str) -> Result<()> {
        if !self.config.factions.iter().any(|f| f == faction) {
            return Err(kit::error(codes::BAD_REQUEST, format!("a faction is one of {}", self.config.factions.join(", "))));
        }
        match self.members.get(&player) {
            Some(f) if f == faction => Ok(()),
            Some(f) => Err(kit::error(codes::CONFLICT, format!("you fight for {f}"))),
            None if self.members.len() >= self.config.members_max => Err(kit::error(codes::FULL, "the war has all the members it takes")),
            None => {
                self.members.insert(player, faction.to_owned());
                self.moved = true;
                Ok(())
            }
        }
    }

    /// One scan: `seen[i]` is who stands on point `i`. Moves the captures over `dt_ms`.
    pub fn scan(&mut self, seen: &[Vec<Seen>], dt_ms: u64) {
        if self.phase != WarPhase::Open {
            return;
        }
        let dt = dt_ms as f64 / 1000.0;
        for (i, point) in self.points.iter_mut().enumerate() {
            let mut present: BTreeMap<String, u32> = BTreeMap::new();
            for s in seen.get(i).map(Vec::as_slice).unwrap_or_default() {
                if let Some(f) = self.members.get(&s.player) {
                    *present.entry(f.clone()).or_insert(0) += 1;
                }
            }
            let mut counts: Vec<(&String, u32)> = present.iter().map(|(f, n)| (f, *n)).collect();
            counts.sort_by_key(|(_, n)| std::cmp::Reverse(*n));
            let lead = counts.first().map(|(f, n)| ((*f).clone(), n - counts.get(1).map_or(0, |c| c.1)));
            let before = (point.owner.clone(), point.capturing.clone(), point.progress);
            match lead {
                Some((leader, margin)) if margin > 0 && point.owner.as_ref() != Some(&leader) => {
                    if point.capturing.as_ref() != Some(&leader) {
                        point.capturing = Some(leader.clone());
                        point.progress = 0.0;
                    }
                    point.progress += self.config.capture_per_sec * f64::from(margin) * dt;
                    if point.progress >= 1.0 {
                        let previous = point.owner.replace(leader.clone());
                        point.capturing = None;
                        point.progress = 0.0;
                        self.events.push(with_type("captured", json!({ "point": point.id, "owner": leader, "previous": previous })));
                    }
                }
                // Contested: the capture holds where it is.
                Some((_, 0)) => {}
                _ => {
                    point.progress = (point.progress - self.config.decay_per_sec * dt).max(0.0);
                    if point.progress == 0.0 {
                        point.capturing = None;
                    }
                }
            }
            if before != (point.owner.clone(), point.capturing.clone(), point.progress) || point.present != present {
                self.moved = true;
            }
            point.present = present;
        }
    }

    /// Income for each point held; the first faction to the winning score wins.
    pub fn income(&mut self) {
        if self.phase != WarPhase::Open {
            return;
        }
        let mut paid: BTreeMap<String, i64> = BTreeMap::new();
        for p in &self.points {
            if let Some(owner) = &p.owner {
                *paid.entry(owner.clone()).or_insert(0) += self.config.income_per_point;
            }
        }
        if paid.is_empty() {
            return;
        }
        for (faction, gold) in &paid {
            *self.scores.entry(faction.clone()).or_insert(0) += gold;
        }
        self.events.push(with_type("income", json!({ "paid": paid })));
        self.moved = true;
        if let Some(target) = self.config.win_score
            && let Some((faction, _)) = self.scores.iter().filter(|(_, s)| **s >= target).max_by_key(|(_, s)| **s)
        {
            self.phase = WarPhase::Won;
            self.winner = Some(faction.clone());
            self.events.push(with_type("won", json!({ "faction": faction })));
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
pub struct TerritoryHub {
    #[serde(flatten)]
    pub war: Territory,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl TerritoryHub {
    /// A new war on the default map, for a y-up snapshot: its points cannot be read z up.
    fn start_again(ctx: &Ctx) -> Self {
        ctx.cancel_timer(SCAN);
        ctx.cancel_timer(INCOME);
        ctx.log(Level::Warn, "a y-up snapshot: the war starts again on the default map");
        Self { war: Territory::new(TerritoryConfig::default()), ..Self::default() }
    }

    fn act(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<Vec<u8>> {
        let out = match call.method {
            "join" => {
                let j: Join = kit::args(call)?;
                self.war.join(call.player()?, &j.faction)?;
                self.arm(ctx)?;
                encode(&true)
            }
            "reset" => {
                call.developer().map_err(|_| kit::error(codes::FORBIDDEN, "developers start a new war"))?;
                self.war.reset();
                self.arm(ctx)?;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.flush(ctx)?;
        out
    }

    fn arm(&mut self, ctx: &Ctx) -> Result<()> {
        if !self.war.armed {
            ctx.timer_every(SCAN, self.war.config.scan_ms.max(500))?;
            ctx.timer_every(INCOME, self.war.config.income_ms.max(1_000))?;
            self.war.armed = true;
        }
        Ok(())
    }

    fn flush(&mut self, ctx: &Ctx) -> Result<()> {
        for e in std::mem::take(&mut self.war.events) {
            self.kit.event(ctx, TOPIC, &e)?;
            if e["type"] == "captured" {
                self.realtime(ctx, &e);
            }
        }
        if std::mem::take(&mut self.war.moved) {
            self.kit.snapshot(ctx, TOPIC, &self.war.view())?;
        }
        Ok(())
    }

    fn realtime(&self, ctx: &Ctx, e: &Value) {
        let Some(point) = self.war.config.points.iter().find(|p| e["point"] == p.id.as_str()) else { return };
        let body = wire::ControlPointChange {
            point_id: point.id.clone(),
            owner: e["owner"].as_str().unwrap_or_default().into(),
            previous_owner: e["previous"].as_str().unwrap_or_default().into(),
            progress: 1.0,
        };
        let actor = actor_id(&format!("point:{}", point.id));
        if let Err(Error(why)) = wire::emit_event(ctx, ChunkPos::new(point.x, point.y, point.z), 4, &actor, wire::CONTROL_POINT, &body) {
            ctx.log(Level::Warn, &format!("a capture's realtime event did not go out: {why}"));
        }
    }

    fn on_scan(&mut self, ctx: &Ctx) -> Result<()> {
        if self.war.phase != WarPhase::Open {
            return Ok(());
        }
        let mut seen = Vec::with_capacity(self.war.config.points.len());
        for p in &self.war.config.points {
            match on_point(ctx, p) {
                Ok(s) => seen.push(s),
                Err(e) => {
                    ctx.log(Level::Warn, &format!("point {} was not scanned: {}", p.id, e.message));
                    return Ok(());
                }
            }
        }
        self.war.scan(&seen, self.war.config.scan_ms);
        self.flush(ctx)
    }
}

impl Hub for TerritoryHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        Ok(Self { war: Territory::new(config(seed)?), ..Self::default() })
    }

    fn load(ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        let hub: Self = match decode(snapshot) {
            Ok(hub) => hub,
            Err(_) if decode::<YUpNode>(snapshot).is_ok() => return Ok(Self::start_again(ctx)),
            Err(e) => return Err(e),
        };
        if !hub.war.z_up {
            return Ok(Self::start_again(ctx));
        }
        Ok(hub)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        match call.method {
            "state" => encode(&self.war.view()),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.war.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, ctx.now_ms(), |h| h.act(ctx, &call)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        match timer {
            SCAN => self.on_scan(ctx),
            INCOME => {
                self.war.income();
                self.flush(ctx)
            }
            _ => Ok(()),
        }
    }
}

ckx_sdk::export_hub!(TerritoryHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, error_code};
    use ckx_sdk::testing::TestHub;
    use std::cell::RefCell;
    use std::rc::Rc;

    /// A player at ground `x`, `y`, standing on height 0.
    fn seen(player: u64, x: f64, y: f64) -> Seen {
        Seen { player, uuid: format!("u{player}"), x, y, z: 0.5, precise: true }
    }

    type World = Rc<RefCell<Vec<(u64, i64, i64, i64)>>>;

    /// Answers `world.actors_radius` with the players in `world` (player, x, y, z, z up) within
    /// the read's box: ground x and height z within its xz radius, ground y within its y radius.
    fn world_of(hub: &TestHub<TerritoryHub>) -> World {
        let world: World = Rc::default();
        let w = world.clone();
        hub.answer_node("world.actors_radius", move |body| {
            let n = |k: &str| body[k].as_str().and_then(|v| v.parse::<i64>().ok()).unwrap_or(0);
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

    /// An emitted spatial event's chunk and its JSON body.
    fn emitted_at(inject: &[u8]) -> (ChunkPos, Value) {
        let n = |i: usize| i64::from_le_bytes(inject[i..i + 8].try_into().unwrap());
        (ChunkPos::new(n(10), n(18), n(26)), serde_json::from_slice(&inject[72..]).unwrap())
    }

    fn one_point(p: &str) -> String {
        format!(r#"{{"capture_per_sec":0.5,"scan_ms":1000,"points":[{p}]}}"#)
    }

    #[test]
    fn a_seed_mode_other_than_territory_is_refused() {
        assert!(TestHub::<TerritoryHub>::spawn_with("territory", "valley", br#"{"mode":"zone"}"#).is_err());
        assert!(TestHub::<TerritoryHub>::spawn_with("conquest", "valley", br#"{"mode":"territory","decay_per_sec":0.2}"#).is_ok());
        assert!(TestHub::<TerritoryHub>::spawn("conquest", "valley").is_ok(), "the crate runs one type, whatever its name");
    }

    #[test]
    fn the_faction_with_more_on_a_point_captures_it_and_a_contest_holds_it() {
        let mut war = Territory::new(TerritoryConfig { capture_per_sec: 0.25, ..TerritoryConfig::default() });
        war.join(1, "red").unwrap();
        war.join(2, "red").unwrap();
        war.join(3, "blue").unwrap();
        let Error(e) = war.join(1, "blue").unwrap_err();
        assert_eq!(error_code(&e).map(|c| c.0), Some(codes::CONFLICT));
        // Two red on the mill against nobody: 0.5 a second, captured in two.
        war.scan(&[vec![seen(1, -8.5, 0.5), seen(2, -8.2, 0.4)], vec![], vec![]], 1_000);
        assert_eq!((war.points[0].capturing.as_deref(), war.points[0].progress), (Some("red"), 0.5));
        war.scan(&[vec![seen(1, -8.5, 0.5), seen(2, -8.2, 0.4)], vec![], vec![]], 1_000);
        assert_eq!(war.points[0].owner.as_deref(), Some("red"));
        assert!(war.events.iter().any(|e| e["type"] == "captured"));
        // One blue alone on the bridge starts it; one red joining contests it and it holds.
        war.scan(&[vec![], vec![seen(3, 0.5, 0.5)], vec![]], 2_000);
        let p = war.points[1].progress;
        war.scan(&[vec![], vec![seen(3, 0.5, 0.5), seen(1, 0.4, 0.4)], vec![]], 2_000);
        assert_eq!(war.points[1].progress, p);
        // Nobody: it decays.
        war.scan(&[vec![], vec![], vec![]], 2_000);
        assert!(war.points[1].progress < p);
        // Income for the mill, then the win.
        war.config.win_score = Some(20);
        war.income();
        war.income();
        assert_eq!((war.phase, war.winner.as_deref()), (WarPhase::Won, Some("red")));
    }

    #[test]
    fn the_territory_hub_scans_where_players_stand_and_sends_a_capture_as_a_realtime_event() {
        let mut hub = TestHub::<TerritoryHub>::spawn_with("territory", "valley", br#"{"capture_per_sec":0.5,"scan_ms":1000}"#).unwrap();
        let world = world_of(&hub);
        let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "faction": "blue" })).unwrap();
        world.borrow_mut().push((1, 8, 0, 0));
        hub.advance(2_500).unwrap();
        let view: TerritoryView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.points[2].owner.as_deref(), Some("blue"), "{view:?}");
        assert_eq!(view.members["blue"], 1);
        assert_eq!(hub.emitted().len(), 1, "the capture went out as event 96");
        let e = hub.call(Caller::Player(1), "reset", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(error_code(&e.message).map(|c| c.0), Some(codes::FORBIDDEN));
        let _: bool = hub.call_typed(Caller::Developer(9), "reset", &()).unwrap();
        let snap: Message<TerritoryView> =
            decode(&hub.call(Caller::Player(1), kit::SNAPSHOT_METHOD, &encode(&json!({ "topic": TOPIC })).unwrap()).unwrap()).unwrap();
        assert_eq!(snap.body.points[2].owner, None);
    }

    #[test]
    fn a_player_above_or_below_the_height_band_is_not_on_the_point() {
        let seed = one_point(r#"{"id":"keep","x":2,"y":5,"z":4,"radius":2}"#);
        let mut hub = TestHub::<TerritoryHub>::spawn_with("territory", "valley", seed.as_bytes()).unwrap();
        let world = world_of(&hub);
        for p in 1..=4 {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &json!({ "faction": "red" })).unwrap();
        }
        // The read's xz radius of 2 reaches height 2 and 6 too; the hub keeps only 3 to 5.
        world.borrow_mut().extend([(1, 2, 5, 3), (2, 2, 5, 5), (3, 2, 5, 6), (4, 2, 5, 2)]);
        hub.advance(1_000).unwrap();
        let view: TerritoryView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.points[0].present.get("red"), Some(&2), "{view:?}");
    }

    #[test]
    fn a_scan_covers_the_points_radius_on_both_ground_axes_in_the_fewest_reads() {
        for (radius, reads) in [(0u8, 1usize), (1, 1), (2, 2), (3, 3)] {
            let seed = one_point(&format!(r#"{{"id":"keep","x":10,"y":-20,"z":1,"radius":{radius}}}"#));
            let mut hub = TestHub::<TerritoryHub>::spawn_with("territory", "valley", seed.as_bytes()).unwrap();
            let world = world_of(&hub);
            let r = i64::from(radius);
            let edges = [(1, 10, -20 + r), (2, 10, -20 - r), (3, 10 + r, -20), (4, 10 - r, -20 - r), (5, 10, -20 + r + 1), (6, 10 - r - 1, -20)];
            for (p, x, y) in edges {
                let _: bool = hub.call_typed(Caller::Player(p), "join", &json!({ "faction": "red" })).unwrap();
                world.borrow_mut().push((p, x, y, 1));
            }
            hub.advance(1_000).unwrap();
            let asked = hub.node_requests().iter().filter(|(op, _)| op == "world.actors_radius").count();
            assert_eq!(asked, reads, "reads a scan at radius {radius}");
            let view: TerritoryView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
            assert_eq!(view.points[0].present.get("red"), Some(&4), "the four on the edge, not the two past it, at radius {radius}");
        }
    }

    #[test]
    fn a_capture_goes_out_at_the_point_with_z_its_height() {
        let seed = one_point(r#"{"id":"keep","x":2,"y":-7,"z":3,"radius":1}"#);
        let mut hub = TestHub::<TerritoryHub>::spawn_with("territory", "valley", seed.as_bytes()).unwrap();
        let world = world_of(&hub);
        let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "faction": "red" })).unwrap();
        world.borrow_mut().push((1, 2, -7, 3));
        hub.advance(2_500).unwrap();
        let emitted = hub.emitted();
        assert_eq!(emitted.len(), 1);
        let (at, body) = emitted_at(&emitted[0]);
        assert_eq!(at, ChunkPos::new(2, -7, 3));
        assert_eq!((body["pointId"].as_str(), body["owner"].as_str()), (Some("keep"), Some("red")));
    }

    #[test]
    fn a_players_own_state_never_places_it() {
        let mut pose = vec![0u8; 48];
        pose[..4].copy_from_slice(&8.0f32.to_le_bytes());
        for state in [encode(&ckx_sdk::kit::wire::ActorState::new("player", 1, 0.5, 0.5)).unwrap(), pose] {
            let actor = Actor { uuid: "u1".into(), player: Some(1), chunk: ChunkPos::new(40, -30, 2), state: Some(state) };
            assert_eq!(position_of(&actor), (40.5, -29.5, 2.5, false));
        }
    }

    #[test]
    fn once_a_faction_has_won_a_scan_reads_nothing_until_a_reset() {
        let seed = r#"{"capture_per_sec":0.5,"scan_ms":1000,"income_ms":1000,"income_per_point":10,"win_score":10,"points":[{"id":"keep","x":2,"y":-7,"z":3,"radius":1}]}"#;
        let mut hub = TestHub::<TerritoryHub>::spawn_with("territory", "valley", seed.as_bytes()).unwrap();
        let world = world_of(&hub);
        let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "faction": "red" })).unwrap();
        world.borrow_mut().push((1, 2, -7, 3));
        let reads = |hub: &TestHub<TerritoryHub>| hub.node_requests().iter().filter(|(op, _)| op == "world.actors_radius").count();
        hub.advance(3_500).unwrap();
        let view: TerritoryView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.winner.as_deref()), (WarPhase::Won, Some("red")));
        let after_win = reads(&hub);
        hub.advance(5_000).unwrap();
        assert_eq!(reads(&hub), after_win, "a won war is not scanned");
        let _: bool = hub.call_typed(Caller::Developer(9), "reset", &()).unwrap();
        hub.advance(2_000).unwrap();
        assert!(reads(&hub) > after_win, "a reset scans again");
    }

    #[test]
    fn a_snapshot_from_the_y_up_starter_starts_again() {
        #[derive(Serialize)]
        enum Platform<'a> {
            Territory(&'a TerritoryHub),
        }
        let (ours, unmarked, platform) = {
            let mut hub = TestHub::<TerritoryHub>::spawn_with("territory", "valley", one_point(r#"{"id":"keep","x":2,"y":0,"z":9}"#).as_bytes()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "faction": "red" })).unwrap();
            let ours = hub.persist().unwrap();
            hub.hub_mut().war.z_up = false;
            (ours, hub.persist().unwrap(), encode(&Platform::Territory(hub.hub())).unwrap())
        };
        {
            let kept = TestHub::<TerritoryHub>::load("territory", "valley", &ours, 1).unwrap();
            assert_eq!(kept.hub().war.config.points[0].id, "keep", "a z-up snapshot loads");
            assert_eq!(kept.hub().war.members.len(), 1);
        }
        {
            let again = TestHub::<TerritoryHub>::load("territory", "valley", &unmarked, 1).unwrap();
            assert!(again.hub().war.z_up);
            assert_eq!(again.hub().war.config, TerritoryConfig::default());
            assert!(again.hub().war.members.is_empty());
        }
        {
            let again = TestHub::<TerritoryHub>::load("territory", "valley", &platform, 1).unwrap();
            assert!(again.hub().war.z_up && again.hub().war.members.is_empty(), "the platform's snapshot starts again too");
        }
        assert!(TestHub::<TerritoryHub>::load("territory", "valley", b"\x01\x02", 1).is_err(), "a broken snapshot is still an error");
    }
}
