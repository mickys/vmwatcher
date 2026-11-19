#include <dlfcn.h>

#include <algorithm>
#include <cstring>

#include "vmw.hpp"

namespace vmw {
namespace {

// zlib is loaded at run time so the binary builds without its headers.
struct Zlib {
  int (*compress2)(unsigned char*, unsigned long*, const unsigned char*, unsigned long, int) = nullptr;
  int (*uncompress)(unsigned char*, unsigned long*, const unsigned char*, unsigned long) = nullptr;
  unsigned long (*compressBound)(unsigned long) = nullptr;
  unsigned long (*crc32)(unsigned long, const unsigned char*, unsigned int) = nullptr;
  Zlib() {
    void* lib = dlopen("libz.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) throw std::runtime_error("zlib (libz.so.1) is required");
    compress2 = reinterpret_cast<decltype(compress2)>(dlsym(lib, "compress2"));
    uncompress = reinterpret_cast<decltype(uncompress)>(dlsym(lib, "uncompress"));
    compressBound = reinterpret_cast<decltype(compressBound)>(dlsym(lib, "compressBound"));
    crc32 = reinterpret_cast<decltype(crc32)>(dlsym(lib, "crc32"));
    if (!compress2 || !uncompress || !compressBound || !crc32) throw std::runtime_error("zlib is incomplete");
  }
};
const Zlib& zlib() {
  static Zlib instance;
  return instance;
}

void be32(std::string& out, uint32_t v) {
  for (int i = 3; i >= 0; i--) out += char(v >> (8 * i));
}
void le16(std::string& out, uint32_t v) {
  out += char(v);
  out += char(v >> 8);
}
void le32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out += char(v >> (8 * i));
}
uint32_t readBe32(const std::string& s, size_t at) {
  return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) | (uint32_t(uint8_t(s[at + 2])) << 8) |
         uint8_t(s[at + 3]);
}
void chunk(std::string& out, const char* type, const std::string& data) {
  be32(out, uint32_t(data.size()));
  std::string body = std::string(type, 4) + data;
  out += body;
  be32(out, crc32Of(body));
}
void checkSize(long width, long height) {
  if (width < 1 || height < 1 || width > 16384 || height > 16384 || width * height > 16777216)
    throw std::runtime_error("Unsupported screenshot dimensions");
}

}  // namespace

uint32_t crc32Of(const std::string& data) {
  unsigned long crc = zlib().crc32(0, nullptr, 0);
  size_t done = 0;
  while (done < data.size()) {
    size_t take = std::min<size_t>(data.size() - done, 1u << 30);
    crc = zlib().crc32(crc, reinterpret_cast<const unsigned char*>(data.data()) + done, unsigned(take));
    done += take;
  }
  return uint32_t(crc);
}

Image decodePpm(const std::string& data) {
  size_t at = 0;
  auto token = [&] {
    while (at < data.size()) {
      if (data[at] == '#')
        while (at < data.size() && data[at] != '\n') at++;
      else if ((unsigned char)data[at] <= 32) at++;
      else break;
    }
    size_t start = at;
    while (at < data.size() && (unsigned char)data[at] > 32) at++;
    return data.substr(start, at - start);
  };
  auto number = [&] {
    std::string t = token();
    if (t.empty() || t.size() > 6 || t.find_first_not_of("0123456789") != std::string::npos)
      throw std::runtime_error("Unsupported screenshot header");
    return atol(t.c_str());
  };
  if (token() != "P6") throw std::runtime_error("Expected P6 screenshot");
  long width = number(), height = number(), max = number();
  checkSize(width, height);
  if (max != 255) throw std::runtime_error("Unsupported screenshot depth");
  if (data.compare(at, 2, "\r\n") == 0) at += 2;
  else at++;
  if (at > data.size() || data.size() - at != size_t(width * height * 3)) throw std::runtime_error("Truncated screenshot");
  Image image;
  image.width = int(width);
  image.height = int(height);
  image.rgb = data.substr(at);
  return image;
}

std::string encodePng(const Image& image, int level) {
  checkSize(image.width, image.height);
  size_t stride = size_t(image.width) * 3;
  if (image.rgb.size() != stride * size_t(image.height)) throw std::runtime_error("Invalid image");
  // Filter 1 (Sub) per row: desktops are mostly flat colour, which this turns into zeros.
  std::string raw((stride + 1) * size_t(image.height), '\0');
  for (int y = 0; y < image.height; y++) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(image.rgb.data()) + stride * size_t(y);
    uint8_t* dst = reinterpret_cast<uint8_t*>(&raw[(stride + 1) * size_t(y)]);
    *dst++ = 1;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    for (size_t x = 3; x < stride; x++) dst[x] = uint8_t(src[x] - src[x - 3]);
  }
  unsigned long size = zlib().compressBound(raw.size());
  std::string packed(size, '\0');
  if (zlib().compress2(reinterpret_cast<unsigned char*>(&packed[0]), &size, reinterpret_cast<const unsigned char*>(raw.data()),
                       raw.size(), level) != 0)
    throw std::runtime_error("PNG compression failed");
  packed.resize(size);
  std::string out("\x89PNG\r\n\x1a\n", 8);
  std::string header;
  be32(header, uint32_t(image.width));
  be32(header, uint32_t(image.height));
  header += std::string("\x08\x02\x00\x00\x00", 5);
  chunk(out, "IHDR", header);
  chunk(out, "IDAT", packed);
  chunk(out, "IEND", "");
  return out;
}

Image decodePng(const std::string& data) {
  if (data.size() < 33 || data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) != 0) throw std::runtime_error("Not a PNG");
  long width = 0, height = 0;
  int channels = 0;
  std::string packed;
  for (size_t at = 8; at + 12 <= data.size();) {
    size_t length = readBe32(data, at);
    std::string type = data.substr(at + 4, 4);
    if (length > data.size() || at + 12 + length > data.size()) throw std::runtime_error("Truncated PNG");
    if (type == "IHDR") {
      if (length != 13) throw std::runtime_error("Invalid PNG");
      width = long(readBe32(data, at + 8));
      height = long(readBe32(data, at + 12));
      int depth = uint8_t(data[at + 16]), color = uint8_t(data[at + 17]), interlace = uint8_t(data[at + 20]);
      channels = color == 2 ? 3 : color == 6 ? 4 : color == 0 ? 1 : 0;
      if (depth != 8 || !channels || interlace) throw std::runtime_error("Unsupported PNG format");
      checkSize(width, height);
    } else if (type == "IDAT")
      packed.append(data, at + 8, length);
    else if (type == "IEND")
      break;
    at += 12 + length;
  }
  if (!channels) throw std::runtime_error("Invalid PNG");
  size_t stride = size_t(width) * size_t(channels);
  std::string raw((stride + 1) * size_t(height), '\0');
  unsigned long size = raw.size();
  if (zlib().uncompress(reinterpret_cast<unsigned char*>(&raw[0]), &size, reinterpret_cast<const unsigned char*>(packed.data()),
                        packed.size()) != 0 ||
      size != raw.size())
    throw std::runtime_error("Corrupt PNG");
  Image image;
  image.width = int(width);
  image.height = int(height);
  image.rgb.resize(size_t(width) * size_t(height) * 3);
  std::vector<uint8_t> previous(stride, 0), current(stride);
  for (long y = 0; y < height; y++) {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(raw.data()) + (stride + 1) * size_t(y);
    int filter = row[0];
    if (filter > 4) throw std::runtime_error("Corrupt PNG");
    for (size_t x = 0; x < stride; x++) {
      int a = x >= size_t(channels) ? current[x - size_t(channels)] : 0, b = previous[x],
          c = x >= size_t(channels) ? previous[x - size_t(channels)] : 0, v = row[x + 1];
      if (filter == 1) v += a;
      else if (filter == 2) v += b;
      else if (filter == 3) v += (a + b) / 2;
      else if (filter == 4) {
        int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      current[x] = uint8_t(v);
    }
    uint8_t* out = reinterpret_cast<uint8_t*>(&image.rgb[size_t(y) * size_t(width) * 3]);
    for (long x = 0; x < width; x++) {
      const uint8_t* px = &current[size_t(x) * size_t(channels)];
      out[x * 3] = px[0];
      out[x * 3 + 1] = channels == 1 ? px[0] : px[1];
      out[x * 3 + 2] = channels == 1 ? px[0] : px[2];
    }
    previous.swap(current);
  }
  return image;
}

Image decodeScreenshot(const std::string& data) {
  if (data.compare(0, 2, "P6") == 0) return decodePpm(data);
  if (data.compare(0, 4, "\x89PNG") == 0) return decodePng(data);
  throw std::runtime_error("Unsupported screenshot format");
}

// Area-averaged downscale, so small previews of text stay readable.
Image resizeImage(const Image& image, int maxWidth) {
  if (image.width <= maxWidth) return image;
  Image out;
  out.width = maxWidth;
  out.height = std::max(1, int((int64_t(image.height) * maxWidth + image.width / 2) / image.width));
  out.rgb.resize(size_t(out.width) * size_t(out.height) * 3);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(image.rgb.data());
  for (int y = 0; y < out.height; y++) {
    int y0 = int(int64_t(y) * image.height / out.height), y1 = std::max(y0 + 1, int(int64_t(y + 1) * image.height / out.height));
    for (int x = 0; x < out.width; x++) {
      int x0 = int(int64_t(x) * image.width / out.width), x1 = std::max(x0 + 1, int(int64_t(x + 1) * image.width / out.width));
      unsigned r = 0, g = 0, b = 0, n = unsigned((y1 - y0) * (x1 - x0));
      for (int sy = y0; sy < y1; sy++) {
        const uint8_t* p = src + (size_t(sy) * size_t(image.width) + size_t(x0)) * 3;
        for (int sx = x0; sx < x1; sx++, p += 3) {
          r += p[0];
          g += p[1];
          b += p[2];
        }
      }
      uint8_t* q = reinterpret_cast<uint8_t*>(&out.rgb[(size_t(y) * size_t(out.width) + size_t(x)) * 3]);
      q[0] = uint8_t(r / n);
      q[1] = uint8_t(g / n);
      q[2] = uint8_t(b / n);
    }
  }
  return out;
}

Image combineImages(const std::vector<Image>& images) {
  if (images.empty()) throw std::runtime_error("No screenshots");
  if (images.size() == 1) return images[0];
  const int gap = 8;
  int64_t width = gap * int64_t(images.size() - 1);
  int height = 0;
  for (const auto& image : images) {
    width += image.width;
    height = std::max(height, image.height);
  }
  if (width * height > 33554432) throw std::runtime_error("Combined capture too large");
  Image out;
  out.width = int(width);
  out.height = height;
  out.rgb.assign(size_t(width) * size_t(height) * 3, char(24));
  int x = 0;
  for (const auto& image : images) {
    for (int y = 0; y < image.height; y++)
      memcpy(&out.rgb[(size_t(y) * size_t(out.width) + size_t(x)) * 3], &image.rgb[size_t(y) * size_t(image.width) * 3],
             size_t(image.width) * 3);
    x += image.width + gap;
  }
  return out;
}

std::string zipStore(const std::vector<std::pair<std::string, std::string>>& files) {
  std::string out, directory;
  for (const auto& file : files) {
    uint32_t crc = crc32Of(file.second), size = uint32_t(file.second.size()), offset = uint32_t(out.size());
    auto common = [&](std::string& target) {
      le16(target, 20);  // version needed
      le16(target, 0);   // flags
      le16(target, 0);   // stored
      le16(target, 0);   // time
      le16(target, 0x21);  // date: 1980-01-01
      le32(target, crc);
      le32(target, size);
      le32(target, size);
      le16(target, uint32_t(file.first.size()));
      le16(target, 0);
    };
    le32(out, 0x04034b50);
    common(out);
    out += file.first;
    out += file.second;
    le32(directory, 0x02014b50);
    le16(directory, 20);
    common(directory);
    le16(directory, 0);
    le16(directory, 0);
    le16(directory, 0);
    le32(directory, 0);
    le32(directory, offset);
    directory += file.first;
  }
  uint32_t start = uint32_t(out.size());
  out += directory;
  le32(out, 0x06054b50);
  le16(out, 0);
  le16(out, 0);
  le16(out, uint32_t(files.size()));
  le16(out, uint32_t(files.size()));
  le32(out, uint32_t(directory.size()));
  le32(out, start);
  le16(out, 0);
  return out;
}

}  // namespace vmw
