//! A market, keyed by its name (`town`, `harbor`): vendors with fixed prices whose stock
//! restocks on a schedule, an order book for each item it lists, and trades between two players.
//! It holds nothing itself. What a player puts up waits in a hold in their own `player` hub
//! (`ckx_sdk::kit::econ::holds`), and the market drives each transfer, a settle from the giver's
//! hold then a deposit to the taker, as steps their hubs answer once by reference; it retries a
//! step until its hub answers. It is not a ledger (`ckx_sdk::kit::econ`).
//!
//! Each change it asks of a player's hub has a reference of the market's own (its key, what the
//! change is for, its placement's epoch and a count), never one built from what a client sent, so
//! a hub cannot take one change for another. Its key is at most [`KEY_MAX`] bytes.
//!
//! Endpoints (players): `vendors`, `buy { vendor, item, count }`, `sell { vendor, item, count }`,
//! `book { item }`, `order { item, side, price, count }`, `cancel { order }`, `orders`,
//! `trade { with }`, `offer { trade, bundle }`, `accept { trade }`, `decline { trade }`,
//! `trades`, `state` (anyone). A change that needs a player's hub and could not hear from it is
//! refused `unavailable`: send it again with the same `action_id`, and it finishes what it began.
//! It speaks the kit protocol (`ckx_sdk::kit`): the `market` topic carries the vendors' stock and
//! each book's best prices as `snapshot` messages, at most once a second, and `changed` messages
//! naming the players whose orders or trades moved (`what`: `orders`, `trades`);
//! `snapshot { topic: "market" }` answers it; refusals carry `kit:<code>:` codes.

use std::collections::{BTreeMap, BTreeSet, VecDeque};

use ckx_sdk::kit::econ::orderbook::{OrderBook, Side};
use ckx_sdk::kit::econ::vendor::{Offer, Shelf, Vendor};
use ckx_sdk::kit::econ::{self, Bundle};
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;
use serde_json::{Value, json};

pub const TOPIC: &str = "market";
const PUBLISH_MS: u64 = 1_000;
/// How often unfinished steps are tried again.
const STEPS_MS: u64 = 1_000;
/// Steps one call drives before it answers; the timer drives the rest.
const STEPS_PER_CALL: usize = 16;
/// A change whose answer was lost, and that no resend finished, is forgotten after this long; a
/// hold among them is given back.
const PENDING_MS: u64 = 60_000;
/// How long a player whose hub did not answer a step waits before it is asked again, doubling up to the longest.
const RETRY_MIN_MS: u64 = 1_000;
const RETRY_MAX_MS: u64 = 30_000;
const DEPTH: usize = 10;
/// The longest key a market takes.
pub const KEY_MAX: usize = 48;
// Its longest reference (key, `:t` and a trade id, `:` epoch `.` count, a step's `:s`) fits a
// player's hub.
const _: () = assert!(KEY_MAX + 2 + 20 + 1 + 20 + 1 + 20 + 2 <= econ::REFERENCE_MAX);

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct Config {
    pub vendors: Vec<Vendor>,
    /// The items with an order book, and the currency they trade in.
    pub book_items: Vec<String>,
    pub book_currency: String,
    pub orders_max: usize,
    /// The open and settling trades a player may be in, so nobody can fill another player's trades.
    pub trades_max: usize,
    /// How long a trade stays open.
    pub trade_ms: u64,
    /// The node type of the players' hubs.
    pub player_type: String,
}

impl Default for Config {
    fn default() -> Self {
        let offer = |item: &str, price: i64, stock: Option<u64>, per_player: Option<u64>, buys_for: Option<i64>| Offer {
            item: item.into(),
            price: Bundle::coins("gold", price),
            stock,
            per_player,
            buys_for: buys_for.map(|p| Bundle::coins("gold", p)),
        };
        Self {
            vendors: vec![Vendor {
                id: "smith".into(),
                offers: vec![offer("sword", 50, Some(5), Some(1), Some(20)), offer("potion", 5, None, None, Some(2))],
                restock: Some("0 * * * *".into()),
            }],
            book_items: vec!["wood".into(), "plank".into(), "ore".into(), "ingot".into()],
            book_currency: "gold".into(),
            orders_max: 20,
            trades_max: 10,
            trade_ms: 5 * 60_000,
            player_type: "player".into(),
        }
    }
}

/// An order the market keeps beside its book entry.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct OrderMeta {
    pub id: u64,
    pub player: u64,
    pub item: String,
    pub side: Side,
    pub price: i64,
    pub count: u64,
    /// What is still open.
    pub open: u64,
    /// The hold that backs it, in the player's hub.
    pub hold: String,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum TradeState {
    Open,
    Settling,
    Done,
    Cancelled,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct Side_ {
    pub bundle: Bundle,
    pub hold: Option<String>,
    pub accepted: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Trade {
    pub id: u64,
    pub players: [u64; 2],
    pub sides: [Side_; 2],
    pub state: TradeState,
    pub expires_at: u64,
    /// Offers made so far.
    pub offers: u64,
}

impl Trade {
    fn side(&self, player: u64) -> Option<usize> {
        self.players.iter().position(|p| *p == player)
    }
}

/// One step of a transfer, for a player's hub.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Step {
    /// Takes `bundle` out of `hold` in `from`'s hub, then gives it to `to`: the settle by `ref`
    /// and `:s`, the deposit by `ref` and `:d`.
    Transfer { from: u64, to: u64, hold: String, bundle: Bundle, r#ref: String, settled: bool, trade: Option<u64> },
    /// Gives what is left of `hold` back to `player`.
    Release { player: u64, hold: String, r#ref: String, trade: Option<u64> },
}

impl Step {
    fn player(&self) -> u64 {
        match self {
            Step::Transfer { from, to, settled, .. } => {
                if *settled {
                    *to
                } else {
                    *from
                }
            }
            Step::Release { player, .. } => *player,
        }
    }

    fn trade(&self) -> Option<u64> {
        match self {
            Step::Transfer { trade, .. } | Step::Release { trade, .. } => *trade,
        }
    }
}

/// Who to tell, and what changed for them.
type Tell = Vec<(u64, &'static str)>;

/// What a player's hub said.
enum Heard {
    Done(Vec<u8>),
    Refused(String),
    /// It may or may not have run: ask again.
    Unknown(String),
}

/// How a change asked of a player's hub failed.
enum Failed {
    /// The hub refused it.
    Refused(Error),
    /// The hub may have run it; its answer was lost.
    Lost(Error),
    /// It was never asked.
    Rejected(Error),
}

impl Failed {
    fn into_error(self) -> Error {
        match self {
            Failed::Refused(e) | Failed::Lost(e) | Failed::Rejected(e) => e,
        }
    }
}

/// A change asked of a player's hub whose answer was lost: it may or may not have run.
#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Unknown {
    pub player: u64,
    /// The `action_id` of the call that asked: a resend with it asks again by the same reference.
    pub action: Option<String>,
    pub method: String,
    pub args: Value,
    pub at: u64,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(default)]
pub struct Market {
    pub config: Config,
    pub shelves: BTreeMap<String, Shelf>,
    pub books: BTreeMap<String, OrderBook>,
    pub orders: BTreeMap<u64, OrderMeta>,
    /// Each player's open orders, by id.
    pub by_player: BTreeMap<u64, BTreeSet<u64>>,
    /// Open and settling trades; a finished one is removed.
    pub trades: BTreeMap<u64, Trade>,
    pub steps: VecDeque<Step>,
    /// When a player whose hub did not answer a step is asked again, and how long the next miss waits.
    pub retry_at: BTreeMap<u64, u64>,
    pub retry_gap: BTreeMap<u64, u64>,
    /// Changes asked of a player's hub whose answer was lost, by reference.
    pub unknown: BTreeMap<String, Unknown>,
    /// The last trade price of each item.
    pub last: BTreeMap<String, i64>,
    pub next_id: u64,
    pub next_ref: u64,
    /// Steps a hub refused, which nothing will retry; an operator reads `state`.
    pub stuck: u64,
    pub dirty: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct OfferView {
    pub item: String,
    pub price: Bundle,
    /// Left on the shelf; none for no limit.
    pub stock: Option<u64>,
    pub per_player: Option<u64>,
    pub buys_for: Option<Bundle>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct VendorView {
    pub id: String,
    pub offers: Vec<OfferView>,
    pub restocked_at: u64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Quote {
    pub bid: Option<i64>,
    pub ask: Option<i64>,
    pub last: Option<i64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct MarketView {
    pub vendors: Vec<VendorView>,
    pub currency: String,
    pub quotes: BTreeMap<String, Quote>,
    pub open_trades: usize,
    pub pending_steps: usize,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct BookView {
    pub item: String,
    pub currency: String,
    pub bids: Vec<(i64, u64)>,
    pub asks: Vec<(i64, u64)>,
    pub last: Option<i64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Placed {
    pub order: u64,
    /// `(price, count)` it traded at once.
    pub fills: Vec<(i64, u64)>,
    /// What rests on the book.
    pub open: u64,
}

#[derive(Deserialize)]
struct Shop {
    vendor: String,
    item: String,
    #[serde(default = "one")]
    count: u64,
}

fn one() -> u64 {
    1
}

#[derive(Deserialize)]
struct Item {
    item: String,
}

#[derive(Deserialize)]
struct Place {
    item: String,
    side: Side,
    price: i64,
    count: u64,
}

#[derive(Deserialize)]
struct Cancel {
    order: u64,
}

#[derive(Deserialize)]
struct With {
    with: u64,
}

#[derive(Deserialize)]
struct OfferArgs {
    trade: u64,
    #[serde(default)]
    bundle: Bundle,
}

#[derive(Deserialize)]
struct TradeArgs {
    trade: u64,
}

fn definite(status: Status) -> bool {
    matches!(status, Status::AppError | Status::NotFound | Status::Denied | Status::BadRequest)
}

impl Market {
    pub fn new(config: Config, now: u64) -> Self {
        let mut m = Market { config, dirty: true, ..Market::default() };
        for v in m.config.vendors.clone() {
            m.shelves.entry(v.id.clone()).or_default().restock(&v, now);
        }
        m
    }

    fn vendor(&self, id: &str) -> Result<&Vendor> {
        self.config.vendors.iter().find(|v| v.id == id).ok_or_else(|| kit::error(codes::NOT_FOUND, format!("there is no vendor `{id}`")))
    }

    pub fn view(&self) -> MarketView {
        MarketView {
            vendors: self.vendors(),
            currency: self.config.book_currency.clone(),
            quotes: self
                .config
                .book_items
                .iter()
                .map(|i| {
                    let book = self.books.get(i);
                    (
                        i.clone(),
                        Quote {
                            bid: book.and_then(|b| b.bids.first()).map(|o| o.price),
                            ask: book.and_then(|b| b.asks.first()).map(|o| o.price),
                            last: self.last.get(i).copied(),
                        },
                    )
                })
                .collect(),
            open_trades: self.trades.values().filter(|t| t.state == TradeState::Open).count(),
            pending_steps: self.steps.len(),
        }
    }

    pub fn vendors(&self) -> Vec<VendorView> {
        self.config
            .vendors
            .iter()
            .map(|v| {
                let shelf = self.shelves.get(&v.id);
                VendorView {
                    id: v.id.clone(),
                    offers: v
                        .offers
                        .iter()
                        .map(|o| OfferView {
                            item: o.item.clone(),
                            price: o.price.clone(),
                            stock: o.stock.map(|_| shelf.and_then(|s| s.stock.get(&o.item)).copied().unwrap_or(0)),
                            per_player: o.per_player,
                            buys_for: o.buys_for.clone(),
                        })
                        .collect(),
                    restocked_at: shelf.map(|s| s.restocked_at).unwrap_or(0),
                }
            })
            .collect()
    }

    pub fn book(&self, item: &str) -> BookView {
        let book = self.books.get(item).cloned().unwrap_or_default();
        BookView {
            item: item.to_owned(),
            currency: self.config.book_currency.clone(),
            bids: book.depth(Side::Buy, DEPTH),
            asks: book.depth(Side::Sell, DEPTH),
            last: self.last.get(item).copied(),
        }
    }

    pub fn orders_of(&self, player: u64) -> Vec<OrderMeta> {
        let Some(ids) = self.by_player.get(&player) else { return Vec::new() };
        ids.iter().filter_map(|id| self.orders.get(id)).filter(|o| o.open > 0).cloned().collect()
    }

    /// Rebuilds the per-player order index, for a snapshot taken before it existed.
    pub fn reindex(&mut self) {
        self.by_player.clear();
        for o in self.orders.values() {
            self.by_player.entry(o.player).or_default().insert(o.id);
        }
    }

    fn forget_order(&mut self, id: u64) {
        let Some(meta) = self.orders.remove(&id) else { return };
        let Some(ids) = self.by_player.get_mut(&meta.player) else { return };
        ids.remove(&id);
        if ids.is_empty() {
            self.by_player.remove(&meta.player);
        }
    }

    /// Takes back a sale counted for an exchange the player's hub refused.
    fn unsell(&mut self, vendor: &str, offer: &Offer, player: u64, count: u64) {
        let Some(shelf) = self.shelves.get_mut(vendor) else { return };
        if let (Some(cap), Some(stock)) = (offer.stock, shelf.stock.get_mut(&offer.item)) {
            *stock = stock.saturating_add(count).min(cap);
        }
        let key = format!("{player}:{}", offer.item);
        let left = shelf.bought.get(&key).copied().unwrap_or(0).saturating_sub(count);
        if left == 0 {
            shelf.bought.remove(&key);
        } else {
            shelf.bought.insert(key, left);
        }
        self.dirty = true;
    }

    fn trades_in(&self, player: u64) -> usize {
        self.trades.values().filter(|t| t.players.contains(&player) && matches!(t.state, TradeState::Open | TradeState::Settling)).count()
    }

    pub fn trades_of(&self, player: u64) -> Vec<Trade> {
        self.trades
            .values()
            .filter(|t| t.players.contains(&player) && matches!(t.state, TradeState::Open | TradeState::Settling))
            .cloned()
            .collect()
    }

    /// A reference no other change of this market has: its key, what it is for, and its
    /// placement's epoch with a count. A placement raises the epoch, so a count a crash took back
    /// is never sent again.
    fn fresh(&mut self, ctx: &Ctx, what: &str) -> String {
        self.next_ref += 1;
        format!("{}:{what}:{}.{}", ctx.key, ctx.epoch, self.next_ref)
    }

    fn hear(&self, ctx: &Ctx, player: u64, method: &str, args: &Value) -> Heard {
        let payload = match encode(args) {
            Ok(p) => p,
            Err(e) => return Heard::Refused(e.0),
        };
        match ctx.call(&self.config.player_type, &player.to_string(), method, &payload) {
            Ok(reply) => Heard::Done(reply),
            Err(e) if definite(e.status) => Heard::Refused(e.message),
            Err(e) => Heard::Unknown(e.message),
        }
    }

    /// Asks `player`'s hub for a change a client's call needs now, by a fresh reference; the hub's
    /// refusal is the call's. A lost answer refuses the call `unavailable` and keeps the change
    /// with the call's `action_id`: a resend with it and the same arguments asks again by the
    /// same reference, so the hub runs it once, and one with other arguments is refused
    /// `conflict`. Answers the reference and the hub's reply.
    fn change(&mut self, ctx: &Ctx, call: &Call<'_>, player: u64, what: &str, method: &str, args: Value) -> Result<(String, Vec<u8>)> {
        self.ask(ctx, call, player, what, method, args).map_err(Failed::into_error)
    }

    /// Whether `call` is a resend of a change whose answer was lost.
    fn is_resend(&self, call: &Call<'_>, player: u64) -> Result<bool> {
        let Some(action) = Receipts::action_id(call.payload)? else { return Ok(false) };
        Ok(self.unknown.values().any(|u| u.player == player && u.action.as_deref() == Some(action.as_str())))
    }

    /// [`Self::change`], saying how it failed.
    fn ask(&mut self, ctx: &Ctx, call: &Call<'_>, player: u64, what: &str, method: &str, args: Value) -> std::result::Result<(String, Vec<u8>), Failed> {
        let action = Receipts::action_id(call.payload).map_err(Failed::Rejected)?;
        let resent = action.as_deref().and_then(|a| {
            self.unknown
                .iter()
                .find(|(_, u)| u.player == player && u.action.as_deref() == Some(a))
                .map(|(r, u)| (r.clone(), u.method == method && u.args == args))
        });
        let reference = match resent {
            Some((r, true)) => r,
            Some((_, false)) => {
                return Err(Failed::Rejected(kit::error(codes::CONFLICT, "that action_id asked for another change; send this one with its own")));
            }
            None => self.fresh(ctx, what),
        };
        let mut sent = args.clone();
        sent["ref"] = Value::String(reference.clone());
        match self.hear(ctx, player, method, &sent) {
            Heard::Done(reply) => {
                self.unknown.remove(&reference);
                Ok((reference, reply))
            }
            Heard::Refused(message) => {
                self.unknown.remove(&reference);
                Err(Failed::Refused(Error::new(message)))
            }
            Heard::Unknown(message) => {
                if action.is_some() || method == "hold" {
                    self.unknown.insert(reference, Unknown { player, action, method: method.to_owned(), args, at: ctx.now_ms() });
                }
                Err(Failed::Lost(kit::error(codes::UNAVAILABLE, format!("the player's hub did not answer ({message}); send it again"))))
            }
        }
    }

    /// Drives the steps, each player's in order; a player whose hub did not answer waits. Answers
    /// the players whose trades it finished, to tell.
    pub fn drive(&mut self, ctx: &Ctx, budget: usize) -> Result<Tell> {
        let mut tell = Tell::new();
        let mut waiting: BTreeSet<u64> = BTreeSet::new();
        let mut touched: BTreeSet<u64> = BTreeSet::new();
        let mut finished_trades: BTreeSet<u64> = BTreeSet::new();
        let (mut i, mut tries) = (0, 0);
        let now = ctx.now_ms();
        while i < self.steps.len() && tries < budget {
            let player = self.steps[i].player();
            if waiting.contains(&player) || self.retry_at.get(&player).is_some_and(|at| now < *at) {
                i += 1;
                continue;
            }
            tries += 1;
            let (method, args) = match &self.steps[i] {
                Step::Transfer { hold, bundle, r#ref, settled: false, .. } => {
                    ("settle", json!({ "ref": format!("{ref}:s"), "hold": hold, "bundle": bundle }))
                }
                Step::Transfer { bundle, r#ref, settled: true, .. } => ("deposit", json!({ "ref": format!("{ref}:d"), "bundle": bundle })),
                Step::Release { hold, r#ref, .. } => ("release", json!({ "ref": r#ref, "hold": hold })),
            };
            match self.hear(ctx, player, method, &args) {
                Heard::Unknown(message) => {
                    ctx.log(Level::Warn, &format!("step for player {player} unanswered ({message}); trying again"));
                    waiting.insert(player);
                    let gap = self.retry_gap.get(&player).map_or(RETRY_MIN_MS, |g| (g * 2).min(RETRY_MAX_MS));
                    self.retry_gap.insert(player, gap);
                    self.retry_at.insert(player, now + gap);
                    i += 1;
                    continue;
                }
                Heard::Refused(message) => {
                    ctx.log(Level::Error, &format!("player {player}'s hub refused {method} ({message}); the transfer is dropped"));
                    self.stuck += 1;
                    self.retry_gap.remove(&player);
                    self.retry_at.remove(&player);
                }
                Heard::Done(_) => {
                    self.retry_gap.remove(&player);
                    self.retry_at.remove(&player);
                    if let Step::Transfer { settled: settled @ false, .. } = &mut self.steps[i] {
                        *settled = true;
                        touched.insert(player);
                        continue;
                    }
                }
            }
            touched.insert(player);
            let step = self.steps.remove(i).expect("the step at i");
            if let Some(t) = step.trade()
                && !self.steps.iter().any(|s| s.trade() == Some(t))
            {
                finished_trades.insert(t);
            }
        }
        for t in finished_trades {
            if self.trades.get(&t).is_some_and(|trade| trade.state == TradeState::Settling)
                && let Some(trade) = self.trades.remove(&t)
            {
                touched.extend(trade.players);
                tell.extend(trade.players.iter().map(|p| (*p, "trades")));
            }
        }
        if !touched.is_empty() {
            self.dirty = true;
        }
        Ok(tell)
    }

    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<(Vec<u8>, Tell)> {
        let player = call.player()?;
        match call.method {
            "buy" | "sell" => {
                let s: Shop = kit::args(call)?;
                let vendor = self.vendor(&s.vendor)?.clone();
                let offer = vendor.offer(&s.item)?.clone();
                let buying = call.method == "buy";
                let resent = buying && self.is_resend(call, player)?;
                let (pay, get) = if buying {
                    if !resent {
                        self.shelves.entry(vendor.id.clone()).or_default().check(&offer, player, s.count)?;
                    }
                    (offer.price.times(s.count)?, Bundle::item(&offer.item, s.count))
                } else {
                    let price = offer
                        .buys_for
                        .clone()
                        .ok_or_else(|| kit::error(codes::FORBIDDEN, format!("`{}` does not buy `{}`", vendor.id, offer.item)))?;
                    (Bundle::item(&offer.item, s.count), price.times(s.count)?)
                };
                let counted = buying && !resent;
                if counted {
                    self.shelves.entry(vendor.id.clone()).or_default().sold(&offer, player, s.count);
                    self.dirty = true;
                }
                if let Err(failed) = self.ask(ctx, call, player, &call.method[..1], "exchange", json!({ "pay": pay, "get": get })) {
                    let undo = match failed {
                        Failed::Refused(_) => buying,
                        Failed::Rejected(_) => counted,
                        Failed::Lost(_) => false,
                    };
                    if undo {
                        self.unsell(&vendor.id, &offer, player, s.count);
                    }
                    return Err(failed.into_error());
                }
                self.dirty = true;
                Ok((encode(&get)?, vec![]))
            }
            "order" => {
                let p: Place = kit::args(call)?;
                if !self.config.book_items.contains(&p.item) {
                    return Err(kit::error(codes::NOT_FOUND, format!("`{}` has no book here", p.item)));
                }
                if p.price <= 0 || p.count == 0 {
                    return Err(kit::error(codes::BAD_REQUEST, "a price and a count are positive"));
                }
                if self.by_player.get(&player).map_or(0, |ids| ids.len()) >= self.config.orders_max {
                    return Err(kit::error(codes::LIMIT, format!("{} open orders at most", self.config.orders_max)));
                }
                let held = match p.side {
                    Side::Buy => Bundle::coins(
                        &self.config.book_currency,
                        p.price
                            .checked_mul(i64::try_from(p.count).unwrap_or(i64::MAX))
                            .ok_or_else(|| kit::error(codes::LIMIT, "too much"))?,
                    ),
                    Side::Sell => Bundle::item(&p.item, p.count),
                };
                let (hold, _) = self.change(ctx, call, player, "o", "hold", json!({ "bundle": held }))?;
                self.next_id += 1;
                let id = self.next_id;
                let fills = self.books.entry(p.item.clone()).or_default().place(id, player, p.side, p.price, p.count, now)?;
                let mut tell = vec![(player, "orders")];
                let filled: u64 = fills.iter().map(|f| f.count).sum();
                self.orders.insert(
                    id,
                    OrderMeta { id, player, item: p.item.clone(), side: p.side, price: p.price, count: p.count, open: p.count, hold },
                );
                self.by_player.entry(player).or_default().insert(id);
                for f in &fills {
                    let (buy, sell) = (self.orders[&f.buy_order].clone(), self.orders[&f.sell_order].clone());
                    let (items_ref, coins_ref) = (self.fresh(ctx, "x"), self.fresh(ctx, "x"));
                    self.steps.push_back(Step::Transfer {
                        from: f.seller,
                        to: f.buyer,
                        hold: sell.hold.clone(),
                        bundle: Bundle::item(&p.item, f.count),
                        r#ref: items_ref,
                        settled: false,
                        trade: None,
                    });
                    self.steps.push_back(Step::Transfer {
                        from: f.buyer,
                        to: f.seller,
                        hold: buy.hold.clone(),
                        bundle: Bundle::coins(
                            &self.config.book_currency,
                            f.price.saturating_mul(i64::try_from(f.count).unwrap_or(i64::MAX)),
                        ),
                        r#ref: coins_ref,
                        settled: false,
                        trade: None,
                    });
                    for o in [f.buy_order, f.sell_order] {
                        let meta = self.orders.get_mut(&o).expect("a filled order");
                        meta.open -= f.count;
                        if meta.open == 0 {
                            let (owner, hold) = (meta.player, meta.hold.clone());
                            self.steps.push_back(Step::Release { player: owner, r#ref: format!("{hold}:r"), hold, trade: None });
                        }
                    }
                    tell.push((f.buyer, "orders"));
                    tell.push((f.seller, "orders"));
                    self.last.insert(p.item.clone(), f.price);
                }
                for f in &fills {
                    for o in [f.buy_order, f.sell_order] {
                        if self.orders.get(&o).is_some_and(|meta| meta.open == 0) {
                            self.forget_order(o);
                        }
                    }
                }
                self.dirty = true;
                Ok((
                    encode(&Placed { order: id, fills: fills.iter().map(|f| (f.price, f.count)).collect(), open: p.count - filled })?,
                    tell,
                ))
            }
            "cancel" => {
                let Cancel { order } = kit::args(call)?;
                let meta = self.orders.get(&order).filter(|o| o.player == player).cloned();
                let meta = meta.ok_or_else(|| kit::error(codes::NOT_FOUND, format!("you have no open order {order}")))?;
                if let Some(book) = self.books.get_mut(&meta.item) {
                    book.cancel(order, player);
                }
                self.forget_order(order);
                self.steps.push_back(Step::Release { player, r#ref: format!("{}:r", meta.hold), hold: meta.hold, trade: None });
                self.dirty = true;
                Ok((encode(&true)?, vec![(player, "orders")]))
            }
            "trade" => {
                let With { with } = kit::args(call)?;
                if with == player {
                    return Err(kit::error(codes::BAD_REQUEST, "a trade is between two players"));
                }
                let open = self.trades.values().find(|t| t.state == TradeState::Open && t.players.contains(&player) && t.players.contains(&with));
                if let Some(open) = open {
                    return Ok((encode(&open.id)?, vec![]));
                }
                if self.trades_in(player) >= self.config.trades_max {
                    return Err(kit::error(codes::LIMIT, format!("{} open trades at most", self.config.trades_max)));
                }
                if self.trades_in(with) >= self.config.trades_max {
                    return Err(kit::error(codes::LIMIT, format!("player {with} is in {} open trades already", self.config.trades_max)));
                }
                self.next_id += 1;
                let id = self.next_id;
                let trade = Trade {
                    id,
                    players: [player, with],
                    sides: Default::default(),
                    state: TradeState::Open,
                    expires_at: now + self.config.trade_ms,
                    offers: 0,
                };
                self.trades.insert(id, trade);
                self.dirty = true;
                Ok((encode(&id)?, vec![(player, "trades"), (with, "trades")]))
            }
            "offer" => {
                let o: OfferArgs = kit::args(call)?;
                let (side, others) = self.open_trade(o.trade, player)?;
                let previous = self.trades[&o.trade].sides[side].hold.clone();
                let hold = if o.bundle.is_empty() {
                    None
                } else {
                    Some(self.change(ctx, call, player, &format!("t{}", o.trade), "hold", json!({ "bundle": o.bundle }))?.0)
                };
                if let Some(h) = previous {
                    self.steps.push_back(Step::Release { player, r#ref: format!("{h}:r"), hold: h, trade: None });
                }
                let t = self.trades.get_mut(&o.trade).expect("an open trade");
                t.offers += 1;
                t.sides[side] = Side_ { bundle: o.bundle.clone().trimmed(), hold, accepted: false };
                t.sides[1 - side].accepted = false;
                Ok((encode(&true)?, vec![(player, "trades"), (others, "trades")]))
            }
            "accept" => {
                let TradeArgs { trade } = kit::args(call)?;
                let (side, others) = self.open_trade(trade, player)?;
                let t = self.trades.get_mut(&trade).expect("an open trade");
                if t.sides.iter().all(|s| s.bundle.is_empty()) {
                    return Err(kit::error(codes::NOT_READY, "nothing is offered yet"));
                }
                t.sides[side].accepted = true;
                if t.sides.iter().all(|s| s.accepted) {
                    t.state = TradeState::Settling;
                    let t = t.clone();
                    for (i, s) in t.sides.iter().enumerate() {
                        if let Some(hold) = &s.hold {
                            let r#ref = self.fresh(ctx, "x");
                            self.steps.push_back(Step::Transfer {
                                from: t.players[i],
                                to: t.players[1 - i],
                                hold: hold.clone(),
                                bundle: s.bundle.clone(),
                                r#ref,
                                settled: false,
                                trade: Some(t.id),
                            });
                        }
                    }
                }
                self.dirty = true;
                Ok((encode(&self.trades[&trade].state)?, vec![(player, "trades"), (others, "trades")]))
            }
            "decline" => {
                let TradeArgs { trade } = kit::args(call)?;
                let (_, others) = self.open_trade(trade, player)?;
                self.call_off(trade);
                Ok((encode(&true)?, vec![(player, "trades"), (others, "trades")]))
            }
            other => Err(Error::unknown_method(other)),
        }
    }

    /// The caller's side of open trade `id`, and the other player.
    fn open_trade(&self, id: u64, player: u64) -> Result<(usize, u64)> {
        let t = self.trades.get(&id).ok_or_else(|| kit::error(codes::NOT_FOUND, format!("there is no trade {id}")))?;
        let side = t.side(player).ok_or_else(|| kit::error(codes::FORBIDDEN, "it is not your trade"))?;
        if t.state != TradeState::Open {
            return Err(kit::error(codes::FINISHED, "the trade is settled or called off"));
        }
        Ok((side, t.players[1 - side]))
    }

    fn call_off(&mut self, id: u64) {
        let Some(t) = self.trades.remove(&id) else { return };
        for (i, s) in t.sides.iter().enumerate() {
            if let Some(hold) = &s.hold {
                self.steps.push_back(Step::Release {
                    player: t.players[i],
                    r#ref: format!("{hold}:r"),
                    hold: hold.clone(),
                    trade: Some(id),
                });
            }
        }
        self.dirty = true;
    }

    /// Expires trades, and forgets changes whose answer was lost, giving back the holds among them.
    /// Answers the players whose trades it called off, to tell.
    fn sweep(&mut self, now: u64) -> Tell {
        let expired: Vec<(u64, [u64; 2])> =
            self.trades.values().filter(|t| t.state == TradeState::Open && now >= t.expires_at).map(|t| (t.id, t.players)).collect();
        let mut tell = Tell::new();
        for (id, players) in expired {
            self.call_off(id);
            tell.extend(players.iter().map(|p| (*p, "trades")));
        }
        let stale: Vec<String> =
            self.unknown.iter().filter(|(_, u)| now.saturating_sub(u.at) >= PENDING_MS).map(|(r, _)| r.clone()).collect();
        for r in stale {
            let u = self.unknown.remove(&r).expect("a stale change");
            if u.method == "hold" {
                self.steps.push_back(Step::Release { player: u.player, r#ref: format!("{r}:r"), hold: r, trade: None });
            }
        }
        tell
    }
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct MarketHub {
    #[serde(flatten)]
    pub market: Market,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl MarketHub {
    fn arm(&self, ctx: &Ctx) -> Result<()> {
        for v in &self.market.config.vendors {
            if let Some(expr) = &v.restock {
                ctx.timer_cron(&format!("restock:{}", v.id), &Cron::parse(expr).map_err(|e| kit::error(codes::BAD_REQUEST, e))?)?;
            }
        }
        Ok(())
    }

    fn tell(&mut self, ctx: &Ctx, tell: &[(u64, &'static str)]) -> Result<()> {
        let mut by_what: BTreeMap<&str, Vec<u64>> = BTreeMap::new();
        for (p, what) in tell {
            let who = by_what.entry(what).or_default();
            if !who.contains(p) {
                who.push(*p);
            }
        }
        for (what, who) in by_what {
            self.kit.changed(ctx, TOPIC, &who, what)?;
        }
        Ok(())
    }
}

impl Hub for MarketHub {
    fn spawn(ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        if ctx.key.len() > KEY_MAX {
            return Err(kit::error(codes::BAD_REQUEST, format!("a market's key is at most {KEY_MAX} bytes")));
        }
        let config = if seed.is_empty() {
            Config::default()
        } else {
            serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?
        };
        let hub = MarketHub { market: Market::new(config, ctx.now_ms()), ..MarketHub::default() };
        hub.arm(ctx)?;
        ctx.timer_every("publish", PUBLISH_MS)?;
        ctx.timer_every("steps", STEPS_MS)?;
        Ok(hub)
    }

    fn load(_ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        let mut hub: Self = decode(snapshot)?;
        hub.market.reindex();
        Ok(hub)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        let m = &self.market;
        match call.method {
            "state" => return encode(&m.view()),
            "vendors" => return encode(&m.vendors()),
            "book" => {
                let Item { item } = kit::args(&call)?;
                return encode(&m.book(&item));
            }
            "orders" => return encode(&m.orders_of(call.player()?)),
            "trades" => return encode(&m.trades_of(call.player()?)),
            kit::SNAPSHOT_METHOD => return self.kit.snapshot_reply(ctx, TOPIC, &m.view()),
            _ => {}
        }
        let mut tell = Vec::new();
        let reply = Receipts::run(
            self,
            |h| &mut h.receipts,
            &call,
            now,
            |h| {
                let (reply, t) = h.market.act(ctx, &call, now)?;
                tell = t;
                Ok(reply)
            },
        )?;
        tell.extend(self.market.drive(ctx, STEPS_PER_CALL)?);
        self.tell(ctx, &tell)?;
        Ok(reply)
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        let now = ctx.now_ms();
        if let Some(id) = timer.strip_prefix("restock:") {
            if let Some(v) = self.market.config.vendors.iter().find(|v| v.id == id).cloned() {
                self.market.shelves.entry(v.id.clone()).or_default().restock(&v, now);
                self.market.dirty = true;
            }
            return Ok(());
        }
        match timer {
            "steps" => {
                let mut tell = self.market.sweep(now);
                if !self.market.steps.is_empty() {
                    tell.extend(self.market.drive(ctx, STEPS_PER_CALL * 4)?);
                }
                self.tell(ctx, &tell)?;
            }
            "publish" if self.market.dirty => {
                self.market.dirty = false;
                self.kit.snapshot(ctx, TOPIC, &self.market.view())?;
            }
            _ => {}
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(MarketHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::CallError;
    use ckx_sdk::kit::econ::items::{Catalog, ItemDef};
    use ckx_sdk::kit::econ::{Applied, Holdings, Holds};
    use ckx_sdk::kit::error_code;
    use ckx_sdk::testing::{Message as Sent, TestHub};
    use std::cell::RefCell;
    use std::rc::Rc;

    /// The players' hubs as the market sees them: holdings, holds and the references answered.
    #[derive(Default)]
    struct Players {
        catalog: Catalog,
        of: BTreeMap<u64, (Holdings, Holds)>,
        applied: Applied,
        /// Players whose hub does not answer.
        down: BTreeSet<u64>,
        /// Players whose hub runs what it is asked, but whose answer is lost.
        lost: BTreeSet<u64>,
        /// How many times each player's hub was asked.
        asked: BTreeMap<u64, u64>,
    }

    impl Players {
        fn new(players: &[(u64, Bundle)]) -> Rc<RefCell<Self>> {
            let catalog = Catalog::from(
                ["wood", "plank", "ore", "ingot", "potion"]
                    .iter()
                    .map(|i| ItemDef { id: (*i).into(), max_stack: 99, tradable: true, name: String::new() })
                    .chain(std::iter::once(ItemDef { id: "sword".into(), max_stack: 1, tradable: true, name: String::new() }))
                    .collect::<Vec<_>>(),
            );
            let of = players
                .iter()
                .map(|(p, b)| {
                    let mut h = Holdings::new(20);
                    h.give(&catalog, b).unwrap();
                    (*p, (h, Holds::default()))
                })
                .collect();
            Rc::new(RefCell::new(Players { catalog, of, ..Players::default() }))
        }

        fn answer(&mut self, m: &Sent) -> std::result::Result<Vec<u8>, CallError> {
            let player: u64 = m.key.parse().unwrap();
            *self.asked.entry(player).or_default() += 1;
            if self.down.contains(&player) {
                return Err(CallError { status: Status::DeadlineExceeded, message: "no reply".into() });
            }
            let args: Value = decode(&m.payload).unwrap();
            let reference = args["ref"].as_str().unwrap().to_owned();
            if let Some(replayed) = self.applied.replay(&reference, 0) {
                return replayed.map_err(|e| CallError { status: Status::AppError, message: e.0 });
            }
            let catalog = self.catalog.clone();
            let (holdings, holds) = self.of.get_mut(&player).unwrap();
            let bundle = |k: &str| -> Bundle { serde_json::from_value(args[k].clone()).unwrap_or_default() };
            let result: Result<Vec<u8>> = (|| match m.method.as_str() {
                "hold" => holds.hold(holdings, &reference, &bundle("bundle")).and_then(|_| encode(&true)),
                "settle" => holds.settle(args["hold"].as_str().unwrap(), &bundle("bundle")).and_then(|_| encode(&true)),
                "release" => encode(&holds.release(holdings, &catalog, args["hold"].as_str().unwrap())?),
                "deposit" => holdings.give_or_mail(&catalog, &bundle("bundle")).and_then(|_| encode(&true)),
                "exchange" => holdings.swap(&catalog, &bundle("pay"), &bundle("get")).and_then(|_| encode(&true)),
                other => panic!("the market asked for {other}"),
            })();
            self.applied.record(&reference, &result, 0);
            if self.lost.contains(&player) {
                return Err(CallError { status: Status::DeadlineExceeded, message: "the answer was lost".into() });
            }
            result.map_err(|e| CallError { status: Status::AppError, message: e.0 })
        }

        fn gold(&self, p: u64) -> i64 {
            self.of[&p].0.wallet.balance("gold")
        }

        fn count(&self, p: u64, item: &str) -> u64 {
            self.of[&p].0.inventory.count(item)
        }
    }

    fn market(players: &Rc<RefCell<Players>>) -> TestHub<MarketHub> {
        let hub = TestHub::<MarketHub>::spawn("market", "town").unwrap();
        let p = players.clone();
        hub.answer_calls("player", move |m| p.borrow_mut().answer(m));
        hub
    }

    fn code(e: &CallError) -> Option<&str> {
        error_code(&e.message).map(|c| c.0)
    }

    #[test]
    fn a_vendor_sells_from_its_stock_once_a_player_and_buys_back() {
        let players = Players::new(&[(1, Bundle::coins("gold", 120)), (2, Bundle::coins("gold", 10))]);
        let mut m = market(&players);
        let got: Bundle =
            m.call_typed(Caller::Player(1), "buy", &json!({ "vendor": "smith", "item": "sword", "action_id": "b1" })).unwrap();
        assert_eq!(got, Bundle::item("sword", 1));
        let _: Bundle = m.call_typed(Caller::Player(1), "buy", &json!({ "vendor": "smith", "item": "sword", "action_id": "b1" })).unwrap();
        assert_eq!(players.borrow().gold(1), 70, "a resent buy is answered, not bought again");
        let e = m.call(Caller::Player(1), "buy", &encode(&json!({ "vendor": "smith", "item": "sword" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::LIMIT), "one a player until the restock");
        let e = m.call(Caller::Player(2), "buy", &encode(&json!({ "vendor": "smith", "item": "sword" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::INSUFFICIENT), "player 2 has 10 gold");
        let paid: Bundle = m.call_typed(Caller::Player(1), "sell", &json!({ "vendor": "smith", "item": "sword" })).unwrap();
        assert_eq!(paid, Bundle::coins("gold", 20));
        let v: Vec<VendorView> = m.call_typed(Caller::Player(2), "vendors", &()).unwrap();
        assert_eq!(v[0].offers[0].stock, Some(4));
        m.advance(3_600_000).unwrap();
        let v: Vec<VendorView> = m.call_typed(Caller::Player(2), "vendors", &()).unwrap();
        assert_eq!(v[0].offers[0].stock, Some(5), "restocked on the hour");
    }

    #[test]
    fn orders_hold_what_they_offer_and_a_fill_moves_both_sides_once() {
        let players = Players::new(&[(1, Bundle::coins("gold", 100)), (2, Bundle::item("ore", 10))]);
        let mut m = market(&players);
        let sell: Placed = m
            .call_typed(Caller::Player(2), "order", &json!({ "item": "ore", "side": "sell", "price": 4, "count": 6, "action_id": "s1" }))
            .unwrap();
        assert_eq!((sell.open, players.borrow().count(2, "ore")), (6, 4), "the ore is held");
        let buy: Placed = m
            .call_typed(Caller::Player(1), "order", &json!({ "item": "ore", "side": "buy", "price": 5, "count": 4, "action_id": "o1" }))
            .unwrap();
        assert_eq!((buy.fills.clone(), buy.open), (vec![(4, 4)], 0), "at the resting price");
        let p = players.borrow();
        assert_eq!((p.count(1, "ore"), p.gold(1)), (4, 84), "16 paid; the 4 held above it came back");
        assert_eq!(p.gold(2), 16);
        drop(p);
        let book: BookView = m.call_typed(Caller::Player(1), "book", &json!({ "item": "ore" })).unwrap();
        assert_eq!((book.asks.clone(), book.last), (vec![(4, 2)], Some(4)));
        let mine: Vec<OrderMeta> = m.call_typed(Caller::Player(2), "orders", &()).unwrap();
        let _: bool = m.call_typed(Caller::Player(2), "cancel", &json!({ "order": mine[0].id })).unwrap();
        assert_eq!(players.borrow().count(2, "ore"), 6, "the rest comes back");
        let e = m
            .call(Caller::Player(1), "order", &encode(&json!({ "item": "ore", "side": "buy", "price": 5, "count": 100 })).unwrap())
            .unwrap_err();
        assert_eq!(code(&e), Some(codes::INSUFFICIENT), "a buy holds its price");
    }

    #[test]
    fn a_player_whose_hub_does_not_answer_gets_unavailable_and_a_resend_finishes() {
        let players = Players::new(&[(1, Bundle::coins("gold", 100)), (2, Bundle::item("ore", 10))]);
        let mut m = market(&players);
        let _: Placed = m
            .call_typed(Caller::Player(2), "order", &json!({ "item": "ore", "side": "sell", "price": 4, "count": 5, "action_id": "s1" }))
            .unwrap();
        players.borrow_mut().down.insert(2);
        let fill: Placed = m
            .call_typed(Caller::Player(1), "order", &json!({ "item": "ore", "side": "buy", "price": 4, "count": 5, "action_id": "o1" }))
            .unwrap();
        assert_eq!(fill.open, 0);
        assert_eq!(players.borrow().count(1, "ore"), 0, "the seller's hub must give the ore up first");
        players.borrow_mut().down.remove(&2);
        m.advance(STEPS_MS).unwrap();
        assert_eq!((players.borrow().count(1, "ore"), players.borrow().gold(2)), (5, 20), "the steps finish once it answers");
        players.borrow_mut().down.insert(1);
        let e = m
            .call(Caller::Player(1), "buy", &encode(&json!({ "vendor": "smith", "item": "potion", "action_id": "b9" })).unwrap())
            .unwrap_err();
        assert_eq!(code(&e), Some(codes::UNAVAILABLE));
        players.borrow_mut().down.remove(&1);
        let got: Bundle =
            m.call_typed(Caller::Player(1), "buy", &json!({ "vendor": "smith", "item": "potion", "action_id": "b9" })).unwrap();
        assert_eq!(got, Bundle::item("potion", 1), "the resend runs: unavailable was not a receipt");
    }

    #[test]
    fn a_trade_settles_when_both_accept_the_last_offers_and_a_declined_one_gives_back() {
        let players = Players::new(&[(1, Bundle::coins("gold", 50)), (2, Bundle::item("plank", 5))]);
        let mut m = market(&players);
        let t: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        let _: bool = m
            .call_typed(Caller::Player(1), "offer", &json!({ "trade": t, "bundle": { "currency": { "gold": 30 } }, "action_id": "a" }))
            .unwrap();
        let _: bool = m
            .call_typed(Caller::Player(2), "offer", &json!({ "trade": t, "bundle": { "items": { "plank": 5 } }, "action_id": "b" }))
            .unwrap();
        let _: TradeState = m.call_typed(Caller::Player(1), "accept", &json!({ "trade": t })).unwrap();
        let _: bool = m
            .call_typed(Caller::Player(1), "offer", &json!({ "trade": t, "bundle": { "currency": { "gold": 20 } }, "action_id": "a2" }))
            .unwrap();
        let state: TradeState = m.call_typed(Caller::Player(2), "accept", &json!({ "trade": t })).unwrap();
        assert_eq!(state, TradeState::Open, "an offer changed: player 1 accepts again");
        let state: TradeState = m.call_typed(Caller::Player(1), "accept", &json!({ "trade": t })).unwrap();
        assert_eq!(state, TradeState::Settling);
        let p = players.borrow();
        assert_eq!((p.gold(1), p.count(1, "plank"), p.gold(2), p.count(2, "plank")), (30, 5, 20, 0));
        drop(p);
        let trades: Vec<Trade> = m.call_typed(Caller::Player(1), "trades", &()).unwrap();
        assert!(trades.is_empty(), "settled");
        let t2: u64 = m.call_typed(Caller::Player(2), "trade", &json!({ "with": 1 })).unwrap();
        let _: bool = m
            .call_typed(Caller::Player(1), "offer", &json!({ "trade": t2, "bundle": { "currency": { "gold": 10 } }, "action_id": "c" }))
            .unwrap();
        assert_eq!(players.borrow().gold(1), 20);
        let e = m.call(Caller::Player(3), "decline", &encode(&json!({ "trade": t2 })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN));
        let _: bool = m.call_typed(Caller::Player(2), "decline", &json!({ "trade": t2 })).unwrap();
        assert_eq!(players.borrow().gold(1), 30, "the hold comes back");
    }

    #[test]
    fn nobody_fills_another_players_trades() {
        let players = Players::new(&[(1, Bundle::default()), (2, Bundle::default()), (3, Bundle::default())]);
        let mut m = market(&players);
        let max = Config::default().trades_max as u64;
        let first: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        let again: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        let reverse: u64 = m.call_typed(Caller::Player(2), "trade", &json!({ "with": 1 })).unwrap();
        assert_eq!((again, reverse), (first, first), "a pair has one open trade; asking again answers it");
        assert_eq!(m.hub().market.trades.len(), 1);
        for opener in 10..10 + max - 1 {
            let _: u64 = m.call_typed(Caller::Player(opener), "trade", &json!({ "with": 2 })).unwrap();
        }
        let e = m.call(Caller::Player(3), "trade", &encode(&json!({ "with": 2 })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::LIMIT), "it takes as many accounts as trades to fill a player's trades");
        let e = m.call(Caller::Player(2), "trade", &encode(&json!({ "with": 3 })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::LIMIT), "and the full player cannot open another");
        let _: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 3 })).unwrap();
        let _: bool = m.call_typed(Caller::Player(2), "decline", &json!({ "trade": first })).unwrap();
        let _: u64 = m.call_typed(Caller::Player(3), "trade", &json!({ "with": 2 })).unwrap();
    }

    #[test]
    fn a_finished_trade_is_not_kept() {
        let players = Players::new(&[(1, Bundle::coins("gold", 50)), (2, Bundle::item("plank", 5))]);
        let mut m = market(&players);
        for with in 100..300u64 {
            let t: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": with })).unwrap();
            let _: bool = m.call_typed(Caller::Player(1), "decline", &json!({ "trade": t, "action_id": format!("d{with}") })).unwrap();
        }
        assert!(m.hub().market.trades.is_empty(), "a declined trade is gone at once");
        let t: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        let decline = json!({ "trade": t, "action_id": "dd" });
        let _: bool = m.call_typed(Caller::Player(1), "decline", &decline).unwrap();
        let again: bool = m.call_typed(Caller::Player(1), "decline", &decline).unwrap();
        assert!(again, "a resent decline gets its first answer from the receipt");
        let t: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        let offer = |bundle: Value, action: &str| json!({ "trade": t, "bundle": bundle, "action_id": action });
        let _: bool = m.call_typed(Caller::Player(1), "offer", &offer(json!({ "currency": { "gold": 30 } }), "a")).unwrap();
        let _: bool = m.call_typed(Caller::Player(2), "offer", &offer(json!({ "items": { "plank": 5 } }), "b")).unwrap();
        let _: TradeState = m.call_typed(Caller::Player(1), "accept", &json!({ "trade": t })).unwrap();
        let _: TradeState = m.call_typed(Caller::Player(2), "accept", &json!({ "trade": t })).unwrap();
        assert!(m.hub().market.trades.is_empty(), "a settled trade is gone once its steps finish");
        let _: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        m.advance(Config::default().trade_ms + STEPS_MS).unwrap();
        assert!(m.hub().market.trades.is_empty(), "an expired trade is gone");
    }

    #[test]
    fn a_buy_whose_answer_was_lost_still_counts_against_the_shelf_and_the_limit() {
        let players = Players::new(&[(1, Bundle::coins("gold", 200))]);
        let mut m = market(&players);
        let buy = |action: &str| json!({ "vendor": "smith", "item": "sword", "action_id": action });
        players.borrow_mut().lost.insert(1);
        let e = m.call(Caller::Player(1), "buy", &encode(&buy("b1")).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::UNAVAILABLE));
        let v: Vec<VendorView> = m.call_typed(Caller::Player(1), "vendors", &()).unwrap();
        assert_eq!(v[0].offers[0].stock, Some(4), "it ran; the sale counts");
        players.borrow_mut().lost.remove(&1);
        let e = m.call(Caller::Player(1), "buy", &encode(&buy("b2")).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::LIMIT), "another action_id cannot buy past the limit");
        let got: Bundle = m.call_typed(Caller::Player(1), "buy", &buy("b1")).unwrap();
        assert_eq!(got, Bundle::item("sword", 1));
        let shelf = &m.hub().market.shelves["smith"];
        assert_eq!((shelf.stock["sword"], shelf.bought["1:sword"], players.borrow().gold(1)), (4, 1, 150), "the resend counts nothing more");
    }

    #[test]
    fn a_buy_the_hub_refuses_is_not_counted() {
        let players = Players::new(&[(2, Bundle::coins("gold", 10))]);
        let mut m = market(&players);
        let e = m.call(Caller::Player(2), "buy", &encode(&json!({ "vendor": "smith", "item": "sword" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::INSUFFICIENT));
        let shelf = &m.hub().market.shelves["smith"];
        assert_eq!((shelf.stock["sword"], shelf.bought.is_empty()), (5, true), "the shelf and the limit are as they were");
    }

    #[test]
    fn a_player_whose_hub_is_down_is_asked_again_only_after_its_backoff() {
        let players = Players::new(&[(1, Bundle::coins("gold", 100)), (2, Bundle::item("ore", 10)), (3, Bundle::coins("gold", 100))]);
        let mut m = market(&players);
        let _: Placed = m
            .call_typed(Caller::Player(2), "order", &json!({ "item": "ore", "side": "sell", "price": 4, "count": 5, "action_id": "s1" }))
            .unwrap();
        players.borrow_mut().down.insert(2);
        let _: Placed = m
            .call_typed(Caller::Player(1), "order", &json!({ "item": "ore", "side": "buy", "price": 4, "count": 5, "action_id": "o1" }))
            .unwrap();
        let asked = |players: &Rc<RefCell<Players>>| players.borrow().asked[&2];
        let base = asked(&players);
        for i in 0..5 {
            let _: Bundle = m.call_typed(Caller::Player(3), "buy", &json!({ "vendor": "smith", "item": "potion", "action_id": format!("p{i}") })).unwrap();
        }
        assert_eq!(asked(&players), base, "calls in the meantime do not wait on the down hub");
        m.advance(STEPS_MS).unwrap();
        assert_eq!(asked(&players), base + 1, "asked again after a second");
        m.advance(STEPS_MS).unwrap();
        assert_eq!(asked(&players), base + 1, "then two seconds");
        m.advance(STEPS_MS).unwrap();
        assert_eq!(asked(&players), base + 2);
        players.borrow_mut().down.clear();
        m.advance(4 * STEPS_MS).unwrap();
        assert_eq!(players.borrow().count(1, "ore"), 5, "it finishes once the hub answers");
        assert!(m.hub().market.retry_at.is_empty() && m.hub().market.retry_gap.is_empty());
    }

    #[test]
    fn the_order_index_follows_a_fill_a_cancel_and_a_reload() {
        let players = Players::new(&[(1, Bundle::coins("gold", 100)), (2, Bundle::item("ore", 10))]);
        let mut m = market(&players);
        let sell = json!({ "item": "ore", "side": "sell", "price": 4, "count": 5 });
        let _: Placed = m.call_typed(Caller::Player(2), "order", &sell).unwrap();
        let _: Placed = m.call_typed(Caller::Player(1), "order", &json!({ "item": "ore", "side": "buy", "price": 4, "count": 5 })).unwrap();
        assert!(m.hub().market.by_player.is_empty() && m.hub().market.orders.is_empty(), "a filled order leaves the index");
        let _: Placed = m.call_typed(Caller::Player(2), "order", &sell).unwrap();
        m.hub_mut().market.by_player.clear();
        m.restart().unwrap();
        let mine: Vec<OrderMeta> = m.call_typed(Caller::Player(2), "orders", &()).unwrap();
        assert_eq!(mine.len(), 1, "a snapshot without the index is indexed on load");
        let _: bool = m.call_typed(Caller::Player(2), "cancel", &json!({ "order": mine[0].id })).unwrap();
        assert!(m.hub().market.by_player.is_empty(), "a cancelled order leaves the index");
        let mine: Vec<OrderMeta> = m.call_typed(Caller::Player(2), "orders", &()).unwrap();
        assert!(mine.is_empty());
    }

    #[test]
    fn a_hold_whose_answer_was_lost_is_given_back_and_a_late_resend_is_a_new_order() {
        let players = Players::new(&[(1, Bundle::coins("gold", 100))]);
        let mut m = market(&players);
        let order = json!({ "item": "ore", "side": "buy", "price": 5, "count": 2, "action_id": "p1" });
        players.borrow_mut().lost.insert(1);
        let e = m.call(Caller::Player(1), "order", &encode(&order).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::UNAVAILABLE));
        assert_eq!(players.borrow().gold(1), 90, "it ran; only its answer was lost");
        players.borrow_mut().lost.remove(&1);
        m.advance(PENDING_MS + STEPS_MS).unwrap();
        assert_eq!(players.borrow().gold(1), 100, "given back");
        let placed: Placed = m.call_typed(Caller::Player(1), "order", &order).unwrap();
        assert_eq!((placed.open, players.borrow().gold(1)), (2, 90), "a late resend is a new order, held anew");
    }

    #[test]
    fn a_resend_after_a_lost_answer_asks_again_by_the_same_reference() {
        let players = Players::new(&[(1, Bundle::coins("gold", 100))]);
        let mut m = market(&players);
        let order = |count: u64, action: &str| json!({ "item": "ore", "side": "buy", "price": 5, "count": count, "action_id": action });
        players.borrow_mut().lost.insert(1);
        assert!(m.call(Caller::Player(1), "order", &encode(&order(2, "q1")).unwrap()).is_err());
        assert!(m.call(Caller::Player(1), "order", &encode(&order(2, "q2")).unwrap()).is_err());
        players.borrow_mut().lost.remove(&1);
        assert_eq!(players.borrow().gold(1), 80);
        let placed: Placed = m.call_typed(Caller::Player(1), "order", &order(2, "q1")).unwrap();
        assert_eq!((placed.open, players.borrow().gold(1)), (2, 80), "held once");
        let e = m.call(Caller::Player(1), "order", &encode(&order(3, "q2")).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::CONFLICT), "the same action_id for another order");
        m.advance(PENDING_MS + STEPS_MS).unwrap();
        assert_eq!(players.borrow().gold(1), 90, "the hold nobody finished comes back");
        let mine: Vec<OrderMeta> = m.call_typed(Caller::Player(1), "orders", &()).unwrap();
        assert_eq!(mine.len(), 1);
        assert!(m.hub().market.unknown.is_empty());
    }

    #[test]
    fn an_action_id_never_makes_a_hold_that_holds_nothing() {
        // Ids as long as the platform's, an action id used again once the market's receipt of it
        // is gone, and one shaped like a step's suffix.
        let (a, b) = (101_910_912_345_678_u64, 101_910_912_345_679_u64);
        let players = Players::new(&[(a, Bundle::coins("gold", 100)), (b, Bundle::item("potion", 3))]);
        let mut m = market(&players);
        let id = "3f2c9a1e-8b7d-4e6f-9a0b-1c2d3e4f5a6b";
        let order = |action: &str| json!({ "item": "ore", "side": "buy", "price": 5, "count": 2, "action_id": action });
        let first: Placed = m.call_typed(Caller::Player(a), "order", &order(id)).unwrap();
        let _: bool = m.call_typed(Caller::Player(a), "cancel", &json!({ "order": first.order })).unwrap();
        m.advance(kit::RECEIPT_TTL_MS + STEPS_MS).unwrap();
        let _: Placed = m.call_typed(Caller::Player(a), "order", &order(id)).unwrap();
        let _: Placed = m.call_typed(Caller::Player(a), "order", &order(&format!("{id}:r"))).unwrap();
        assert_eq!(players.borrow().gold(a), 80, "each order holds its own 10");
        let t: u64 = m.call_typed(Caller::Player(a), "trade", &json!({ "with": b })).unwrap();
        let offer = |bundle: Value, action: &str| json!({ "trade": t, "bundle": bundle, "action_id": action });
        let gold = offer(json!({ "currency": { "gold": 10 } }), "9d1e7c3b-2a4f-4b6e-8c0d-5e6f7a8b9c0d");
        let _: bool = m.call_typed(Caller::Player(a), "offer", &gold).unwrap();
        let _: bool = m.call_typed(Caller::Player(b), "offer", &offer(json!({ "items": { "potion": 1 } }), id)).unwrap();
        let _: TradeState = m.call_typed(Caller::Player(a), "accept", &json!({ "trade": t })).unwrap();
        let _: TradeState = m.call_typed(Caller::Player(b), "accept", &json!({ "trade": t })).unwrap();
        let p = players.borrow();
        assert_eq!((p.gold(a), p.count(a, "potion"), p.gold(b), p.count(b, "potion")), (70, 1, 10, 2), "both sides moved");
        assert_eq!(m.hub().market.stuck, 0);
    }

    #[test]
    fn a_trade_the_timer_finishes_or_calls_off_tells_both_players() {
        use ckx_sdk::kit::{Changed, Message, MessageKind};
        let told = |m: &TestHub<MarketHub>| -> Vec<Vec<u64>> {
            m.published()
                .iter()
                .filter_map(|(_, b)| decode::<Message<Changed>>(b).ok())
                .filter(|c| c.header.kind == MessageKind::Changed && c.body.what == "trades")
                .map(|c| c.body.who)
                .collect()
        };
        let players = Players::new(&[(1, Bundle::coins("gold", 50)), (2, Bundle::item("plank", 5))]);
        let mut m = market(&players);
        let t: u64 = m.call_typed(Caller::Player(1), "trade", &json!({ "with": 2 })).unwrap();
        let offer = |bundle: Value, action: &str| json!({ "trade": t, "bundle": bundle, "action_id": action });
        let _: bool = m.call_typed(Caller::Player(1), "offer", &offer(json!({ "currency": { "gold": 30 } }), "a")).unwrap();
        let _: bool = m.call_typed(Caller::Player(2), "offer", &offer(json!({ "items": { "plank": 5 } }), "b")).unwrap();
        let _: TradeState = m.call_typed(Caller::Player(1), "accept", &json!({ "trade": t })).unwrap();
        players.borrow_mut().down.insert(2);
        let state: TradeState = m.call_typed(Caller::Player(2), "accept", &json!({ "trade": t })).unwrap();
        assert_eq!(state, TradeState::Settling, "player 2's hub must give the planks up first");
        let before = told(&m).len();
        players.borrow_mut().down.remove(&2);
        m.advance(STEPS_MS).unwrap();
        let after = told(&m);
        assert!(after.len() > before && after.last().is_some_and(|who| who.contains(&1) && who.contains(&2)), "the timer finished it: {after:?}");
        let t2: u64 = m.call_typed(Caller::Player(2), "trade", &json!({ "with": 1 })).unwrap();
        let before = told(&m).len();
        m.advance(Config::default().trade_ms + STEPS_MS).unwrap();
        let after = told(&m);
        assert!(after.len() > before && after.last().is_some_and(|who| who.contains(&1) && who.contains(&2)), "trade {t2} expired: {after:?}");
    }

    #[test]
    fn a_market_key_fits_its_references() {
        assert!(TestHub::<MarketHub>::spawn("market", &"k".repeat(KEY_MAX)).is_ok());
        assert!(TestHub::<MarketHub>::spawn("market", &"k".repeat(KEY_MAX + 1)).is_err());
    }
}
