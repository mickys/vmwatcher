// VM Watcher core: one dependency-free C++17 binary that runs either as the
// central service (web UI, configuration, authentication) or as a per-host agent.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace vmw {

// ---------------------------------------------------------------- util
struct HttpError : std::runtime_error {
  int status;
  HttpError(int s, const std::string& m) : std::runtime_error(m), status(s) {}
};
[[noreturn]] void fail(const std::string& message, int status = 400);

std::string hex(const std::string& bytes);
std::string base64(const std::string& bytes);
std::string randomBytes(size_t n);
std::string randomHex(size_t bytes);
std::string uuid4();
int64_t nowMs();
std::string isoTime(int64_t ms);
std::string lower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
bool isUuid(const std::string& s);
bool constantEq(const std::string& a, const std::string& b);
std::string urlDecode(const std::string& s);
bool fileExists(const std::string& path);
std::string readFile(const std::string& path, size_t max = 256u << 20);
void writeFileAtomic(const std::string& path, const std::string& data, int mode = 0600);
void makeDirs(const std::string& path, int mode = 0700);
void removeTree(const std::string& path);
std::vector<std::string> listDir(const std::string& path);
void logLine(const std::string& text);

// ---------------------------------------------------------------- json
class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;
  enum Type { Null, Bool, Number, String, Arr, Obj };
  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool v) : type_(Bool), b_(v) {}
  Json(int v) : type_(Number), n_(v) {}
  Json(int64_t v) : type_(Number), n_(double(v)) {}
  Json(size_t v) : type_(Number), n_(double(v)) {}
  Json(double v) : type_(Number), n_(v) {}
  Json(const char* v) : type_(String), s_(v) {}
  Json(std::string v) : type_(String), s_(std::move(v)) {}
  Json(Array v) : type_(Arr), a_(std::make_shared<Array>(std::move(v))) {}
  Json(Object v) : type_(Obj), o_(std::make_shared<Object>(std::move(v))) {}
  static Json parse(const std::string& text);
  std::string dump(int indent = -1) const;
  Type type() const { return type_; }
  bool isNull() const { return type_ == Null; }
  bool isBool() const { return type_ == Bool; }
  bool isNumber() const { return type_ == Number; }
  bool isString() const { return type_ == String; }
  bool isArray() const { return type_ == Arr; }
  bool isObject() const { return type_ == Obj; }
  bool isInt() const;
  bool boolean(bool fallback = false) const { return type_ == Bool ? b_ : fallback; }
  double num(double fallback = 0) const { return type_ == Number ? n_ : fallback; }
  int64_t integer(int64_t fallback = 0) const { return isInt() ? int64_t(n_) : fallback; }
  const std::string& str() const;
  std::string str(const std::string& fallback) const { return type_ == String ? s_ : fallback; }
  const Array& arr() const;
  const Object& obj() const;
  // Missing keys read as null, so lookups chain without checks.
  const Json& operator[](const std::string& key) const;
  bool has(const std::string& key) const;
  Json& set(const std::string& key, Json value);
  void push(Json value);
  void erase(const std::string& key);

 private:
  void own();
  Type type_ = Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::shared_ptr<Array> a_;
  std::shared_ptr<Object> o_;
  void write(std::string& out, int indent, int depth) const;
};

inline Json obj(std::initializer_list<std::pair<const std::string, Json>> items) { return Json(Json::Object(items)); }

// ---------------------------------------------------------------- crypto
std::string sha1(const std::string& data);
std::string sha256(const std::string& data);
std::string hmacSha256(const std::string& key, const std::string& data);
std::string pbkdf2Sha256(const std::string& password, const std::string& salt, uint32_t iterations, size_t length = 32);
Json hashPassword(const std::string& password, uint32_t iterations = 210000);
bool verifyPassword(const Json& record, const std::string& password);

// ---------------------------------------------------------------- xml
struct XmlNode {
  std::string name, text;
  std::map<std::string, std::string> attrs;
  std::vector<XmlNode> children;
  const XmlNode* child(const std::string& n) const;
  std::vector<const XmlNode*> all(const std::string& n) const;
  std::string attr(const std::string& n, const std::string& fallback = "") const;
};
XmlNode parseXml(const std::string& text);

// ---------------------------------------------------------------- net
struct Traffic {
  std::atomic<int64_t> down{0}, up{0};
};
struct Conn {
  std::shared_ptr<Traffic> traffic;
  int fd = -1;
  std::string in;  // bytes received but not yet consumed
  void* tls = nullptr;  // TLS session when the listener uses HTTPS
  explicit Conn(int f) : fd(f) {}
  ~Conn();
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  void timeouts(int receiveSeconds, int sendSeconds);
  size_t fill();  // receive more into `in`; 0 on a clean close
  size_t readSome(char* buffer, size_t size);  // one read; 0 on a clean close
  bool buffered() const;  // decrypted bytes are waiting that poll() cannot see
  bool readUntil(const std::string& delimiter, size_t max, std::string& out);
  std::string readN(size_t n);
  void write(const char* data, size_t n);
  void write(const std::string& data) { write(data.data(), data.size()); }
  int release();  // hand the descriptor to the caller
};
std::unique_ptr<Conn> connectTcp(const std::string& host, int port, int timeoutSeconds = 8);

struct Request {
  size_t headerBytes = 0;
  std::string method, target, path, peer;
  std::map<std::string, std::string> query, headers;  // header names lower-cased
  std::string body;
  std::string header(const std::string& name) const;
  std::string param(const std::string& name, const std::string& fallback = "") const;
};
struct Response {
  int status = 200;  // 0: the handler already used the connection; close it
  std::string type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  static Response json(const Json& value, int status = 200);
  static Response error(int status, const std::string& message);
  static Response bytes(std::string body, const std::string& type);
  static Response taken() { Response r; r.status = 0; return r; }
};
bool readRequest(Conn& conn, Request& req, size_t maxBody = 1 << 20);
void writeResponse(Conn& conn, const Response& res, bool keepAlive);

class Server {
 public:
  using Handler = std::function<Response(Request&, Conn&)>;
  explicit Server(Handler handler) : handler_(std::move(handler)) {}
  ~Server();
  int listen(const std::string& bind, int port);  // returns the bound port
  void enableTls(const std::string& certificateChainFile, const std::string& privateKeyFile);
  void run();    // accept loop; returns after stop()
  void start();  // run() on a background thread
  void stop();
  int maxConnections = 512;

 private:
  void serve(int fd, std::string peer);
  Handler handler_;
  void* tlsContext_ = nullptr;
  int listenFd_ = -1;
  std::atomic<bool> stopping_{false};
  std::atomic<int> active_{0};
  std::mutex mutex_;
  std::map<int, bool> fds_;
  std::unique_ptr<std::thread> thread_;
};

struct Url {
  std::string host;
  int port = 0;
};
Url parseHttpUrl(const std::string& url);
using Headers = std::vector<std::pair<std::string, std::string>>;
Response httpCall(const Url& base, const std::string& method, const std::string& path, const Headers& headers = {},
                  const std::string& body = "", int timeoutSeconds = 20, size_t maxBody = 96u << 20);
// Ask the peer to switch to a raw byte stream; returns the connection after "101".
std::unique_ptr<Conn> httpUpgrade(const Url& base, const std::string& path, const Headers& headers, const std::string& protocol);

struct WsMessage {
  int opcode = 0;
  std::string data;
};
struct WsParser {
  explicit WsParser(size_t max = 16u << 20) : max(max) {}
  size_t max;
  std::string buffer, fragment;
  int fragmentOpcode = 0;
  bool next(WsMessage& out);  // throws on protocol violations
};
std::string wsFrame(int opcode, const char* data, size_t size);
inline std::string wsFrame(int opcode, const std::string& data) { return wsFrame(opcode, data.data(), data.size()); }
// Validates the upgrade request and writes the "101" reply. `protocol` may be empty.
void wsAccept(Conn& conn, const Request& req, const std::string& protocol);

// ---------------------------------------------------------------- image
struct Image {
  int width = 0, height = 0;
  std::string rgb;  // width * height * 3
};
Image decodePpm(const std::string& data);
Image decodePng(const std::string& data);
Image decodeScreenshot(const std::string& data);
std::string encodePng(const Image& image, int level = 2);
Image resizeImage(const Image& image, int maxWidth);
Image combineImages(const std::vector<Image>& images);
std::string zipStore(const std::vector<std::pair<std::string, std::string>>& files);
uint32_t crc32Of(const std::string& data);

// ---------------------------------------------------------------- rfb
// Fail-closed client-message filter: viewers may request frames, never inject
// input, clipboard, resize or power operations. RFB 3.3/3.7/3.8, None/VNC auth.
class ReadOnlyRfb {
 public:
  void observeServer(const char* data, size_t n);
  std::string push(const std::string& data);

 private:
  void selectSecurity(int type);
  enum State { Version, Security33, Security, Auth, Init, Messages } state_ = Version;
  std::string buffer_, server_;
  long security33_ = -2;  // -2: unknown yet, -1: not an RFB 3.3 server
};
// QEMU VNC audio extension client: requests audio only on a shared session.
class AudioRfb {
 public:
  std::function<void(const std::string&)> write, pcm;
  std::function<void()> ready;
  void push(const char* data, size_t n);
  bool enabled = false;

 private:
  enum State { Version, Security, Result, Init, Messages } state_ = Version;
  std::string buffer_;
};

// ---------------------------------------------------------------- input
struct Display {
  int width = 0, height = 0;
};
Json inputEvents(const Json& events);
Json desktopInputEvents(int head, const Json& events, const std::vector<Display>& displays);
std::vector<Json> inputFrames(const Json& events);

// ---------------------------------------------------------------- hypervisor
struct DomainInfo {
  std::string uuid, name, state, xml;
};
struct Hypervisor {
  virtual ~Hypervisor() = default;
  virtual std::vector<DomainInfo> list() = 0;
  virtual std::string screenshot(const std::string& uuid, int screen) = 0;
  virtual std::string monitorCommand(const std::string& uuid, const std::string& command) = 0;
};
std::unique_ptr<Hypervisor> openLibvirt(const std::string& uri);
std::unique_ptr<Hypervisor> openDemo(const std::string& label);
bool libvirtAvailable(std::string* why = nullptr);

struct Video {
  std::string alias;
  int heads = 1;
};
struct Vm {
  std::string id, name, state;
  int heads = 1;
  bool vnc = false;
  std::string vncHost;
  int vncPort = 0;
  std::vector<Video> videos;
};
Vm parseDomain(const std::string& xml, const std::string& state);

// ---------------------------------------------------------------- host
// Everything the central service needs from one hypervisor host. Implemented
// in-process over libvirt, and remotely by talking to an agent.
struct HostApi {
  virtual ~HostApi() = default;
  virtual Json info() = 0;
  virtual Json inventory() = 0;  // array of VM objects
  virtual std::string headPng(const std::string& id, int head, bool fresh) = 0;
  virtual std::string previewPng(const std::string& id, int width) = 0;
  virtual void input(const std::string& id, int head, const Json& events) = 0;
  virtual std::unique_ptr<Conn> console(const std::string& id) = 0;
};
Json videoEncoders();
void streamVideo(Conn&, HostApi&, const std::string& id, const std::string& choice);
struct HostOptions {
  bool local = true;            // libvirt runs on this machine
  std::string remoteHost;       // hypervisor address for a remote libvirt URI
  std::vector<std::string> own; // extra addresses that belong to this machine
  int inventoryTtlMs = 10000;
};
// A keyboard/mouse device driven directly by the agent (physical.json "hid" object).
struct HidDevice {
  virtual ~HidDevice() = default;
  // `events` is validated inputEvents() output for display `head`.
  virtual void apply(int head, const Json& events) = 0;
};
std::shared_ptr<HidDevice> makeHidDevice(const Json& config);
int hidUsageOfQcode(const std::string& qcode);  // 0 when unmapped
std::shared_ptr<HostApi> makePhysicalHost(const Json& config);
std::shared_ptr<HostApi> makeLibvirtHost(std::shared_ptr<Hypervisor> hypervisor, HostOptions options);
std::shared_ptr<HostApi> makeAgentHost(const std::string& url, const std::string& token);
std::string remoteHostOfUri(const std::string& uri);
// Why a console cannot be proxied ("" when it can); `target` receives the address to dial.
std::string consoleTarget(const Vm& vm, const HostOptions& options, std::string* target);

// ---------------------------------------------------------------- bridges
// Both run on the calling thread until either side closes.
void bridgeConsole(Conn& browser, Conn& console, bool readOnly);
void bridgeAudio(Conn& browser, Conn& console);
void bridgeRaw(Conn& a, Conn& b);

// ---------------------------------------------------------------- services
struct Options {
  std::string bind = "127.0.0.1";
  int port = 6088;
  std::string data = "./data";
  std::string web = "./web";
  std::string uri = "qemu:///system";
  std::vector<std::string> allowedHosts;
  std::string tlsCert, tlsKey;  // both set: serve HTTPS
  bool demo = false;
};
struct Service {
  virtual ~Service() = default;
  virtual Response handle(Request& req, Conn& conn) = 0;
};
std::unique_ptr<Service> makeCentral(const Options& options);
std::unique_ptr<Service> makeAgent(const Options& options);
std::string agentToken(const std::string& dataDir, bool create);
void setPassword(const std::string& dataDir, const std::string& password);
extern const char* const kVersion;

}  // namespace vmw
