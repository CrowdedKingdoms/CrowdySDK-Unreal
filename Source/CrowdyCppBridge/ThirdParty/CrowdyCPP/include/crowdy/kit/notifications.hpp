#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "crowdy/kit/core.hpp"

namespace crowdy::kit {


/// Builders for a model function's `notifications`, so the non-staleable shape is the easy
/// one to reach for.
///
/// A channel notification can name its target two ways, and only one survives the app being
/// recreated or moved between organizations. Membership is scoped to the app, so a function
/// naming a channel belonging to a DIFFERENT app produces a notification that is built,
/// sent to every server and then dropped for want of a recipient -- while the invoke
/// succeeds and the run records success, because emission is best-effort and never fails a
/// function. The only symptom is silence. The server reports it as the
/// `notification_channel_foreign` lint error.
///
/// A NAME is resolved per-invocation against the app that is running, so it cannot go stale
/// the way a literal id copied between apps does.
namespace notifications {

/// Notify every client in the app's default session channel.
///
/// `$session_channel_name` is injected by the server from the app the function is RUNNING
/// in, so this is correct in any app that ever holds the definition. It is also the channel
/// every SDK client joins on connect.
inline JVal sessionChannel(std::string_view payloadExpression) {
  JVal name;
  name["name"] = "channel_name";
  name["expression"] = "$session_channel_name";
  JVal payload;
  payload["name"] = "payload";
  payload["expression"] = std::string(payloadExpression);
  JVal n;
  n["kind"] = "channel";
  n["args"] = JVal::array({std::move(name), std::move(payload)});
  return n;
}

/// Notify a channel named by NAME rather than id -- a lobby per app, say, as
/// `concat("lobby-", $app_id)`. Same property: resolved against the running app.
inline JVal namedChannel(std::string_view channelNameExpression,
                         std::string_view payloadExpression) {
  JVal name;
  name["name"] = "channel_name";
  name["expression"] = std::string(channelNameExpression);
  JVal payload;
  payload["name"] = "payload";
  payload["expression"] = std::string(payloadExpression);
  JVal n;
  n["kind"] = "channel";
  n["args"] = JVal::array({std::move(name), std::move(payload)});
  return n;
}

/// Notify a channel named by id. Correct when the id is READ FROM MODEL STATE --
/// `self.channel_id` on a container owning its own channel travels with the row. A LITERAL
/// id is the shape to avoid.
inline JVal channelId(std::string_view channelIdExpression,
                      std::string_view payloadExpression) {
  JVal id;
  id["name"] = "channel_id";
  id["expression"] = std::string(channelIdExpression);
  JVal payload;
  payload["name"] = "payload";
  payload["expression"] = std::string(payloadExpression);
  JVal n;
  n["kind"] = "channel";
  n["args"] = JVal::array({std::move(id), std::move(payload)});
  return n;
}

}  // namespace notifications

}  // namespace crowdy::kit
