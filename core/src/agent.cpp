#include <unistd.h>

#include "vmw.hpp"

namespace vmw {

std::string agentToken(const std::string& dataDir, bool create) {
  std::string path = dataDir + "/agent.json";
  if (fileExists(path)) {
    std::string token = Json::parse(readFile(path))["token"].str();
    if (token.size() >= 32) return token;
  }
  if (!create) throw std::runtime_error("No agent token yet: start the agent once first");
  makeDirs(dataDir);
  std::string token = randomHex(32);
  writeFileAtomic(path, obj({{"token", token}}).dump(2));
  return token;
}

namespace {

// The agent exposes one hypervisor to the central service. Every request must
// carry the agent token; there is no browser-facing surface here.
class Agent : public Service {
 public:
  explicit Agent(const Options& options) : token_(agentToken(options.data, true)) {
    HostOptions hostOptions;
    if (options.bind != "0.0.0.0" && options.bind != "::") hostOptions.own.push_back(options.bind);
    std::shared_ptr<Hypervisor> hypervisor =
        options.demo ? std::shared_ptr<Hypervisor>(openDemo("demo")) : std::shared_ptr<Hypervisor>(openLibvirt(options.uri));
    host_ = makeLibvirtHost(hypervisor, hostOptions);
    char name[256] = "";
    gethostname(name, sizeof name - 1);
    hostname_ = name;
  }

  Response handle(Request& req, Conn& conn) override {
    std::string given = req.header("authorization");
    if (given.compare(0, 7, "Bearer ") != 0 || !constantEq(given.substr(7), token_)) {
      usleep(300000);
      return Response::error(401, "Invalid agent token");
    }
    auto parts = split(req.path, '/');  // "", "agent", "v1", ...
    if (parts.size() < 4 || parts[1] != "agent" || parts[2] != "v1") return Response::error(404, "Not found");
    if (parts.size() == 4 && parts[3] == "info" && req.method == "GET") {
      Json info = host_->info();
      info.set("hostname", hostname_);
      info.set("agent", true);
      return Response::json(info);
    }
    if (parts.size() == 4 && parts[3] == "vms" && req.method == "GET") return Response::json(obj({{"vms", host_->inventory()}}));
    if (parts.size() < 6 || parts[3] != "vms" || !isUuid(parts[4])) return Response::error(404, "Not found");
    const std::string& id = parts[4];
    if (parts.size() == 6 && parts[5] == "preview.png" && req.method == "GET")
      return Response::bytes(host_->previewPng(id, atoi(req.param("width", "720").c_str())), "image/png");
    if (parts.size() == 7 && parts[5] == "heads" && parts[6].size() == 5 && parts[6][0] >= '0' && parts[6][0] <= '3' &&
        parts[6].compare(1, 4, ".png") == 0 && req.method == "GET")
      return Response::bytes(host_->headPng(id, parts[6][0] - '0', req.param("fresh") == "1"), "image/png");
    if (parts.size() == 6 && parts[5] == "input" && req.method == "POST") {
      Json body = Json::parse(req.body);
      if (!body["head"].isInt()) fail("Monitor unavailable");
      host_->input(id, int(body["head"].integer()), body["events"]);
      return Response::json(obj({{"ok", true}}));
    }
    if (parts.size() == 6 && parts[5] == "console" && req.method == "GET") {
      if (req.header("upgrade") != "vmwatcher-console") return Response::error(400, "Expected a console upgrade");
      if (consoles_ >= 64) return Response::error(429, "Console limit");
      std::unique_ptr<Conn> console = host_->console(id);
      consoles_++;
      try {
        conn.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: vmwatcher-console\r\nConnection: Upgrade\r\n\r\n");
        bridgeRaw(conn, *console);
      } catch (const std::exception&) {
      }
      consoles_--;
      return Response::taken();
    }
    return Response::error(404, "Not found");
  }

 private:
  std::string token_, hostname_;
  std::shared_ptr<HostApi> host_;
  std::atomic<int> consoles_{0};
};

}  // namespace

std::unique_ptr<Service> makeAgent(const Options& options) { return std::unique_ptr<Service>(new Agent(options)); }

}  // namespace vmw
