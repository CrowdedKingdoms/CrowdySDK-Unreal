//! The mobs, NPCs and pets of one area of the world. The Instance Id is the area's place on a
//! grid of 4 by 4 chunks, `x,y`, as This Actor's Area gives it for a player standing in it
//! (ground x and y, z up). They are moved on a timer and shown to nearby players as server-driven
//! actors. This is the Unreal SDK's copy of the platform's starter. Mobs wander near home, chase
//! the nearest player in range, go home past their leash, take players' hits and respawn; NPCs
//! walk their routes and talk; pets belong to a player and follow them while summoned. Mobs see
//! further at night and under an event's `aggro` modifier, from the root `world` hub's topic.
//! All of them are typed containers ([`ckx_sdk::model`]), so players read only their public
//! properties (a pet's owner also reads its bond). Replaces the `mob-engine` and `npc-engine`
//! templates.
//!
//! Endpoints: `state` (anyone), `attack { mob }` and `talk { npc }` (players), and a player's
//! pets: `adopt { species, name }`, `rename { pet, name }`, `summon { pet }`, `dismiss { pet }`,
//! `release { pet }` and `pets` (the caller's, with their bond). The type needs the
//! `world.read` scope (where players are) and may call `world` (for its topic).
//!
//! It speaks the kit protocol (`ckx_sdk::kit`): public property changes go out on the `area`
//! topic as `delta`s once a second, `snapshot { topic: "area" }` answers every container's
//! public view, changes may carry an `action_id`, and refusals carry `kit:<code>:` codes.
//! Actors are shown with [`ActorState`] (`kit::wire`).

use std::collections::BTreeMap;

use ckx_sdk::kit::wire::{self, ActorState};
use ckx_sdk::kit::{self, Publisher, Receipts, codes};
use ckx_sdk::prelude::*;
use serde_json::{Value, json};

pub const TICK_MS: u64 = 250;
pub const TOPIC: &str = "area";
/// Chunks along one side of an area; the key counts areas, not chunks.
pub const AREA_CHUNKS: i64 = 4;
/// The largest area index in either axis a key may name, small enough that `M<key>@<id>` fits an actor id's 32 octets.
const KEY_LIMIT: i64 = 1 << 32;
/// Ticks between scans of where players are, and between `area` pushes.
const SCAN_EVERY: u64 = 4;
const MOBS: u64 = 6;
/// Distances are in chunks, speeds in chunks a second.
const AGGRO: f64 = 2.5;
const LEASH: f64 = 6.0;
const MOB_SPEED: f64 = 0.5;
const NPC_SPEED: f64 = 0.25;
const PET_SPEED: f64 = 1.0;
/// How close a following pet keeps to its owner.
const PET_HEEL: f64 = 0.8;
/// How near a player must be to hit a mob.
const REACH: f64 = 1.5;
/// The server decides a hit's damage, never the client.
const HIT: i64 = 10;
const HIT_COOLDOWN_MS: u64 = 500;
const RESPAWN_MS: u64 = 10_000;
/// Players within this many chunks of a mob see it move.
const SHOW_DISTANCE: u8 = 2;
/// Pets one player may keep in an area, and the area may hold in all.
pub const PETS_PER_PLAYER: usize = 3;
const PETS_MAX: usize = 64;
const NAME_MAX: usize = 24;
/// Ground-y offsets of the scans: each reaches 1 chunk either side, so together ground y within 4.
const SCAN_Y_OFFSETS: [i64; 3] = [-3, 0, 3];

pub fn schema() -> Schema {
    Schema::new()
        .container("Mob", |c| {
            c.prop("kind", PropKind::Str, "wolf", Visibility::Public)
                .prop("hp", PropKind::Int, 100, Visibility::Public)
                .prop("max_hp", PropKind::Int, 100, Visibility::Public)
                .prop("x", PropKind::Float, 0.0, Visibility::Public)
                .prop("y", PropKind::Float, 0.0, Visibility::Public)
                .prop("mode", PropKind::Str, "idle", Visibility::Public)
                .prop("home_x", PropKind::Float, 0.0, Visibility::Hidden)
                .prop("home_y", PropKind::Float, 0.0, Visibility::Hidden)
                .prop("respawn_at", PropKind::Int, 0, Visibility::Hidden)
        })
        .container("Npc", |c| {
            c.prop("name", PropKind::Str, "", Visibility::Public)
                .prop("line", PropKind::Str, "Well met.", Visibility::Public)
                .prop("x", PropKind::Float, 0.0, Visibility::Public)
                .prop("y", PropKind::Float, 0.0, Visibility::Public)
                .prop("route",PropKind::Json, json!([]), Visibility::Hidden)
                .prop("leg", PropKind::Int, 0, Visibility::Hidden)
        })
        .container("Pet", |c| {
            c.prop("species", PropKind::Str, "cat", Visibility::Public)
                .prop("name", PropKind::Str, "", Visibility::Public)
                .prop("x", PropKind::Float, 0.0, Visibility::Public)
                .prop("y", PropKind::Float, 0.0, Visibility::Public)
                .prop("mode", PropKind::Str, "stay", Visibility::Public)
                .prop("bond", PropKind::Int, 0, Visibility::Owner)
        })
}

#[derive(Deserialize)]
struct Conditions {
    night: bool,
    #[serde(default)]
    modifiers: BTreeMap<String, f64>,
}

#[derive(Deserialize)]
struct Attack {
    mob: u64,
}

#[derive(Deserialize)]
struct Talk {
    npc: u64,
}

#[derive(Deserialize)]
struct Adopt {
    species: String,
    name: String,
}

#[derive(Deserialize)]
struct PetRef {
    pet: u64,
    #[serde(default)]
    name: Option<String>,
}

fn dist(a: (f64, f64), b: (f64, f64)) -> f64 {
    ((a.0 - b.0).powi(2) + (a.1 - b.1).powi(2)).sqrt()
}

/// `from` moved toward `to` by at most `step`.
fn toward(from: (f64, f64), to: (f64, f64), step: f64) -> (f64, f64) {
    let d = dist(from, to);
    if d <= step || d == 0.0 {
        return to;
    }
    (from.0 + (to.0 - from.0) * step / d, from.1 + (to.1 - from.1) * step / d)
}

fn short_text(what: &str, s: &str, max: usize) -> Result<String> {
    let t = s.trim();
    if t.is_empty() || t.chars().count() > max {
        return Err(kit::error(codes::BAD_REQUEST, format!("a {what} is 1 to {max} characters")));
    }
    Ok(t.to_owned())
}

#[derive(Serialize, Deserialize)]
pub struct Area {
    #[serde(skip, default = "schema")]
    schema: Schema,
    pub model: Model,
    pub center: (i64, i64),
    /// Snapshots from the y-up starter lack this and start again.
    #[serde(default)]
    pub z_up: bool,
    pub ticks: u64,
    pub night: bool,
    pub aggro_scale: f64,
    /// Where each nearby player was at the last scan, in chunks.
    pub players: BTreeMap<u64, (f64, f64)>,
    pub last_hit: BTreeMap<u64, u64>,
}

impl Area {
    pub fn new(key: &str) -> Result<Self> {
        let canonical = kit::shard::parse(key).filter(|(x, y)| format!("{x},{y}") == key && x.abs() <= KEY_LIMIT && y.abs() <= KEY_LIMIT);
        let Some((ax, ay)) = canonical else {
            return Err(kit::error(codes::BAD_REQUEST, "an area's key is its x,y on the 4-chunk grid"));
        };
        let center = (ax * AREA_CHUNKS + AREA_CHUNKS / 2, ay * AREA_CHUNKS + AREA_CHUNKS / 2);
        let mut area = Self {
            schema: schema(),
            model: Model::new(),
            center,
            z_up: true,
            ticks: 0,
            night: false,
            aggro_scale: 1.0,
            players: BTreeMap::new(),
            last_hit: BTreeMap::new(),
        };
        let (cx, cy) = (center.0 as f64 + 0.5, center.1 as f64 + 0.5);
        for i in 0..MOBS {
            let angle = i as f64 * std::f64::consts::TAU / MOBS as f64;
            let home = (cx + 3.0 * angle.cos(), cy + 3.0 * angle.sin());
            let id = area.model.create(&area.schema, "Mob", None)?;
            for (k, v) in [("x", home.0), ("y", home.1), ("home_x", home.0), ("home_y", home.1)] {
                area.model.set(&area.schema, id, k, v)?;
            }
        }
        for (name, line, route) in [
            (
                "Warden",
                "Mind the wolves after dark.",
                json!([[cx - 2.0, cy - 2.0], [cx + 2.0, cy - 2.0], [cx + 2.0, cy + 2.0], [cx - 2.0, cy + 2.0]]),
            ),
            ("Trader", "Fair prices, mostly.", json!([[cx - 1.0, cy], [cx + 1.0, cy]])),
        ] {
            let id = area.model.create(&area.schema, "Npc", None)?;
            area.model.set(&area.schema, id, "name", name)?;
            area.model.set(&area.schema, id, "line", line)?;
            area.model.set(&area.schema, id, "x", route[0][0].as_f64().unwrap_or(cx))?;
            area.model.set(&area.schema, id, "y", route[0][1].as_f64().unwrap_or(cy))?;
            area.model.set(&area.schema, id, "route", route)?;
        }
        Ok(area)
    }

    /// The area's key, `x,y`: its actors carry it, so a client knows which area to call.
    pub fn key(&self) -> String {
        let half = AREA_CHUNKS / 2;
        format!("{},{}", (self.center.0 - half) / AREA_CHUNKS, (self.center.1 - half) / AREA_CHUNKS)
    }

    fn pos(&self, id: u64) -> Result<(f64, f64)> {
        Ok((self.model.get_f64(&self.schema, id, "x")?, self.model.get_f64(&self.schema, id, "y")?))
    }

    fn place(&mut self, id: u64, to: (f64, f64)) -> Result<()> {
        if dist(self.pos(id)?, to) > 0.01 {
            self.model.set(&self.schema, id, "x", to.0)?;
            self.model.set(&self.schema, id, "y", to.1)?;
        }
        Ok(())
    }

    fn set_mode(&mut self, id: u64, mode: &str) -> Result<()> {
        if self.model.get_str(&self.schema, id, "mode")? != mode {
            self.model.set(&self.schema, id, "mode", mode)?;
        }
        Ok(())
    }

    pub fn step_mobs(&mut self, now: u64, dt: f64) -> Result<()> {
        let range = AGGRO * self.aggro_scale * if self.night { 1.5 } else { 1.0 };
        let ids: Vec<u64> = self.model.of_type("Mob").collect();
        for id in ids {
            let home = (self.model.get_f64(&self.schema, id, "home_x")?, self.model.get_f64(&self.schema, id, "home_y")?);
            let respawn_at = self.model.get_i64(&self.schema, id, "respawn_at")?;
            if respawn_at > 0 {
                if now < respawn_at as u64 {
                    continue;
                }
                let max_hp = self.model.get_i64(&self.schema, id, "max_hp")?;
                self.model.set(&self.schema, id, "hp", max_hp)?;
                self.model.set(&self.schema, id, "respawn_at", 0)?;
                self.place(id, home)?;
            }
            let at = self.pos(id)?;
            let prey = self
                .players
                .values()
                .map(|p| (dist(at, *p), *p))
                .filter(|(d, p)| *d <= range && dist(home, *p) <= LEASH)
                .min_by(|a, b| a.0.total_cmp(&b.0))
                .map(|(_, p)| p);
            let (goal, mode) = if dist(at, home) > LEASH {
                (home, "return")
            } else if let Some(p) = prey {
                (p, "chase")
            } else {
                // Idle mobs circle their home slowly.
                let t = self.ticks as f64 * 0.02 + id as f64;
                ((home.0 + t.cos(), home.1 + t.sin()), "idle")
            };
            self.place(id, toward(at, goal, MOB_SPEED * dt))?;
            self.set_mode(id, mode)?;
        }
        Ok(())
    }

    pub fn step_npcs(&mut self, dt: f64) -> Result<()> {
        let ids: Vec<u64> = self.model.of_type("Npc").collect();
        for id in ids {
            let route = self.model.get(&self.schema, id, "route")?;
            let points: Vec<(f64, f64)> = route
                .as_array()
                .map(|a| a.iter().filter_map(|p| Some((p.get(0)?.as_f64()?, p.get(1)?.as_f64()?))).collect())
                .unwrap_or_default();
            if points.is_empty() {
                continue;
            }
            let leg = self.model.get_i64(&self.schema, id, "leg")?.rem_euclid(points.len() as i64) as usize;
            let next = toward(self.pos(id)?, points[leg], NPC_SPEED * dt);
            self.place(id, next)?;
            if dist(next, points[leg]) < 0.05 {
                self.model.set(&self.schema, id, "leg", ((leg + 1) % points.len()) as i64)?;
            }
        }
        Ok(())
    }

    /// Summoned pets walk to heel beside their owner, while the owner is in the area.
    pub fn step_pets(&mut self, dt: f64) -> Result<()> {
        let ids: Vec<u64> = self.model.of_type("Pet").collect();
        for id in ids {
            if self.model.get_str(&self.schema, id, "mode")? != "follow" {
                continue;
            }
            let Some(owner) = self.model.container(id).and_then(|c| c.owner) else { continue };
            let Some(&target) = self.players.get(&owner) else { continue };
            let at = self.pos(id)?;
            if dist(at, target) > PET_HEEL {
                let d = dist(at, target);
                let heel = (target.0 + (at.0 - target.0) * PET_HEEL / d, target.1 + (at.1 - target.1) * PET_HEEL / d);
                self.place(id, toward(at, heel, PET_SPEED * dt))?;
            }
        }
        Ok(())
    }

    /// A player's hit on a mob: in reach at the last scan, not too fast, fixed damage.
    pub fn attack(&mut self, player: u64, mob: u64, now: u64) -> Result<i64> {
        if self.model.container(mob).is_none_or(|c| c.type_name != "Mob") {
            return Err(kit::error(codes::NOT_FOUND, format!("no mob {mob}")));
        }
        if self.last_hit.get(&player).is_some_and(|at| now < at + HIT_COOLDOWN_MS) {
            return Err(kit::error(codes::COOLDOWN, "attacking too fast"));
        }
        let at = *self.players.get(&player).ok_or_else(|| kit::error("not_in_area", "you are not in this area"))?;
        if dist(at, self.pos(mob)?) > REACH {
            return Err(kit::error(codes::OUT_OF_REACH, "out of reach"));
        }
        if self.model.get_i64(&self.schema, mob, "respawn_at")? > 0 {
            return Err(kit::error("down", "that mob is down"));
        }
        self.last_hit.insert(player, now);
        let hp = self.model.add(&self.schema, mob, "hp", -HIT)?;
        if hp <= 0 {
            self.model.set(&self.schema, mob, "hp", 0)?;
            self.model.set(&self.schema, mob, "respawn_at", (now + RESPAWN_MS) as i64)?;
            self.set_mode(mob, "down")?;
        }
        Ok(hp.max(0))
    }

    /// A new pet for `player`, standing where they stand.
    pub fn adopt(&mut self, player: u64, species: &str, name: &str) -> Result<u64> {
        let at = *self.players.get(&player).ok_or_else(|| kit::error("not_in_area", "you are not in this area"))?;
        let (species, name) = (short_text("species", species, NAME_MAX)?, short_text("name", name, NAME_MAX)?);
        if self.pets_of(player).len() >= PETS_PER_PLAYER {
            return Err(kit::error(codes::LIMIT, format!("at most {PETS_PER_PLAYER} pets a player in an area")));
        }
        if self.model.of_type("Pet").count() >= PETS_MAX {
            return Err(kit::error(codes::FULL, "this area has as many pets as it holds"));
        }
        let id = self.model.create(&self.schema, "Pet", Some(player))?;
        self.model.set(&self.schema, id, "species", species)?;
        self.model.set(&self.schema, id, "name", name)?;
        self.place(id, at)?;
        Ok(id)
    }

    fn own_pet(&self, player: u64, pet: u64) -> Result<()> {
        match self.model.container(pet) {
            Some(c) if c.type_name == "Pet" && c.owner == Some(player) => Ok(()),
            Some(c) if c.type_name == "Pet" => Err(kit::error(codes::FORBIDDEN, "that is someone else's pet")),
            _ => Err(kit::error(codes::NOT_FOUND, format!("no pet {pet}"))),
        }
    }

    /// The player's pets as their owner reads them, bond included.
    pub fn pets_of(&self, player: u64) -> Vec<BTreeMap<String, Value>> {
        self.model
            .of_type("Pet")
            .filter(|id| self.model.container(*id).is_some_and(|c| c.owner == Some(player)))
            .filter_map(|id| {
                let mut v = self.model.view(&self.schema, id, Viewer::Player(player)).ok()?;
                self.name_member(&mut v, id);
                Some(v)
            })
            .collect()
    }

    pub fn pet_command(&mut self, player: u64, method: &str, pet: u64, name: Option<String>) -> Result<()> {
        self.own_pet(player, pet)?;
        match method {
            "rename" => {
                let name = short_text("name", name.as_deref().unwrap_or(""), NAME_MAX)?;
                self.model.set(&self.schema, pet, "name", name)?;
            }
            "summon" => {
                self.set_mode(pet, "follow")?;
                self.model.add(&self.schema, pet, "bond", 1)?;
            }
            "dismiss" => self.set_mode(pet, "stay")?,
            _ => {
                self.model.delete(pet);
            }
        }
        Ok(())
    }

    pub fn state(&self) -> Result<Vec<BTreeMap<String, Value>>> {
        let ids = self.model.of_type("Mob").chain(self.model.of_type("Npc")).chain(self.model.of_type("Pet"));
        ids.map(|id| {
            let mut v = self.model.view(&self.schema, id, Viewer::Public)?;
            self.name_member(&mut v, id);
            Ok(v)
        })
        .collect()
    }

    /// A reply's view of a member says which member it is.
    pub fn name_member(&self, view: &mut BTreeMap<String, Value>, id: u64) {
        view.insert("id".into(), json!(id));
        if let Some(c) = self.model.container(id) {
            view.insert("type_name".into(), json!(c.type_name));
        }
    }

    fn scan(&mut self, ctx: &Ctx) {
        let mut players = BTreeMap::new();
        for dy in SCAN_Y_OFFSETS {
            match ctx.world().actors_near(ChunkPos::new(self.center.0, self.center.1 + dy, 0), 3, 1) {
                Ok(actors) => {
                    players.extend(actors.into_iter().filter_map(|a| {
                        if a.chunk.z.abs() > 1 {
                            return None;
                        }
                        Some((a.player?, (a.chunk.x as f64 + 0.5, a.chunk.y as f64 + 0.5)))
                    }));
                }
                Err(e) => {
                    if self.ticks % (SCAN_EVERY * 60) == 1 {
                        ctx.log(Level::Warn, &format!("scanning for players: {e}"));
                    }
                    return;
                }
            }
        }
        self.players = players;
    }

    /// What players near each actor see of it.
    pub fn actor_states(&self) -> Result<Vec<([u8; 32], ActorState)>> {
        let mut out = Vec::new();
        for type_name in ["Mob", "Npc", "Pet"] {
            for id in self.model.of_type(type_name) {
                let (x, y) = self.pos(id)?;
                let mut state = ActorState::new(&type_name.to_lowercase(), id, x, 0.0);
                state.y = y;
                match type_name {
                    "Mob" => {
                        if self.model.get_i64(&self.schema, id, "respawn_at")? > 0 {
                            continue;
                        }
                        state.hp = Some(self.model.get_i64(&self.schema, id, "hp")?);
                        state.max_hp = Some(self.model.get_i64(&self.schema, id, "max_hp")?);
                        state.mode = Some(self.model.get_str(&self.schema, id, "mode")?);
                    }
                    "Pet" => {
                        state.owner = self.model.container(id).and_then(|c| c.owner);
                        state.name = Some(self.model.get_str(&self.schema, id, "name")?);
                        state.mode = Some(self.model.get_str(&self.schema, id, "mode")?);
                    }
                    _ => state.name = Some(self.model.get_str(&self.schema, id, "name")?),
                }
                let actor = actor_id(&format!("{}{}@{id}", &type_name[..1], self.key()));
                out.push((actor, state));
            }
        }
        Ok(out)
    }
}

/// The hub: the area, and what the kit protocol keeps beside it.
#[derive(Serialize, Deserialize)]
pub struct AreaHub {
    #[serde(flatten)]
    pub area: Area,
    #[serde(default)]
    pub kit: Publisher,
    #[serde(default)]
    pub receipts: Receipts,
}

impl AreaHub {
    fn act(&mut self, call: &Call<'_>, now: u64) -> Result<Vec<u8>> {
        let player = call.player()?;
        match call.method {
            "attack" => {
                let req: Attack = kit::args(call)?;
                encode(&self.area.attack(player, req.mob, now)?)
            }
            "adopt" => {
                let req: Adopt = kit::args(call)?;
                encode(&self.area.adopt(player, &req.species, &req.name)?)
            }
            "rename" | "summon" | "dismiss" | "release" => {
                let req: PetRef = kit::args(call)?;
                self.area.pet_command(player, call.method, req.pet, req.name)?;
                encode(&true)
            }
            other => Err(Error::unknown_method(other)),
        }
    }
}

impl Hub for AreaHub {
    fn spawn(ctx: &Ctx, _seed: &[u8]) -> Result<Self> {
        let area = Area::new(&ctx.key)?;
        ctx.timer_every("tick", TICK_MS)?;
        // The root hub's key is empty. Without a `world` type (or leave to call it) the area
        // runs without day and night.
        if let Err(e) = ctx.subscribe("world", "", "world") {
            ctx.log(Level::Info, &format!("not watching the world: {e}"));
        }
        Ok(Self { area, kit: Publisher::new(), receipts: Receipts::new() })
    }

    /// Reads this version's snapshots and the earlier ones, which held the area alone.
    fn load(ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {
        let mut hub: Self = decode(snapshot)?;
        if !hub.area.z_up {
            hub.area = Area::new(&ctx.key)?;
        }
        Ok(hub)
    }

    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {
        encode(self)
    }

    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {
        match call.method {
            "state" => encode(&self.area.state()?),
            kit::SNAPSHOT_METHOD => self.kit.model_snapshot_reply(ctx, &self.area.model, &self.area.schema, TOPIC),
            "talk" => {
                call.player()?;
                let req: Talk = kit::args(&call)?;
                if self.area.model.container(req.npc).is_none_or(|c| c.type_name != "Npc") {
                    return Err(kit::error(codes::NOT_FOUND, format!("no npc {}", req.npc)));
                }
                let mut view = self.area.model.view(&self.area.schema, req.npc, Viewer::Public)?;
                self.area.name_member(&mut view, req.npc);
                encode(&view)
            }
            "pets" => encode(&self.area.pets_of(call.player()?)),
            _ => Receipts::run(self, |h| &mut h.receipts, &call, ctx.now_ms(), |h| h.act(&call, ctx.now_ms())),
        }
    }

    fn on_timer(&mut self, ctx: &Ctx, timer: &str) -> Result<()> {
        if timer != "tick" {
            return Ok(());
        }
        let area = &mut self.area;
        area.ticks += 1;
        if area.ticks % SCAN_EVERY == 1 {
            area.scan(ctx);
        }
        let dt = TICK_MS as f64 / 1_000.0;
        area.step_mobs(ctx.now_ms(), dt)?;
        area.step_npcs(dt)?;
        area.step_pets(dt)?;
        if !area.players.is_empty() {
            for (actor, state) in area.actor_states()? {
                let _ = wire::emit_actor_state(ctx, &actor, SHOW_DISTANCE, &state);
            }
        }
        if area.ticks.is_multiple_of(SCAN_EVERY) {
            area.last_hit.retain(|_, at| ctx.now_ms().saturating_sub(*at) < 60_000);
            self.kit.model_changes(ctx, &mut self.area.model, &self.area.schema, TOPIC)?;
        }
        Ok(())
    }

    fn on_topic(&mut self, _ctx: &Ctx, msg: TopicMsg<'_>) -> Result<()> {
        if (msg.node_type, msg.topic) == ("world", "world") {
            let c: Conditions = msg.decode()?;
            self.area.night = c.night;
            self.area.aggro_scale = c.modifiers.get("aggro").copied().unwrap_or(1.0).clamp(0.1, 10.0);
        }
        Ok(())
    }
}

ckx_sdk::export_hub!(AreaHub);

#[cfg(test)]
mod tests {
    use super::*;
    use ckx_sdk::kit::{Containers, Message, MessageKind, error_code};
    use ckx_sdk::model::Published;
    use ckx_sdk::testing::TestHub;

    fn area() -> Area {
        Area::new("10,-4").unwrap()
    }

    fn first_mob(a: &Area) -> u64 {
        a.model.of_type("Mob").next().unwrap()
    }

    #[test]
    fn the_key_is_an_area_index_and_the_area_is_populated() {
        let a = area();
        assert_eq!(a.center, (42, -14));
        assert_eq!(a.model.of_type("Mob").count(), MOBS as usize);
        assert_eq!(a.model.of_type("Npc").count(), 2);
        assert_eq!(Area::new("-1,2").unwrap().center, (-2, 10));
        assert!(Area::new("-4294967296,-4294967296").is_ok());
        for bad in ["not a key", " 0,0", "+0,0", "00,0", "4294967297,0", "0,-4294967297"] {
            let Error(why) = Area::new(bad).err().unwrap_or_else(|| panic!("{bad:?} was accepted"));
            assert_eq!(error_code(&why).map(|c| c.0), Some(codes::BAD_REQUEST), "{bad:?}");
        }
    }

    #[test]
    fn a_mob_chases_a_near_player_and_goes_home_past_its_leash() {
        let mut a = area();
        let m = first_mob(&a);
        let at = a.pos(m).unwrap();
        a.players.insert(7, (at.0 + 1.0, at.1));
        a.step_mobs(0, 1.0).unwrap();
        assert_eq!(a.model.get_str(&a.schema, m, "mode").unwrap(), "chase");
        assert!(dist(a.pos(m).unwrap(), (at.0 + 1.0, at.1)) < 0.6);
        a.players.clear();
        a.place(m, (at.0 + LEASH + 5.0, at.1)).unwrap();
        a.step_mobs(0, 1.0).unwrap();
        assert_eq!(a.model.get_str(&a.schema, m, "mode").unwrap(), "return");
    }

    #[test]
    fn night_and_an_aggro_event_widen_the_range() {
        let mut a = area();
        let m = first_mob(&a);
        let at = a.pos(m).unwrap();
        a.players.insert(7, (at.0 + AGGRO + 0.5, at.1));
        a.step_mobs(0, 0.0).unwrap();
        assert_eq!(a.model.get_str(&a.schema, m, "mode").unwrap(), "idle");
        a.night = true;
        a.step_mobs(0, 0.0).unwrap();
        assert_eq!(a.model.get_str(&a.schema, m, "mode").unwrap(), "chase");
    }

    #[test]
    fn hits_need_reach_respect_the_cooldown_and_a_downed_mob_respawns() {
        let mut a = area();
        let m = first_mob(&a);
        let Error(why) = a.attack(7, m, 0).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some("not_in_area"));
        let at = a.pos(m).unwrap();
        a.players.insert(7, (at.0 + REACH + 1.0, at.1));
        assert!(a.attack(7, m, 0).is_err(), "out of reach");
        a.players.insert(7, at);
        assert_eq!(a.attack(7, m, 0).unwrap(), 90);
        assert!(a.attack(7, m, 100).is_err(), "cooldown");
        let mut now = 0;
        while a.model.get_i64(&a.schema, m, "hp").unwrap() > 0 {
            now += HIT_COOLDOWN_MS;
            a.attack(7, m, now).unwrap();
        }
        assert!(a.attack(7, m, now + HIT_COOLDOWN_MS).is_err(), "down");
        a.step_mobs(now + RESPAWN_MS, 0.0).unwrap();
        assert_eq!(a.model.get_i64(&a.schema, m, "hp").unwrap(), 100);
        let npc = a.model.of_type("Npc").next().unwrap();
        assert!(a.attack(7, npc, now + RESPAWN_MS).is_err(), "not a mob");
    }

    #[test]
    fn npcs_walk_their_route_and_players_see_only_public_properties() {
        let mut a = area();
        let warden = a.model.of_type("Npc").next().unwrap();
        let route: Vec<(f64, f64)> = a
            .model
            .get(&a.schema, warden, "route")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|p| (p[0].as_f64().unwrap(), p[1].as_f64().unwrap()))
            .collect();
        let mut reached = vec![false; route.len()];
        // 50 chunks of walking: three times round a 16-chunk loop.
        for _ in 0..200 {
            a.step_npcs(1.0).unwrap();
            let at = a.pos(warden).unwrap();
            for (i, corner) in route.iter().enumerate() {
                reached[i] |= dist(at, *corner) < 0.05;
            }
        }
        assert_eq!(reached, vec![true; 4]);
        let views = a.state().unwrap();
        assert!(views.iter().all(|v| !v.contains_key("route") && !v.contains_key("home_x") && !v.contains_key("respawn_at")));
        assert!(views.iter().any(|v| v.get("name") == Some(&json!("Warden"))));
    }

    #[test]
    fn a_pet_is_its_owners_follows_them_when_summoned_and_keeps_its_bond_private() {
        let mut a = area();
        assert!(a.adopt(7, "cat", "Mog").is_err(), "not in the area");
        let (cx, cy) = (a.center.0 as f64 + 0.5, a.center.1 as f64 + 0.5);
        a.players.insert(7, (cx, cy));
        let pet = a.adopt(7, "cat", "Mog").unwrap();
        a.players.insert(8, (cx + 1.0, cy));
        let Error(why) = a.pet_command(8, "summon", pet, None).unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::FORBIDDEN));
        a.pet_command(7, "summon", pet, None).unwrap();
        a.players.insert(7, (cx + 4.0, cy));
        for _ in 0..10 {
            a.step_pets(1.0).unwrap();
        }
        assert!(dist(a.pos(pet).unwrap(), (cx + 4.0, cy)) <= PET_HEEL + 0.01, "at heel");
        assert_eq!(a.pets_of(7)[0].get("bond"), Some(&json!(1)), "the owner reads the bond");
        let public = a.state().unwrap();
        let shown = public.iter().find(|v| v.get("id") == Some(&json!(pet))).unwrap();
        assert!(!shown.contains_key("bond"), "nobody else does");
        for i in 0..PETS_PER_PLAYER - 1 {
            a.adopt(7, "dog", &format!("Rex{i}")).unwrap();
        }
        let Error(why) = a.adopt(7, "dog", "One too many").unwrap_err();
        assert_eq!(error_code(&why).map(|c| c.0), Some(codes::LIMIT));
        a.pet_command(7, "release", pet, None).unwrap();
        assert_eq!(a.pets_of(7).len(), PETS_PER_PLAYER - 1);
    }

    #[test]
    fn changes_are_deltas_on_the_area_topic_and_the_snapshot_holds_every_container() {
        let mut hub = TestHub::<AreaHub>::spawn("npcs", "0,0").unwrap();
        hub.answer_node("world.actors_radius", |_| Ok(json!({ "actors": [] })));
        hub.advance(TICK_MS * SCAN_EVERY).unwrap();
        let deltas: Vec<Message<Published>> = hub.published().iter().filter(|(t, _)| t == TOPIC).map(|(_, b)| decode(b).unwrap()).collect();
        assert_eq!(deltas.len(), 8, "every container's first view");
        assert!(deltas.iter().all(|m| m.header.kind == MessageKind::Delta));
        assert!(deltas.iter().all(|m| m.body.props.as_ref().is_some_and(|p| !p.contains_key("home_x"))));
        let snap: Message<Containers> = hub.call_typed(Caller::Player(1), "snapshot", &json!({ "topic": TOPIC })).unwrap();
        assert_eq!((snap.body.containers.len(), snap.header.seq), (8, 8));
    }

    #[test]
    fn actors_are_shown_on_the_ground_with_z_up() {
        let a = area();
        let mob = first_mob(&a);
        let (x, y) = a.pos(mob).unwrap();
        let states = a.actor_states().unwrap();
        let (_, state) = states.iter().find(|(_, s)| s.kind == "mob" && s.id == mob).unwrap();
        assert_eq!(state.z, 0.0);
        assert_eq!(state.y, a.model.get_f64(&a.schema, mob, "y").unwrap());
        assert_eq!(state.chunk(), ChunkPos::new(x.floor() as i64, y.floor() as i64, 0));
    }

    #[test]
    fn an_actor_id_names_its_area_key_and_fits_at_the_key_limit() {
        let a = area();
        let mob = first_mob(&a);
        let states = a.actor_states().unwrap();
        let (actor, _) = states.iter().find(|(_, s)| s.kind == "mob" && s.id == mob).unwrap();
        assert_eq!(*actor, actor_id(&format!("M10,-4@{mob}")));
        let far = Area::new("-4294967296,-4294967296").unwrap();
        assert_eq!(far.key(), "-4294967296,-4294967296");
        let text = format!("N{}@{}", far.key(), 9_999_999);
        assert!(text.len() <= 32, "{text}");
    }

    #[test]
    fn a_scan_covers_the_ground_y_within_four_and_a_height_within_one() {
        let mut hub = TestHub::<AreaHub>::spawn("npcs", "5,7").unwrap();
        assert_eq!(hub.hub().area.center, (22, 30));
        hub.answer_node("world.actors_radius", |body| {
            let y: i64 = body["y"].as_str().unwrap().parse().unwrap();
            let actors = if y == 30 + 3 {
                vec![
                    json!({ "uuid": "p7", "userId": "7", "chunkX": "22", "chunkY": y.to_string(), "chunkZ": "0" }),
                    json!({ "uuid": "p8", "userId": "8", "chunkX": "22", "chunkY": y.to_string(), "chunkZ": "2" }),
                ]
            } else {
                vec![]
            };
            Ok(json!({ "actors": actors }))
        });
        hub.advance(TICK_MS).unwrap();
        assert_eq!(hub.hub().area.players.get(&7), Some(&(22.5, 33.5)));
        assert_eq!(hub.hub().area.players.get(&8), None, "too high to be in the area");
    }

    #[test]
    fn replies_carry_each_members_type_and_id() {
        let a = area();
        let views = a.state().unwrap();
        assert!(views.iter().any(|v| v.get("type_name") == Some(&json!("Npc"))));
        assert!(views.iter().any(|v| v.get("type_name") == Some(&json!("Mob"))));
        assert!(views.iter().all(|v| v.contains_key("id")));
    }

    #[test]
    fn a_snapshot_from_before_the_kit_protocol_loads() {
        let old = encode(&area()).unwrap();
        let hub = TestHub::<AreaHub>::load("npcs", "10,-4", &old, 1).unwrap();
        assert_eq!(hub.hub().area.model.of_type("Mob").count(), MOBS as usize);
    }

    #[test]
    fn a_snapshot_from_the_y_up_starter_starts_again() {
        let mut a = area();
        let mob = first_mob(&a);
        a.model.set(&a.schema, mob, "x", 99.0).unwrap();
        a.z_up = false;
        let old = encode(&a).unwrap();
        let hub = TestHub::<AreaHub>::load("npcs", "10,-4", &old, 1).unwrap();
        let a = &hub.hub().area;
        assert!(a.z_up);
        assert!((a.pos(first_mob(a)).unwrap().0 - 42.5).abs() < 4.0, "back near the centre layout");
    }
}
