// json.hpp - 零依赖最小 JSON 库（解析 + 序列化）
#pragma once
#include <string>
#include <vector>
#include <map>
#include <initializer_list>

namespace json {

struct Value {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Value> arr;
  std::map<std::string, Value> obj;

  Value() : type(Null) {}
  Value(std::nullptr_t) : type(Null) {}
  Value(bool v) : type(Bool), b(v) {}
  Value(int v) : type(Number), num((double)v) {}
  Value(long v) : type(Number), num((double)v) {}
  Value(double v) : type(Number), num(v) {}
  Value(const char* v) : type(String), str(v ? v : "") {}
  Value(const std::string& v) : type(String), str(v) {}
  Value(std::vector<Value> v) : type(Array), arr(std::move(v)) {}
  Value(std::initializer_list<Value> items) : type(Array) {
    for (auto& x : items) arr.push_back(std::move(x));
  }
  Value(std::initializer_list<std::pair<std::string, Value>> items) : type(Object) {
    for (auto& kv : items) obj[kv.first] = std::move(kv.second);
  }

  bool isNull()   const { return type == Null; }
  bool isBool()   const { return type == Bool; }
  bool isNumber() const { return type == Number; }
  bool isString() const { return type == String; }
  bool isArray()  const { return type == Array; }
  bool isObject() const { return type == Object; }

  const std::string& asString() const { return str; }
  bool asBool() const { return b; }
  double asDouble() const { return num; }
  long asLong() const { return (long)num; }
  int asInt() const { return (int)num; }

  const std::vector<Value>& asArray() const { return arr; }
  const std::map<std::string, Value>& asObject() const { return obj; }

  Value& operator[](const std::string& k) { type = Object; return obj[k]; }
  bool has(const std::string& k) const {
    return type == Object && obj.find(k) != obj.end();
  }
  Value get(const std::string& k, const Value& def = {}) const {
    if (type != Object) return def;
    auto it = obj.find(k);
    return it == obj.end() ? def : it->second;
  }
  // 便捷：取对象里字符串字段
  std::string getStr(const std::string& k, const std::string& def = "") const {
    auto it = obj.find(k);
    if (it == obj.end() || it->second.type != String) return def;
    return it->second.str;
  }
  long getLong(const std::string& k, long def = 0) const {
    auto it = obj.find(k);
    if (it == obj.end() || it->second.type != Number) return def;
    return (long)it->second.num;
  }

  // 构造助手
  static Value object() { Value v; v.type = Object; return v; }
  static Value array() { Value v; v.type = Array; return v; }
  Value& set(const std::string& k, Value v) {
    type = Object;
    obj[k] = std::move(v);
    return *this;
  }
  Value& push(Value v) {
    type = Array;
    arr.push_back(std::move(v));
    return *this;
  }
};

// 解析整个字符串；失败抛 std::runtime_error
Value parse(const std::string& text);

// 便捷：从 {key,value} 列表构造对象（避免 Value({...}) 的构造歧义）
inline Value object(std::initializer_list<std::pair<std::string, Value>> items) {
  Value v;
  v.type = Value::Object;
  for (auto& kv : items) v.obj[std::move(kv.first)] = std::move(kv.second);
  return v;
}

// 序列化（pretty=true 时缩进）
std::string serialize(const Value& v, bool pretty = false);

}  // namespace json
