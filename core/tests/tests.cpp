#include <cmath>
#include <mutex>
#include <atomic>
#include <netinet/in.h>
#include <arpa/inet.h>
// Unit and integration tests. No test framework: each check reports its line.
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "vmw.hpp"

using namespace vmw;

static int failures = 0, checks = 0;
#define CHECK(condition)                                                    \
  do {                                                                      \
    checks++;                                                               \
    if (!(condition)) {                                                     \
      failures++;                                                           \
      fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
    }                                                                       \
  } while (0)
#define THROWS(expression)                                                    \
  do {                                                                        \
    checks++;                                                                 \
    bool threw = false;                                                       \
    try {                                                                     \
      (void)(expression);                                                     \
    } catch (const std::exception&) {                                         \
      threw = true;                                                           \
    }                                                                         \
    if (!threw) {                                                             \
      failures++;                                                             \
      fprintf(stderr, "FAIL %s:%d  expected a throw: %s\n", __FILE__, __LINE__, #expression); \
    }                                                                         \
  } while (0)

static std::string bytes(std::initializer_list<int> list) {
  std::string out;
  for (int v : list) out += char(v);
  return out;
}

static void testCrypto() {
  CHECK(hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(sha256(std::string(1000, 'a'))) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
  CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  // RFC 4231 test case 2
  CHECK(hex(hmacSha256("Jefe", "what do ya want for nothing?")) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // RFC 7914 section 11
  CHECK(hex(pbkdf2Sha256("passwd", "salt", 1, 64)) ==
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
  CHECK(hex(pbkdf2Sha256("password", "salt", 4096, 32)) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
  Json record = hashPassword("correct horse", 2000);
  CHECK(verifyPassword(record, "correct horse"));
  CHECK(!verifyPassword(record, "correct horsf"));
  CHECK(!verifyPassword(Json(), "x"));
  CHECK(base64("any carnal pleas") == "YW55IGNhcm5hbCBwbGVhcw==");
  CHECK(base64("any carnal pleasu") == "YW55IGNhcm5hbCBwbGVhc3U=");
  CHECK(isUuid(uuid4()) && uuid4() != uuid4());
}

static void testJson() {
  Json v = Json::parse(R"({"a":[1,2.5,"x\n\u00e9\ud83d\ude00"],"b":{"c":true,"d":null},"e":-3})");
  CHECK(v["a"].arr().size() == 3 && v["a"].arr()[0].integer() == 1 && v["a"].arr()[1].num() == 2.5);
  CHECK(v["a"].arr()[2].str() == "x\n\xc3\xa9\xf0\x9f\x98\x80");
  CHECK(v["b"]["c"].boolean() && v["b"]["d"].isNull() && v["missing"]["deeper"].isNull());
  CHECK(v["e"].integer() == -3 && !v["a"].arr()[1].isInt());
  CHECK(Json::parse(v.dump()).dump() == v.dump());
  CHECK(Json::parse(v.dump(2)).dump() == v.dump());
  CHECK(obj({{"k", "a\"b\\\x01"}}).dump() == R"({"k":"a\"b\\\u0001"})");
  Json copy = v;
  copy.set("e", 9);
  CHECK(v["e"].integer() == -3 && copy["e"].integer() == 9);
  for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "nul", "1 2", "\"\\x\"", "{\"a\" 1}", "[\"\x01\"]"}) THROWS(Json::parse(bad));
  THROWS(Json::parse(std::string(200, '[')));
}

static const char* kDomain = R"(<?xml version="1.0"?>
<!-- comment -->
<domain type='kvm' id='3'>
  <name>lab &amp; test</name>
  <uuid>0B5F1C4E-1111-4222-8333-444455556666</uuid>
  <devices>
    <graphics type='vnc' port='5903' autoport='yes' listen='127.0.0.1' sharePolicy='ignore'><listen type='address' address='127.0.0.1'/></graphics>
    <video><model type='virtio' heads='2' primary='yes'/><alias name='video0'/></video>
    <video><model type='qxl'/><alias name='video1'/></video>
  </devices>
</domain>)";

static void testDomain() {
  Vm vm = parseDomain(kDomain, "running");
  CHECK(vm.id == "0b5f1c4e-1111-4222-8333-444455556666" && vm.name == "lab & test");
  CHECK(vm.heads == 3 && vm.videos.size() == 2 && vm.videos[0].alias == "video0" && vm.videos[1].heads == 1);
  CHECK(vm.vnc && vm.vncPort == 5903 && vm.vncHost == "127.0.0.1");
  THROWS(parseDomain("<domain><uuid>nope</uuid></domain>", "running"));
  THROWS(parseDomain("<domain><name>x</name>", "running"));
  Vm none = parseDomain("<domain><name>x</name><uuid>0b5f1c4e-1111-4222-8333-444455556666</uuid></domain>", "shut off");
  CHECK(!none.vnc && none.heads == 1);

  // Where the proxy may dial: never off the hypervisor for a local host.
  HostOptions local;
  local.own = {"192.0.2.3"};
  std::string target;
  CHECK(consoleTarget(vm, local, &target).empty() && target == "127.0.0.1");
  vm.vncHost = "0.0.0.0";
  CHECK(consoleTarget(vm, local, &target).empty() && target == "127.0.0.1");
  vm.vncHost = "192.0.2.3";
  CHECK(consoleTarget(vm, local, &target).empty() && target == "192.0.2.3");
  vm.vncHost = "10.9.9.9";
  CHECK(!consoleTarget(vm, local, &target).empty());
  vm.vncHost = "127.0.0.1";
  vm.vncPort = 80;
  CHECK(!consoleTarget(vm, local, &target).empty());
  vm.vncPort = 5903;
  HostOptions remote;
  remote.local = false;
  remote.remoteHost = remoteHostOfUri("qemu+ssh://root@hv2.lan:2222/system?keyfile=/k");
  CHECK(remote.remoteHost == "hv2.lan");
  CHECK(consoleTarget(vm, remote, &target).find("agent") != std::string::npos);
  vm.vncHost = "0.0.0.0";
  CHECK(consoleTarget(vm, remote, &target).empty() && target == "hv2.lan");
  CHECK(remoteHostOfUri("qemu:///system").empty() && remoteHostOfUri("qemu+tls://[fd00::2]/system") == "fd00::2");
}

static void testImages() {
  Image image;
  image.width = 5;
  image.height = 3;
  for (int i = 0; i < 45; i++) image.rgb += char(i * 5 + 3);
  std::string ppm = "P6\n# comment\n5 3\n255\n" + image.rgb;
  Image decoded = decodePpm(ppm);
  CHECK(decoded.width == 5 && decoded.height == 3 && decoded.rgb == image.rgb);
  THROWS(decodePpm("P6\n5 3\n255\n" + image.rgb.substr(1)));
  THROWS(decodePpm("P5\n5 3\n255\n" + image.rgb));
  THROWS(decodePpm("P6\n99999 99999\n255\n"));
  std::string png = encodePng(image);
  CHECK(png.compare(0, 4, "\x89PNG") == 0);
  Image back = decodePng(png);
  CHECK(back.width == 5 && back.height == 3 && back.rgb == image.rgb);
  CHECK(decodeScreenshot(png).rgb == image.rgb && decodeScreenshot(ppm).rgb == image.rgb);
  THROWS(decodePng(png.substr(0, png.size() - 20)));
  THROWS(decodeScreenshot("GIF89a"));
  Image big;
  big.width = 400;
  big.height = 200;
  big.rgb.assign(400 * 200 * 3, char(200));
  Image small = resizeImage(big, 100);
  CHECK(small.width == 100 && small.height == 50 && uint8_t(small.rgb[77]) == 200);
  CHECK(resizeImage(big, 800).width == 400);
  Image both = combineImages({big, small});
  CHECK(both.width == 508 && both.height == 200);
  CHECK(uint8_t(both.rgb[(60 * 508 + 450) * 3]) == 24 && uint8_t(both.rgb[(10 * 508 + 450) * 3]) == 200);
  std::string zip = zipStore({{"a.txt", "hello"}, {"b.bin", std::string(3, '\0')}});
  CHECK(zip.compare(0, 4, "PK\x03\x04") == 0 && zip.find("a.txt") != std::string::npos && crc32Of("hello") == 0x3610a686u);
  CHECK(zip.compare(zip.size() - 22, 4, "PK\x05\x06") == 0 && uint8_t(zip[zip.size() - 12]) == 2);
}

static void testInput() {
  Json events = Json::parse(R"([{"type":"abs","data":{"axis":"x","value":32767,"extra":1}},
    {"type":"btn","data":{"button":"left","down":true}},{"type":"btn","data":{"button":"left","down":false}},
    {"type":"key","data":{"down":true,"key":{"type":"qcode","data":"ctrl"}}}])");
  Json safe = inputEvents(events);
  CHECK(safe.arr().size() == 4 && !safe.arr()[0]["data"].has("extra"));
  for (const char* bad : {R"([])", R"([{"type":"abs","data":{"axis":"z","value":1}}])", R"([{"type":"abs","data":{"axis":"x","value":40000}}])",
                          R"([{"type":"btn","data":{"button":"side","down":true}}])",
                          R"([{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"A B"}}}])",
                          R"([{"type":"key","data":{"down":true,"key":{"type":"number","data":"30"}}}])", R"({"type":"abs"})"})
    THROWS(inputEvents(Json::parse(bad)));
  // Two monitors side by side: the right edge of monitor 2 is the right edge of the desktop.
  std::vector<Display> displays = {{1280, 800}, {1024, 768}};
  Json mapped = desktopInputEvents(1, events, displays);
  CHECK(mapped.arr()[0]["data"]["value"].integer() == 32767);
  Json origin = desktopInputEvents(1, Json::parse(R"([{"type":"abs","data":{"axis":"x","value":0}},{"type":"abs","data":{"axis":"y","value":32767}}])"), displays);
  CHECK(origin.arr()[0]["data"]["value"].integer() == 18212);  // 1280 / 2303 * 32767
  CHECK(origin.arr()[1]["data"]["value"].integer() == 31455);  // 767 / 799 * 32767
  THROWS(desktopInputEvents(2, events, displays));
  // A press and release of the same button never share one HID report.
  auto frames = inputFrames(safe);
  CHECK(frames.size() == 2 && frames[0].arr().size() == 2 && frames[1].arr().size() == 2);
}

static void testRfb() {
  ReadOnlyRfb filter;
  std::string hello = "RFB 003.008\n";
  filter.observeServer(hello.data(), hello.size());
  CHECK(filter.push("RFB 003.008\n") == "RFB 003.008\n");
  CHECK(filter.push(bytes({1})) == bytes({1}));     // security: None
  CHECK(filter.push(bytes({0})) == bytes({1}));     // exclusive request rewritten to shared
  std::string request = bytes({3, 1, 0, 0, 0, 0, 0, 10, 0, 10});
  std::string key = bytes({4, 1, 0, 0, 0, 0, 0, 65}), pointer = bytes({5, 1, 0, 5, 0, 5});
  std::string cut = bytes({6, 0, 0, 0, 0, 0, 0, 3, 'a', 'b', 'c'});
  CHECK(filter.push(key + request + pointer + cut + request) == request + request);
  CHECK(filter.push(request.substr(0, 4)).empty() && filter.push(request.substr(4)) == request);
  CHECK(filter.push(bytes({250, 0, 1, 2})).empty());        // power control dropped
  CHECK(filter.push(bytes({255, 0, 0, 1, 0, 0, 0, 30, 0, 0, 0, 30})).empty());  // QEMU key event dropped
  THROWS(filter.push(bytes({99})));
  ReadOnlyRfb password;
  CHECK(password.push("RFB 003.008\n" + bytes({2})) == "RFB 003.008\n" + bytes({2}));
  CHECK(password.push(std::string(16, 'r')) == std::string(16, 'r'));
  CHECK(password.push(bytes({0}) + key) == bytes({1}));
  ReadOnlyRfb other;
  THROWS(other.push("RFB 003.008\n" + bytes({19})));  // VeNCrypt and friends fail closed
  ReadOnlyRfb junk;
  THROWS(junk.push("GET / HTTP/1.1"));
  ReadOnlyRfb old;  // RFB 3.3: the server names the security type
  CHECK(old.push("RFB 003.003\n" + bytes({0})) == "RFB 003.003\n");
  std::string server33 = "RFB 003.003\n" + bytes({0, 0, 0, 1});
  old.observeServer(server33.data(), server33.size());
  CHECK(old.push("") == bytes({1}));

  std::string wrote, pcm;
  bool ready = false;
  AudioRfb audio;
  audio.write = [&](const std::string& d) { wrote += d; };
  audio.pcm = [&](const std::string& d) { pcm += d; };
  audio.ready = [&] { ready = true; };
  std::string stream = "RFB 003.008\n" + bytes({1, 1}) + bytes({0, 0, 0, 0}) + std::string(20, '\0') + bytes({0, 0, 0, 2, 'v', 'm'});
  stream += bytes({0, 0, 0, 1}) + std::string(8, '\0') + bytes({0xff, 0xff, 0xfe, 0xfd});
  stream += bytes({255, 1, 0, 1}) + bytes({255, 1, 0, 2, 0, 0, 0, 4, 1, 2, 3, 4}) + bytes({255, 1, 0, 0});
  for (char c : stream) audio.push(&c, 1);  // byte at a time: framing must not depend on packet boundaries
  CHECK(ready && pcm == bytes({1, 2, 3, 4}));
  CHECK(wrote.find(bytes({2, 0, 0, 1, 0xff, 0xff, 0xfe, 0xfd})) != std::string::npos);
  CHECK(wrote.find(bytes({255, 1, 0, 2, 3, 2, 0, 0, 0xbb, 0x80})) != std::string::npos);
  AudioRfb refused;
  refused.write = [](const std::string&) {};
  std::string needsPassword = "RFB 003.008\n" + bytes({1, 2});
  THROWS(refused.push(needsPassword.data(), needsPassword.size()));
}

static std::string maskedFrame(int opcode, const std::string& payload, bool fin = true) {
  std::string out;
  out += char((fin ? 0x80 : 0) | opcode);
  if (payload.size() < 126) out += char(0x80 | payload.size());
  else {
    out += char(0x80 | 126);
    out += char(payload.size() >> 8);
    out += char(payload.size());
  }
  std::string mask = bytes({7, 99, 200, 13});
  out += mask;
  for (size_t i = 0; i < payload.size(); i++) out += char(payload[i] ^ mask[i & 3]);
  return out;
}

static void testWebSocket() {
  WsParser parser(1000);
  WsMessage message;
  std::string data = maskedFrame(2, "hello") + maskedFrame(1, "par", false) + maskedFrame(9, "p") + maskedFrame(0, "ts") +
                     maskedFrame(2, std::string(300, 'z'));
  std::vector<std::pair<int, std::string>> seen;
  for (char c : data) {
    parser.buffer += c;
    while (parser.next(message)) seen.push_back({message.opcode, message.data});
  }
  CHECK(seen.size() == 4 && seen[0].second == "hello" && seen[1].first == 9 && seen[2].first == 1 && seen[2].second == "parts" &&
        seen[3].second.size() == 300);
  WsParser unmasked;
  unmasked.buffer = bytes({0x82, 1, 'x'});
  THROWS(unmasked.next(message));
  WsParser huge(100);
  huge.buffer = maskedFrame(2, std::string(300, 'z'));
  THROWS(huge.next(message));
  CHECK(wsFrame(2, "abc") == bytes({0x82, 3, 'a', 'b', 'c'}));
  CHECK(wsFrame(2, std::string(70000, 'q')).compare(0, 10, bytes({0x82, 127, 0, 0, 0, 0, 0, 1, 0x11, 0x70})) == 0);
}

// Browser <-> console bridge over socket pairs, with a scripted VNC server.
static void testBridge() {
  int browserPair[2], consolePair[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, browserPair);
  socketpair(AF_UNIX, SOCK_STREAM, 0, consolePair);
  std::thread bridge([&] {
    Conn browser(browserPair[0]), console(consolePair[0]);
    bridgeConsole(browser, console, true);
  });
  Conn browser(browserPair[1]), server(consolePair[1]);
  browser.timeouts(5, 5);
  server.timeouts(5, 5);
  server.write("RFB 003.008\n");
  CHECK(browser.readN(14) == bytes({0x82, 12}) + "RFB 003.008\n");
  browser.write(maskedFrame(2, "RFB 003.008\n" + bytes({1, 0})));
  CHECK(server.readN(14) == "RFB 003.008\n" + bytes({1, 1}));
  std::string request = bytes({3, 0, 0, 0, 0, 0, 0, 4, 0, 4});
  browser.write(maskedFrame(2, bytes({4, 1, 0, 0, 0, 0, 0, 65}) + request));  // key press must not get through
  CHECK(server.readN(10) == request);
  browser.write(maskedFrame(9, "hi"));
  CHECK(browser.readN(4) == bytes({0x8a, 2, 'h', 'i'}));
  browser.write(maskedFrame(8, ""));
  bridge.join();
  CHECK(browser.readN(2) == bytes({0x88, 0}));
}

struct Client {
  Url url;
  std::string cookie;
  Response call(const std::string& method, const std::string& path, const std::string& body = "", bool origin = true) {
    Headers headers;
    if (origin) headers.push_back({"Origin", "http://127.0.0.1:" + std::to_string(url.port)});
    if (!cookie.empty()) headers.push_back({"Cookie", cookie});
    return httpCall(url, method, path, headers, body, 20);
  }
  Json json(const std::string& method, const std::string& path, const std::string& body = "") {
    return Json::parse(call(method, path, body).body);
  }
};

// The whole stack on loopback: browser -> central -> agent -> (demo) hypervisor.
static void testServices() {
  std::string root = "build/test-data-" + randomHex(4);
  Options agentOptions;
  agentOptions.data = root + "/agent";
  agentOptions.demo = true;
  auto agent = makeAgent(agentOptions);
  Server agentServer([&](Request& req, Conn& conn) { return agent->handle(req, conn); });
  int agentPort = agentServer.listen("127.0.0.1", 0);
  agentServer.start();
  std::string token = agentToken(agentOptions.data, false);
  CHECK(token.size() == 64);
  Url agentUrl{"127.0.0.1", agentPort};
  CHECK(httpCall(agentUrl, "GET", "/agent/v1/vms").status == 401);
  CHECK(httpCall(agentUrl, "GET", "/agent/v1/vms", {{"Authorization", "Bearer " + std::string(64, '0')}}).status == 401);

  Options options;
  options.data = root + "/central";
  options.web = root + "/web";
  options.demo = true;
  makeDirs(options.web + "/assets");
  writeFileAtomic(options.web + "/index.html", "<!doctype html><title>t</title>");
  writeFileAtomic(options.web + "/assets/app-1.js", "console.log(1)");
  auto central = makeCentral(options);
  Server server([&](Request& req, Conn& conn) { return central->handle(req, conn); });
  int port = server.listen("127.0.0.1", 0);
  server.start();
  Client c{Url{"127.0.0.1", port}, ""};

  // Static files and request trust.
  Response page = c.call("GET", "/", "", false);
  CHECK(page.status == 200 && page.body.find("<title>t</title>") != std::string::npos);
  CHECK(c.call("GET", "/assets/app-1.js", "", false).type == "text/javascript");
  CHECK(c.call("GET", "/assets/..%2f..%2fcentral/config.json", "", false).status == 404);
  CHECK(c.call("GET", "/config.json", "", false).status == 404);
  CHECK(httpCall(c.url, "GET", "/", {{"Origin", "http://evil.example"}}).status == 403);
  CHECK(httpCall(c.url, "GET", "/", {{"Sec-Fetch-Site", "cross-site"}}).status == 403);
  Headers navigation{{"Sec-Fetch-Site", "cross-site"}, {"Sec-Fetch-Mode", "navigate"},
                                                 {"Sec-Fetch-Dest", "document"}};
  CHECK(httpCall(c.url, "GET", "/", navigation).status == 200);
  CHECK(httpCall(c.url, "GET", "/api/session", navigation).status == 403);
  {
    auto raw = connectTcp("127.0.0.1", port);
    raw->write("GET / HTTP/1.1\r\nHost: evil.example\r\nConnection: close\r\n\r\n");
    std::string line;
    raw->readUntil("\r\n", 200, line);
    CHECK(line.find(" 403 ") != std::string::npos);
  }

  // First run: the password must be set before anything else works.
  CHECK(c.json("GET", "/api/session")["setupRequired"].boolean());
  CHECK(c.call("GET", "/api/vms").status == 401);
  CHECK(c.call("POST", "/api/setup", R"({"password":"short"})").status == 400);
  CHECK(c.call("POST", "/api/setup", R"({"password":"first-password"})", false).status == 403);  // no Origin
  Response setup = c.call("POST", "/api/setup", R"({"password":"first-password"})");
  CHECK(setup.status == 200);
  CHECK(c.call("POST", "/api/setup", R"({"password":"second-password"})").status == 403);
  CHECK(c.call("POST", "/api/login", R"({"password":"wrong-password"})").status == 401);
  CHECK(c.call("GET", "/api/vms").status == 401);
  Response login = httpCall(c.url, "POST", "/api/login", {{"Origin", "http://127.0.0.1:" + std::to_string(port)}},
                            R"({"password":"first-password"})");
  CHECK(login.status == 200);
  // httpCall does not expose headers; read the cookie through a raw request.
  {
    auto raw = connectTcp("127.0.0.1", port);
    std::string body = R"({"password":"first-password"})";
    raw->write("POST /api/login HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nOrigin: http://127.0.0.1:" +
               std::to_string(port) + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    std::string head;
    raw->readUntil("\r\n\r\n", 4000, head);
    size_t at = head.find("vmw_session=");
    CHECK(at != std::string::npos && head.find("HttpOnly") != std::string::npos && head.find("SameSite=Strict") != std::string::npos);
    CHECK(head.find("Max-Age=604800") != std::string::npos);
    if (at != std::string::npos) c.cookie = head.substr(at, head.find(';', at) - at);
  }
  Json session = c.json("GET", "/api/session");
  CHECK(session["authenticated"].boolean() && !session["setupRequired"].boolean() && session["settings"]["previewSeconds"].integer() == 10);

  CHECK(session["settings"]["sessionHours"].integer() == 168);
  CHECK(session["settings"]["maxLivePerSession"].integer() == 16);
  CHECK(c.call("PUT", "/api/settings", R"({"sessionHours":167})").status == 400);

  // The demo host is local; add the agent as a second host.
  Json before = c.json("GET", "/api/vms");
  CHECK(before["hosts"].arr().size() == 1 && before["vms"].arr().size() == 5);
  std::string agentBody = obj({{"name", "Rack B / hv2"}, {"type", "agent"}, {"url", "http://127.0.0.1:" + std::to_string(agentPort)}, {"token", token}}).dump();
  Json probe = c.json("POST", "/api/hosts/test", agentBody);
  CHECK(probe["ok"].boolean() && probe["vms"].integer() == 5 && probe["info"]["agent"].boolean());
  Json badProbe = c.json("POST", "/api/hosts/test", obj({{"name", "x"}, {"type", "agent"}, {"url", "http://127.0.0.1:" + std::to_string(agentPort)}, {"token", std::string(64, 'a')}}).dump());
  CHECK(!badProbe["ok"].boolean() && badProbe["error"].str().find("token") != std::string::npos);
  CHECK(c.call("POST", "/api/hosts", R"({"name":"x","type":"libvirt","uri":"qemu+ext:///system?command=/bin/sh"})").status == 400);
  CHECK(c.call("POST", "/api/hosts", R"({"name":"x","type":"libvirt","uri":"qemu+ssh://h/system?command=x"})").status == 400);
  CHECK(c.call("POST", "/api/hosts", agentBody, false).status == 403);
  Response added = c.call("POST", "/api/hosts", agentBody);
  CHECK(added.status == 201);
  Json host = Json::parse(added.body);
  CHECK(host["id"].str() == "rack-b-hv2" && host["hasToken"].boolean() && !host.has("token"));
  CHECK(c.call("GET", "/api/settings").body.find(token) == std::string::npos);  // the token is never sent back
  Json all = c.json("GET", "/api/vms");
  CHECK(all["hosts"].arr().size() == 2 && all["vms"].arr().size() == 10 && all["hosts"].arr()[1]["ok"].boolean());
  std::string agentVm, twoHeads;
  for (const Json& vm : all["vms"].arr())
    if (vm["host"].str() == "rack-b-hv2" && vm["state"].str() == "running") {
      if (agentVm.empty()) agentVm = vm["id"].str();
      if (vm["heads"].integer() == 2) twoHeads = vm["id"].str();
    }
  CHECK(!agentVm.empty() && !twoHeads.empty());

  // Images through the agent.
  std::string base = "/api/hosts/rack-b-hv2/vms/";
  Response head = c.call("GET", base + twoHeads + "/heads/1.png");
  CHECK(head.status == 200 && head.type == "image/png");
  Image second = decodePng(head.body);
  CHECK(second.width == 1024 && second.height == 768);
  Image preview = decodePng(c.call("GET", base + twoHeads + "/preview.png?width=600").body);
  CHECK(preview.width == 600 && preview.height == 208);  // (1280 + 8 + 1024) x 800 scaled
  CHECK(c.call("GET", base + twoHeads + "/heads/3.png").status == 400);
  CHECK(c.call("GET", "/api/hosts/nope/vms/" + twoHeads + "/preview.png").status == 404);
  CHECK(c.call("GET", base + "00000000-0000-4000-8000-000000000000/preview.png").status == 404);

  // Input goes browser -> central -> agent -> QMP, and can be switched off.
  std::string click = R"({"head":1,"events":[{"type":"abs","data":{"axis":"x","value":100}},{"type":"btn","data":{"button":"left","down":true}}]})";
  CHECK(c.call("POST", base + twoHeads + "/input", click).status == 200);
  CHECK(c.call("POST", base + twoHeads + "/input", R"({"head":5,"events":[{"type":"btn","data":{"button":"left","down":true}}]})").status == 400);
  CHECK(c.call("POST", base + twoHeads + "/input", click, false).status == 403);
  CHECK(c.call("PUT", "/api/settings", R"({"controlEnabled":false,"siteName":"Lab wall","previewSeconds":5})").status == 200);
  CHECK(c.call("POST", base + twoHeads + "/input", click).status == 403);
  CHECK(c.json("GET", "/api/session")["siteName"].str() == "VM Watcher");
  CHECK(c.call("PUT", "/api/settings", R"({"maxLiveTotal":9999})").status == 400);

  // A console that cannot be reached is refused before the WebSocket upgrade.
  Response live = httpCall(c.url, "GET", base + agentVm + "/vnc",
                           {{"Origin", "http://127.0.0.1:" + std::to_string(port)}, {"Cookie", c.cookie}, {"Upgrade", "websocket"},
                            {"Connection", "Upgrade"}, {"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ=="}, {"Sec-WebSocket-Version", "13"}});
  CHECK(live.status == 400 && live.body.find("no VNC console") != std::string::npos);

  // Audits: per-head PNGs, combined image and manifest in a ZIP.
  Response captured = c.call("POST", base + twoHeads + "/capture");
  CHECK(captured.status == 201);
  Json manifest = Json::parse(captured.body);
  CHECK(manifest["displays"].arr().size() == 2 && manifest["vm"]["host"].str() == "rack-b-hv2");
  Json audits = c.json("GET", "/api/audits?vm=rack-b-hv2/" + twoHeads);
  CHECK(audits["audits"].arr().size() == 1 && c.json("GET", "/api/audits?vm=demo/" + twoHeads)["audits"].arr().empty());
  std::string auditBase = "/api/audits/" + manifest["id"].str();
  Image combined = decodePng(c.call("GET", auditBase + "/combined.png").body);
  CHECK(combined.width == 2312 && combined.height == 800);
  std::string headFile = c.call("GET", auditBase + "/head-0.png").body;
  CHECK(hex(sha256(headFile)) == manifest["displays"].arr()[0]["sha256"].str());
  Response zip = c.call("GET", auditBase + "/bundle.zip");
  CHECK(zip.type == "application/zip" && zip.body.find("manifest.json") != std::string::npos && zip.body.find(headFile) != std::string::npos);
  CHECK(c.call("GET", auditBase + "/..%2f..%2fconfig.json").status == 404);

  // Groups hold host-qualified members.
  std::string member = "rack-b-hv2/" + agentVm;
  CHECK(c.call("PUT", "/api/groups/Night%20shift", obj({{"vms", Json::Array{member, member}}}).dump()).status == 200);
  Json groups = c.json("GET", "/api/groups")["groups"];
  CHECK(groups.arr().size() == 1 && groups.arr()[0]["name"].str() == "Night shift" && groups.arr()[0]["vms"].arr().size() == 1);
  CHECK(c.call("PUT", "/api/groups/bad", R"({"vms":["../../etc/passwd"]})").status == 400);
  CHECK(c.call("DELETE", "/api/groups/Night%20shift").status == 200 && c.json("GET", "/api/groups")["groups"].arr().empty());

  // Disabling and removing hosts.
  CHECK(c.call("PUT", "/api/hosts/rack-b-hv2", R"({"enabled":false})").status == 200);
  Json disabled = c.json("GET", "/api/vms");
  CHECK(disabled["vms"].arr().size() == 5 && !disabled["hosts"].arr()[1]["enabled"].boolean());
  CHECK(c.call("GET", base + agentVm + "/preview.png").status == 400);
  CHECK(c.call("PUT", "/api/hosts/rack-b-hv2", R"({"enabled":true,"name":"hv2"})").status == 200);  // blank token keeps the stored one
  CHECK(c.json("GET", "/api/vms")["vms"].arr().size() == 10);
  CHECK(c.call("DELETE", "/api/hosts/rack-b-hv2").status == 200 && c.json("GET", "/api/vms")["hosts"].arr().size() == 1);

  // Password change signs other sessions out; the configuration survives a restart.
  Client other{c.url, ""};
  {
    auto raw = connectTcp("127.0.0.1", port);
    std::string body = R"({"password":"first-password"})";
    raw->write("POST /api/login HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\nOrigin: http://localhost:" + std::to_string(port) +
               "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    std::string headText;
    raw->readUntil("\r\n\r\n", 4000, headText);
    size_t at = headText.find("vmw_session=");
    if (at != std::string::npos) other.cookie = headText.substr(at, headText.find(';', at) - at);
  }
  CHECK(other.call("GET", "/api/vms").status == 200);
  CHECK(c.call("POST", "/api/password", R"({"current":"nope-nope","password":"second-password"})").status == 403);
  CHECK(c.call("POST", "/api/password", R"({"current":"first-password","password":"second-password"})").status == 200);
  CHECK(c.call("GET", "/api/vms").status == 200 && other.call("GET", "/api/vms").status == 401);
  server.stop();
  std::unique_ptr<Server> restarted;
  auto restartCentral = [&] {
    restarted.reset();
    central = makeCentral(options);
    restarted = std::make_unique<Server>([&](Request& req, Conn& conn) { return central->handle(req, conn); });
    c.url.port = restarted->listen("127.0.0.1", 0);
    other.url.port = c.url.port;
    restarted->start();
  };
  CHECK(readFile(options.data + "/sessions.json").find(c.cookie.substr(c.cookie.find('=') + 1)) == std::string::npos);
  restartCentral();
  CHECK(c.call("GET", "/api/vms").status == 200 && other.call("GET", "/api/vms").status == 401);
  CHECK(c.call("POST", "/api/logout").status == 200 && c.call("GET", "/api/vms").status == 401);
  restartCentral();
  CHECK(c.call("GET", "/api/vms").status == 401);
  Json saved = Json::parse(readFile(options.data + "/config.json"));
  CHECK(saved["settings"]["siteName"].str() == "VM Watcher" && verifyPassword(saved["password"], "second-password"));
  CHECK(readFile(options.data + "/config.json").find("second-password") == std::string::npos);

  // Repeated wrong passwords lock the address out.
  int locked = 0;
  for (int i = 0; i < 7; i++) locked += c.call("POST", "/api/login", R"({"password":"guess-guess"})").status == 429;
  CHECK(locked >= 1);
  CHECK(c.call("POST", "/api/login", R"({"password":"second-password"})").status == 429);

  restarted.reset();
  agentServer.stop();
  removeTree(root);
}

static void testPhysicalAgent() {
  char cwd[4096];
  CHECK(getcwd(cwd, sizeof cwd) != nullptr);
  std::string root = std::string(cwd) + "/build/physical-test-" + randomHex(4);
  makeDirs(root);
  std::string capture = root + "/capture", hid = root + "/hid";
  writeFileAtomic(root + "/sample.ppm", "P6\n2 1\n255\n" + bytes({255, 0, 0, 0, 255, 0}));
  writeFileAtomic(capture, "#!/bin/sh\nexec cat \"$(dirname \"$0\")/sample.ppm\"\n");
  writeFileAtomic(hid, "#!/bin/sh\nexec cat > \"$(dirname \"$0\")/events.json\"\n");
  CHECK(chmod(capture.c_str(), 0700) == 0 && chmod(hid.c_str(), 0700) == 0);
  const std::string id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  const std::string viewOnly = "bbbbbbbb-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  Json sources = Json::Array{};
  sources.push(obj({{"id", id}, {"name", "Physical test"}, {"capture", capture}, {"hid", hid}}));
  sources.push(obj({{"id", viewOnly}, {"name", "Capture only"}, {"capture", capture}}));
  const std::string twoHeads = "cccccccc-bbbb-4ccc-8ddd-eeeeeeeeeeee";
  sources.push(obj({{"id", twoHeads}, {"name", "Two displays"}, {"capture", Json::Array{Json(capture), Json(capture)}}}));
  writeFileAtomic(root + "/physical.json", obj({{"sources", sources}}).dump());

  Options options;
  options.data = root;
  auto agent = makeAgent(options);
  Server server([&](Request& req, Conn& conn) { return agent->handle(req, conn); });
  int port = server.listen("127.0.0.1", 0);
  server.start();
  CHECK(httpCall(Url{"127.0.0.1", port}, "GET", "/agent/v1/vms").status == 401);
  auto host = makeAgentHost("http://127.0.0.1:" + std::to_string(port), agentToken(root, false));
  CHECK(host->info()["mode"].str() == "physical");
  CHECK(host->inventory().arr().size() == 3 && host->inventory().arr()[0]["kind"].str() == "physical");
  Json listed = host->inventory();
  for (const Json& vm : listed.arr())
    if (vm["id"].str() == twoHeads) CHECK(vm["heads"].integer() == 2 && vm["displays"].arr().size() == 2 && !vm["input"].boolean());
  CHECK(decodeScreenshot(host->headPng(twoHeads, 1, true)).width == 2);
  CHECK(decodeScreenshot(host->previewPng(twoHeads, 1024)).width == 2 + 8 + 2);  // side by side, 8 px gap
  THROWS(host->headPng(twoHeads, 2, false));
  CHECK(decodeScreenshot(host->headPng(id, 0, true)).rgb == bytes({255, 0, 0, 0, 255, 0}));
  CHECK(decodeScreenshot(host->previewPng(id, 128)).width == 2);
  THROWS(host->headPng(id, 1, false));
  Json events = Json::parse(R"([{"type":"abs","data":{"axis":"x","value":1234}}])");
  THROWS(host->input(id, 0, Json::parse(R"([{"type":"invalid"}])")));
  CHECK(!fileExists(root + "/events.json"));
  host->input(id, 0, events);
  CHECK(Json::parse(readFile(root + "/events.json"))["events"].dump() == events.dump());
  THROWS(host->input(viewOnly, 0, events));
  server.stop();
  removeTree(root);
}

// A fake ESP32-S2 dongle on loopback: checks the HMAC handshake and records commands.
struct FakeDongle {
  int listenFd = -1, port = 0;
  std::string key;
  std::mutex mutex;
  std::vector<std::string> lines;
  int sessions = 0, rejected = 0;
  std::thread thread;
  std::atomic<bool> stop{false};
  explicit FakeDongle(std::string k) : key(std::move(k)) {
    listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    listen(listenFd, 4);
    socklen_t n = sizeof a;
    getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &n);
    port = ntohs(a.sin_port);
    thread = std::thread([this] {
      while (!stop) {
        int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0) return;
        Conn c(fd);
        c.timeouts(5, 5);
        try {
          std::string challenge = randomHex(16), line;
          c.write("CHALLENGE " + challenge + "\r\n");
          if (!c.readUntil("\n", 400, line)) continue;
          if (line != "auth " + hex(hmacSha256(key, challenge))) {
            std::lock_guard<std::mutex> lock(mutex);
            rejected++;
            c.write("ERR\r\n");
            continue;
          }
          c.write("OK\r\n");
          {
            std::lock_guard<std::mutex> lock(mutex);
            sessions++;
          }
          while (c.readUntil("\n", 400, line)) {
            {
              std::lock_guard<std::mutex> lock(mutex);
              lines.push_back(line);
            }
            c.write(line == "down 999" ? "ERR\r\n" : "OK\r\n");
          }
        } catch (const std::exception&) {
        }
      }
    });
  }
  ~FakeDongle() {
    stop = true;
    shutdown(listenFd, SHUT_RDWR);
    close(listenFd);
    thread.join();
  }
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> lock(mutex);
    auto out = lines;
    lines.clear();
    return out;
  }
};

static void testS2Kvm() {
  CHECK(hidUsageOfQcode("a") == 4 && hidUsageOfQcode("z") == 29 && hidUsageOfQcode("1") == 30 && hidUsageOfQcode("0") == 39);
  CHECK(hidUsageOfQcode("ret") == 40 && hidUsageOfQcode("f1") == 58 && hidUsageOfQcode("f12") == 69 && hidUsageOfQcode("kp_1") == 89);
  CHECK(hidUsageOfQcode("kp_0") == 98 && hidUsageOfQcode("meta_l") == 227 && hidUsageOfQcode("shift_r") == 229);
  CHECK(hidUsageOfQcode("nonsense") == 0 && hidUsageOfQcode("f99") == 0);

  char cwd[4096];
  CHECK(getcwd(cwd, sizeof cwd) != nullptr);
  std::string root = std::string(cwd) + "/build/kvm-test-" + randomHex(4);
  makeDirs(root);
  std::string key = randomHex(32);
  writeFileAtomic(root + "/kvm.key", key + "\n");
  FakeDongle dongle(key);
  Json config = obj({{"type", "esp32-s2-kvm"}, {"address", "127.0.0.1"}, {"port", dongle.port}, {"keyFile", root + "/kvm.key"},
                     {"screen", Json::Array{Json(1000), Json(500)}}});
  THROWS(makeHidDevice(obj({{"type", "nope"}})));
  THROWS(makeHidDevice(obj({{"type", "esp32-s2-kvm"}, {"address", "127.0.0.1"}, {"keyFile", "relative"}})));
  auto device = makeHidDevice(config);

  // Keys go out as HID usages on one authenticated session.
  device->apply(0, inputEvents(Json::parse(R"([{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"shift"}}},
    {"type":"key","data":{"down":true,"key":{"type":"qcode","data":"a"}}},{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"a"}}}])")));
  CHECK((dongle.take() == std::vector<std::string>{"down 225", "down 4", "up 4"}));
  // The first pointer move homes to the corner, then moves in steps of at most 127.
  device->apply(0, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":32767}},{"type":"abs","data":{"axis":"y","value":16384}}])")));
  auto moves = dongle.take();
  int homes = 0, sx = 0, sy = 0;
  for (const auto& line : moves) {
    if (line == "mouse -127 -127 0 0 0") { homes++; continue; }
    int dx, dy, w, p, b;
    CHECK(sscanf(line.c_str(), "mouse %d %d %d %d %d", &dx, &dy, &w, &p, &b) == 5 && std::abs(dx) <= 127 && std::abs(dy) <= 127);
    sx += dx;
    sy += dy;
  }
  CHECK(homes == 11 && sx == 999 && sy == 250);  // ceil(1000/127) + 3 homing steps
  // A click re-homes first (pointer acceleration drifts), then presses and releases.
  device->apply(0, inputEvents(Json::parse(R"([{"type":"btn","data":{"button":"left","down":true}},{"type":"btn","data":{"button":"left","down":false}},
    {"type":"btn","data":{"button":"wheel-down","down":true}},{"type":"btn","data":{"button":"wheel-down","down":false}}])")));
  auto click = dongle.take();
  CHECK(click.size() >= 4 && click[0] == "mouse -127 -127 0 0 0");
  CHECK(click[click.size() - 3] == "mouse 0 0 0 0 1" && click[click.size() - 2] == "mouse 0 0 0 0 0" && click.back() == "mouse 0 0 -1 0 0");
  // Unmapped keys are skipped; a refused command reports an error.
  device->apply(0, inputEvents(Json::parse(R"([{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"unmapped"}}}])")));
  CHECK(dongle.take().empty());
  {
    std::lock_guard<std::mutex> lock(dongle.mutex);
    CHECK(dongle.sessions == 1 && dongle.rejected == 0);
  }
  // Holding a key keeps it alive past the dongle's 3 s release.
  device->apply(0, inputEvents(Json::parse(R"([{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"ctrl"}}}])")));
  usleep(2300000);
  auto held = dongle.take();
  CHECK(held.size() >= 2 && held[0] == "down 224" && held[1] == "mouse 0 0 0 0 0");
  // Divided model (AMI/ASUS UEFI): whole steps per 8 counts, 3 px for a small report.
  device.reset();
  dongle.take();
  Json divided = config;
  divided.set("screen", Json::Array{Json(1024), Json(768)});
  divided.set("pointer", obj({{"model", "divided"}, {"divisor", 8}, {"step", 12}, {"small", 3}}));
  auto bios = makeHidDevice(divided);
  auto travel = [](int c) { return std::abs(c) >= 8 ? (c < 0 ? -1 : 1) * std::abs(c) / 8 * 12 : (c > 0 ? 3 : c < 0 ? -3 : 0); };
  auto measure = [&](const std::vector<std::string>& got, int& px, int& py, int& homing) {
    for (const auto& line : got) {
      int dx, dy, w, p, b;
      if (sscanf(line.c_str(), "mouse %d %d %d %d %d", &dx, &dy, &w, &p, &b) != 5) continue;
      CHECK(std::abs(dx) <= 127 && std::abs(dy) <= 127);
      if (dx == -127 && dy == -127) { homing++; px = py = 0; continue; }
      px += travel(dx);
      py += travel(dy);
    }
  };
  int px = 0, py = 0, homing = 0;
  bios->apply(0, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":32767}},{"type":"abs","data":{"axis":"y","value":16384}}])")));
  measure(dongle.take(), px, py, homing);
  CHECK(homing == 9 && px == 1023 && py == 384);  // ceil(1024 / 180 per report) + 3
  bios->apply(0, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":100}},{"type":"abs","data":{"axis":"y","value":32767}}])")));
  measure(dongle.take(), px, py, homing);
  CHECK(homing == 9 && px == 3 && py == 768);  // 383 px down rounds to 32 whole steps
  bios->apply(0, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"y","value":0}}])")));
  measure(dongle.take(), px, py, homing);
  CHECK(py == 0);  // tracking clamped to 767 at the edge, so the way back is exact
  // Without rehomeOnClick (the divided default) a click does not jump to the corner.
  bios->apply(0, inputEvents(Json::parse(R"([{"type":"btn","data":{"button":"left","down":true}},{"type":"btn","data":{"button":"left","down":false}}])")));
  CHECK((dongle.take() == std::vector<std::string>{"mouse 0 0 0 0 1", "mouse 0 0 0 0 0"}));
  bios.reset();
  // Two displays side by side: display 2 starts at x 1024; switching displays re-homes.
  dongle.take();
  Json dual = divided;
  dual.set("displays", Json::Array{Json(Json::Array{Json(0), Json(0), Json(1024), Json(768)}),
                                   Json(Json::Array{Json(1024), Json(0), Json(1024), Json(768)})});
  auto desk = makeHidDevice(dual);
  px = py = homing = 0;
  desk->apply(0, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":16384}},{"type":"abs","data":{"axis":"y","value":0}}])")));
  measure(dongle.take(), px, py, homing);
  CHECK(homing == 15 && px == 513 && py == 0);  // ceil(2048 / 180) + 3 homing reports
  desk->apply(1, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":0}},{"type":"abs","data":{"axis":"y","value":16384}}])")));
  measure(dongle.take(), px, py, homing);
  CHECK(homing == 30 && px == 1023 && py == 384);  // re-homed, then 1024 rounds to 85 steps + 1 small
  desk->apply(1, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":32767}}])")));
  measure(dongle.take(), px, py, homing);
  CHECK(homing == 30 && px == 2046);  // same display: no re-home; 2047 within one small step
  THROWS(desk->apply(2, inputEvents(Json::parse(R"([{"type":"abs","data":{"axis":"x","value":1}}])"))));
  THROWS(makeHidDevice([&] { Json bad = divided; bad.set("displays", Json::Array{Json(Json::Array{Json(0), Json(0), Json(10)})}); return bad; }()));
  desk.reset();
  // A wrong key is rejected by the dongle and reported, not retried forever.
  device.reset();
  writeFileAtomic(root + "/kvm.key", std::string(64, 'a') + "\n");
  auto wrong = makeHidDevice(config);
  THROWS(wrong->apply(0, inputEvents(Json::parse(R"([{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"a"}}}])"))));
  {
    std::lock_guard<std::mutex> lock(dongle.mutex);
    CHECK(dongle.rejected == 1);
  }
  wrong.reset();
  removeTree(root);
}

int main() {
  testCrypto();
  testJson();
  testDomain();
  testImages();
  testInput();
  testRfb();
  testWebSocket();
  testBridge();
  testServices();
  testPhysicalAgent();
  testS2Kvm();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
