#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "vmw.hpp"

extern char** environ;
namespace vmw {
namespace {
using Clock = std::chrono::steady_clock;

std::string runPlugin(const std::string& path, const std::string& input, size_t maxOutput) {
  int fd[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd)) fail("Cannot open plugin connection", 503);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fd[1], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);
  char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
  pid_t pid;
  int rc = posix_spawn(&pid, path.c_str(), &actions, &attr, argv, environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  close(fd[1]);
  if (rc) {
    close(fd[0]);
    fail("Cannot start device plugin", 503);
  }
  struct Child {
    int fd;
    pid_t pid;
    ~Child() {
      close(fd);
      if (pid <= 0) return;
      kill(-pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
  } child{fd[0], pid};
  if (!input.empty() && send(child.fd, input.data(), input.size(), MSG_DONTWAIT | MSG_NOSIGNAL) != ssize_t(input.size()))
    fail("Cannot send input to HID plugin", 503);
  shutdown(child.fd, SHUT_WR);
  auto deadline = Clock::now() + std::chrono::seconds(3);
  std::string output;
  for (;;) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) fail("Device plugin timed out", 504);
    pollfd p{child.fd, POLLIN, 0};
    int ready = poll(&p, 1, int(remaining));
    if (ready < 0 && errno == EINTR) continue;
    if (!ready) fail("Device plugin timed out", 504);
    if (ready < 0) fail("Device plugin connection failed", 503);
    char buffer[65536];
    ssize_t n = recv(child.fd, buffer, sizeof buffer, MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n < 0) fail("Cannot read device plugin output", 503);
    if (!n) break;
    if (output.size() + size_t(n) > maxOutput) fail("Device plugin output is too large", 502);
    output.append(buffer, size_t(n));
  }
  int status;
  for (;;) {
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) {
      child.pid = -1;
      if (!WIFEXITED(status) || WEXITSTATUS(status)) fail("Device plugin failed", 502);
      return output;
    }
    if (done < 0 && errno != EINTR) fail("Cannot wait for device plugin", 503);
    if (Clock::now() >= deadline) fail("Device plugin timed out", 504);
    usleep(1000);
  }
}

std::string pluginPath(const Json& source, const std::string& key, bool required) {
  if (!required && !source.has(key)) return "";
  std::string path = source[key].str();
  if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos || access(path.c_str(), X_OK))
    throw std::runtime_error(key + " must name an executable absolute path");
  return path;
}

class PhysicalHost : public HostApi {
 public:
  explicit PhysicalHost(const Json& config) {
    const Json& sources = config["sources"];
    if (!sources.isArray()) throw std::runtime_error("Physical agent requires a sources array");
    for (const auto& entry : sources.arr()) {
      std::string id = lower(entry["id"].str());
      std::string name = entry["name"].str();
      if (!isUuid(id) || name.empty() || sources_.count(id)) throw std::runtime_error("Invalid or duplicate physical source ID or name");
      auto source = std::make_unique<Source>();
      source->name = name;
      source->capture = pluginPath(entry, "capture", true);
      source->hid = pluginPath(entry, "hid", false);
      sources_.emplace(id, std::move(source));
    }
  }

  Json info() override {
    bool input = false;
    for (const auto& entry : sources_) input |= !entry.second->hid.empty();
    return obj({{"version", kVersion}, {"mode", "physical"},
                {"capabilities", obj({{"live", true}, {"audio", false}, {"input", input}})}});
  }

  Json inventory() override {
    Json out = Json::Array{};
    for (const auto& entry : sources_) {
      auto& source = *entry.second;
      Json display = obj({{"head", 0}});
      {
        std::lock_guard<std::mutex> lock(source.frameMutex);
        if (source.frame) {
          display.set("width", source.frame->image.width);
          display.set("height", source.frame->image.height);
          display.set("capturedAt", isoTime(source.frame->time));
        }
      }
      Json displays = Json::Array{};
      displays.push(display);
      out.push(obj({{"id", entry.first}, {"name", source.name}, {"kind", "physical"}, {"state", "running"},
                    {"heads", 1}, {"vnc", false}, {"live", false}, {"input", !source.hid.empty()}, {"displays", displays}}));
    }
    return out;
  }

  std::string headPng(const std::string& id, int head, bool fresh) override {
    if (head != 0) fail("Head is not configured", 404);
    return frame(get(id), fresh)->png;
  }

  std::string previewPng(const std::string& id, int width) override {
    auto captured = frame(get(id), false);
    width = std::clamp(width, 128, 4096);
    if (captured->image.width <= width) return captured->png;
    return encodePng(resizeImage(captured->image, width));
  }

  void input(const std::string& id, int head, const Json& raw) override {
    auto& source = get(id);
    if (head != 0) fail("Head is not configured", 404);
    if (source.hid.empty()) fail("No HID plugin is configured", 403);
    Json events = inputEvents(raw);
    std::unique_lock<std::timed_mutex> lock(source.hidMutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(1))) fail("HID plugin busy", 503);
    runPlugin(source.hid, obj({{"head", head}, {"events", events}}).dump() + "\n", 4096);
  }

  std::unique_ptr<Conn> console(const std::string& id) override {
    get(id);
    fail("Physical sources use encoded video", 404);
  }

 private:
  struct Frame {
    Image image;
    std::string png;
    int64_t time;
  };

  struct Source {
    std::string name, capture, hid;
    std::timed_mutex captureMutex, hidMutex;
    std::mutex frameMutex;
    std::shared_ptr<Frame> frame;
  };

  Source& get(const std::string& id) {
    auto found = sources_.find(lower(id));
    if (found == sources_.end()) fail("Physical source not found", 404);
    return *found->second;
  }

  std::shared_ptr<Frame> frame(Source& source, bool fresh) {
    std::unique_lock<std::timed_mutex> capture(source.captureMutex, std::defer_lock);
    if (!capture.try_lock_for(std::chrono::seconds(3))) fail("Capture plugin busy", 503);
    {
      std::lock_guard<std::mutex> lock(source.frameMutex);
      if (source.frame && nowMs() - source.frame->time < (fresh ? 100 : 1000)) return source.frame;
    }
    auto result = std::make_shared<Frame>();
    try {
      result->image = decodeScreenshot(runPlugin(source.capture, "", 64u << 20));
      result->png = encodePng(result->image);
    } catch (const HttpError&) {
      throw;
    } catch (const std::exception&) {
      fail("Capture plugin did not return a valid frame", 502);
    }
    result->time = nowMs();
    std::lock_guard<std::mutex> lock(source.frameMutex);
    return source.frame = result;
  }

  std::map<std::string, std::unique_ptr<Source>> sources_;
};

}  // namespace

std::shared_ptr<HostApi> makePhysicalHost(const Json& config) { return std::make_shared<PhysicalHost>(config); }

}  // namespace vmw
