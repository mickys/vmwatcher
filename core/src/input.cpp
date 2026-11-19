#include <cmath>
#include <set>

#include "vmw.hpp"

namespace vmw {
namespace {

Json absEvent(const std::string& axis, int64_t value) {
  Json data = Json::Object{};
  data.set("axis", axis);
  data.set("value", value);
  Json event = Json::Object{};
  event.set("type", "abs");
  event.set("data", data);
  return event;
}
bool qcodeOk(const std::string& s) {
  if (s.empty() || s.size() > 24) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}

}  // namespace

// Rebuilds every event from validated fields, so nothing the browser sent
// reaches QEMU verbatim.
Json inputEvents(const Json& events) {
  if (!events.isArray() || events.arr().empty() || events.arr().size() > 64) fail("Invalid input events");
  static const std::set<std::string> buttons = {"left", "middle", "right", "wheel-up", "wheel-down"};
  Json out = Json::Array{};
  for (const Json& e : events.arr()) {
    const Json& d = e["data"];
    const std::string& type = e["type"].str();
    if (type == "abs" && (d["axis"].str() == "x" || d["axis"].str() == "y") && d["value"].isInt() &&
        d["value"].integer() >= 0 && d["value"].integer() <= 32767) {
      out.push(absEvent(d["axis"].str(), d["value"].integer()));
    } else if (type == "btn" && buttons.count(d["button"].str()) && d["down"].isBool()) {
      Json data = Json::Object{};
      data.set("button", d["button"].str());
      data.set("down", d["down"].boolean());
      Json event = Json::Object{};
      event.set("type", "btn");
      event.set("data", data);
      out.push(event);
    } else if (type == "key" && d["down"].isBool() && d["key"]["type"].str() == "qcode" && qcodeOk(d["key"]["data"].str())) {
      Json key = Json::Object{};
      key.set("type", "qcode");
      key.set("data", d["key"]["data"].str());
      Json data = Json::Object{};
      data.set("down", d["down"].boolean());
      data.set("key", key);
      Json event = Json::Object{};
      event.set("type", "key");
      event.set("data", data);
      out.push(event);
    } else
      fail("Invalid input event");
  }
  return out;
}

// Absolute USB tablets address the whole guest desktop. Monitor-local input
// must include the offset and size of its display in the side-by-side layout.
Json desktopInputEvents(int head, const Json& events, const std::vector<Display>& displays) {
  Json safe = inputEvents(events);
  if (displays.empty() || displays.size() > 4 || head < 0 || size_t(head) >= displays.size()) fail("Invalid monitor layout");
  int64_t width = 0, height = 0, left = 0;
  for (size_t i = 0; i < displays.size(); i++) {
    const Display& d = displays[i];
    if (d.width < 1 || d.height < 1 || d.width > 16384 || d.height > 16384) fail("Invalid monitor dimensions");
    if (int(i) < head) left += d.width;
    width += d.width;
    height = std::max<int64_t>(height, d.height);
  }
  const Display& display = displays[size_t(head)];
  Json out = Json::Array{};
  for (const Json& event : safe.arr()) {
    if (event["type"].str() != "abs") {
      out.push(event);
      continue;
    }
    bool horizontal = event["data"]["axis"].str() == "x";
    double pixel = (horizontal ? double(left) : 0.0) +
                   (double(event["data"]["value"].integer()) / 32767.0) * double((horizontal ? display.width : display.height) - 1);
    double span = double(std::max<int64_t>(1, (horizontal ? width : height) - 1));
    out.push(absEvent(event["data"]["axis"].str(), int64_t(std::llround(pixel / span * 32767.0))));
  }
  return out;
}

// USB HID devices sample state on a poll. A press and release in one QMP
// transaction can disappear entirely; keep repeated edges in separate reports.
std::vector<Json> inputFrames(const Json& events) {
  std::vector<Json> frames;
  Json frame = Json::Array{};
  std::set<std::string> changed;
  for (const Json& event : events.arr()) {
    std::string key;
    if (event["type"].str() == "key") key = "key:" + event["data"]["key"]["data"].str();
    else if (event["type"].str() == "btn") key = "btn:" + event["data"]["button"].str();
    if (!key.empty() && changed.count(key)) {
      frames.push_back(frame);
      frame = Json::Array{};
      changed.clear();
    }
    frame.push(event);
    if (!key.empty()) changed.insert(key);
  }
  if (!frame.arr().empty()) frames.push_back(frame);
  return frames;
}

}  // namespace vmw
