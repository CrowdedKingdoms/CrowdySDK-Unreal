//! A leaderboard, keyed by its name (`weekly-score`, `fastest-lap`, ...). The hub ranks players
//! itself, and only other hubs and developers submit: a client never sends its own score, the
//! hub that decided the result does (a session, a race, a run). Players read the top, the
//! places around a player, and their own. A season rolls on a cron schedule, or when a
//! developer says so, and the board keeps the last season's final standings.
//!
//! Endpoints: `submit { player, score }` (another instance, or a developer), `top { limit,
//! offset }`, `around { player, radius }` (players default to themselves), `me` (players),
//! `season`, `configure { .. }` and `roll` (developers). It speaks the kit protocol
//! (`ckx_sdk::kit`): the `board` topic carries the top of the board and the season as
//! `snapshot` messages, at most once a second, and so does `snapshot { topic: "board" }`;
//! changes may carry an `action_id`; refusals carry `kit:<code>:` codes.
//!
//! A `limit` or `player` of 0 and a `season_cron` of "" read as not given, since a Blueprint
//! input cannot be left out.

use std::collections::BTreeMap;

use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;

pub const TOPIC: &str = "board";
/// How often a changed board is published.
const PUBLISH_MS: u64 = 1_000;
const MAX_PAGE: usize = 100;

/// How a player's submissions make their score.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Keep {
    /// Their best submission (by `order`).
    #[default]
    Best,
    /// Every submission added up.
    Sum,
    /// Their last submission.
    Latest,
}

/// Which way is better.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Order {
    /// Higher scores rank first (points).
    #[default]
    High,
    /// Lower scores rank first (times).
    Low,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(default)]
pub struct Config {
    pub keep: Keep,
    pub order: Order,
    /// Entries the `board` topic carries.
    pub top_size: usize,
    /// A cron schedule that ends the season (`0 0 * * 1`: every Monday), or none.
    pub season_cron: Option<String>,
    pub max_entries: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self { keep: Keep::Best, order: Order::High, top_size: 10, season_cron: None, max_entries: 100_000 }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub score: i64,
    /// When the score was last improved (or set): the earlier of a tie ranks first.
    pub at: u64,
    pub submissions: u32,
}

/// One place on the board. Players with the same score share a rank.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Ranked {
    pub rank: u32,
    pub player: u64,
    pub score: i64,
}

/// What the `board` topic carries.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct BoardView {
    pub season: u32,
    pub season_started: u64,
    pub entries: usize,
    pub top: Vec<Ranked>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct SeasonView {
    pub season: u32,
    pub started: u64,
    pub entries: usize,
    /// The final standings of the season before, top first.
    pub last: Vec<Ranked>,
}

#[derive(Deserialize)]
struct Submit {
    player: u64,
    score: i64,
}

#[derive(Deserialize, Default)]
#[serde(default)]
struct Page {
    limit: Option<usize>,
    offset: usize,
}

#[derive(Deserialize, Default)]
#[serde(default)]
struct Around {
    player: Option<u64>,
    radius: Option<usize>,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Board {
    pub config: Config,
    pub entries: BTreeMap<u64, Entry>,
    pub season: u32,
    pub season_started: u64,
    pub last_season: Vec<Ranked>,
    /// The board changed since it was last published.
    #[serde(default)]
    pub dirty: bool,
}

impl Board {
    fn better(&self, a: i64, b: i64) -> bool {
        match self.config.order {
            Order::High => a > b,
            Order::Low => a < b,
        }
    }

    /// Takes a submission; returns the player's score now.
    pub fn submit(&mut self, player: u64, score: i64, now: u64) -> Result<i64> {
        if !self.entries.contains_key(&player) && self.entries.len() >= self.config.max_entries {
            return Err(kit::error(codes::FULL, "the board holds as many players as it takes"));
        }
        let keep = self.config.keep;
        let improves = self.entries.get(&player).map(|e| self.better(score, e.score));
        let e = self.entries.entry(player).or_insert(Entry { score, at: now, submissions: 0 });
        e.submissions += 1;
        match keep {
            Keep::Best => {
                if improves == Some(true) {
                    e.score = score;
                    e.at = now;
                }
            }
            Keep::Sum => {
                if e.submissions > 1 {
                    e.score = e.score.saturating_add(score);
                }
                e.at = now;
            }
            Keep::Latest => {
                e.score = score;
                e.at = now;
            }
        }
        self.dirty = true;
        Ok(e.score)
    }

    /// Every entry, best first; equal scores share a rank and the earlier one is listed first.
    pub fn ranked(&self) -> Vec<Ranked> {
        let mut all: Vec<(u64, &Entry)> = self.entries.iter().map(|(p, e)| (*p, e)).collect();
        let high = self.config.order == Order::High;
        all.sort_by(|a, b| {
            let by_score = if high { b.1.score.cmp(&a.1.score) } else { a.1.score.cmp(&b.1.score) };
            by_score.then(a.1.at.cmp(&b.1.at)).then(a.0.cmp(&b.0))
        });
        let mut out = Vec::with_capacity(all.len());
        let mut rank = 0;
        let mut last: Option<i64> = None;
        for (i, (player, e)) in all.into_iter().enumerate() {
            if last != Some(e.score) {
                rank = i as u32 + 1;
                last = Some(e.score);
            }
            out.push(Ranked { rank, player, score: e.score });
        }
        out
    }

    pub fn top(&self, offset: usize, limit: usize) -> Vec<Ranked> {
        self.ranked().into_iter().skip(offset).take(limit.min(MAX_PAGE)).collect()
    }

    pub fn around(&self, player: u64, radius: usize) -> Vec<Ranked> {
        let all = self.ranked();
        let Some(i) = all.iter().position(|r| r.player == player) else { return Vec::new() };
        let radius = radius.min(MAX_PAGE / 2);
        all[i.saturating_sub(radius)..(i + radius + 1).min(all.len())].to_vec()
    }

    pub fn me(&self, player: u64) -> Option<Ranked> {
        self.ranked().into_iter().find(|r| r.player == player)
    }

    /// Ends the season: keeps its final standings and starts the next, empty.
    pub fn roll(&mut self, now: u64) {
        self.last_season = self.top(0, MAX_PAGE);
        self.entries.clear();
        self.season += 1;
        self.season_started = now;
        self.dirty = true;
    }

    pub fn view(&self) -> BoardView {
        BoardView {
            season: self.season,
            season_started: self.season_started,
            entries: self.entries.len(),
            top: self.top(0, self.config.top_size),
        }
    }

    fn arm(&self, ctx: &Ctx) -> Result<()> {
        match self.config.season_cron.as_deref().filter(|c| !c.is_empty()) {
            Some(expr) => ctx.timer_cron("season", &Cron::parse(expr).map_err(|e| kit::error(codes::BAD_REQUEST, e))?),
            None => {
                ctx.cancel_timer("season");
                Ok(())
            }
        }
    }
}

/// The hub: the board, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct BoardHub {
    #[serde(flatten)]
    pub board: Board,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

fn developer(call: &Call<'_>) -> Result<u64> {
    call.developer().map_err(|_| kit::error(codes::FORBIDDEN, format!("`{}` is for the app's developers", call.method)))
}

impl BoardHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let board = &mut self.board;
        match call.method {
            "submit" => {
                if !matches!(call.caller, Caller::Instance(_) | Caller::Developer(_)) {
                    return Err(kit::error(codes::FORBIDDEN, "scores come from the hub that decided them, not from players"));
                }
                let s: Submit = kit::args(call)?;
                encode(&board.submit(s.player, s.score, now)?)
            }
            "configure" => {
                developer(call)?;
                let config: Config = kit::args(call)?;
                if config.top_size == 0 || config.top_size > MAX_PAGE || config.max_entries == 0 {
                    return Err(kit::error(codes::BAD_REQUEST, format!("1 <= top_size <= {MAX_PAGE}, and max_entries is at least 1")));
                }
                board.config = config;
                board.arm(ctx)?;
                board.dirty = true;
                encode(&true)
            }
            "roll" => {
                developer(call)?;
                board.roll(now);
                ctx.persist_now();
                encode(&board.season)
            }
            other => Err(Error::unknown_method(other)),
        }
    }
}

impl Hub for BoardHub {
    fn spawn(ctx: &Ctx, _seed: &[u8]) -> Result<Self> {
        ctx.timer_every("publish", PUBLISH_MS)?;
        let mut hub = Self::default();
        hub.board.season = 1;
        hub.board.season_started = ctx.now_ms();
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
        let board = &self.board;
        match call.method {
            "top" => {
                let p: Page = kit::args(&call).unwrap_or_default();
                encode(&board.top(p.offset, p.limit.filter(|l| *l > 0).unwrap_or(board.config.top_size)))
            }
            "around" => {
                let a: Around = kit::args(&call).unwrap_or_default();
                let player = match (a.player.filter(|p| *p != 0), call.caller) {
                    (Some(p), _) => p,
                    (None, Caller::Player(p)) => p,
                    _ => return Err(kit::error(codes::BAD_REQUEST, "name a player")),
                };
                encode(&board.around(player, a.radius.unwrap_or(2)))
            }
            "me" => encode(&board.me(call.player()?)),
            "season" => encode(&SeasonView {
                season: board.season,
                started: board.season_started,
                entries: board.entries.len(),
                last: board.last_season.clone(),
            }),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &board.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        match timer {
            "season" => {
                self.board.roll(ctx.now_ms());
                self.board.arm(ctx)?;
                ctx.persist_now();
            }
            "publish" if self.board.dirty => {}
            _ => return Ok(()),
        }
        self.board.dirty = false;
        self.kit.snapshot(ctx, TOPIC, &self.board.view())
    }
}

ckx_sdk::export_hub!(BoardHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, error_code};
    use ckx_sdk::testing::TestHub;

    #[test]
    fn players_rank_by_their_best_ties_share_a_rank_and_the_earlier_lists_first() {
        let mut b = Board::default();
        b.submit(1, 50, 0).unwrap();
        b.submit(2, 70, 1).unwrap();
        b.submit(3, 70, 2).unwrap();
        assert_eq!(b.submit(1, 40, 3).unwrap(), 50, "a worse score keeps the best");
        let r = b.ranked();
        assert_eq!(r.iter().map(|x| (x.rank, x.player)).collect::<Vec<_>>(), vec![(1, 2), (1, 3), (3, 1)]);
        assert_eq!(b.around(3, 1).len(), 3);
        assert_eq!(b.me(1).unwrap().rank, 3);
    }

    #[test]
    fn times_rank_low_first_and_sums_add_up() {
        let mut times = Board { config: Config { order: Order::Low, ..Config::default() }, ..Default::default() };
        times.submit(1, 61_000, 0).unwrap();
        times.submit(2, 59_500, 0).unwrap();
        assert_eq!(times.submit(1, 58_000, 1).unwrap(), 58_000, "a faster lap improves");
        assert_eq!(times.top(0, 1)[0].player, 1);
        let mut sums = Board { config: Config { keep: Keep::Sum, ..Config::default() }, ..Default::default() };
        sums.submit(1, 5, 0).unwrap();
        assert_eq!(sums.submit(1, 7, 1).unwrap(), 12);
    }

    #[test]
    fn zero_and_empty_inputs_read_as_not_given() {
        let mut hub = TestHub::<BoardHub>::spawn("leaderboard", "weekly").unwrap();
        for (player, score) in [(1, 9), (2, 7), (3, 5)] {
            let _: i64 = hub.call_typed(Caller::Developer(9), "submit", &serde_json::json!({ "player": player, "score": score })).unwrap();
        }
        let top: Vec<Ranked> = hub.call_typed(Caller::Player(3), "top", &serde_json::json!({ "limit": 0, "offset": 0 })).unwrap();
        assert_eq!(top.len(), 3, "a limit of 0 is the board's top size");
        let around: Vec<Ranked> = hub.call_typed(Caller::Player(3), "around", &serde_json::json!({ "player": 0, "radius": 1 })).unwrap();
        assert_eq!(around.iter().map(|r| r.player).collect::<Vec<_>>(), vec![2, 3], "a player of 0 is the caller");
        let config = serde_json::json!({ "keep": "best", "order": "high", "top_size": 10, "season_cron": "", "max_entries": 100 });
        let _: bool = hub.call_typed(Caller::Developer(9), "configure", &config).unwrap();
    }

    #[test]
    fn a_season_rolls_on_its_schedule_and_keeps_the_final_standings() {
        let mut hub = TestHub::<BoardHub>::spawn("leaderboard", "weekly").unwrap();
        let e = hub.call(Caller::Player(1), "submit", &encode(&serde_json::json!({ "player": 1, "score": 9 })).unwrap()).unwrap_err();
        assert_eq!(error_code(&e.message).map(|c| c.0), Some(codes::FORBIDDEN), "players do not submit");
        let _: i64 = hub.call_typed(Caller::Instance(4), "submit", &serde_json::json!({ "player": 1, "score": 9 })).unwrap();
        let _: i64 = hub.call_typed(Caller::Instance(4), "submit", &serde_json::json!({ "player": 2, "score": 3 })).unwrap();
        hub.advance(PUBLISH_MS).unwrap();
        let msg: Message<BoardView> = decode(&hub.published().last().unwrap().1).unwrap();
        assert_eq!(msg.body.top.iter().map(|r| r.player).collect::<Vec<_>>(), vec![1, 2]);
        let _: bool = hub.call_typed(Caller::Developer(9), "configure", &serde_json::json!({ "season_cron": "0 0 * * *" })).unwrap();
        hub.advance(24 * 3_600_000).unwrap();
        let season: SeasonView = hub.call_typed(Caller::Player(1), "season", &()).unwrap();
        assert_eq!((season.season, season.entries, season.last.len()), (2, 0, 2));
        let me: Option<Ranked> = hub.call_typed(Caller::Player(1), "me", &()).unwrap();
        assert_eq!(me, None, "the new season starts empty");
    }
}
