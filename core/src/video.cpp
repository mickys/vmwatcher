// Encode the primary display on the viewer host. Use explicit VA-API nodes so
// FFmpeg never opens a different GPU through automatic device selection.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include "vmw.hpp"

extern char** environ;
namespace vmw {
namespace {
struct Encoder {
  std::string id, label, codec, device;
  bool hevc;
};

pid_t spawnEncoder(std::vector<std::string> args, int input, int output) {
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
  pid_t pid;
  int rc = posix_spawn(&pid, "/usr/bin/ffmpeg", &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  return rc ? -1 : pid;
}

std::vector<std::string> command(const Encoder& e, bool probe) {
  std::vector<std::string> args{"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"};
  auto add = [&](std::initializer_list<std::string> fields) { args.insert(args.end(), fields); };
  if (!e.device.empty()) add({"-vaapi_device", e.device});
  if (probe) add({"-f", "lavfi", "-i", "color=s=128x128:r=8", "-frames:v", "1"});
  else add({"-f", "image2pipe", "-framerate", "8", "-probesize", "32", "-analyzeduration", "0", "-c:v", "png", "-i", "pipe:0"});
  std::string filter = "pad=ceil(iw/2)*2:ceil(ih/2)*2";
  add({"-vf", filter + (e.device.empty() ? ",format=yuv420p" : ",format=nv12,hwupload"), "-c:v", e.codec});
  if (e.device.empty()) {
    add({"-threads", "2", "-preset", "ultrafast", "-tune", "zerolatency"});
    if (e.hevc) add({"-x265-params", "pools=1:frame-threads=1:log-level=error"});
  }
  add({"-g", "8", "-bf", "0"});
  if (e.hevc) add({"-tag:v", "hvc1"});
  if (!e.device.empty()) add({"-bsf:v", "extract_extradata"});
  add({"-movflags", "+delay_moov+default_base_moof+frag_every_frame", "-flush_packets", "1", "-f", "mp4", "pipe:1"});
  return args;
}

std::string attribute(const std::filesystem::path& p) {
  std::ifstream f(p);
  std::string value;
  f >> value;
  return value;
}
std::vector<Encoder> detect() {
  std::vector<Encoder> candidates;
  auto pair = [&](const std::string& id, const std::string& name, const std::string& device) {
    candidates.push_back({id+"-h264", name+" H.264", device.empty() ? "libx264" : "h264_vaapi", device, false});
    candidates.push_back({id+"-h265", name+" H.265", device.empty() ? "libx265" : "hevc_vaapi", device, true});
  };
  pair("software", "Software", "");
  std::error_code ec;
  for (const auto& node : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
    auto name = node.path().filename().string();
    auto pci = node.path() / "device";
    if (name.find("renderD") || attribute(pci/"vendor") != "0x1002" || attribute(pci/"device") != "0x67df" ||
        attribute(pci/"subsystem_vendor") != "0x1682" || attribute(pci/"subsystem_device") != "0xc580") continue;
    pair(name, "Radeon RX 580", "/dev/dri/"+name);
  }
  std::vector<Encoder> found;
  int sink = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (sink < 0) return found;
  for (auto& e : candidates) {
    pid_t pid = spawnEncoder(command(e, true), sink, sink);
    int status = 0;
    if (pid > 0) {
      pid_t done;
      do { done = waitpid(pid, &status, 0); } while (done < 0 && errno == EINTR);
      if (done == pid && WIFEXITED(status) && !WEXITSTATUS(status)) found.push_back(e);
    }
  }
  close(sink);
  return found;
}
const std::vector<Encoder>& encoders() {
  static const auto found = detect();
  return found;
}
}
Json videoEncoders() {
  Json out = obj({{"vnc", "VNC"}});
  for (const auto& e : encoders()) out.set(e.id, e.label);
  return out;
}

void streamVideo(Conn& browser, HostApi& host, const std::string& id, const std::string& choice) {
  const Encoder* selected = nullptr;
  for (const auto& e : encoders()) if (e.id == choice) selected = &e;
  if (!selected) fail("Selected video encoder unavailable", 503);
  using Clock = std::chrono::steady_clock;
  constexpr auto interval = std::chrono::milliseconds(125);
  auto latest = std::make_shared<const std::string>(host.headPng(id, 0, true));
  auto lastCapture = Clock::now();
  std::mutex frameMutex;
  int input[2], output[2];
  if (pipe2(input, O_CLOEXEC)) fail("Encoder pipe unavailable", 503);
  if (pipe2(output, O_CLOEXEC)) {
    close(input[0]); close(input[1]); fail("Encoder pipe unavailable", 503);
  }
  pid_t pid = spawnEncoder(command(*selected, false), input[0], output[1]);
  close(input[0]); close(output[1]);
  if (pid < 0) {
    close(input[1]); close(output[0]); fail("Cannot start FFmpeg", 503);
  }
  std::atomic<bool> stop{false};
  // A capture card can be slower than the encoder's declared frame rate.
  // Sample independently so a slow or temporarily busy source cannot starve playback.
  std::thread capture([&] {
    while (!stop) {
      auto next = Clock::now() + interval;
      try {
        auto frame = std::make_shared<const std::string>(host.headPng(id, 0, true));
        std::lock_guard<std::mutex> lock(frameMutex);
        latest = std::move(frame);
        lastCapture = Clock::now();
      } catch (const std::exception&) {
        // The writer keeps the last good frame for a short interruption.
      }
      std::this_thread::sleep_until(next);
    }
  });
  std::thread writer([&] {
    while (!stop) {
      auto next = Clock::now() + interval;
      std::shared_ptr<const std::string> frame;
      {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (next - lastCapture > std::chrono::seconds(5)) break;
        frame = latest;
      }
      size_t sent = 0;
      while (sent < frame->size()) {
        ssize_t n = write(input[1], frame->data() + sent, frame->size() - sent);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
          stop = true;
          break;
        }
        sent += size_t(n);
      }
      std::this_thread::sleep_until(next);
    }
    close(input[1]);
  });
  bool started = false;
  try {
    char bytes[65536];
    while (true) {
      ssize_t n = read(output[0], bytes, sizeof bytes);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      if (!started) {
        browser.write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nCross-Origin-Resource-Policy: same-origin\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
        started = true;
      }
      browser.write(bytes, n);
    }
  } catch (const std::exception&) {}
  stop = true;
  kill(pid, SIGKILL);
  close(output[0]);
  writer.join();
  capture.join();
  while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
  if (!started) fail("Video encoder produced no frames", 503);
}
}
