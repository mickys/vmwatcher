// Native driver for the ESP32-S2 "Mordin KVM" USB HID dongle.
//
// The dongle is a USB keyboard and relative mouse on the controlled computer
// and takes line commands over TCP after an HMAC challenge:
//   CHALLENGE <32 hex>       ->  auth <hex HMAC-SHA256(ASCII hex key, challenge)>
//   down <usage> / up <usage>    keyboard usage (decimal), "OK" or "ERR"
//   mouse dx dy wheel pan buttons    relative report, each move within +-127
// It serves one client at a time, releases every held input on disconnect or
// after three seconds without a report, and drops idle sessions after 60 s.
// A persistent session is therefore required to hold keys between batches.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <set>
#include <thread>

#include "vmw.hpp"

namespace vmw {

int hidUsageOfQcode(const std::string& q) {
  if (q.size() == 1 && q[0] >= 'a' && q[0] <= 'z') return 4 + (q[0] - 'a');
  if (q.size() == 1 && q[0] >= '1' && q[0] <= '9') return 30 + (q[0] - '1');
  if (q == "0") return 39;
  if (q.size() >= 2 && q.size() <= 3 && q[0] == 'f' && isdigit((unsigned char)q[1])) {
    int n = atoi(q.c_str() + 1);
    if (n >= 1 && n <= 12) return 57 + n;
    if (n >= 13 && n <= 24) return 91 + n;
  }
  if (q.size() == 4 && q.compare(0, 3, "kp_") == 0 && q[3] >= '1' && q[3] <= '9') return 89 + (q[3] - '1');
  static const std::map<std::string, int> named = {
      {"ret", 40},          {"esc", 41},          {"backspace", 42},     {"tab", 43},          {"spc", 44},
      {"minus", 45},        {"equal", 46},        {"bracket_left", 47},  {"bracket_right", 48}, {"backslash", 49},
      {"semicolon", 51},    {"apostrophe", 52},   {"grave_accent", 53},  {"comma", 54},        {"dot", 55},
      {"slash", 56},        {"caps_lock", 57},    {"print", 70},         {"sysrq", 70},        {"scroll_lock", 71},
      {"pause", 72},        {"insert", 73},       {"home", 74},          {"pgup", 75},         {"delete", 76},
      {"end", 77},          {"pgdn", 78},         {"right", 79},         {"left", 80},         {"down", 81},
      {"up", 82},           {"num_lock", 83},     {"kp_divide", 84},     {"kp_multiply", 85},  {"kp_subtract", 86},
      {"kp_add", 87},       {"kp_enter", 88},     {"kp_0", 98},          {"kp_decimal", 99},   {"less", 100},
      {"menu", 101},        {"compose", 101},     {"ctrl", 224},         {"shift", 225},       {"alt", 226},
      {"meta_l", 227},      {"ctrl_r", 228},      {"shift_r", 229},      {"alt_r", 230},       {"meta_r", 231},
  };
  auto found = named.find(q);
  return found == named.end() ? 0 : found->second;
}

namespace {

using Clock = std::chrono::steady_clock;

class S2Kvm : public HidDevice {
 public:
  explicit S2Kvm(const Json& config) {
    address_ = config["address"].str();
    port_ = int(config["port"].integer(8889));
    keyFile_ = config["keyFile"].str();
    if (address_.empty() || address_.find_first_of(" /@") != std::string::npos) throw std::runtime_error("hid.address is required");
    if (port_ < 1 || port_ > 65535) throw std::runtime_error("hid.port is invalid");
    if (keyFile_.empty() || keyFile_[0] != '/') throw std::runtime_error("hid.keyFile must be an absolute path");
    const Json& screen = config["screen"];
    if (screen.isArray() && screen.arr().size() == 2) {
      width_ = int(screen.arr()[0].integer(0));
      height_ = int(screen.arr()[1].integer(0));
    }
    if (width_ < 64 || height_ < 64 || width_ > 16384 || height_ > 16384) throw std::runtime_error("hid.screen must be [width, height]");
    gain_ = config["mickeysPerPixel"].num(1.0);
    if (!(gain_ > 0.05 && gain_ < 20)) throw std::runtime_error("hid.mickeysPerPixel is out of range");
    rehomeOnClick_ = config["rehomeOnClick"].boolean(true);
    worker_ = std::thread([this] { maintain(); });
  }

  ~S2Kvm() override {
    {
      std::lock_guard<std::timed_mutex> lock(mutex_);
      stopping_ = true;
      drop();
    }
    wake_.notify_all();
    worker_.join();
  }

  void apply(const Json& events) override {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(2))) fail("Keyboard/mouse dongle busy", 503);
    lastInput_ = Clock::now();
    std::vector<std::string> lines;
    // Pointer moves are collected and sent as one move before the next button
    // or key, so a coalesced x/y pair becomes a single diagonal motion.
    int targetX = -1, targetY = -1;
    auto flushMove = [&](bool forceHome) {
      if (targetX < 0 && targetY < 0 && !forceHome) return;
      if (!homed_ || forceHome) home(lines);
      int tx = targetX < 0 ? x_ : targetX, ty = targetY < 0 ? y_ : targetY;
      moveTo(tx, ty, lines);
      targetX = targetY = -1;
    };
    for (const Json& event : events.arr()) {
      const std::string& type = event["type"].str();
      const Json& data = event["data"];
      if (type == "abs") {
        int v = int(data["value"].integer());
        bool horizontal = data["axis"].str() == "x";
        int span = (horizontal ? width_ : height_) - 1;
        int pixel = int(std::lround(double(v) / 32767.0 * span));
        (horizontal ? targetX : targetY) = pixel;
      } else if (type == "btn") {
        const std::string& name = data["button"].str();
        bool down = data["down"].boolean();
        if (name == "wheel-up" || name == "wheel-down") {
          flushMove(false);
          if (down) lines.push_back(mouse(0, 0, name == "wheel-up" ? 1 : -1));
          continue;
        }
        int bit = name == "left" ? 1 : name == "right" ? 2 : name == "middle" ? 4 : 0;
        if (!bit) continue;
        // Relative pointers drift with the target's pointer acceleration; a
        // fresh press starts from a known corner so the click lands where shown.
        flushMove(down && rehomeOnClick_ && buttons_ == 0);
        buttons_ = down ? (buttons_ | bit) : (buttons_ & ~bit);
        lines.push_back(mouse(0, 0, 0));
      } else if (type == "key") {
        flushMove(false);
        int usage = hidUsageOfQcode(data["key"]["data"].str());
        if (!usage) continue;
        bool down = data["down"].boolean();
        if (down) keys_.insert(usage);
        else keys_.erase(usage);
        lines.push_back((down ? "down " : "up ") + std::to_string(usage));
      }
    }
    flushMove(false);
    if (lines.empty()) return;
    try {
      send(lines);
    } catch (const HttpError&) {
      throw;
    } catch (const std::exception& e) {
      // The dongle released everything when the session broke. Start over
      // from a clean state; the viewer re-sends held keys on its next press.
      drop();
      fail(std::string("Keyboard/mouse dongle: ") + e.what(), 502);
    }
  }

 private:
  std::string mouse(int dx, int dy, int wheel) const {
    return "mouse " + std::to_string(dx) + " " + std::to_string(dy) + " " + std::to_string(wheel) + " 0 " + std::to_string(buttons_);
  }

  // Pins the pointer to the top-left corner: overshooting is clamped by the OS.
  void home(std::vector<std::string>& lines) {
    int steps = int(std::ceil(std::max(width_, height_) * gain_ / 127.0)) + 4;
    for (int i = 0; i < steps; i++) lines.push_back(mouse(-127, -127, 0));
    x_ = y_ = 0;
    homed_ = true;
    residualX_ = residualY_ = 0;
  }

  void moveTo(int tx, int ty, std::vector<std::string>& lines) {
    double dx = (tx - x_) * gain_ + residualX_, dy = (ty - y_) * gain_ + residualY_;
    long mx = std::lround(dx), my = std::lround(dy);
    residualX_ = dx - double(mx);
    residualY_ = dy - double(my);
    x_ = tx;
    y_ = ty;
    // Split into steps of at most 127 that keep the direction of travel.
    while (mx || my) {
      long n = std::max<long>(1, (std::max(std::labs(mx), std::labs(my)) + 126) / 127);
      int sx = int(mx / n), sy = int(my / n);
      lines.push_back(mouse(sx, sy, 0));
      mx -= sx;
      my -= sy;
    }
  }

  // Call with mutex_ held.
  void connect() {
    std::string key = trim(readFile(keyFile_, 4096));
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos)
      fail("The dongle key file must hold 64 lowercase hex characters", 503);
    auto conn = connectTcp(address_, port_, 3);
    conn->timeouts(3, 3);
    std::string line;
    if (!conn->readUntil("\n", 256, line)) throw std::runtime_error("closed before the challenge");
    line = trim(line);
    if (line.size() != 42 || line.compare(0, 10, "CHALLENGE ") != 0) throw std::runtime_error("unexpected greeting");
    conn->write("auth " + hex(hmacSha256(key, line.substr(10))) + "\n");
    if (!conn->readUntil("\n", 256, line) || trim(line) != "OK") fail("The keyboard/mouse dongle rejected the key", 502);
    conn_ = std::move(conn);
    homed_ = false;  // the pointer may have moved while nobody was connected
    lastSend_ = Clock::now();
  }

  void drop() {
    conn_.reset();
    homed_ = false;
    buttons_ = 0;
    keys_.clear();
  }

  // Pipelines the lines and checks one reply per line. Call with mutex_ held.
  void send(const std::vector<std::string>& lines) {
    if (!conn_) connect();
    for (size_t at = 0; at < lines.size(); at += 16) {
      size_t end = std::min(lines.size(), at + 16);  // stays under the dongle's read chunk
      std::string batch;
      for (size_t i = at; i < end; i++) batch += lines[i] + "\n";
      conn_->write(batch);
      for (size_t i = at; i < end; i++) {
        std::string reply;
        if (!conn_->readUntil("\n", 256, reply)) throw std::runtime_error("connection closed");
        if (trim(reply).compare(0, 2, "OK") != 0) fail("The keyboard/mouse dongle refused: " + lines[i], 502);
      }
    }
    lastSend_ = Clock::now();
  }

  // Keeps held keys and buttons alive past the dongle's three-second release,
  // gives up on them when the viewer has been silent for 30 s, and closes an
  // idle session before the dongle does.
  void maintain() {
    std::unique_lock<std::timed_mutex> lock(mutex_);
    while (!stopping_) {
      cv_wait(lock, std::chrono::milliseconds(500));
      if (stopping_ || !conn_) continue;
      auto now = Clock::now();
      bool held = buttons_ || !keys_.empty();
      try {
        if (held && now - lastInput_ > std::chrono::seconds(30)) {
          send({"release"});
          buttons_ = 0;
          keys_.clear();
        } else if (held && now - lastSend_ > std::chrono::milliseconds(1500)) {
          send({mouse(0, 0, 0)});
        } else if (!held && now - lastSend_ > std::chrono::seconds(45)) {
          drop();
        }
      } catch (const std::exception&) {
        drop();
      }
    }
  }

  template <class Duration>
  void cv_wait(std::unique_lock<std::timed_mutex>& lock, Duration duration) {
    lock.unlock();
    {
      std::unique_lock<std::mutex> wait(waitMutex_);
      wake_.wait_for(wait, duration);
    }
    lock.lock();
  }

  std::string address_, keyFile_;
  int port_ = 8889, width_ = 1920, height_ = 1080;
  double gain_ = 1.0, residualX_ = 0, residualY_ = 0;
  bool rehomeOnClick_ = true;
  std::timed_mutex mutex_;
  std::mutex waitMutex_;
  std::condition_variable wake_;
  std::unique_ptr<Conn> conn_;
  bool homed_ = false, stopping_ = false;
  int x_ = 0, y_ = 0, buttons_ = 0;
  std::set<int> keys_;
  Clock::time_point lastSend_ = Clock::now(), lastInput_ = Clock::now();
  std::thread worker_;
};

}  // namespace

std::shared_ptr<HidDevice> makeHidDevice(const Json& config) {
  const std::string& type = config["type"].str();
  if (type == "esp32-s2-kvm") return std::make_shared<S2Kvm>(config);
  throw std::runtime_error("Unknown hid.type \"" + type + "\" (supported: esp32-s2-kvm)");
}

}  // namespace vmw
