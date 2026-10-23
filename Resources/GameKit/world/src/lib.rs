//! The world's clock, weather and resource nodes, advanced on a timer while the app runs, and
//! the liveops events its developers schedule. It is the root hub, so it hears players enter
//! and leave the app, and other hubs watch its `world` topic: the NPC engine's mobs grow bolder
//! at night and under an event's `aggro` modifier. Replaces the `world-engine` and
//! `liveops-scheduler` templates.
//!
//! Endpoints: `state` (anyone), `harvest { node }` (players), and for the app's developers
//! `schedule { name, start_ms, end_ms, modifiers }` and `cancel { name }`. The spawn seed, if
//! any, is a JSON [`Config`].
//!
//! It speaks the kit protocol (`ckx_sdk::kit`). The `world` topic carries the [`Conditions`]
//! as `snapshot` messages; the `nodes` topic carries each resource node's changes as `delta`s
//! (`id`, `type_name` `Node`, `props` `amount` and `regrow_at`). `snapshot { topic }` answers
//! either. Changes may carry an `action_id`; refusals carry `kit:<code>:` codes. It is the root
//! hub, which takes 50 calls a second for the whole app: clients subscribe to its topics
//! rather than polling `state`.
//!
//! The Crowdy SDK's copy of the `world-tick` starter: the conditions also carry `online`, the
//! players in the app's world, published on the next tick after it changes, so a game reads the
//! count from the `world` topic.

use std::collections::BTreeMap;

use ckx_sdk::kit::{self, Containers, Publisher, Receipts, codes};
use ckx_sdk::model::Published;
use ckx_sdk::prelude::*;
use serde_json::json;

pub const TICK_MS: u64 = 1_000;
pub const WORLD_TOPIC: &str = "world";
pub const NODES_TOPIC: &str = "nodes";
/// The longest step the clock takes at once, so a world that was stopped resumes where it was.
const MAX_STEP_MS: u64 = 5_000;
const HARVEST_COOLDOWN_MS: u64 = 500;
/// Ticks between `world` pushes when nothing notable changed, so clients keep the clock.
const PUBLISH_EVERY: u64 = 30;
const MAX_EVENTS: usize = 64;
const MAX_MODIFIERS: usize = 16;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    /// Real milliseconds in one game day.
    pub day_ms: u64,
    pub nodes: u32,
    /// What a full resource node holds.
    pub node_amount: u32,
    /// How long an emptied node takes to grow back.
    pub regrow_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self { day_ms: 20 * 60 * 1000, nodes: 12, node_amount: 5, regrow_ms: 60_000 }
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Weather {
    #[default]
    Clear,
    Rain,
    Storm,
    Fog,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Node {
    pub id: u32,
    pub amount: u32,
    /// When it grows back, in Unix milliseconds; set while it is empty.
    pub regrow_at: Option<u64>,
}

/// A liveops event: between `start_ms` and `end_ms` (Unix milliseconds) its modifiers apply.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Event {
    pub name: String,
    pub start_ms: u64,
    pub end_ms: u64,
    #[serde(default)]
    pub modifiers: BTreeMap<String, f64>,
}

/// What the `world` topic carries.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Conditions {
    /// 0 at midnight, 0.5 at noon.
    pub time_of_day: f64,
    pub night: bool,
    pub weather: Weather,
    /// The active events' modifiers; where events overlap they multiply.
    pub modifiers: BTreeMap<String, f64>,
    /// Players in the app's world. Defaulted, so a snapshot saved before it loads.
    #[serde(default)]
    pub online: u32,
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
pub struct View {
    pub conditions: Conditions,
    pub clock_ms: u64,
    pub nodes: Vec<Node>,
    pub events: Vec<Event>,
    pub online: u32,
}

#[derive(Deserialize)]
struct Harvest {
    node: u32,
}

#[derive(Deserialize)]
struct Cancel {
    name: String,
}

#[derive(Serialize, Deserialize, Default)]
pub struct World {
    pub config: Config,
    /// Game time since the world began, advanced only while it runs.
    pub clock_ms: u64,
    pub last_tick: u64,
    pub ticks: u64,
    pub weather: Weather,
    /// The game time the weather next changes.
    pub weather_until: u64,
    pub rng: u64,
    pub nodes: Vec<Node>,
    pub events: BTreeMap<String, Event>,
    pub online: u32,
    /// Each player's last harvest, for the cooldown.
    pub harvested: BTreeMap<u64, u64>,
    pub published: Option<Conditions>,
}

impl World {
    pub fn new(config: Config, now: u64) -> Self {
        let nodes = (0..config.nodes).map(|id| Node { id, amount: config.node_amount, regrow_at: None }).collect();
        Self { rng: now | 1, last_tick: now, nodes, config, ..Default::default() }
    }

    fn next_rand(&mut self) -> u64 {
        let mut x = self.rng;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.rng = x;
        x
    }

    /// Advances the world to `now` (Unix milliseconds), and returns the nodes that grew back.
    pub fn tick(&mut self, now: u64) -> Vec<u32> {
        self.ticks += 1;
        self.clock_ms += now.saturating_sub(self.last_tick).min(MAX_STEP_MS);
        self.last_tick = now;
        if self.clock_ms >= self.weather_until {
            self.weather = match self.next_rand() % 10 {
                0..=5 => Weather::Clear,
                6 | 7 => Weather::Rain,
                8 => Weather::Storm,
                _ => Weather::Fog,
            };
            let hour = (self.config.day_ms / 24).max(1);
            self.weather_until = self.clock_ms + hour * (2 + self.next_rand() % 5);
        }
        let amount = self.config.node_amount;
        let mut regrown = Vec::new();
        for n in &mut self.nodes {
            if n.regrow_at.is_some_and(|at| now >= at) {
                n.amount = amount;
                n.regrow_at = None;
                regrown.push(n.id);
            }
        }
        self.harvested.retain(|_, at| now.saturating_sub(*at) < 60_000);
        self.events.retain(|_, e| e.end_ms > now);
        regrown
    }

    pub fn conditions(&self, now: u64) -> Conditions {
        let day = self.config.day_ms.max(1);
        let time_of_day = (self.clock_ms % day) as f64 / day as f64;
        let mut modifiers = BTreeMap::new();
        for e in self.events.values().filter(|e| e.start_ms <= now && now < e.end_ms) {
            for (k, v) in &e.modifiers {
                *modifiers.entry(k.clone()).or_insert(1.0) *= v;
            }
        }
        Conditions { time_of_day, night: !(0.25..0.75).contains(&time_of_day), weather: self.weather, modifiers, online: self.online }
    }

    pub fn harvest(&mut self, player: u64, node: u32, now: u64) -> Result<u32> {
        if self.harvested.get(&player).is_some_and(|at| now < at + HARVEST_COOLDOWN_MS) {
            return Err(kit::error(codes::COOLDOWN, "harvesting too fast"));
        }
        let regrow_at = now + self.config.regrow_ms;
        let n =
            self.nodes.iter_mut().find(|n| n.id == node).ok_or_else(|| kit::error(codes::NOT_FOUND, format!("no resource node {node}")))?;
        if n.amount == 0 {
            return Err(kit::error(codes::EMPTY, "that node is empty; it grows back"));
        }
        n.amount -= 1;
        if n.amount == 0 {
            n.regrow_at = Some(regrow_at);
        }
        self.harvested.insert(player, now);
        Ok(n.amount)
    }

    pub fn schedule(&mut self, event: Event, now: u64) -> Result<()> {
        if event.name.is_empty() || event.name.len() > 64 {
            return Err(kit::error(codes::BAD_REQUEST, "an event name is 1-64 characters"));
        }
        if event.end_ms <= event.start_ms || event.end_ms <= now {
            return Err(kit::error(codes::BAD_REQUEST, "an event ends after it starts, and in the future"));
        }
        if event.modifiers.len() > MAX_MODIFIERS || event.modifiers.values().any(|v| !v.is_finite() || *v <= 0.0 || *v > 100.0) {
            return Err(kit::error(codes::BAD_REQUEST, format!("at most {MAX_MODIFIERS} modifiers, each above 0 and at most 100")));
        }
        if !self.events.contains_key(&event.name) && self.events.len() >= MAX_EVENTS {
            return Err(kit::error(codes::LIMIT, format!("at most {MAX_EVENTS} events are scheduled at once")));
        }
        self.events.insert(event.name.clone(), event);
        Ok(())
    }

    /// A resource node as the `nodes` topic shows it.
    pub fn node_delta(&self, id: u32) -> Option<Published> {
        let n = self.nodes.iter().find(|n| n.id == id)?;
        let props = BTreeMap::from([("amount".to_owned(), json!(n.amount)), ("regrow_at".to_owned(), json!(n.regrow_at))]);
        Some(Published { id: u64::from(n.id), type_name: Some("Node".to_owned()), props: Some(props), removed: false })
    }

    pub fn node_collection(&self) -> Containers {
        Containers { containers: self.nodes.iter().filter_map(|n| self.node_delta(n.id)).collect() }
    }

    pub fn view(&self, now: u64) -> View {
        View {
            conditions: self.conditions(now),
            clock_ms: self.clock_ms,
            nodes: self.nodes.clone(),
            events: self.events.values().cloned().collect(),
            online: self.online,
        }
    }
}

/// The hub: the world, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default)]
pub struct WorldHub {
    #[serde(flatten)]
    pub world: World,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

fn developer(call: &Call<'_>) -> Result<u64> {
    call.developer().map_err(|_| kit::error(codes::FORBIDDEN, format!("`{}` is for the app's developers", call.method)))
}

impl WorldHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        match call.method {
            "harvest" => {
                let player = call.player()?;
                let req: Harvest = kit::args(call)?;
                let left = self.world.harvest(player, req.node, now)?;
                if let Some(d) = self.world.node_delta(req.node) {
                    self.kit.delta(ctx, NODES_TOPIC, &d)?;
                }
                encode(&left)
            }
            "schedule" => {
                developer(call)?;
                self.world.schedule(kit::args(call)?, now)?;
                ctx.persist_now();
                self.publish_conditions(ctx, now, true)?;
                encode(&true)
            }
            "cancel" => {
                developer(call)?;
                let req: Cancel = kit::args(call)?;
                let removed = self.world.events.remove(&req.name).is_some();
                self.publish_conditions(ctx, now, true)?;
                encode(&removed)
            }
            other => Err(Error::unknown_method(other)),
        }
    }

    /// Publishes the conditions when they changed in a way players notice, or `always`, and
    /// every [`PUBLISH_EVERY`] ticks regardless so clients keep the clock.
    fn publish_conditions(&mut self, ctx: &Ctx, now: u64, always: bool) -> Result<()> {
        let c = self.world.conditions(now);
        let notable = self
            .world
            .published
            .as_ref()
            .is_none_or(|p| p.night != c.night || p.weather != c.weather || p.modifiers != c.modifiers || p.online != c.online);
        if always || notable || self.world.ticks.is_multiple_of(PUBLISH_EVERY) {
            self.kit.snapshot(ctx, WORLD_TOPIC, &c)?;
            self.world.published = Some(c);
        }
        Ok(())
    }
}

impl Hub for WorldHub {
    fn spawn(ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let config = if seed.is_empty() {
            Config::default()
        } else {
            serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?
        };
        ctx.timer_every("tick", TICK_MS)?;
        Ok(Self { world: World::new(config, ctx.now_ms()), ..Default::default() })
    }

    /// Reads this version's snapshots and the earlier ones, which held the world alone.
    fn load(_ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        decode(snapshot)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        match call.method {
            "state" => encode(&self.world.view(now)),
            kit::SNAPSHOT_METHOD => match kit::snapshot_topic(&call, WORLD_TOPIC).as_str() {
                WORLD_TOPIC => self.kit.snapshot_reply(ctx, WORLD_TOPIC, &self.world.conditions(now)),
                NODES_TOPIC => self.kit.snapshot_reply(ctx, NODES_TOPIC, &self.world.node_collection()),
                other => Err(kit::error(codes::NOT_FOUND, format!("no topic `{other}`"))),
            },
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "tick" {
            return Ok(());
        }
        let now = ctx.now_ms();
        for id in self.world.tick(now) {
            if let Some(d) = self.world.node_delta(id) {
                self.kit.delta(ctx, NODES_TOPIC, &d)?;
            }
        }
        self.publish_conditions(ctx, now, false)
    }

    fn on_presence(&mut self, _ctx: &Ctx, presence: &Presence) -> Result<()> {
        self.world.online = presence.count;
        Ok(())
    }
}

ckx_sdk::export_hub!(WorldHub);

#[cfg(test)]
mod tests {
    use super::*;

    fn world() -> World {
        World::new(Config { day_ms: 24_000, nodes: 2, node_amount: 2, regrow_ms: 1_000 }, 1_000)
    }

    #[test]
    fn the_clock_steps_at_most_five_seconds_so_a_stopped_world_resumes() {
        let mut w = world();
        w.tick(2_000);
        assert_eq!(w.clock_ms, 1_000);
        w.tick(1_000_000);
        assert_eq!(w.clock_ms, 1_000 + MAX_STEP_MS);
    }

    #[test]
    fn night_is_the_first_and_last_quarter_of_the_day() {
        let mut w = world();
        assert!(w.conditions(0).night);
        w.clock_ms = 12_000;
        assert!(!w.conditions(0).night);
        w.clock_ms = 20_000;
        assert!(w.conditions(0).night);
    }

    #[test]
    fn a_node_empties_cools_down_and_grows_back() {
        let mut w = world();
        assert_eq!(w.harvest(7, 0, 10_000).unwrap(), 1);
        assert!(w.harvest(7, 0, 10_100).is_err(), "cooldown");
        assert_eq!(w.harvest(7, 0, 10_600).unwrap(), 0);
        assert!(w.harvest(8, 0, 10_700).is_err(), "empty");
        assert!(w.harvest(8, 9, 10_700).is_err(), "no such node");
        w.tick(11_700);
        assert_eq!(w.nodes[0].amount, 2);
    }

    #[test]
    fn events_are_bounded_active_in_their_window_and_multiply() {
        let mut w = world();
        let ev = |name: &str, start, end, aggro| Event {
            name: name.into(),
            start_ms: start,
            end_ms: end,
            modifiers: BTreeMap::from([("aggro".to_string(), aggro)]),
        };
        w.schedule(ev("a", 100, 5_000, 2.0), 0).unwrap();
        w.schedule(ev("b", 200, 5_000, 1.5), 0).unwrap();
        assert!(w.schedule(ev("c", 300, 200, 1.0), 0).is_err(), "ends before it starts");
        assert!(w.schedule(ev("d", 300, 400, 0.0), 0).is_err(), "modifier out of range");
        assert_eq!(w.conditions(50).modifiers.get("aggro"), None);
        assert_eq!(w.conditions(250).modifiers.get("aggro"), Some(&3.0));
        w.tick(6_000);
        assert!(w.events.is_empty(), "ended events are dropped");
    }

    #[test]
    fn a_seed_is_a_json_config_with_defaults() {
        let c: Config = serde_json::from_slice(br#"{"nodes": 3}"#).unwrap();
        assert_eq!((c.nodes, c.node_amount), (3, Config::default().node_amount));
    }

    #[derive(Serialize)]
    struct H<'a> {
        node: u32,
        action_id: &'a str,
    }

    #[test]
    fn harvests_and_regrowth_are_deltas_on_the_nodes_topic_and_a_resent_harvest_takes_once() {
        use ckx_sdk::kit::{Message, MessageKind};
        use ckx_sdk::testing::TestHub;
        let seed = br#"{"nodes": 2, "node_amount": 1, "regrow_ms": 1000}"#;
        let mut hub = TestHub::<WorldHub>::spawn_with("world", "", seed).unwrap();
        let left: u32 = hub.call_typed(Caller::Player(1), "harvest", &H { node: 0, action_id: "h1" }).unwrap();
        let again: u32 = hub.call_typed(Caller::Player(1), "harvest", &H { node: 0, action_id: "h1" }).unwrap();
        assert_eq!((left, again), (0, 0));
        let e = hub.call(Caller::Player(1), "harvest", &encode(&H { node: 0, action_id: "h2" }).unwrap()).unwrap_err();
        assert_eq!(kit::error_code(&e.message).map(|c| c.0), Some(codes::COOLDOWN));
        hub.advance(1_500).unwrap();
        let deltas: Vec<Message<Published>> =
            hub.published().iter().filter(|(t, _)| t == NODES_TOPIC).map(|(_, b)| decode(b).unwrap()).collect();
        assert_eq!(deltas.len(), 2, "emptied, then grown back");
        assert!(deltas.iter().all(|m| m.header.kind == MessageKind::Delta && m.body.id == 0));
        assert_eq!(deltas[1].body.props.as_ref().unwrap()["amount"], json!(1));
        let conditions: Vec<Message<Conditions>> =
            hub.published().iter().filter(|(t, _)| t == WORLD_TOPIC).map(|(_, b)| decode(b).unwrap()).collect();
        assert!(!conditions.is_empty(), "the first tick publishes the conditions");
        let nodes: Message<Containers> = hub.call_typed(Caller::Player(2), "snapshot", &json!({ "topic": NODES_TOPIC })).unwrap();
        assert_eq!((nodes.body.containers.len(), nodes.header.seq), (2, 2));
        assert!(hub.call(Caller::Player(1), "schedule", &[]).is_err(), "developers only");
    }

    #[test]
    fn the_players_online_ride_the_world_topic_from_the_next_tick() {
        use ckx_sdk::kit::Message;
        use ckx_sdk::testing::TestHub;
        let mut hub = TestHub::<WorldHub>::spawn("world", "").unwrap();
        hub.advance(1_000).unwrap();
        hub.clear();
        hub.presence(&Presence { count: 3, ..Default::default() }).unwrap();
        hub.advance(1_000).unwrap();
        let conditions: Vec<Message<Conditions>> =
            hub.published().iter().filter(|(t, _)| t == WORLD_TOPIC).map(|(_, b)| decode(b).unwrap()).collect();
        assert_eq!(conditions.last().map(|m| m.body.online), Some(3), "a new count is published on the next tick");
        let read: Message<Conditions> = hub.call_typed(Caller::Player(1), "snapshot", &json!({ "topic": WORLD_TOPIC })).unwrap();
        assert_eq!(read.body.online, 3);
    }

    #[test]
    fn a_snapshot_from_before_the_count_was_published_loads() {
        #[derive(Serialize)]
        struct OldConditions {
            time_of_day: f64,
            night: bool,
            weather: Weather,
            modifiers: BTreeMap<String, f64>,
        }
        let old = OldConditions { time_of_day: 0.5, night: false, weather: Weather::Rain, modifiers: BTreeMap::new() };
        let read: Conditions = decode(&encode(&old).unwrap()).unwrap();
        assert_eq!((read.weather, read.online), (Weather::Rain, 0));
    }

    #[test]
    fn a_snapshot_from_before_the_kit_protocol_loads() {
        use ckx_sdk::testing::TestHub;
        let old = encode(&world()).unwrap();
        let hub = TestHub::<WorldHub>::load("world", "", &old, 1).unwrap();
        assert_eq!(hub.hub().world.nodes.len(), 2);
    }
}
