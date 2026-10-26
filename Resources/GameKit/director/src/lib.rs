//! An encounter, keyed by its id: waves of enemies down a lane to the players' base, towers the
//! players build, and a boss that changes phase as it weakens. The tower-defense and horde
//! shape: enemies follow one flow field (`ckx_sdk::kit::ai::flow`) from the spawn to the base, a
//! tower may not cut the lane, towers strike what comes into range, players strike too, kills
//! pay gold, and an enemy that reaches the base takes from its health. The waves
//! (`ckx_sdk::kit::sim::waves`) start when the last is cleared, after a delay; the last brings the
//! boss. Replace the [`Config`] with your game's lane, waves and numbers.
//!
//! Endpoints (players): `join`, `start` (the first to join, or a developer), `build { x, z }`,
//! `hit { enemy }`, `state` (anyone). It speaks the kit protocol (`ckx_sdk::kit`): the `encounter`
//! topic carries the encounter as `snapshot` messages once a second while it runs, and `event`
//! messages (`type`: `wave_started`, `wave_cleared`, `boss_phase`, `won`, `lost`) as they happen;
//! `snapshot { topic: "encounter" }` answers it; changes may carry an `action_id`; refusals carry
//! `kit:<code>:` codes.
//!
//! This is the Unreal SDK's copy of the platform's starter, unchanged.

use std::collections::{BTreeMap, BTreeSet};

use ckx_sdk::kit::ai::flow::FlowField;
use ckx_sdk::kit::ai::path::GridProvider;
use ckx_sdk::kit::play::combat::Cooldowns;
use ckx_sdk::kit::sim::waves::{WaveDef, WaveEvent, WavePhase, WaveSchedule};
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;
use serde_json::json;

pub const TOPIC: &str = "encounter";
pub const TICK_MS: u64 = 500;
/// Ticks between `encounter` snapshots.
const PUBLISH_EVERY: u64 = 2;
const MAX_ENEMIES: usize = 200;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct EnemyKind {
    pub id: String,
    pub hp: i64,
    /// Ticks between steps (1 is a cell a tick).
    pub step_ticks: u32,
    /// Base health it takes on arrival.
    pub leak: i64,
    /// Gold its kill pays.
    pub bounty: i64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    pub width: i32,
    pub height: i32,
    pub spawn: (i32, i32),
    pub base: (i32, i32),
    pub base_hp: i64,
    pub gold: i64,
    pub tower_cost: i64,
    /// Damage a tower deals a tick, and how far it reaches (cells, Manhattan).
    pub tower_damage: i64,
    pub tower_range: i32,
    /// A player's strike: damage and cooldown.
    pub hit_damage: i64,
    pub hit_cooldown_ms: u64,
    pub kinds: Vec<EnemyKind>,
    pub waves: Vec<WaveDef>,
    /// The kind the last wave's boss is; none for no boss.
    pub boss: Option<EnemyKind>,
}

impl Default for Config {
    fn default() -> Self {
        let grunt = EnemyKind { id: "grunt".into(), hp: 20, step_ticks: 2, leak: 1, bounty: 5 };
        let runner = EnemyKind { id: "runner".into(), hp: 10, step_ticks: 1, leak: 1, bounty: 4 };
        Self {
            width: 12,
            height: 8,
            spawn: (0, 4),
            base: (11, 4),
            base_hp: 10,
            gold: 30,
            tower_cost: 10,
            tower_damage: 4,
            tower_range: 2,
            hit_damage: 5,
            hit_cooldown_ms: 500,
            kinds: vec![grunt, runner],
            waves: vec![
                WaveDef { units: vec![("grunt".into(), 4)], delay_ms: 3_000 },
                WaveDef { units: vec![("grunt".into(), 4), ("runner".into(), 3)], delay_ms: 5_000 },
                WaveDef { units: vec![("runner".into(), 6)], delay_ms: 5_000 },
            ],
            boss: Some(EnemyKind { id: "warlord".into(), hp: 200, step_ticks: 3, leak: 5, bounty: 50 }),
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Lobby,
    Running,
    Won,
    Lost,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Enemy {
    pub id: u32,
    pub kind: String,
    pub x: i32,
    pub z: i32,
    pub hp: i64,
    pub max_hp: i64,
    pub boss: bool,
    /// Ticks until its next step.
    pub wait: u32,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Tower {
    pub x: i32,
    pub z: i32,
    pub owner: u64,
}

/// The encounter as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct EncounterView {
    pub phase: Phase,
    pub players: Vec<u64>,
    pub width: i32,
    pub height: i32,
    pub spawn: (i32, i32),
    pub base: (i32, i32),
    pub base_hp: i64,
    pub gold: i64,
    pub wave: Option<usize>,
    pub waves: usize,
    /// When the next wave starts, while one counts down.
    pub next_wave_at: Option<u64>,
    pub enemies: Vec<Enemy>,
    pub towers: Vec<Tower>,
    pub boss_phase: u32,
    pub kills: BTreeMap<u64, u32>,
}

#[derive(Deserialize)]
struct Build {
    x: i32,
    z: i32,
}

#[derive(Deserialize)]
struct Hit {
    enemy: u32,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Encounter {
    pub config: Config,
    pub phase: Phase,
    pub players: Vec<u64>,
    pub base_hp: i64,
    pub gold: i64,
    pub schedule: WaveSchedule,
    pub enemies: Vec<Enemy>,
    pub towers: Vec<Tower>,
    pub next_id: u32,
    pub ticks: u64,
    pub boss_phase: u32,
    pub kills: BTreeMap<u64, u32>,
    pub cooldowns: Cooldowns,
    /// Events since they were last published.
    #[serde(skip)]
    pub events: Vec<serde_json::Value>,
}

fn manhattan(a: (i32, i32), b: (i32, i32)) -> i32 {
    (a.0 - b.0).abs() + (a.1 - b.1).abs()
}

impl Encounter {
    fn towers_at(&self) -> BTreeSet<(i32, i32)> {
        self.towers.iter().map(|t| (t.x, t.z)).collect()
    }

    /// The lane: every free cell's distance to the base.
    pub fn field(&self, extra: Option<(i32, i32)>) -> FlowField {
        let mut blocked = self.towers_at();
        blocked.extend(extra);
        let is_blocked = |x: i32, z: i32| blocked.contains(&(x, z));
        let grid = GridProvider { width: self.config.width, height: self.config.height, blocked: &is_blocked };
        FlowField::build(&grid, (0, 0, self.config.width, self.config.height), &[self.config.base], 4_096)
    }

    pub fn view(&self, now: u64) -> EncounterView {
        let (wave, next_wave_at) = match &self.schedule.phase {
            WavePhase::Active { index } => (Some(*index), None),
            WavePhase::Countdown { next, starts_at_ms } => (next.checked_sub(1), Some(*starts_at_ms)),
            WavePhase::Complete => (None, None),
        };
        let _ = now;
        EncounterView {
            phase: self.phase,
            players: self.players.clone(),
            width: self.config.width,
            height: self.config.height,
            spawn: self.config.spawn,
            base: self.config.base,
            base_hp: self.base_hp,
            gold: self.gold,
            wave,
            waves: self.config.waves.len(),
            next_wave_at,
            enemies: self.enemies.clone(),
            towers: self.towers.clone(),
            boss_phase: self.boss_phase,
            kills: self.kills.clone(),
        }
    }

    fn event(&mut self, kind: &str, extra: serde_json::Value) {
        let mut e = json!({ "type": kind });
        if let (Some(e), Some(x)) = (e.as_object_mut(), extra.as_object()) {
            e.extend(x.clone());
        }
        self.events.push(e);
    }

    pub fn join(&mut self, player: u64) -> Result<()> {
        if !self.players.contains(&player) {
            if self.players.len() >= 8 {
                return Err(kit::error(codes::FULL, "the encounter is full"));
            }
            self.players.push(player);
        }
        Ok(())
    }

    pub fn start(&mut self, now: u64) -> Result<()> {
        if self.phase == Phase::Running {
            return Err(kit::error(codes::STARTED, "the encounter is running"));
        }
        if self.field(None).distance(self.config.spawn.0, self.config.spawn.1).is_none() {
            return Err(kit::error(codes::BAD_REQUEST, "the spawn has no way to the base"));
        }
        self.phase = Phase::Running;
        self.base_hp = self.config.base_hp;
        self.gold = self.config.gold;
        self.enemies.clear();
        self.towers.clear();
        self.boss_phase = 0;
        self.kills.clear();
        self.schedule = WaveSchedule::start(&self.config.waves, now);
        Ok(())
    }

    pub fn build(&mut self, player: u64, at: (i32, i32)) -> Result<i64> {
        if self.phase != Phase::Running {
            return Err(kit::error(codes::CONFLICT, "the encounter is not running"));
        }
        if !self.players.contains(&player) {
            return Err(kit::error(codes::NOT_MEMBER, "join the encounter first"));
        }
        let (w, h) = (self.config.width, self.config.height);
        if at.0 < 0 || at.1 < 0 || at.0 >= w || at.1 >= h || at == self.config.spawn || at == self.config.base {
            return Err(kit::error(codes::BAD_REQUEST, "build on a free cell of the field"));
        }
        if self.towers_at().contains(&at) || self.enemies.iter().any(|e| (e.x, e.z) == at) {
            return Err(kit::error(codes::CONFLICT, "that cell is taken"));
        }
        if self.gold < self.config.tower_cost {
            return Err(kit::error(codes::INSUFFICIENT, "not enough gold"));
        }
        if self.field(Some(at)).distance(self.config.spawn.0, self.config.spawn.1).is_none() {
            return Err(kit::error("blocks_lane", "a tower there would cut the lane"));
        }
        self.gold -= self.config.tower_cost;
        self.towers.push(Tower { x: at.0, z: at.1, owner: player });
        Ok(self.gold)
    }

    pub fn hit(&mut self, player: u64, enemy: u32, now: u64) -> Result<i64> {
        if !self.players.contains(&player) {
            return Err(kit::error(codes::NOT_MEMBER, "join the encounter first"));
        }
        if !self.cooldowns.take(&player.to_string(), "hit", self.config.hit_cooldown_ms, now) {
            return Err(kit::error(codes::COOLDOWN, "striking too fast"));
        }
        let damage = self.config.hit_damage;
        let i = self.enemies.iter().position(|e| e.id == enemy).ok_or_else(|| kit::error(codes::NOT_FOUND, format!("no enemy {enemy}")))?;
        self.enemies[i].hp -= damage;
        let left = self.enemies[i].hp.max(0);
        self.reap(Some(player));
        Ok(left)
    }

    fn spawn_wave(&mut self, index: usize) {
        let wave = self.config.waves.get(index).cloned();
        let last = index + 1 == self.config.waves.len();
        let (sx, sz) = self.config.spawn;
        for (kind, count) in wave.map(|w| w.units).unwrap_or_default() {
            let Some(k) = self.config.kinds.iter().find(|k| k.id == kind).cloned() else { continue };
            for _ in 0..count {
                self.add(&k, false, (sx, sz));
            }
        }
        if last && let Some(boss) = self.config.boss.clone() {
            self.add(&boss, true, (sx, sz));
        }
        self.event("wave_started", json!({ "wave": index }));
    }

    fn add(&mut self, k: &EnemyKind, boss: bool, at: (i32, i32)) {
        if self.enemies.len() >= MAX_ENEMIES {
            return;
        }
        self.next_id += 1;
        // Enemies wait their turn at the spawn so they come one after another.
        let queued = self.enemies.iter().filter(|e| (e.x, e.z) == at).count() as u32;
        self.enemies.push(Enemy {
            id: self.next_id,
            kind: k.id.clone(),
            x: at.0,
            z: at.1,
            hp: k.hp,
            max_hp: k.hp,
            boss,
            wait: k.step_ticks.max(1) * queued,
        });
    }

    fn kind(&self, e: &Enemy) -> EnemyKind {
        if e.boss {
            return self.config.boss.clone().unwrap_or(EnemyKind { id: e.kind.clone(), hp: e.max_hp, step_ticks: 3, leak: 5, bounty: 50 });
        }
        self.config.kinds.iter().find(|k| k.id == e.kind).cloned().unwrap_or(EnemyKind {
            id: e.kind.clone(),
            hp: e.max_hp,
            step_ticks: 2,
            leak: 1,
            bounty: 0,
        })
    }

    /// Takes the dead off the field and pays their bounty.
    fn reap(&mut self, killer: Option<u64>) {
        let dead: Vec<Enemy> = self.enemies.iter().filter(|e| e.hp <= 0).cloned().collect();
        self.enemies.retain(|e| e.hp > 0);
        for e in dead {
            self.gold += self.kind(&e).bounty;
            if let Some(p) = killer {
                *self.kills.entry(p).or_insert(0) += 1;
            }
        }
        if let Some(boss) = self.enemies.iter().find(|e| e.boss) {
            let phase = match boss.hp * 4 / boss.max_hp.max(1) {
                3.. => 0,
                2 => 1,
                1 => 2,
                _ => 3,
            };
            if phase > self.boss_phase {
                self.boss_phase = phase;
                self.event("boss_phase", json!({ "phase": phase }));
            }
        }
    }

    /// One tick: waves, steps, towers, leaks. Returns whether the encounter ran.
    pub fn tick(&mut self, now: u64) -> bool {
        if self.phase != Phase::Running {
            return false;
        }
        self.ticks += 1;
        let field = self.field(None);
        let mut leaked = 0;
        let paces: Vec<u32> = self.enemies.iter().map(|e| self.kind(e).step_ticks.max(1)).collect();
        for (e, pace) in self.enemies.iter_mut().zip(paces) {
            if e.wait > 0 {
                e.wait -= 1;
                continue;
            }
            if let Some((dx, dz)) = field.step(e.x, e.z) {
                e.x += dx;
                e.z += dz;
            }
            e.wait = pace - 1;
        }
        let base = self.config.base;
        let arriving: Vec<Enemy> = self.enemies.iter().filter(|e| (e.x, e.z) == base).cloned().collect();
        self.enemies.retain(|e| (e.x, e.z) != base);
        for e in &arriving {
            leaked += self.kind(e).leak;
        }
        let towers = self.towers.clone();
        for t in &towers {
            let target = self
                .enemies
                .iter_mut()
                .filter(|e| manhattan((e.x, e.z), (t.x, t.z)) <= self.config.tower_range)
                .min_by_key(|e| (manhattan((e.x, e.z), base), e.id));
            if let Some(e) = target {
                e.hp -= self.config.tower_damage;
            }
        }
        self.reap(None);
        self.base_hp -= leaked;
        if self.base_hp <= 0 {
            self.base_hp = 0;
            self.phase = Phase::Lost;
            self.event("lost", json!({}));
            return true;
        }
        let cleared = self.enemies.is_empty();
        let was = self.schedule.phase.clone();
        match self.schedule.step(&self.config.waves, now, cleared) {
            Some(WaveEvent::Start(i)) => {
                if let WavePhase::Active { index } = was {
                    self.event("wave_cleared", json!({ "wave": index }));
                }
                self.spawn_wave(i);
            }
            Some(WaveEvent::Finished) => {
                self.phase = Phase::Won;
                self.event("won", json!({}));
            }
            None => {
                if let (WavePhase::Active { index }, WavePhase::Countdown { .. }) = (&was, &self.schedule.phase) {
                    let index = *index;
                    self.event("wave_cleared", json!({ "wave": index }));
                }
            }
        }
        true
    }
}

/// The hub: the encounter, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct EncounterHub {
    #[serde(flatten)]
    pub encounter: Encounter,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl EncounterHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let e = &mut self.encounter;
        let out = match call.method {
            "join" => {
                e.join(call.player()?)?;
                encode(&true)
            }
            "start" => {
                let allowed = match call.caller {
                    Caller::Developer(_) => true,
                    Caller::Player(p) => e.players.first() == Some(&p),
                    _ => false,
                };
                if !allowed {
                    return Err(kit::error(codes::FORBIDDEN, "the first to join, or a developer, starts it"));
                }
                e.start(now)?;
                ctx.timer_every("tick", TICK_MS)?;
                encode(&true)
            }
            "build" => {
                let b: Build = kit::args(call)?;
                encode(&e.build(call.player()?, (b.x, b.z))?)
            }
            "hit" => {
                let h: Hit = kit::args(call)?;
                encode(&e.hit(call.player()?, h.enemy, now)?)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.publish(ctx, now)?;
        out
    }

    fn publish(&mut self, ctx: &Ctx, now: u64) -> Result<()> {
        for e in std::mem::take(&mut self.encounter.events) {
            self.kit.event(ctx, TOPIC, &e)?;
        }
        self.kit.snapshot(ctx, TOPIC, &self.encounter.view(now))
    }
}

impl Hub for EncounterHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut hub = Self::default();
        if !seed.is_empty() {
            hub.encounter.config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
        }
        hub.encounter.base_hp = hub.encounter.config.base_hp;
        hub.encounter.gold = hub.encounter.config.gold;
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
            "state" => encode(&self.encounter.view(now)),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.encounter.view(now)),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "tick" {
            return Ok(());
        }
        let now = ctx.now_ms();
        if !self.encounter.tick(now) {
            ctx.cancel_timer("tick");
            return Ok(());
        }
        let ended = self.encounter.phase != Phase::Running;
        if ended {
            ctx.cancel_timer("tick");
            ctx.persist_now();
        }
        if ended || !self.encounter.events.is_empty() || self.encounter.ticks.is_multiple_of(PUBLISH_EVERY) {
            self.publish(ctx, now)?;
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(EncounterHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, MessageKind, error_code};
    use ckx_sdk::testing::TestHub;

    fn running() -> Encounter {
        let mut e = Encounter::default();
        e.join(1).unwrap();
        e.start(0).unwrap();
        e
    }

    #[test]
    fn a_tower_may_not_cut_the_lane_and_costs_gold() {
        let mut e =
            Encounter { config: Config { width: 3, height: 3, spawn: (0, 1), base: (2, 1), ..Config::default() }, ..Default::default() };
        e.join(1).unwrap();
        e.start(0).unwrap();
        e.build(1, (1, 0)).unwrap();
        e.build(1, (1, 2)).unwrap();
        let Error(why) = e.build(1, (1, 1)).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some("blocks_lane"));
        assert_eq!(e.gold, Config::default().gold - 2 * Config::default().tower_cost);
        e.gold = 0;
        let Error(why) = e.build(1, (0, 0)).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::INSUFFICIENT));
    }

    #[test]
    fn with_no_towers_the_waves_leak_into_the_base_until_it_falls() {
        let mut e = running();
        let mut now = 0;
        while e.phase == Phase::Running && now < 600_000 {
            now += TICK_MS;
            e.tick(now);
        }
        assert_eq!(e.phase, Phase::Lost);
        assert!(e.events.iter().any(|x| x["type"] == "wave_started"));
    }

    #[test]
    fn towers_along_the_lane_hold_it_and_the_boss_turns_its_phases() {
        let mut e = running();
        e.gold = 10_000;
        for x in 1..11 {
            for z in [3, 5] {
                e.build(1, (x, z)).unwrap();
            }
        }
        let mut now = 0;
        while e.phase == Phase::Running && now < 3_600_000 {
            now += TICK_MS;
            e.tick(now);
        }
        assert_eq!(e.phase, Phase::Won, "base {} gold {}", e.base_hp, e.gold);
        assert!(e.events.iter().any(|x| x["type"] == "boss_phase"));
        assert!(e.events.iter().any(|x| x["type"] == "wave_cleared"));
    }

    #[test]
    fn players_strike_on_a_cooldown_and_the_topic_carries_snapshots_and_events() {
        let mut hub = TestHub::<EncounterHub>::spawn("director", "gate").unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let e = hub.call(Caller::Player(2), "start", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(error_code(&e.message).map(|c| c.0), Some(codes::FORBIDDEN));
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(4_000).unwrap();
        let view: EncounterView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        let enemy = view.enemies.first().expect("the first wave is out").id;
        let _: i64 = hub.call_typed(Caller::Player(1), "hit", &serde_json::json!({ "enemy": enemy })).unwrap();
        let e = hub.call(Caller::Player(1), "hit", &encode(&serde_json::json!({ "enemy": enemy })).unwrap()).unwrap_err();
        assert_eq!(error_code(&e.message).map(|c| c.0), Some(codes::COOLDOWN));
        #[derive(Deserialize)]
        struct K {
            kind: MessageKind,
        }
        let kinds: Vec<MessageKind> = hub.published().iter().map(|(_, b)| decode::<K>(b).unwrap().kind).collect();
        assert!(kinds.contains(&MessageKind::Event) && kinds.contains(&MessageKind::Snapshot));
        let started: Vec<Message<serde_json::Value>> = hub
            .published()
            .iter()
            .filter(|(_, b)| decode::<K>(b).unwrap().kind == MessageKind::Event)
            .map(|(_, b)| decode(b).unwrap())
            .collect();
        assert_eq!(started[0].body["type"], "wave_started");
    }
}
