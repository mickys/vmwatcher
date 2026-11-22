#include "vmw.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace vmw {

const char* const kVersion = "2.1.0";

void fail(const std::string& message, int status) { throw HttpError(status, message); }

std::string hex(const std::string& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}

std::string base64(const std::string& bytes) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
    out += table[v >> 18];
    out += table[(v >> 12) & 63];
    out += table[(v >> 6) & 63];
    out += table[v & 63];
  }
  if (i + 1 == bytes.size()) {
    uint32_t v = uint8_t(bytes[i]) << 16;
    out += table[v >> 18];
    out += table[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == bytes.size()) {
    uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
    out += table[v >> 18];
    out += table[(v >> 12) & 63];
    out += table[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

std::string randomBytes(size_t n) {
  std::string out(n, '\0');
  size_t done = 0;
  while (done < n) {
    ssize_t got = getrandom(&out[done], n - done, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("No secure random source");
    }
    done += size_t(got);
  }
  return out;
}

std::string randomHex(size_t bytes) { return hex(randomBytes(bytes)); }

std::string uuid4() {
  std::string b = randomBytes(16);
  b[6] = char((b[6] & 0x0f) | 0x40);
  b[8] = char((b[8] & 0x3f) | 0x80);
  std::string h = hex(b);
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

int64_t nowMs() {
  timeval tv{};
  gettimeofday(&tv, nullptr);
  return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

std::string isoTime(int64_t ms) {
  time_t seconds = time_t(ms / 1000);
  tm parts{};
  gmtime_r(&seconds, &parts);
  char buffer[96];
  snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.tm_year + 1900, parts.tm_mon + 1,
           parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec, int(ms % 1000));
  return buffer;
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= 32) a++;
  while (b > a && (unsigned char)s[b - 1] <= 32) b--;
  return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t at = s.find(sep, start);
    if (at == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, at - start));
    start = at + 1;
  }
}

bool isUuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; i++) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

bool constantEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

std::string urlDecode(const std::string& s) {
  std::string out;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && nibble(s[i + 1]) >= 0 && nibble(s[i + 2]) >= 0) {
      out += char(nibble(s[i + 1]) * 16 + nibble(s[i + 2]));
      i += 2;
    } else
      out += s[i];
  }
  return out;
}

bool fileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

std::string readFile(const std::string& path, size_t max) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw HttpError(404, "Not found");
  std::string out;
  char buffer[65536];
  for (;;) {
    ssize_t n = read(fd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buffer, size_t(n));
    if (out.size() > max) {
      close(fd);
      throw std::runtime_error("File too large");
    }
  }
  close(fd);
  return out;
}

void writeFileAtomic(const std::string& path, const std::string& data, int mode) {
  std::string temporary = path + ".tmp" + randomHex(4);
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) throw std::runtime_error("Cannot write " + path + ": " + strerror(errno));
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      close(fd);
      unlink(temporary.c_str());
      throw std::runtime_error("Cannot write " + path);
    }
    done += size_t(n);
  }
  fsync(fd);
  close(fd);
  if (rename(temporary.c_str(), path.c_str()) != 0) {
    unlink(temporary.c_str());
    throw std::runtime_error("Cannot replace " + path);
  }
}

void makeDirs(const std::string& path, int mode) {
  for (size_t at = 1; at <= path.size(); at++)
    if (at == path.size() || path[at] == '/') {
      std::string part = path.substr(0, at);
      if (mkdir(part.c_str(), mode) != 0 && errno != EEXIST)
        throw std::runtime_error("Cannot create " + part + ": " + strerror(errno));
    }
}

std::vector<std::string> listDir(const std::string& path) {
  std::vector<std::string> out;
  DIR* dir = opendir(path.c_str());
  if (!dir) return out;
  while (dirent* entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..") out.push_back(name);
  }
  closedir(dir);
  return out;
}

void removeTree(const std::string& path) {
  struct stat st {};
  if (lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    for (const auto& name : listDir(path)) removeTree(path + "/" + name);
    rmdir(path.c_str());
  } else
    unlink(path.c_str());
}

void logLine(const std::string& text) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  fprintf(stderr, "%s %s\n", isoTime(nowMs()).c_str(), text.c_str());
}

}  // namespace vmw
