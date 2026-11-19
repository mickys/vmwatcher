#include "vmw.hpp"

namespace vmw {
namespace {

struct XmlParser {
  const std::string& text;
  size_t at = 0;
  [[noreturn]] void bad() { throw std::runtime_error("Invalid XML"); }
  static std::string unescape(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i] != '&') {
        out += raw[i];
        continue;
      }
      size_t end = raw.find(';', i);
      if (end == std::string::npos || end - i > 10) {
        out += raw[i];
        continue;
      }
      std::string name = raw.substr(i + 1, end - i - 1);
      if (name == "amp") out += '&';
      else if (name == "lt") out += '<';
      else if (name == "gt") out += '>';
      else if (name == "quot") out += '"';
      else if (name == "apos") out += '\'';
      else if (name.size() > 1 && name[0] == '#') {
        long code = name[1] == 'x' ? strtol(name.c_str() + 2, nullptr, 16) : strtol(name.c_str() + 1, nullptr, 10);
        if (code > 0 && code < 128) out += char(code);
        else out += '?';
      } else
        out += raw.substr(i, end - i + 1);
      i = end;
    }
    return out;
  }
  void space() {
    while (at < text.size() && (unsigned char)text[at] <= 32) at++;
  }
  // Skips declarations, comments and doctype blocks between elements.
  void misc() {
    for (;;) {
      space();
      if (text.compare(at, 4, "<!--") == 0) {
        size_t end = text.find("-->", at);
        if (end == std::string::npos) bad();
        at = end + 3;
      } else if (text.compare(at, 2, "<?") == 0) {
        size_t end = text.find("?>", at);
        if (end == std::string::npos) bad();
        at = end + 2;
      } else if (text.compare(at, 2, "<!") == 0 && text.compare(at, 9, "<![CDATA[") != 0) {
        size_t end = text.find('>', at);
        if (end == std::string::npos) bad();
        at = end + 1;
      } else
        return;
    }
  }
  std::string name() {
    size_t start = at;
    while (at < text.size()) {
      unsigned char c = (unsigned char)text[at];
      if (c <= 32 || c == '>' || c == '/' || c == '=') break;
      at++;
    }
    if (at == start) bad();
    return text.substr(start, at - start);
  }
  XmlNode element(int depth) {
    if (depth > 48 || at >= text.size() || text[at] != '<') bad();
    at++;
    XmlNode node;
    node.name = name();
    for (;;) {
      space();
      if (at >= text.size()) bad();
      if (text[at] == '/') {
        if (text.compare(at, 2, "/>") != 0) bad();
        at += 2;
        return node;
      }
      if (text[at] == '>') {
        at++;
        break;
      }
      std::string key = name();
      space();
      if (at >= text.size() || text[at] != '=') bad();
      at++;
      space();
      if (at >= text.size() || (text[at] != '"' && text[at] != '\'')) bad();
      char quote = text[at++];
      size_t end = text.find(quote, at);
      if (end == std::string::npos) bad();
      node.attrs[key] = unescape(text.substr(at, end - at));
      at = end + 1;
    }
    for (;;) {
      size_t open = text.find('<', at);
      if (open == std::string::npos) bad();
      node.text += unescape(text.substr(at, open - at));
      at = open;
      if (text.compare(at, 9, "<![CDATA[") == 0) {
        size_t end = text.find("]]>", at);
        if (end == std::string::npos) bad();
        node.text += text.substr(at + 9, end - at - 9);
        at = end + 3;
      } else if (text.compare(at, 2, "</") == 0) {
        at += 2;
        if (name() != node.name) bad();
        space();
        if (at >= text.size() || text[at] != '>') bad();
        at++;
        node.text = trim(node.text);
        return node;
      } else if (text.compare(at, 4, "<!--") == 0 || text.compare(at, 2, "<?") == 0) {
        misc();
      } else {
        if (node.children.size() > 20000) bad();
        node.children.push_back(element(depth + 1));
      }
    }
  }
};

}  // namespace

const XmlNode* XmlNode::child(const std::string& n) const {
  for (const auto& c : children)
    if (c.name == n) return &c;
  return nullptr;
}
std::vector<const XmlNode*> XmlNode::all(const std::string& n) const {
  std::vector<const XmlNode*> out;
  for (const auto& c : children)
    if (c.name == n) out.push_back(&c);
  return out;
}
std::string XmlNode::attr(const std::string& n, const std::string& fallback) const {
  auto found = attrs.find(n);
  return found == attrs.end() ? fallback : found->second;
}

XmlNode parseXml(const std::string& text) {
  if (text.size() > (8u << 20)) throw std::runtime_error("XML too large");
  XmlParser parser{text};
  parser.misc();
  return parser.element(0);
}

}  // namespace vmw
