#pragma once

#include <stdexcept>
#include <vector>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Decks — cards & hidden information: an admin CardDef catalog and
/// session-scoped CardInstances whose card_id carries visibility "owner" —
/// the two-property hidden-info trick: only the owner's reads include
/// card_id, while the public revealed_card_id stays empty until the card is
/// played/discarded (the play function copies it in the same transaction).
/// Opponents can see a card EXISTS in your hand, not what it is — enforced
/// by read filtering, not client discipline.
///
/// Shuffling is honest about the platform: expressions can't index arrays,
/// so decks are ordered by a position int dealt by assign_position
/// (rand_int(0, 1000000) per card) via a manual, type-fan-out automation —
/// DecksKit::shuffle() runs it (admin), and drawing takes the owner's
/// lowest-position deck card ("top of deck"; the runtime picks it, the
/// server enforces the zone transition and ownership). Mirrors CrowdyJS's
/// decksBlueprint / kit.decks.
namespace crowdy::kit {

struct DecksBlueprintOptions {
  /// Prefix for the type/function names. Defaults to none.
  std::string typePrefix;
  /// Turn-based mode: adds is_current_turn to draw/play/discard, so only the
  /// player whose session turn it is may act. Defaults to false.
  bool turnBased = false;
  /// Fan-out cap of the manual shuffle automation (raise it above your
  /// largest deck size). Defaults to 200.
  int shuffleMaxTargets = 200;
  /// Owner-mirror typing (see the kit convention). Defaults to Int.
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by decksBlueprint for a given prefix.
struct DecksNames {
  std::string cardDefType;
  std::string cardType;
  std::string drawFn;
  std::string playFn;
  std::string discardFn;
  std::string assignPositionFn;
  std::string shuffleAutomation;
};

/// Compute the type/function names a decks blueprint (and its runtime
/// helper) uses.
inline DecksNames decksNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  std::string autoPrefix = fnPrefix;
  for (auto& c : autoPrefix) {
    if (c == '_') c = '-';
  }
  DecksNames n;
  n.cardDefType = std::string(typePrefix) + "CardDef";
  n.cardType = std::string(typePrefix) + "CardInstance";
  n.drawFn = fnPrefix + "draw_card";
  n.playFn = fnPrefix + "play_card";
  n.discardFn = fnPrefix + "discard_card";
  n.assignPositionFn = fnPrefix + "assign_position";
  n.shuffleAutomation = autoPrefix + "deck-shuffle";
  return n;
}

/// Build the decks blueprint. Runtime counterpart: DecksKit.
inline KitBlueprint decksBlueprint(const DecksBlueprintOptions& options = {}) {
  const DecksNames names = decksNames(options.typePrefix);
  const OwnerIdKind kind = options.ownerIdKind;

  auto playerPolicy = [&](std::string_view condition) {
    JArray rules;
    rules.push_back(ownerOfSelfPolicy());
    if (options.turnBased) rules.push_back(isCurrentTurnPolicy());
    rules.push_back(conditionPolicy(condition));
    return kitPolicyJson(andPolicy(std::move(rules)));
  };

  auto mutation = [](const char* target, const char* property, const char* expression) {
    JVal m;
    m["target"] = target;
    m["property"] = property;
    m["expression"] = expression;
    return m;
  };

  KitBlueprint bp;
  bp.name = names.cardType;

  {
    JVal t;
    t["typeName"] = names.cardDefType;
    t["displayName"] = names.cardDefType;
    t["instantiableBy"] = "admin";
    t["description"] = "Studio card catalog row.";
    bp.containerTypes.push_back(std::move(t));
  }
  {
    JVal t;
    t["typeName"] = names.cardType;
    t["displayName"] = names.cardType;
    t["instantiableBy"] = "member";
    t["description"] =
        "One card in play: hidden while in deck/hand (owner-visible card_id), public once "
        "revealed.";
    bp.containerTypes.push_back(std::move(t));
  }

  {
    JVal p;
    p["containerTypeName"] = names.cardDefType;
    p["key"] = "card_id";
    p["valueType"] = "string";
    p["description"] = "Stable card identifier (rules data lives on the def).";
    bp.propertyDefinitions.push_back(std::move(p));
  }
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.cardType, kind));
  {
    JVal p;
    p["containerTypeName"] = names.cardType;
    p["key"] = "card_id";
    p["valueType"] = "string";
    p["defaultValueJson"] = "\"\"";
    p["visibility"] = "owner";
    p["description"] =
        "WHICH card this is — owner-visible only (the hidden-information half of the "
        "two-property trick); opponents' reads omit it.";
    bp.propertyDefinitions.push_back(std::move(p));
  }
  {
    JVal p;
    p["containerTypeName"] = names.cardType;
    p["key"] = "revealed_card_id";
    p["valueType"] = "string";
    p["defaultValueJson"] = "\"\"";
    p["description"] =
        "The public half: empty while the card hides in deck/hand, copied from card_id when "
        "played or discarded.";
    bp.propertyDefinitions.push_back(std::move(p));
  }
  {
    JVal p;
    p["containerTypeName"] = names.cardType;
    p["key"] = "zone";
    p["valueType"] = "string";
    p["defaultValueJson"] = "\"deck\"";
    p["description"] = "Card zone: 'deck' | 'hand' | 'board' | 'discard'.";
    bp.propertyDefinitions.push_back(std::move(p));
  }
  {
    JVal p;
    p["containerTypeName"] = names.cardType;
    p["key"] = "position";
    p["valueType"] = "int";
    p["defaultValueJson"] = "0";
    p["description"] =
        "Deck order (lowest = top), dealt by the shuffle automation via rand_int.";
    bp.propertyDefinitions.push_back(std::move(p));
  }

  {
    JVal fn;
    fn["name"] = names.drawFn;
    fn["containerTypeName"] = names.cardType;
    fn["returnType"] = "string";
    fn["mutations"] = JVal::array({mutation("self", "zone", "\"hand\"")});
    fn["returnExpression"] = "self.zone";
    fn["invokePolicyJson"] = playerPolicy("self.zone == \"deck\"");
    fn["description"] =
        "Draw a card you own from your deck into your hand (the runtime picks the "
        "lowest-position deck card — \"top of deck\"); card_id stays owner-visible.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.playFn;
    fn["containerTypeName"] = names.cardType;
    fn["returnType"] = "string";
    fn["mutations"] =
        JVal::array({mutation("self", "zone", "\"board\""),
                     mutation("self", "revealed_card_id", "self.card_id")});
    fn["returnExpression"] = "self.revealed_card_id";
    fn["invokePolicyJson"] = playerPolicy("self.zone == \"hand\"");
    fn["description"] =
        "Play a card from your hand: the zone flip AND the public reveal (revealed_card_id = "
        "card_id) commit together.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.discardFn;
    fn["containerTypeName"] = names.cardType;
    fn["returnType"] = "string";
    fn["mutations"] =
        JVal::array({mutation("self", "zone", "\"discard\""),
                     mutation("self", "revealed_card_id", "self.card_id")});
    fn["returnExpression"] = "self.revealed_card_id";
    fn["invokePolicyJson"] = playerPolicy("self.zone == \"hand\" || self.zone == \"board\"");
    fn["description"] = "Discard a card face-up (discard piles are public information).";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.assignPositionFn;
    fn["containerTypeName"] = names.cardType;
    fn["returnType"] = "int";
    fn["mutations"] =
        JVal::array({mutation("self", "position", "rand_int(0, 1000000)")});
    fn["returnExpression"] = "self.position";
    fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
    fn["autonomousInvocable"] = true;
    fn["description"] =
        "Shuffle step (automation-only): deals this card a random deck position — the supported "
        "server-side shuffle pattern (expressions cannot permute arrays).";
    bp.functions.push_back(std::move(fn));
  }

  {
    JVal w;
    w["key"] = "zone";
    w["op"] = "==";
    w["value"] = "deck";
    JVal selector;
    selector["selfWhere"] = JVal::array({std::move(w)});
    JVal a;
    a["name"] = names.shuffleAutomation;
    a["functionName"] = names.assignPositionFn;
    a["targetMode"] = "type";
    a["targetTypeName"] = names.cardType;
    a["triggerType"] = "manual";
    a["maxTargets"] = options.shuffleMaxTargets;
    a["selectorJson"] = selector.dump();
    a["description"] =
        "Manual shuffle: fans assign_position out over every deck-zone card (run via "
        "gameModelRunAutomation / kit.decks.shuffle).";
    bp.automations.push_back(std::move(a));
  }

  return bp;
}

/// A parsed view of one card instance, as visible to the CALLER.
struct KitCard {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;  ///< empty when unowned
  /// The hidden identity — non-empty only when the caller may see it (their
  /// own cards, or any revealed card via revealedCardId).
  std::string cardId;
  /// The public identity (empty until played/discarded).
  std::string revealedCardId;
  std::string zone;
  std::int64_t position = 0;
};

/// Runtime helpers for the decksBlueprint conventions: deal session-scoped
/// cards, shuffle by dealing random positions (admin-run automation), and
/// draw/play/discard through the zone-guarded functions. Hidden information
/// is enforced by property visibility server-side: reads go through
/// containerState, which strips other players' card_id.
class DecksKit {
 public:
  DecksKit(std::string appId, domains::GameModelAPI& gameModel,
           std::string_view typePrefix = {}, EngineDetector* engines = nullptr,
           std::string_view engineModuleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        names_(decksNames(typePrefix)),
        engines_(engines),
        engineModuleName_(engineModuleName.empty() ? std::string("deck-engine")
                                                   : std::string(engineModuleName)) {}

  const DecksNames& names() const { return names_; }

  // -- Engine path (Wave 2): true hidden information over the deck engine --

  /// Is a deck compute engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_ != nullptr && engines_->has(engineModuleName_); }

  /// Create an engine table with seeded shuffle + hidden deals.
  Json engineNewTable(std::string_view tableId, const std::vector<std::string>& players,
                      std::int64_t handSize = 5, std::string_view deckDef = {}) {
    JVal params;
    params["tableId"] = tableId;
    JArray list;
    for (const auto& player : players) list.push_back(JVal(player));
    params["players"] = JVal(std::move(list));
    params["handSize"] = handSize;
    if (!deckDef.empty()) params["deckDef"] = deckDef;
    return engineInvoke("new_table", params);
  }

  /// YOUR hidden hand (the only read path; opponents can never see it).
  std::vector<std::string> engineHand(std::string_view tableId) {
    JVal params;
    params["tableId"] = tableId;
    Json body = engineInvoke("hand", params);
    std::vector<std::string> hand;
    body["hand"].forEach([&](Json card) { hand.push_back(card.asString()); });
    return hand;
  }

  /// Draw one card into your hidden hand.
  Json engineDraw(std::string_view tableId) {
    JVal params;
    params["tableId"] = tableId;
    return engineInvoke("draw", params);
  }

  /// Play (reveal) a card from your hand into a public zone.
  Json enginePlay(std::string_view tableId, std::string_view card, std::string_view zone = "table") {
    JVal params;
    params["tableId"] = tableId;
    params["card"] = card;
    params["zone"] = zone;
    return engineInvoke("play", params);
  }

  /// Collect a public zone to the discard (trick taken).
  Json engineTakeZone(std::string_view tableId, std::string_view zone) {
    JVal params;
    params["tableId"] = tableId;
    params["zone"] = zone;
    return engineInvoke("take_zone", params);
  }

  /// The public table view (counts + zones — never hidden hands).
  Json engineTable(std::string_view tableId) {
    JVal params;
    params["tableId"] = tableId;
    return engineInvoke("table", params);
  }

  /// Deal a deck to a player: creates one CardInstance per card id (zone
  /// "deck"), owned by the player. Run shuffle() afterwards to deal random
  /// positions.
  std::vector<Json> deal(std::string_view ownerUserId, const std::vector<std::string>& cardIds,
                         std::string_view sessionId = {}) {
    std::vector<Json> created;
    created.reserve(cardIds.size());
    for (std::size_t index = 0; index < cardIds.size(); ++index) {
      JVal input;
      input["appId"] = appId_;
      input["typeName"] = names_.cardType;
      input["displayName"] = "Card " + std::to_string(index + 1);
      if (!sessionId.empty()) input["sessionId"] = sessionId;
      JArray properties;
      properties.push_back(property("owner_user_id", "int", std::string(ownerUserId)));
      properties.push_back(property("card_id", "string", JVal(cardIds[index]).dump()));
      properties.push_back(property("position", "int", std::to_string(index)));
      input["properties"] = JVal(std::move(properties));
      created.push_back(gameModel_.createContainer(input));
    }
    return created;
  }

  /// Shuffle every deck-zone card by running the manual assign_position
  /// automation (admin): each card gets a fresh rand_int position
  /// server-side.
  Json shuffle() { return gameModel_.runAutomation(appId_, names_.shuffleAutomation); }

  /// A player's cards (all players when ownerUserId is empty), optionally
  /// filtered to one zone, as visible to the caller.
  std::vector<KitCard> cards(std::string_view ownerUserId, std::string_view zone = {},
                             std::string_view sessionId = {}) {
    Json containers = gameModel_.containers(appId_, names_.cardType, sessionId);
    std::vector<KitCard> out;
    containers.forEach([&](Json c) {
      if (!ownerUserId.empty() && c["ownerUserId"].asString() != ownerUserId) return;
      KitCard card = toCard(c["containerId"].asStringView(), c["displayName"].asString(),
                            c["ownerUserId"].asString());
      if (zone.empty() || card.zone == zone) out.push_back(std::move(card));
    });
    return out;
  }

  /// Your hand (owner-visible card ids included by the server).
  std::vector<KitCard> myHand(std::string_view ownerUserId, std::string_view sessionId = {}) {
    return cards(ownerUserId, "hand", sessionId);
  }

  /// Every card on the board (public: revealed ids).
  std::vector<KitCard> board(std::string_view sessionId = {}) {
    return cards({}, "board", sessionId);
  }

  /// Draw the top of your deck (lowest position — positions were dealt by
  /// the shuffle automation). The server enforces ownership, turn (when
  /// turnBased), and the deck→hand zone transition. Throws
  /// std::runtime_error when the deck is empty.
  KitInvokeResult draw(std::string_view ownerUserId, std::string_view sessionId = {}) {
    std::vector<KitCard> deck = cards(ownerUserId, "deck", sessionId);
    if (deck.empty()) {
      throw std::runtime_error("Deck is empty — nothing to draw");
    }
    const KitCard* top = &deck.front();
    for (const KitCard& card : deck) {
      if (card.position < top->position) top = &card;
    }
    return drawCard(top->containerId, sessionId);
  }

  /// Draw one specific deck card (prefer draw() for top-of-deck).
  KitInvokeResult drawCard(std::string_view cardInstanceId, std::string_view sessionId = {}) {
    return kitInvoke(gameModel_, appId_, names_.drawFn, cardInstanceId, JVal(), sessionId);
  }

  /// Play a card from your hand: the zone flip and the public reveal commit
  /// together. Returns the revealed card id.
  KitInvokeResult play(std::string_view cardInstanceId, std::string_view sessionId = {}) {
    return kitInvoke(gameModel_, appId_, names_.playFn, cardInstanceId, JVal(), sessionId);
  }

  /// Discard a card face-up.
  KitInvokeResult discard(std::string_view cardInstanceId, std::string_view sessionId = {}) {
    return kitInvoke(gameModel_, appId_, names_.discardFn, cardInstanceId, JVal(), sessionId);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  KitCard toCard(std::string_view containerId, std::string displayName,
                 std::string ownerUserId) {
    // containerState filters properties by visibility: other players' hands
    // come back WITHOUT card_id — the hidden-info guarantee is server-side.
    Json props = kitContainerProperties(gameModel_, appId_, containerId);
    KitCard card;
    card.containerId = std::string(containerId);
    card.displayName = std::move(displayName);
    card.ownerUserId = std::move(ownerUserId);
    card.cardId = props["card_id"].asString();
    card.revealedCardId = props["revealed_card_id"].asString();
    card.zone = props["zone"].asString();
    card.position = props["position"].asInt64();
    return card;
  }

  Json engineInvoke(std::string_view exportName, const JVal& params) {
    if (engines_ == nullptr) {
      throw std::runtime_error("deck engine unavailable: compute domain not wired");
    }
    EngineInvokeResult result = engines_->invoke(engineModuleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("decks." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  DecksNames names_;
  EngineDetector* engines_ = nullptr;
  std::string engineModuleName_;
};

}  // namespace crowdy::kit
