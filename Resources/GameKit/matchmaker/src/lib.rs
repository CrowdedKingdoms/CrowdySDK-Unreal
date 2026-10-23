//! A matchmaking queue, keyed by its name (`ranked`, `casual`, ...). Players join with a
//! rating; the window of ratings they accept widens the longer they wait; a match is proposed
//! when enough players fall in each other's windows, and every one of them must accept in
//! time. When all do, the queue reserves a `session` hub for them (the manifest lets this type
//! call `session`) and tells each player its key. Replaces the `matchmaking` template.
//!
//! A party queues as one block: its leader joins with `party` (the other members' ids), each
//! member confirms by joining with `leader`, and once all have, the block waits in the queue
//! and is matched whole, or not at all.
//!
//! Endpoints (players): `join { rating, party?, leader? }`, `leave`, `accept`, `decline`,
//! `status` (your own place: forming a party, queued, proposed or matched). `configure { .. }`
//! (developers). It speaks the kit protocol (`ckx_sdk::kit`): the `queue` topic carries
//! `snapshot` messages of how many are waiting, never who, and so does `snapshot`; changes
//! may carry an `action_id`; refusals carry `kit:<code>:` codes. A client polls `status` while
//! it waits.

use std::collections::{BTreeMap, BTreeSet};

use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;

pub const TICK_MS: u64 = 1_000;
pub const TOPIC: &str = "queue";
/// How long a player's match stays in `status` after it is made.
const MATCHED_KEEP_MS: u64 = 120_000;
const MAX_QUEUE: usize = 10_000;
/// The most players one party may hold.
pub const MAX_PARTY: usize = 8;
const MAX_FORMING: usize = 2_000;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Config {
    pub players_per_match: usize,
    /// The rating difference a player accepts at first, and how it widens each second.
    pub window: u32,
    pub widen_per_sec: u32,
    pub max_window: u32,
    pub accept_ms: u64,
    /// How long a party has for its members to confirm.
    pub party_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self { players_per_match: 2, window: 100, widen_per_sec: 10, max_window: 500, accept_ms: 15_000, party_ms: 60_000 }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Entry {
    pub player: u64,
    pub rating: u32,
    pub since: u64,
    /// The leader of the party this entry queues with.
    #[serde(default)]
    pub party: Option<u64>,
}

impl Entry {
    /// The block it is matched in: its party's leader, or the player alone.
    fn block(&self) -> u64 {
        self.party.unwrap_or(self.player)
    }
}

/// A party its members are still confirming.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Party {
    pub leader: u64,
    pub members: Vec<u64>,
    /// Each confirmed member's rating; the leader's from the start.
    pub ratings: BTreeMap<u64, u32>,
    pub since: u64,
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Proposal {
    pub id: u64,
    pub entries: Vec<Entry>,
    pub accepted: BTreeSet<u64>,
    pub expires: u64,
}

impl Proposal {
    fn has(&self, player: u64) -> bool {
        self.entries.iter().any(|e| e.player == player)
    }

    pub fn players(&self) -> Vec<u64> {
        self.entries.iter().map(|e| e.player).collect()
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct Matched {
    pub session: String,
    pub at: u64,
}

#[derive(Serialize, Deserialize, Debug, PartialEq)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum Place {
    Idle,
    /// In a party its members are confirming.
    Party {
        leader: u64,
        members: Vec<u64>,
        confirmed: Vec<u64>,
        expires_in_ms: u64,
    },
    Queued {
        rating: u32,
        waited_ms: u64,
        window: u32,
    },
    Proposed {
        players: Vec<u64>,
        accepted: Vec<u64>,
        expires_in_ms: u64,
    },
    Matched {
        session: String,
    },
}

/// What the `queue` topic carries: how many, never who.
#[derive(Serialize, Deserialize, Debug, PartialEq, Eq, Default)]
pub struct Counts {
    pub waiting: usize,
    pub proposals: usize,
    pub forming: usize,
}

#[derive(Deserialize)]
struct Join {
    rating: u32,
    /// The other members, when the caller leads a party.
    #[serde(default)]
    party: Vec<u64>,
    /// The party's leader, when the caller confirms a place in one.
    #[serde(default)]
    leader: Option<u64>,
}

#[derive(Serialize)]
struct Reserve<'a> {
    players: &'a [u64],
}

#[derive(Serialize, Deserialize, Default, Debug)]
pub struct Matchmaker {
    pub config: Config,
    pub queue: Vec<Entry>,
    pub proposals: BTreeMap<u64, Proposal>,
    pub matched: BTreeMap<u64, Matched>,
    pub next_id: u64,
    /// Parties being confirmed, by leader.
    #[serde(default)]
    pub forming: BTreeMap<u64, Party>,
}

impl Matchmaker {
    fn window(&self, e: &Entry, now: u64) -> u32 {
        let waited = (now.saturating_sub(e.since) / 1_000) as u32;
        self.config.window.saturating_add(self.config.widen_per_sec.saturating_mul(waited)).min(self.config.max_window)
    }

    fn proposal_of(&self, player: u64) -> Option<u64> {
        self.proposals.values().find(|p| p.has(player)).map(|p| p.id)
    }

    fn forming_of(&self, player: u64) -> Option<u64> {
        self.forming.values().find(|p| p.members.contains(&player)).map(|p| p.leader)
    }

    pub fn counts(&self) -> Counts {
        Counts { waiting: self.queue.len(), proposals: self.proposals.len(), forming: self.forming.len() }
    }

    /// Takes the player out of any forming party; the party goes with them.
    fn drop_forming(&mut self, player: u64) {
        if let Some(leader) = self.forming_of(player) {
            self.forming.remove(&leader);
        }
    }

    /// Takes the player's block out of the queue: a party leaves with any one member.
    fn drop_queued(&mut self, player: u64) {
        if let Some(block) = self.queue.iter().find(|e| e.player == player).map(Entry::block) {
            self.queue.retain(|e| e.block() != block);
        }
    }

    pub fn join(&mut self, player: u64, rating: u32, now: u64) -> Result<()> {
        self.join_with(player, rating, &[], None, now)
    }

    /// Joins alone, leads a party (`party`: the other members), or confirms a place in the
    /// party `leader` leads.
    pub fn join_with(&mut self, player: u64, rating: u32, party: &[u64], leader: Option<u64>, now: u64) -> Result<()> {
        if self.proposal_of(player).is_some() {
            return Err(kit::error(codes::CONFLICT, "you have a match to accept"));
        }
        self.matched.remove(&player);
        if let Some(leader) = leader.filter(|l| *l != player) {
            let p = self.forming.get_mut(&leader).ok_or_else(|| kit::error(codes::NOT_FOUND, "no party is forming under that leader"))?;
            if !p.members.contains(&player) {
                return Err(kit::error(codes::FORBIDDEN, "you are not in that party"));
            }
            p.ratings.insert(player, rating);
            if p.ratings.len() == p.members.len() {
                let p = self.forming.remove(&leader).expect("found above");
                for m in &p.members {
                    self.drop_queued(*m);
                }
                for m in &p.members {
                    self.queue.push(Entry { player: *m, rating: p.ratings[m], since: now, party: Some(leader) });
                }
            }
            return Ok(());
        }
        if !party.is_empty() {
            let mut members = vec![player];
            for m in party {
                if !members.contains(m) {
                    members.push(*m);
                }
            }
            let max = self.config.players_per_match.min(MAX_PARTY);
            if members.len() < 2 || members.len() > max {
                return Err(kit::error(codes::LIMIT, format!("a party is 2 to {max} players")));
            }
            if members.iter().any(|m| self.proposal_of(*m).is_some()) {
                return Err(kit::error(codes::CONFLICT, "a member has a match to accept"));
            }
            if !self.forming.contains_key(&player) && self.forming.len() >= MAX_FORMING {
                return Err(kit::error(codes::FULL, "too many parties are forming"));
            }
            self.drop_forming(player);
            self.drop_queued(player);
            self.forming.insert(player, Party { leader: player, members, ratings: BTreeMap::from([(player, rating)]), since: now });
            return Ok(());
        }
        self.drop_forming(player);
        if let Some(e) = self.queue.iter_mut().find(|e| e.player == player) {
            e.rating = rating;
            return Ok(());
        }
        if self.queue.len() >= MAX_QUEUE {
            return Err(kit::error(codes::FULL, "the queue is full"));
        }
        self.queue.push(Entry { player, rating, since: now, party: None });
        Ok(())
    }

    pub fn leave(&mut self, player: u64) {
        self.drop_forming(player);
        self.drop_queued(player);
        if let Some(id) = self.proposal_of(player) {
            self.decline(player, id);
        }
    }

    /// Proposes matches from the queue, oldest blocks first. A block (a player, or a party)
    /// joins a match whole, or waits.
    pub fn propose(&mut self, now: u64) -> Vec<u64> {
        let n = self.config.players_per_match.max(1);
        let mut waiting = std::mem::take(&mut self.queue);
        waiting.sort_by_key(|e| e.since);
        let mut order: Vec<u64> = Vec::new();
        let mut blocks: BTreeMap<u64, Vec<Entry>> = BTreeMap::new();
        for e in waiting {
            let b = e.block();
            if !blocks.contains_key(&b) {
                order.push(b);
            }
            blocks.entry(b).or_default().push(e);
        }
        // A block's rating is its members' mean; its window is its narrowest.
        let stats: BTreeMap<u64, (u32, u32, usize)> = blocks
            .iter()
            .map(|(b, es)| {
                let rating = (es.iter().map(|e| u64::from(e.rating)).sum::<u64>() / es.len().max(1) as u64) as u32;
                let window = es.iter().map(|e| self.window(e, now)).min().unwrap_or(0);
                (*b, (rating, window, es.len()))
            })
            .collect();
        let mut made = Vec::new();
        let mut used: BTreeSet<u64> = BTreeSet::new();
        for (i, a) in order.iter().enumerate() {
            if used.contains(a) {
                continue;
            }
            let (ra, wa, mut size) = stats[a];
            let mut picked = vec![*a];
            for b in &order[i + 1..] {
                if size == n {
                    break;
                }
                let (rb, wb, len) = stats[b];
                if used.contains(b) || size + len > n {
                    continue;
                }
                if ra.abs_diff(rb) <= wa.min(wb) {
                    picked.push(*b);
                    size += len;
                }
            }
            if size != n {
                continue;
            }
            used.extend(picked.iter().copied());
            let entries: Vec<Entry> = picked.iter().flat_map(|b| blocks[b].clone()).collect();
            self.next_id += 1;
            let id = self.next_id;
            self.proposals.insert(id, Proposal { id, entries, accepted: BTreeSet::new(), expires: now + self.config.accept_ms });
            made.push(id);
        }
        self.queue = order.iter().filter(|b| !used.contains(b)).flat_map(|b| blocks.remove(b).unwrap_or_default()).collect();
        made
    }

    /// Returns the proposal when this acceptance completes it.
    pub fn accept(&mut self, player: u64) -> Result<Option<Proposal>> {
        let id = self.proposal_of(player).ok_or_else(|| kit::error(codes::NOT_FOUND, "you have no match to accept"))?;
        let p = self.proposals.get_mut(&id).expect("found above");
        p.accepted.insert(player);
        if p.accepted.len() < p.entries.len() {
            return Ok(None);
        }
        Ok(self.proposals.remove(&id))
    }

    /// The decliner's block leaves the queue; the others go back, keeping their place.
    pub fn decline(&mut self, player: u64, id: u64) {
        if let Some(p) = self.proposals.remove(&id) {
            let gone = p.entries.iter().find(|e| e.player == player).map(Entry::block);
            self.queue.extend(p.entries.into_iter().filter(|e| Some(e.block()) != gone));
        }
    }

    /// Proposals past their time: the blocks whose every member accepted go back, the others
    /// leave. Parties past their time to confirm are dropped.
    pub fn expire(&mut self, now: u64) {
        let late: Vec<u64> = self.proposals.values().filter(|p| p.expires <= now).map(|p| p.id).collect();
        for id in late {
            if let Some(p) = self.proposals.remove(&id) {
                let declined: BTreeSet<u64> = p.entries.iter().filter(|e| !p.accepted.contains(&e.player)).map(Entry::block).collect();
                self.queue.extend(p.entries.into_iter().filter(|e| !declined.contains(&e.block())));
            }
        }
        let party_ms = self.config.party_ms;
        self.forming.retain(|_, p| now.saturating_sub(p.since) < party_ms);
        self.matched.retain(|_, m| now.saturating_sub(m.at) < MATCHED_KEEP_MS);
    }

    pub fn status(&self, player: u64, now: u64) -> Place {
        if let Some(p) = self.forming_of(player).and_then(|l| self.forming.get(&l)) {
            return Place::Party {
                leader: p.leader,
                members: p.members.clone(),
                confirmed: p.ratings.keys().copied().collect(),
                expires_in_ms: (p.since + self.config.party_ms).saturating_sub(now),
            };
        }
        if let Some(e) = self.queue.iter().find(|e| e.player == player) {
            return Place::Queued { rating: e.rating, waited_ms: now.saturating_sub(e.since), window: self.window(e, now) };
        }
        if let Some(p) = self.proposal_of(player).and_then(|id| self.proposals.get(&id)) {
            return Place::Proposed {
                players: p.players(),
                accepted: p.accepted.iter().copied().collect(),
                expires_in_ms: p.expires.saturating_sub(now),
            };
        }
        match self.matched.get(&player) {
            Some(m) => Place::Matched { session: m.session.clone() },
            None => Place::Idle,
        }
    }

    fn busy(&self) -> bool {
        !self.queue.is_empty() || !self.proposals.is_empty() || !self.forming.is_empty()
    }
}

/// The hub: the queue, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize, Default, Debug)]
pub struct MatchmakerHub {
    #[serde(flatten)]
    pub mm: Matchmaker,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl MatchmakerHub {
    fn act(&mut self, ctx: &Ctx, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let mm = &mut self.mm;
        let out = match call.method {
            "join" => {
                let player = call.player()?;
                let req: Join = kit::args(call)?;
                mm.join_with(player, req.rating, &req.party, req.leader, now)?;
                encode(&mm.status(player, now))
            }
            "leave" => {
                mm.leave(call.player()?);
                encode(&true)
            }
            "accept" => {
                let player = call.player()?;
                if let Some(p) = mm.accept(player)? {
                    // The epoch rises on every placement, so a queue restarted without its
                    // snapshot never hands out a key an earlier match used.
                    let session = format!("{}-{}-{}", ctx.key, ctx.epoch, p.id);
                    let players = p.players();
                    match ctx.call_typed::<_, bool>("session", &session, "reserve", &Reserve { players: &players }) {
                        Ok(_) => {
                            for player in players {
                                mm.matched.insert(player, Matched { session: session.clone(), at: now });
                            }
                        }
                        Err(e) => {
                            ctx.log(Level::Warn, &format!("reserving session {session}: {e}"));
                            mm.queue.extend(p.entries);
                        }
                    }
                }
                encode(&mm.status(player, now))
            }
            "decline" => {
                let player = call.player()?;
                if let Some(id) = mm.proposal_of(player) {
                    mm.decline(player, id);
                }
                encode(&true)
            }
            "configure" => {
                call.developer().map_err(|_| kit::error(codes::FORBIDDEN, "configure is for the app's developers"))?;
                let config: Config = kit::args(call)?;
                if !(1..=64).contains(&config.players_per_match) || config.accept_ms < 1_000 || config.party_ms < 1_000 {
                    return Err(kit::error(
                        codes::BAD_REQUEST,
                        "1 <= players_per_match <= 64, and accept_ms and party_ms are at least 1000",
                    ));
                }
                mm.config = config;
                encode(&true)
            }
            other => return Err(Error::unknown_method(other)),
        };
        if mm.busy() && !ctx.timers().iter().any(|(name, _)| name == "match") {
            ctx.timer_every("match", TICK_MS)?;
        }
        out
    }
}

impl Hub for MatchmakerHub {
    fn spawn(_ctx: &Ctx, _seed: &[u8]) -> Result<Self> {
        Ok(Self::default())
    }

    /// Reads this version's snapshots and the earlier ones, which held the queue alone.
    fn load(_ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        decode(snapshot)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        let now = ctx.now_ms();
        match call.method {
            "status" => encode(&self.mm.status(call.player()?, now)),
            kit::SNAPSHOT_METHOD => self.kit.snapshot_reply(ctx, TOPIC, &self.mm.counts()),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, now, |h| h.act(ctx, &call, now)),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "match" {
            return Ok(());
        }
        let now = ctx.now_ms();
        self.mm.expire(now);
        self.mm.propose(now);
        self.kit.snapshot(ctx, TOPIC, &self.mm.counts())?;
        if !self.mm.busy() {
            ctx.cancel_timer("match");
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(MatchmakerHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Message, MessageKind, error_code};
    use ckx_sdk::testing::TestHub;

    #[test]
    fn close_ratings_match_at_once_and_far_ones_after_the_window_widens() {
        let mut m = Matchmaker::default();
        m.join(1, 1000, 0).unwrap();
        m.join(2, 1050, 0).unwrap();
        m.join(3, 1400, 0).unwrap();
        let first = m.propose(0);
        assert_eq!(first.len(), 1);
        assert_eq!(m.proposals[&first[0]].players(), vec![1, 2]);
        m.join(4, 1700, 0).unwrap();
        assert!(m.propose(10_000).is_empty(), "300 apart, windows are 200");
        assert_eq!(m.propose(20_000).len(), 1, "windows reach 300 after 20 s");
    }

    #[test]
    fn everyone_must_accept_and_a_decline_requeues_the_others() {
        let mut m = Matchmaker::default();
        m.join(1, 1000, 0).unwrap();
        m.join(2, 1000, 5).unwrap();
        m.propose(10);
        assert!(m.accept(1).unwrap().is_none());
        assert!(matches!(m.status(2, 10), Place::Proposed { .. }));
        let id = m.proposal_of(2).unwrap();
        m.decline(2, id);
        assert_eq!(m.status(1, 10), Place::Queued { rating: 1000, waited_ms: 10, window: 100 }, "keeps its place");
        assert_eq!(m.status(2, 10), Place::Idle);
        m.join(2, 1000, 20).unwrap();
        m.propose(20);
        m.accept(1).unwrap();
        let done = m.accept(2).unwrap().expect("complete");
        assert_eq!(done.players(), vec![1, 2]);
    }

    #[test]
    fn an_expired_proposal_requeues_only_who_accepted() {
        let mut m = Matchmaker::default();
        m.join(1, 1000, 0).unwrap();
        m.join(2, 1000, 0).unwrap();
        m.propose(0);
        m.accept(1).unwrap();
        m.expire(m.config.accept_ms);
        assert!(m.proposals.is_empty());
        assert_eq!(m.queue.iter().map(|e| e.player).collect::<Vec<_>>(), vec![1]);
    }

    #[test]
    fn a_player_with_a_match_to_accept_cannot_rejoin() {
        let mut m = Matchmaker::default();
        m.join(1, 1000, 0).unwrap();
        m.join(2, 1000, 0).unwrap();
        m.propose(0);
        let Error(why) = m.join(1, 900, 1).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::CONFLICT));
        m.leave(1);
        assert_eq!(m.queue.iter().map(|e| e.player).collect::<Vec<_>>(), vec![2]);
    }

    #[test]
    fn a_party_queues_once_every_member_confirms_and_is_matched_whole() {
        let mut m = Matchmaker { config: Config { players_per_match: 4, ..Config::default() }, ..Default::default() };
        m.join_with(1, 1000, &[2, 3], None, 0).unwrap();
        assert!(matches!(m.status(2, 0), Place::Party { leader: 1, .. }));
        let Error(why) = m.join_with(9, 1000, &[], Some(1), 0).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::FORBIDDEN), "not a member");
        m.join_with(2, 1100, &[], Some(1), 1).unwrap();
        assert!(m.queue.is_empty(), "3 has not confirmed");
        m.join_with(3, 900, &[], Some(1), 2).unwrap();
        assert_eq!(m.queue.len(), 3);
        assert!(m.queue.iter().all(|e| e.party == Some(1)));
        // Three players of a party and one alone make four; two alone do not split the party.
        m.join(4, 1000, 3).unwrap();
        m.join(5, 1000, 3).unwrap();
        let ids = m.propose(3);
        assert_eq!(ids.len(), 1);
        let mut players = m.proposals[&ids[0]].players();
        players.sort_unstable();
        assert_eq!(players, vec![1, 2, 3, 4]);
        assert_eq!(m.queue.iter().map(|e| e.player).collect::<Vec<_>>(), vec![5]);
        // One member declining takes the whole party out; the solo player goes back.
        m.decline(2, ids[0]);
        assert_eq!(m.queue.iter().map(|e| e.player).collect::<Vec<_>>(), vec![5, 4]);
    }

    #[test]
    fn a_party_too_large_or_left_unconfirmed_goes() {
        let mut m = Matchmaker::default();
        let Error(why) = m.join_with(1, 1000, &[2, 3], None, 0).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::LIMIT), "two a match");
        m.join_with(1, 1000, &[2], None, 0).unwrap();
        m.expire(m.config.party_ms);
        assert!(m.forming.is_empty());
        assert_eq!(m.status(1, m.config.party_ms), Place::Idle);
    }

    #[test]
    fn the_queue_topic_counts_and_never_names_and_old_snapshots_load() {
        #[derive(Serialize)]
        struct J {
            rating: u32,
        }
        let mut hub = TestHub::<MatchmakerHub>::spawn("matchmaker", "ranked").unwrap();
        let _: Place = hub.call_typed(Caller::Player(1), "join", &J { rating: 1000 }).unwrap();
        hub.advance(TICK_MS).unwrap();
        let (topic, bytes) = hub.published().last().cloned().unwrap();
        assert_eq!(topic, TOPIC);
        let msg: Message<Counts> = decode(&bytes).unwrap();
        assert_eq!((msg.header.kind, msg.body.waiting), (MessageKind::Snapshot, 1));
        drop(hub);
        let mut old = Matchmaker::default();
        old.join(7, 1200, 0).unwrap();
        let hub = TestHub::<MatchmakerHub>::load("matchmaker", "ranked", &encode(&old).unwrap(), 1).unwrap();
        assert_eq!(hub.hub().mm.queue.len(), 1);
    }
}
