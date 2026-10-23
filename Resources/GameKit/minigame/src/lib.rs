//! A duel, keyed by its table: two players, a hidden throw each round that the hub keeps until
//! both are in (or the round's time runs out), and the first to win a majority of `best_of`
//! rounds wins. Rock, paper, scissors is the game; replace [`HANDS`] and [`beats`] with yours (a
//! bid, a guess, a sealed move). The round is `ckx_sdk::kit::play::turns::RevealRound`, so
//! nobody sees a throw before both are in.
//!
//! Endpoints (players): `join`, `throw { hand }`, `leave` (before it starts, or once it is over;
//! a player who is not seated is refused `not_member` in every phase, and nothing changes),
//! `rematch` (once it is over), `state` (anyone). It speaks the kit protocol (`ckx_sdk::kit`): the `duel` topic
//! carries the table as `snapshot` messages, which say who has thrown but never what until the
//! reveal; `snapshot { topic: "duel" }` answers it; changes may carry an `action_id`; refusals
//! carry `kit:<code>:` codes. A player who does not throw in time loses the round.
//!
//! This is the Unreal SDK's copy of the platform's starter: a rematch round arms its timer too.

use ckx_sdk::kit::play::turns::RevealRound;
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;

pub const TOPIC: &str = "duel";
pub const HANDS: [&str; 3] = ["rock", "paper", "scissors"];
const ROUND_TIMER: &str = "round";

/// Whether hand `a` beats hand `b`.
pub fn beats(a: &str, b: &str) -> bool {
    matches!((a, b), ("rock", "scissors") | ("paper", "rock") | ("scissors", "paper"))
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    /// Rounds in a match; the first to win a majority wins it.
    pub best_of: u32,
    /// How long a round waits for both throws.
    pub round_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self { best_of: 3, round_ms: 20_000 }
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Waiting,
    Playing,
    Finished,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Seat {
    pub player: u64,
    pub wins: u32,
    /// Whether this round's throw is in (never which).
    pub thrown: bool,
}

/// A revealed round.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Reveal {
    pub round: u32,
    /// Each seat's throw, in seat order; `None` when it was not in on time.
    pub hands: Vec<Option<String>>,
    pub winner: Option<u64>,
}

/// The table as everyone sees it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct DuelView {
    pub phase: Phase,
    pub seats: Vec<Seat>,
    pub best_of: u32,
    pub round: u32,
    /// When this round closes.
    pub deadline: Option<u64>,
    pub last: Option<Reveal>,
    pub winner: Option<u64>,
}

#[derive(Deserialize)]
struct Throw {
    hand: String,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Duel {
    pub config: Config,
    pub phase: Phase,
    pub players: Vec<u64>,
    pub wins: Vec<u32>,
    pub round: u32,
    pub pending: Option<RevealRound>,
    pub last: Option<Reveal>,
    pub winner: Option<u64>,
}

impl Duel {
    pub fn view(&self) -> DuelView {
        let thrown = |p: u64| self.pending.as_ref().is_some_and(|r| r.submissions.iter().any(|(id, _)| *id == p.to_string()));
        DuelView {
            phase: self.phase,
            seats: self.players.iter().zip(&self.wins).map(|(&player, &wins)| Seat { player, wins, thrown: thrown(player) }).collect(),
            best_of: self.config.best_of,
            round: self.round,
            deadline: self.pending.as_ref().map(|r| r.deadline_ms),
            last: self.last.clone(),
            winner: self.winner,
        }
    }

    /// Changes whenever a new round starts, including a rematch that restarts at the same number.
    fn round_mark(&self) -> (u32, Phase) {
        (self.round, self.phase)
    }

    fn to_win(&self) -> u32 {
        self.config.best_of.max(1) / 2 + 1
    }

    /// Seats `player`; the second to sit starts the first round. Returns whether it started.
    pub fn join(&mut self, player: u64, now: u64) -> Result<bool> {
        if self.players.contains(&player) {
            return Ok(false);
        }
        if self.players.len() >= 2 {
            return Err(kit::error(codes::FULL, "the table has two players"));
        }
        self.players.push(player);
        self.wins.push(0);
        if self.players.len() == 2 {
            self.next_round(now);
            return Ok(true);
        }
        Ok(false)
    }

    pub fn leave(&mut self, player: u64) -> Result<()> {
        let Some(i) = self.players.iter().position(|&p| p == player) else {
            return Err(kit::error(codes::NOT_MEMBER, "you are not at this table"));
        };
        if self.phase == Phase::Playing {
            return Err(kit::error(codes::STARTED, "the duel is under way"));
        }
        self.players.remove(i);
        self.wins.remove(i);
        if self.phase == Phase::Finished {
            self.phase = Phase::Waiting;
            self.wins = vec![0; self.players.len()];
            self.round = 0;
            self.last = None;
            self.winner = None;
        }
        Ok(())
    }

    fn next_round(&mut self, now: u64) {
        self.phase = Phase::Playing;
        self.round += 1;
        self.pending = Some(RevealRound::new(self.players.iter().map(u64::to_string).collect(), now, self.config.round_ms));
    }

    /// Takes `player`'s hidden throw. Returns whether it completed the round.
    pub fn throw(&mut self, player: u64, hand: &str, now: u64) -> Result<bool> {
        if self.phase != Phase::Playing {
            return Err(kit::error(codes::CONFLICT, "no round is open"));
        }
        if !HANDS.contains(&hand) {
            return Err(kit::error(codes::BAD_REQUEST, format!("a hand is one of {}", HANDS.join(", "))));
        }
        let round = self.pending.as_mut().ok_or_else(|| kit::error(codes::CONFLICT, "no round is open"))?;
        round.submit(&player.to_string(), serde_json::Value::String(hand.into())).map_err(|why| match why {
            "already submitted" => kit::error(codes::CONFLICT, "your throw is in"),
            _ => kit::error(codes::NOT_MEMBER, "you are not at this table"),
        })?;
        if round.complete() {
            self.resolve(now);
            return Ok(true);
        }
        Ok(false)
    }

    /// Reveals the round when both throws are in or its time is up. Returns whether it did.
    pub fn resolve(&mut self, now: u64) -> bool {
        if !self.pending.as_ref().is_some_and(|r| r.ready(now)) {
            return false;
        }
        let submissions = self.pending.take().map(RevealRound::reveal).unwrap_or_default();
        let hands: Vec<Option<String>> = self
            .players
            .iter()
            .map(|p| submissions.iter().find(|(id, _)| *id == p.to_string()).and_then(|(_, v)| v.as_str().map(String::from)))
            .collect();
        let winner = match (hands.first().cloned().flatten(), hands.get(1).cloned().flatten()) {
            (Some(a), Some(b)) if beats(&a, &b) => Some(0),
            (Some(a), Some(b)) if beats(&b, &a) => Some(1),
            (Some(_), None) => Some(0),
            (None, Some(_)) => Some(1),
            _ => None,
        };
        if let Some(i) = winner {
            self.wins[i] += 1;
        }
        self.last = Some(Reveal { round: self.round, hands, winner: winner.map(|i| self.players[i]) });
        if let Some(i) = winner.filter(|&i| self.wins[i] >= self.to_win()) {
            self.phase = Phase::Finished;
            self.winner = Some(self.players[i]);
        } else {
            self.next_round(now);
        }
        true
    }

    pub fn rematch(&mut self, player: u64, now: u64) -> Result<()> {
        if self.phase != Phase::Finished {
            return Err(kit::error(codes::CONFLICT, "the duel is not over"));
        }
        if !self.players.contains(&player) {
            return Err(kit::error(codes::NOT_MEMBER, "you are not at this table"));
        }
        self.wins = vec![0; self.players.len()];
        self.round = 0;
        self.last = None;
        self.winner = None;
        self.next_round(now);
        Ok(())
    }
}

/// The hub: the duel, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct DuelHub {
    #[serde(flatten)]
    pub duel: Duel,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl DuelHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let round_before = self.duel.round_mark();
        let out = match call.method {
            "join" => encode(&self.duel.join(call.player()?, now)?),
            "leave" => {
                self.duel.leave(call.player()?)?;
                encode(&true)
            }
            "throw" => {
                let t: Throw = kit::args(call)?;
                encode(&self.duel.throw(call.player()?, &t.hand, now)?)
            }
            "rematch" => {
                self.duel.rematch(call.player()?, now)?;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.after(ctx, round_before)?;
        out
    }

    /// Arms the round timer for a new round, or stops it, and publishes.
    fn after(&mut self, ctx: &Ctx, round_before: (u32, Phase)) -> Result<()> {
        match &self.duel.pending {
            Some(_) if self.duel.round_mark() != round_before =>ctx.timer_after(ROUND_TIMER, self.duel.config.round_ms)?,
            None => {
                ctx.cancel_timer(ROUND_TIMER);
            }
            _ => {}
        }
        if self.duel.phase == Phase::Finished {
            ctx.persist_now();
        }
        self.kit.snapshot(ctx, TOPIC, &self.duel.view())
    }
}

impl Hub for DuelHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let mut hub = Self::default();
        if !seed.is_empty() {
            hub.duel.config = serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?;
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
        match call.method {
            "state" => encode(&self.duel.view()),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.duel.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, ctx.now_ms(), |h| h.act(ctx, &call, ctx.now_ms())),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != ROUND_TIMER {
            return Ok(());
        }
        let round_before = self.duel.round_mark();
        if self.duel.resolve(ctx.now_ms()) {
            self.after(ctx, round_before)?;
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(DuelHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, error_code};
    use ckx_sdk::testing::TestHub;
    use serde_json::json;

    fn code(e: &CallError) -> Option<&str> {
        error_code(&e.message).map(|c| c.0)
    }

    #[test]
    fn the_rules_go_round() {
        assert!(beats("rock", "scissors") && beats("scissors", "paper") && beats("paper", "rock"));
        assert!(!beats("rock", "rock") && !beats("rock", "paper"));
    }

    #[test]
    fn throws_stay_hidden_until_both_are_in_and_a_majority_wins() {
        let mut hub = TestHub::<DuelHub>::spawn("minigame", "table-1").unwrap();
        assert!(!hub.call_typed::<_, bool>(Caller::Player(1), "join", &()).unwrap());
        assert!(hub.call_typed::<_, bool>(Caller::Player(2), "join", &()).unwrap());
        let e = hub.call(Caller::Player(3), "join", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FULL));

        assert!(!hub.call_typed::<_, bool>(Caller::Player(1), "throw", &json!({ "hand": "rock" })).unwrap());
        let view: DuelView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
        assert!(view.seats[0].thrown && !view.seats[1].thrown);
        let published = hub.published();
        let (_, last) = published.last().unwrap();
        assert!(!String::from_utf8_lossy(last).contains("rock"), "a throw is never published before the reveal");
        let e = hub.call(Caller::Player(1), "throw", &encode(&json!({ "hand": "paper" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::CONFLICT));
        let e = hub.call(Caller::Player(2), "throw", &encode(&json!({ "hand": "lizard" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::BAD_REQUEST));

        assert!(hub.call_typed::<_, bool>(Caller::Player(2), "throw", &json!({ "hand": "scissors" })).unwrap());
        let view: DuelView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
        assert_eq!(view.last, Some(Reveal { round: 1, hands: vec![Some("rock".into()), Some("scissors".into())], winner: Some(1) }));
        assert_eq!((view.round, view.seats[0].wins), (2, 1));

        let _: bool = hub.call_typed(Caller::Player(1), "throw", &json!({ "hand": "paper" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "throw", &json!({ "hand": "rock" })).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
        assert_eq!((view.phase, view.winner), (Phase::Finished, Some(1)));
        let snap: Message<DuelView> =
            decode(&hub.call(Caller::Player(2), kit::SNAPSHOT_METHOD, &encode(&json!({ "topic": TOPIC })).unwrap()).unwrap()).unwrap();
        assert_eq!(snap.body.winner, Some(1));

        hub.call_typed::<_, bool>(Caller::Player(2), "rematch", &()).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
        assert_eq!((view.phase, view.round, view.seats[0].wins), (Phase::Playing, 1, 0));
    }

    #[test]
    fn a_throw_not_in_on_time_loses_the_round_and_a_round_with_none_is_replayed() {
        let mut hub = TestHub::<DuelHub>::spawn_with("minigame", "table-2", br#"{"best_of":1,"round_ms":5000}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        hub.advance(6_000).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.round, view.last.as_ref().map(|r| r.winner)), (Phase::Playing, 2, Some(None)));
        let _: bool = hub.call_typed(Caller::Player(2), "throw", &json!({ "hand": "paper" })).unwrap();
        hub.advance(6_000).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.winner), (Phase::Finished, Some(2)));
        assert_eq!(view.last.unwrap().hands, vec![None, Some("paper".into())]);
    }

    #[test]
    fn a_rematch_round_times_out_even_when_the_match_ended_in_round_one() {
        let mut hub = TestHub::<DuelHub>::spawn_with("minigame", "table-4", br#"{"best_of":1,"round_ms":5000}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "throw", &json!({ "hand": "rock" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "throw", &json!({ "hand": "scissors" })).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.round, view.winner), (Phase::Finished, 1, Some(1)));
        let _: bool = hub.call_typed(Caller::Player(2), "rematch", &()).unwrap();
        hub.advance(6_000).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.round, view.last.as_ref().map(|r| r.winner)), (Phase::Playing, 2, Some(None)));
    }

    #[test]
    fn a_player_who_is_not_seated_cannot_leave_and_changes_nothing() {
        let mut hub = TestHub::<DuelHub>::spawn_with("minigame", "table-5", br#"{"best_of":1}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let e = hub.call(Caller::Player(3), "leave", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::NOT_MEMBER), "waiting");
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let e = hub.call(Caller::Player(3), "leave", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::NOT_MEMBER), "playing");
        let _: bool = hub.call_typed(Caller::Player(1), "throw", &json!({ "hand": "rock" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "throw", &json!({ "hand": "scissors" })).unwrap();
        let finished: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((finished.phase, finished.winner), (Phase::Finished, Some(1)));
        let e = hub.call(Caller::Player(3), "leave", &encode(&()).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::NOT_MEMBER), "finished");
        let after: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!(after, finished, "the table is as it was");
        let _: bool = hub.call_typed(Caller::Player(2), "rematch", &()).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.seats.len()), (Phase::Playing, 2));
    }

    #[test]
    fn a_seated_player_leaves_a_finished_table_and_it_waits() {
        let mut hub = TestHub::<DuelHub>::spawn_with("minigame", "table-6", br#"{"best_of":1}"#).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "throw", &json!({ "hand": "rock" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "throw", &json!({ "hand": "scissors" })).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "leave", &()).unwrap();
        let view: DuelView = hub.call_typed(Caller::Player(1), "state", &()).unwrap();
        assert_eq!((view.phase, view.seats.len(), view.winner, view.last), (Phase::Waiting, 1, None, None));
    }

    #[test]
    fn a_resent_throw_is_answered_once() {
        let mut hub = TestHub::<DuelHub>::spawn("minigame", "table-3").unwrap();
        let _: bool = hub.call_typed(Caller::Player(1), "join", &()).unwrap();
        let _: bool = hub.call_typed(Caller::Player(2), "join", &()).unwrap();
        let args = json!({ "hand": "rock", "action_id": "t-1" });
        let first: bool = hub.call_typed(Caller::Player(1), "throw", &args).unwrap();
        let again: bool = hub.call_typed(Caller::Player(1), "throw", &args).unwrap();
        assert_eq!(first, again);
    }
}
