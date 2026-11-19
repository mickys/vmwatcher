#pragma once
#include "vmw.hpp"
#include <condition_variable>

namespace vmw {
class UpdateChecker {
 public:
  ~UpdateChecker();
  void start();
  Json status(bool request);

 private:
  void run();
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stopping_ = false, checking_ = false;
  int64_t last_ = 0;
  std::string message_ = "Automatic checks are disabled in demo mode.";
};
}
