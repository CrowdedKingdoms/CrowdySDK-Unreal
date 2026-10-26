//! A player's own hub, keyed by their player id: what they own and what they have done. Their
//! inventory and soft currencies, with mail for what does not fit; crafting; loot with pity;
//! idle production that catches up on the time away; experience and levels; achievements; and
//! quests, first-session tutorials among them (`ckx_sdk::kit::econ`). It is not a ledger: the
//! items and balances are this hub's state, and a transfer to another player's hub is steps a
//! market drives, each answered once by its reference.
//!
//! Endpoints for the player whose hub it is: `me` (everything), `inventory`, `wallet`,
//! `progress`, `craft { recipe, times }`, `open { table }`, `collect`, `accept { quest }`,
//! `claim { quest }`, `step { counter }` (a counter the config lets the client report),
//! `discard { item, count }` and `take_mail`. For anyone: `profile` (level and achievements)
//! and `rules` (the items, recipes, loot tables, quests and achievements). For the app's other
//! hubs and its developers: `grant { ref, bundle, xp, counters }` and `record { ref, counter, n }`
//! (rewards and what the player did, from the hub that decided it), and the steps of a transfer:
//! `hold { ref, bundle }`, `settle { ref, hold, bundle }`, `release { ref, hold }`,
//! `deposit { ref, bundle }` and `exchange { ref, pay, get }`, each answered once by its `ref`.
//!
//! It speaks the kit protocol (`ckx_sdk::kit`): the `player` topic carries the public profile as
//! `snapshot` messages, `event` messages when the player levels up or unlocks an achievement,
//! and `changed` messages naming the player (`what`: `inventory`, `wallet`, `progress`) when what
//! only they read changes; `snapshot { topic: "player" }` answers the profile; the player's
//! changes may carry an `action_id`; refusals carry `kit:<code>:` codes.

use std::collections::BTreeMap;

use ckx_sdk::kit::econ::loot::{self, LootBook, LootRoll, LootTable};
use ckx_sdk::kit::econ::production::Chain;
use ckx_sdk::kit::econ::progression::{AchievementDef, Levels, Objective, Progress, QuestDef, Record, Unlocks};
use ckx_sdk::kit::econ::recipes::{self, Recipe};
use ckx_sdk::kit::econ::{Applied, Bundle, Catalog, Holdings, Holds, ItemDef, Stack};
use ckx_sdk::kit::{self, Publisher, Receipts, Rng, codes};
use ckx_sdk::prelude::*;
use serde_json::json;

pub const TOPIC: &str = "player";
const PUBLISH_MS: u64 = 1_000;
const INVENTORY: &str = "inventory";
const WALLET: &str = "wallet";
const PROGRESS: &str = "progress";

/// A loot table and what one roll costs.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct LootDef {
    pub id: String,
    #[serde(default)]
    pub cost: Bundle,
    #[serde(flatten)]
    pub table: LootTable,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct Config {
    pub slots: u32,
    pub items: Catalog,
    pub currencies: Vec<String>,
    /// What a new player starts with.
    pub start: Bundle,
    pub levels: Levels,
    pub achievements: Vec<AchievementDef>,
    pub quests: Vec<QuestDef>,
    /// Counters the player's client may report itself (`step`): tutorial steps, worth nothing.
    pub client_counters: Vec<String>,
    pub recipes: Vec<Recipe>,
    pub loot: Vec<LootDef>,
    /// The idle producers, their storage, and nothing else: its clock starts with the hub.
    pub production: Chain,
    /// Mixed into the loot seed with what a client cannot see (the instance, when it started),
    /// so nobody can work out a roll before paying for it. Never answered by `rules`.
    pub secret: Option<String>,
}

fn item(id: &str, max_stack: u64, tradable: bool) -> ItemDef {
    ItemDef { id: id.into(), max_stack, tradable, name: String::new() }
}

fn items(pairs: &[(&str, u64)]) -> Bundle {
    pairs.iter().fold(Bundle::default(), |b, (id, n)| b.plus(&Bundle::item(id, *n)).unwrap_or(b))
}

fn objective(counter: &str, target: u64) -> Objective {
    Objective { counter: counter.into(), target }
}

impl Default for Config {
    fn default() -> Self {
        use ckx_sdk::kit::econ::loot::LootEntry;
        use ckx_sdk::kit::econ::production::Producer;
        Self {
            slots: 30,
            items: Catalog::from(vec![
                item("wood", 99, true),
                item("plank", 99, true),
                item("ore", 99, true),
                item("ingot", 99, true),
                item("sword", 1, true),
                item("potion", 20, true),
                item("relic", 1, false),
            ]),
            currencies: vec!["gold".into(), "gems".into()],
            start: Bundle::coins("gold", 100).plus(&Bundle::coins("gems", 20)).unwrap_or_default(),
            levels: Levels::default(),
            achievements: vec![
                AchievementDef {
                    id: "first_craft".into(),
                    counter: "crafted".into(),
                    target: 1,
                    reward: Bundle::coins("gold", 10),
                    xp: 25,
                    hidden: false,
                },
                AchievementDef {
                    id: "smith".into(),
                    counter: "crafted:sword".into(),
                    target: 3,
                    reward: Bundle::coins("gems", 5),
                    xp: 100,
                    hidden: false,
                },
            ],
            quests: vec![
                QuestDef {
                    id: "welcome".into(),
                    objectives: vec![objective("tutorial:inventory", 1), objective("crafted", 1)],
                    reward: Bundle::coins("gold", 25),
                    xp: 50,
                    requires: vec![],
                    level: 0,
                    tutorial: true,
                },
                QuestDef {
                    id: "gatherer".into(),
                    objectives: vec![objective("collected", 20)],
                    reward: Bundle::item("potion", 3),
                    xp: 100,
                    requires: vec!["welcome".into()],
                    level: 0,
                    tutorial: false,
                },
            ],
            client_counters: vec!["tutorial:inventory".into(), "tutorial:map".into()],
            recipes: vec![
                Recipe { id: "plank".into(), inputs: Bundle::item("wood", 2), outputs: Bundle::item("plank", 1), level: 0 },
                Recipe {
                    id: "ingot".into(),
                    inputs: Bundle::item("ore", 3).plus(&Bundle::coins("gold", 1)).unwrap_or_default(),
                    outputs: Bundle::item("ingot", 1),
                    level: 0,
                },
                Recipe { id: "sword".into(), inputs: items(&[("ingot", 2), ("plank", 1)]), outputs: Bundle::item("sword", 1), level: 2 },
            ],
            loot: vec![LootDef {
                id: "chest".into(),
                cost: Bundle::coins("gems", 10),
                table: LootTable {
                    entries: vec![
                        LootEntry { item: "potion".into(), count: 3, weight: 70, pity_group: None },
                        LootEntry { item: "ingot".into(), count: 2, weight: 25, pity_group: None },
                        LootEntry { item: "relic".into(), count: 1, weight: 5, pity_group: Some("rare".into()) },
                    ],
                    pity_within: BTreeMap::from([("rare".to_string(), 10)]),
                },
            }],
            production: Chain {
                producers: vec![
                    Producer { id: "lumber".into(), outputs: BTreeMap::from([("wood".into(), 1)]), per_minute: 2.0, ..Producer::default() },
                    Producer { id: "mine".into(), outputs: BTreeMap::from([("ore".into(), 1)]), per_minute: 1.0, ..Producer::default() },
                ],
                capacity: BTreeMap::from([("wood".into(), 120), ("ore".into(), 60)]),
                ..Chain::default()
            },
            secret: None,
        }
    }
}

impl Config {
    /// Every item a config names is in its catalog, and every currency in its list.
    fn check(&self) -> Result<()> {
        let bundles = self
            .recipes
            .iter()
            .flat_map(|r| [&r.inputs, &r.outputs])
            .chain(self.loot.iter().map(|l| &l.cost))
            .chain(self.achievements.iter().map(|a| &a.reward))
            .chain(self.quests.iter().map(|q| &q.reward))
            .chain(std::iter::once(&self.start));
        for b in bundles {
            self.check_bundle(b)?;
        }
        let produced = self.production.producers.iter().flat_map(|p| p.outputs.keys().chain(p.inputs.keys()));
        let looted = self.loot.iter().flat_map(|l| l.table.entries.iter().map(|e| &e.item));
        for id in produced.chain(looted) {
            self.items.item(id)?;
        }
        Ok(())
    }

    fn check_bundle(&self, b: &Bundle) -> Result<()> {
        b.check()?;
        for id in b.items.keys() {
            self.items.item(id)?;
        }
        match b.currency.keys().find(|c| !self.currencies.contains(c)) {
            Some(c) => Err(kit::error(codes::BAD_REQUEST, format!("there is no currency `{c}`"))),
            None => Ok(()),
        }
    }
}

// ---- the views ----

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct ProfileView {
    pub player: u64,
    pub level: u32,
    pub achievements: Vec<String>,
    pub quests_done: usize,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct InventoryView {
    pub slots: u32,
    pub stacks: Vec<Stack>,
    /// Items given that did not fit, waiting for `take_mail`.
    pub mail: BTreeMap<String, u64>,
    /// Items held for a trade or an order.
    pub held: BTreeMap<String, u64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct WalletView {
    pub balances: BTreeMap<String, i64>,
    /// Currency held for a trade or an order.
    pub held: BTreeMap<String, i64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct ObjectiveView {
    pub counter: String,
    pub target: u64,
    pub count: u64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct QuestView {
    pub id: String,
    /// `locked`, `available`, `active`, `complete` (its reward waits) or `done`.
    pub state: String,
    pub objectives: Vec<ObjectiveView>,
    pub reward: Bundle,
    pub xp: u64,
    pub tutorial: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct AchievementView {
    pub id: String,
    pub counter: String,
    pub target: u64,
    pub count: u64,
    pub unlocked_at: Option<u64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct ProgressView {
    pub level: u32,
    pub xp: u64,
    /// What the level takes to finish; none at the top.
    pub next: Option<u64>,
    pub total_xp: u64,
    pub achievements: Vec<AchievementView>,
    pub quests: Vec<QuestView>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct ProductionView {
    pub producers: Vec<(String, f64)>,
    pub stock: BTreeMap<String, u64>,
    pub capacity: BTreeMap<String, u64>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct MeView {
    pub profile: ProfileView,
    pub inventory: InventoryView,
    pub wallet: WalletView,
    pub progress: ProgressView,
    pub production: ProductionView,
}

/// What one change of the player's earned: levels, achievements, quests complete.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct Earned {
    pub levels: u32,
    pub achievements: Vec<String>,
    pub quests: Vec<String>,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Crafted {
    pub made: Bundle,
    pub earned: Earned,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Opened {
    pub roll: LootRoll,
    pub earned: Earned,
}

// ---- the arguments ----

#[derive(Deserialize)]
struct Craft {
    recipe: String,
    #[serde(default = "one")]
    times: u64,
}

fn one() -> u64 {
    1
}

#[derive(Deserialize)]
struct Named {
    #[serde(alias = "table", alias = "quest", alias = "counter")]
    id: String,
}

#[derive(Deserialize)]
struct Discard {
    item: String,
    count: u64,
}

#[derive(Deserialize, Default)]
#[serde(default)]
struct Grant {
    r#ref: String,
    bundle: Bundle,
    xp: u64,
    counters: BTreeMap<String, u64>,
}

#[derive(Deserialize)]
struct Count {
    r#ref: String,
    counter: String,
    #[serde(default = "one")]
    n: u64,
}

#[derive(Deserialize)]
struct Hold {
    r#ref: String,
    bundle: Bundle,
}

#[derive(Deserialize)]
struct Settle {
    r#ref: String,
    hold: String,
    bundle: Bundle,
}

#[derive(Deserialize)]
struct Release {
    r#ref: String,
    hold: String,
}

#[derive(Deserialize)]
struct Exchange {
    r#ref: String,
    pay: Bundle,
    get: Bundle,
}

/// Which of the player's private views one call moved, for the `changed` messages after it.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
struct Touched(u8);

impl Touched {
    const INVENTORY: u8 = 1;
    const WALLET: u8 = 2;
    const PROGRESS: u8 = 4;

    fn has(self, bit: u8) -> bool {
        self.0 & bit != 0
    }

    fn bundle(&mut self, b: &Bundle) {
        if b.items.values().any(|&n| n > 0) {
            self.0 |= Self::INVENTORY;
        }
        if b.currency.values().any(|&n| n != 0) {
            self.0 |= Self::WALLET;
        }
    }
}

// ---- the hub ----

#[derive(Serialize, Deserialize, Debug)]
pub struct PlayerHub {
    pub config: Config,
    pub owner: u64,
    pub holdings: Holdings,
    #[serde(default)]
    pub holds: Holds,
    #[serde(default)]
    pub applied: Applied,
    pub progress: Progress,
    #[serde(default)]
    pub record: Record,
    #[serde(default)]
    pub books: BTreeMap<String, LootBook>,
    pub rng: Rng,
    pub chain: Chain,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
    #[serde(default)]
    pub dirty: bool,
    #[serde(skip)]
    touched: Touched,
}

impl PlayerHub {
    /// A new player's hub. `entropy` seeds its loot: something no client can see.
    pub fn new(config: Config, owner: u64, now: u64, entropy: u64) -> Result<Self> {
        config.check()?;
        let mut holdings = Holdings::new(config.slots);
        holdings.give_or_mail(&config.items, &config.start)?;
        let mut record = Record::default();
        for q in config.quests.iter().filter(|q| q.tutorial) {
            record.accept(q, 1)?;
        }
        let mut chain = config.production.clone();
        chain.at = 0;
        chain.start(now);
        Ok(Self {
            owner,
            holdings,
            holds: Holds::default(),
            applied: Applied::default(),
            progress: Progress::default(),
            record,
            books: BTreeMap::new(),
            rng: Rng::seeded(Rng::seed_from(&format!("player:{owner}:{entropy}:{}", config.secret.as_deref().unwrap_or("")))),
            chain,
            kit: Publisher::default(),
            receipts: Receipts::default(),
            dirty: true,
            touched: Touched::default(),
            config,
        })
    }

    pub fn profile(&self) -> ProfileView {
        ProfileView {
            player: self.owner,
            level: self.progress.level,
            achievements: self.record.unlocked.keys().cloned().collect(),
            quests_done: self.record.done.len(),
        }
    }

    pub fn inventory(&self) -> InventoryView {
        let held = self.holds.total();
        InventoryView {
            slots: self.holdings.inventory.slots,
            stacks: self.holdings.inventory.stacks.clone(),
            mail: self.holdings.mail.clone(),
            held: held.items,
        }
    }

    pub fn wallet(&self) -> WalletView {
        WalletView { balances: self.holdings.wallet.balances.clone(), held: self.holds.total().currency }
    }

    pub fn progress(&self) -> ProgressView {
        let r = &self.record;
        let achievements = self
            .config
            .achievements
            .iter()
            .filter(|a| !a.hidden || r.unlocked.contains_key(&a.id))
            .map(|a| AchievementView {
                id: a.id.clone(),
                counter: a.counter.clone(),
                target: a.target,
                count: r.counters.get(&a.counter).copied().unwrap_or(0).min(a.target),
                unlocked_at: r.unlocked.get(&a.id).copied(),
            })
            .collect();
        let quests = self
            .config
            .quests
            .iter()
            .map(|q| {
                let counts = r.active.get(&q.id);
                let state = if r.done.contains_key(&q.id) {
                    "done"
                } else if r.complete.contains(&q.id) {
                    "complete"
                } else if counts.is_some() {
                    "active"
                } else if q.requires.iter().all(|x| r.done.contains_key(x)) && self.progress.level >= q.level {
                    "available"
                } else {
                    "locked"
                };
                let finished = matches!(state, "done" | "complete");
                QuestView {
                    id: q.id.clone(),
                    state: state.into(),
                    objectives: q
                        .objectives
                        .iter()
                        .enumerate()
                        .map(|(i, o)| ObjectiveView {
                            counter: o.counter.clone(),
                            target: o.target,
                            count: if finished { o.target } else { counts.and_then(|c| c.get(i)).copied().unwrap_or(0) },
                        })
                        .collect(),
                    reward: q.reward.clone(),
                    xp: q.xp,
                    tutorial: q.tutorial,
                }
            })
            .collect();
        ProgressView {
            level: self.progress.level,
            xp: self.progress.xp,
            next: self.config.levels.to_next(self.progress.level),
            total_xp: self.progress.total_xp,
            achievements,
            quests,
        }
    }

    pub fn production(&self) -> ProductionView {
        ProductionView {
            producers: self.chain.producers.iter().map(|p| (p.id.clone(), p.per_minute)).collect(),
            stock: self.chain.stock.clone(),
            capacity: self.chain.capacity.clone(),
        }
    }

    pub fn me(&self) -> MeView {
        MeView {
            profile: self.profile(),
            inventory: self.inventory(),
            wallet: self.wallet(),
            progress: self.progress(),
            production: self.production(),
        }
    }

    /// Counts what the player did, and gives what it earned.
    fn count(&mut self, counter: &str, n: u64, now: u64, earned: &mut Earned) -> Result<()> {
        let Unlocks { achievements, quests } = self.record.count(&self.config.achievements, &self.config.quests, counter, n, now);
        self.touched.0 |= Touched::PROGRESS;
        for id in &achievements {
            if let Some(a) = self.config.achievements.iter().find(|a| &a.id == id).cloned() {
                self.holdings.give_or_mail(&self.config.items, &a.reward)?;
                self.touched.bundle(&a.reward);
                self.gain(a.xp, earned);
            }
        }
        earned.achievements.extend(achievements);
        earned.quests.extend(quests);
        Ok(())
    }

    fn gain(&mut self, xp: u64, earned: &mut Earned) {
        if xp > 0 {
            self.touched.0 |= Touched::PROGRESS;
        }
        earned.levels += self.progress.gain(&self.config.levels, xp);
    }

    fn owner_of(&self, call: &Call<'_>) -> Result<()> {
        match call.caller {
            Caller::Player(p) if p == self.owner => Ok(()),
            Caller::Developer(_) => Ok(()),
            _ => Err(kit::error(codes::FORBIDDEN, "this is another player's")),
        }
    }

    /// What the player asked for, run once per action id.
    fn act(&mut self, call: &Call<'_>, now: u64, earned: &mut Earned) -> Result<Vec<u8>> {
        self.owner_of(call)?;
        match call.method {
            "craft" => {
                let c: Craft = kit::args(call)?;
                let recipe = self.config.recipes.iter().find(|r| r.id == c.recipe).cloned();
                let recipe = recipe.ok_or_else(|| kit::error(codes::NOT_FOUND, format!("there is no recipe `{}`", c.recipe)))?;
                let made = recipes::craft(&mut self.holdings, &self.config.items, &recipe, c.times, self.progress.level)?;
                self.touched.bundle(&recipe.inputs);
                self.touched.bundle(&made);
                self.count("crafted", c.times, now, earned)?;
                self.count(&format!("crafted:{}", recipe.id), c.times, now, earned)?;
                encode(&Crafted { made, earned: earned.clone() })
            }
            "open" => {
                let Named { id } = kit::args(call)?;
                let def = self.config.loot.iter().find(|l| l.id == id).cloned();
                let def = def.ok_or_else(|| kit::error(codes::NOT_FOUND, format!("there is no loot table `{id}`")))?;
                let (mut book, mut rng) = (self.books.get(&id).cloned().unwrap_or_default(), self.rng);
                // The hub's own state, and the moment it rolls, decide it.
                let mut dice = Rng::seeded(rng.next_u64() ^ now.wrapping_mul(0x9e37_79b9_7f4a_7c15));
                let roll = loot::roll(&def.table, &mut book, &mut dice)
                    .ok_or_else(|| kit::error(codes::EMPTY, format!("`{id}` has nothing to roll")))?;
                let won = Bundle::item(&roll.item, roll.count);
                let mut next = self.holdings.clone();
                next.take(&def.cost)?;
                next.give_or_mail(&self.config.items, &won)?;
                self.holdings = next;
                self.touched.bundle(&def.cost);
                self.touched.bundle(&won);
                self.books.insert(id.clone(), book);
                self.rng = rng;
                self.count(&format!("opened:{id}"), 1, now, earned)?;
                encode(&Opened { roll, earned: earned.clone() })
            }
            "collect" => {
                self.chain.advance(now);
                let mut collected = Bundle::default();
                for (resource, have) in self.chain.stock.clone() {
                    let fits = have.min(self.holdings.inventory.room(&self.config.items, &resource));
                    if fits > 0 {
                        self.holdings.inventory.add(&self.config.items, &resource, fits)?;
                        self.chain.take(&resource, fits);
                        collected = collected.plus(&Bundle::item(&resource, fits))?;
                    }
                }
                let total: u64 = collected.items.values().sum();
                if total > 0 {
                    self.touched.0 |= Touched::INVENTORY;
                    self.count("collected", total, now, earned)?;
                }
                encode(&collected)
            }
            "accept" => {
                let Named { id } = kit::args(call)?;
                let q = self.config.quests.iter().find(|q| q.id == id).cloned();
                let q = q.ok_or_else(|| kit::error(codes::NOT_FOUND, format!("there is no quest `{id}`")))?;
                self.record.accept(&q, self.progress.level)?;
                self.touched.0 |= Touched::PROGRESS;
                encode(&true)
            }
            "claim" => {
                let Named { id } = kit::args(call)?;
                let q = self.config.quests.iter().find(|q| q.id == id).cloned();
                let q = q.ok_or_else(|| kit::error(codes::NOT_FOUND, format!("there is no quest `{id}`")))?;
                let mut next = self.holdings.clone();
                next.give(&self.config.items, &q.reward)?;
                self.record.claim(&id, now)?;
                self.holdings = next;
                self.touched.0 |= Touched::PROGRESS;
                self.touched.bundle(&q.reward);
                self.gain(q.xp, earned);
                self.count(&format!("quest:{id}"), 1, now, earned)?;
                encode(&q.reward)
            }
            "step" => {
                let Named { id } = kit::args(call)?;
                if !self.config.client_counters.contains(&id) {
                    return Err(kit::error(codes::FORBIDDEN, format!("`{id}` is counted by the game's hubs, not by a client")));
                }
                self.count(&id, 1, now, earned)?;
                encode(&earned.clone())
            }
            "discard" => {
                let d: Discard = kit::args(call)?;
                self.holdings.take(&Bundle::item(&d.item, d.count))?;
                self.touched.0 |= Touched::INVENTORY;
                encode(&true)
            }
            "take_mail" => {
                let moved = self.holdings.take_mail(&self.config.items);
                self.touched.bundle(&moved);
                encode(&moved)
            }
            other => Err(Error::unknown_method(other)),
        }
    }

    /// What another hub (or a developer) asked for, run once per reference.
    fn trusted(&mut self, call: &Call<'_>, now: u64, earned: &mut Earned) -> Result<Vec<u8>> {
        match call.method {
            "grant" => {
                let g: Grant = kit::args(call)?;
                Applied::run(
                    self,
                    |h| &mut h.applied,
                    &g.r#ref,
                    now,
                    |h| {
                        h.config.check_bundle(&g.bundle)?;
                        h.holdings.give_or_mail(&h.config.items, &g.bundle)?;
                        h.touched.bundle(&g.bundle);
                        h.gain(g.xp, earned);
                        for (counter, n) in &g.counters {
                            h.count(counter, *n, now, earned)?;
                        }
                        encode(&earned.clone())
                    },
                )
            }
            "record" => {
                let c: Count = kit::args(call)?;
                Applied::run(
                    self,
                    |h| &mut h.applied,
                    &c.r#ref,
                    now,
                    |h| {
                        h.count(&c.counter, c.n, now, earned)?;
                        encode(&earned.clone())
                    },
                )
            }
            "hold" => {
                let h: Hold = kit::args(call)?;
                if let Some(id) = h.bundle.items.keys().find(|id| self.config.items.get(id).is_some_and(|d| !d.tradable)) {
                    return Err(kit::error(codes::FORBIDDEN, format!("`{id}` does not change hands")));
                }
                Applied::run(
                    self,
                    |s| &mut s.applied,
                    &h.r#ref,
                    now,
                    |s| {
                        s.config.check_bundle(&h.bundle)?;
                        s.holds.hold(&mut s.holdings, &h.r#ref, &h.bundle)?;
                        s.touched.bundle(&h.bundle);
                        encode(&true)
                    },
                )
            }
            "settle" => {
                let s: Settle = kit::args(call)?;
                Applied::run(
                    self,
                    |h| &mut h.applied,
                    &s.r#ref,
                    now,
                    |h| {
                        h.holds.settle(&s.hold, &s.bundle)?;
                        h.touched.bundle(&s.bundle);
                        encode(&true)
                    },
                )
            }
            "release" => {
                let r: Release = kit::args(call)?;
                Applied::run(
                    self,
                    |h| &mut h.applied,
                    &r.r#ref,
                    now,
                    |h| {
                        let back = h.holds.release(&mut h.holdings, &h.config.items, &r.hold)?;
                        h.touched.bundle(&back);
                        encode(&back)
                    },
                )
            }
            "deposit" => {
                let d: Hold = kit::args(call)?;
                Applied::run(
                    self,
                    |h| &mut h.applied,
                    &d.r#ref,
                    now,
                    |h| {
                        h.config.check_bundle(&d.bundle)?;
                        h.holdings.give_or_mail(&h.config.items, &d.bundle)?;
                        h.touched.bundle(&d.bundle);
                        encode(&true)
                    },
                )
            }
            "exchange" => {
                let x: Exchange = kit::args(call)?;
                Applied::run(
                    self,
                    |h| &mut h.applied,
                    &x.r#ref,
                    now,
                    |h| {
                        h.config.check_bundle(&x.pay)?;
                        h.config.check_bundle(&x.get)?;
                        h.holdings.swap(&h.config.items, &x.pay, &x.get)?;
                        h.touched.bundle(&x.pay);
                        h.touched.bundle(&x.get);
                        encode(&true)
                    },
                )
            }
            other => Err(Error::unknown_method(other)),
        }
    }

    /// Tells the player what changed, and anyone watching what they earned.
    fn after(&mut self, ctx: &Ctx, earned: &Earned) -> Result<()> {
        for (bit, what) in [(Touched::INVENTORY, INVENTORY), (Touched::WALLET, WALLET), (Touched::PROGRESS, PROGRESS)] {
            if self.touched.has(bit) {
                self.kit.changed(ctx, TOPIC, &[self.owner], what)?;
            }
        }
        if earned.levels > 0 {
            self.kit.event(ctx, TOPIC, &json!({ "type": "level", "player": self.owner, "level": self.progress.level }))?;
        }
        for id in &earned.achievements {
            self.kit.event(ctx, TOPIC, &json!({ "type": "achievement", "player": self.owner, "achievement": id }))?;
        }
        if earned.levels > 0 || !earned.achievements.is_empty() || !earned.quests.is_empty() {
            self.dirty = true;
        }
        Ok(())
    }
}

impl Hub for PlayerHub {
    fn spawn(ctx: &Ctx, seed: &[u8]) -> Result<Self> {
        let owner: u64 = ctx.key.parse().map_err(|_| kit::error(codes::BAD_REQUEST, "a player hub's key is its player's id"))?;
        let config = if seed.is_empty() {
            Config::default()
        } else {
            serde_json::from_slice(seed).map_err(|e| Error::new(format!("seed is not a JSON config: {e}")))?
        };
        ctx.timer_every("publish", PUBLISH_MS)?;
        PlayerHub::new(config, owner, ctx.now_ms(), ctx.instance ^ ctx.now_ms().rotate_left(29))
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
            "profile" => return encode(&self.profile()),
            "rules" => {
                let config = &self.config;
                return encode(&json!({
                    "slots": config.slots,
                    "items": config.items,
                    "currencies": config.currencies,
                    "recipes": config.recipes,
                    "loot": config.loot,
                    "quests": config.quests,
                    "achievements": config.achievements.iter().filter(|a| !a.hidden).collect::<Vec<_>>(),
                    "client_counters": config.client_counters,
                }));
            }
            kit::SNAPSHOT_METHOD => return self.kit.snapshot_reply(ctx, TOPIC, &self.profile()),
            "me" | "inventory" | "wallet" | "progress" => {
                self.owner_of(&call)?;
                return match call.method {
                    "me" => {
                        self.chain.advance(now);
                        encode(&self.me())
                    }
                    "inventory" => encode(&self.inventory()),
                    "wallet" => encode(&self.wallet()),
                    _ => encode(&self.progress()),
                };
            }
            _ => {}
        }
        let mut earned = Earned::default();
        self.touched = Touched::default();
        let result = match call.caller {
            Caller::Instance(_) => self.trusted(&call, now, &mut earned),
            Caller::Developer(_) if matches!(call.method, "grant" | "record" | "deposit") => self.trusted(&call, now, &mut earned),
            Caller::Player(_) if matches!(call.method, "grant" | "record" | "hold" | "settle" | "release" | "deposit" | "exchange") => {
                Err(kit::error(codes::FORBIDDEN, format!("`{}` comes from the game's hubs", call.method)))
            }
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(&call, now, &mut earned)),
        }?;
        self.after(ctx, &earned)?;
        Ok(result)
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer == "publish" && self.dirty {
            self.dirty = false;
            self.kit.snapshot(ctx, TOPIC, &self.profile())?;
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(PlayerHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Changed, Message, error_code};
    use ckx_sdk::testing::TestHub;

    fn code(e: &ckx_sdk::CallError) -> Option<&str> {
        error_code(&e.message).map(|c| c.0)
    }

    fn hub() -> TestHub<PlayerHub> {
        TestHub::<PlayerHub>::spawn("player", "7").unwrap()
    }

    #[test]
    fn a_new_player_starts_with_the_config_and_its_tutorial() {
        let mut h = hub();
        let me: MeView = h.call_typed(Caller::Player(7), "me", &()).unwrap();
        assert_eq!(me.wallet.balances["gold"], 100);
        assert_eq!(me.progress.level, 1);
        let welcome = me.progress.quests.iter().find(|q| q.id == "welcome").unwrap();
        assert_eq!(welcome.state, "active", "a tutorial is taken on as the player starts");
        assert_eq!(me.progress.quests.iter().find(|q| q.id == "gatherer").unwrap().state, "locked");
        let e = h.call(Caller::Player(8), "me", &[]).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN), "only its player reads it");
        let p: ProfileView = h.call_typed(Caller::Player(8), "profile", &()).unwrap();
        assert_eq!((p.player, p.level), (7, 1));
    }

    #[test]
    fn crafting_counts_toward_achievements_and_quests_and_the_reward_is_claimed_once() {
        let mut h = hub();
        let _: bool =
            h.call_typed(Caller::Developer(1), "deposit", &json!({ "ref": "seed-wood", "bundle": { "items": { "wood": 4 } } })).unwrap();
        let crafted: Crafted =
            h.call_typed(Caller::Player(7), "craft", &json!({ "recipe": "plank", "times": 2, "action_id": "c1" })).unwrap();
        assert_eq!(crafted.made, Bundle::item("plank", 2));
        assert_eq!(crafted.earned.achievements, vec!["first_craft".to_string()]);
        let again: Crafted =
            h.call_typed(Caller::Player(7), "craft", &json!({ "recipe": "plank", "times": 2, "action_id": "c1" })).unwrap();
        assert_eq!(again, crafted, "a resent craft is answered, not made again");
        let e = h.call(Caller::Player(7), "craft", &encode(&json!({ "recipe": "plank" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::INSUFFICIENT));
        let e = h.call(Caller::Player(7), "step", &encode(&json!({ "counter": "crafted" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN), "a client reports only its own counters");
        let earned: Earned = h.call_typed(Caller::Player(7), "step", &json!({ "counter": "tutorial:inventory" })).unwrap();
        assert_eq!(earned.quests, vec!["welcome".to_string()]);
        let reward: Bundle = h.call_typed(Caller::Player(7), "claim", &json!({ "quest": "welcome" })).unwrap();
        assert_eq!(reward, Bundle::coins("gold", 25));
        let e = h.call(Caller::Player(7), "claim", &encode(&json!({ "quest": "welcome" })).unwrap()).unwrap_err();
        assert_eq!(code(&e), Some(codes::NOT_READY));
        let me: MeView = h.call_typed(Caller::Player(7), "me", &()).unwrap();
        assert_eq!(me.wallet.balances["gold"], 135, "100, the achievement's 10 and the quest's 25");
        assert_eq!(me.progress.total_xp, 75);
        assert_eq!(me.progress.quests.iter().find(|q| q.id == "gatherer").unwrap().state, "available");
        let changed: Vec<Message<Changed>> = h
            .published()
            .iter()
            .filter_map(|(_, m)| decode::<Message<Changed>>(m).ok())
            .filter(|m| m.header.kind == kit::MessageKind::Changed)
            .collect();
        assert!(!changed.is_empty());
        assert!(changed.iter().all(|m| m.body.who == vec![7]), "a change is told to its player alone");
    }

    #[test]
    fn a_loot_roll_pays_its_cost_and_pity_guarantees_the_rare_within_ten() {
        let mut h = hub();
        let _: bool =
            h.call_typed(Caller::Developer(1), "deposit", &json!({ "ref": "gems", "bundle": { "currency": { "gems": 200 } } })).unwrap();
        let mut since = 0;
        for i in 0..12 {
            let o: Opened = h.call_typed(Caller::Player(7), "open", &json!({ "table": "chest", "action_id": format!("o{i}") })).unwrap();
            since = if o.roll.item == "relic" { 0 } else { since + 1 };
            assert!(since < 10);
        }
        let w: WalletView = h.call_typed(Caller::Player(7), "wallet", &()).unwrap();
        assert_eq!(w.balances["gems"], 220 - 120);
    }

    fn changed_what(h: &TestHub<PlayerHub>) -> Vec<String> {
        h.published()
            .iter()
            .filter_map(|(_, m)| decode::<Message<Changed>>(m).ok())
            .filter(|m| m.header.kind == kit::MessageKind::Changed)
            .map(|m| m.body.what)
            .collect()
    }

    #[test]
    fn a_paid_roll_lands_in_the_mail_when_the_inventory_is_full() {
        let seed = serde_json::to_vec(&Config { slots: 0, ..Config::default() }).unwrap();
        let mut h = TestHub::<PlayerHub>::spawn_with("player", "7", &seed).unwrap();
        let rng = h.hub().rng;
        let o: Opened = h.call_typed(Caller::Player(7), "open", &json!({ "table": "chest", "action_id": "o1" })).unwrap();
        let w: WalletView = h.call_typed(Caller::Player(7), "wallet", &()).unwrap();
        assert_eq!(w.balances["gems"], 10, "the cost is paid though nothing fits");
        assert_ne!(h.hub().rng, rng, "a full inventory does not hold the dice back");
        assert!(h.hub().books.contains_key("chest"), "nor the pity book");
        let inv: InventoryView = h.call_typed(Caller::Player(7), "inventory", &()).unwrap();
        assert_eq!(inv.mail[&o.roll.item], o.roll.count, "the roll waits in the mail");
    }

    #[test]
    fn a_call_announces_only_the_views_it_moved() {
        let mut h = hub();
        h.clear();
        let _: Earned = h.call_typed(Caller::Player(7), "step", &json!({ "counter": "tutorial:map" })).unwrap();
        assert_eq!(changed_what(&h), vec![PROGRESS.to_string()], "a step that earns nothing moves only progress");
        h.clear();
        let _: bool =
            h.call_typed(Caller::Developer(1), "deposit", &json!({ "ref": "d1", "bundle": { "currency": { "gold": 5 } } })).unwrap();
        assert_eq!(changed_what(&h), vec![WALLET.to_string()], "a coin deposit moves the wallet alone");
        h.clear();
        let _: Earned = h.call_typed(Caller::Developer(1), "record", &json!({ "ref": "r1", "counter": "crafted", "n": 1 })).unwrap();
        let what = changed_what(&h);
        assert!(what.contains(&PROGRESS.to_string()) && what.contains(&WALLET.to_string()), "an achievement's gold moves the wallet");
        assert!(!what.contains(&INVENTORY.to_string()));
    }

    #[test]
    fn production_catches_up_and_collect_fills_the_inventory() {
        let mut h = hub();
        h.advance(30 * 60_000).unwrap();
        let got: Bundle = h.call_typed(Caller::Player(7), "collect", &json!({ "action_id": "k1" })).unwrap();
        assert_eq!((got.items["wood"], got.items["ore"]), (60, 30), "two and one a minute for half an hour");
        let p: ProgressView = h.call_typed(Caller::Player(7), "progress", &()).unwrap();
        assert_eq!(p.achievements.iter().map(|a| a.count).sum::<u64>(), 0);
        let inv: InventoryView = h.call_typed(Caller::Player(7), "inventory", &()).unwrap();
        assert_eq!(inv.stacks.iter().map(|s| s.count).sum::<u64>(), 90);
    }

    #[test]
    fn a_hold_settles_or_releases_and_each_reference_runs_once() {
        let mut h = hub();
        let call = |h: &mut TestHub<PlayerHub>, method: &str, args: serde_json::Value| {
            h.call(Caller::Instance(3), method, &encode(&args).unwrap())
        };
        call(&mut h, "hold", json!({ "ref": "m:o:1", "bundle": { "currency": { "gold": 60 } } })).unwrap();
        call(&mut h, "hold", json!({ "ref": "m:o:1", "bundle": { "currency": { "gold": 60 } } })).unwrap();
        let w: WalletView = h.call_typed(Caller::Player(7), "wallet", &()).unwrap();
        assert_eq!((w.balances["gold"], w.held["gold"]), (40, 60), "held once");
        call(&mut h, "settle", json!({ "ref": "m:f:1", "hold": "m:o:1", "bundle": { "currency": { "gold": 25 } } })).unwrap();
        call(&mut h, "settle", json!({ "ref": "m:f:1", "hold": "m:o:1", "bundle": { "currency": { "gold": 25 } } })).unwrap();
        let back: Bundle = decode(&call(&mut h, "release", json!({ "ref": "m:o:1:r", "hold": "m:o:1" })).unwrap()).unwrap();
        assert_eq!(back, Bundle::coins("gold", 35), "settled once, the rest released");
        let e = call(&mut h, "hold", json!({ "ref": "m:o:2", "bundle": { "items": { "relic": 1 } } })).unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN), "a relic does not change hands");
        let e = h
            .call(Caller::Player(7), "deposit", &encode(&json!({ "ref": "x", "bundle": { "currency": { "gold": 1 } } })).unwrap())
            .unwrap_err();
        assert_eq!(code(&e), Some(codes::FORBIDDEN), "a player does not give themselves gold");
        call(&mut h, "exchange", json!({ "ref": "m:b:1", "pay": { "currency": { "gold": 50 } }, "get": { "items": { "sword": 1 } } }))
            .unwrap();
        let e =
            call(&mut h, "exchange", json!({ "ref": "m:b:2", "pay": { "currency": { "gold": 50 } }, "get": { "items": { "sword": 1 } } }))
                .unwrap_err();
        assert_eq!(code(&e), Some(codes::INSUFFICIENT));
    }

    #[test]
    fn loot_is_seeded_with_what_a_client_cannot_see() {
        let draws = |entropy: u64, secret: Option<&str>| {
            let config = Config { secret: secret.map(String::from), ..Config::default() };
            let mut hub = PlayerHub::new(config, 7, 1, entropy).unwrap();
            (0..8).map(|_| hub.rng.next_u64()).collect::<Vec<_>>()
        };
        assert_ne!(draws(1, None), draws(2, None), "another instance or start rolls otherwise");
        assert_ne!(draws(1, None), draws(1, Some("s3cret")), "and so does the config's secret");
    }

    #[test]
    fn a_snapshot_from_before_loads_and_another_key_is_refused() {
        let bytes = encode(hub().hub()).unwrap();
        let back: PlayerHub = decode(&bytes).unwrap();
        assert_eq!(back.owner, 7);
        assert!(TestHub::<PlayerHub>::spawn("player", "not-a-player").is_err());
    }
}
