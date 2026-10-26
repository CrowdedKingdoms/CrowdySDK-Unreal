//! A ball game for two teams, keyed by its match: a ball the hub moves (passes, shots, friction,
//! the side walls), possession (claim a loose ball near it, or take it from an opponent whose
//! protection is over), a goal in each end wall, first to `score_to_win` or the better score when
//! the time is up. Clients report where their player is (`move { x, y }`, a few times a second)
//! and the movement warden keeps what the rules allow: the hub referees the positions it kept,
//! not the ones a client claimed. Replace the [`Config`] with your game's.
//!
//! This is the Unreal SDK's z-up copy of the platform's starter. What differs: every position,
//! on the wire and in the view, is in world metres on the ground, `x` then `y` (the Unreal
//! location over 100), and the pitch is the rectangle from `origin` of `width` along X and
//! `depth` along Y, with a goal in each end wall (x = origin.x and x = origin.x + width) centred
//! on the pitch's Y. With `realtime` on, the ball is also shown every tick as a kit actor of kind
//! `ball` (16 m chunks, z up, `height` metres up, `owner` the holder) to players within as many
//! chunks as the pitch's longer side needs, plus one (at most 8), and its id is `B<key>@1`, so a
//! client reads the match's key from it. Possession changes, including a holder who leaves
//! dropping the ball, go out as realtime event 100 at the pitch's centre chunk. A key over 29
//! bytes does not fit that id, and one with an `@` or a byte outside `!` to `~` cannot be read
//! back from it, so `spawn` refuses them with `realtime` on. And these holes are closed. A `move`
//! or `shoot` whose position is not a finite number is refused `bad_request` and changes
//! nothing. The platform's warden gives every report 0.5 m of slack, so a client reporting faster
//! moved faster; this crate's own warden gives each player a travel budget that grows with the
//! time since the last report at `max_speed` times `grace` (so the hub allows up to `grace` times
//! `max_speed`) and holds at most 1 s of it plus 0.5 m, so the rate buys no speed and a slow
//! round trip is not cut. A cut of 1 m or less is jitter and a bounds clamp (the pitch) is not a
//! violation: neither is a strike. A player the hub has just placed (join, start) is answered
//! with the place, with no strike, until a report comes from within reach of it. `join` also
//! works while the match is on (the platform refused it, so a player who dropped could never come
//! back) and places a new player at their team's half; a player already in may not change team
//! then (`started`). `leave` is new: the player is removed and a ball they held drops loose
//! where they were, with no speed and no hold time. A player who left a match that is on is
//! remembered (team and last kept position, until the next `start`, at most 8 times `team_max`):
//! they rejoin only their own team, placed where they were, so leaving is no teleport and no
//! change of team. A player who is playing and sends no `move` for `idle_ms` is removed by the
//! tick as by `leave`, with `removed` (`why`: `idle`), where the platform kept a silent player,
//! and the ball they held, forever; 0 never does. `start` puts every player at their team's
//! half. The view shows `ends_at` only while the match is on.
//!
//! Endpoints (players): `join { team }` (`red` or `blue`), `leave`, `start` (the first to join,
//! or a developer), `move { x, y }` (answers the position kept, as `{ x, y }`), `claim`, `pass
//! { to }`, `shoot { x, y }` (aimed at a point), `state` (anyone). It speaks the kit protocol
//! (`ckx_sdk::kit`): the `ball` topic carries the match as `snapshot` messages up to five times a
//! second while it changes, and `event` messages (`type`: `claimed`, `passed`, `shot`, `goal`,
//! `violation`, `left`, `removed`, `finished`) as they happen; `snapshot { topic: "ball" }`
//! answers it; changes may carry an `action_id`; refusals carry `kit:<code>:` codes.

use ckx_sdk::emit::MAX_DISTANCE;
use ckx_sdk::kit::play::combat::Cooldowns;
use ckx_sdk::kit::play::movement::{Rules, STRIKE_WINDOW_MS, Verdict, Violation, ViolationKind};
use ckx_sdk::kit::wire::{self, ActorState};
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;
use serde_json::{Value, json};

pub const TOPIC: &str = "ball";
pub const TICK_MS: u64 = 100;
/// Ticks between snapshots while nothing else publishes.
const PUBLISH_EVERY: u64 = 2;
/// Metres along one side of a chunk.
const CHUNK_M: f64 = 16.0;
/// The longest key the ball's actor id, `B<key>@1`, fits in 32 bytes.
const KEY_MAX: usize = 29;
/// The most travel a player banks: this many seconds at full speed, and this many metres more.
const BANK_S: f64 = 1.0;
const BANK_M: f64 = 0.5;
/// A speed cut of at most this many metres is jitter and costs no strike.
const JITTER_M: f64 = 1.0;

pub const TEAMS: [&str; 2] = ["red", "blue"];

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    /// The pitch's corner in the world, `[x, y]` in metres.
    pub origin: (f64, f64),
    /// Metres along X from the origin.
    pub width: f64,
    /// Metres along Y from the origin.
    pub depth: f64,
    /// The goal's width in each end wall, centred.
    pub goal_width: f64,
    pub team_max: usize,
    pub claim_radius: f64,
    /// How long a new holder cannot be robbed.
    pub protect_ms: u64,
    pub pass_speed: f64,
    pub shoot_speed: f64,
    /// The share of its speed a loose ball loses each second.
    pub friction: f64,
    pub score_to_win: u32,
    pub duration_ms: u64,
    /// Metres a second.
    pub max_speed: f64,
    /// The hub allows up to this times `max_speed`, for jitter in the reports' timing.
    pub grace: f64,
    /// A reported move this far at once is refused.
    pub teleport: f64,
    /// A playing player who sends no `move` for this long is removed; 0 never does.
    pub idle_ms: u64,
    /// Whether the ball is shown as a realtime actor and possession goes out as realtime events.
    pub realtime: bool,
    /// The pitch's height in metres, for the chunks' z.
    pub height: f64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            origin: (0.0, 0.0),
            width: 60.0,
            depth: 40.0,
            goal_width: 12.0,
            team_max: 5,
            claim_radius: 1.5,
            protect_ms: 1_500,
            pass_speed: 18.0,
            shoot_speed: 28.0,
            friction: 0.8,
            score_to_win: 3,
            duration_ms: 5 * 60_000,
            max_speed: 9.0,
            grace: 1.25,
            teleport: 15.0,
            idle_ms: 15_000,
            realtime: false,
            height: 0.0,
        }
    }
}

fn chunk(metres: f64) -> i64 {
    (metres / CHUNK_M).floor() as i64
}

impl Config {
    /// The chunk at the pitch's centre, z up.
    pub fn centre_chunk(&self) -> ChunkPos {
        ChunkPos::new(chunk(self.origin.0 + self.width / 2.0), chunk(self.origin.1 + self.depth / 2.0), chunk(self.height))
    }

    /// How many chunks around the centre see the ball: the pitch's longer side, plus one.
    pub fn reach(&self) -> u8 {
        ((self.width.max(self.depth) / CHUNK_M).ceil() + 1.0).clamp(1.0, f64::from(MAX_DISTANCE)) as u8
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Lobby,
    Playing,
    Finished,
}

/// One player's movement as the hub last accepted it. The movement warden of
/// `ckx_sdk::kit::play::movement`, with a capped travel budget that grows with time instead of
/// a slack per report, no strike for a bounds clamp, a small cut or a placement.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq)]
pub struct Warden {
    /// The position kept, and when.
    pub last: Option<(f64, f64, u64)>,
    pub strikes: u32,
    pub last_strike_ms: u64,
    /// The hub put the player here and no report within reach of it has come yet: a report far
    /// from it is answered with it, without a strike.
    #[serde(default)]
    pub placed: bool,
    /// Metres the player may still travel, as of the time in `last`.
    #[serde(default)]
    pub budget: f64,
}

fn clamp_to(bounds: Option<(f64, f64, f64, f64)>, p: (f64, f64)) -> (f64, f64) {
    let Some((x0, y0, x1, y1)) = bounds else { return p };
    (p.0.clamp(x0.min(x1), x0.max(x1)), p.1.clamp(y0.min(y1), y0.max(y1)))
}

impl Warden {
    /// A warden that starts from `at`: a place the hub put the player.
    pub fn at(at: (f64, f64), now: u64) -> Self {
        Self { last: Some((at.0, at.1, now)), placed: true, ..Self::default() }
    }

    /// Moves the player where the hub says, with no check.
    pub fn place(&mut self, at: (f64, f64), now: u64) {
        self.last = Some((at.0, at.1, now));
        self.placed = true;
        self.budget = 0.0;
    }

    /// The position kept.
    pub fn position(&self) -> Option<(f64, f64)> {
        self.last.map(|(x, y, _)| (x, y))
    }

    /// Strikes in the current window.
    pub fn strikes(&self, now: u64) -> u32 {
        if now.saturating_sub(self.last_strike_ms) >= STRIKE_WINDOW_MS { 0 } else { self.strikes }
    }

    /// Checks a reported position, which is finite, and keeps the one the rules allow.
    pub fn check(&mut self, rules: &Rules, claimed: (f64, f64), now: u64) -> Verdict {
        let at = clamp_to(rules.bounds, claimed);
        let Some((x, y, t)) = self.last else {
            self.last = Some((at.0, at.1, now));
            return Verdict { at, violation: None };
        };
        let (dx, dy) = (at.0 - x, at.1 - y);
        let dist = (dx * dx + dy * dy).sqrt();
        let dt = now.saturating_sub(t) as f64 / 1000.0;
        let rate = rules.max_speed * rules.grace.max(1.0);
        let budget = (self.budget + rate * dt).min(rate * BANK_S + BANK_M);
        if self.placed {
            if dist > budget {
                return Verdict { at: (x, y), violation: None };
            }
            self.placed = false;
        }
        if dist >= rules.teleport {
            return self.strike(now, (x, y), ViolationKind::Teleport, format!("{dist:.1} metres at once"));
        }
        if dist <= budget {
            self.budget = budget - dist;
            self.last = Some((at.0, at.1, now));
            return Verdict { at, violation: None };
        }
        let k = budget / dist;
        let kept = (x + dx * k, y + dy * k);
        self.budget = 0.0;
        self.last = Some((kept.0, kept.1, now));
        if dist - budget <= JITTER_M {
            return Verdict { at: kept, violation: None };
        }
        self.strike(now, kept, ViolationKind::Speed, format!("{:.1} metres a second", dist / dt.max(0.001)))
    }

    fn strike(&mut self, now: u64, at: (f64, f64), kind: ViolationKind, detail: String) -> Verdict {
        self.strikes = self.strikes(now) + 1;
        self.last_strike_ms = now;
        Verdict { at, violation: Some(Violation { kind, detail }) }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Footballer {
    pub player: u64,
    pub team: usize,
    pub warden: Warden,
    /// When the player last sent a `move`, or last came into play.
    #[serde(default)]
    pub moved_at: u64,
}

/// A player who left a match that was on: the team and the last position kept.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Leaver {
    pub player: u64,
    pub team: usize,
    pub at: (f64, f64),
}

/// The ball, in world metres.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct BallState {
    pub x: f64,
    pub y: f64,
    pub vx: f64,
    pub vy: f64,
    pub holder: Option<u64>,
    pub held_since: u64,
    pub last_touch: Option<u64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct PlayerView {
    pub player: u64,
    pub team: String,
    pub x: f64,
    pub y: f64,
}

/// The match as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct BallView {
    pub phase: Phase,
    /// The pitch's corner, ground `[x, y]` in world metres.
    pub origin: (f64, f64),
    pub width: f64,
    pub depth: f64,
    pub goal_width: f64,
    pub ball: BallState,
    pub players: Vec<PlayerView>,
    /// Red's, then blue's.
    pub score: [u32; 2],
    pub ends_at: Option<u64>,
    pub winner: Option<String>,
}

#[derive(Deserialize)]
struct Join {
    team: String,
}

#[derive(Deserialize)]
struct Pass {
    to: u64,
}

#[derive(Deserialize)]
struct Point {
    x: f64,
    y: f64,
}

/// What `move` answers: the position kept, in world metres.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
pub struct Kept {
    pub x: f64,
    pub y: f64,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Match {
    pub config: Config,
    pub phase: Phase,
    pub players: Vec<Footballer>,
    pub ball: BallState,
    pub score: [u32; 2],
    pub ends_at: Option<u64>,
    pub winner: Option<usize>,
    pub cooldowns: Cooldowns,
    pub ticks: u64,
    /// Who left the running match, for the rest of it.
    #[serde(default)]
    pub leavers: Vec<Leaver>,
    /// Events since they were last published.
    #[serde(skip)]
    pub events: Vec<Value>,
    /// Balls a leaving holder dropped since the last publish, for the realtime possession event.
    #[serde(skip)]
    pub drops: u32,
    /// Whether the view changed since it was last published.
    #[serde(skip)]
    pub dirty: bool,
}

fn dist(a: (f64, f64), b: (f64, f64)) -> f64 {
    ((a.0 - b.0).powi(2) + (a.1 - b.1).powi(2)).sqrt()
}

/// Whether both numbers are finite, also as the `f32`s the client works in.
fn finite(p: (f64, f64)) -> bool {
    (p.0 as f32).is_finite() && (p.1 as f32).is_finite()
}

impl Match {
    /// A match in its lobby, the ball at the centre.
    pub fn new(config: Config) -> Self {
        let mut game = Self { config, ..Self::default() };
        game.kickoff();
        game
    }

    pub fn view(&self) -> BallView {
        BallView {
            phase: self.phase,
            origin: self.config.origin,
            width: self.config.width,
            depth: self.config.depth,
            goal_width: self.config.goal_width,
            ball: self.ball.clone(),
            players: self
                .players
                .iter()
                .map(|p| {
                    let (x, y) = p.warden.position().unwrap_or_else(|| self.half(p.team));
                    PlayerView { player: p.player, team: TEAMS[p.team].into(), x, y }
                })
                .collect(),
            score: self.score,
            ends_at: self.ends_at.filter(|_| self.phase == Phase::Playing),
            winner: self.winner.map(|t| TEAMS[t].to_string()),
        }
    }

    fn event(&mut self, kind: &str, extra: Value) {
        let mut e = json!({ "type": kind });
        if let (Some(e), Some(x)) = (e.as_object_mut(), extra.as_object()) {
            e.extend(x.clone());
        }
        self.events.push(e);
        self.dirty = true;
    }

    /// `(min_x, min_y, max_x, max_y)` in world metres.
    fn bounds(&self) -> (f64, f64, f64, f64) {
        let (x, y) = self.config.origin;
        (x, y, x + self.config.width, y + self.config.depth)
    }

    fn rules(&self) -> Rules {
        Rules { max_speed: self.config.max_speed, teleport: self.config.teleport, bounds: Some(self.bounds()), grace: self.config.grace }
    }

    /// Where a team's players are put: its half's middle, red on the left.
    fn half(&self, team: usize) -> (f64, f64) {
        let (x, y) = self.config.origin;
        let share = if team == 0 { 0.25 } else { 0.75 };
        (x + self.config.width * share, y + self.config.depth / 2.0)
    }

    fn at(&self, player: u64) -> Result<(usize, (f64, f64))> {
        let i =
            self.players.iter().position(|p| p.player == player).ok_or_else(|| kit::error(codes::NOT_MEMBER, "join the match first"))?;
        Ok((i, self.players[i].warden.position().unwrap_or_else(|| self.half(self.players[i].team))))
    }

    fn playing(&self) -> Result<()> {
        match self.phase {
            Phase::Playing => Ok(()),
            Phase::Lobby => Err(kit::error(codes::NOT_READY, "the match has not started")),
            Phase::Finished => Err(kit::error(codes::FINISHED, "the match is over")),
        }
    }

    fn kickoff(&mut self) {
        let (x0, y0, x1, y1) = self.bounds();
        self.ball = BallState { x: (x0 + x1) / 2.0, y: (y0 + y1) / 2.0, ..BallState::default() };
        self.dirty = true;
    }

    pub fn join(&mut self, player: u64, team: &str, now: u64) -> Result<()> {
        let t = TEAMS.iter().position(|x| *x == team).ok_or_else(|| kit::error(codes::BAD_REQUEST, "a team is red or blue"))?;
        let member = self.players.iter().position(|p| p.player == player);
        if member.is_some_and(|i| self.players[i].team == t) {
            return Ok(());
        }
        if member.is_some() && self.phase == Phase::Playing {
            return Err(kit::error(codes::STARTED, "teams are picked before the match"));
        }
        let left = self.leavers.iter().position(|l| l.player == player).filter(|_| self.phase == Phase::Playing);
        if left.is_some_and(|l| self.leavers[l].team != t) {
            return Err(kit::error(codes::STARTED, "back on the team you left"));
        }
        if self.players.iter().filter(|p| p.team == t).count() >= self.config.team_max {
            return Err(kit::error(codes::FULL, format!("{team} is full")));
        }
        let at = left.map_or_else(|| self.half(t), |l| self.leavers[l].at);
        match member {
            Some(i) => {
                self.players[i].team = t;
                self.players[i].warden.place(at, now);
            }
            None => self.players.push(Footballer { player, team: t, warden: Warden::at(at, now), moved_at: now }),
        }
        if let Some(l) = left {
            self.leavers.remove(l);
        }
        self.dirty = true;
        Ok(())
    }

    /// Takes the player out of the match; a ball they held drops loose where they were. They are
    /// remembered for a rejoin while the match is on. Returns their team.
    fn remove(&mut self, i: usize) -> usize {
        let gone = self.players.remove(i);
        let at = gone.warden.position().unwrap_or_else(|| self.half(gone.team));
        if self.ball.holder == Some(gone.player) {
            self.ball = BallState { x: at.0, y: at.1, last_touch: self.ball.last_touch, ..BallState::default() };
            self.drops += 1;
        }
        self.leavers.retain(|l| l.player != gone.player);
        self.leavers.push(Leaver { player: gone.player, team: gone.team, at });
        if self.leavers.len() > 8 * self.config.team_max {
            self.leavers.remove(0);
        }
        self.dirty = true;
        gone.team
    }

    pub fn leave(&mut self, player: u64) -> Result<()> {
        let (i, _) = self.at(player)?;
        let team = self.remove(i);
        self.event("left", json!({ "player": player, "team": TEAMS[team] }));
        Ok(())
    }

    pub fn start(&mut self, now: u64) -> Result<()> {
        if self.phase == Phase::Playing {
            return Err(kit::error(codes::STARTED, "the match is under way"));
        }
        if !(0..2).all(|t| self.players.iter().any(|p| p.team == t)) {
            return Err(kit::error(codes::NOT_READY, "each team needs a player"));
        }
        self.phase = Phase::Playing;
        self.leavers.clear();
        self.score = [0, 0];
        self.winner = None;
        self.ends_at = Some(now + self.config.duration_ms);
        for i in 0..self.players.len() {
            let at = self.half(self.players[i].team);
            self.players[i].warden.place(at, now);
            self.players[i].moved_at = now;
        }
        self.kickoff();
        Ok(())
    }

    /// A reported position in world metres: the warden keeps what the rules allow. Returns the
    /// position kept.
    pub fn moved(&mut self, player: u64, to: (f64, f64), now: u64) -> Result<(f64, f64)> {
        if !finite(to) {
            return Err(kit::error(codes::BAD_REQUEST, "a position is a number of metres"));
        }
        let (i, _) = self.at(player)?;
        let rules = self.rules();
        self.players[i].moved_at = now;
        let verdict = self.players[i].warden.check(&rules, to, now);
        if let Some(v) = &verdict.violation {
            self.event("violation", json!({ "player": player, "what": v.kind.as_str(), "detail": v.detail }));
        }
        if self.ball.holder == Some(player) {
            (self.ball.x, self.ball.y) = verdict.at;
        }
        self.dirty = true;
        Ok(verdict.at)
    }

    pub fn claim(&mut self, player: u64, now: u64) -> Result<()> {
        self.playing()?;
        let (i, at) = self.at(player)?;
        if dist(at, (self.ball.x, self.ball.y)) > self.config.claim_radius {
            return Err(kit::error(codes::OUT_OF_REACH, "the ball is too far"));
        }
        if !self.cooldowns.take(&player.to_string(), "claim", 500, now) {
            return Err(kit::error(codes::COOLDOWN, "claiming too fast"));
        }
        let from = self.ball.holder;
        if let Some(h) = from {
            if h == player {
                return Ok(());
            }
            let holder_team = self.players.iter().find(|p| p.player == h).map(|p| p.team);
            if holder_team == Some(self.players[i].team) {
                return Err(kit::error(codes::CONFLICT, "a teammate has it"));
            }
            if now.saturating_sub(self.ball.held_since) < self.config.protect_ms {
                return Err(kit::error(codes::COOLDOWN, "they only just got it"));
            }
        }
        self.ball = BallState { x: at.0, y: at.1, holder: Some(player), held_since: now, last_touch: Some(player), ..BallState::default() };
        self.event("claimed", json!({ "player": player, "team": TEAMS[self.players[i].team], "from": from }));
        Ok(())
    }

    fn release(&mut self, player: u64, toward: (f64, f64), speed: f64) -> Result<()> {
        self.playing()?;
        if self.ball.holder != Some(player) {
            return Err(kit::error(codes::CONFLICT, "you do not have the ball"));
        }
        let (dx, dy) = (toward.0 - self.ball.x, toward.1 - self.ball.y);
        let d = (dx * dx + dy * dy).sqrt();
        let (ux, uy) = if d > 1e-6 { (dx / d, dy / d) } else { (1.0, 0.0) };
        self.ball.vx = ux * speed;
        self.ball.vy = uy * speed;
        self.ball.holder = None;
        self.ball.held_since = 0;
        self.ball.last_touch = Some(player);
        self.dirty = true;
        Ok(())
    }

    pub fn pass(&mut self, player: u64, to: u64) -> Result<()> {
        let (i, _) = self.at(player)?;
        let (j, target) = self.at(to)?;
        if i == j || self.players[i].team != self.players[j].team {
            return Err(kit::error(codes::BAD_REQUEST, "pass to a teammate"));
        }
        self.release(player, target, self.config.pass_speed)?;
        self.event("passed", json!({ "player": player, "to": to, "team": TEAMS[self.players[i].team] }));
        Ok(())
    }

    pub fn shoot(&mut self, player: u64, aim: (f64, f64)) -> Result<()> {
        if !finite(aim) {
            return Err(kit::error(codes::BAD_REQUEST, "an aim is a position in metres"));
        }
        let (i, _) = self.at(player)?;
        self.release(player, aim, self.config.shoot_speed)?;
        self.event("shot", json!({ "player": player, "team": TEAMS[self.players[i].team] }));
        Ok(())
    }

    /// One tick: idle players go, the ball moves, the walls turn it, a goal counts, the clock
    /// runs. Returns whether the match ran.
    pub fn tick(&mut self, now: u64) -> bool {
        if self.phase != Phase::Playing {
            return false;
        }
        self.ticks += 1;
        self.remove_the_idle(now);
        self.roll();
        let decided = self.score.iter().position(|s| *s >= self.config.score_to_win);
        if decided.is_some() || self.ends_at.is_some_and(|t| now >= t) {
            self.phase = Phase::Finished;
            self.winner = decided.or(match self.score[0].cmp(&self.score[1]) {
                std::cmp::Ordering::Greater => Some(0),
                std::cmp::Ordering::Less => Some(1),
                std::cmp::Ordering::Equal => None,
            });
            self.event("finished", json!({ "winner": self.winner.map(|t| TEAMS[t]), "score": self.score }));
        }
        true
    }

    /// Removes the players who have sent no `move` for `idle_ms`.
    fn remove_the_idle(&mut self, now: u64) {
        let idle_ms = self.config.idle_ms;
        if idle_ms == 0 {
            return;
        }
        for i in (0..self.players.len()).rev() {
            if now.saturating_sub(self.players[i].moved_at) < idle_ms {
                continue;
            }
            let player = self.players[i].player;
            let team = self.remove(i);
            self.event("removed", json!({ "player": player, "team": TEAMS[team], "why": "idle" }));
        }
    }

    /// A loose ball rolls on, slows, bounces off the side walls, and scores or bounces at an end.
    fn roll(&mut self) {
        if self.ball.holder.is_some() || (self.ball.vx == 0.0 && self.ball.vy == 0.0) {
            return;
        }
        let dt = TICK_MS as f64 / 1000.0;
        let (x0, y0, x1, y1) = self.bounds();
        let (centre, half) = ((y0 + y1) / 2.0, self.config.goal_width / 2.0);
        let b = &mut self.ball;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        let keep = (1.0 - self.config.friction * dt).max(0.0);
        b.vx *= keep;
        b.vy *= keep;
        if b.vx.hypot(b.vy) < 0.2 {
            (b.vx, b.vy) = (0.0, 0.0);
        }
        if b.y < y0 || b.y > y1 {
            b.y = b.y.clamp(y0, y1);
            b.vy = -b.vy;
        }
        let in_mouth = (b.y - centre).abs() <= half;
        let scored = match (b.x < x0, b.x > x1) {
            (true, _) if in_mouth => Some(1),
            (_, true) if in_mouth => Some(0),
            (true, _) | (_, true) => {
                b.x = b.x.clamp(x0, x1);
                b.vx = -b.vx;
                None
            }
            _ => None,
        };
        self.dirty = true;
        let Some(team) = scored else { return };
        self.score[team] += 1;
        let scorer = self.ball.last_touch;
        self.event("goal", json!({ "team": TEAMS[team], "player": scorer, "score": self.score }));
        self.kickoff();
    }
}

fn ball_actor(key: &str) -> [u8; 32] {
    actor_id(&format!("B{key}@1"))
}

/// The hub: the match, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct BallHub {
    #[serde(flatten)]
    pub game: Match,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl BallHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let g = &mut self.game;
        let out = match call.method {
            "join" => {
                let j: Join = kit::args(call)?;
                g.join(call.player()?, &j.team, now)?;
                encode(&true)
            }
            "leave" => {
                g.leave(call.player()?)?;
                encode(&true)
            }
            "start" => {
                let allowed = match call.caller {
                    Caller::Developer(_) => true,
                    Caller::Player(p) => g.players.first().map(|f| f.player) == Some(p),
                    _ => false,
                };
                if !allowed {
                    return Err(kit::error(codes::FORBIDDEN, "the first to join, or a developer, starts it"));
                }
                g.start(now)?;
                ctx.timer_every("tick", TICK_MS)?;
                encode(&true)
            }
            "move" => {
                let m: Point = kit::args(call)?;
                let (x, y) = g.moved(call.player()?, (m.x, m.y), now)?;
                self.flush(ctx, false)?;
                return encode(&Kept { x, y });
            }
            "claim" => {
                g.claim(call.player()?, now)?;
                encode(&true)
            }
            "pass" => {
                let p: Pass = kit::args(call)?;
                g.pass(call.player()?, p.to)?;
                encode(&true)
            }
            "shoot" => {
                let a: Point = kit::args(call)?;
                g.shoot(call.player()?, (a.x, a.y))?;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.flush(ctx, true)?;
        out
    }

    /// Publishes the events since the last time, and the match when `snapshot` says so.
    fn flush(&mut self, ctx: &Ctx, snapshot: bool) -> Result<()> {
        for e in std::mem::take(&mut self.game.events) {
            self.kit.event(ctx, TOPIC, &e)?;
            self.possession(ctx, &e);
        }
        for _ in 0..std::mem::take(&mut self.game.drops) {
            self.send_possession(ctx, wire::PossessionChange { kind: "loose".into(), holder_id: None, team: None });
        }
        if snapshot && std::mem::take(&mut self.game.dirty) {
            self.kit.snapshot(ctx, TOPIC, &self.game.view())?;
        }
        Ok(())
    }

    /// A possession change as a realtime event, when the match is realtime.
    fn possession(&self, ctx: &Ctx, e: &Value) {
        let kind = match e["type"].as_str() {
            Some("claimed") => "claimed",
            Some("passed") => "passed",
            Some("shot") => "loose",
            Some("goal") => "goal",
            _ => return,
        };
        let body = wire::PossessionChange {
            kind: kind.into(),
            holder_id: if kind == "claimed" { e["player"].as_u64().map(|p| p.to_string()) } else { None },
            team: e["team"].as_str().map(String::from),
        };
        self.send_possession(ctx, body);
    }

    fn send_possession(&self, ctx: &Ctx, body: wire::PossessionChange) {
        let config = &self.game.config;
        if !config.realtime {
            return;
        }
        if let Err(Error(why)) = wire::emit_event(ctx, config.centre_chunk(), config.reach(), &ball_actor(&ctx.key), wire::POSSESSION, &body) {
            ctx.log(Level::Warn, &format!("a possession event did not go out: {why}"));
        }
    }

    /// The ball as a server-driven actor near the pitch, in chunks.
    fn show_ball(&self, ctx: &Ctx) {
        if !self.game.config.realtime {
            return;
        }
        let b = &self.game.ball;
        let mut state = ActorState::new("ball", 1, b.x / CHUNK_M, self.game.config.height / CHUNK_M);
        state.y = b.y / CHUNK_M;
        state.owner = b.holder;
        if let Err(Error(why)) = wire::emit_actor_state(ctx, &ball_actor(&ctx.key), self.game.config.reach(), &state) {
            ctx.log(Level::Warn, &format!("the ball was not shown: {why}"));
        }
    }
}

impl Hub for BallHub {
    fn spawn(ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut config = Config::default();
        if !seed.is_empty() {
            config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
        }
        if config.realtime && ctx.key.len() > KEY_MAX {
            return Err(Error::new(format!("a realtime match key is at most {KEY_MAX} bytes: the ball's actor id is B<key>@1")));
        }
        if config.realtime && !ctx.key.bytes().all(|b| b != b'@' && (0x21..=0x7E).contains(&b)) {
            return Err(Error::new("a realtime match key has no `@` and only the characters `!` to `~`: a client reads it back from the ball's actor id"));
        }
        Ok(Self { game: Match::new(config), ..Self::default() })
    }

    fn load(ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        let mut hub: Self = decode(snapshot)?;
        let now = ctx.now_ms();
        for p in &mut hub.game.players {
            p.moved_at = now;
        }
        Ok(hub)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        match call.method {
            "state" => encode(&self.game.view()),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.game.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "tick" {
            return Ok(());
        }
        if !self.game.tick(ctx.now_ms()) {
            ctx.cancel_timer("tick");
            return Ok(());
        }
        let ended = self.game.phase != Phase::Playing;
        if ended {
            ctx.cancel_timer("tick");
            ctx.persist_now();
        } else {
            self.show_ball(ctx);
        }
        let due = ended || !self.game.events.is_empty() || self.game.ticks.is_multiple_of(PUBLISH_EVERY);
        self.flush(ctx, due)
    }
}

ckx_sdk::export_hub!(BallHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::error_code;
    use ckx_sdk::testing::TestHub;

    const ORIGIN: (f64, f64) = (100.0, -50.0);
    /// The pitch's centre, the middle of red's half and of blue's.
    const CENTRE: (f64, f64) = (130.0, -30.0);
    const RED_HALF: (f64, f64) = (115.0, -30.0);
    const BLUE_HALF: (f64, f64) = (145.0, -30.0);

    fn code(e: &CallError) -> Option<&str> {
        error_code(&e.message).map(|c| c.0)
    }

    fn refused<T: std::fmt::Debug>(r: Result<T>) -> Option<String> {
        let Error(e) = r.unwrap_err();
        error_code(&e).map(|c| c.0.to_owned())
    }

    fn lobby() -> Match {
        Match::new(Config { origin: ORIGIN, ..Config::default() })
    }

    /// Reds 1 and 2, blue 3, started at time 0.
    fn pitch() -> Match {
        let mut m = lobby();
        m.join(1, "red", 0).unwrap();
        m.join(2, "red", 0).unwrap();
        m.join(3, "blue", 0).unwrap();
        m.start(0).unwrap();
        m
    }

    fn seed(extra: &str) -> Vec<u8> {
        format!(r#"{{"origin":[160,-80],"height":32{extra}}}"#).into_bytes()
    }

    fn started_hub(key: &str, seed: &[u8]) -> TestHub<BallHub> {
        let mut hub = TestHub::<BallHub>::spawn_with("ball", key, seed).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "team": "red" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &json!({ "team": "blue" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub
    }

    /// The opcode, the chunk and the actor of an inject, and the rest.
    fn unpack(inject: &[u8]) -> (u8, [i64; 3], [u8; 32], &[u8]) {
        let at = |n: usize| i64::from_le_bytes(inject[10 + 8 * n..18 + 8 * n].try_into().unwrap());
        (inject[1], [at(0), at(1), at(2)], inject[36..68].try_into().unwrap(), &inject[70..])
    }

    #[test]
    fn possession_is_claimed_near_the_ball_protected_for_a_moment_and_passed_to_a_teammate() {
        let mut m = pitch();
        assert_eq!(refused(m.claim(1, 100)).as_deref(), Some(codes::OUT_OF_REACH));
        m.players[0].warden.place(CENTRE, 100);
        m.claim(1, 200).unwrap();
        assert_eq!(m.ball.holder, Some(1));
        m.players[2].warden.place((130.5, -30.0), 300);
        assert_eq!(refused(m.claim(3, 400)).as_deref(), Some(codes::COOLDOWN), "protected");
        m.claim(3, 2_000).unwrap();
        assert_eq!(m.ball.holder, Some(3), "taken after the protection");
        assert_eq!(refused(m.pass(3, 1)).as_deref(), Some(codes::BAD_REQUEST), "not to an opponent");
        m.players[2].warden.place((130.5, -30.0), 2_000);
        m.claim(1, 4_000).unwrap();
        m.pass(1, 2).unwrap();
        assert!(m.ball.holder.is_none() && m.ball.vx < 0.0, "toward red's other player at (115, -30)");
    }

    #[test]
    fn a_shot_into_the_goal_scores_and_the_first_to_the_target_wins() {
        let mut m = pitch();
        m.config.score_to_win = 1;
        m.players[0].warden.place(CENTRE, 0);
        m.claim(1, 100).unwrap();
        m.shoot(1, (160.0, -30.0)).unwrap();
        let mut now = 100;
        while m.phase == Phase::Playing && now < 10_000 {
            now += TICK_MS;
            m.tick(now);
            m.players.iter_mut().for_each(|p| p.moved_at = now);
        }
        assert_eq!((m.score, m.winner), ([1, 0], Some(0)));
        assert!(m.events.iter().any(|e| e["type"] == "goal"));
    }

    #[test]
    fn a_wide_shot_comes_back_off_the_end_wall() {
        let mut m = pitch();
        m.players[0].warden.place((155.0, -48.0), 0);
        m.ball = BallState { x: 155.0, y: -48.0, ..BallState::default() };
        m.claim(1, 600).unwrap();
        m.shoot(1, (170.0, -48.0)).unwrap();
        for i in 1..10 {
            m.tick(600 + i * TICK_MS);
            m.players.iter_mut().for_each(|p| p.moved_at = 600 + i * TICK_MS);
        }
        assert_eq!(m.score, [0, 0]);
        assert!(m.ball.vx < 0.0 && m.ball.x <= 160.0, "it bounced: {:?}", m.ball);
    }

    #[test]
    fn the_side_walls_turn_the_ball_and_a_time_out_decides_by_the_score() {
        let mut m = pitch();
        m.ball = BallState { x: 130.0, y: -49.0, vy: -30.0, ..BallState::default() };
        m.tick(100);
        assert!(m.ball.vy > 0.0 && m.ball.y >= -50.0, "{:?}", m.ball);
        m.score = [0, 1];
        m.players.iter_mut().for_each(|p| p.moved_at = 300_000);
        m.tick(300_000);
        assert_eq!((m.phase, m.winner), (Phase::Finished, Some(1)));
        assert!(m.events.iter().any(|e| e["type"] == "finished" && e["winner"] == "blue"));
    }

    #[test]
    fn positions_are_world_metres_from_the_origin_on_the_wire_and_in_the_view() {
        let mut hub = TestHub::<BallHub>::spawn_with("ball", "m1", br#"{"origin":[1000,-500]}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "team": "red" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &json!({ "team": "blue" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        let view: BallView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.origin, view.width, view.depth), ((1000.0, -500.0), 60.0, 40.0));
        assert_eq!((view.ball.x, view.ball.y), (1030.0, -480.0));
        let at: Vec<(f64, f64)> = view.players.iter().map(|p| (p.x, p.y)).collect();
        assert_eq!(at, vec![(1015.0, -480.0), (1045.0, -480.0)]);
        hub.advance(500).unwrap();
        let kept: Kept = hub.call_typed(Caller::Player(1), "move", &json!({ "x": 1015.5, "y": -480.0 })).unwrap();
        assert_eq!(kept, Kept { x: 1015.5, y: -480.0 });
        let map: Value = decode(&hub.call(Caller::Player(1), "move", &encode(&json!({ "x": 1016.0, "y": -480.0 })).unwrap()).unwrap()).unwrap();
        assert_eq!(map.as_object().map(|o| o.len()), Some(2), "a map, not an array");
    }

    #[test]
    fn a_position_or_aim_that_is_not_a_finite_number_is_refused_and_changes_nothing() {
        let mut m = pitch();
        m.players[0].warden.place(CENTRE, 0);
        m.claim(1, 100).unwrap();
        m.events.clear();
        let (players, ball) = (m.players.clone(), m.ball.clone());
        for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 1e39] {
            assert_eq!(refused(m.moved(1, (bad, -30.0), 200)).as_deref(), Some(codes::BAD_REQUEST));
            assert_eq!(refused(m.moved(1, (130.0, bad), 200)).as_deref(), Some(codes::BAD_REQUEST));
            assert_eq!(refused(m.shoot(1, (bad, -30.0))).as_deref(), Some(codes::BAD_REQUEST));
            assert_eq!(refused(m.shoot(1, (130.0, bad))).as_deref(), Some(codes::BAD_REQUEST));
        }
        assert_eq!((&m.players, &m.ball), (&players, &ball), "no position, clock or ball changed");
        assert!(m.events.is_empty());
        m.shoot(1, (160.0, -30.0)).unwrap();
        let mut hub = started_hub("m1", br#"{"origin":[160,-80]}"#);
        let e = hub.call_typed::<_, Kept>(Caller::Player(1), "move", &json!({ "x": 10.0 })).unwrap_err();
        assert_eq!(code(&e), Some(codes::BAD_REQUEST), "a missing coordinate too");
    }

    #[test]
    fn a_leaver_drops_the_ball_loose_where_they_were_and_may_join_again_while_playing() {
        let mut m = pitch();
        m.players[0].warden.place(CENTRE, 0);
        m.claim(1, 100).unwrap();
        m.moved(1, (130.5, -30.0), 300).unwrap();
        assert_eq!((m.ball.x, m.ball.holder), (130.5, Some(1)));
        m.leave(1).unwrap();
        assert_eq!((m.ball.x, m.ball.y, m.ball.vx, m.ball.vy, m.ball.holder), (130.5, -30.0, 0.0, 0.0, None));
        assert_eq!(m.ball.last_touch, Some(1));
        assert!(m.events.iter().any(|e| e["type"] == "left" && e["player"] == 1 && e["team"] == "red"));
        assert_eq!(m.players.len(), 2);
        assert_eq!(refused(m.leave(1)).as_deref(), Some(codes::NOT_MEMBER));
        assert_eq!(refused(m.join(1, "blue", 500)).as_deref(), Some(codes::STARTED), "no change of team by leaving");
        m.join(1, "red", 500).unwrap();
        assert_eq!(m.view().players.last().map(|p| (p.player, p.team.as_str(), p.x, p.y)), Some((1, "red", 130.5, -30.0)));
        m.join(4, "red", 600).unwrap();
        assert_eq!(m.view().players.last().map(|p| (p.x, p.y)), Some(RED_HALF), "a new player while playing is placed at their half");
        assert_eq!(refused(m.join(4, "blue", 700)).as_deref(), Some(codes::STARTED), "no change of team while playing");
        m.join(4, "red", 700).unwrap();
        assert_eq!(refused(m.join(5, "green", 700)).as_deref(), Some(codes::BAD_REQUEST));
    }

    #[test]
    fn a_leaver_in_the_lobby_is_gone_and_a_full_team_is_full() {
        let mut m = lobby();
        m.config.team_max = 1;
        m.join(1, "red", 0).unwrap();
        assert_eq!(refused(m.join(2, "red", 0)).as_deref(), Some(codes::FULL));
        m.leave(1).unwrap();
        m.join(2, "red", 0).unwrap();
        m.join(2, "blue", 10).unwrap();
        assert_eq!(m.view().players.iter().map(|p| (p.team.as_str(), p.x)).collect::<Vec<_>>(), vec![("blue", 145.0)]);
    }

    #[test]
    fn a_player_who_sends_no_move_is_removed_by_the_tick_and_drops_the_ball() {
        let mut m = pitch();
        m.config.idle_ms = 1_000;
        m.players[0].warden.place(CENTRE, 0);
        m.claim(1, 100).unwrap();
        m.moved(2, (115.5, -30.0), 800).unwrap();
        m.moved(3, (145.5, -30.0), 800).unwrap();
        assert!(m.tick(900) && m.players.len() == 3);
        m.tick(1_100);
        assert_eq!(m.players.iter().map(|p| p.player).collect::<Vec<_>>(), vec![2, 3], "the ones who reported stay");
        assert_eq!((m.ball.holder, m.ball.x, m.ball.y, m.ball.vx), (None, 130.0, -30.0, 0.0));
        assert!(m.events.iter().any(|e| e["type"] == "removed" && e["why"] == "idle" && e["player"] == 1 && e["team"] == "red"));
        assert_eq!((m.drops, m.leavers.len(), m.leavers[0].player), (1, 1, 1), "the held ball's drop and the leaver are recorded");
        m.join(1, "red", 1_200).unwrap();
        m.tick(1_300);
        assert_eq!(m.players.len(), 3, "back with a fresh clock");
        m.config.idle_ms = 0;
        m.tick(200_000);
        assert_eq!(m.players.len(), 3, "0 never removes");
    }

    #[test]
    fn a_loaded_snapshot_restarts_the_idle_clock() {
        let snapshot = {
            let mut hub = started_hub("m1", br#"{"idle_ms":1000}"#);
            hub.hub_mut().game.players.iter_mut().for_each(|p| p.moved_at = 0);
            hub.persist().unwrap()
        };
        let mut hub = TestHub::<BallHub>::load("ball", "m1", &snapshot, 1).unwrap();
        let now = hub.now_ms();
        assert!(hub.hub().game.players.iter().all(|p| p.moved_at == now));
        assert!(hub.hub_mut().game.tick(now + 100));
        assert_eq!(hub.hub().game.players.len(), 2);
    }

    #[test]
    fn start_puts_every_player_at_their_half_with_a_fresh_clock_and_needs_both_teams() {
        let mut m = lobby();
        m.join(1, "red", 0).unwrap();
        assert_eq!(refused(m.start(10)).as_deref(), Some(codes::NOT_READY));
        m.join(2, "blue", 0).unwrap();
        m.players[0].warden.place((101.0, -49.0), 5);
        m.players[1].warden.place((159.0, -31.0), 5);
        m.start(1_000).unwrap();
        let at: Vec<(f64, f64)> = m.view().players.iter().map(|p| (p.x, p.y)).collect();
        assert_eq!(at, vec![RED_HALF, BLUE_HALF]);
        assert!(m.players.iter().all(|p| p.moved_at == 1_000 && p.warden.placed));
        assert_eq!(refused(m.start(1_100)).as_deref(), Some(codes::STARTED));
        m.phase = Phase::Finished;
        m.players[0].warden.place(CENTRE, 2_000);
        m.start(3_000).unwrap();
        assert_eq!(m.view().players[0].x, RED_HALF.0, "a restart places them again");
    }

    #[test]
    fn a_start_and_a_join_place_without_a_strike() {
        let mut m = pitch();
        let kept = m.moved(1, (150.0, -30.0), 200).unwrap();
        assert_eq!((kept, m.players[0].warden.strikes(200)), (RED_HALF, 0), "a start");
        m.join(4, "blue", 300).unwrap();
        let kept = m.moved(4, (110.0, -30.0), 500).unwrap();
        assert_eq!((kept, m.players[3].warden.strikes(500)), (BLUE_HALF, 0), "a join while playing");
        m.leave(4).unwrap();
        m.join(4, "blue", 600).unwrap();
        assert_eq!(m.moved(4, (110.0, -30.0), 700).unwrap(), BLUE_HALF, "a rejoin");
        assert!(m.events.iter().all(|e| e["type"] != "violation"));
    }

    #[test]
    fn the_pitch_is_the_wardens_bounds_and_pressing_its_edge_is_no_strike() {
        let mut m = pitch();
        m.players[0].warden.place((100.5, -49.5), 0);
        let kept = m.moved(1, (99.0, -51.0), 200).unwrap();
        assert_eq!(kept, (100.0, -50.0), "the pitch's edge is its origin");
        assert_eq!(m.players[0].warden.strikes(200), 0);
        assert!(m.events.is_empty());
    }

    #[test]
    fn a_leaver_of_a_running_match_rejoins_only_their_team_and_where_they_were() {
        let mut m = pitch();
        assert_eq!(m.moved(2, (116.0, -30.0), 300).unwrap(), (116.0, -30.0));
        m.leave(2).unwrap();
        assert_eq!(refused(m.join(2, "blue", 500)).as_deref(), Some(codes::STARTED), "not the other team");
        m.join(2, "red", 500).unwrap();
        let back = m.view().players.into_iter().find(|p| p.player == 2).unwrap();
        assert_eq!((back.x, back.y), (116.0, -30.0), "not the half");
        assert_eq!(m.moved(2, (135.0, -30.0), 600).unwrap(), (116.0, -30.0), "placed there");
        assert_eq!(m.players.last().map(|p| p.warden.strikes(600)), Some(0));
        assert!(m.leavers.is_empty());
    }

    #[test]
    fn the_leavers_are_capped_and_a_start_forgets_them() {
        let mut m = pitch();
        m.config.team_max = 1;
        for n in 0..8 {
            m.leavers.push(Leaver { player: 100 + n, team: 0, at: RED_HALF });
        }
        m.leave(1).unwrap();
        let ids: Vec<u64> = m.leavers.iter().map(|l| l.player).collect();
        assert_eq!(ids, vec![101, 102, 103, 104, 105, 106, 107, 1], "8 times team_max, the oldest dropped");
        m.config.team_max = 5;
        m.phase = Phase::Finished;
        m.start(2_000).unwrap();
        assert!(m.leavers.is_empty());
        m.join(1, "blue", 2_100).unwrap();
        assert_eq!(m.view().players.last().map(|p| (p.team.as_str(), p.x)), Some(("blue", 145.0)), "a new match is a new start");
        m.leave(3).unwrap();
        m.phase = Phase::Finished;
        m.join(3, "red", 3_000).unwrap();
        assert_eq!(m.view().players.last().map(|p| (p.team.as_str(), p.x)), Some(("red", 115.0)), "between matches teams are free");
    }

    #[test]
    fn the_view_shows_ends_at_only_while_the_match_is_on() {
        let mut m = lobby();
        assert_eq!(m.view().ends_at, None);
        m.join(1, "red", 0).unwrap();
        m.join(2, "blue", 0).unwrap();
        m.start(1_000).unwrap();
        assert_eq!(m.view().ends_at, Some(301_000));
        m.players.iter_mut().for_each(|p| p.moved_at = 301_000);
        m.tick(301_000);
        assert_eq!(m.phase, Phase::Finished);
        assert_eq!(m.view().ends_at, None);
    }

    #[test]
    fn a_ball_that_goes_loose_has_no_hold_time() {
        let mut m = pitch();
        m.players[0].warden.place(CENTRE, 0);
        m.claim(1, 100).unwrap();
        assert_eq!(m.ball.held_since, 100);
        m.pass(1, 2).unwrap();
        assert_eq!(m.ball.held_since, 0, "a pass");
        m.claim(1, 1_000).unwrap();
        m.shoot(1, (160.0, -30.0)).unwrap();
        assert_eq!(m.ball.held_since, 0, "a shot");
        m.ball = BallState { x: 130.0, y: -30.0, ..BallState::default() };
        m.claim(1, 2_000).unwrap();
        assert_eq!(m.ball.held_since, 2_000);
        m.leave(1).unwrap();
        assert_eq!((m.ball.holder, m.ball.held_since), (None, 0), "a holder who leaves");
    }

    #[test]
    fn a_holder_who_leaves_drops_the_ball_as_a_loose_possession_event() {
        let mut m = pitch();
        m.leave(2).unwrap();
        assert_eq!(m.drops, 0, "a player without the ball drops nothing");
        m.players[0].warden.place(CENTRE, 0);
        m.claim(1, 100).unwrap();
        m.leave(1).unwrap();
        assert_eq!(m.drops, 1);
        let mut hub = started_hub("m1", &seed(r#","realtime":true"#));
        let now = hub.now_ms();
        hub.hub_mut().game.players[0].warden.place((190.0, -60.0), now);
        let _: bool = hub.call_typed(Caller::Player(1), "claim", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "leave", &()).unwrap();
        let kinds: Vec<(Option<String>, bool)> = hub
            .emitted()
            .into_iter()
            .filter(|m| m[1] == 139)
            .map(|m| {
                let body: Value = serde_json::from_slice(&unpack(&m).3[2..]).unwrap();
                (body["kind"].as_str().map(String::from), body["holderId"].is_null())
            })
            .collect();
        assert_eq!(kinds, vec![(Some("claimed".into()), false), (Some("loose".into()), true)]);
    }

    #[test]
    fn the_ball_and_its_possession_reach_the_pitchs_longer_side_plus_one_chunk() {
        let at = |width: f64, depth: f64| Config { width, depth, ..Config::default() }.reach();
        assert_eq!((at(60.0, 40.0), at(40.0, 100.0), at(16.0, 10.0), at(400.0, 40.0)), (5, 8, 2, 8), "at most 8");
        let mut hub = started_hub("m1", &seed(r#","realtime":true,"width":120"#));
        hub.advance(100).unwrap();
        let now = hub.now_ms();
        hub.hub_mut().game.players[0].warden.place((220.0, -60.0), now);
        let _: bool = hub.call_typed(Caller::Player(1), "claim", &()).unwrap();
        let sent = hub.emitted();
        assert!(sent.iter().any(|m| m[1] == 130) && sent.iter().any(|m| m[1] == 139));
        assert!(sent.iter().all(|m| m[34] == 9u8.min(MAX_DISTANCE)), "ceil(120 / 16) + 1 = 9, capped at 8");
    }

    #[test]
    fn a_realtime_key_the_client_cannot_read_back_is_refused() {
        for key in ["cup@1", "has space", "caf\u{e9}", "tab\there", "a\u{7f}b"] {
            let Err(e) = TestHub::<BallHub>::spawn_with("ball", key, &seed(r#","realtime":true"#)) else { panic!("{key:?} was accepted") };
            assert!(e.message.contains('@'), "{}", e.message);
        }
        assert!(TestHub::<BallHub>::spawn_with("ball", "cup-1_~!", &seed(r#","realtime":true"#)).is_ok());
        assert!(TestHub::<BallHub>::spawn_with("ball", "cup@1", &seed("")).is_ok(), "only with realtime");
    }

    #[test]
    fn the_grace_is_configurable_and_the_hub_allows_up_to_grace_times_max_speed() {
        assert_eq!(Config::default().grace, 1.25);
        assert_eq!(serde_json::from_str::<Config>(r#"{"grace":1.0}"#).unwrap().grace, 1.0);
        let walked = |grace: f64| {
            let mut m = Match::new(Config { origin: ORIGIN, grace, ..Config::default() });
            m.join(1, "red", 0).unwrap();
            m.join(2, "blue", 0).unwrap();
            m.start(0).unwrap();
            let mut kept = RED_HALF;
            for n in 1..=10 {
                let now = n * 200;
                kept = m.moved(1, (RED_HALF.0 + 10.8 * now as f64 / 1000.0, RED_HALF.1), now).unwrap();
            }
            kept.0 - RED_HALF.0
        };
        assert!((walked(1.25) - 21.6).abs() < 1e-9, "1.2 times max_speed is within 1.25");
        assert!(walked(1.0) < 21.0, "but not within 1.0: {}", walked(1.0));
    }

    #[test]
    fn an_honest_walker_whose_reports_are_800_ms_apart_is_kept_in_full_and_never_struck() {
        let rules = Rules { max_speed: 9.0, ..walker_rules() };
        let mut w = walking(&rules);
        for n in 1..=40 {
            let now = n * 800;
            let claimed = (9.0 * now as f64 / 1000.0, 0.0);
            let verdict = w.check(&rules, claimed, now);
            assert!(verdict.violation.is_none() && (verdict.at.0 - claimed.0).abs() < 1e-9, "cut at {now} ms");
        }
        assert_eq!(w.strikes(32_000), 0);
    }

    #[test]
    fn the_hub_publishes_events_and_snapshots_and_answers_the_snapshot_call() {
        let mut hub = started_hub("m1", br#"{"duration_ms":2000}"#);
        let _: bool = hub.call_typed(Caller::Player(1), "join", &json!({ "team": "red" })).unwrap();
        let e = hub.call(Caller::Player(2), "start", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN));
        let e = hub.call(Caller::Player(1), "start", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::STARTED));
        let e = hub.call(Caller::Player(2), "claim", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::OUT_OF_REACH));
        hub.advance(2_500).unwrap();
        let view: BallView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.winner), (Phase::Finished, None));
        let reply = hub.call(Caller::Player(1), kit::SNAPSHOT_METHOD, &encode(&json!({ "topic": "ball" })).unwrap()).unwrap();
        let snapshot: kit::Message<BallView> = decode(&reply).unwrap();
        assert_eq!(snapshot.body.phase, Phase::Finished);
        assert!(hub.timers().is_empty());
    }

    #[test]
    fn a_realtime_match_shows_its_ball_as_an_actor_named_by_its_key_in_chunks_with_the_owner() {
        let mut hub = started_hub("bp-x", &seed(r#","realtime":true,"duration_ms":1000"#));
        hub.advance(300).unwrap();
        let shown: Vec<Vec<u8>> = hub.emitted().into_iter().filter(|m| m[1] == 130).collect();
        assert_eq!(shown.len(), 3, "once a tick");
        let (_, chunk_at, actor, payload) = unpack(&shown[0]);
        assert_eq!(actor, actor_id("Bbp-x@1"), "the text between the leading capital and @ is the match's key");
        let state: ActorState = decode(payload).unwrap();
        assert_eq!((state.kind.as_str(), state.id, state.owner), ("ball", 1, None));
        assert_eq!((state.x, state.y, state.z), (190.0 / 16.0, -60.0 / 16.0, 2.0), "world metres over 16, height up");
        assert_eq!(chunk_at, [11, -4, 2]);
        let now = hub.now_ms();
        hub.hub_mut().game.players[0].warden.place((190.0, -60.0), now);
        let _: bool = hub.call_typed(Caller::Player(1), "claim", &()).unwrap();
        hub.advance(100).unwrap();
        let last = hub.emitted().into_iter().rfind(|m| m[1] == 130).unwrap();
        let state: ActorState = decode(unpack(&last).3).unwrap();
        assert_eq!(state.owner, Some(1), "the holder owns it");
        hub.advance(1_000).unwrap();
        let view: BallView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(view.phase, Phase::Finished);
        let count = hub.emitted().iter().filter(|m| m[1] == 130).count();
        hub.advance(1_000).unwrap();
        assert_eq!(hub.emitted().iter().filter(|m| m[1] == 130).count(), count, "nothing is shown once the match is over");
    }

    #[test]
    fn a_realtime_match_sends_possession_at_the_centre_chunk_and_others_send_nothing() {
        {
            let mut hub = started_hub("m1", &seed(r#","realtime":true"#));
            let now = hub.now_ms();
            hub.hub_mut().game.players[0].warden.place((190.0, -60.0), now);
            let _: bool = hub.call_typed(Caller::Player(1), "claim", &()).unwrap();
            let events: Vec<Vec<u8>> = hub.emitted().into_iter().filter(|m| m[1] == 139).collect();
            assert_eq!(events.len(), 1);
            let (_, chunk_at, actor, payload) = unpack(&events[0]);
            assert_eq!((chunk_at, actor, payload[..2].to_vec()), ([11, -4, 2], actor_id("Bm1@1"), 100u16.to_le_bytes().to_vec()));
            let body: Value = serde_json::from_slice(&payload[2..]).unwrap();
            assert_eq!((body["kind"].as_str(), body["holderId"].as_str()), (Some("claimed"), Some("1")));
        }
        let mut hub = started_hub("m1", &seed(""));
        let now = hub.now_ms();
        hub.hub_mut().game.players[0].warden.place((190.0, -60.0), now);
        let _: bool = hub.call_typed(Caller::Player(1), "claim", &()).unwrap();
        hub.advance(1_000).unwrap();
        assert!(hub.emitted().is_empty());
    }

    #[test]
    fn a_key_the_actor_id_cannot_hold_is_refused_only_with_realtime() {
        let key = "k".repeat(30);
        {
            let Err(e) = TestHub::<BallHub>::spawn_with("ball", &key, &seed(r#","realtime":true"#)) else { panic!("a 30 byte key fits no id") };
            assert!(e.message.contains("29"), "{}", e.message);
        }
        {
            let hub = TestHub::<BallHub>::spawn_with("ball", &"k".repeat(29), &seed(r#","realtime":true"#)).unwrap();
            assert!(hub.hub().game.config.realtime);
        }
        assert!(TestHub::<BallHub>::spawn_with("ball", &key, &seed("")).is_ok());
    }

    fn walker_rules() -> Rules {
        Rules { max_speed: 6.0, teleport: 12.0, bounds: None, grace: 1.25 }
    }

    /// A warden at the origin that has had its first report and is no longer just placed.
    fn walking(rules: &Rules) -> Warden {
        let mut w = Warden::at((0.0, 0.0), 0);
        assert_eq!(w.check(rules, (0.0, 0.0), 0).violation, None);
        assert!(!w.placed);
        w
    }

    #[test]
    fn an_honest_walker_is_never_cut_at_any_report_rate_or_after_a_late_report() {
        let rules = walker_rules();
        let gaps: [&[u64]; 3] = [&[200], &[50], &[350, 50]];
        for gaps in gaps {
            let mut w = walking(&rules);
            let mut now = 0;
            for n in 0..400 {
                now += gaps[n % gaps.len()];
                let claimed = (6.0 * now as f64 / 1000.0, 0.0);
                let verdict = w.check(&rules, claimed, now);
                assert!(verdict.violation.is_none() && (verdict.at.0 - claimed.0).abs() < 1e-9, "cut at {now} ms with gaps {gaps:?}");
            }
            assert_eq!(w.strikes(now), 0);
        }
    }

    #[test]
    fn a_speed_hack_is_held_to_the_top_speed_whatever_the_report_rate() {
        let rules = Rules { teleport: 30.0, ..walker_rules() };
        for hz in [5, 20] {
            let mut w = walking(&rules);
            let mut kept = (0.0, 0.0);
            for n in 1..=5 * hz {
                let now = n * 1_000 / hz;
                kept = w.check(&rules, (9.6 * now as f64 / 1000.0, 0.0), now).at;
            }
            assert!((kept.0 - 37.5).abs() < 1e-6, "{} m in 5 s at {hz} Hz: 1.25 times 6 m/s is 37.5", kept.0);
            assert!(w.strikes(5_000) > 0, "a cut of more than a metre is a strike");
        }
    }

    #[test]
    fn a_silence_banks_only_the_cap_and_a_small_cut_is_no_strike() {
        let rules = walker_rules();
        let mut w = walking(&rules);
        let verdict = w.check(&rules, (10.0, 0.0), 1_600);
        let cap = 6.0 * 1.25 * 1.0 + 0.5;
        assert!((verdict.at.0 - cap).abs() < 1e-9, "1.6 s of silence moved {} m", verdict.at.0);
        assert_eq!(verdict.violation.map(|v| v.kind), Some(ViolationKind::Speed));
        let mut w = walking(&rules);
        let jitter = w.check(&rules, (3.8, 0.0), 400);
        assert!((jitter.at.0 - 3.0).abs() < 1e-9 && jitter.violation.is_none() && w.strikes(400) == 0, "{jitter:?}");
        let mut w = walking(&rules);
        assert!(w.check(&rules, (4.2, 0.0), 400).violation.is_some());
        let mut w = walking(&rules);
        assert_eq!(w.check(&rules, (3.0, 0.0), 400).at, (3.0, 0.0));
        assert_eq!(w.check(&rules, (5.0, 0.0), 400).at, (3.0, 0.0), "what was spent is spent");
    }

    #[test]
    fn a_placed_player_is_answered_with_the_place_and_never_struck() {
        let rules = walker_rules();
        let mut w = Warden::at((5.0, 5.0), 0);
        for now in [200, 400, 600] {
            let verdict = w.check(&rules, (10.0, 5.0), now);
            assert_eq!((verdict.at, verdict.violation), ((5.0, 5.0), None));
        }
        assert_eq!((w.strikes(600), w.last), (0, Some((5.0, 5.0, 0))), "the placement time is kept");
        assert!(w.placed);
        assert_eq!(w.check(&rules, (5.5, 5.0), 800).violation, None);
        assert!(!w.placed, "a report within reach ends the placement");
        assert_eq!(w.check(&rules, (25.0, 5.0), 1_000).violation.map(|v| v.kind), Some(ViolationKind::Teleport));
        w.place((50.0, 50.0), 2_000);
        assert_eq!(w.check(&rules, (47.0, 50.0), 2_200).at, (50.0, 50.0), "a placement starts with no budget");
        assert_eq!(w.strikes(2_200), 1);
    }

    #[test]
    fn pressing_against_the_edge_clamps_and_is_not_a_strike() {
        let rules = Rules { bounds: Some((0.0, 0.0, 40.0, 40.0)), ..walker_rules() };
        let mut w = Warden::at((39.0, 20.0), 0);
        let mut now = 0;
        for _ in 0..50 {
            now += 200;
            let verdict = w.check(&rules, (45.0, 20.0), now);
            assert_eq!(verdict.violation, None);
            assert!(verdict.at.0 <= 40.0);
        }
        assert_eq!((w.last.map(|l| (l.0, l.1)), w.strikes(now)), (Some((40.0, 20.0)), 0));
    }
}
