#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>

#include "vmw.hpp"

namespace vmw {

Vm parseDomain(const std::string& xml, const std::string& state) {
  XmlNode domain = parseXml(xml);
  const XmlNode* uuid = domain.child("uuid");
  const XmlNode* name = domain.child("name");
  if (domain.name != "domain" || !uuid || !isUuid(uuid->text)) throw std::runtime_error("Invalid domain XML");
  Vm vm;
  vm.id = lower(uuid->text);
  vm.name = name ? name->text : vm.id;
  vm.state = state;
  int heads = 0;
  if (const XmlNode* devices = domain.child("devices")) {
    for (const XmlNode* video : devices->all("video")) {
      Video v;
      const XmlNode* model = video->child("model");
      const XmlNode* alias = video->child("alias");
      v.heads = std::max(1, atoi(model ? model->attr("heads", "1").c_str() : "1"));
      v.alias = alias ? alias->attr("name") : "";
      heads += v.heads;
      vm.videos.push_back(v);
    }
    for (const XmlNode* graphics : devices->all("graphics")) {
      if (graphics->attr("type") != "vnc") continue;
      vm.vncPort = atoi(graphics->attr("port", "0").c_str());
      vm.vncHost = graphics->attr("listen");
      if (vm.vncHost.empty())
        for (const XmlNode* listen : graphics->all("listen"))
          if (listen->attr("type") == "address" && vm.vncHost.empty()) vm.vncHost = listen->attr("address");
      if (vm.vncHost.empty()) vm.vncHost = "127.0.0.1";
      vm.vnc = vm.vncPort > 0;
      break;
    }
  }
  vm.heads = std::max(1, std::min(4, heads));
  return vm;
}

std::string remoteHostOfUri(const std::string& uri) {
  size_t scheme = uri.find("://");
  if (scheme == std::string::npos) return "";
  std::string authority = uri.substr(scheme + 3);
  authority = authority.substr(0, authority.find_first_of("/?"));
  size_t user = authority.rfind('@');
  if (user != std::string::npos) authority = authority.substr(user + 1);
  if (!authority.empty() && authority[0] == '[') return authority.substr(1, authority.find(']') - 1);
  return authority.substr(0, authority.find(':'));
}

std::string consoleTarget(const Vm& vm, const HostOptions& options, std::string* target) {
  if (!vm.vnc) return "This VM has no VNC console configured";
  if (vm.vncPort < 1024 || vm.vncPort > 65535) return "The VNC console uses an unsupported port";
  const std::string& host = vm.vncHost;
  bool loopback = host == "127.0.0.1" || host == "::1" || host == "localhost";
  bool wildcard = host == "0.0.0.0" || host == "::";
  std::string dial;
  if (options.local) {
    // On the hypervisor itself the proxy only ever dials its own addresses.
    if (loopback || wildcard) dial = host == "::1" ? "::1" : "127.0.0.1";
    else if (std::find(options.own.begin(), options.own.end(), host) != options.own.end()) dial = host;
    else return "The VNC console listens on an address that is not on this host, so it cannot be viewed here";
  } else {
    if (loopback)
      return "The console only listens on the hypervisor's loopback address. Install the VM Watcher agent on this host for live view and audio";
    dial = wildcard ? options.remoteHost : host;
    if (dial.empty()) return "The hypervisor address is unknown for this libvirt connection";
  }
  if (target) *target = dial;
  return "";
}

namespace {

bool activeState(const std::string& state) { return state == "running" || state == "paused" || state == "pmsuspended"; }

class LibvirtHost : public HostApi {
 public:
  LibvirtHost(std::shared_ptr<Hypervisor> hypervisor, HostOptions options)
      : hypervisor_(std::move(hypervisor)), options_(std::move(options)) {}

  Json info() override {
    Json capabilities = Json::Object{};
    capabilities.set("live", true);
    capabilities.set("audio", true);
    capabilities.set("input", true);
    Json out = Json::Object{};
    out.set("version", kVersion);
    out.set("mode", options_.local ? "local" : "libvirt");
    out.set("capabilities", capabilities);
    return out;
  }

  Json inventory() override {
    Json out = Json::Array{};
    std::vector<Vm> list = vms(false);
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (const Vm& vm : list) {
      Json item = Json::Object{};
      item.set("id", vm.id);
      item.set("name", vm.name);
      item.set("state", vm.state);
      item.set("heads", vm.heads);
      item.set("vnc", vm.vnc);
      std::string note = consoleTarget(vm, options_, nullptr);
      item.set("live", note.empty());
      item.set("liveNote", note);
      Json displays = Json::Array{};
      for (int head = 0; head < vm.heads; head++) {
        Json display = Json::Object{};
        display.set("head", head);
        auto known = heads_.find(vm.id + ":" + std::to_string(head));
        if (known != heads_.end()) {
          display.set("width", known->second.width);
          display.set("height", known->second.height);
          display.set("capturedAt", isoTime(known->second.time));
        }
        displays.push(display);
      }
      item.set("displays", displays);
      out.push(item);
    }
    return out;
  }

  std::string headPng(const std::string& id, int head, bool fresh) override { return frame(id, head, fresh)->png; }

  std::string previewPng(const std::string& id, int width) override {
    width = std::max(128, std::min(4096, width));
    std::string key = id + ":" + std::to_string(width);
    {
      std::lock_guard<std::mutex> lock(cacheMutex_);
      auto found = previews_.find(key);
      if (found != previews_.end() && nowMs() - found->second.first < 1000) return found->second.second;
    }
    Vm vm = get(id);
    std::vector<Image> images;
    std::string problem = "No display available";
    for (int head = 0; head < vm.heads; head++) {
      try {
        images.push_back(frame(id, head, false)->image);
      } catch (const std::exception& e) {
        if (head == 0) problem = e.what();
      }
    }
    if (images.empty()) fail(problem);
    std::string png = encodePng(resizeImage(combineImages(images), width));
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (previews_.size() > 128) previews_.clear();
    previews_[key] = {nowMs(), png};
    return png;
  }

  void input(const std::string& id, int head, const Json& raw) override {
    Json events = inputEvents(raw);
    Vm vm = get(id);
    if (vm.state != "running" || head < 0 || head >= vm.heads) fail("Monitor unavailable");
    bool absolute = false;
    for (const Json& event : events.arr()) absolute = absolute || event["type"].str() == "abs";
    if (absolute) {
      std::vector<Display> displays;
      for (int h = 0; h < vm.heads; h++) {
        Display d;
        bool known = false;
        {
          std::lock_guard<std::mutex> lock(cacheMutex_);
          auto found = heads_.find(id + ":" + std::to_string(h));
          if (found != heads_.end()) {
            d.width = found->second.width;
            d.height = found->second.height;
            known = true;
          }
        }
        if (!known) {
          auto f = frame(id, h, false);
          d.width = f->image.width;
          d.height = f->image.height;
        }
        displays.push_back(d);
      }
      events = desktopInputEvents(head, events, displays);
    }
    std::string device;
    int offset = head;
    for (const Video& video : vm.videos) {
      if (offset < video.heads) {
        device = video.alias;
        break;
      }
      offset -= video.heads;
    }
    if (device.empty()) fail("Monitor has no input target");
    std::vector<Json> frames = inputFrames(events);
    for (size_t i = 0; i < frames.size(); i++) {
      if (i) usleep(25000);
      Json arguments = Json::Object{};
      arguments.set("device", device);
      arguments.set("head", offset);
      arguments.set("events", frames[i]);
      Json command = Json::Object{};
      command.set("execute", "input-send-event");
      command.set("arguments", arguments);
      Json result = Json::parse(hypervisor_->monitorCommand(id, command.dump()));
      if (result.has("error")) fail(result["error"]["desc"].str("Input rejected"));
    }
  }

  std::unique_ptr<Conn> console(const std::string& id) override {
    Vm vm = get(id);
    if (!activeState(vm.state)) fail("VM is not running");
    vms(true);  // the VNC port changes whenever the VM restarts
    vm = get(id);
    std::string target, note = consoleTarget(vm, options_, &target);
    if (!note.empty()) fail(note);
    return connectTcp(target, vm.vncPort, 5);
  }

 private:
  struct Frame {
    Image image;
    std::string png;
    int64_t time = 0;
  };
  using FramePtr = std::shared_ptr<Frame>;
  struct HeadInfo {
    int width, height;
    int64_t time;
  };

  std::vector<Vm> vms(bool force) {
    std::lock_guard<std::mutex> lock(inventoryMutex_);  // also makes concurrent callers share one load
    if (!force && loaded_ && nowMs() < until_) return cached_;
    std::vector<Vm> result;
    for (const DomainInfo& info : hypervisor_->list()) {
      try {
        if (info.xml.empty()) throw std::runtime_error("no description");
        result.push_back(parseDomain(info.xml, info.state));
      } catch (const std::exception&) {
        Vm vm;
        vm.id = lower(info.uuid);
        vm.name = info.name.empty() ? info.uuid : info.name;
        vm.state = "unavailable";
        if (isUuid(vm.id)) result.push_back(vm);
      }
    }
    std::sort(result.begin(), result.end(), [](const Vm& a, const Vm& b) { return a.name < b.name; });
    cached_ = result;
    loaded_ = true;
    until_ = nowMs() + options_.inventoryTtlMs;
    return result;
  }

  Vm get(const std::string& id) {
    if (!isUuid(id)) fail("Invalid VM identifier");
    std::string wanted = lower(id);
    for (const Vm& vm : vms(false))
      if (vm.id == wanted) return vm;
    fail("VM not found", 404);
  }

  FramePtr frame(const std::string& id, int head, bool fresh) {
    if (head < 0 || head > 3) fail("Invalid head");
    std::string key = lower(id) + ":" + std::to_string(head);
    std::promise<FramePtr> promise;
    std::shared_future<FramePtr> shared;
    bool owner = false;
    {
      std::lock_guard<std::mutex> lock(cacheMutex_);
      auto cached = frames_.find(key);
      if (!fresh && cached != frames_.end() && nowMs() - cached->second->time < 1200) return cached->second;
      auto running = inflight_.find(key);
      if (running != inflight_.end()) shared = running->second;
      else {
        owner = true;
        shared = promise.get_future().share();
        inflight_[key] = shared;
      }
    }
    if (!owner) return shared.get();
    try {
      FramePtr result = capture(id, head);
      {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (!frames_.count(key)) order_.push_back(key);
        frames_[key] = result;
        heads_[key] = HeadInfo{result->image.width, result->image.height, result->time};
        auto bytes = [&] {
          size_t n = 0;
          for (const auto& f : frames_) n += f.second->image.rgb.size() + f.second->png.size();
          return n;
        };
        while (order_.size() > 1 && (order_.size() > 32 || bytes() > (128u << 20))) {
          frames_.erase(order_.front());
          order_.pop_front();
        }
        inflight_.erase(key);
      }
      promise.set_value(result);
      return result;
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        inflight_.erase(key);
        heads_.erase(key);
      }
      promise.set_exception(std::current_exception());
      throw;
    }
  }

  FramePtr capture(const std::string& id, int head) {
    Vm vm = get(id);
    if (!activeState(vm.state)) fail("VM is not running");
    if (head >= vm.heads) fail("Head is not configured");
    struct Slot {
      LibvirtHost& host;
      explicit Slot(LibvirtHost& h) : host(h) {
        std::unique_lock<std::mutex> lock(host.slotMutex_);
        if (host.activeSlots_ >= 3) {
          if (host.waitingSlots_ >= 80) fail("Capture queue busy", 503);
          host.waitingSlots_++;
          host.slotFree_.wait(lock, [&] { return host.activeSlots_ < 3; });
          host.waitingSlots_--;
        }
        host.activeSlots_++;
      }
      ~Slot() {
        std::lock_guard<std::mutex> lock(host.slotMutex_);
        host.activeSlots_--;
        host.slotFree_.notify_one();
      }
    } slot(*this);
    auto result = std::make_shared<Frame>();
    result->image = decodeScreenshot(hypervisor_->screenshot(vm.id, head));
    result->png = encodePng(result->image);
    result->time = nowMs();
    return result;
  }

  std::shared_ptr<Hypervisor> hypervisor_;
  HostOptions options_;
  std::mutex inventoryMutex_, cacheMutex_, slotMutex_;
  std::condition_variable slotFree_;
  int activeSlots_ = 0, waitingSlots_ = 0;
  std::vector<Vm> cached_;
  bool loaded_ = false;
  int64_t until_ = 0;
  std::map<std::string, FramePtr> frames_;
  std::deque<std::string> order_;
  std::map<std::string, std::shared_future<FramePtr>> inflight_;
  std::map<std::string, HeadInfo> heads_;
  std::map<std::string, std::pair<int64_t, std::string>> previews_;
};

// A host reached through its agent: every call is one authenticated request.
class AgentHost : public HostApi {
 public:
  AgentHost(const std::string& url, std::string token) : url_(parseHttpUrl(url)), token_(std::move(token)) {}

  Json info() override { return Json::parse(call("GET", "/agent/v1/info", "", 8).body); }
  Json inventory() override { return Json::parse(call("GET", "/agent/v1/vms", "", 20).body)["vms"]; }
  std::string headPng(const std::string& id, int head, bool fresh) override {
    return call("GET", vmPath(id) + "/heads/" + std::to_string(head) + ".png" + (fresh ? "?fresh=1" : ""), "", 30).body;
  }
  std::string previewPng(const std::string& id, int width) override {
    return call("GET", vmPath(id) + "/preview.png?width=" + std::to_string(width), "", 30).body;
  }
  void input(const std::string& id, int head, const Json& events) override {
    Json body = Json::Object{};
    body.set("head", head);
    body.set("events", events);
    call("POST", vmPath(id) + "/input", body.dump(), 15);
  }
  std::unique_ptr<Conn> console(const std::string& id) override {
    return httpUpgrade(url_, vmPath(id) + "/console", headers(), "vmwatcher-console");
  }

 private:
  static std::string vmPath(const std::string& id) {
    if (!isUuid(id)) fail("Invalid VM identifier");
    return "/agent/v1/vms/" + lower(id);
  }
  Headers headers() const { return {{"Authorization", "Bearer " + token_}}; }
  Response call(const std::string& method, const std::string& path, const std::string& body, int timeout) {
    Headers h = headers();
    if (!body.empty()) h.push_back({"Content-Type", "application/json"});
    Response res;
    try {
      res = httpCall(url_, method, path, h, body, timeout);
    } catch (const HttpError&) {
      throw;
    } catch (const std::exception& e) {
      fail(std::string("Agent unreachable: ") + e.what(), 502);
    }
    if (res.status == 401) fail("The agent rejected the token", 502);
    if (res.status >= 400) {
      std::string message = "Agent request failed";
      try {
        message = Json::parse(res.body)["error"].str(message);
      } catch (const std::exception&) {
      }
      fail(message, res.status == 404 ? 404 : 400);
    }
    return res;
  }
  Url url_;
  std::string token_;
};

}  // namespace

std::shared_ptr<HostApi> makeLibvirtHost(std::shared_ptr<Hypervisor> hypervisor, HostOptions options) {
  return std::make_shared<LibvirtHost>(std::move(hypervisor), std::move(options));
}
std::shared_ptr<HostApi> makeAgentHost(const std::string& url, const std::string& token) {
  return std::make_shared<AgentHost>(url, token);
}

}  // namespace vmw
