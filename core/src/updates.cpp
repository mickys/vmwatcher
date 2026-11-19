#include "updates.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>

namespace vmw {
namespace {
using Version = std::array<unsigned, 3>;
bool parse(const std::string& value, Version& out) {
  const char* next = value.data();
  const char* end = next + value.size();
  for (size_t i = 0; i < out.size(); ++i) {
    auto result = std::from_chars(next, end, out[i]);
    if (result.ec != std::errc() || (result.ptr - next > 1 && *next == '0')) return false;
    next = result.ptr;
    if (i < 2 && (next == end || *next++ != '.')) return false;
  }
  return next == end;
}

std::string checkTags() {
  // All arguments are fixed. Bound network time and consume stdout to EOF.
  FILE* pipe = popen("timeout -k 2 20 git ls-remote --tags --refs git@github.com:mickys/vmwatcher.git 'refs/tags/v*'", "re");
  if (!pipe) return "Could not start the GitHub update check.";
  Version latest{}, installed{};
  std::string tag;
  char line[4096];
  while (fgets(line, sizeof line, pipe)) {
    std::string ref = trim(line);
    auto tab = ref.find('\t');
    const std::string prefix = "\trefs/tags/v";
    if (tab == std::string::npos || ref.compare(tab, prefix.size(), prefix) != 0) continue;
    std::string candidate = ref.substr(tab + prefix.size());
    Version version{};
    if (parse(candidate, version) && (tag.empty() || latest < version)) {
      latest = version;
      tag = candidate;
    }
  }
  if (pclose(pipe) != 0) return "Could not read GitHub tags. Check repository access and connectivity.";
  if (tag.empty()) return "GitHub has no stable version tags.";
  if (!parse(kVersion, installed)) return "Latest stable GitHub version is v" + tag + ".";
  if (latest > installed) return "Update available: v" + tag + ".";
  if (latest < installed) return "Installed version is newer than GitHub v" + tag + ".";
  return "Up to date with GitHub v" + tag + ".";
}
}

void UpdateChecker::start() {
  std::lock_guard<std::mutex> guard(mutex_);
  checking_ = true;
  worker_ = std::thread(&UpdateChecker::run, this);
}
UpdateChecker::~UpdateChecker() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
}
Json UpdateChecker::status(bool request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (request && worker_.joinable() && !checking_ && nowMs() - last_ >= 5000) {
    checking_ = true;
    wake_.notify_one();
  }
  return obj({{"checking", checking_}, {"message", message_}, {"checkedAt", last_}});
}
void UpdateChecker::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    checking_ = true;
    lock.unlock();
    std::string message = checkTags();
    logLine("GitHub update check: " + message);
    lock.lock();
    message_ = std::move(message);
    checking_ = false;
    last_ = nowMs();
    wake_.wait_for(lock, std::chrono::hours(6), [&] { return stopping_ || checking_; });
  }
}
}
