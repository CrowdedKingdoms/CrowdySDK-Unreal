//! One game session, keyed by its id, from lobby to result. Players join the lobby; the host
//! (the first to join, then the next connected player if the host goes) starts once everyone
//! is ready; turns go round with a time limit, and a player who runs out of time is skipped;
//! the hub decides each move's points and the winner. A matchmaker can reserve the session for
//! its players first. Replaces the `match-engine` template.
//!
//! Endpoints (players): `join`, `leave`, `ready { ready }`, `start` (host), `act { points }`
//! (the player whose turn it is), `state` (anyone). `configure { .. }` (the host in the lobby,
//! or a developer) and `reserve { players }` (another instance, or a developer). It speaks the
//! kit protocol (`ckx_sdk::kit`): every change is a `snapshot` message on the `session` topic,
//! `snapshot { topic: "session" }` answers the same, a call that changes something may carry
//! an `action_id`, and refusals carry `kit:<code>:` codes.
//!
//! The spawn seed, if any, is the JSON config. With a `board`, a finished session posts each
//! player's score to that `leaderboard` (the manifest lets `session` call `leaderboard`); only
//! the seed sets it, so a host's `configure` cannot send scores elsewhere.
//!
//! [`points_for`] is the game's rule; replace it with yours.

use std::collections::{BTreeMap, BTreeSet, VecDeque};

use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;

const LOG_MOVES: usize = 20;
pub const TOPIC: &str = "session";

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    pub min_players: usize,
    pub max_players: usize,
    pub turn_ms: u64,
    pub target_score: i64,
    pub max_rounds: u32,
    /// A lobby nobody joins closes after this long.
    pub lobby_idle_ms: u64,
    /// The `leaderboard` key a finished session posts its scores to.
    pub board: Option<String>,
}

impl Default for Config {
    fn default() -> Self {
        Self { min_players: 2, max_players: 8, turn_ms: 30_000, target_score: 10, max_rounds: 20, lobby_idle_ms: 600_000, board: None }
    }
}

impl Config {
    fn check(&self) -> Result<()> {
        if self.min_players < 1 || self.max_players < self.min_players || self.max_players > 64 {
            return Err(kit::error(codes::BAD_REQUEST, "1 <= min_players <= max_players <= 64"));
        }
        if self.turn_ms < 1_000 {
            return Err(kit::error(codes::BAD_REQUEST, "turn_ms is at least 1000"));
        }
        Ok(())
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

/// A move as the client sends it. Replace with your game's action.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Action {
    pub points: i64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Move {
    pub player: u64,
    pub round: u32,
    /// None when the player ran out of time.
    pub points: Option<i64>,
}

/// The game's rule for what a move scores. The starter's: 0 to 3 points, as asked.
pub fn points_for(action: &Action) -> Result<i64> {
    if (0..=3).contains(&action.points) { Ok(action.points) } else { Err(kit::error("invalid_move", "a move scores 0 to 3 points")) }
}

#[derive(Deserialize)]
struct Ready {
    ready: bool,
}

#[derive(Deserialize)]
struct Reserve {
    players: Vec<u64>,
}

#[derive(Serialize)]
struct Submit {
    player: u64,
    score: i64,
    action_id: String,
}

/// The session as everyone sees it: what `state` answers and the `session` topic carries.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct GameSession {
    pub config: Config,
    pub phase: Phase,
    /// Only these players may join, when set (a matchmaker's reservation).
    pub reserved: Vec<u64>,
    pub players: Vec<u64>,
    pub ready: BTreeSet<u64>,
    pub connected: BTreeSet<u64>,
    pub host: Option<u64>,
    pub turn: usize,
    pub round: u32,
    pub scores: BTreeMap<u64, i64>,
    pub winner: Option<u64>,
    pub moves: VecDeque<Move>,
}

impl GameSession {
    /// Whose turn it is, while the session plays.
    pub fn now_turn(&self) -> Option<u64> {
        (self.phase == Phase::Playing).then(|| self.players.get(self.turn).copied()).flatten()
    }

    pub fn join(&mut self, player: u64) -> Result<()> {
        if self.players.contains(&player) {
            return Ok(());
        }
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the session has started"));
        }
        if !self.reserved.is_empty() && !self.reserved.contains(&player) {
            return Err(kit::error(codes::RESERVED, "this session is reserved for other players"));
        }
        if self.players.len() >= self.config.max_players {
            return Err(kit::error(codes::FULL, "the session is full"));
        }
        self.players.push(player);
        self.scores.insert(player, 0);
        self.host.get_or_insert(player);
        Ok(())
    }

    /// Returns whether the turn timer needs re-arming.
    pub fn leave(&mut self, player: u64) -> bool {
        let Some(i) = self.players.iter().position(|p| *p == player) else { return false };
        let was_turn = self.now_turn() == Some(player);
        self.players.remove(i);
        self.ready.remove(&player);
        if self.phase == Phase::Lobby {
            self.scores.remove(&player);
        }
        if self.host == Some(player) {
            self.host = self.players.iter().find(|p| self.connected.contains(p)).or(self.players.first()).copied();
        }
        if self.phase != Phase::Playing {
            return false;
        }
        if self.players.len() < self.config.min_players {
            self.finish();
            return false;
        }
        if i < self.turn {
            self.turn -= 1;
        }
        if self.turn >= self.players.len() {
            self.turn = 0;
            self.round += 1;
        }
        was_turn
    }

    pub fn start(&mut self, player: u64) -> Result<()> {
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the session has started"));
        }
        if self.host != Some(player) {
            return Err(kit::error(codes::FORBIDDEN, "only the host starts the session"));
        }
        if self.players.len() < self.config.min_players {
            return Err(kit::error("too_few_players", format!("at least {} players are needed", self.config.min_players)));
        }
        if self.players.iter().any(|p| !self.ready.contains(p)) {
            return Err(kit::error(codes::NOT_READY, "everyone must be ready"));
        }
        self.phase = Phase::Playing;
        self.turn = 0;
        self.round = 1;
        Ok(())
    }

    /// The current player's move, or a timeout (`action` None). Returns whether play goes on.
    pub fn play(&mut self, player: u64, action: Option<&Action>) -> Result<bool> {
        if self.now_turn() != Some(player) {
            return Err(kit::error(codes::NOT_YOUR_TURN, "it is not your turn"));
        }
        let points = action.map(points_for).transpose()?;
        let score = self.scores.entry(player).or_insert(0);
        *score += points.unwrap_or(0);
        let reached = *score >= self.config.target_score;
        self.moves.push_back(Move { player, round: self.round, points });
        while self.moves.len() > LOG_MOVES {
            self.moves.pop_front();
        }
        self.turn += 1;
        if self.turn >= self.players.len() {
            self.turn = 0;
            self.round += 1;
        }
        if reached || self.round > self.config.max_rounds {
            self.finish();
            return Ok(false);
        }
        Ok(true)
    }

    fn finish(&mut self) {
        self.phase = Phase::Finished;
        // The highest score; the earlier player wins a tie.
        self.winner = self.players.iter().copied().max_by_key(|p| (self.scores.get(p).copied().unwrap_or(0), std::cmp::Reverse(*p)));
    }

    fn arm_turn(&self, ctx: &Ctx) -> Result<()> {
        if self.phase == Phase::Playing {
            ctx.timer_after("turn", self.config.turn_ms)
        } else {
            ctx.cancel_timer("turn");
            Ok(())
        }
    }

    fn set_ready(&mut self, player: u64, ready: bool) -> Result<bool> {
        if self.phase != Phase::Lobby || !self.players.contains(&player) {
            return Err(kit::error(codes::NOT_MEMBER, "join the lobby first"));
        }
        if ready {
            self.ready.insert(player);
        } else {
            self.ready.remove(&player);
        }
        Ok(ready)
    }

    fn configure(&mut self, caller: Caller, config: Config) -> Result<()> {
        let allowed = match caller {
            Caller::Developer(_) => true,
            Caller::Player(p) => self.host == Some(p),
            _ => false,
        };
        if !allowed || self.phase != Phase::Lobby {
            return Err(kit::error(codes::FORBIDDEN, "the host configures the session in the lobby"));
        }
        config.check()?;
        let board = self.config.board.take();
        self.config = Config { board, ..config };
        Ok(())
    }

    fn reserve(&mut self, caller: Caller, players: Vec<u64>) -> Result<()> {
        if !matches!(caller, Caller::Instance(_) | Caller::Developer(_)) {
            return Err(kit::error(codes::FORBIDDEN, "reserve is for the matchmaker"));
        }
        if self.phase != Phase::Lobby || !self.players.is_empty() {
            return Err(kit::error(codes::CONFLICT, "the session is already in use"));
        }
        if players.is_empty() || players.len() > self.config.max_players {
            return Err(kit::error(codes::BAD_REQUEST, "reserve 1 to max_players players"));
        }
        self.reserved = players;
        Ok(())
    }
}

/// The hub: the session, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct SessionHub {
    #[serde(flatten)]
    pub game: GameSession,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl SessionHub {
    /// Each player's score, to the board, once the session has just finished.
    fn post(&self, ctx: &Ctx, before: Phase) {
        if before == Phase::Finished || self.game.phase != Phase::Finished {
            return;
        }
        let Some(board) = self.game.config.board.as_deref().filter(|b| !b.is_empty()) else { return };
        for (player, score) in &self.game.scores {
            // The board keeps receipts per calling instance, so the player alone keeps the id unique and under 64 bytes.
            let submit = Submit { player: *player, score: *score, action_id: format!("score:{player}") };
            let sent = encode(&submit).map_err(|Error(e)| e).and_then(|b| ctx.send("leaderboard", board, "submit", &b).map_err(|e| e.message));
            if let Err(why) = sent {
                ctx.log(Level::Warn, &format!("a score did not reach board {board}: {why}"));
            }
        }
    }

    fn act(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<Vec<u8>> {
        let before = self.game.phase;
        let game = &mut self.game;
        let out = match call.method {
            "join" => {
                game.join(call.player()?)?;
                ctx.cancel_timer("idle");
                encode(&true)
            }
            "leave" => {
                if game.leave(call.player()?) {
                    game.arm_turn(ctx)?;
                }
                encode(&true)
            }
            "ready" => {
                let req: Ready = kit::args(call)?;
                encode(&game.set_ready(call.player()?, req.ready)?)
            }
            "start" => {
                game.start(call.player()?)?;
                game.arm_turn(ctx)?;
                encode(&true)
            }
            "act" => {
                let player = call.player()?;
                let action: Action = kit::args(call)?;
                let more = game.play(player, Some(&action))?;
                game.arm_turn(ctx)?;
                encode(&more)
            }
            "configure" => {
                game.configure(call.caller, kit::args(call)?)?;
                encode(&true)
            }
            "reserve" => {
                let req: Reserve = kit::args(call)?;
                game.reserve(call.caller, req.players)?;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.post(ctx, before);
        self.kit.snapshot(ctx, TOPIC, &self.game)?;
        ctx.persist_now();
        out
    }
}

impl Hub for SessionHub {
    fn spawn(ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut s = Self::default();
        if !seed.is_empty() {
            s.game.config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
            s.game.config.check()?;
        }
        ctx.timer_after("idle", s.game.config.lobby_idle_ms)?;
        Ok(s)
    }

    /// Reads this version's snapshots and the earlier ones, which held the session alone.
    fn load(_ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        decode(snapshot)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        match call.method {
            "state" => encode(&self.game),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.game),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, ctx.now_ms(), |h| h.act(ctx, &call)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        let before = self.game.phase;
        let game = &mut self.game;
        match timer {
            "turn" => {
                if let Some(player) = game.now_turn() {
                    game.play(player, None)?;
                    game.arm_turn(ctx)?;
                }
            }
            "idle" if game.phase == Phase::Lobby && game.players.is_empty() => game.phase = Phase::Finished,
            _ => return Ok(()),
        }
        self.post(ctx, before);
        self.kit.snapshot(ctx, TOPIC, &self.game)
    }

    fn on_session(&mut self, ctx: &Ctx, player: u64, session: Session) -> Result<()> {
        let game = &mut self.game;
        match session {
            Session::Joined => {
                game.connected.insert(player);
            }
            Session::Left => {
                game.connected.remove(&player);
                if game.host == Some(player)
                    && let Some(next) = game.players.iter().find(|p| game.connected.contains(p))
                {
                    game.host = Some(*next);
                }
            }
        }
        self.kit.snapshot(ctx, TOPIC, &self.game)
    }
}

ckx_sdk::export_hub!(SessionHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, MessageKind, error_code};
    use ckx_sdk::testing::TestHub;

    fn lobby(players: &[u64]) -> GameSession {
        let mut s = GameSession::default();
        for p in players {
            s.join(*p).unwrap();
            s.ready.insert(*p);
        }
        s
    }

    #[test]
    fn the_first_to_join_hosts_and_only_the_host_starts_when_all_are_ready() {
        let mut s = lobby(&[1, 2]);
        assert_eq!(s.host, Some(1));
        s.ready.remove(&2);
        assert!(s.start(1).is_err(), "not everyone is ready");
        s.ready.insert(2);
        assert!(s.start(2).is_err(), "not the host");
        s.start(1).unwrap();
        assert_eq!((s.phase, s.round, s.now_turn()), (Phase::Playing, 1, Some(1)));
        assert!(s.join(3).is_err(), "started");
    }

    #[test]
    fn turns_go_round_timeouts_skip_and_the_target_wins() {
        let mut s = lobby(&[1, 2]);
        s.config.target_score = 4;
        s.start(1).unwrap();
        assert!(s.play(2, Some(&Action { points: 1 })).is_err(), "not your turn");
        assert!(s.play(1, Some(&Action { points: 9 })).is_err(), "the rule refuses it");
        assert!(s.play(1, Some(&Action { points: 3 })).unwrap());
        assert!(s.play(2, None).unwrap(), "a timeout scores nothing");
        assert_eq!(s.round, 2);
        assert!(!s.play(1, Some(&Action { points: 1 })).unwrap());
        assert_eq!((s.phase, s.winner), (Phase::Finished, Some(1)));
        assert_eq!(s.moves.iter().filter(|m| m.points.is_none()).count(), 1);
    }

    #[test]
    fn a_reserved_session_admits_only_its_players() {
        let mut s = GameSession { reserved: vec![1, 2], ..Default::default() };
        let Error(why) = s.join(3).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::RESERVED));
        assert!(why.contains("reserved"));
        s.join(2).unwrap();
        assert_eq!(s.host, Some(2));
    }

    #[test]
    fn leaving_hands_on_the_host_and_the_turn_and_too_few_players_ends_it() {
        let mut s = lobby(&[1, 2, 3]);
        s.connected.extend([2, 3]);
        s.start(1).unwrap();
        assert!(s.leave(1), "it was their turn");
        assert_eq!((s.host, s.now_turn()), (Some(2), Some(2)));
        s.scores.insert(3, 5);
        assert!(!s.leave(2));
        assert_eq!((s.phase, s.winner), (Phase::Finished, Some(3)));
    }

    #[derive(Serialize)]
    struct Act<'a> {
        points: i64,
        action_id: &'a str,
    }

    #[test]
    fn every_change_is_a_snapshot_message_and_an_action_sent_again_runs_once() {
        let mut hub = TestHub::<SessionHub>::spawn("session", "s1").unwrap();
        for p in [1, 2] {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(p), "ready", &Ready2 { ready: true }).unwrap();
        }
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        let more: bool = hub.call_typed(Caller::Player(1), "act", &Act { points: 3, action_id: "m1" }).unwrap();
        assert!(more);
        // The same action again (a lost reply): answered from the receipt, not played twice.
        let again: bool = hub.call_typed(Caller::Player(1), "act", &Act { points: 3, action_id: "m1" }).unwrap();
        assert!(again);
        assert_eq!(hub.hub().game.scores[&1], 3);
        assert_eq!(hub.hub().game.now_turn(), Some(2));

        let published = hub.published();
        assert!(published.iter().all(|(t, _)| t == TOPIC));
        let seqs: Vec<u64> = published.iter().map(|(_, b)| decode::<Message<PhaseOnly>>(b).unwrap().header.seq).collect();
        assert_eq!(seqs, (1..=seqs.len() as u64).collect::<Vec<_>>(), "one message a change, in order");
        let last: Message<PhaseOnly> = decode(&published.last().unwrap().1).unwrap();
        assert_eq!(last.header.kind, MessageKind::Snapshot);
        assert_eq!(last.body.phase, Phase::Playing);

        let snap: Message<PhaseOnly> = hub.call_typed(Caller::Player(2), "snapshot", &Topic { topic: TOPIC }).unwrap();
        assert_eq!((snap.header.seq, snap.header.kind), (seqs.len() as u64, MessageKind::Snapshot));

        let e = hub.call(Caller::Player(1), "act", &encode(&Act { points: 1, action_id: "m2" }).unwrap()).unwrap_err();
        assert_eq!(error_code(&e.message).map(|c| c.0), Some(codes::NOT_YOUR_TURN));
    }

    #[derive(Deserialize, Debug, PartialEq)]
    struct Posted {
        player: u64,
        score: i64,
        action_id: String,
    }

    fn play_to_the_end(hub: &mut TestHub<SessionHub>) {
        for p in [1, 2] {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(p), "ready", &Ready2 { ready: true }).unwrap();
        }
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        for (i, p) in [1, 2, 1, 2, 1, 2, 1].into_iter().enumerate() {
            let id = format!("m{i}");
            let _: bool = hub.call_typed(Caller::Player(p), "act", &Act { points: if p == 1 { 3 } else { 1 }, action_id: &id }).unwrap();
        }
        assert_eq!(hub.hub().game.phase, Phase::Finished);
        // The finishing move sent again is answered from its receipt, and a change after the end posts nothing more.
        let _: bool = hub.call_typed(Caller::Player(1), "act", &Act { points: 3, action_id: "m6" }).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "leave", &()).unwrap();
    }

    #[test]
    fn a_finished_session_posts_each_score_once_to_its_seeded_board() {
        let mut hub = TestHub::<SessionHub>::spawn_with("session", "s9", br#"{"board":"weekly"}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let config = serde_json::json!({ "min_players": 2, "max_players": 8, "turn_ms": 30000, "target_score": 10, "max_rounds": 20, "lobby_idle_ms": 600000, "board": "elsewhere" });
        let _: bool = hub.call_typed(Caller::Player(1), "configure", &config).unwrap();
        assert_eq!(hub.hub().game.config.board.as_deref(), Some("weekly"), "a host's configure keeps the seeded board");
        let _: bool = hub.call_typed(Caller::Player(1), "leave", &()).unwrap();
        assert!(hub.sent().is_empty(), "nothing is posted before the end");
        play_to_the_end(&mut hub);
        let sent = hub.sent();
        assert!(sent.iter().all(|m| (m.node_type.as_str(), m.key.as_str(), m.method.as_str()) == ("leaderboard", "weekly", "submit")));
        let posted: Vec<Posted> = sent.iter().map(|m| decode(&m.payload).unwrap()).collect();
        assert_eq!(posted, vec![
            Posted { player: 1, score: 12, action_id: "score:1".into() },
            Posted { player: 2, score: 3, action_id: "score:2".into() },
        ]);
    }

    #[test]
    fn a_seed_is_checked_as_configure_checks_its_config() {
        assert!(TestHub::<SessionHub>::spawn_with("session", "s3", br#"{"max_players":0}"#).is_err(), "nobody could join");
        assert!(TestHub::<SessionHub>::spawn_with("session", "s3", br#"{"turn_ms":10}"#).is_err());
        assert!(TestHub::<SessionHub>::spawn_with("session", "s3", br#"{"turn_ms":"30000"}"#).is_err(), "not a number");
        assert!(TestHub::<SessionHub>::spawn_with("session", "s3", br#"{"min_players":1,"board":"weekly"}"#).is_ok());
        let mut hub = TestHub::<SessionHub>::spawn("session", "s3").unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let config = serde_json::json!({ "min_players": 1, "max_players": 0, "turn_ms": 30000, "target_score": 10, "max_rounds": 20, "lobby_idle_ms": 600000 });
        let e = hub.call(Caller::Player(1), "configure", &encode(&config).unwrap()).unwrap_err();
        assert_eq!(error_code(&e.message).map(|c| c.0), Some(codes::BAD_REQUEST), "and the host's configure the same way");
    }

    #[test]
    fn a_session_with_an_empty_board_posts_nothing() {
        let mut hub = TestHub::<SessionHub>::spawn_with("session", "s1", br#"{"board":""}"#).unwrap();
        play_to_the_end(&mut hub);
        assert!(hub.sent().is_empty());
    }

    #[test]
    fn a_session_its_turn_timer_ends_posts_too() {
        let mut hub = TestHub::<SessionHub>::spawn_with("session", "s2", br#"{"board":"weekly","max_rounds":1}"#).unwrap();
        for p in [1, 2] {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
            let _: bool = hub.call_typed(Caller::Player(p), "ready", &Ready2 { ready: true }).unwrap();
        }
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "act", &Act { points: 2, action_id: "m0" }).unwrap();
        assert!(hub.sent().is_empty());
        hub.advance(30_000).unwrap();
        assert_eq!(hub.hub().game.phase, Phase::Finished, "player 2 ran out of time in the last round");
        assert_eq!(hub.sent().len(), 2);
    }

    #[test]
    fn a_snapshot_from_before_the_kit_protocol_loads() {
        let old = encode(&lobby(&[4, 5])).unwrap();
        let hub = TestHub::<SessionHub>::load("session", "s1", &old, 1).unwrap();
        assert_eq!(hub.hub().game.players, vec![4, 5]);
        assert!(hub.hub().receipts.is_empty());
    }

    #[derive(Serialize)]
    struct Ready2 {
        ready: bool,
    }

    #[derive(Deserialize)]
    struct PhaseOnly {
        phase: Phase,
    }

    #[derive(Serialize)]
    struct Topic<'a> {
        topic: &'a str,
    }
}
