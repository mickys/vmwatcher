#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "vmw.hpp"

namespace vmw {

// ---------------------------------------------------------------- TLS
namespace {

// OpenSSL is loaded at run time (3.x or 1.1), like libvirt and zlib.
struct Tls {
  const void* (*serverMethod)() = nullptr;
  void* (*ctxNew)(const void*) = nullptr;
  long (*ctxCtrl)(void*, int, long, void*) = nullptr;
  int (*ctxUseChain)(void*, const char*) = nullptr;
  int (*ctxUseKey)(void*, const char*, int) = nullptr;
  int (*ctxCheckKey)(const void*) = nullptr;
  void* (*sslNew)(void*) = nullptr;
  int (*setFd)(void*, int) = nullptr;
  int (*accept)(void*) = nullptr;
  int (*read)(void*, void*, int) = nullptr;
  int (*write)(void*, const void*, int) = nullptr;
  int (*pending)(const void*) = nullptr;
  int (*getError)(const void*, int) = nullptr;
  int (*shutdown)(void*) = nullptr;
  void (*sslFree)(void*) = nullptr;
  unsigned long (*errGet)() = nullptr;
  void (*errString)(unsigned long, char*, size_t) = nullptr;
  std::string problem;
  std::string lastError(const char* fallback) const {
    unsigned long code = errGet ? errGet() : 0;
    if (!code) return fallback;
    char text[256] = "";
    errString(code, text, sizeof text);
    while (errGet()) {
    }
    return std::string(fallback) + ": " + text;
  }
};

const Tls& tls() {
  static Tls instance = [] {
    Tls t;
    void* lib = dlopen("libssl.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libssl.so.1.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
      t.problem = "HTTPS needs OpenSSL (libssl.so.3 or libssl.so.1.1), which is not installed";
      return t;
    }
    bool complete = true;
#define VMW_TLS(field, name)                                              \
  t.field = reinterpret_cast<decltype(t.field)>(dlsym(lib, name)); \
  if (!t.field) complete = false
    VMW_TLS(serverMethod, "TLS_server_method");
    VMW_TLS(ctxNew, "SSL_CTX_new");
    VMW_TLS(ctxCtrl, "SSL_CTX_ctrl");
    VMW_TLS(ctxUseChain, "SSL_CTX_use_certificate_chain_file");
    VMW_TLS(ctxUseKey, "SSL_CTX_use_PrivateKey_file");
    VMW_TLS(ctxCheckKey, "SSL_CTX_check_private_key");
    VMW_TLS(sslNew, "SSL_new");
    VMW_TLS(setFd, "SSL_set_fd");
    VMW_TLS(accept, "SSL_accept");
    VMW_TLS(read, "SSL_read");
    VMW_TLS(write, "SSL_write");
    VMW_TLS(pending, "SSL_pending");
    VMW_TLS(getError, "SSL_get_error");
    VMW_TLS(shutdown, "SSL_shutdown");
    VMW_TLS(sslFree, "SSL_free");
    VMW_TLS(errGet, "ERR_get_error");
    VMW_TLS(errString, "ERR_error_string_n");
#undef VMW_TLS
    if (!complete) t.problem = "The installed OpenSSL is incomplete";
    return t;
  }();
  return instance;
}

}  // namespace

// ---------------------------------------------------------------- Conn
Conn::~Conn() {
  if (tls) {
    vmw::tls().shutdown(tls);  // best effort close_notify, bounded by the send timeout
    vmw::tls().sslFree(tls);
  }
  if (fd >= 0) ::close(fd);
}
bool Conn::buffered() const { return tls && vmw::tls().pending(tls) > 0; }
size_t Conn::readSome(char* buffer, size_t size) {
  if (tls) {
    int n = vmw::tls().read(tls, buffer, int(std::min<size_t>(size, 1 << 30)));
    if (n > 0) {
      if (traffic) traffic->up += n;
      return size_t(n);
    }
    int why = vmw::tls().getError(tls, n);
    while (vmw::tls().errGet()) {
    }
    if (why == 6 /* SSL_ERROR_ZERO_RETURN */) return 0;
    if (why == 5 /* SSL_ERROR_SYSCALL */ && n == 0) return 0;  // closed without close_notify
    throw std::runtime_error("Connection failed");
  }
  for (;;) {
    ssize_t n = ::recv(fd, buffer, size, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) throw std::runtime_error(errno == EAGAIN || errno == EWOULDBLOCK ? "Connection timed out" : "Connection failed");
    if (traffic) traffic->up += n;
    return size_t(n);
  }
}
int Conn::release() {
  int f = fd;
  fd = -1;
  return f;
}
void Conn::timeouts(int receiveSeconds, int sendSeconds) {
  timeval r{receiveSeconds, 0}, s{sendSeconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof s);
}
size_t Conn::fill() {
  char buffer[65536];
  size_t n = readSome(buffer, sizeof buffer);
  in.append(buffer, n);
  return n;
}
bool Conn::readUntil(const std::string& delimiter, size_t max, std::string& out) {
  size_t searched = 0;
  for (;;) {
    size_t at = in.find(delimiter, searched > delimiter.size() ? searched - delimiter.size() : 0);
    if (at != std::string::npos) {
      out = in.substr(0, at);
      in.erase(0, at + delimiter.size());
      return true;
    }
    if (in.size() > max) throw HttpError(431, "Request too large");
    searched = in.size();
    if (!fill()) return false;
  }
}
std::string Conn::readN(size_t n) {
  while (in.size() < n)
    if (!fill()) throw std::runtime_error("Connection closed early");
  std::string out = in.substr(0, n);
  in.erase(0, n);
  return out;
}
void Conn::write(const char* data, size_t n) {
  size_t done = 0;
  while (tls && done < n) {
    int sent = vmw::tls().write(tls, data + done, int(std::min<size_t>(n - done, 1 << 30)));
    if (sent <= 0) {
      while (vmw::tls().errGet()) {
      }
      throw std::runtime_error("Connection write failed");
    }
    if (traffic) traffic->down += sent;
    done += size_t(sent);
  }
  while (done < n) {
    ssize_t sent = ::send(fd, data + done, n - done, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) throw std::runtime_error("Connection write failed");
    if (traffic) traffic->down += sent;
    done += size_t(sent);
  }
}

std::unique_ptr<Conn> connectTcp(const std::string& host, int port, int timeoutSeconds) {
  addrinfo hints{}, *list = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list)
    throw std::runtime_error("Cannot resolve " + host);
  std::string reason = "Cannot connect to " + host + ":" + std::to_string(port);
  for (addrinfo* a = list; a; a = a->ai_next) {
    int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
    if (fd < 0) continue;
    int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      int error = 0;
      socklen_t length = sizeof error;
      if (poll(&p, 1, timeoutSeconds * 1000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0)
        rc = 0;
    }
    if (rc == 0) {
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      freeaddrinfo(list);
      return std::unique_ptr<Conn>(new Conn(fd));
    }
    ::close(fd);
  }
  freeaddrinfo(list);
  throw std::runtime_error(reason);
}

// ---------------------------------------------------------------- HTTP
std::string Request::header(const std::string& name) const {
  auto found = headers.find(name);
  return found == headers.end() ? "" : found->second;
}
std::string Request::param(const std::string& name, const std::string& fallback) const {
  auto found = query.find(name);
  return found == query.end() ? fallback : found->second;
}

Response Response::json(const Json& value, int status) {
  Response r;
  r.status = status;
  r.body = value.dump();
  return r;
}
Response Response::error(int status, const std::string& message) {
  Json body = Json::Object{};
  body.set("error", message);
  return json(body, status);
}
Response Response::bytes(std::string body, const std::string& type) {
  Response r;
  r.type = type;
  r.body = std::move(body);
  return r;
}

static bool readHead(Conn& conn, std::string& firstLine, std::map<std::string, std::string>& headers, size_t* bytes = nullptr) {
  std::string head;
  if (!conn.readUntil("\r\n\r\n", 16384, head)) return false;
  if (bytes) *bytes = head.size() + 4;
  auto lines = split(head, '\n');
  if (lines.empty()) throw HttpError(400, "Bad request");
  firstLine = trim(lines[0]);
  for (size_t i = 1; i < lines.size(); i++) {
    size_t colon = lines[i].find(':');
    if (colon == std::string::npos) throw HttpError(400, "Bad header");
    headers[lower(trim(lines[i].substr(0, colon)))] = trim(lines[i].substr(colon + 1));
  }
  return true;
}

bool readRequest(Conn& conn, Request& req, size_t maxBody) {
  std::string line;
  if (!readHead(conn, line, req.headers, &req.headerBytes)) return false;
  auto parts = split(line, ' ');
  if (parts.size() != 3 || parts[2].compare(0, 7, "HTTP/1.") != 0 || parts[1].empty() || parts[1][0] != '/')
    throw HttpError(400, "Bad request");
  req.method = parts[0];
  req.target = parts[1];
  size_t mark = req.target.find('?');
  req.path = urlDecode(req.target.substr(0, mark));
  if (req.path.find('\0') != std::string::npos) throw HttpError(400, "Bad request");
  if (mark != std::string::npos)
    for (const auto& pair : split(req.target.substr(mark + 1), '&')) {
      size_t eq = pair.find('=');
      if (!pair.empty())
        req.query[urlDecode(pair.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(pair.substr(eq + 1));
    }
  if (!req.header("transfer-encoding").empty()) throw HttpError(411, "Send a Content-Length");
  std::string length = req.header("content-length");
  if (!length.empty()) {
    if (length.size() > 9 || length.find_first_not_of("0123456789") != std::string::npos) throw HttpError(400, "Bad length");
    size_t n = size_t(std::stoul(length));
    if (n > maxBody) throw HttpError(413, "Request too large");
    req.body = conn.readN(n);
  }
  return true;
}

static const char* reasonOf(int status) {
  switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default: return status >= 500 ? "Server Error" : "Error";
  }
}

void writeResponse(Conn& conn, const Response& res, bool keepAlive) {
  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reasonOf(res.status) + "\r\n";
  out += "Content-Type: " + res.type + "\r\n";
  out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
  out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  for (const auto& h : res.headers) out += h.first + ": " + h.second + "\r\n";
  out += "\r\n";
  if (res.body.size() < 65536) {
    out += res.body;
    conn.write(out);
  } else {
    conn.write(out);
    conn.write(res.body);
  }
}

// ---------------------------------------------------------------- Server
Server::~Server() { stop(); }

void Server::enableTls(const std::string& certificateChainFile, const std::string& privateKeyFile) {
  const Tls& t = tls();
  if (!t.problem.empty()) throw std::runtime_error(t.problem);
  void* context = t.ctxNew(t.serverMethod());
  if (!context) throw std::runtime_error(t.lastError("Cannot start TLS"));
  t.ctxCtrl(context, 123 /* SSL_CTRL_SET_MIN_PROTO_VERSION */, 0x0303 /* TLS 1.2 */, nullptr);
  t.ctxCtrl(context, 33 /* SSL_CTRL_MODE */, 4 /* SSL_MODE_AUTO_RETRY */, nullptr);
  if (t.ctxUseChain(context, certificateChainFile.c_str()) != 1)
    throw std::runtime_error(t.lastError(("Cannot load the certificate " + certificateChainFile).c_str()));
  if (t.ctxUseKey(context, privateKeyFile.c_str(), 1 /* PEM */) != 1)
    throw std::runtime_error(t.lastError(("Cannot load the private key " + privateKeyFile).c_str()));
  if (t.ctxCheckKey(context) != 1) throw std::runtime_error("The private key does not match the certificate");
  tlsContext_ = context;
}

int Server::listen(const std::string& bind, int port) {
  addrinfo hints{}, *list = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
  if (getaddrinfo(bind.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list)
    throw std::runtime_error("Invalid bind address " + bind);
  int fd = socket(list->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (fd < 0 || ::bind(fd, list->ai_addr, list->ai_addrlen) != 0 || ::listen(fd, 128) != 0) {
    std::string why = strerror(errno);
    freeaddrinfo(list);
    if (fd >= 0) ::close(fd);
    throw std::runtime_error("Cannot listen on " + bind + ":" + std::to_string(port) + ": " + why);
  }
  freeaddrinfo(list);
  sockaddr_storage bound{};
  socklen_t length = sizeof bound;
  getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length);
  listenFd_ = fd;
  return ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                           : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
}

void Server::run() {
  while (!stopping_) {
    sockaddr_storage peer{};
    socklen_t length = sizeof peer;
    int fd = accept4(listenFd_, reinterpret_cast<sockaddr*>(&peer), &length, SOCK_CLOEXEC);
    if (fd < 0) {
      if (stopping_) break;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      usleep(50000);  // out of descriptors: let connections drain
      continue;
    }
    if (active_ >= maxConnections) {
      ::close(fd);
      continue;
    }
    char name[INET6_ADDRSTRLEN] = "";
    if (peer.ss_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, name, sizeof name);
    else inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr, name, sizeof name);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    active_++;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      fds_[fd] = true;
    }
    std::thread([this, fd, peerName = std::string(name)] {
      serve(fd, peerName);
      active_--;
    }).detach();
  }
}

void Server::serve(int fd, std::string peer) {
  // The descriptor is unregistered and closed under the same lock, so stop()
  // can shut a live connection down but never touches a reused number.
  struct Guard {
    Server* server;
    int fd;
    std::unique_ptr<Conn> conn;
    ~Guard() {
      std::lock_guard<std::mutex> lock(server->mutex_);
      server->fds_.erase(fd);
      conn.reset();
    }
  } guard{this, fd, std::unique_ptr<Conn>(new Conn(fd))};
  Conn& conn = *guard.conn;
  if (tlsContext_) {
    conn.timeouts(10, 10);
    conn.tls = tls().sslNew(tlsContext_);
    if (!conn.tls || tls().setFd(conn.tls, fd) != 1 || tls().accept(conn.tls) != 1) {
      while (tls().errGet()) {
      }
      return;  // not a TLS client, or the handshake failed
    }
  }
  try {
    for (int served = 0; served < 1000 && !stopping_; served++) {
      conn.timeouts(served ? 75 : 20, 30);
      conn.traffic.reset();
      Request req;
      req.peer = peer;
      Response res;
      bool keep = true;
      try {
        if (!readRequest(conn, req)) return;
        res = handler_(req, conn);
        if (res.status == 0) return;
        keep = lower(req.header("connection")) != "close";
      } catch (const HttpError& e) {
        res = Response::error(e.status, e.what());
        keep = !req.method.empty() && e.status != 413 && e.status != 431 && e.status != 411;
      } catch (const std::exception& e) {
        if (req.method.empty()) return;  // the peer went away or sent garbage
        logLine("Request failed: " + req.path + ": " + e.what());
        res = Response::error(400, e.what());
      }
      writeResponse(conn, res, keep);
      if (!keep) return;
    }
  } catch (const std::exception&) {
  }
}

void Server::start() {
  thread_.reset(new std::thread([this] { run(); }));
}

void Server::stop() {
  if (stopping_.exchange(true)) return;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : fds_) ::shutdown(entry.first, SHUT_RDWR);
  }
  if (thread_ && thread_->joinable()) thread_->join();
  for (int i = 0; i < 300 && active_ > 0; i++) usleep(10000);
}

// ---------------------------------------------------------------- client
Url parseHttpUrl(const std::string& url) {
  std::string rest = trim(url);
  if (rest.compare(0, 7, "http://") != 0) fail("Agent address must look like http://host:port");
  rest = rest.substr(7);
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  Url out;
  out.port = 6089;
  size_t colon = rest.rfind(':');
  if (!rest.empty() && rest[0] == '[') {
    size_t end = rest.find(']');
    if (end == std::string::npos) fail("Invalid agent address");
    out.host = rest.substr(1, end - 1);
    colon = rest.find(':', end);
  } else
    out.host = rest.substr(0, colon);
  if (colon != std::string::npos) {
    std::string port = rest.substr(colon + 1);
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) fail("Invalid agent port");
    out.port = std::stoi(port);
  }
  if (out.host.empty() || out.port < 1 || out.port > 65535 || out.host.find_first_of("/@?# ") != std::string::npos)
    fail("Invalid agent address");
  return out;
}

static std::string requestHead(const Url& base, const std::string& method, const std::string& path, const Headers& headers,
                               size_t bodySize) {
  std::string out = method + " " + path + " HTTP/1.1\r\nHost: " + base.host + ":" + std::to_string(base.port) + "\r\n";
  for (const auto& h : headers) out += h.first + ": " + h.second + "\r\n";
  if (bodySize || method == "POST" || method == "PUT") out += "Content-Length: " + std::to_string(bodySize) + "\r\n";
  return out;
}

static int statusOf(const std::string& line) {
  auto parts = split(line, ' ');
  if (parts.size() < 2 || parts[0].compare(0, 7, "HTTP/1.") != 0) throw std::runtime_error("Unexpected reply");
  return atoi(parts[1].c_str());
}

Response httpCall(const Url& base, const std::string& method, const std::string& path, const Headers& headers,
                  const std::string& body, int timeoutSeconds, size_t maxBody) {
  auto conn = connectTcp(base.host, base.port, std::min(timeoutSeconds, 8));
  conn->timeouts(timeoutSeconds, timeoutSeconds);
  conn->write(requestHead(base, method, path, headers, body.size()) + "Connection: close\r\n\r\n" + body);
  std::string line;
  std::map<std::string, std::string> replyHeaders;
  if (!readHead(*conn, line, replyHeaders)) throw std::runtime_error("No reply");
  Response res;
  res.status = statusOf(line);
  res.type = replyHeaders["content-type"];
  std::string length = replyHeaders["content-length"];
  if (length.empty() || length.size() > 10 || length.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("Reply has no length");
  size_t n = size_t(std::stoull(length));
  if (n > maxBody) throw std::runtime_error("Reply too large");
  res.body = conn->readN(n);
  return res;
}

std::unique_ptr<Conn> httpUpgrade(const Url& base, const std::string& path, const Headers& headers, const std::string& protocol) {
  auto conn = connectTcp(base.host, base.port, 8);
  conn->timeouts(15, 15);
  conn->write(requestHead(base, "GET", path, headers, 0) + "Connection: Upgrade\r\nUpgrade: " + protocol + "\r\n\r\n");
  std::string line;
  std::map<std::string, std::string> replyHeaders;
  if (!readHead(*conn, line, replyHeaders)) throw std::runtime_error("No reply");
  int status = statusOf(line);
  if (status != 101) {
    std::string message = "Console unavailable";
    try {
      size_t n = size_t(std::stoul(replyHeaders["content-length"]));
      if (n < 4096) message = Json::parse(conn->readN(n))["error"].str(message);
    } catch (...) {
    }
    throw HttpError(status == 401 ? 502 : 400, message);
  }
  return conn;
}

// ---------------------------------------------------------------- WebSocket
void wsAccept(Conn& conn, const Request& req, const std::string& protocol) {
  std::string key = req.header("sec-websocket-key");
  if (req.method != "GET" || lower(req.header("upgrade")) != "websocket" || key.size() < 16 || key.size() > 64 ||
      req.header("sec-websocket-version") != "13")
    throw HttpError(400, "Expected a WebSocket upgrade");
  std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                      base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) + "\r\n";
  if (!protocol.empty()) {
    bool offered = false;
    for (const auto& p : split(req.header("sec-websocket-protocol"), ','))
      if (trim(p) == protocol) offered = true;
    if (!offered) throw HttpError(400, "WebSocket protocol not offered");
    reply += "Sec-WebSocket-Protocol: " + protocol + "\r\n";
  }
  conn.write(reply + "\r\n");
}

std::string wsFrame(int opcode, const char* data, size_t size) {
  std::string out;
  out.reserve(size + 10);
  out += char(0x80 | opcode);
  if (size < 126) out += char(size);
  else if (size < 65536) {
    out += char(126);
    out += char(size >> 8);
    out += char(size);
  } else {
    out += char(127);
    for (int i = 7; i >= 0; i--) out += char(uint64_t(size) >> (8 * i));
  }
  out.append(data, size);
  return out;
}

// Client frames only: they must be masked, as the protocol requires of browsers.
bool WsParser::next(WsMessage& out) {
  for (;;) {
    if (buffer.size() < 2) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buffer.data());
    bool fin = p[0] & 0x80;
    int opcode = p[0] & 15;
    if ((p[0] & 0x70) || !(p[1] & 0x80)) throw std::runtime_error("Invalid WebSocket frame");
    uint64_t length = p[1] & 127;
    size_t header = 2;
    if (length == 126) {
      if (buffer.size() < 4) return false;
      length = (uint64_t(p[2]) << 8) | p[3];
      header = 4;
    } else if (length == 127) {
      if (buffer.size() < 10) return false;
      length = 0;
      for (int i = 0; i < 8; i++) length = (length << 8) | p[2 + i];
      header = 10;
    }
    if (length > max || fragment.size() + length > max) throw std::runtime_error("WebSocket message too large");
    if (buffer.size() < header + 4 + length) return false;
    uint8_t mask[4] = {p[header], p[header + 1], p[header + 2], p[header + 3]};
    std::string payload = buffer.substr(header + 4, size_t(length));
    for (size_t i = 0; i < payload.size(); i++) payload[i] = char(uint8_t(payload[i]) ^ mask[i & 3]);
    buffer.erase(0, header + 4 + size_t(length));
    if (opcode >= 8) {  // control frames are never fragmented
      if (!fin || length > 125) throw std::runtime_error("Invalid WebSocket control frame");
      out.opcode = opcode;
      out.data = std::move(payload);
      return true;
    }
    if (opcode == 0) {
      if (!fragmentOpcode) throw std::runtime_error("Unexpected WebSocket continuation");
      fragment += payload;
    } else {
      if (fragmentOpcode || (opcode != 1 && opcode != 2)) throw std::runtime_error("Invalid WebSocket frame order");
      fragmentOpcode = opcode;
      fragment = std::move(payload);
    }
    if (!fin) continue;
    out.opcode = fragmentOpcode;
    out.data = std::move(fragment);
    fragment.clear();
    fragmentOpcode = 0;
    return true;
  }
}

// ---------------------------------------------------------------- bridges
namespace {

// Waits for either side; returns a bitmask (1: a readable, 2: b readable), 0 on timeout.
int waitEither(Conn& a, Conn& b, int timeoutMs) {
  int ready = (a.buffered() ? 1 : 0) | (b.buffered() ? 2 : 0);
  if (ready) return ready;
  pollfd fds[2] = {{a.fd, POLLIN, 0}, {b.fd, POLLIN, 0}};
  int rc;
  do rc = poll(fds, 2, timeoutMs);
  while (rc < 0 && errno == EINTR);
  if (rc <= 0) return 0;
  return (fds[0].revents ? 1 : 0) | (fds[1].revents ? 2 : 0);
}

size_t receive(Conn& c, char* buffer, size_t size) { return c.readSome(buffer, size); }

// Shared loop for browser WebSocket <-> console TCP. `fromBrowser` returns false to end.
void pump(Conn& browser, Conn& console, const std::function<void(const char*, size_t)>& fromConsole,
          const std::function<bool(WsMessage&)>& fromBrowser, size_t maxMessage) {
  // Reads follow poll(), so they return at once; the limit only bounds a TLS
  // record that stalls half-way.
  browser.timeouts(30, 30);
  console.timeouts(30, 30);
  WsParser parser(maxMessage);
  parser.buffer.swap(browser.in);
  std::vector<char> buffer(256 * 1024);
  auto drain = [&] {
    WsMessage message;
    while (parser.next(message)) {
      if (message.opcode == 8) return false;
      if (message.opcode == 9) browser.write(wsFrame(10, message.data));
      else if (message.opcode != 10 && !fromBrowser(message)) return false;
    }
    return true;
  };
  if (!console.in.empty()) {
    std::string early;
    early.swap(console.in);
    fromConsole(early.data(), early.size());
  }
  if (!drain()) return;
  for (;;) {
    int ready = waitEither(browser, console, 120000);
    if (!ready) return;  // idle for two minutes
    if (ready & 2) {
      size_t n = receive(console, buffer.data(), buffer.size());
      if (!n) return;
      fromConsole(buffer.data(), n);
    }
    if (ready & 1) {
      size_t n = receive(browser, buffer.data(), buffer.size());
      if (!n) return;
      parser.buffer.append(buffer.data(), n);
      if (!drain()) return;
    }
  }
}

}  // namespace

void bridgeConsole(Conn& browser, Conn& console, bool readOnly) {
  ReadOnlyRfb filter;
  try {
    pump(
        browser, console,
        [&](const char* data, size_t n) {
          if (readOnly) filter.observeServer(data, n);
          browser.write(wsFrame(2, data, n));
          if (readOnly) {
            std::string pending = filter.push("");
            if (!pending.empty()) console.write(pending);
          }
        },
        [&](WsMessage& message) {
          if (message.opcode != 2) return false;
          std::string payload = readOnly ? filter.push(message.data) : message.data;
          if (!payload.empty()) console.write(payload);
          return true;
        },
        16u << 20);
  } catch (const std::exception&) {
  }
  try {
    browser.write(wsFrame(8, ""));
  } catch (const std::exception&) {
  }
}

void bridgeAudio(Conn& browser, Conn& console) {
  auto text = [&](const Json& value) { browser.write(wsFrame(1, value.dump())); };
  auto error = [&](const std::string& message) {
    try {
      Json body = Json::Object{};
      body.set("type", "error");
      body.set("message", message);
      text(body);
      browser.write(wsFrame(8, ""));
    } catch (const std::exception&) {
    }
  };
  AudioRfb parser;
  parser.write = [&](const std::string& data) { console.write(data); };
  parser.ready = [&] {
    Json format = Json::Object{};
    format.set("type", "format");
    format.set("format", "s16le");
    format.set("sampleRate", 48000);
    format.set("channels", 2);
    text(format);
  };
  parser.pcm = [&](const std::string& data) { browser.write(wsFrame(2, data)); };
  int64_t started = nowMs();
  try {
    pump(
        browser, console,
        [&](const char* data, size_t n) {
          parser.push(data, n);
          if (!parser.enabled && nowMs() - started > 10000) throw HttpError(400, "This VM has no enabled VNC audio stream");
        },
        [&](WsMessage&) { return false; }, 4096);
    error(parser.enabled ? "VM audio disconnected" : "This VM has no enabled VNC audio stream");
  } catch (const HttpError& e) {
    error(e.what());
  } catch (const std::exception& e) {
    error(std::string(e.what()).find("udio") != std::string::npos ? e.what() : "VM audio connection failed");
  }
}

void bridgeRaw(Conn& a, Conn& b) {
  a.timeouts(0, 30);
  b.timeouts(0, 30);
  std::vector<char> buffer(256 * 1024);
  try {
    if (!a.in.empty()) {
      b.write(a.in);
      a.in.clear();
    }
    if (!b.in.empty()) {
      a.write(b.in);
      b.in.clear();
    }
    for (;;) {
      int ready = waitEither(a, b, 150000);
      if (!ready) return;
      if (ready & 1) {
        size_t n = receive(a, buffer.data(), buffer.size());
        if (!n) return;
        b.write(buffer.data(), n);
      }
      if (ready & 2) {
        size_t n = receive(b, buffer.data(), buffer.size());
        if (!n) return;
        a.write(buffer.data(), n);
      }
    }
  } catch (const std::exception&) {
  }
}

}  // namespace vmw
