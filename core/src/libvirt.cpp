// libvirt is loaded at run time: the binary needs no libvirt headers to build
// and still starts (as a central service for agents only) where it is absent.
#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "vmw.hpp"

namespace vmw {
namespace {

struct Api {
  void* (*connectOpen)(const char*) = nullptr;
  int (*connectClose)(void*) = nullptr;
  int (*connectIsAlive)(void*) = nullptr;
  int (*connectListAllDomains)(void*, void***, unsigned) = nullptr;
  void* (*domainLookupByUUIDString)(void*, const char*) = nullptr;
  int (*domainFree)(void*) = nullptr;
  int (*domainGetUUIDString)(void*, char*) = nullptr;
  const char* (*domainGetName)(void*) = nullptr;
  int (*domainGetState)(void*, int*, int*, unsigned) = nullptr;
  char* (*domainGetXMLDesc)(void*, unsigned) = nullptr;
  char* (*domainScreenshot)(void*, void*, unsigned, unsigned) = nullptr;
  void* (*streamNew)(void*, unsigned) = nullptr;
  int (*streamRecv)(void*, char*, size_t) = nullptr;
  int (*streamFinish)(void*) = nullptr;
  int (*streamAbort)(void*) = nullptr;
  int (*streamFree)(void*) = nullptr;
  const char* (*getLastErrorMessage)() = nullptr;
  void (*setErrorFunc)(void*, void (*)(void*, void*)) = nullptr;
  int (*initialize)() = nullptr;
  int (*qemuMonitorCommand)(void*, const char*, char**, unsigned) = nullptr;
  std::string problem;
};

void quiet(void*, void*) {}

const Api& api() {
  static Api instance = [] {
    Api a;
    void* lib = dlopen("libvirt.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
      a.problem = "libvirt is not installed on this machine (libvirt.so.0 not found)";
      return a;
    }
    bool complete = true;
    auto load = [&](void* from, const char* name) {
      void* symbol = from ? dlsym(from, name) : nullptr;
      if (!symbol) complete = false;
      return symbol;
    };
#define VMW_LOAD(field, name) a.field = reinterpret_cast<decltype(a.field)>(load(lib, name))
    VMW_LOAD(connectOpen, "virConnectOpen");
    VMW_LOAD(connectClose, "virConnectClose");
    VMW_LOAD(connectIsAlive, "virConnectIsAlive");
    VMW_LOAD(connectListAllDomains, "virConnectListAllDomains");
    VMW_LOAD(domainLookupByUUIDString, "virDomainLookupByUUIDString");
    VMW_LOAD(domainFree, "virDomainFree");
    VMW_LOAD(domainGetUUIDString, "virDomainGetUUIDString");
    VMW_LOAD(domainGetName, "virDomainGetName");
    VMW_LOAD(domainGetState, "virDomainGetState");
    VMW_LOAD(domainGetXMLDesc, "virDomainGetXMLDesc");
    VMW_LOAD(domainScreenshot, "virDomainScreenshot");
    VMW_LOAD(streamNew, "virStreamNew");
    VMW_LOAD(streamRecv, "virStreamRecv");
    VMW_LOAD(streamFinish, "virStreamFinish");
    VMW_LOAD(streamAbort, "virStreamAbort");
    VMW_LOAD(streamFree, "virStreamFree");
    VMW_LOAD(getLastErrorMessage, "virGetLastErrorMessage");
    VMW_LOAD(setErrorFunc, "virSetErrorFunc");
    VMW_LOAD(initialize, "virInitialize");
#undef VMW_LOAD
    void* qemu = dlopen("libvirt-qemu.so.0", RTLD_NOW | RTLD_GLOBAL);
    a.qemuMonitorCommand = reinterpret_cast<decltype(a.qemuMonitorCommand)>(load(qemu, "virDomainQemuMonitorCommand"));
    if (!complete) {
      a.problem = "The installed libvirt is too old or incomplete";
      return a;
    }
    a.initialize();
    a.setErrorFunc(nullptr, quiet);  // errors are reported through exceptions instead of stderr
    return a;
  }();
  return instance;
}

const char* stateName(int state) {
  static const char* names[] = {"no state", "running", "idle", "paused", "in shutdown", "shut off", "crashed", "pmsuspended"};
  return state >= 0 && state < 8 ? names[state] : "unknown";
}

class Libvirt : public Hypervisor {
 public:
  explicit Libvirt(std::string uri) : uri_(std::move(uri)) {
    if (!api().problem.empty()) throw std::runtime_error(api().problem);
  }
  ~Libvirt() override {
    if (conn_) api().connectClose(conn_);
  }

  std::vector<DomainInfo> list() override {
    std::lock_guard<std::mutex> lock(mutex_);
    void* conn = connection();
    void** domains = nullptr;
    int count = api().connectListAllDomains(conn, &domains, 0);
    if (count < 0) broken("Cannot list VMs");
    std::vector<DomainInfo> out;
    for (int i = 0; i < count; i++) {
      DomainInfo info;
      char uuid[40] = "";
      int state = 0, reason = 0;
      if (api().domainGetUUIDString(domains[i], uuid) == 0) {
        info.uuid = uuid;
        const char* name = api().domainGetName(domains[i]);
        info.name = name ? name : uuid;
        info.state = api().domainGetState(domains[i], &state, &reason, 0) == 0 ? stateName(state) : "unavailable";
        if (char* xml = api().domainGetXMLDesc(domains[i], 0)) {
          info.xml = xml;
          free(xml);
        } else
          info.state = "unavailable";
        out.push_back(std::move(info));
      }
      api().domainFree(domains[i]);
    }
    free(domains);
    return out;
  }

  std::string screenshot(const std::string& uuid, int screen) override {
    // Screenshots run concurrently: libvirt connections are thread-safe, and
    // only the connection pointer itself is guarded.
    void* conn;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      conn = connection();
    }
    void* domain = api().domainLookupByUUIDString(conn, uuid.c_str());
    if (!domain) throw std::runtime_error("VM not found");
    void* stream = api().streamNew(conn, 0);
    if (!stream) {
      api().domainFree(domain);
      throw std::runtime_error("Screenshot stream unavailable");
    }
    std::string out, problem;
    char* mime = api().domainScreenshot(domain, stream, unsigned(screen), 0);
    if (!mime) problem = last("Screenshot failed");
    else {
      free(mime);
      std::vector<char> buffer(256 * 1024);
      for (;;) {
        int n = api().streamRecv(stream, buffer.data(), buffer.size());
        if (n == 0) break;
        if (n < 0) {
          problem = last("Screenshot transfer failed");
          break;
        }
        out.append(buffer.data(), size_t(n));
        if (out.size() > (64u << 20)) {
          problem = "Screenshot too large";
          break;
        }
      }
    }
    if (problem.empty()) api().streamFinish(stream);
    else api().streamAbort(stream);
    api().streamFree(stream);
    api().domainFree(domain);
    if (!problem.empty()) throw std::runtime_error(problem);
    return out;
  }

  std::string monitorCommand(const std::string& uuid, const std::string& command) override {
    void* conn;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      conn = connection();
    }
    void* domain = api().domainLookupByUUIDString(conn, uuid.c_str());
    if (!domain) throw std::runtime_error("VM not found");
    char* result = nullptr;
    int rc = api().qemuMonitorCommand(domain, command.c_str(), &result, 0);
    std::string out = result ? result : "";
    free(result);
    api().domainFree(domain);
    if (rc != 0) throw std::runtime_error(last("Input rejected"));
    return out;
  }

 private:
  std::string last(const char* fallback) {
    const char* message = api().getLastErrorMessage();
    return message && *message ? std::string(fallback) + ": " + message : fallback;
  }
  [[noreturn]] void broken(const char* what) {
    std::string message = last(what);
    if (conn_ && api().connectIsAlive(conn_) != 1) {
      api().connectClose(conn_);
      conn_ = nullptr;
    }
    throw std::runtime_error(message);
  }
  // Call with mutex_ held. Reconnects after libvirtd restarts or a link drops.
  void* connection() {
    if (conn_ && api().connectIsAlive(conn_) != 1) {
      api().connectClose(conn_);
      conn_ = nullptr;
    }
    if (!conn_) {
      if (nowMs() < retryAt_) throw std::runtime_error(lastFailure_);
      conn_ = api().connectOpen(uri_.c_str());
      if (!conn_) {
        lastFailure_ = last("Cannot connect to libvirt");
        retryAt_ = nowMs() + 5000;
        throw std::runtime_error(lastFailure_);
      }
    }
    return conn_;
  }
  std::string uri_, lastFailure_;
  std::mutex mutex_;
  void* conn_ = nullptr;
  int64_t retryAt_ = 0;
};

// Synthetic VMs with moving test patterns: lets the whole stack run on a
// machine without libvirt. Enabled only by the --demo flag.
class Demo : public Hypervisor {
 public:
  explicit Demo(std::string label) : label_(std::move(label)) {}
  std::vector<DomainInfo> list() override {
    std::vector<DomainInfo> out;
    for (int i = 0; i < 5; i++) {
      DomainInfo d;
      d.uuid = id(i);
      d.name = label_ + "-vm" + std::to_string(i + 1);
      d.state = i == 4 ? "shut off" : i == 3 ? "paused" : "running";
      int heads = i == 1 ? 2 : 1;
      d.xml = "<domain type='kvm'><name>" + d.name + "</name><uuid>" + d.uuid + "</uuid><devices><video><model type='virtio' heads='" +
              std::to_string(heads) + "'/><alias name='video0'/></video></devices></domain>";
      out.push_back(d);
    }
    return out;
  }
  std::string screenshot(const std::string& uuid, int screen) override {
    int index = -1;
    for (int i = 0; i < 5; i++)
      if (id(i) == uuid) index = i;
    if (index < 0 || index == 4) throw std::runtime_error("VM is not running");
    int width = screen ? 1024 : 1280, height = screen ? 768 : 800;
    std::string out = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    size_t start = out.size();
    out.resize(start + size_t(width) * size_t(height) * 3);
    int phase = int((nowMs() / 250) % 4096);
    uint8_t* p = reinterpret_cast<uint8_t*>(&out[start]);
    int bar = (phase * 9) % width;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++, p += 3) {
        bool grid = x % 80 == 0 || y % 80 == 0, sweep = abs(x - bar) < 6;
        p[0] = uint8_t(sweep ? 255 : grid ? 70 : 20 + index * 28 + x * 60 / width);
        p[1] = uint8_t(sweep ? 255 : grid ? 80 : 30 + screen * 60 + y * 70 / height);
        p[2] = uint8_t(sweep ? 255 : grid ? 95 : 60 + ((x + y + phase * 4) & 63));
      }
    return out;
  }
  std::string monitorCommand(const std::string&, const std::string&) override { return "{\"return\":{}}"; }

 private:
  std::string id(int i) const {
    std::string h = hex(sha256("vmwatcher-demo:" + label_ + ":" + std::to_string(i)));
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-4" + h.substr(13, 3) + "-8" + h.substr(17, 3) + "-" + h.substr(20, 12);
  }
  std::string label_;
};

}  // namespace

bool libvirtAvailable(std::string* why) {
  if (why) *why = api().problem;
  return api().problem.empty();
}
std::unique_ptr<Hypervisor> openLibvirt(const std::string& uri) { return std::unique_ptr<Hypervisor>(new Libvirt(uri)); }
std::unique_ptr<Hypervisor> openDemo(const std::string& label) { return std::unique_ptr<Hypervisor>(new Demo(label)); }

}  // namespace vmw
