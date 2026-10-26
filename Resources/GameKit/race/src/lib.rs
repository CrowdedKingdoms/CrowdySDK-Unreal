//! A race, keyed by its id: a course of gates passed in order (`ckx_sdk::kit::play::timing`), a
//! countdown, laps and splits, places in finishing order, the rest given `finish_grace_ms` after
//! the winner, and a ghost of the winner's run. The shape of a kart race, a time trial, a
//! foot race: clients report where their racer is (`move { x, y }`, a few times a second) and the
//! movement warden keeps what the rules allow, so the hub times the positions it kept, not the ones
//! a client claimed. With a `board`, the finishers' times go to that `leaderboard` (configure it
//! with `order: "low"`; the manifest lets `race` call `leaderboard`). Replace the [`Config`]'s
//! gates with your course.
//!
//! This is the Unreal SDK's z-up copy of the platform's starter. What differs: it is one type, a
//! race (the ball game has its own crate), every position and gate, on the wire and in the view,
//! is in world metres on the ground, `x` then `y` (the Unreal location over 100), and `move`
//! answers a map, `{ x, y }`. With `realtime` on, timing also goes out as realtime event 97 at the
//! first gate's chunk (16 m chunks, z up, `height` metres up), to the players within the chunks
//! from there to the farthest gate plus one (at least 4, at most the realtime tier's 8). And the
//! platform's holes are closed. A `move` whose position is not a finite number is refused
//! `bad_request` and changes nothing. The platform's warden gives every report 0.5 m of slack,
//! so a client reporting faster moved faster; this crate's own warden gives each racer a travel
//! budget that grows with the time since the last report at `rules.max_speed` times
//! `rules.grace`, and holds at most 1 s of it plus 0.5 m, so the report rate buys no speed while
//! a slow round trip is not cut. The hub therefore allows up to `grace` (1.25) times `max_speed`:
//! set `max_speed` to what the game allows divided by the grace for a strict cap. A cut of 1 m
//! or less is jitter and a bounds clamp is not a violation: neither is a strike. A racer the hub
//! has just placed (join, start, the go) is answered with the place, with no strike, until a
//! report comes from within reach of it. During the countdown every racer is held on the start
//! line: a `move` is answered with the line (no flying start). A gate is passed when the path
//! from the last position kept to the new one comes within its radius, not only the reported
//! point, so a slower report rate cannot miss a gate, and the pass is timed at its place along
//! the path between the two reports. On a one-gate course a lap needs a report of its own. A
//! race nobody finishes ends: `time_limit_ms` after the go it closes; a racer who leaves
//! (`retired`, `why`: `left`) or sends no `move` for `idle_ms` while racing (`why`: `idle`)
//! retires and does not finish; and once every racer has finished or retired it closes at once,
//! where the platform's waited for everyone to finish. `join` from a racer already in is true in
//! any phase, unless they retired. The `finished` results list the finishers by place, then the rest. The ghost keeps
//! the whole run however long: past 600 frames it drops every other one and doubles the least
//! spacing between frames. The leaderboard `action_id` is a hash of the race's key and the
//! player, so a long key still fits the leaderboard's limit. The snapshot is not sent per
//! report: `move` publishes its events at once, and the topic's snapshot goes out from a timer
//! every 200 ms while counting down or racing, when something changed; join, leave, start, the
//! go and the close publish one at once.
//!
//! Endpoints (players): `join`, `leave` (in the lobby the racer is removed; later they retire and
//! cannot join again), `start` (the first to join, or a developer), `move { x, y }` (answers the
//! position kept, as `{ x, y }`), `ghost` (the winner's track, `{ player, total_ms, frames: [{
//! at_ms, x, y }] }`, or nil before a winner), `state` (anyone). It speaks the kit protocol
//! (`ckx_sdk::kit`): the `race` topic carries `snapshot` messages as racers move on and `event`
//! messages (`type`: `go`, `timing` with its `stage`, `violation`, `retired`, `finished`) as they
//! happen; `snapshot { topic: "race" }` answers it; changes may carry an `action_id`; refusals
//! carry `kit:<code>:` codes.

use ckx_sdk::kit::play::movement::{Rules, STRIKE_WINDOW_MS, Verdict, Violation, ViolationKind};
use ckx_sdk::emit::MAX_DISTANCE;
use ckx_sdk::kit::play::timing::{self, Course, RunTimer, TimingEvent};
use ckx_sdk::kit::{self, Publisher, Receipts, codes, wire};
use ckx_sdk::prelude::*;
use serde_json::{Value, json};

pub const TOPIC: &str = "race";
pub const TICK_MS: u64 = 200;
/// The least reach, in chunks, of a timing event.
const EVENT_DISTANCE: u8 = 4;
/// Metres along one side of a chunk.
const CHUNK_M: f64 = 16.0;
/// Frames of a ghost kept before every other one goes.
const GHOST_FRAMES: usize = 600;
/// The most travel a racer banks: this many seconds at full speed, and this many metres more.
const BANK_S: f64 = 1.0;
const BANK_M: f64 = 0.5;
/// A speed cut of at most this many metres is jitter and costs no strike.
const JITTER_M: f64 = 1.0;

/// A gate in world metres.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
pub struct Gate {
    pub x: f64,
    pub y: f64,
    pub radius: f64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    /// The course's name in timing events.
    pub course_id: String,
    /// In order; gate 0 is the start and finish line.
    pub gates: Vec<Gate>,
    pub laps: u32,
    pub countdown_ms: u64,
    pub racers_max: usize,
    /// The hub allows up to `grace` times `max_speed`: for a strict cap set `max_speed` to what
    /// the game allows divided by the grace.
    pub rules: Rules,
    /// How long the rest have once the winner is in.
    pub finish_grace_ms: u64,
    /// The leaderboard the finishers' times go to.
    pub board: Option<String>,
    /// The race closes this long after the go; 0 never does.
    pub time_limit_ms: u64,
    /// A racer who has not finished and sends no `move` for this long while racing retires; 0
    /// never does.
    pub idle_ms: u64,
    /// Whether timing also goes out as a realtime event at the first gate's chunk.
    pub realtime: bool,
    /// The course's height in metres, for that chunk's z.
    pub height: f64,
}

impl Default for Config {
    fn default() -> Self {
        let gate = |x: f64, y: f64| Gate { x, y, radius: 3.0 };
        Self {
            course_id: "loop".into(),
            gates: vec![gate(0.0, 0.0), gate(30.0, 0.0), gate(15.0, 25.0)],
            laps: 2,
            countdown_ms: 3_000,
            racers_max: 8,
            rules: Rules { max_speed: 12.0, teleport: 20.0, bounds: None, grace: 1.25 },
            finish_grace_ms: 30_000,
            board: None,
            time_limit_ms: 600_000,
            idle_ms: 15_000,
            realtime: false,
            height: 0.0,
        }
    }
}

impl Config {
    /// The chunk at the first gate, z up.
    pub fn start_chunk(&self) -> ChunkPos {
        let chunk = |metres: f64| (metres / CHUNK_M).floor() as i64;
        let (x, y) = self.gates.first().map_or((0.0, 0.0), |g| (g.x, g.y));
        ChunkPos::new(chunk(x), chunk(y), chunk(self.height))
    }

    /// How many chunks around the first gate a timing event reaches: out to the farthest gate and
    /// one more.
    fn reach(&self) -> u8 {
        let chunk = |metres: f64| (metres / CHUNK_M).floor() as i64;
        let (x, y) = self.gates.first().map_or((0, 0), |g| (chunk(g.x), chunk(g.y)));
        let far = self.gates.iter().map(|g| (chunk(g.x) - x).abs().max((chunk(g.y) - y).abs())).max().unwrap_or(0);
        (far + 1).clamp(i64::from(EVENT_DISTANCE), i64::from(MAX_DISTANCE)) as u8
    }

    fn course(&self) -> Course {
        let gates = self.gates.iter().map(|g| timing::Gate { x: g.x as f32, z: g.y as f32, radius: g.radius as f32 }).collect();
        Course { gates, laps: self.laps }
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Lobby,
    Countdown,
    Racing,
    Finished,
}

/// One racer's movement as the hub last accepted it. The movement warden of
/// `ckx_sdk::kit::play::movement`, with a capped travel budget that grows with time instead of
/// a slack per report, no strike for a bounds clamp, a small cut or a placement.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq)]
pub struct Warden {
    /// The position kept, and when.
    pub last: Option<(f64, f64, u64)>,
    pub strikes: u32,
    pub last_strike_ms: u64,
    /// The hub put the racer here and no report within reach of it has come yet: a report far
    /// from it is answered with it, without a strike.
    #[serde(default)]
    pub placed: bool,
    /// Metres the racer may still travel, as of the time in `last`.
    #[serde(default)]
    pub budget: f64,
}

fn clamp_to(bounds: Option<(f64, f64, f64, f64)>, p: (f64, f64)) -> (f64, f64) {
    let Some((x0, y0, x1, y1)) = bounds else { return p };
    (p.0.clamp(x0.min(x1), x0.max(x1)), p.1.clamp(y0.min(y1), y0.max(y1)))
}

impl Warden {
    /// A warden that starts from `at`: a place the hub put the racer.
    pub fn at(at: (f64, f64), now: u64) -> Self {
        Self { last: Some((at.0, at.1, now)), placed: true, ..Self::default() }
    }

    /// Moves the racer where the hub says, with no check.
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

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Racer {
    pub player: u64,
    pub timer: RunTimer,
    pub warden: Warden,
    pub ghost: Trace,
    pub place: Option<u32>,
    pub total_ms: Option<u64>,
    pub did_not_finish: bool,
    /// When the racer last sent a `move`, or last came into play.
    #[serde(default)]
    pub moved_at: u64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct RacerView {
    pub player: u64,
    pub x: f64,
    pub y: f64,
    pub lap: u32,
    pub next_gate: usize,
    pub best_lap_ms: Option<u64>,
    pub place: Option<u32>,
    pub total_ms: Option<u64>,
    pub did_not_finish: bool,
}

/// The race as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct RaceView {
    pub phase: Phase,
    pub course_id: String,
    pub gates: Vec<Gate>,
    pub laps: u32,
    pub go_at: Option<u64>,
    /// When the race closes on the winner's grace.
    pub closes_at: Option<u64>,
    /// When the race closes on its time limit: only while racing, and only with a limit.
    pub ends_at: Option<u64>,
    pub racers: Vec<RacerView>,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq)]
pub struct GhostFrame {
    /// Milliseconds from the winner's start.
    pub at_ms: u32,
    pub x: f64,
    pub y: f64,
}

/// A racer's run as frames, thinned as it grows so the whole run stays covered.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct Trace {
    pub frames: Vec<GhostFrame>,
    pub started_at_ms: u64,
    /// The least time between two frames; it doubles each time the frames are thinned.
    pub gap_ms: u64,
}

impl Trace {
    pub fn record(&mut self, now: u64, x: f64, y: f64) {
        if self.started_at_ms == 0 {
            self.started_at_ms = now;
        }
        let at_ms = now.saturating_sub(self.started_at_ms) as u32;
        if self.frames.last().is_some_and(|f| u64::from(at_ms.saturating_sub(f.at_ms)) < self.gap_ms) {
            return;
        }
        self.frames.push(GhostFrame { at_ms, x, y });
        if self.frames.len() < GHOST_FRAMES {
            return;
        }
        let mut n = 0;
        self.frames.retain(|_| {
            n += 1;
            n % 2 == 1
        });
        let span = u64::from(self.frames.last().map_or(0, |f| f.at_ms));
        self.gap_ms = (span / self.frames.len() as u64).max(self.gap_ms * 2);
    }
}

/// The winner's run, to race against, in world metres.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Ghost {
    pub player: u64,
    pub total_ms: u64,
    pub frames: Vec<GhostFrame>,
}

#[derive(Deserialize)]
struct Move {
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
pub struct Race {
    pub config: Config,
    pub phase: Phase,
    pub racers: Vec<Racer>,
    pub go_at: Option<u64>,
    pub closes_at: Option<u64>,
    pub ends_at: Option<u64>,
    pub ghost: Option<Ghost>,
    /// Events since they were last published.
    #[serde(skip)]
    pub events: Vec<Value>,
    /// Whether the view changed since it was last published.
    #[serde(skip)]
    pub dirty: bool,
}

/// Whether both numbers are finite, also as the `f32`s the timer works in.
fn finite(p: (f64, f64)) -> bool {
    (p.0 as f32).is_finite() && (p.1 as f32).is_finite()
}

/// How far along the segment `a` to `b`, from 0 to 1, the point nearest `c` is.
fn along(a: (f64, f64), b: (f64, f64), c: (f64, f64)) -> f64 {
    let (dx, dy) = (b.0 - a.0, b.1 - a.1);
    let length_sq = dx * dx + dy * dy;
    if length_sq == 0.0 {
        return 1.0;
    }
    (((c.0 - a.0) * dx + (c.1 - a.1) * dy) / length_sq).clamp(0.0, 1.0)
}

/// An id for a leaderboard submission that fits its length limit however long the race's key:
/// the key's 64-bit FNV-1a hash, then the player.
fn board_action_id(key: &str, player: u64) -> String {
    let hash = key.bytes().fold(0xcbf2_9ce4_8422_2325_u64, |h, b| (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3));
    format!("{hash:016x}:{player}")
}

impl Race {
    pub fn view(&self) -> RaceView {
        let line = self.start_line();
        RaceView {
            phase: self.phase,
            course_id: self.config.course_id.clone(),
            gates: self.config.gates.clone(),
            laps: self.config.laps,
            go_at: self.go_at,
            closes_at: self.closes_at,
            ends_at: self.ends_at.filter(|_| self.phase == Phase::Racing),
            racers: self
                .racers
                .iter()
                .map(|r| {
                    let (x, y) = r.warden.position().unwrap_or(line);
                    RacerView {
                        player: r.player,
                        x,
                        y,
                        lap: r.timer.lap,
                        next_gate: r.timer.next_gate,
                        best_lap_ms: (r.timer.best_lap_ms > 0).then_some(r.timer.best_lap_ms),
                        place: r.place,
                        total_ms: r.total_ms,
                        did_not_finish: r.did_not_finish,
                    }
                })
                .collect(),
        }
    }

    fn event(&mut self, kind: &str, extra: Value) {
        let mut e = json!({ "type": kind });
        if let (Some(e), Some(x)) = (e.as_object_mut(), extra.as_object()) {
            e.extend(x.clone());
        }
        self.events.push(e);
    }

    fn index(&self, player: u64) -> Result<usize> {
        self.racers.iter().position(|r| r.player == player).ok_or_else(|| kit::error(codes::NOT_MEMBER, "join the race first"))
    }

    fn start_line(&self) -> (f64, f64) {
        self.config.gates.first().map_or((0.0, 0.0), |g| (g.x, g.y))
    }

    pub fn join(&mut self, player: u64, now: u64) -> Result<()> {
        if let Some(r) = self.racers.iter().find(|r| r.player == player) {
            return if r.did_not_finish { Err(kit::error(codes::STARTED, "the race is under way")) } else { Ok(()) };
        }
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the race is under way"));
        }
        if self.racers.len() >= self.config.racers_max {
            return Err(kit::error(codes::FULL, "the grid is full"));
        }
        self.racers.push(Racer {
            player,
            timer: RunTimer::default(),
            warden: Warden::at(self.start_line(), now),
            ghost: Trace::default(),
            place: None,
            total_ms: None,
            did_not_finish: false,
            moved_at: now,
        });
        self.dirty = true;
        Ok(())
    }

    /// In the lobby the racer is removed; once the countdown has begun they retire.
    pub fn leave(&mut self, player: u64) {
        let Ok(i) = self.index(player) else { return };
        match self.phase {
            Phase::Lobby => {
                self.racers.remove(i);
                self.dirty = true;
            }
            Phase::Countdown | Phase::Racing => {
                if self.racers[i].place.is_none() && !self.racers[i].did_not_finish {
                    self.retire(i, "left");
                    self.settle();
                }
            }
            Phase::Finished => {}
        }
    }

    fn retire(&mut self, i: usize, why: &str) {
        self.racers[i].did_not_finish = true;
        let player = self.racers[i].player;
        self.event("retired", json!({ "player": player, "why": why }));
        self.dirty = true;
    }

    pub fn start(&mut self, now: u64) -> Result<()> {
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the race is under way"));
        }
        if self.racers.is_empty() || self.config.gates.is_empty() {
            return Err(kit::error(codes::NOT_READY, "a race needs racers and a course"));
        }
        let line = self.start_line();
        for r in &mut self.racers {
            r.warden.place(line, now);
            r.moved_at = now;
        }
        self.phase = Phase::Countdown;
        self.go_at = Some(now + self.config.countdown_ms);
        self.dirty = true;
        Ok(())
    }

    /// The start: every racer on the line starts the clock now.
    pub fn go(&mut self, now: u64) {
        if self.phase != Phase::Countdown {
            return;
        }
        self.phase = Phase::Racing;
        self.ends_at = (self.config.time_limit_ms > 0).then(|| now + self.config.time_limit_ms);
        self.event("go", json!({ "at": now }));
        let line = self.start_line();
        for i in 0..self.racers.len() {
            self.racers[i].moved_at = now;
            self.racers[i].warden.place(line, now);
            self.feed(i, line, line, now, now);
        }
        self.dirty = true;
    }

    /// A reported position in world metres: the warden keeps what the rules allow. Returns the
    /// position kept.
    pub fn moved(&mut self, player: u64, to: (f64, f64), now: u64) -> Result<(f64, f64)> {
        if !finite(to) {
            return Err(kit::error(codes::BAD_REQUEST, "a position is a number of metres"));
        }
        let i = self.index(player)?;
        let since = std::mem::replace(&mut self.racers[i].moved_at, now);
        if self.phase == Phase::Countdown {
            let line = self.start_line();
            self.racers[i].warden.place(line, now);
            return Ok(line);
        }
        let from = self.racers[i].warden.position().unwrap_or(to);
        let verdict = self.racers[i].warden.check(&self.config.rules, to, now);
        if let Some(v) = &verdict.violation {
            self.event("violation", json!({ "player": player, "what": v.kind.as_str(), "detail": v.detail }));
        }
        if self.phase == Phase::Racing {
            self.feed(i, from, verdict.at, since, now);
        }
        self.dirty = true;
        Ok(verdict.at)
    }

    /// Feeds the timer the path from `from` at `since` to `to` at `now`: for each next gate, the
    /// point of the path nearest it, timed by its place along the path, for as long as a gate
    /// is passed.
    fn feed(&mut self, i: usize, from: (f64, f64), to: (f64, f64), since: u64, now: u64) {
        let r = &self.racers[i];
        if r.place.is_some() || r.did_not_finish || self.config.gates.is_empty() {
            return;
        }
        if r.timer.started_at_ms > 0 {
            self.racers[i].ghost.record(now, to.0, to.1);
        }
        let course = self.config.course();
        let mut done = 0.0;
        for _ in 0..=course.gates.len() {
            let gate = self.config.gates[self.racers[i].timer.next_gate];
            let f = along(from, to, (gate.x, gate.y)).max(done);
            let pass = (from.0 + (to.0 - from.0) * f, from.1 + (to.1 - from.1) * f);
            let when = since + (now.saturating_sub(since) as f64 * f) as u64;
            if !self.sample(i, &course, pass, when) {
                break;
            }
            done = f;
        }
        self.settle();
    }

    /// One point to the timer. Returns whether to go on to the next gate: it passed one, and did
    /// not complete a lap of a one-gate course, whose next gate is the same.
    fn sample(&mut self, i: usize, course: &Course, at: (f64, f64), now: u64) -> bool {
        let events = self.racers[i].timer.feed(course, (at.0 as f32, at.1 as f32), now);
        let player = self.racers[i].player;
        let passed = events.iter().any(|e| !matches!(e, TimingEvent::Started { .. }));
        let lapped = events.iter().any(|e| matches!(e, TimingEvent::LapCompleted { .. }));
        for e in events {
            let (stage, checkpoint, lap, ms) = match e {
                TimingEvent::Started { .. } => {
                    let r = &mut self.racers[i];
                    r.ghost = Trace::default();
                    r.ghost.record(now, at.0, at.1);
                    ("started", 0, 1, 0)
                }
                TimingEvent::Checkpoint { gate, split_ms } => ("checkpoint", gate as u32, self.racers[i].timer.lap, split_ms),
                TimingEvent::LapCompleted { lap, lap_ms, .. } => ("lap", 0, lap, lap_ms),
                TimingEvent::Finished { total_ms, .. } => {
                    self.finish(i, total_ms, now);
                    ("finished", 0, self.config.laps, total_ms)
                }
            };
            let body = json!({ "stage": stage, "player": player, "checkpoint": checkpoint, "lap": lap, "elapsed_ms": ms });
            self.event("timing", body);
        }
        passed && !(lapped && course.gates.len() == 1)
    }

    fn finish(&mut self, i: usize, total_ms: u64, now: u64) {
        let place = self.racers.iter().filter(|r| r.place.is_some()).count() as u32 + 1;
        let r = &mut self.racers[i];
        r.place = Some(place);
        r.total_ms = Some(total_ms);
        if place == 1 {
            self.ghost = Some(Ghost { player: r.player, total_ms, frames: r.ghost.frames.clone() });
            self.closes_at = Some(now + self.config.finish_grace_ms);
        }
    }

    /// Closes the race once every racer has finished or retired.
    fn settle(&mut self) {
        let running = matches!(self.phase, Phase::Countdown | Phase::Racing);
        if running && self.racers.iter().all(|r| r.place.is_some() || r.did_not_finish) {
            self.close();
        }
    }

    /// Ends the race: whoever has not finished did not.
    pub fn close(&mut self) {
        if self.phase == Phase::Finished {
            return;
        }
        self.phase = Phase::Finished;
        for r in self.racers.iter_mut().filter(|r| r.place.is_none()) {
            r.did_not_finish = true;
        }
        let mut order: Vec<&Racer> = self.racers.iter().collect();
        order.sort_by_key(|r| r.place.unwrap_or(u32::MAX));
        let results: Vec<Value> = order.iter().map(|r| json!({ "player": r.player, "place": r.place, "total_ms": r.total_ms })).collect();
        self.event("finished", json!({ "results": results }));
        self.dirty = true;
    }

    /// One tick: idle racers, the grace and the time limit. Returns whether the race is still on.
    pub fn tick(&mut self, now: u64) -> bool {
        if !matches!(self.phase, Phase::Countdown | Phase::Racing) {
            return false;
        }
        if self.phase == Phase::Racing {
            self.retire_the_idle(now);
            let due = |t: Option<u64>| t.is_some_and(|t| now >= t);
            if due(self.closes_at) || due(self.ends_at) {
                self.close();
            }
        }
        self.phase != Phase::Finished
    }

    /// Retires the racers who have not finished and have sent no `move` for `idle_ms`.
    fn retire_the_idle(&mut self, now: u64) {
        let idle_ms = self.config.idle_ms;
        if idle_ms == 0 {
            return;
        }
        for i in 0..self.racers.len() {
            let r = &self.racers[i];
            if r.place.is_none() && !r.did_not_finish && now.saturating_sub(r.moved_at) >= idle_ms {
                self.retire(i, "idle");
            }
        }
        self.settle();
    }
}

/// The hub: the race, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct RaceHub {
    #[serde(flatten)]
    pub race: Race,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl RaceHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let r = &mut self.race;
        let out = match call.method {
            "join" => {
                r.join(call.player()?, now)?;
                encode(&true)
            }
            "leave" => {
                r.leave(call.player()?);
                encode(&true)
            }
            "start" => {
                let allowed = match call.caller {
                    Caller::Developer(_) => true,
                    Caller::Player(p) => r.racers.first().map(|r| r.player) == Some(p),
                    _ => false,
                };
                if !allowed {
                    return Err(kit::error(codes::FORBIDDEN, "the first to join, or a developer, starts it"));
                }
                r.start(now)?;
                ctx.timer_after("go", r.config.countdown_ms.max(10))?;
                ctx.timer_every("tick", TICK_MS)?;
                encode(&true)
            }
            "move" => {
                let m: Move = kit::args(call)?;
                let kept = r.moved(call.player()?, (m.x, m.y), now);
                self.flush(ctx, false)?;
                let (x, y) = kept?;
                return encode(&Kept { x, y });
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.flush(ctx, true)?;
        out
    }

    /// Publishes the events since the last time, and the race when `snapshot` says so or it has
    /// just closed.
    fn flush(&mut self, ctx: &Ctx, snapshot: bool) -> Result<()> {
        let events = std::mem::take(&mut self.race.events);
        let closed = events.iter().any(|e| e["type"] == "finished");
        for e in &events {
            self.kit.event(ctx, TOPIC, e)?;
            self.realtime(ctx, e);
        }
        if closed {
            ctx.cancel_timer("go");
            ctx.cancel_timer("tick");
            self.post(ctx);
            ctx.persist_now();
        }
        if snapshot || closed {
            self.race.dirty = false;
            self.kit.snapshot(ctx, TOPIC, &self.race.view())?;
        }
        Ok(())
    }

    /// The finishers' times, to the board.
    fn post(&self, ctx: &Ctx) {
        let Some(board) = &self.race.config.board else { return };
        for r in &self.race.racers {
            let Some(ms) = r.total_ms else { continue };
            let submit = json!({ "player": r.player, "score": ms, "action_id": board_action_id(&ctx.key, r.player) });
            let sent = encode(&submit).map_err(|Error(e)| e).and_then(|b| ctx.send("leaderboard", board, "submit", &b).map_err(|e| e.message));
            if let Err(why) = sent {
                ctx.log(Level::Warn, &format!("a time did not reach board {board}: {why}"));
            }
        }
    }

    /// Timing as a realtime event, when the race is realtime.
    fn realtime(&self, ctx: &Ctx, e: &Value) {
        if !self.race.config.realtime || e["type"] != "timing" {
            return;
        }
        let body = wire::RaceTiming {
            kind: e["stage"].as_str().unwrap_or_default().into(),
            course_id: self.race.config.course_id.clone(),
            user_id: e["player"].as_u64().unwrap_or(0).to_string(),
            checkpoint: e["checkpoint"].as_u64().unwrap_or(0) as u32,
            lap: e["lap"].as_u64().unwrap_or(0) as u32,
            elapsed_ms: e["elapsed_ms"].as_u64().unwrap_or(0),
        };
        let actor = actor_id(&format!("race:{}", ctx.key));
        if let Err(Error(why)) = wire::emit_event(ctx, self.race.config.start_chunk(), self.race.config.reach(), &actor, wire::RACE_TIMING, &body) {
            ctx.log(Level::Warn, &format!("a timing event did not go out: {why}"));
        }
    }
}

impl Hub for RaceHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut hub = Self::default();
        if !seed.is_empty() {
            hub.race.config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
        }
        Ok(hub)
    }

    fn load(ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        let mut hub: Self = decode(snapshot)?;
        let now = ctx.now_ms();
        for r in &mut hub.race.racers {
            r.moved_at = now;
        }
        Ok(hub)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        match call.method {
            "state" => encode(&self.race.view()),
            "ghost" => encode(&self.race.ghost),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.race.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        let now = ctx.now_ms();
        match timer {
            "go" => {
                self.race.go(now);
                self.flush(ctx, true)
            }
            "tick" => {
                if !self.race.tick(now) {
                    ctx.cancel_timer("tick");
                }
                let due = self.race.dirty;
                self.flush(ctx, due)
            }
            _ => Ok(()),
        }
    }
}

ckx_sdk::export_hub!(RaceHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{MessageKind, error_code};
    use ckx_sdk::testing::TestHub;

    fn code(e: &CallError) -> Option<&str> {
        error_code(&e.message).map(|c| c.0)
    }

    fn refused<T: std::fmt::Debug>(r: Result<T>) -> Option<String> {
        let Error(e) = r.unwrap_err();
        error_code(&e).map(|c| c.0.to_owned())
    }

    fn gate(x: f64, y: f64, radius: f64) -> Gate {
        Gate { x, y, radius }
    }

    fn race_on(gates: Vec<Gate>, laps: u32) -> Race {
        Race { config: Config { gates, laps, ..Config::default() }, ..Default::default() }
    }

    /// `racers` racers on a course of one gate, a lap long, after the go: any report on the line
    /// finishes it.
    fn sprint(racers: u64) -> Race {
        let mut r = race_on(vec![gate(0.0, 0.0, 3.0)], 1);
        for p in 1..=racers {
            r.join(p, 0).unwrap();
        }
        r.start(0).unwrap();
        r.go(3_000);
        r
    }

    /// The reports from `from` to `to`, about 4 m apart, the last on `to`.
    fn leg(from: (f64, f64), to: (f64, f64)) -> Vec<(f64, f64)> {
        let n = (dist(from, to) / 4.0).ceil().max(1.0) as usize;
        (1..=n).map(|k| (from.0 + (to.0 - from.0) * k as f64 / n as f64, from.1 + (to.1 - from.1) * k as f64 / n as f64)).collect()
    }

    fn dist(a: (f64, f64), b: (f64, f64)) -> f64 {
        ((a.0 - b.0).powi(2) + (a.1 - b.1).powi(2)).sqrt()
    }

    /// One lap of the default course, as reports.
    fn lap_path() -> Vec<(f64, f64)> {
        [leg((0.0, 0.0), (30.0, 0.0)), leg((30.0, 0.0), (15.0, 25.0)), leg((15.0, 25.0), (0.0, 0.0))].concat()
    }

    /// Reports each position half a second apart.
    fn drive(hub: &mut TestHub<RaceHub>, player: u64, path: &[(f64, f64)]) {
        for &(x, y) in path {
            hub.advance(500).unwrap();
            let _: Kept = hub.call_typed(Caller::Player(player), "move", &json!({ "x": x, "y": y })).unwrap();
        }
    }

    fn view(hub: &mut TestHub<RaceHub>) -> RaceView {
        hub.call_typed(Caller::Player(1), "state", &()).unwrap()
    }

    fn snapshots(hub: &TestHub<RaceHub>) -> usize {
        #[derive(Deserialize)]
        struct K {
            kind: MessageKind,
        }
        hub.published().iter().filter(|(_, b)| decode::<K>(b).unwrap().kind == MessageKind::Snapshot).count()
    }

    fn has_event(r: &Race, kind: &str, key: &str, value: Value) -> bool {
        r.events.iter().any(|e| e["type"] == kind && e[key] == value)
    }

    #[test]
    fn a_race_counts_down_times_gates_in_order_places_finishers_and_keeps_the_winners_ghost() {
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"board":"fastest","finish_grace_ms":5000,"idle_ms":0}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let e = hub.call(Caller::Player(2), "start", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN));
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        let e = hub.call(Caller::Player(3), "join", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::STARTED));
        hub.advance(3_000).unwrap();
        assert_eq!(view(&mut hub).phase, Phase::Racing);
        let path = lap_path();
        drive(&mut hub, 1, &path);
        drive(&mut hub, 1, &path);
        let v = view(&mut hub);
        assert_eq!(v.racers[0].place, Some(1), "{v:?}");
        assert_eq!(v.racers[0].total_ms, Some(2 * path.len() as u64 * 500));
        let ghost: Option<Ghost> = hub.call_typed(Caller::Player(2), "ghost", &()).unwrap();
        let ghost = ghost.expect("the winner's ghost");
        assert_eq!((ghost.player, ghost.frames.len()), (1, 2 * path.len() + 1));
        hub.advance(6_000).unwrap();
        let v = view(&mut hub);
        assert_eq!(v.phase, Phase::Finished);
        assert!(v.racers[1].did_not_finish, "the other racer is given the grace, then did not finish");
        let sent = hub.sent();
        assert_eq!(sent.len(), 1, "only a finisher's time goes to the board");
        assert_eq!((sent[0].node_type.as_str(), sent[0].key.as_str()), ("leaderboard", "fastest"));
        assert!(hub.timers().is_empty());
    }

    #[test]
    fn the_warden_keeps_a_racer_from_cutting_the_course() {
        let mut race = Race::default();
        race.join(1, 0).unwrap();
        race.start(0).unwrap();
        race.go(3_000);
        race.moved(1, (1.0, 0.0), 3_100).unwrap();
        let kept = race.moved(1, (30.0, 0.0), 3_200).unwrap();
        assert_eq!(kept, (1.0, 0.0), "a jump across the course is refused");
        assert_eq!(race.racers[0].timer.next_gate, 1, "and passes no gate");
        assert!(has_event(&race, "violation", "what", json!("teleport")));
    }

    #[test]
    fn positions_gates_and_the_view_are_world_metres() {
        let seed = br#"{"gates":[{"x":1000,"y":-500,"radius":3},{"x":1030,"y":-500,"radius":3}],"laps":1,"idle_ms":0}"#;
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", seed).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let v = view(&mut hub);
        assert_eq!(v.gates, vec![gate(1000.0, -500.0, 3.0), gate(1030.0, -500.0, 3.0)]);
        assert_eq!((v.racers[0].x, v.racers[0].y), (1000.0, -500.0), "placed on the start line");
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(3_500).unwrap();
        let reply = hub.call(Caller::Player(1), "move", &encode(&json!({ "x": 1002.0, "y": -500.0 })).unwrap()).unwrap();
        let map: Value = decode(&reply).unwrap();
        assert_eq!((map["x"].as_f64(), map["y"].as_f64()), (Some(1002.0), Some(-500.0)));
        assert_eq!(map.as_object().map(|m| m.len()), Some(2), "a map, not an array");
        let v = view(&mut hub);
        assert_eq!((v.racers[0].x, v.racers[0].y, v.racers[0].next_gate), (1002.0, -500.0, 1));
    }

    #[test]
    fn a_position_that_is_not_a_finite_number_is_refused_and_changes_nothing() {
        let mut r = sprint(1);
        let before = (r.racers[0].warden, r.racers[0].moved_at, r.dirty);
        for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 1e39] {
            assert_eq!(refused(r.moved(1, (bad, 1.0), 3_500)).as_deref(), Some(codes::BAD_REQUEST));
            assert_eq!(refused(r.moved(1, (1.0, bad), 3_500)).as_deref(), Some(codes::BAD_REQUEST));
        }
        assert_eq!((r.racers[0].warden, r.racers[0].moved_at, r.dirty), before);
        r.events.clear();
        assert!(r.moved(1, (1.0, 1.0), 3_500).is_ok());
        #[derive(Serialize)]
        struct At {
            x: f64,
            y: f64,
        }
        let mut hub = TestHub::<RaceHub>::spawn("race", "r1").unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(3_500).unwrap();
        let e = hub.call_typed::<_, Kept>(Caller::Player(1), "move", &At { x: f64::NAN, y: 0.0 }).unwrap_err();
        assert_eq!(code(&e), Some(codes::BAD_REQUEST));
        let e = hub.call_typed::<_, Kept>(Caller::Player(1), "move", &json!({ "x": 1.0 })).unwrap_err();
        assert_eq!(code(&e), Some(codes::BAD_REQUEST), "a missing coordinate too");
        let kept: Kept = hub.call_typed(Caller::Player(1), "move", &At { x: 1.0, y: 0.0 }).unwrap();
        assert_eq!(kept, Kept { x: 1.0, y: 0.0 });
    }

    fn rules() -> Rules {
        Rules { teleport: 100.0, ..Config::default().rules }
    }

    /// A warden at the origin that has had its first report and is no longer just placed.
    fn walking(rules: &Rules) -> Warden {
        let mut w = Warden::at((0.0, 0.0), 0);
        assert_eq!(w.check(rules, (0.0, 0.0), 0).violation, None);
        assert!(!w.placed);
        w
    }

    #[test]
    fn an_honest_racer_is_never_cut_at_any_report_rate_or_after_a_late_report() {
        let rules = rules();
        let gaps: [&[u64]; 3] = [&[200], &[50], &[350, 50]];
        for gaps in gaps {
            let mut w = walking(&rules);
            let mut now = 0;
            for n in 0..400 {
                now += gaps[n % gaps.len()];
                let claimed = (12.0 * now as f64 / 1000.0, 0.0);
                let verdict = w.check(&rules, claimed, now);
                assert!(verdict.violation.is_none() && (verdict.at.0 - claimed.0).abs() < 1e-9, "cut at {now} ms with gaps {gaps:?}");
            }
            assert_eq!(w.strikes(now), 0);
        }
    }

    #[test]
    fn the_report_rate_buys_no_speed() {
        let rules = rules();
        for hz in [5, 20] {
            let mut w = walking(&rules);
            let mut kept = (0.0, 0.0);
            for n in 1..=5 * hz {
                let now = n * 1_000 / hz;
                kept = w.check(&rules, (18.0 * now as f64 / 1000.0, 0.0), now).at;
            }
            assert!((kept.0 - 75.0).abs() < 1e-6, "{} m in 5 s at {hz} Hz: 1.25 times 12 m/s is 75", kept.0);
            assert!(w.strikes(5_000) > 0, "a cut of more than a metre is a strike");
        }
    }

    #[test]
    fn a_silence_banks_only_the_cap_and_a_small_cut_is_no_strike() {
        let rules = rules();
        let mut w = walking(&rules);
        let verdict = w.check(&rules, (40.0, 0.0), 2_600);
        let cap = 12.0 * 1.25 * 1.0 + 0.5;
        assert!((verdict.at.0 - cap).abs() < 1e-9, "1.6 s of silence moved {} m", verdict.at.0);
        assert_eq!(verdict.violation.map(|v| v.kind), Some(ViolationKind::Speed));
        let mut w = walking(&rules);
        let jitter = w.check(&rules, (6.8, 0.0), 400);
        assert!((jitter.at.0 - 6.0).abs() < 1e-9 && jitter.violation.is_none() && w.strikes(400) == 0, "{jitter:?}");
        let mut w = walking(&rules);
        assert!(w.check(&rules, (7.6, 0.0), 400).violation.is_some());
        let mut w = walking(&rules);
        assert_eq!(w.check(&rules, (6.0, 0.0), 400).at, (6.0, 0.0));
        assert_eq!(w.check(&rules, (9.0, 0.0), 400).at, (6.0, 0.0), "what was spent is spent");
    }

    #[test]
    fn pressing_against_the_edge_clamps_and_is_not_a_strike() {
        let rules = Rules { bounds: Some((0.0, 0.0, 40.0, 40.0)), ..rules() };
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

    #[test]
    fn a_placement_and_the_countdown_hold_do_not_strike() {
        let mut r = Race::default();
        r.join(1, 0).unwrap();
        let (kept, strikes) = (r.moved(1, (25.0, 0.0), 200).unwrap(), r.racers[0].warden.strikes(200));
        assert_eq!((kept, strikes), ((0.0, 0.0), 0), "a join places on the line");
        r.moved(1, (2.0, 0.0), 400).unwrap();
        assert_eq!(r.view().racers[0].x, 2.0, "the lobby is free to roam");
        r.start(500).unwrap();
        assert_eq!(r.view().racers[0].x, 0.0, "a start places on the line");
        assert_eq!(r.moved(1, (1.0, 0.0), 700).unwrap(), (0.0, 0.0), "no flying start");
        assert_eq!(r.moved(1, (25.0, 0.0), 900).unwrap(), (0.0, 0.0));
        assert_eq!(r.racers[0].warden.strikes(900), 0);
        r.go(3_500);
        assert!(r.racers[0].timer.started_at_ms > 0, "the clock starts on the line");
        assert_eq!(r.moved(1, (25.0, 0.0), 3_700).unwrap(), (0.0, 0.0), "still placed after the go");
        assert_eq!(r.moved(1, (1.0, 0.0), 3_900).unwrap(), (1.0, 0.0), "a report within reach ends the placement");
        assert_eq!(r.racers[0].warden.strikes(3_900), 0);
        assert!(r.events.iter().all(|e| e["type"] != "violation"));
    }

    #[test]
    fn a_gate_passed_between_two_reports_counts() {
        let mut r = race_on(vec![gate(0.0, 0.0, 1.0), gate(30.0, 0.0, 1.0), gate(15.0, 25.0, 1.0)], 1);
        r.join(1, 0).unwrap();
        r.start(0).unwrap();
        r.go(3_000);
        r.racers[0].warden.place((26.5, 1.5), 3_000);
        r.moved(1, (32.5, 1.5), 4_000).unwrap();
        assert_eq!(r.racers[0].timer.next_gate, 1, "a path 1.5 m from the gate's centre misses it");
        r.racers[0].warden.place((26.5, 0.0), 4_000);
        let kept = r.moved(1, (32.5, 0.0), 5_000).unwrap();
        assert_eq!(kept, (32.5, 0.0), "neither report is within the 1 m radius");
        assert_eq!(r.racers[0].timer.next_gate, 2, "the path through the gate counts");
        assert!(has_event(&r, "timing", "stage", json!("checkpoint")));
    }

    #[test]
    fn every_gate_on_the_path_between_two_reports_counts_in_order() {
        let mut r = race_on(vec![gate(0.0, 0.0, 0.5), gate(3.0, 0.0, 0.5), gate(6.0, 0.0, 0.5)], 1);
        r.join(1, 0).unwrap();
        r.start(0).unwrap();
        r.go(3_000);
        r.racers[0].warden.place((7.0, 0.0), 3_000);
        r.moved(1, (1.5, 0.0), 4_000).unwrap();
        assert_eq!(r.racers[0].timer.next_gate, 2, "back along the course only the first gate is passed");
        let mut r = race_on(vec![gate(0.0, 0.0, 0.5), gate(3.0, 0.0, 0.5), gate(6.0, 0.0, 0.5)], 1);
        r.join(1, 0).unwrap();
        r.start(0).unwrap();
        r.go(3_000);
        r.racers[0].warden.place((1.5, 0.0), 3_000);
        r.moved(1, (7.0, 0.0), 4_000).unwrap();
        let checkpoints = r.events.iter().filter(|e| e["type"] == "timing" && e["stage"] == "checkpoint").count();
        assert_eq!((checkpoints, r.racers[0].timer.next_gate), (2, 0), "both gates in one report");
    }

    #[test]
    fn the_go_starts_the_run_and_does_not_also_finish_a_one_gate_lap() {
        let r = sprint(1);
        assert!(r.racers[0].timer.started_at_ms > 0 && r.racers[0].place.is_none() && r.phase == Phase::Racing);
    }

    #[test]
    fn the_time_limit_closes_a_race_nobody_finishes() {
        {
            let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"time_limit_ms":10000}"#).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
            assert_eq!(view(&mut hub).ends_at, None, "not while counting down");
            hub.advance(3_000).unwrap();
            let v = view(&mut hub);
            assert_eq!((v.phase, v.ends_at), (Phase::Racing, Some(v.go_at.unwrap() + 10_000)));
            hub.advance(9_800).unwrap();
            assert_eq!(view(&mut hub).phase, Phase::Racing);
            hub.advance(400).unwrap();
            let v = view(&mut hub);
            assert_eq!((v.phase, v.ends_at), (Phase::Finished, None));
            assert!(v.racers.iter().all(|r| r.did_not_finish && r.place.is_none()));
            assert!(hub.timers().is_empty());
        }
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"time_limit_ms":0,"idle_ms":0}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(700_000).unwrap();
        let v = view(&mut hub);
        assert_eq!((v.phase, v.ends_at), (Phase::Racing, None), "0 never closes it");
    }

    #[test]
    fn a_leaver_and_an_idle_racer_retire_and_the_last_finisher_closes_the_race() {
        let mut r = race_on(vec![gate(0.0, 0.0, 3.0), gate(5.0, 0.0, 1.0)], 1);
        r.config.idle_ms = 1_000;
        for p in 1..=3 {
            r.join(p, 0).unwrap();
        }
        r.start(0).unwrap();
        r.go(3_000);
        r.leave(2);
        assert!(r.racers[1].did_not_finish && has_event(&r, "retired", "why", json!("left")));
        assert_eq!(refused(r.join(2, 3_100)).as_deref(), Some(codes::STARTED), "they cannot join again");
        r.moved(1, (5.0, 0.0), 3_500).unwrap();
        assert!(r.tick(3_900) && !r.racers[2].did_not_finish, "900 ms is not idle");
        assert!(r.tick(4_100));
        assert!(r.racers[2].did_not_finish && has_event(&r, "retired", "why", json!("idle")));
        assert_eq!(r.phase, Phase::Racing, "the one still racing carries on");
        r.moved(1, (0.0, 0.0), 4_200).unwrap();
        assert_eq!(r.phase, Phase::Finished, "the last to finish closes it at once");
        let finished = r.events.iter().find(|e| e["type"] == "finished").unwrap();
        assert_eq!(finished["results"], json!([{ "player": 1, "place": 1, "total_ms": 1_200 }, { "player": 2, "place": null, "total_ms": null }, { "player": 3, "place": null, "total_ms": null }]));

        let mut r = sprint(2);
        r.moved(1, (0.0, 0.0), 3_100).unwrap();
        assert!(r.phase == Phase::Racing && r.closes_at.is_some(), "the other racer has the grace");
        r.leave(1);
        assert!(r.racers[0].place == Some(1) && !r.racers[0].did_not_finish && r.phase == Phase::Racing, "a finisher leaving changes nothing");
        r.leave(2);
        assert_eq!(r.phase, Phase::Finished, "the last to retire closes it at once");

        let mut r = sprint(2);
        r.config.idle_ms = 1_000;
        assert!(!r.tick(4_100), "the tick that retires the last racer closes the race");
        assert!(r.phase == Phase::Finished && r.racers.iter().all(|r| r.did_not_finish));
    }

    #[test]
    fn leaving_the_lobby_removes_the_racer_and_a_non_member_leaves_quietly() {
        let mut r = Race::default();
        r.join(1, 0).unwrap();
        r.join(2, 0).unwrap();
        r.leave(1);
        r.leave(9);
        assert_eq!(r.racers.iter().map(|r| r.player).collect::<Vec<_>>(), vec![2]);
        r.join(1, 0).unwrap();
        assert_eq!(r.racers.len(), 2, "free to join again in the lobby");
    }

    #[test]
    fn join_and_start_follow_the_grid_and_the_course() {
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"racers_max":2}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let e = hub.call(Caller::Player(3), "join", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FULL));
        assert_eq!(view(&mut hub).racers.len(), 2, "joining twice is one place");
        let e = hub.call(Caller::Player(9), "move", &encode(&json!({ "x": 0.0, "y": 0.0 })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::NOT_MEMBER));
        let _: bool = hub.call_typed(Caller::Developer(7), "start", &()).unwrap();
        let e = hub.call(Caller::Player(1), "start", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::STARTED));
        assert_eq!(refused(Race::default().start(0)).as_deref(), Some(codes::NOT_READY));
        let mut empty = race_on(vec![], 1);
        empty.join(1, 0).unwrap();
        assert_eq!(refused(empty.start(0)).as_deref(), Some(codes::NOT_READY), "no gates, no race");
    }

    #[test]
    fn a_stream_of_moves_publishes_no_snapshot_until_the_tick() {
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"idle_ms":0}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let before_start = snapshots(&hub);
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        assert_eq!(snapshots(&hub), before_start + 1, "a start publishes at once");
        hub.advance(2_950).unwrap();
        let before_go = snapshots(&hub);
        hub.advance(50).unwrap();
        assert!(snapshots(&hub) > before_go, "the go publishes at once");
        let base = snapshots(&hub);
        for n in 1..=3 {
            hub.advance(50).unwrap();
            let _: Kept = hub.call_typed(Caller::Player(1), "move", &json!({ "x": 0.5 * n as f64, "y": 0.0 })).unwrap();
        }
        assert_eq!(snapshots(&hub), base, "three reports, no snapshot");
        hub.advance(100).unwrap();
        assert_eq!(snapshots(&hub), base + 1, "the tick sends what changed");
        hub.advance(400).unwrap();
        assert_eq!(snapshots(&hub), base + 1, "and nothing when nothing did");
        assert_eq!(view(&mut hub).racers[0].x, 1.5);
    }

    #[test]
    fn a_move_that_finishes_the_race_publishes_its_events_and_the_close_at_once() {
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"gates":[{"x":0,"y":0,"radius":3}],"laps":1}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(3_050).unwrap();
        let base = snapshots(&hub);
        let _: Kept = hub.call_typed(Caller::Player(1), "move", &json!({ "x": 0.0, "y": 0.0 })).unwrap();
        assert_eq!(snapshots(&hub), base + 1);
        assert_eq!(view(&mut hub).phase, Phase::Finished);
        assert!(hub.timers().is_empty());
    }

    #[test]
    fn the_ghost_frames_are_maps_in_world_metres() {
        let seed = br#"{"gates":[{"x":1000,"y":-500,"radius":3},{"x":1007,"y":-500,"radius":1}],"laps":1}"#;
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", seed).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(3_000).unwrap();
        let nil: Value = decode(&hub.call(Caller::Player(1), "ghost", &encode(&()).unwrap()).unwrap()).unwrap();
        assert!(nil.is_null(), "no ghost before a winner");
        hub.advance(500).unwrap();
        let _: Kept = hub.call_typed(Caller::Player(1), "move", &json!({ "x": 1007.0, "y": -500.0 })).unwrap();
        hub.advance(500).unwrap();
        let _: Kept = hub.call_typed(Caller::Player(1), "move", &json!({ "x": 1000.5, "y": -500.0 })).unwrap();
        let ghost: Value = decode(&hub.call(Caller::Player(1), "ghost", &encode(&()).unwrap()).unwrap()).unwrap();
        assert_eq!((ghost["player"].as_u64(), ghost["total_ms"].as_u64()), (Some(1), Some(1_000)));
        let frames = ghost["frames"].as_array().unwrap();
        assert_eq!(frames.len(), 3);
        let (first, last) = (&frames[0], &frames[2]);
        assert_eq!((first["at_ms"].as_u64(), first["x"].as_f64(), first["y"].as_f64()), (Some(0), Some(1000.0), Some(-500.0)));
        assert_eq!((last["at_ms"].as_u64(), last["x"].as_f64(), last["y"].as_f64()), (Some(1_000), Some(1000.5), Some(-500.0)));
        assert_eq!(last.as_object().map(|m| m.len()), Some(3), "maps of at_ms, x and y");
    }

    #[test]
    fn a_realtime_race_sends_timing_at_the_first_gates_chunk_and_others_send_nothing() {
        let chunk_of = |inject: &[u8]| -> [i64; 3] {
            let at = |n: usize| i64::from_le_bytes(inject[10 + 8 * n..18 + 8 * n].try_into().unwrap());
            [at(0), at(1), at(2)]
        };
        let course = r#""gates":[{"x":70,"y":40,"radius":3},{"x":100,"y":40,"radius":3}],"laps":1"#;
        {
            let seed = format!(r#"{{{course},"realtime":true,"height":40}}"#);
            let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", seed.as_bytes()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
            hub.advance(3_000).unwrap();
            let emitted = hub.emitted();
            assert_eq!(emitted.len(), 1, "the start of the run");
            assert_eq!(chunk_of(&emitted[0]), [4, 2, 2], "first gate (70, 40) m, 40 m up, in 16 m chunks");
        }
        let seed = format!("{{{course}}}");
        let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", seed.as_bytes()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub.advance(3_000).unwrap();
        assert!(hub.emitted().is_empty());
    }

    #[test]
    fn a_racer_silent_through_the_countdown_gets_no_flying_start() {
        let mut r = Race::default();
        r.join(1, 0).unwrap();
        r.start(0).unwrap();
        r.go(3_000);
        assert_eq!(r.moved(1, (6.0, 0.0), 3_005).unwrap(), (0.0, 0.0), "6 m, 5 ms after the go, is not banked");
        assert_eq!(r.racers[0].warden.strikes(3_005), 0);
        assert_eq!(r.moved(1, (1.0, 0.0), 3_205).unwrap(), (1.0, 0.0));
    }

    #[test]
    fn the_results_list_the_finishers_by_place_then_the_rest() {
        let mut r = sprint(3);
        r.moved(2, (0.0, 0.0), 3_100).unwrap();
        r.moved(3, (0.0, 0.0), 3_200).unwrap();
        r.leave(1);
        assert_eq!(r.phase, Phase::Finished);
        let finished = r.events.iter().find(|e| e["type"] == "finished").unwrap();
        let players: Vec<u64> = finished["results"].as_array().unwrap().iter().map(|e| e["player"].as_u64().unwrap()).collect();
        assert_eq!(players, vec![2, 3, 1], "the second to join won");
        assert_eq!(finished["results"][2]["place"], Value::Null);
    }

    #[test]
    fn an_honest_racer_with_a_slow_round_trip_is_kept_in_full() {
        let rules = Config::default().rules;
        let mut w = Warden::at((0.0, 0.0), 0);
        assert_eq!(w.check(&rules, (0.0, 0.0), 0).violation, None);
        for n in 1..=20 {
            let verdict = w.check(&rules, (9.6 * n as f64, 0.0), 800 * n);
            assert!(verdict.violation.is_none() && (verdict.at.0 - 9.6 * n as f64).abs() < 1e-9, "cut at report {n}: {verdict:?}");
        }
        assert_eq!(w.strikes(16_000), 0);
    }

    #[test]
    fn a_racer_already_in_joins_in_any_phase_unless_they_retired() {
        let mut r = Race::default();
        r.join(1, 0).unwrap();
        r.join(2, 0).unwrap();
        r.start(100).unwrap();
        assert!(r.join(1, 200).is_ok(), "a reconnect during the countdown");
        r.go(3_100);
        assert!(r.join(1, 3_200).is_ok(), "and during the race");
        assert_eq!(refused(r.join(3, 3_200)).as_deref(), Some(codes::STARTED), "a newcomer is late");
        r.leave(2);
        assert_eq!(refused(r.join(2, 3_300)).as_deref(), Some(codes::STARTED), "a leaver stays out");
    }

    #[test]
    fn a_lap_of_a_one_gate_course_needs_a_report_of_its_own() {
        let mut r = race_on(vec![gate(0.0, 0.0, 3.0)], 2);
        r.join(1, 0).unwrap();
        r.start(0).unwrap();
        r.go(3_000);
        let stages = |r: &Race| r.events.iter().filter(|e| e["type"] == "timing").map(|e| e["stage"].as_str().unwrap().to_owned()).collect::<Vec<_>>();
        r.events.clear();
        r.moved(1, (0.0, 0.0), 3_100).unwrap();
        assert_eq!(stages(&r), vec!["checkpoint", "lap"]);
        assert!(r.racers[0].place.is_none(), "one report is one lap, not two");
        r.moved(1, (0.0, 0.0), 3_200).unwrap();
        assert_eq!(r.racers[0].place, Some(1));
    }

    #[test]
    fn the_leaderboard_action_id_is_bounded_and_hashes_the_key() {
        assert_eq!(board_action_id("a", 7), "af63dc4c8601ec8c:7", "FNV-1a of the key, then the player");
        assert_eq!(board_action_id("", 1), "cbf29ce484222325:1");
        let key = "k".repeat(60);
        let seed = br#"{"board":"fastest","gates":[{"x":0,"y":0,"radius":3},{"x":5,"y":0,"radius":1}],"laps":1}"#;
        let mut hub = TestHub::<RaceHub>::spawn_with("race", &key, seed).unwrap();
        let _: bool = hub.call_typed(Caller::Player(18_446_744_073_709_551_615), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(18_446_744_073_709_551_615), "start", &()).unwrap();
        hub.advance(3_000).unwrap();
        for x in [5.0, 0.0] {
            hub.advance(500).unwrap();
            let _: Kept = hub.call_typed(Caller::Player(18_446_744_073_709_551_615), "move", &json!({ "x": x, "y": 0.0 })).unwrap();
        }
        let sent = hub.sent();
        assert_eq!(sent.len(), 1);
        let submit: Value = sent[0].decode().unwrap();
        let id = submit["action_id"].as_str().unwrap();
        assert_eq!(id, board_action_id(&key, 18_446_744_073_709_551_615));
        assert!(id.len() <= 64, "{id}");
    }

    #[test]
    fn a_gate_pass_is_timed_where_it_happened_between_two_reports() {
        let mut r = race_on(vec![gate(0.0, 0.0, 1.0), gate(30.0, 0.0, 1.0), gate(15.0, 25.0, 1.0)], 1);
        r.join(1, 0).unwrap();
        r.start(0).unwrap();
        r.go(3_000);
        r.racers[0].warden.place((26.5, 0.0), 3_000);
        r.events.clear();
        r.moved(1, (32.5, 0.0), 4_000).unwrap();
        let split = r.events.iter().find(|e| e["stage"] == "checkpoint").unwrap()["elapsed_ms"].as_u64().unwrap();
        assert!((582..=584).contains(&split), "the centre is 3.5 of the 6 m on, so 583 of the 1000 ms: {split}");
    }

    #[test]
    fn a_five_minute_ghost_keeps_the_whole_run_in_600_frames() {
        let mut trace = Trace::default();
        for n in 0..1_500_u64 {
            trace.record(10_000 + 200 * n, n as f64, 0.0);
        }
        let frames = &trace.frames;
        assert!(frames.len() <= GHOST_FRAMES && frames.len() >= GHOST_FRAMES / 4, "{} frames", frames.len());
        assert_eq!(frames[0].at_ms, 0);
        let last = frames.last().unwrap().at_ms;
        assert!(299_800 - u64::from(last) <= trace.gap_ms, "ends at {last} ms with a gap of {}", trace.gap_ms);
        let widest = frames.windows(2).map(|w| w[1].at_ms - w[0].at_ms).max().unwrap();
        assert!(u64::from(widest) <= 2 * trace.gap_ms, "frames are spread across the run, widest {widest} ms");
    }

    #[test]
    fn a_timing_event_reaches_out_to_the_farthest_gate_and_no_further_than_the_tier_allows() {
        let reach_of = |course: &str| -> u8 {
            let seed = format!(r#"{{{course},"laps":1,"realtime":true}}"#);
            let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", seed.as_bytes()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
            hub.advance(3_000).unwrap();
            hub.emitted()[0][34]
        };
        let gates = |far: u32| format!(r#""gates":[{{"x":70,"y":40,"radius":3}},{{"x":{far},"y":40,"radius":3}}]"#);
        assert_eq!(reach_of(&gates(150)), 6, "chunk 4 to chunk 9 is 5, and one more");
        assert_eq!(reach_of(&gates(100)), 4, "never less than 4");
        assert_eq!(reach_of(&gates(1_000)), 8, "the realtime tier's most");
        assert_eq!(reach_of(&gates(4_144)), 8, "a course so long its reach would wrap a byte");
    }

    #[test]
    fn a_loaded_snapshot_restarts_the_idle_clock() {
        let snapshot = {
            let mut hub = TestHub::<RaceHub>::spawn_with("race", "r1", br#"{"idle_ms":1000}"#).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
            hub.hub_mut().race.racers.iter_mut().for_each(|r| r.moved_at = 0);
            hub.persist().unwrap()
        };
        let mut hub = TestHub::<RaceHub>::load("race", "r1", &snapshot, 1).unwrap();
        let now = hub.now_ms();
        assert!(hub.hub().race.racers.iter().all(|r| r.moved_at == now));
        hub.hub_mut().race.go(now);
        assert!(hub.hub_mut().race.tick(now + 100));
        assert!(!hub.hub().race.racers[0].did_not_finish);
    }
}
