#include <cstring>

#include "vmw.hpp"

namespace vmw {
namespace {

bool rfbVersion(const std::string& v) {
  return v == "RFB 003.003\n" || v == "RFB 003.007\n" || v == "RFB 003.008\n";
}
uint32_t be32(const std::string& s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8) |
         uint8_t(s[at + 3]);
}
uint32_t be16(const std::string& s, size_t at) { return (uint32_t(uint8_t(s[at])) << 8) | uint8_t(s[at + 1]); }

}  // namespace

void ReadOnlyRfb::observeServer(const char* data, size_t n) {
  if (security33_ != -2) return;
  server_.append(data, std::min<size_t>(n, 32 - std::min<size_t>(32, server_.size())));
  if (server_.size() >= 12 && server_.compare(0, 12, "RFB 003.003\n") != 0) {
    security33_ = -1;
    server_.clear();
    return;
  }
  if (server_.size() >= 16) security33_ = long(be32(server_, 12));
}

void ReadOnlyRfb::selectSecurity(int type) {
  if (type != 1 && type != 2) throw std::runtime_error("Read-only mode supports None or VNC password authentication");
  state_ = type == 2 ? Auth : Init;
}

std::string ReadOnlyRfb::push(const std::string& data) {
  buffer_ += data;
  const size_t limit = 16u << 20;
  if (buffer_.size() > limit) throw std::runtime_error("RFB message too large");
  std::string output;
  size_t at = 0;  // consumed prefix of buffer_
  auto left = [&] { return buffer_.size() - at; };
  while (left()) {
    size_t length = 0;
    bool allow = true;
    if (state_ == Version) {
      if (left() < 12) break;
      std::string version = buffer_.substr(at, 12);
      if (!rfbVersion(version)) throw std::runtime_error("Unsupported RFB version");
      length = 12;
      state_ = version == "RFB 003.003\n" ? Security33 : Security;
    } else if (state_ == Security33) {
      // RFB 3.3: the server alone picks the security type, and the client sends nothing.
      if (security33_ == -2) break;
      selectSecurity(int(security33_));
      continue;
    } else if (state_ == Security) {
      length = 1;
      selectSecurity(uint8_t(buffer_[at]));
    } else if (state_ == Auth) {
      length = 16;
      if (left() < length) break;
      state_ = Init;
    } else if (state_ == Init) {
      length = 1;
      output += char(1);  // always join as a shared viewer
      allow = false;
      state_ = Messages;
    } else {
      int type = uint8_t(buffer_[at]);
      if (type == 0) length = 20;  // SetPixelFormat
      else if (type == 2) {        // SetEncodings
        if (left() < 4) break;
        uint32_t n = be16(buffer_, at + 2);
        if (n > 512) throw std::runtime_error("Too many encodings");
        length = 4 + 4 * n;
      } else if (type == 3) length = 10;  // FramebufferUpdateRequest
      else if (type == 4) {               // KeyEvent
        length = 8;
        allow = false;
      } else if (type == 5) {  // PointerEvent
        length = 6;
        allow = false;
      } else if (type == 6) {  // ClientCutText (negative length: extended clipboard)
        if (left() < 8) break;
        int32_t n = int32_t(be32(buffer_, at + 4));
        length = 8 + size_t(n < 0 ? -int64_t(n) : int64_t(n));
        allow = false;
      } else if (type == 150) length = 10;  // EnableContinuousUpdates
      else if (type == 248) {               // ClientFence
        if (left() < 9) break;
        length = 9 + uint8_t(buffer_[at + 8]);
      } else if (type == 250) {  // xvp (power)
        length = 4;
        allow = false;
      } else if (type == 251) {  // SetDesktopSize
        if (left() < 8) break;
        length = 8 + 16 * size_t(uint8_t(buffer_[at + 6]));
        allow = false;
      } else if (type == 255) {  // QEMU extended key event
        if (left() < 2) break;
        if (buffer_[at + 1] != 0) throw std::runtime_error("Unsupported QEMU client message");
        length = 12;
        allow = false;
      } else
        throw std::runtime_error("Unsupported read-only RFB message " + std::to_string(type));
    }
    if (length > limit) throw std::runtime_error("RFB message too large");
    if (left() < length) break;
    if (allow) output.append(buffer_, at, length);
    at += length;
  }
  buffer_.erase(0, at);
  return output;
}

// QEMU VNC audio extension: ui/vnc.c and ui/vnc.h.
void AudioRfb::push(const char* data, size_t size) {
  buffer_.append(data, size);
  if (buffer_.size() > (2u << 20)) throw std::runtime_error("Audio protocol buffer too large");
  size_t at = 0;
  auto left = [&] { return buffer_.size() - at; };
  for (;;) {
    size_t n = 0;
    if (state_ == Version) {
      n = 12;
      if (left() < n) break;
      std::string version = buffer_.substr(at, 12);
      if (!rfbVersion(version)) throw std::runtime_error("Unsupported audio console protocol");
      if (version == "RFB 003.003\n") throw std::runtime_error("Audio requires an RFB 3.7 or newer console");
      write("RFB 003.008\n");
      state_ = Security;
    } else if (state_ == Security) {
      if (left() < 1) break;
      size_t count = uint8_t(buffer_[at]);
      n = 1 + count;
      if (left() < n) break;
      if (!count || buffer_.substr(at + 1, count).find(char(1)) == std::string::npos)
        throw std::runtime_error("Audio is unavailable for this console authentication mode");
      write(std::string(1, char(1)));
      state_ = Result;
    } else if (state_ == Result) {
      n = 4;
      if (left() < n) break;
      if (be32(buffer_, at) != 0) throw std::runtime_error("Audio console authentication failed");
      write(std::string(1, char(1)));  // shared session
      state_ = Init;
    } else if (state_ == Init) {
      if (left() < 24) break;
      n = 24 + size_t(be32(buffer_, at + 20));
      if (n > 65536) throw std::runtime_error("Audio console name too long");
      if (left() < n) break;
      // SetEncodings: only the QEMU audio pseudo-encoding (-259).
      write(std::string("\x02\x00\x00\x01\xff\xff\xfe\xfd", 8));
      state_ = Messages;
    } else {
      if (!left()) break;
      int type = uint8_t(buffer_[at]);
      if (type == 0) {
        if (left() < 4) break;
        uint32_t count = be16(buffer_, at + 2);
        if (count > 16) throw std::runtime_error("Unexpected audio framebuffer update");
        n = 4 + count * 12;
        if (left() < n) break;
        for (uint32_t i = 0; i < count; i++)
          if (int32_t(be32(buffer_, at + 4 + i * 12 + 8)) != -259)
            throw std::runtime_error("Console does not support audio-only playback");
        if (count && !enabled) {
          enabled = true;
          // Set format (signed 16-bit, stereo, 48 kHz), then enable audio capture.
          std::string format("\xff\x01\x00\x02\x03\x02\x00\x00\x00\x00", 10);
          format[6] = char(48000u >> 24);
          format[7] = char(48000u >> 16);
          format[8] = char(48000u >> 8);
          format[9] = char(48000u & 255);
          write(format + std::string("\xff\x01\x00\x00", 4));
          ready();
        }
      } else if (type == 255) {
        if (left() < 4) break;
        if (buffer_[at + 1] != 1) throw std::runtime_error("Unexpected audio extension");
        uint32_t kind = be16(buffer_, at + 2);
        n = 4;
        if (kind == 2) {
          if (left() < 8) break;
          uint32_t length = be32(buffer_, at + 4);
          if (length > (1u << 20) || length % 4) throw std::runtime_error("Invalid audio packet");
          n = 8 + length;
          if (left() < n) break;
          if (!enabled) throw std::runtime_error("Audio received before negotiation");
          pcm(buffer_.substr(at + 8, length));
        } else if (kind != 0 && kind != 1)
          throw std::runtime_error("Unknown audio packet");
      } else if (type == 2) {
        n = 1;  // bell
      } else if (type == 3) {
        if (left() < 8) break;
        n = 8 + size_t(be32(buffer_, at + 4));
        if (n > 65536) throw std::runtime_error("Console clipboard too large");
        if (left() < n) break;
      } else
        throw std::runtime_error("Unexpected audio console message");
    }
    at += n;
  }
  buffer_.erase(0, at);
}

}  // namespace vmw
