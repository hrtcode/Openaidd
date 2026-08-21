// json.cpp
#include "json.hpp"
#include <stdexcept>
#include <cstdio>
#include <cstdlib>

namespace json {

struct Parser {
  const std::string& s;
  size_t i = 0;
  explicit Parser(const std::string& str) : s(str) {}

  [[noreturn]] void err(const std::string& m) {
    throw std::runtime_error("JSON parse error at " + std::to_string(i) + ": " + m);
  }

  void skipWs() {
    while (i < s.size()) {
      char c = s[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') i++;
      else break;
    }
  }

  Value parse() {
    skipWs();
    Value v = parseValue();
    skipWs();
    if (i != s.size()) err("trailing characters");
    return v;
  }

  Value parseValue() {
    skipWs();
    if (i >= s.size()) err("unexpected end");
    char c = s[i];
    switch (c) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return Value(parseString());
      case 't': case 'f': return parseBool();
      case 'n': return parseNull();
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        err("unexpected character");
    }
    return Value();
  }

  Value parseObject() {
    Value v = Value::object();
    i++; // {
    skipWs();
    if (i < s.size() && s[i] == '}') { i++; return v; }
    while (true) {
      skipWs();
      if (i >= s.size() || s[i] != '"') err("expected key string");
      std::string key = parseString();
      skipWs();
      if (i >= s.size() || s[i] != ':') err("expected ':'");
      i++;
      Value val = parseValue();
      v.obj[key] = std::move(val);
      skipWs();
      if (i >= s.size()) err("unexpected end in object");
      if (s[i] == ',') { i++; continue; }
      if (s[i] == '}') { i++; break; }
      err("expected ',' or '}'");
    }
    return v;
  }

  Value parseArray() {
    Value v = Value::array();
    i++; // [
    skipWs();
    if (i < s.size() && s[i] == ']') { i++; return v; }
    while (true) {
      Value val = parseValue();
      v.arr.push_back(std::move(val));
      skipWs();
      if (i >= s.size()) err("unexpected end in array");
      if (s[i] == ',') { i++; continue; }
      if (s[i] == ']') { i++; break; }
      err("expected ',' or ']'");
    }
    return v;
  }

  std::string parseString() {
    // 前置假设 s[i] == '"'
    i++; // "
    std::string out;
    while (i < s.size()) {
      char c = s[i++];
      if (c == '"') return out;
      if (c == '\\') {
        if (i >= s.size()) err("bad escape");
        char e = s[i++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (i + 4 > s.size()) err("bad unicode escape");
            unsigned code = 0;
            for (int k = 0; k < 4; k++) {
              char h = s[i++];
              code <<= 4;
              if (h >= '0' && h <= '9') code |= (h - '0');
              else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
              else err("bad hex");
            }
            // 仅处理基本平面（够用）
            char buf[8];
            int n = snprintf(buf, sizeof(buf), "%c", (char)code);
            if (n > 0) out.append(buf, n);
            break;
          }
          default: err("unknown escape");
        }
      } else {
        out.push_back(c);
      }
    }
    err("unterminated string");
    return out;
  }

  Value parseNumber() {
    size_t start = i;
    if (i < s.size() && s[i] == '-') i++;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' ||
           s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) i++;
    std::string num = s.substr(start, i - start);
    if (num.empty()) err("bad number");
    Value v;
    v.type = Value::Number;
    v.num = atof(num.c_str());
    return v;
  }

  Value parseBool() {
    if (s.compare(i, 4, "true") == 0) { i += 4; return Value(true); }
    if (s.compare(i, 5, "false") == 0) { i += 5; return Value(false); }
    err("bad literal");
    return Value();
  }

  Value parseNull() {
    if (s.compare(i, 4, "null") == 0) { i += 4; return Value(nullptr); }
    err("bad literal");
    return Value();
  }
};

Value parse(const std::string& text) {
  Parser p(text);
  return p.parse();
}

static void serializeValue(const Value& v, std::string& out, bool pretty, int depth) {
  std::string indent(depth * 2, ' ');
  std::string indent1((depth + 1) * 2, ' ');
  switch (v.type) {
    case Value::Null: out += "null"; break;
    case Value::Bool: out += v.b ? "true" : "false"; break;
    case Value::Number: {
      double n = v.num;
      if (n == (long long)n && n >= -1e15 && n <= 1e15) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)n);
        out += buf;
      } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", n);
        out += buf;
      }
      break;
    }
    case Value::String: {
      out.push_back('"');
      for (char c : v.str) {
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\n': out += "\\n"; break;
          case '\t': out += "\\t"; break;
          case '\r': out += "\\r"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          default:
            if ((unsigned char)c < 0x20) {
              char buf[8];
              snprintf(buf, sizeof(buf), "\\u%04x", c);
              out += buf;
            } else out.push_back(c);
        }
      }
      out.push_back('"');
      break;
    }
    case Value::Array: {
      if (v.arr.empty()) { out += "[]"; break; }
      out.push_back('[');
      if (pretty) out.push_back('\n');
      for (size_t k = 0; k < v.arr.size(); k++) {
        if (pretty) out += indent1;
        serializeValue(v.arr[k], out, pretty, depth + 1);
        if (k + 1 < v.arr.size()) out.push_back(',');
        if (pretty) out.push_back('\n'); else out.push_back(' ');
      }
      if (pretty) { out += indent; out.push_back(']'); }
      else out[out.size() - 1] = ']';
      break;
    }
    case Value::Object: {
      if (v.obj.empty()) { out += "{}"; break; }
      out.push_back('{');
      if (pretty) out.push_back('\n');
      bool first = true;
      for (auto& kv : v.obj) {
        if (pretty) out += indent1;
        if (!first) out.push_back(',');
        // key
        out.push_back('"');
        for (char c : kv.first) {
          if (c == '"') out += "\\\"";
          else if (c == '\\') out += "\\\\";
          else out.push_back(c);
        }
        out += "\":";
        if (pretty) out.push_back(' ');
        serializeValue(kv.second, out, pretty, depth + 1);
        if (pretty) out.push_back('\n'); else out.push_back(' ');
        first = false;
      }
      if (pretty) { out += indent; out.push_back('}'); }
      else { out[out.size() - 1] = '}'; }
      break;
    }
  }
}

std::string serialize(const Value& v, bool pretty) {
  std::string out;
  serializeValue(v, out, pretty, 0);
  return out;
}

}  // namespace json
