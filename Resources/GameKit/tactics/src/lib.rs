//! A grid battle, keyed by its id: players' units against the hub's, in turns. Each player who
//! joins brings a unit; the host starts, and enemies take the far side. On the players' phase each
//! unit may move (within its move points, around obstacles and other units) and attack once; when
//! every player has ended their turn, or the phase's time runs out, the enemies move and strike
//! (`ckx_sdk::kit::ai::turn`), and the round turns. The battle ends when one side has no unit
//! standing. Replace [`damage`] and the [`Config`] with your game's numbers.
//!
//! Endpoints (players): `join`, `start` (the host), `move { x, z }`, `attack { target }`,
//! `end_turn`, `state` (anyone). It speaks the kit protocol (`ckx_sdk::kit`): the `battle` topic
//! carries the battle as `snapshot` messages, and so does `snapshot { topic: "battle" }`; changes
//! may carry an `action_id`; refusals carry `kit:<code>:` codes.
//!
//! This is the Unreal SDK's copy of the platform's starter, unchanged.

use std::collections::{BTreeMap, BTreeSet, VecDeque};

use ckx_sdk::kit::ai::path::GridProvider;
use ckx_sdk::kit::ai::turn::{approach_scorer, best_move, reachable};
use ckx_sdk::kit::{self, Publisher, Receipts, Rng, codes};
use ckx_sdk::prelude::*;

pub const TOPIC: &str = "battle";
const MAX_PLAYERS: usize = 4;
const LOG: usize = 24;
const EXPAND: usize = 512;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(default)]
pub struct Config {
    pub width: i32,
    pub height: i32,
    /// Obstacles scattered at the start, out of every 100 cells.
    pub obstacles_pct: u32,
    pub phase_ms: u64,
    pub player_unit: UnitStats,
    pub enemy_unit: UnitStats,
    /// Enemies per player, plus one.
    pub enemies_per_player: u32,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            width: 8,
            height: 8,
            obstacles_pct: 10,
            phase_ms: 60_000,
            player_unit: UnitStats { hp: 30, attack: 9, defense: 3, moves: 4, range: 1 },
            enemy_unit: UnitStats { hp: 18, attack: 7, defense: 2, moves: 3, range: 1 },
            enemies_per_player: 1,
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct UnitStats {
    pub hp: i64,
    pub attack: i64,
    pub defense: i64,
    pub moves: u32,
    /// How far an attack reaches, in cells (Manhattan).
    pub range: i32,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq, Default)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Lobby,
    Players,
    Enemies,
    Won,
    Lost,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Team {
    Players,
    Enemies,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Unit {
    pub id: u32,
    pub team: Team,
    /// The player it belongs to; none for the hub's.
    pub owner: Option<u64>,
    pub x: i32,
    pub z: i32,
    pub hp: i64,
    pub stats: UnitStats,
    pub moved: bool,
    pub acted: bool,
}

impl Unit {
    fn alive(&self) -> bool {
        self.hp > 0
    }
}

/// What a strike does: attack less defense, at least 1. The starter's rule.
pub fn damage(attacker: &UnitStats, defender: &UnitStats) -> i64 {
    (attacker.attack - defender.defense).max(1)
}

fn manhattan(a: (i32, i32), b: (i32, i32)) -> i32 {
    (a.0 - b.0).abs() + (a.1 - b.1).abs()
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Battle {
    pub config: Config,
    pub phase: Phase,
    pub players: Vec<u64>,
    pub host: Option<u64>,
    pub round: u32,
    pub obstacles: BTreeSet<(i32, i32)>,
    pub units: Vec<Unit>,
    pub ended_turn: BTreeSet<u64>,
    pub phase_ends: u64,
    pub log: VecDeque<String>,
}

/// The battle as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct BattleView {
    pub phase: Phase,
    pub round: u32,
    pub width: i32,
    pub height: i32,
    pub obstacles: Vec<(i32, i32)>,
    pub units: Vec<Unit>,
    pub players: Vec<u64>,
    pub host: Option<u64>,
    pub ended_turn: Vec<u64>,
    pub phase_ends: Option<u64>,
    pub log: Vec<String>,
}

#[derive(Deserialize)]
struct Move {
    x: i32,
    z: i32,
}

#[derive(Deserialize)]
struct Attack {
    target: u32,
}

impl Battle {
    pub fn view(&self) -> BattleView {
        BattleView {
            phase: self.phase,
            round: self.round,
            width: self.config.width,
            height: self.config.height,
            obstacles: self.obstacles.iter().copied().collect(),
            units: self.units.clone(),
            players: self.players.clone(),
            host: self.host,
            ended_turn: self.ended_turn.iter().copied().collect(),
            phase_ends: matches!(self.phase, Phase::Players).then_some(self.phase_ends),
            log: self.log.iter().cloned().collect(),
        }
    }

    fn note(&mut self, line: String) {
        self.log.push_back(line);
        while self.log.len() > LOG {
            self.log.pop_front();
        }
    }

    fn blocked(&self, at: (i32, i32), ignore: u32) -> bool {
        self.obstacles.contains(&at) || self.units.iter().any(|u| u.alive() && u.id != ignore && (u.x, u.z) == at)
    }

    pub fn join(&mut self, player: u64) -> Result<()> {
        if self.players.contains(&player) {
            return Ok(());
        }
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the battle has started"));
        }
        if self.players.len() >= MAX_PLAYERS {
            return Err(kit::error(codes::FULL, "the battle is full"));
        }
        self.players.push(player);
        self.host.get_or_insert(player);
        Ok(())
    }

    /// Lays out the field and the units.
    pub fn start(&mut self, player: u64, seed: u64, now: u64) -> Result<()> {
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the battle has started"));
        }
        if self.host != Some(player) {
            return Err(kit::error(codes::FORBIDDEN, "only the host starts the battle"));
        }
        let (w, h) = (self.config.width.clamp(4, 32), self.config.height.clamp(4, 32));
        self.config.width = w;
        self.config.height = h;
        let mut rng = Rng::seeded(seed);
        self.units.clear();
        let mut next_id = 1;
        for (i, p) in self.players.iter().enumerate() {
            let z = (i as i32 * 2 + 1).min(h - 1);
            self.units.push(Unit {
                id: next_id,
                team: Team::Players,
                owner: Some(*p),
                x: 0,
                z,
                hp: self.config.player_unit.hp,
                stats: self.config.player_unit,
                moved: false,
                acted: false,
            });
            next_id += 1;
        }
        let enemies = (self.players.len() as u32 * self.config.enemies_per_player + 1).min(h as u32);
        for i in 0..enemies as i32 {
            let z = (i * 2).min(h - 1);
            self.units.push(Unit {
                id: next_id,
                team: Team::Enemies,
                owner: None,
                x: w - 1,
                z,
                hp: self.config.enemy_unit.hp,
                stats: self.config.enemy_unit,
                moved: false,
                acted: false,
            });
            next_id += 1;
        }
        // Obstacles in the middle columns, never on a unit.
        self.obstacles.clear();
        for x in 2..w - 2 {
            for z in 0..h {
                if rng.below(100) < u64::from(self.config.obstacles_pct) && !self.blocked((x, z), 0) {
                    self.obstacles.insert((x, z));
                }
            }
        }
        self.round = 1;
        self.begin_players(now);
        Ok(())
    }

    fn begin_players(&mut self, now: u64) {
        self.phase = Phase::Players;
        self.ended_turn.clear();
        self.phase_ends = now + self.config.phase_ms;
        for u in &mut self.units {
            u.moved = false;
            u.acted = false;
        }
    }

    fn unit_of(&self, player: u64) -> Result<usize> {
        if self.phase != Phase::Players {
            return Err(kit::error(codes::NOT_YOUR_TURN, "it is not the players' phase"));
        }
        if self.ended_turn.contains(&player) {
            return Err(kit::error(codes::NOT_YOUR_TURN, "you ended your turn"));
        }
        self.units
            .iter()
            .position(|u| u.owner == Some(player) && u.alive())
            .ok_or_else(|| kit::error(codes::NOT_MEMBER, "you have no unit standing"))
    }

    pub fn move_to(&mut self, player: u64, to: (i32, i32)) -> Result<()> {
        let i = self.unit_of(player)?;
        let u = self.units[i].clone();
        if u.moved {
            return Err(kit::error(codes::CONFLICT, "the unit has moved this turn"));
        }
        let (w, h) = (self.config.width, self.config.height);
        let blocked = |x: i32, z: i32| self.blocked((x, z), u.id);
        let grid = GridProvider { width: w, height: h, blocked: &blocked };
        if !reachable(&grid, (u.x, u.z), u.stats.moves, EXPAND).iter().any(|c| (c.x, c.z) == to) {
            return Err(kit::error(codes::OUT_OF_REACH, "that cell is out of the unit's reach"));
        }
        let unit = &mut self.units[i];
        unit.x = to.0;
        unit.z = to.1;
        unit.moved = true;
        self.note(format!("unit {} moved to {},{}", u.id, to.0, to.1));
        Ok(())
    }

    pub fn attack(&mut self, player: u64, target: u32) -> Result<i64> {
        let i = self.unit_of(player)?;
        let me = self.units[i].clone();
        if me.acted {
            return Err(kit::error(codes::CONFLICT, "the unit has attacked this turn"));
        }
        let t = self
            .units
            .iter()
            .position(|u| u.id == target && u.alive())
            .ok_or_else(|| kit::error(codes::NOT_FOUND, "no such unit standing"))?;
        if self.units[t].team == me.team {
            return Err(kit::error(codes::FORBIDDEN, "that unit is on your side"));
        }
        if manhattan((me.x, me.z), (self.units[t].x, self.units[t].z)) > me.stats.range {
            return Err(kit::error(codes::OUT_OF_REACH, "out of reach"));
        }
        let dealt = damage(&me.stats, &self.units[t].stats);
        let left = {
            let target = &mut self.units[t];
            target.hp = (target.hp - dealt).max(0);
            target.hp
        };
        self.units[i].acted = true;
        self.note(format!("unit {} hit unit {target} for {dealt}", me.id));
        self.judge();
        Ok(left)
    }

    pub fn end_turn(&mut self, player: u64, now: u64) -> Result<()> {
        if self.phase != Phase::Players || !self.players.contains(&player) {
            return Err(kit::error(codes::NOT_YOUR_TURN, "it is not the players' phase"));
        }
        self.ended_turn.insert(player);
        let standing: Vec<u64> = self.units.iter().filter(|u| u.alive()).filter_map(|u| u.owner).collect();
        if standing.iter().all(|p| self.ended_turn.contains(p)) {
            self.enemy_phase(now);
        }
        Ok(())
    }

    /// The hub's units: each moves toward the nearest player's unit and strikes it if it can.
    pub fn enemy_phase(&mut self, now: u64) {
        if self.phase != Phase::Players {
            return;
        }
        self.phase = Phase::Enemies;
        let ids: Vec<u32> = self.units.iter().filter(|u| u.team == Team::Enemies && u.alive()).map(|u| u.id).collect();
        for id in ids {
            let targets: Vec<(i32, i32)> = self.units.iter().filter(|u| u.team == Team::Players && u.alive()).map(|u| (u.x, u.z)).collect();
            if targets.is_empty() {
                break;
            }
            let me = self.units.iter().find(|u| u.id == id).cloned().expect("listed above");
            let near = targets.iter().any(|t| manhattan(*t, (me.x, me.z)) <= me.stats.range);
            if !near {
                let (w, h) = (self.config.width, self.config.height);
                let blocked = |x: i32, z: i32| self.blocked((x, z), id);
                let grid = GridProvider { width: w, height: h, blocked: &blocked };
                if let Some(m) = best_move(&grid, (me.x, me.z), me.stats.moves, EXPAND, approach_scorer(targets.clone())) {
                    let u = self.units.iter_mut().find(|u| u.id == id).expect("listed above");
                    u.x = m.to.0;
                    u.z = m.to.1;
                }
            }
            let me = self.units.iter().find(|u| u.id == id).cloned().expect("listed above");
            let victim = self
                .units
                .iter()
                .filter(|u| u.team == Team::Players && u.alive() && manhattan((u.x, u.z), (me.x, me.z)) <= me.stats.range)
                .min_by_key(|u| (u.hp, u.id))
                .map(|u| u.id);
            if let Some(v) = victim {
                let target = self.units.iter_mut().find(|u| u.id == v).expect("found above");
                let dealt = damage(&me.stats, &target.stats);
                target.hp = (target.hp - dealt).max(0);
                self.note(format!("unit {id} hit unit {v} for {dealt}"));
            }
        }
        self.judge();
        if self.phase == Phase::Enemies {
            self.round += 1;
            self.begin_players(now);
        }
    }

    fn judge(&mut self) {
        let alive = |t: Team| self.units.iter().any(|u| u.team == t && u.alive());
        if !alive(Team::Enemies) {
            self.phase = Phase::Won;
        } else if !alive(Team::Players) {
            self.phase = Phase::Lost;
        }
    }
}

/// The hub: the battle, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct BattleHub {
    #[serde(flatten)]
    pub battle: Battle,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl BattleHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let player = call.player()?;
        let b = &mut self.battle;
        let out = match call.method {
            "join" => {
                b.join(player)?;
                encode(&true)
            }
            "start" => {
                b.start(player, Rng::seed_from(&ctx.key) ^ ctx.epoch, now)?;
                encode(&true)
            }
            "move" => {
                let m: Move = kit::args(call)?;
                b.move_to(player, (m.x, m.z))?;
                encode(&true)
            }
            "attack" => {
                let a: Attack = kit::args(call)?;
                encode(&b.attack(player, a.target)?)
            }
            "end_turn" => {
                b.end_turn(player, now)?;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.arm(ctx)?;
        self.kit.snapshot(ctx, TOPIC, &self.battle.view())?;
        ctx.persist_now();
        out
    }

    fn arm(&self, ctx: &Ctx) -> Result<()> {
        if self.battle.phase == Phase::Players {
            ctx.timer_after("phase", self.battle.phase_ends.saturating_sub(ctx.now_ms()).max(1))
        } else {
            ctx.cancel_timer("phase");
            Ok(())
        }
    }
}

impl Hub for BattleHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut hub = Self::default();
        if !seed.is_empty() {
            hub.battle.config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
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
            "state" => encode(&self.battle.view()),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.battle.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "phase" || self.battle.phase != Phase::Players {
            return Ok(());
        }
        self.battle.enemy_phase(ctx.now_ms());
        self.arm(ctx)?;
        self.kit.snapshot(ctx, TOPIC, &self.battle.view())
    }
}

ckx_sdk::export_hub!(BattleHub);

/// Cells a unit can reach, for a client that highlights them (the hub checks again).
pub fn reach_of(battle: &Battle, unit: u32) -> Vec<(i32, i32)> {
    let Some(u) = battle.units.iter().find(|u| u.id == unit) else { return Vec::new() };
    let blocked = |x: i32, z: i32| battle.blocked((x, z), unit);
    let grid = GridProvider { width: battle.config.width, height: battle.config.height, blocked: &blocked };
    reachable(&grid, (u.x, u.z), u.stats.moves, EXPAND).into_iter().map(|c| (c.x, c.z)).collect::<BTreeSet<_>>().into_iter().collect()
}

/// Units by team, for a quick read.
pub fn standing(battle: &Battle) -> BTreeMap<String, usize> {
    let mut out = BTreeMap::new();
    for u in battle.units.iter().filter(|u| u.alive()) {
        *out.entry(format!("{:?}", u.team).to_lowercase()).or_insert(0) += 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, error_code};
    use ckx_sdk::testing::TestHub;

    fn battle() -> Battle {
        let mut b = Battle::default();
        b.join(1).unwrap();
        b.start(1, 3, 0).unwrap();
        b
    }

    #[test]
    fn a_unit_moves_within_its_reach_and_attacks_once_in_range() {
        let mut b = battle();
        let Error(why) = b.move_to(1, (7, 7)).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::OUT_OF_REACH));
        let reach = reach_of(&b, 1);
        let step = *reach.iter().find(|c| **c != (0, 1)).unwrap();
        b.move_to(1, step).unwrap();
        assert!(b.move_to(1, (0, 1)).is_err(), "one move a turn");
        let enemy = b.units.iter().find(|u| u.team == Team::Enemies).unwrap().id;
        let Error(why) = b.attack(1, enemy).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::OUT_OF_REACH));
        assert_eq!(standing(&b)["enemies"], 2);
    }

    #[test]
    fn the_enemies_close_in_and_strike_and_one_side_falls() {
        let mut b = battle();
        let mut now = 0;
        for _ in 0..40 {
            if !matches!(b.phase, Phase::Players) {
                break;
            }
            // The player's unit holds; it strikes whatever comes into reach.
            let me = b.units.iter().find(|u| u.owner == Some(1)).cloned().unwrap();
            if let Some(t) = b.units.iter().find(|u| u.team == Team::Enemies && u.alive() && manhattan((u.x, u.z), (me.x, me.z)) <= 1) {
                let id = t.id;
                b.attack(1, id).unwrap();
            }
            now += 1_000;
            b.end_turn(1, now).unwrap();
        }
        assert!(matches!(b.phase, Phase::Won | Phase::Lost), "{:?}", b.phase);
        assert!(b.log.iter().any(|l| l.contains("hit")));
    }

    #[test]
    fn the_phase_times_out_into_the_enemies_turn() {
        let mut hub = TestHub::<BattleHub>::spawn("tactics", "b1").unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        let before: BattleView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        hub.advance(Config::default().phase_ms).unwrap();
        let msg: Message<BattleView> = decode(&hub.published().last().unwrap().1).unwrap();
        assert_eq!((before.round, msg.body.round), (1, 2));
        assert_ne!(before.units, msg.body.units, "the enemies moved");
    }
}
