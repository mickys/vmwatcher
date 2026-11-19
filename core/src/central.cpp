#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <set>

#include "vmw.hpp"
#include "updates.hpp"

namespace vmw {
namespace {

const char* const kCookie = "vmw_session";

bool hostIdOk(const std::string& id) {
  if (id.empty() || id.size() > 32) return false;
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

std::string cleanName(const Json& value, size_t max, const char* what) {
  std::string name = trim(value.str());
  if (name.empty() || name.size() > max) fail(std::string(what) + " must be 1 to " + std::to_string(max) + " characters");
  for (unsigned char c : name)
    if (c < 0x20) fail(std::string(what) + " contains control characters");
  return name;
}

// Only plain libvirt transports: a URI may otherwise name a command to run.
std::string checkLibvirtUri(const std::string& raw) {
  std::string uri = trim(raw);
  static const std::set<std::string> schemes = {"qemu", "qemu+ssh", "qemu+libssh", "qemu+libssh2", "qemu+tls", "qemu+tcp", "qemu+unix"};
  static const std::set<std::string> keys = {"keyfile", "no_verify", "no_tty", "known_hosts", "known_hosts_verify",
                                             "sshauth", "pkipath",   "tls_priority"};
  size_t scheme = uri.find("://");
  if (uri.size() > 300 || scheme == std::string::npos || !schemes.count(uri.substr(0, scheme)))
    fail("Use a libvirt URI such as qemu+ssh://root@host/system");
  for (unsigned char c : uri)
    if (c <= 0x20 || c == '\'' || c == '"' || c == '\\' || c == '`' || c == '$' || c == ';' || c == '|')
      fail("The libvirt URI contains unsupported characters");
  size_t query = uri.find('?');
  if (query != std::string::npos)
    for (const auto& pair : split(uri.substr(query + 1), '&'))
      if (!keys.count(pair.substr(0, pair.find('=')))) fail("Unsupported libvirt URI option: " + pair.substr(0, pair.find('=')));
  return uri;
}

struct HostEntry {
  Json config;
  std::shared_ptr<HostApi> api;
  std::string buildError;
  // Inventory snapshot, refreshed in the background so one unreachable host
  // never stalls the wall.
  std::mutex mutex;
  std::condition_variable loadedSignal;
  bool loaded = false, refreshing = false;
  int64_t time = 0;
  Json vms = Json::Array{};
  std::string error;
};
using HostPtr = std::shared_ptr<HostEntry>;

struct TabTraffic {
  int64_t lastSeen = 0;
  std::map<std::string, std::shared_ptr<Traffic>> counters;
};

struct Session {
  std::map<std::string, TabTraffic> tabs;
  int64_t expires = 0;
  int live = 0;
};

class Central : public Service {
 public:
  explicit Central(const Options& options) : options_(options) {
    makeDirs(options_.data);
    makeDirs(options_.data + "/audits");
    char name[256] = "";
    gethostname(name, sizeof name - 1);
    hostname_ = *name ? name : "this host";
    std::string path = options_.data + "/config.json";
    if (fileExists(path)) config_ = Json::parse(readFile(path));
    else {
      config_ = obj({{"hosts", Json::Array{}}, {"settings", Json::Object{}}});
      Json hosts = Json::Array{};
      if (options_.demo) {
        hosts.push(obj({{"id", "demo"}, {"name", "Demo host"}, {"type", "demo"}, {"enabled", true}}));
      } else if (libvirtAvailable()) {
        hosts.push(obj({{"id", "local"}, {"name", hostname_}, {"type", "local"}, {"enabled", true}}));
      }
      config_.set("hosts", hosts);
    }
    rebuildHosts();
    if (!options_.demo) updates_.start();
  }

  Response handle(Request& req, Conn& conn) override {
    Response res;
    try {
      res = route(req, conn);
    } catch (const HttpError& e) {
      res = Response::error(e.status, e.what());
    } catch (const std::exception& e) {
      logLine("Request failed: " + req.path + ": " + e.what());
      res = Response::error(400, e.what());
    }
    if (res.status == 0) return res;
    bool cached = false;
    for (const auto& h : res.headers) cached = cached || h.first == "Cache-Control";
    if (!cached) res.headers.push_back({"Cache-Control", "no-store"});
    res.headers.push_back({"X-Content-Type-Options", "nosniff"});
    res.headers.push_back({"Cross-Origin-Resource-Policy", "same-origin"});
    res.headers.push_back({"Referrer-Policy", "no-referrer"});
    if (https()) res.headers.push_back({"Strict-Transport-Security", "max-age=31536000"});
    res.headers.push_back(
        {"Content-Security-Policy",
         "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self' blob:; connect-src 'self' ws: wss:; "
         "img-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; font-src 'self'; object-src 'none'; base-uri 'none'; "
         "frame-ancestors 'none'"});
    return res;
  }

 private:
  // ------------------------------------------------------------ config
  Json settings() {
    std::lock_guard<std::mutex> lock(mutex_);
    return settingsLocked();
  }
  Json settingsLocked() const {
    const Json& s = config_["settings"];
    Json allowed = Json::Array{};
    for (const Json& h : s["allowedHosts"].arr()) allowed.push(h);
    return obj({
        {"videoEncoder", s["videoEncoder"].str("vnc")},
        {"siteName", "VM Watcher"},
        {"sessionHours", s["sessionHours"].integer(12)},
        {"previewSeconds", s["previewSeconds"].integer(10)},
        {"maxLivePerSession", s["maxLivePerSession"].integer(6)},
        {"maxLiveTotal", s["maxLiveTotal"].integer(24)},
        {"controlEnabled", s["controlEnabled"].boolean(true)},
        {"allowedHosts", allowed},
    });
  }
  void saveLocked() { writeFileAtomic(options_.data + "/config.json", config_.dump(2)); }

  HostPtr buildHost(const Json& config) {
    auto entry = std::make_shared<HostEntry>();
    entry->config = config;
    if (!config["enabled"].boolean(true)) return entry;
    try {
      const std::string& type = config["type"].str();
      if (type == "agent") entry->api = makeAgentHost(config["url"].str(), config["token"].str());
      else if (type == "local" || type == "libvirt") {
        HostOptions hostOptions;
        std::string uri = type == "local" ? options_.uri : checkLibvirtUri(config["uri"].str());
        hostOptions.remoteHost = type == "local" ? "" : remoteHostOfUri(uri);
        hostOptions.local = hostOptions.remoteHost.empty();
        if (hostOptions.local && options_.bind != "0.0.0.0" && options_.bind != "::") hostOptions.own.push_back(options_.bind);
        entry->api = makeLibvirtHost(std::shared_ptr<Hypervisor>(openLibvirt(uri)), hostOptions);
      } else if (type == "demo" && options_.demo)
        entry->api = makeLibvirtHost(std::shared_ptr<Hypervisor>(openDemo(config["id"].str())), HostOptions{});
      else
        throw std::runtime_error("Unknown host type");
    } catch (const std::exception& e) {
      entry->buildError = e.what();
    }
    return entry;
  }
  void rebuildHosts() {
    std::map<std::string, HostPtr> next;
    std::vector<std::string> order;
    for (const Json& config : config_["hosts"].arr()) {
      const std::string& id = config["id"].str();
      auto existing = hosts_.find(id);
      // Keep the live connection (and its caches) when nothing about the host changed.
      next[id] = existing != hosts_.end() && existing->second->config.dump() == config.dump() ? existing->second : buildHost(config);
      order.push_back(id);
    }
    hosts_ = next;
    hostOrder_ = order;
  }
  HostPtr hostEntry(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = hosts_.find(id);
    if (found == hosts_.end()) fail("Host not found", 404);
    return found->second;
  }
  std::shared_ptr<HostApi> hostApi(const std::string& id) {
    HostPtr entry = hostEntry(id);
    if (!entry->config["enabled"].boolean(true)) fail("This host is disabled");
    if (!entry->api) fail(entry->buildError.empty() ? "Host unavailable" : entry->buildError);
    return entry->api;
  }

  static void refresh(HostPtr entry) {
    Json vms = Json::Array{};
    std::string error;
    try {
      vms = entry->api->inventory();
      if (!vms.isArray()) throw std::runtime_error("Unexpected inventory reply");
    } catch (const std::exception& e) {
      error = e.what();
      vms = Json::Array{};
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    // A failed refresh keeps the last known list, flagged with the error.
    if (error.empty() || !entry->loaded) entry->vms = vms;
    entry->error = error;
    entry->time = nowMs();
    entry->loaded = true;
    entry->refreshing = false;
    entry->loadedSignal.notify_all();
  }
  // Returns {vms, error}; only the very first load of a host waits.
  std::pair<Json, std::string> snapshot(const HostPtr& entry, int waitMs = 6000) {
    if (!entry->api) return {Json::Array{}, entry->buildError};
    std::unique_lock<std::mutex> lock(entry->mutex);
    int64_t age = nowMs() - entry->time;
    if ((!entry->loaded || age > (entry->error.empty() ? 3000 : 8000)) && !entry->refreshing) {
      entry->refreshing = true;
      std::thread(refresh, entry).detach();
    }
    if (!entry->loaded)
      entry->loadedSignal.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return entry->loaded; });
    if (!entry->loaded) return {Json::Array{}, "Still connecting…"};
    return {entry->vms, entry->error};
  }
  Json findVm(const std::string& host, const std::string& id) {
    for (const Json& vm : snapshot(hostEntry(host)).first.arr())
      if (vm["id"].str() == id) return vm;
    fail("VM not found", 404);
  }

  // ------------------------------------------------------------ access
  std::vector<std::string> allowedHosts() {
    std::vector<std::string> out = {"localhost", "127.0.0.1", "[::1]"};
    if (options_.bind != "0.0.0.0" && options_.bind != "::")
      out.push_back(options_.bind.find(':') != std::string::npos ? "[" + options_.bind + "]" : options_.bind);
    for (const auto& h : options_.allowedHosts) out.push_back(lower(h));
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Json& h : config_["settings"]["allowedHosts"].arr()) out.push_back(lower(h.str()));
    return out;
  }
  // DNS-rebinding and cross-site protection: the page must be opened through a
  // configured address, and requests may only come from that same origin.
  bool trusted(const Request& req) {
    std::string host = lower(req.header("host"));
    if (host.empty() || host.find_first_of("@/ ") != std::string::npos) return false;
    std::string name = host;
    size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']', colon) == std::string::npos) name = host.substr(0, colon);
    auto allowed = allowedHosts();
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) return false;
    if (req.header("sec-fetch-site") == "cross-site") {
      bool viewerLink = req.method == "GET" && (req.path == "/" || req.path == "/index.html") &&
                        req.header("sec-fetch-mode") == "navigate" && req.header("sec-fetch-dest") == "document";
      if (!viewerLink) return false;
    }
    std::string origin = req.header("origin");
    return origin.empty() || lower(origin) == scheme() + host;
  }
  bool https() const { return !options_.tlsCert.empty(); }
  std::string scheme() const { return https() ? "https://" : "http://"; }
  std::string cookieFlags() const { return std::string("; Path=/; HttpOnly; SameSite=Strict") + (https() ? "; Secure" : ""); }
  std::string cookie(const Request& req) {
    for (const auto& part : split(req.header("cookie"), ';')) {
      std::string item = trim(part);
      if (item.compare(0, strlen(kCookie) + 1, std::string(kCookie) + "=") == 0) return item.substr(strlen(kCookie) + 1);
    }
    return "";
  }
  std::string session(const Request& req, bool touch = true) {
    std::string key = cookie(req);
    if (key.size() != 64) return "";
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(key);
    if (found == sessions_.end() || found->second.expires < nowMs()) return "";
    if (touch) found->second.expires = nowMs() + settingsLocked()["sessionHours"].integer() * 3600000;
    return key;
  }
  Response withSession(Response res) {
    std::string key = randomHex(32);
    int64_t hours;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = sessions_.begin(); it != sessions_.end();)
        it = it->second.expires < nowMs() && it->second.live == 0 ? sessions_.erase(it) : std::next(it);
      if (sessions_.size() >= 256) fail("Too many sessions; try again later", 503);
      hours = settingsLocked()["sessionHours"].integer();
      sessions_[key].expires = nowMs() + hours * 3600000;
    }
    res.headers.push_back({"Set-Cookie", std::string(kCookie) + "=" + key + cookieFlags() + "; Max-Age=" +
                                             std::to_string(hours * 3600)});
    return res;
  }
  void checkNewPassword(const std::string& password) {
    if (password.size() < 8 || password.size() > 256) fail("Use a password of at least 8 characters");
  }
  // Failed logins lock the client address out for a growing interval.
  void throttle(const std::string& peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = failures_[peer];
    if (entry.second > nowMs())
      fail("Too many attempts. Try again in " + std::to_string((entry.second - nowMs()) / 1000 + 1) + " seconds", 429);
  }
  void loginFailed(const std::string& peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failures_.size() > 4096) failures_.clear();
    auto& entry = failures_[peer];
    entry.first++;
    if (entry.first >= 5) entry.second = nowMs() + std::min<int64_t>(300000, 5000LL << std::min(entry.first - 5, 6));
  }
  bool passwordMatches(const std::string& password) {
    Json record;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      record = config_["password"];
    }
    if (password.size() > 256) return false;
    std::lock_guard<std::mutex> one(verifyMutex_);  // bounds the CPU a login flood can use
    return verifyPassword(record, password);
  }

  // ------------------------------------------------------------ routing
  Response route(Request& req, Conn& conn) {
    if (!trusted(req)) return Response::error(403, "Open the viewer through its configured address");
    if (req.path.compare(0, 5, "/api/") != 0) return serveStatic(req);
    auto parts = split(req.path, '/');  // "", "api", ...
    const std::string& first = parts[2];
    bool write = req.method != "GET";
    // State-changing requests must come from the viewer's own page.
    if (write && req.header("origin").empty()) return Response::error(403, "Use the viewer to make changes");

    if (parts.size() == 3 && first == "session" && !write) {
      bool setup;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        setup = !config_["password"].isObject();
      }
      bool in = !session(req).empty();
      Json all = settings();
      Json out = obj({{"setupRequired", setup}, {"authenticated", in}, {"siteName", all["siteName"]}, {"version", kVersion}});
      if (in) {
        all.erase("allowedHosts");
        out.set("settings", all);
      }
      return Response::json(out);
    }
    if (parts.size() == 3 && first == "setup" && req.method == "POST") {
      std::string password = Json::parse(req.body)["password"].str();
      checkNewPassword(password);
      Json record = hashPassword(password);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_["password"].isObject()) fail("A password is already set", 403);
        config_.set("password", record);
        saveLocked();
      }
      logLine("Initial password set from " + req.peer);
      return withSession(Response::json(obj({{"ok", true}})));
    }
    if (parts.size() == 3 && first == "login" && req.method == "POST") {
      throttle(req.peer);
      if (!passwordMatches(Json::parse(req.body)["password"].str())) {
        loginFailed(req.peer);
        logLine("Failed login from " + req.peer);
        usleep(400000);
        return Response::error(401, "Wrong password");
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.erase(req.peer);
      }
      return withSession(Response::json(obj({{"ok", true}})));
    }
    bool meter = req.path == "/api/bandwidth";
    std::string key = session(req, !meter);
    if (parts.size() == 3 && first == "logout" && req.method == "POST") {
      std::lock_guard<std::mutex> lock(mutex_);
      sessions_.erase(key);
      Response res = Response::json(obj({{"ok", true}}));
      res.headers.push_back({"Set-Cookie", std::string(kCookie) + "=" + cookieFlags() + "; Max-Age=0"});
      return res;
    }
    if (key.empty()) return Response::error(401, "Sign in first");

    std::string tabId = req.param("tab");
    if (isUuid(tabId)) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto signedIn = sessions_.find(key);
      if (signedIn == sessions_.end()) return Response::error(401, "Sign in first");
      auto& tabs = signedIn->second.tabs;
      if (!tabs.count(tabId) && tabs.size() >= 32) {
        auto oldest = std::min_element(tabs.begin(), tabs.end(), [](const auto& a, const auto& b) {
          return a.second.lastSeen < b.second.lastSeen;
        });
        tabs.erase(oldest);
      }
      auto& tab = tabs[tabId];
      tab.lastSeen = nowMs();
      if (meter && !write) {
        Json totals = Json::Object{};
        for (const auto& [name, count] : tab.counters)
          totals.set(name, obj({{"down", count->down.load()}, {"up", count->up.load()}}));
        return Response::json(totals);
      }
      std::string scope = "viewer";
      if (parts.size() >= 7 && first == "hosts" && hostIdOk(parts[3]) && parts[4] == "vms" && isUuid(parts[5])) {
        scope = parts[3] + "/" + parts[5];
        if (parts[6] == "video.mp4" || parts[6] == "vnc" || parts[6] == "audio") scope += "/0";
        else if (parts.size() == 8 && parts[6] == "heads" && parts[7].size() == 5 && parts[7][0] >= '0' && parts[7][0] <= '3')
          scope += "/" + parts[7].substr(0,1);
        else if (parts[6] == "input") {
          auto head = Json::parse(req.body)["head"].integer(-1);
          if (head >= 0 && head <= 3) scope += "/" + std::to_string(head);
        }
      }
      if (!tab.counters.count(scope) && tab.counters.size() >= 512) scope = "viewer";
      auto& counter = tab.counters[scope];
      if (!counter) counter = std::make_shared<Traffic>();
      conn.traffic = counter;
      counter->up += req.headerBytes + req.body.size();
    }
    if (parts.size() == 3 && first == "updates") {
      if (req.method != "GET" && req.method != "POST") return Response::error(405, "Use GET or POST");
      return Response::json(updates_.status(req.method == "POST"));
    }
    if (parts.size() == 3 && first == "vms" && !write) return Response::json(inventory());
    if (first == "hosts" && parts.size() >= 6 && parts[4] == "vms") return vmRoute(req, conn, parts, key);
    if (first == "groups") return groupRoute(req, parts);
    if (first == "audits") return auditRoute(req, parts);
    if (first == "settings" && parts.size() == 3) return settingsRoute(req);
    if (first == "password" && parts.size() == 3 && req.method == "POST") return passwordRoute(req, key);
    if (first == "hosts") return hostsRoute(req, parts);
    return Response::error(404, "Not found");
  }

  Response serveStatic(const Request& req) {
    if (req.method != "GET") return Response::error(405, "Method not allowed");
    std::string target = req.path == "/" ? "/index.html" : req.path;
    bool asset = target.compare(0, 8, "/assets/") == 0 && target.size() > 8;
    for (size_t i = asset ? 8 : 1; i < target.size(); i++) {
      char c = target[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
        return Response::error(404, "Not found");
    }
    if (target.find("..") != std::string::npos || (!asset && target != "/index.html" && target != "/favicon.svg"))
      return Response::error(404, "Not found");
    std::string extension = target.substr(target.rfind('.') + 1);
    static const std::map<std::string, std::string> types = {
        {"html", "text/html; charset=utf-8"}, {"js", "text/javascript"}, {"css", "text/css"},   {"wasm", "application/wasm"},
        {"svg", "image/svg+xml"},             {"png", "image/png"},      {"woff2", "font/woff2"}, {"json", "application/json"}};
    auto type = types.find(extension);
    if (type == types.end()) return Response::error(404, "Not found");
    Response res = Response::bytes(readFile(options_.web + target, 64u << 20), type->second);
    if (asset) res.headers.push_back({"Cache-Control", "public, max-age=31536000, immutable"});
    return res;
  }

  Json inventory() {
    std::vector<HostPtr> entries;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& id : hostOrder_) entries.push_back(hosts_[id]);
    }
    // Kick every host's refresh first, so first loads run side by side.
    for (const auto& entry : entries)
      if (entry->config["enabled"].boolean(true)) snapshot(entry, 0);
    Json hosts = Json::Array{}, vms = Json::Array{};
    for (const auto& entry : entries) {
      const std::string& id = entry->config["id"].str();
      bool enabled = entry->config["enabled"].boolean(true);
      std::pair<Json, std::string> state = enabled ? snapshot(entry) : std::make_pair(Json(Json::Array{}), std::string());
      hosts.push(obj({{"id", id},
                      {"name", entry->config["name"]},
                      {"type", entry->config["type"]},
                      {"enabled", enabled},
                      {"ok", enabled && state.second.empty()},
                      {"error", state.second},
                      {"count", state.first.arr().size()}}));
      for (Json vm : state.first.arr()) {
        vm.set("host", id);
        vm.set("key", id + "/" + vm["id"].str());
        vms.push(vm);
      }
    }
    return obj({{"hosts", hosts}, {"vms", vms}, {"capturedAt", isoTime(nowMs())}});
  }

  // /api/hosts/<host>/vms/<id>/<action>
  Response vmRoute(Request& req, Conn& conn, const std::vector<std::string>& parts, const std::string& key) {
    const std::string& host = parts[3];
    const std::string& id = parts[5];
    if (!hostIdOk(host) || !isUuid(id) || parts.size() < 7) return Response::error(404, "Not found");
    const std::string& action = parts[6];
    if (parts.size() == 7 && (action == "vnc" || action == "audio" || action == "video.mp4")) return liveRoute(req, conn, host, id, action, key);
    auto api = hostApi(host);
    if (parts.size() == 7 && action == "preview.png" && req.method == "GET")
      return Response::bytes(api->previewPng(id, atoi(req.param("width", "720").c_str())), "image/png");
    if (parts.size() == 8 && action == "heads" && parts[7].size() == 5 && parts[7][0] >= '0' && parts[7][0] <= '3' &&
        parts[7].compare(1, 4, ".png") == 0 && req.method == "GET")
      return Response::bytes(api->headPng(id, parts[7][0] - '0', false), "image/png");
    if (parts.size() == 7 && action == "capture") {
      if (req.method != "POST") return Response::error(405, "Use POST");
      return Response::json(capture(host, id), 201);
    }
    if (parts.size() == 7 && action == "input") {
      if (req.method != "POST") return Response::error(405, "Use POST");
      if (!settings()["controlEnabled"].boolean()) return Response::error(403, "Keyboard and mouse control is turned off in Settings");
      Json body = Json::parse(req.body);
      if (!body["head"].isInt()) fail("Monitor unavailable");
      api->input(id, int(body["head"].integer()), body["events"]);
      return Response::json(obj({{"ok", true}}));
    }
    return Response::error(404, "Not found");
  }

  Response liveRoute(Request& req, Conn& conn, const std::string& host, const std::string& id, const std::string& action,
                     const std::string& key) {
    if (req.method != "GET") return Response::error(405, "Use GET");
    if (req.header("origin").empty() && action != "video.mp4") return Response::error(403, "Use the viewer");
    Json limits = settings();
    auto take = [&] {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = sessions_.find(key);
      if (found == sessions_.end()) fail("Sign in first", 401);
      if (liveTotal_ >= limits["maxLiveTotal"].integer() || found->second.live >= limits["maxLivePerSession"].integer())
        fail("Live console limit reached", 429);
      liveTotal_++;
      found->second.live++;
    };
    take();
    struct Release {
      Central& central;
      std::string key;
      ~Release() {
        std::lock_guard<std::mutex> lock(central.mutex_);
        central.liveTotal_--;
        auto found = central.sessions_.find(key);
        if (found != central.sessions_.end()) found->second.live--;
      }
    } release{*this, key};
    if (action == "video.mp4") {
      streamVideo(conn, *hostApi(host), id, limits["videoEncoder"].str());
      return Response::taken();
    }
    std::unique_ptr<Conn> console = hostApi(host)->console(id);
    bool binary = false;
    for (const auto& p : split(req.header("sec-websocket-protocol"), ','))
      if (trim(p) == "binary") binary = true;
    wsAccept(conn, req, action == "vnc" && binary ? "binary" : "");
    // The wall never sends input over VNC: keyboard and mouse go through the
    // validated input endpoint instead, so every console is filtered read-only.
    if (action == "audio") bridgeAudio(conn, *console);
    else bridgeConsole(conn, *console, true);
    return Response::taken();
  }

  // ------------------------------------------------------------ groups
  Json readGroups() {
    std::string path = options_.data + "/groups.json";
    Json out = Json::Array{};
    if (!fileExists(path)) return out;
    Json data = Json::parse(readFile(path));
    for (const Json& group : data.arr()) {
      Json members = Json::Array{};
      for (const Json& member : group["vms"].arr())
        // Groups saved before multi-host support hold bare VM identifiers.
        members.push(isUuid(member.str()) ? "local/" + member.str() : member.str());
      out.push(obj({{"name", group["name"]}, {"vms", members}}));
    }
    return out;
  }
  Response groupRoute(Request& req, const std::vector<std::string>& parts) {
    std::lock_guard<std::mutex> lock(groupMutex_);
    Json groups = readGroups();
    if (parts.size() == 3 && req.method == "GET") return Response::json(obj({{"groups", groups}}));
    if (parts.size() != 4 || (req.method != "PUT" && req.method != "DELETE")) return Response::error(405, "Method not allowed");
    std::string name = cleanName(Json(parts[3]), 60, "Group names");
    Json next = Json::Array{};
    bool found = false;
    Json group;
    if (req.method == "PUT") {
      Json body = Json::parse(req.body);
      const Json& vms = body["vms"];
      if (!vms.isArray() || vms.arr().size() > 200) fail("Invalid group members");
      std::set<std::string> seen;
      Json members = Json::Array{};
      for (const Json& member : vms.arr()) {
        auto bits = split(member.str(), '/');
        if (bits.size() != 2 || !hostIdOk(bits[0]) || !isUuid(bits[1])) fail("Invalid group members");
        if (seen.insert(member.str()).second) members.push(member.str());
      }
      group = obj({{"name", name}, {"vms", members}});
    }
    for (const Json& existing : groups.arr()) {
      if (existing["name"].str() != name) next.push(existing);
      else {
        found = true;
        if (req.method == "PUT") next.push(group);
      }
    }
    if (req.method == "PUT" && !found) {
      if (groups.arr().size() >= 100) fail("Too many groups");
      next.push(group);
    }
    if (req.method == "DELETE" && !found) fail("Group not found", 404);
    Json::Array sorted = next.arr();
    std::sort(sorted.begin(), sorted.end(), [](const Json& a, const Json& b) { return a["name"].str() < b["name"].str(); });
    writeFileAtomic(options_.data + "/groups.json", Json(sorted).dump(2));
    return Response::json(obj({{"groups", Json(sorted)}}));
  }

  // ------------------------------------------------------------ audits
  Json capture(const std::string& host, const std::string& id) {
    if (auditPending_ >= 20) fail("Audit queue busy", 503);
    auditPending_++;
    struct Done {
      std::atomic<int>& n;
      ~Done() { n--; }
    } done{auditPending_};
    std::lock_guard<std::mutex> lock(auditMutex_);  // audits are taken one at a time
    Json vm = findVm(host, id);
    auto api = hostApi(host);
    std::string startedAt = isoTime(nowMs());
    std::vector<Image> images;
    std::vector<std::pair<std::string, std::string>> files;
    Json displays = Json::Array{}, unavailable = Json::Array{};
    for (int head = 0; head < vm["heads"].integer(1); head++) {
      try {
        std::string png = api->headPng(id, head, true);
        Image image = decodePng(png);
        std::string name = "head-" + std::to_string(head) + ".png";
        displays.push(obj({{"head", head},
                           {"file", name},
                           {"width", image.width},
                           {"height", image.height},
                           {"capturedAt", isoTime(nowMs())},
                           {"sha256", hex(sha256(png))}}));
        files.push_back({name, png});
        images.push_back(std::move(image));
      } catch (const std::exception& e) {
        unavailable.push(obj({{"head", head}, {"error", e.what()}}));
      }
    }
    if (images.empty()) fail("No displays could be captured");
    Image canvas = combineImages(images);
    std::string combined = encodePng(canvas, 3);
    files.push_back({"combined.png", combined});
    files.push_back({"preview.png", encodePng(resizeImage(canvas, 720))});
    std::string captureId = uuid4(), folder = options_.data + "/audits/" + captureId;
    Json hostName = hostEntry(host)->config["name"];
    Json manifest = obj({
        {"id", captureId},
        {"vm", obj({{"host", host}, {"hostName", hostName}, {"id", id}, {"name", vm["name"]}, {"state", vm["state"]}})},
        {"startedAt", startedAt},
        {"finishedAt", isoTime(nowMs())},
        {"method", "libvirt per-head capture, sequential; combined image preserves native pixel dimensions"},
        {"displays", displays},
        {"unavailable", unavailable},
        {"combined", obj({{"file", "combined.png"}, {"sha256", hex(sha256(combined))}})},
    });
    makeDirs(folder);
    try {
      for (const auto& file : files) writeFileAtomic(folder + "/" + file.first, file.second);
      writeFileAtomic(folder + "/manifest.json", manifest.dump(2));
    } catch (...) {
      removeTree(folder);
      throw;
    }
    trimAudits();
    return manifest;
  }
  static bool auditIdOk(const std::string& id) { return isUuid(id); }
  std::vector<Json> listAudits() {
    std::vector<Json> out;
    for (const auto& name : listDir(options_.data + "/audits")) {
      if (!auditIdOk(name)) continue;
      try {
        Json item = Json::parse(readFile(options_.data + "/audits/" + name + "/manifest.json"));
        if (item["id"].str() == name) out.push_back(item);
      } catch (const std::exception&) {
      }
    }
    std::sort(out.begin(), out.end(), [](const Json& a, const Json& b) { return a["startedAt"].str() > b["startedAt"].str(); });
    return out;
  }
  // Keeps the newest 200 audits and at most 2 GiB.
  void trimAudits() {
    uint64_t used = 0;
    auto items = listAudits();
    for (size_t i = 0; i < items.size(); i++) {
      std::string folder = options_.data + "/audits/" + items[i]["id"].str();
      for (const auto& file : listDir(folder)) {
        try {
          used += readFile(folder + "/" + file).size();
        } catch (const std::exception&) {
        }
      }
      if (i >= 200 || used > (2ull << 30)) removeTree(folder);
    }
  }
  Response auditRoute(Request& req, const std::vector<std::string>& parts) {
    if (req.method != "GET") return Response::error(405, "Method not allowed");
    if (parts.size() == 3) {
      std::string wanted = req.param("vm");
      Json out = Json::Array{};
      for (const Json& item : listAudits()) {
        std::string key = item["vm"]["host"].str("local") + "/" + item["vm"]["id"].str();
        if ((wanted.empty() || key == wanted) && out.arr().size() < 40) out.push(item);
      }
      return Response::json(obj({{"audits", out}}));
    }
    if (parts.size() != 5 || !auditIdOk(parts[3])) return Response::error(404, "Not found");
    std::string folder = options_.data + "/audits/" + parts[3];
    const std::string& file = parts[4];
    Json manifest = Json::parse(readFile(folder + "/manifest.json"));
    if (file == "manifest.json") return Response::json(manifest);
    if (file == "bundle.zip") {
      std::vector<std::pair<std::string, std::string>> files = {{"manifest.json", manifest.dump(2)},
                                                                {"combined.png", readFile(folder + "/combined.png")}};
      for (const Json& display : manifest["displays"].arr()) {
        const std::string& name = display["file"].str();
        if (name.size() == 10 && name.compare(0, 5, "head-") == 0 && name[5] >= '0' && name[5] <= '3' && name.compare(6, 4, ".png") == 0)
          files.push_back({name, readFile(folder + "/" + name)});
      }
      Response res = Response::bytes(zipStore(files), "application/zip");
      res.headers.push_back({"Content-Disposition", "attachment; filename=\"vm-audit-" + parts[3] + ".zip\""});
      return res;
    }
    bool head = file.size() == 10 && file.compare(0, 5, "head-") == 0 && file[5] >= '0' && file[5] <= '3' && file.compare(6, 4, ".png") == 0;
    if (file != "combined.png" && file != "preview.png" && !head) return Response::error(404, "Not found");
    if (file == "preview.png" && !fileExists(folder + "/preview.png"))
      return Response::bytes(encodePng(resizeImage(decodePng(readFile(folder + "/combined.png")), 720)), "image/png");
    return Response::bytes(readFile(folder + "/" + file), "image/png");
  }

  // ------------------------------------------------------------ settings
  static Json publicHost(const Json& config) {
    return obj({{"id", config["id"]},
                {"name", config["name"]},
                {"type", config["type"]},
                {"enabled", config["enabled"].boolean(true)},
                {"url", config["url"].str("")},
                {"uri", config["uri"].str("")},
                {"hasToken", !config["token"].str("").empty()}});
  }
  Json settingsView() {
    std::string problem;
    bool available = libvirtAvailable(&problem);
    Json encoders = videoEncoders();
    std::lock_guard<std::mutex> lock(mutex_);
    Json hosts = Json::Array{};
    for (const Json& config : config_["hosts"].arr()) hosts.push(publicHost(config));
    return obj({{"settings", settingsLocked()},
                {"hosts", hosts},
                {"videoEncoders", encoders},
                {"updates", updates_.status(false)},
                {"libvirt", obj({{"available", available}, {"problem", problem}})},
                {"server", obj({{"version", kVersion}, {"hostname", hostname_}, {"bind", options_.bind}, {"port", options_.port}, {"https", https()},
                                {"demo", options_.demo}})}});
  }
  Response settingsRoute(Request& req) {
    if (req.method == "GET") return Response::json(settingsView());
    if (req.method != "PUT") return Response::error(405, "Method not allowed");
    Json body = Json::parse(req.body);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Json next = settingsLocked();
      auto number = [&](const char* name, int64_t low, int64_t high) {
        if (!body.has(name)) return;
        if (!body[name].isInt() || body[name].integer() < low || body[name].integer() > high)
          fail(std::string(name) + " must be between " + std::to_string(low) + " and " + std::to_string(high));
        next.set(name, body[name].integer());
      };
      number("sessionHours", 1, 720);
      number("previewSeconds", 0, 300);
      number("maxLivePerSession", 1, 16);
      number("maxLiveTotal", 1, 64);
      if (body.has("videoEncoder")) {
        if (!videoEncoders().has(body["videoEncoder"].str())) fail("Video encoder unavailable");
        next.set("videoEncoder", body["videoEncoder"]);
      }
      if (body.has("controlEnabled")) {
        if (!body["controlEnabled"].isBool()) fail("controlEnabled must be true or false");
        next.set("controlEnabled", body["controlEnabled"].boolean());
      }
      if (body.has("allowedHosts")) {
        Json allowed = Json::Array{};
        if (!body["allowedHosts"].isArray() || body["allowedHosts"].arr().size() > 16) fail("Invalid address list");
        for (const Json& item : body["allowedHosts"].arr()) {
          std::string name = lower(trim(item.str()));
          if (name.empty()) continue;
          if (name.size() > 100 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-:[]") != std::string::npos)
            fail("Extra addresses must be host names or IP addresses, without a port");
          allowed.push(name);
        }
        next.set("allowedHosts", allowed);
      }
      config_.set("settings", next);
      saveLocked();
    }
    return Response::json(settingsView());
  }
  Response passwordRoute(Request& req, const std::string& key) {
    Json body = Json::parse(req.body);
    throttle(req.peer);
    if (!passwordMatches(body["current"].str())) {
      loginFailed(req.peer);
      usleep(400000);
      return Response::error(403, "The current password is wrong");
    }
    checkNewPassword(body["password"].str());
    Json record = hashPassword(body["password"].str());
    std::lock_guard<std::mutex> lock(mutex_);
    config_.set("password", record);
    saveLocked();
    // Everyone else has to sign in again with the new password.
    for (auto it = sessions_.begin(); it != sessions_.end();) it = it->first == key ? std::next(it) : sessions_.erase(it);
    logLine("Password changed from " + req.peer);
    return Response::json(obj({{"ok", true}}));
  }

  // Validates a host definition from the settings page. `previous` supplies
  // the stored token when the form left it blank.
  Json hostFromBody(const Json& body, const Json& previous) {
    std::string type = body["type"].str(previous["type"].str());
    Json host = obj({{"name", cleanName(body.has("name") ? body["name"] : previous["name"], 60, "The host name")},
                     {"type", type},
                     {"enabled", body.has("enabled") ? body["enabled"].boolean(true) : previous["enabled"].boolean(true)}});
    if (!previous.isNull() && type != previous["type"].str()) fail("A host's type cannot be changed; add a new host instead");
    if (type == "agent") {
      std::string url = trim(body.has("url") ? body["url"].str() : previous["url"].str());
      parseHttpUrl(url);
      std::string token = trim(body["token"].str());
      if (token.empty()) token = previous["token"].str();
      if (token.size() < 32 || token.size() > 128 || token.find_first_not_of("0123456789abcdef") != std::string::npos)
        fail("Paste the agent token (shown by: vmwatcher agent-token)");
      host.set("url", url);
      host.set("token", token);
    } else if (type == "libvirt") {
      host.set("uri", checkLibvirtUri(body.has("uri") ? body["uri"].str() : previous["uri"].str()));
    } else if (type == "local") {
      std::string problem;
      if (!libvirtAvailable(&problem)) fail(problem);
    } else if (!(type == "demo" && options_.demo))
      fail("Choose a host type: local, agent or libvirt");
    return host;
  }
  Json findHostConfigLocked(const std::string& id) {
    for (const Json& config : config_["hosts"].arr())
      if (config["id"].str() == id) return config;
    return nullptr;
  }
  Response hostsRoute(Request& req, const std::vector<std::string>& parts) {
    // POST /api/hosts/test: try a definition without saving it.
    if (parts.size() == 4 && parts[3] == "test" && req.method == "POST") {
      Json body = Json::parse(req.body), previous;
      if (body["id"].isString()) {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = findHostConfigLocked(body["id"].str());
      }
      Json host = hostFromBody(body, previous);
      host.set("id", "test");
      host.set("enabled", true);
      HostPtr entry = buildHost(host);
      if (!entry->api) return Response::json(obj({{"ok", false}, {"error", entry->buildError}}));
      try {
        Json info = entry->api->info();
        Json vms = entry->api->inventory();
        int64_t running = 0;
        for (const Json& vm : vms.arr()) running += vm["state"].str() == "running";
        return Response::json(obj({{"ok", true}, {"info", info}, {"vms", vms.arr().size()}, {"running", running}}));
      } catch (const std::exception& e) {
        return Response::json(obj({{"ok", false}, {"error", e.what()}}));
      }
    }
    if (parts.size() == 3 && req.method == "POST") {
      Json host = hostFromBody(Json::parse(req.body), nullptr);
      std::lock_guard<std::mutex> lock(mutex_);
      if (config_["hosts"].arr().size() >= 64) fail("Too many hosts");
      if (host["type"].str() == "local" && !findHostConfigLocked("local").isNull()) fail("This machine is already listed");
      // The identifier is permanent: groups and saved audits refer to it.
      std::string base = host["type"].str() == "local" ? "local" : "";
      if (base.empty()) {
        for (char c : lower(host["name"].str())) {
          if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) base += c;
          else if (!base.empty() && base.back() != '-') base += '-';
        }
        while (!base.empty() && base.back() == '-') base.pop_back();
        base = base.substr(0, 24);
        if (base.empty() || base == "local" || base == "test") base = "host";
      }
      std::string id = base;
      for (int n = 2; !findHostConfigLocked(id).isNull(); n++) id = base + "-" + std::to_string(n);
      host.set("id", id);
      Json hosts = config_["hosts"];
      hosts.push(host);
      config_.set("hosts", hosts);
      saveLocked();
      rebuildHosts();
      return Response::json(publicHost(host), 201);
    }
    if (parts.size() == 4 && (req.method == "PUT" || req.method == "DELETE")) {
      const std::string& id = parts[3];
      std::lock_guard<std::mutex> lock(mutex_);
      Json previous = findHostConfigLocked(id);
      if (previous.isNull()) fail("Host not found", 404);
      Json replacement;
      if (req.method == "PUT") {
        replacement = hostFromBody(Json::parse(req.body), previous);
        replacement.set("id", id);
      }
      Json hosts = Json::Array{};
      for (const Json& config : config_["hosts"].arr()) {
        if (config["id"].str() != id) hosts.push(config);
        else if (req.method == "PUT") hosts.push(replacement);
      }
      config_.set("hosts", hosts);
      saveLocked();
      rebuildHosts();
      return Response::json(req.method == "PUT" ? publicHost(replacement) : obj({{"ok", true}}));
    }
    return Response::error(404, "Not found");
  }

  UpdateChecker updates_;
  Options options_;
  std::string hostname_;
  std::mutex mutex_, groupMutex_, auditMutex_, verifyMutex_;
  Json config_;
  std::map<std::string, HostPtr> hosts_;
  std::vector<std::string> hostOrder_;
  std::map<std::string, Session> sessions_;
  std::map<std::string, std::pair<int, int64_t>> failures_;
  int64_t liveTotal_ = 0;
  std::atomic<int> auditPending_{0};
};

}  // namespace

std::unique_ptr<Service> makeCentral(const Options& options) { return std::unique_ptr<Service>(new Central(options)); }

void setPassword(const std::string& dataDir, const std::string& password) {
  if (password.size() < 8 || password.size() > 256) throw std::runtime_error("Use a password of at least 8 characters");
  makeDirs(dataDir);
  std::string path = dataDir + "/config.json";
  Json config = fileExists(path) ? Json::parse(readFile(path)) : obj({{"hosts", Json::Array{}}, {"settings", Json::Object{}}});
  config.set("password", hashPassword(password));
  writeFileAtomic(path, config.dump(2));
}

}  // namespace vmw
