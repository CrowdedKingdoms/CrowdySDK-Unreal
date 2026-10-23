//! A card table, keyed by its id, with true hidden information: the deck's order and every
//! hand live in the hub's state, and a player reads only their own hand, with a call the hub
//! answers for its caller. The starter's game is crazy eights: match the top card's suit or
//! rank, eights are wild and name a suit, draw when you cannot (once a turn), and the first to
//! empty their hand wins the points left in the others'. Replace [`playable`] and
//! [`card_points`] with your game's rules.
//!
//! Endpoints (players): `join`, `leave`, `start` (the host, from two players), `play { card,
//! suit? }`, `draw`, `pass` (after drawing), `hand` (the caller's own), `state` (anyone). It
//! speaks the kit protocol (`ckx_sdk::kit`): the `table` topic carries the public table as
//! `snapshot` messages and a `changed` message naming the players whose hand changed
//! (`what`: `hand`), who read it with `hand`; `snapshot { topic: "table" }` answers the table;
//! changes may carry an `action_id`; refusals carry `kit:<code>:` codes.
//!
//! This is the Unreal SDK's copy of the platform's starter. Every card also travels as a
//! [`CardFace`] (`{ id, rank, suit }`) beside its text: `faces` and `playable_faces` in the hand,
//! `top_face` on the table, `face` on the last move, all in the same order as the text lists. The
//! `changed` message names the current player before the action and after it as well as the
//! players whose cards changed, because a hand's `playable` and `drawn` depend on whose turn it
//! is: the player whose turn began and the one whose turn ended must read it again. Its config
//! (`{ "secret": "..." }`) is mixed into every shuffle, since the table's key, the clock and the
//! epoch are all things a player can see: without it a client could work out every hand.

use std::collections::BTreeMap;

use ckx_sdk::kit::play::cards::{CardId, CardTable, standard_52};
use ckx_sdk::kit::{self, Publisher, Receipts, Rng, codes};
use ckx_sdk::prelude::*;

pub const TOPIC: &str = "table";
pub const MIN_PLAYERS: usize = 2;
pub const MAX_PLAYERS: usize = 6;
const HAND: usize = 5;
const TURN_MS: u64 = 30_000;

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    #[default]
    Lobby,
    Playing,
    Finished,
}

/// A card's rank and suit: `"10H"` is rank `10`, suit `H`.
pub fn split(card: &str) -> (&str, &str) {
    card.split_at(card.len().saturating_sub(1))
}

/// Whether `card` may go on `top` with `suit` the suit in play. The starter's rule: same suit,
/// same rank, or an eight.
pub fn playable(card: &str, top: &str, suit: &str) -> bool {
    let (rank, s) = split(card);
    rank == "8" || s == suit || rank == split(top).0
}

/// What a card left in a hand is worth to the winner: eights 50, faces 10, aces 1.
pub fn card_points(card: &str) -> i64 {
    match split(card).0 {
        "8" => 50,
        "J" | "Q" | "K" | "10" => 10,
        "A" => 1,
        n => n.parse().unwrap_or(0),
    }
}

/// A card as data: its text, its rank (1 ace, 2..10, 11 jack, 12 queen, 13 king; 0 if unreadable)
/// and its suit (`S`, `H`, `D` or `C`).
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct CardFace {
    pub id: String,
    pub rank: u8,
    pub suit: String,
}

pub fn face(card: &str) -> CardFace {
    let (rank, suit) = split(card);
    let rank = match rank {
        "A" => 1,
        "J" => 11,
        "Q" => 12,
        "K" => 13,
        n => n.parse().ok().filter(|r| (2..=10).contains(r)).unwrap_or(0),
    };
    CardFace { id: card.to_owned(), rank, suit: suit.to_owned() }
}

/// A move, as the table shows it.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Played {
    pub player: u64,
    /// The card, or none for a draw or a pass.
    pub card: Option<CardId>,
    pub action: String,
    /// The card as data; a snapshot saved before it existed loads without one.
    pub face: Option<CardFace>,
}

impl Played {
    fn new(player: u64, card: Option<&str>, action: &str) -> Self {
        Self { player, card: card.map(str::to_owned), action: action.to_owned(), face: card.map(face) }
    }
}

/// The table as everyone sees it: never a hidden card.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct TableView {
    pub phase: Phase,
    pub players: Vec<u64>,
    pub host: Option<u64>,
    /// Cards in each player's hand, by player id.
    pub hand_counts: BTreeMap<u64, usize>,
    pub top: Option<CardId>,
    /// The suit in play (an eight names one).
    pub suit: Option<String>,
    pub draw_count: usize,
    pub turn: Option<u64>,
    pub turn_ends: Option<u64>,
    pub last: Option<Played>,
    pub winner: Option<u64>,
    pub scores: BTreeMap<u64, i64>,
    /// `top` as data.
    pub top_face: Option<CardFace>,
}

/// What `hand` answers its caller.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct HandView {
    pub cards: Vec<CardId>,
    /// The cards that may go on the table now (empty when it is not the caller's turn).
    pub playable: Vec<CardId>,
    pub drawn: bool,
    /// `cards` as data, in the same order.
    pub faces: Vec<CardFace>,
    /// `playable` as data, in the same order.
    pub playable_faces: Vec<CardFace>,
}

#[derive(Deserialize)]
struct Play {
    card: CardId,
    #[serde(default)]
    suit: Option<String>,
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Table {
    pub phase: Phase,
    pub players: Vec<u64>,
    pub host: Option<u64>,
    pub cards: CardTable,
    pub suit: String,
    pub turn: usize,
    pub turn_ends: u64,
    /// The current player drew this turn.
    pub drawn: bool,
    pub last: Option<Played>,
    pub winner: Option<u64>,
    pub scores: BTreeMap<u64, i64>,
}

fn id(p: u64) -> String {
    p.to_string()
}

impl Table {
    pub fn current(&self) -> Option<u64> {
        (self.phase == Phase::Playing).then(|| self.players.get(self.turn).copied()).flatten()
    }

    fn top(&self) -> Option<&CardId> {
        self.cards.discard.cards.last()
    }

    pub fn view(&self) -> TableView {
        TableView {
            phase: self.phase,
            players: self.players.clone(),
            host: self.host,
            hand_counts: self.players.iter().map(|p| (*p, self.cards.hand(&id(*p)).len())).collect(),
            top: self.top().cloned(),
            suit: (self.phase != Phase::Lobby).then(|| self.suit.clone()),
            draw_count: self.cards.draw.len(),
            turn: self.current(),
            turn_ends: (self.phase == Phase::Playing).then_some(self.turn_ends),
            last: self.last.clone(),
            winner: self.winner,
            scores: self.scores.clone(),
            top_face: self.top().map(|c| face(c)),
        }
    }

    pub fn hand(&self, player: u64) -> HandView {
        let cards = self.cards.hand(&id(player)).to_vec();
        let mine = self.current() == Some(player);
        let top = self.top().cloned().unwrap_or_default();
        let playable: Vec<CardId> =
            if mine { cards.iter().filter(|c| playable(c, &top, &self.suit)).cloned().collect() } else { Vec::new() };
        let faces = cards.iter().map(|c| face(c)).collect();
        let playable_faces = playable.iter().map(|c| face(c)).collect();
        HandView { cards, playable, drawn: mine && self.drawn, faces, playable_faces }
    }

    pub fn join(&mut self, player: u64) -> Result<()> {
        if self.players.contains(&player) {
            return Ok(());
        }
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the game has started"));
        }
        if self.players.len() >= MAX_PLAYERS {
            return Err(kit::error(codes::FULL, "the table is full"));
        }
        self.players.push(player);
        self.host.get_or_insert(player);
        Ok(())
    }

    pub fn leave(&mut self, player: u64, now: u64) -> Vec<u64> {
        let Some(i) = self.players.iter().position(|p| *p == player) else { return Vec::new() };
        if self.phase == Phase::Playing {
            // Their cards go to the bottom of the deck.
            let hand = self.cards.hands.remove(&id(player)).unwrap_or_default();
            for c in hand {
                self.cards.draw.cards.insert(0, c);
            }
        }
        self.players.remove(i);
        if self.host == Some(player) {
            self.host = self.players.first().copied();
        }
        if self.phase == Phase::Playing {
            // The leaver's hand is gone, so they read it again too.
            if self.players.len() < MIN_PLAYERS {
                self.finish(self.players.first().copied());
                return vec![player];
            }
            let was_current = i == self.turn;
            if i < self.turn {
                self.turn -= 1;
            }
            if self.turn >= self.players.len() {
                self.turn = 0;
            }
            // Someone else leaving keeps the current player's clock and draw.
            if was_current {
                self.turn_ends = now + TURN_MS;
                self.drawn = false;
            }
            return vec![player];
        }
        Vec::new()
    }

    /// Deals and turns the first card; returns the players whose hand changed.
    pub fn start(&mut self, player: u64, seed: u64, now: u64) -> Result<Vec<u64>> {
        if self.phase != Phase::Lobby {
            return Err(kit::error(codes::STARTED, "the game has started"));
        }
        if self.host != Some(player) {
            return Err(kit::error(codes::FORBIDDEN, "only the host starts the game"));
        }
        if self.players.len() < MIN_PLAYERS {
            return Err(kit::error("too_few_players", format!("at least {MIN_PLAYERS} players are needed")));
        }
        self.cards = CardTable::new(standard_52(), seed);
        let ids: Vec<String> = self.players.iter().map(|p| id(*p)).collect();
        self.cards.deal(&ids, HAND);
        let first = self.cards.draw.draw().ok_or_else(|| kit::error(codes::EMPTY, "the deck is empty"))?;
        self.suit = split(&first).1.to_owned();
        self.cards.discard.put(first);
        self.phase = Phase::Playing;
        self.turn = 0;
        self.turn_ends = now + TURN_MS;
        self.drawn = false;
        self.scores = self.players.iter().map(|p| (*p, 0)).collect();
        Ok(self.players.clone())
    }

    fn mine(&self, player: u64) -> Result<()> {
        if self.phase != Phase::Playing {
            return Err(kit::error(codes::CONFLICT, "the game is not being played"));
        }
        if self.current() != Some(player) {
            return Err(kit::error(codes::NOT_YOUR_TURN, "it is not your turn"));
        }
        Ok(())
    }

    fn next(&mut self, now: u64) {
        self.turn = (self.turn + 1) % self.players.len().max(1);
        self.turn_ends = now + TURN_MS;
        self.drawn = false;
    }

    fn finish(&mut self, winner: Option<u64>) -> Vec<u64> {
        self.phase = Phase::Finished;
        self.winner = winner;
        if let Some(w) = winner {
            let won: i64 =
                self.players.iter().filter(|p| **p != w).flat_map(|p| self.cards.hand(&id(*p)).to_vec()).map(|c| card_points(&c)).sum();
            *self.scores.entry(w).or_insert(0) += won;
        }
        Vec::new()
    }

    /// Plays a card; returns the players whose hand changed.
    pub fn play(&mut self, player: u64, card: &str, suit: Option<&str>, now: u64) -> Result<Vec<u64>> {
        self.mine(player)?;
        if !self.cards.hand(&id(player)).iter().any(|c| c == card) {
            return Err(kit::error(codes::NOT_FOUND, "that card is not in your hand"));
        }
        let top = self.top().cloned().unwrap_or_default();
        if !playable(card, &top, &self.suit) {
            return Err(kit::error("not_playable", format!("{card} does not go on {top}")));
        }
        let (rank, card_suit) = split(card);
        let next_suit = if rank == "8" {
            match suit {
                Some(s @ ("S" | "H" | "D" | "C")) => s.to_owned(),
                _ => return Err(kit::error(codes::BAD_REQUEST, "an eight names the next suit: S, H, D or C")),
            }
        } else {
            card_suit.to_owned()
        };
        self.cards.play(&id(player), card, "table").map_err(|e| kit::error(codes::NOT_FOUND, e))?;
        for (_, c) in self.cards.zones.remove("table").unwrap_or_default() {
            self.cards.discard.put(c);
        }
        self.suit = next_suit;
        self.last = Some(Played::new(player, Some(card), "play"));
        if self.cards.hand(&id(player)).is_empty() {
            self.finish(Some(player));
            return Ok(vec![player]);
        }
        self.next(now);
        Ok(vec![player])
    }

    /// Draws one card (once a turn); returns the players whose hand changed.
    pub fn draw(&mut self, player: u64) -> Result<Vec<u64>> {
        self.mine(player)?;
        if self.drawn {
            return Err(kit::error(codes::CONFLICT, "you drew this turn: play or pass"));
        }
        // Recycling the discard keeps its top card on the table.
        let top = self.cards.discard.draw();
        let drew = self.cards.draw_to_hand(&id(player));
        if let Some(t) = top {
            self.cards.discard.put(t);
        }
        drew.ok_or_else(|| kit::error(codes::EMPTY, "no cards left to draw"))?;
        self.drawn = true;
        self.last = Some(Played::new(player, None, "draw"));
        Ok(vec![player])
    }

    pub fn pass(&mut self, player: u64, now: u64) -> Result<()> {
        self.mine(player)?;
        if !self.drawn {
            return Err(kit::error(codes::CONFLICT, "draw before you pass"));
        }
        self.last = Some(Played::new(player, None, "pass"));
        self.next(now);
        Ok(())
    }

    /// A turn that ran out: the player draws (if they had not) and passes.
    pub fn timeout(&mut self, now: u64) -> Vec<u64> {
        let Some(player) = self.current() else { return Vec::new() };
        if now < self.turn_ends {
            return Vec::new();
        }
        let changed = if self.drawn { Vec::new() } else { self.draw(player).unwrap_or_default() };
        self.last = Some(Played::new(player, None, "timeout"));
        self.next(now);
        changed
    }
}

/// The hub: the table, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct TableHub {
    #[serde(flatten)]
    pub table: Table,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
    /// From the config; never published or answered.
    #[serde(default)]
    pub secret: String,
}

/// The starter's config, its spawn seed.
#[derive(Deserialize, Default)]
#[serde(default)]
struct DeckConfig {
    secret: Option<String>,
}

/// The seed a deal shuffles with: the config's secret beside what a player can see.
pub fn deal_seed(key: &str, now: u64, epoch: u64, secret: &str) -> u64 {
    Rng::seed_from(&format!("deck:{key}:{now}:{epoch}:{secret}"))
}

impl TableHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let player = call.player()?;
        let before = self.table.current();
        let secret = self.secret.as_str();
        let t = &mut self.table;
        let (out, changed) = match call.method {
            "join" => (t.join(player).map(|_| encode(&true))?, Vec::new()),
            "leave" => {
                let changed = t.leave(player, now);
                (encode(&true), changed)
            }
            "start" => {
                let seed = deal_seed(&ctx.key, now, ctx.epoch, secret);
                let changed = t.start(player, seed, now)?;
                (encode(&true), changed)
            }
            "play" => {
                let p: Play = kit::args(call)?;
                let changed = t.play(player, &p.card, p.suit.as_deref(), now)?;
                (encode(&(t.phase == Phase::Playing)), changed)
            }
            "draw" => {
                let changed = t.draw(player)?;
                (encode(&t.hand(player)), changed)
            }
            "pass" => {
                t.pass(player, now)?;
                (encode(&true), Vec::new())
            }
            other => return Err(Error::unknown_method(other)),
        };
        self.arm(ctx)?;
        self.publish(ctx, before, changed)?;
        ctx.persist_now();
        out
    }

    fn arm(&self, ctx: &Ctx) -> Result<()> {
        if self.table.phase == Phase::Playing {
            ctx.timer_after("turn", self.table.turn_ends.saturating_sub(ctx.now_ms()).max(1))
        } else {
            ctx.cancel_timer("turn");
            Ok(())
        }
    }

    /// Publishes the table and names who re-reads their hand: those whose cards changed, and the
    /// current player before the action and after it.
    fn publish(&mut self, ctx: &Ctx, before: Option<u64>, mut who: Vec<u64>) -> Result<()> {
        for p in [before, self.table.current()].into_iter().flatten() {
            if !who.contains(&p) {
                who.push(p);
            }
        }
        self.kit.snapshot(ctx, TOPIC, &self.table.view())?;
        self.kit.changed(ctx, TOPIC, &who, "hand")
    }
}

impl Hub for TableHub {
    fn spawn(_ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let config: DeckConfig = if seed.is_empty() {
            DeckConfig::default()
        } else {
            serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?
        };
        Ok(Self { secret: config.secret.unwrap_or_default(), ..Self::default() })
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
            "state" => encode(&self.table.view()),
            "hand" => encode(&self.table.hand(call.player()?)),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.table.view()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "turn" {
            return Ok(());
        }
        let before = self.table.current();
        let changed = self.table.timeout(ctx.now_ms());
        self.arm(ctx)?;
        self.publish(ctx, before, changed)
    }
}

ckx_sdk::export_hub!(TableHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Changed, Message, MessageKind, error_code};
    use ckx_sdk::testing::TestHub;

    fn started() -> Table {
        let mut t = Table::default();
        t.join(1).unwrap();
        t.join(2).unwrap();
        t.start(1, 7, 0).unwrap();
        t
    }

    #[test]
    fn cards_match_suit_or_rank_and_eights_are_wild() {
        assert!(playable("5H", "9H", "H"));
        assert!(playable("9S", "9H", "H"));
        assert!(playable("8C", "2D", "D"));
        assert!(!playable("5S", "9H", "H"));
        assert_eq!(split("10H"), ("10", "H"));
        assert_eq!(card_points("8S") + card_points("KD") + card_points("3C") + card_points("AH"), 64);
    }

    #[test]
    fn hands_are_dealt_hidden_and_only_the_current_player_moves() {
        let mut t = started();
        assert_eq!(t.view().hand_counts, BTreeMap::from([(1, HAND), (2, HAND)]));
        assert_eq!(t.view().draw_count, 52 - 2 * HAND - 1);
        assert!(t.hand(2).playable.is_empty(), "not their turn");
        let Error(why) = t.draw(2).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::NOT_YOUR_TURN));
        t.draw(1).unwrap();
        assert!(t.draw(1).is_err(), "once a turn");
        t.pass(1, 10).unwrap();
        assert_eq!(t.current(), Some(2));
        assert_eq!(t.hand(1).cards.len(), HAND + 1);
    }

    #[test]
    fn playing_out_a_hand_wins_the_points_left_in_the_others() {
        let mut t = started();
        let mut guard = 0;
        while t.phase == Phase::Playing && guard < 500 {
            guard += 1;
            let p = t.current().unwrap();
            let h = t.hand(p);
            if let Some(card) = h.playable.first() {
                let suit = (split(card).0 == "8").then_some("H");
                t.play(p, card, suit, guard).unwrap();
            } else if !h.drawn && t.draw(p).is_ok() {
            } else {
                t.pass(p, guard).unwrap_or(());
                if t.current() == Some(p) {
                    t.timeout(u64::MAX);
                }
            }
        }
        assert_eq!(t.phase, Phase::Finished);
        let w = t.winner.unwrap();
        assert!(t.hand(w).cards.is_empty());
        assert!(t.scores[&w] > 0);
    }

    #[test]
    fn the_table_topic_is_public_and_a_changed_message_names_who_reads_their_hand() {
        let mut hub = TestHub::<TableHub>::spawn("deck", "t1").unwrap();
        for p in [1, 2] {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
        }
        hub.clear();
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        #[derive(Deserialize)]
        struct Kind {
            kind: MessageKind,
        }
        let published_now = hub.published();
        let kinds: Vec<MessageKind> = published_now.iter().map(|(_, b)| decode::<Kind>(b).unwrap().kind).collect();
        assert_eq!(kinds, vec![MessageKind::Snapshot, MessageKind::Changed]);
        let changed: Message<Changed> = decode(&published_now[1].1).unwrap();
        let changed = changed.body;
        assert_eq!((changed.who, changed.what.as_str()), (vec![1, 2], "hand"));
        let published = format!("{:?}", hub.published());
        let hand: HandView = hub.call_typed(Caller::Player(2), "hand", &()).unwrap();
        assert_eq!(hand.cards.len(), HAND);
        assert!(hand.cards.iter().all(|c| !published.contains(&format!("\"{c}\""))), "no hidden card is ever published");
        hub.advance(TURN_MS).unwrap();
        let view: TableView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
        assert_eq!((view.turn, view.hand_counts[&1]), (Some(2), HAND + 1), "a timeout draws and passes");
    }

    fn two_player_hub(key: &str) -> TestHub<TableHub> {
        let mut hub = TestHub::<TableHub>::spawn("deck", key).unwrap();
        for p in [1, 2] {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
        }
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        hub
    }

    fn last_who(hub: &TestHub<TableHub>) -> Vec<u64> {
        let published = hub.published();
        let changed: Message<Changed> = decode(&published.last().unwrap().1).unwrap();
        assert_eq!(changed.body.what, "hand");
        changed.body.who
    }

    fn sorted(mut who: Vec<u64>) -> Vec<u64> {
        who.sort();
        who
    }

    #[test]
    fn a_card_reads_as_rank_and_suit() {
        let ten = face("10H");
        assert_eq!((ten.id.as_str(), ten.rank, ten.suit.as_str()), ("10H", 10, "H"));
        assert_eq!((face("AS").rank, face("AS").suit.as_str()), (1, "S"));
        assert_eq!((face("KD").rank, face("KD").suit.as_str()), (13, "D"));
        assert_eq!((face("8C").rank, face("8C").suit.as_str()), (8, "C"));
        assert_eq!((face("JS").rank, face("QS").rank), (11, 12));
        assert_eq!(face("ZH").rank, 0, "an unreadable rank is 0");
    }

    #[test]
    fn hand_and_table_views_carry_faces_that_match_the_text() {
        let mut t = started();
        let h = t.hand(1);
        assert_eq!(h.faces, h.cards.iter().map(|c| face(c)).collect::<Vec<_>>());
        assert_eq!(h.playable_faces, h.playable.iter().map(|c| face(c)).collect::<Vec<_>>());
        assert!(h.faces.len() == HAND && h.faces.iter().zip(&h.cards).all(|(f, c)| &f.id == c));
        assert!(t.hand(2).playable_faces.is_empty(), "not their turn");
        let v = t.view();
        assert_eq!(v.top_face, v.top.as_deref().map(face));
        assert!(v.top_face.is_some());
        t.draw(1).unwrap();
        let last = t.view().last.unwrap();
        assert_eq!((last.card, last.face), (None, None), "a draw shows no card");
    }

    #[test]
    fn a_played_card_carries_its_face_on_the_table() {
        let mut t = started();
        let p = t.current().unwrap();
        let card = t.hand(p).playable.first().cloned().expect("seed 7 deals a playable card");
        let suit = (split(&card).0 == "8").then_some("H");
        t.play(p, &card, suit, 5).unwrap();
        let v = t.view();
        assert_eq!(v.last.unwrap().face, Some(face(&card)));
        assert_eq!(v.top_face, Some(face(&card)));
    }

    #[test]
    fn a_play_names_the_player_who_played_and_the_next_player() {
        for k in 0..50 {
            let mut hub = two_player_hub(&format!("t{k}"));
            let hand: HandView = hub.call_typed(Caller::Player(1), "hand", &()).unwrap();
            let Some(card) = hand.playable.first() else { continue };
            hub.clear();
            let args = serde_json::json!({ "card": card, "suit": "H" });
            let _: bool = hub.call_typed(Caller::Player(1), "play", &args).unwrap();
            assert_eq!(sorted(last_who(&hub)), vec![1, 2]);
            return;
        }
        panic!("no seed dealt a playable card");
    }

    #[test]
    fn a_pass_names_the_player_whose_turn_ended_and_the_next() {
        let mut hub = two_player_hub("t1");
        let _: HandView = hub.call_typed(Caller::Player(1), "draw", &()).unwrap();
        hub.clear();
        let _: bool = hub.call_typed(Caller::Player(1), "pass", &()).unwrap();
        assert_eq!(sorted(last_who(&hub)), vec![1, 2]);
    }

    #[test]
    fn a_timeout_names_both_players() {
        let mut hub = two_player_hub("t1");
        hub.clear();
        hub.advance(TURN_MS).unwrap();
        assert_eq!(sorted(last_who(&hub)), vec![1, 2]);
    }

    #[test]
    fn a_timeout_after_a_draw_still_names_both_players() {
        let mut hub = two_player_hub("t1");
        let _: HandView = hub.call_typed(Caller::Player(1), "draw", &()).unwrap();
        hub.clear();
        hub.advance(TURN_MS).unwrap();
        assert_eq!(sorted(last_who(&hub)), vec![1, 2]);
    }

    #[test]
    fn the_last_seated_current_player_leaving_wraps_the_turn_to_the_first() {
        let mut t = Table::default();
        for p in [1, 2, 3] {
            t.join(p).unwrap();
        }
        t.start(1, 7, 0).unwrap();
        t.next(1);
        t.next(2);
        assert_eq!(t.current(), Some(3));
        t.leave(3, 3);
        assert_eq!(t.current(), Some(1));
    }

    #[test]
    fn a_player_leaving_before_the_turn_keeps_the_turn_on_the_same_player() {
        let mut t = Table::default();
        for p in [1, 2, 3] {
            t.join(p).unwrap();
        }
        t.start(1, 7, 0).unwrap();
        t.next(1);
        t.next(2);
        t.leave(1, 3);
        assert_eq!(t.current(), Some(3));
    }

    #[test]
    fn a_move_saved_before_faces_existed_still_loads() {
        let old = r#"{"player":1,"card":"5H","action":"play"}"#;
        let played: Played = serde_json::from_str(old).unwrap();
        assert_eq!((played.card.as_deref(), played.face), (Some("5H"), None));
        let mut saved = serde_json::to_value(Table::default()).unwrap();
        saved["last"] = serde_json::from_str(old).unwrap();
        let table: Table = serde_json::from_value(saved).unwrap();
        assert_eq!(table.last.unwrap().face, None);

        #[derive(Serialize)]
        struct OldPlayed {
            player: u64,
            card: Option<CardId>,
            action: String,
        }
        let bytes = encode(&OldPlayed { player: 1, card: Some("5H".into()), action: "play".into() }).unwrap();
        let persisted: Played = decode(&bytes).unwrap();
        assert_eq!((persisted.card.as_deref(), persisted.face), (Some("5H"), None), "as the hub persists it");
    }

    #[test]
    fn someone_else_leaving_keeps_the_current_players_clock_and_draw() {
        let mut t = Table::default();
        for p in [1, 2, 3, 4] {
            t.join(p).unwrap();
        }
        t.start(1, 7, 0).unwrap();
        t.draw(1).unwrap();
        let ends = t.turn_ends;
        t.leave(3, 5_000);
        assert_eq!((t.current(), t.drawn, t.turn_ends), (Some(1), true, ends));
        assert!(t.draw(1).is_err(), "still once a turn");
        t.leave(1, 6_000);
        assert_eq!((t.current(), t.drawn, t.turn_ends), (Some(2), false, 6_000 + TURN_MS));
    }

    #[test]
    fn a_player_leaving_a_game_reads_their_hand_again() {
        let mut t = Table::default();
        for p in [1, 2, 3] {
            t.join(p).unwrap();
        }
        t.start(1, 7, 0).unwrap();
        assert_eq!(t.leave(2, 1), vec![2], "their cards went back to the deck");
        assert!(t.hand(2).cards.is_empty());
        assert_eq!(t.leave(3, 2), vec![3], "and when their leaving ends the game");
        assert_eq!(t.phase, Phase::Finished);
        let mut lobby = Table::default();
        lobby.join(1).unwrap();
        assert!(lobby.leave(1, 0).is_empty(), "nobody holds cards in the lobby");
    }

    #[test]
    fn the_configs_secret_changes_every_deal() {
        let open = deal_seed("tavern-2", 1_000, 3, "");
        assert_ne!(open, deal_seed("tavern-2", 1_000, 3, "s3cret"), "what a player sees is not the whole seed");
        assert_eq!(deal_seed("tavern-2", 1_000, 3, "s3cret"), deal_seed("tavern-2", 1_000, 3, "s3cret"));
        let deal = |secret: &str| {
            let mut t = Table::default();
            t.join(1).unwrap();
            t.join(2).unwrap();
            t.start(1, deal_seed("tavern-2", 1_000, 3, secret), 1_000).unwrap();
            t.hand(1).cards
        };
        assert_ne!(deal(""), deal("s3cret"));
    }

    #[test]
    fn the_hubs_deal_follows_its_secret() {
        let deal = |config: &[u8]| {
            let mut hub = TestHub::<TableHub>::spawn_with("deck", "t1", config).unwrap();
            for p in [1, 2] {
                let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
            }
            let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
            let hand: HandView = hub.call_typed(Caller::Player(1), "hand", &()).unwrap();
            hand.cards
        };
        assert_eq!(deal(br#"{"secret": "a"}"#), deal(br#"{"secret": "a"}"#), "the same table, clock and secret deal the same");
        assert_ne!(deal(br#"{"secret": "a"}"#), deal(br#"{"secret": "b"}"#), "another secret deals another game");
    }

    #[test]
    fn the_hub_keeps_its_configs_secret_and_never_shows_it() {
        {
            let plain = TestHub::<TableHub>::spawn("deck", "t1").unwrap();
            assert!(plain.hub().secret.is_empty());
        }
        assert!(TestHub::<TableHub>::spawn_with("deck", "t1", b"{").is_err(), "a config that is not JSON is refused");
        let mut hub = TestHub::<TableHub>::spawn_with("deck", "t1", br#"{"secret": "s3cret"}"#).unwrap();
        assert_eq!(hub.hub().secret, "s3cret");
        for p in [1, 2] {
            let _: bool = hub.call_typed(Caller::Player(p), "join", &()).unwrap();
        }
        let _: bool = hub.call_typed(Caller::Player(1), "start", &()).unwrap();
        let view: TableView = hub.call_typed(Caller::Player(2), "state", &()).unwrap();
        assert!(!format!("{:?}{:?}", hub.published(), view).contains("s3cret"));
    }
}
