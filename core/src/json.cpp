#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "vmw.hpp"

namespace vmw {
namespace {

const Json kNull;
const std::string kEmptyString;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

struct Parser {
  const std::string& text;
  size_t at = 0;
  [[noreturn]] void bad() { throw HttpError(400, "Invalid JSON"); }
  void space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) at++;
  }
  bool take(const char* word) {
    size_t n = std::char_traits<char>::length(word);
    if (text.compare(at, n, word) != 0) return false;
    at += n;
    return true;
  }
  unsigned hex4() {
    if (at + 4 > text.size()) bad();
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
      char c = text[at++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else bad();
    }
    return v;
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) {
      out += char(0xc0 | (cp >> 6));
      out += char(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      out += char(0xe0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 63));
      out += char(0x80 | (cp & 63));
    } else {
      out += char(0xf0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 63));
      out += char(0x80 | ((cp >> 6) & 63));
      out += char(0x80 | (cp & 63));
    }
  }
  std::string string() {
    if (text[at] != '"') bad();
    at++;
    std::string out;
    for (;;) {
      if (at >= text.size()) bad();
      unsigned char c = (unsigned char)text[at++];
      if (c == '"') return out;
      if (c < 0x20) bad();
      if (c != '\\') {
        out += char(c);
        continue;
      }
      if (at >= text.size()) bad();
      char e = text[at++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xd800 && cp < 0xdc00 && text.compare(at, 2, "\\u") == 0) {
            at += 2;
            unsigned low = hex4();
            if (low < 0xdc00 || low > 0xdfff) bad();
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          }
          utf8(out, cp);
          break;
        }
        default: bad();
      }
    }
  }
  Json value(int depth) {
    if (depth > 64) bad();
    space();
    if (at >= text.size()) bad();
    char c = text[at];
    if (c == '{') {
      at++;
      Json::Object object;
      space();
      if (at < text.size() && text[at] == '}') {
        at++;
        return object;
      }
      for (;;) {
        space();
        if (at >= text.size()) bad();
        std::string key = string();
        space();
        if (at >= text.size() || text[at++] != ':') bad();
        object[key] = value(depth + 1);
        space();
        if (at >= text.size()) bad();
        if (text[at] == ',') {
          at++;
          continue;
        }
        if (text[at++] != '}') bad();
        return object;
      }
    }
    if (c == '[') {
      at++;
      Json::Array array;
      space();
      if (at < text.size() && text[at] == ']') {
        at++;
        return array;
      }
      for (;;) {
        array.push_back(value(depth + 1));
        space();
        if (at >= text.size()) bad();
        if (text[at] == ',') {
          at++;
          continue;
        }
        if (text[at++] != ']') bad();
        return array;
      }
    }
    if (c == '"') return string();
    if (take("true")) return true;
    if (take("false")) return false;
    if (take("null")) return nullptr;
    size_t start = at;
    if (at < text.size() && text[at] == '-') at++;
    while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || text[at] == '.' || text[at] == 'e' ||
                                text[at] == 'E' || text[at] == '+' || text[at] == '-'))
      at++;
    if (at == start) bad();
    std::string number = text.substr(start, at - start);
    char* end = nullptr;
    double v = strtod(number.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) bad();
    return v;
  }
};

void quote(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          snprintf(buffer, sizeof buffer, "\\u%04x", c);
          out += buffer;
        } else
          out += char(c);
    }
  }
  out += '"';
}

}  // namespace

Json Json::parse(const std::string& text) {
  Parser parser{text};
  Json result = parser.value(0);
  parser.space();
  if (parser.at != text.size()) parser.bad();
  return result;
}

bool Json::isInt() const { return type_ == Number && std::floor(n_) == n_ && std::fabs(n_) < 9e15; }
const std::string& Json::str() const { return type_ == String ? s_ : kEmptyString; }
const Json::Array& Json::arr() const { return type_ == Arr ? *a_ : kEmptyArray; }
const Json::Object& Json::obj() const { return type_ == Obj ? *o_ : kEmptyObject; }
const Json& Json::operator[](const std::string& key) const {
  if (type_ != Obj) return kNull;
  auto found = o_->find(key);
  return found == o_->end() ? kNull : found->second;
}
bool Json::has(const std::string& key) const { return type_ == Obj && o_->count(key) > 0; }

// Copies share their containers until one of them is modified.
void Json::own() {
  if (type_ == Obj && o_.use_count() > 1) o_ = std::make_shared<Object>(*o_);
  if (type_ == Arr && a_.use_count() > 1) a_ = std::make_shared<Array>(*a_);
}
Json& Json::set(const std::string& key, Json value) {
  if (type_ != Obj) {
    *this = Json(Object{});
  }
  own();
  (*o_)[key] = std::move(value);
  return *this;
}
void Json::push(Json value) {
  if (type_ != Arr) *this = Json(Array{});
  own();
  a_->push_back(std::move(value));
}
void Json::erase(const std::string& key) {
  if (type_ != Obj) return;
  own();
  o_->erase(key);
}

void Json::write(std::string& out, int indent, int depth) const {
  auto newline = [&](int d) {
    if (indent < 0) return;
    out += '\n';
    out.append(size_t(indent * d), ' ');
  };
  switch (type_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Number: {
      char buffer[40];
      if (isInt()) snprintf(buffer, sizeof buffer, "%lld", (long long)n_);
      else snprintf(buffer, sizeof buffer, "%.17g", n_);
      out += buffer;
      break;
    }
    case String: quote(out, s_); break;
    case Arr: {
      if (a_->empty()) {
        out += "[]";
        break;
      }
      out += '[';
      bool first = true;
      for (const auto& item : *a_) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        item.write(out, indent, depth + 1);
      }
      newline(depth);
      out += ']';
      break;
    }
    case Obj: {
      if (o_->empty()) {
        out += "{}";
        break;
      }
      out += '{';
      bool first = true;
      for (const auto& entry : *o_) {
        if (!first) out += ',';
        first = false;
        newline(depth + 1);
        quote(out, entry.first);
        out += indent < 0 ? ":" : ": ";
        entry.second.write(out, indent, depth + 1);
      }
      newline(depth);
      out += '}';
      break;
    }
  }
}

std::string Json::dump(int indent) const {
  std::string out;
  write(out, indent, 0);
  return out;
}

}  // namespace vmw
