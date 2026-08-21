// session.cpp - 会话持久化实现
#include "session.hpp"
#include "json.hpp"
#include "utils.hpp"
#include <cstdio>
#include <cstdlib>

namespace session {

std::string sessions_dir() {
  // 历史会话统一保存在 exe 旁边的 resume 目录（发布版即 d:/openaidd/resume），
  // 与 workspace 解耦：换 workspace / WebUI 打开时都看这里。
  std::string d = util::exe_dir();
  for (auto& c : d) if (c == '\\') c = '/';
  if (!d.empty() && d.back() != '/') d += '/';
  return d + "resume";
}

// ---------- 消息 <-> JSON ----------

static json::Value message_to_json(const prov::Message& m) {
  json::Value jm = json::Value::object();
  jm.set("role", m.role);
  json::Value items = json::Value::array();
  for (const auto& it : m.items) {
    json::Value j = json::Value::object();
    if (it.type == prov::ContentItem::Text) {
      j.set("type", "text");
      j.set("text", it.text);
    } else if (it.type == prov::ContentItem::ToolUse) {
      j.set("type", "tool_use");
      j.set("id", it.id);
      j.set("name", it.name);
      j.set("input", it.input.isNull() ? json::Value::object() : it.input);
    } else {
      j.set("type", "tool_result");
      j.set("tool_use_id", it.tool_use_id);
      j.set("is_error", it.is_error);
      j.set("text", it.text);
    }
    items.push(std::move(j));
  }
  jm.set("items", std::move(items));
  return jm;
}

static prov::Message message_from_json(const json::Value& jm) {
  prov::Message m;
  m.role = jm.getStr("role", "user");
  auto items = jm.get("items", json::Value::array());
  if (items.type == json::Value::Array) {
    for (const auto& j : items.arr) {
      prov::ContentItem it;
      std::string t = j.getStr("type", "text");
      if (t == "tool_use") {
        it.type = prov::ContentItem::ToolUse;
        it.id = j.getStr("id");
        it.name = j.getStr("name");
        it.input = j.get("input", json::Value::object());
      } else if (t == "tool_result") {
        it.type = prov::ContentItem::ToolResult;
        it.tool_use_id = j.getStr("tool_use_id");
        it.is_error = j.has("is_error") && j.get("is_error").asBool();
        it.text = j.getStr("text");
      } else {
        it.type = prov::ContentItem::Text;
        it.text = j.getStr("text");
      }
      m.items.push_back(std::move(it));
    }
  }
  return m;
}

// ---------- 文件读写 ----------

static std::string timestamp_compact() {
  // "2026-08-21 15:14:33" -> "20260821151433"
  std::string s = util::now_string();
  std::string out;
  for (char c : s)
    if (c >= '0' && c <= '9') out += c;
  return out;
}

std::string new_session_file() {
  std::string dir = sessions_dir();
  util::ensure_dir(dir);
  std::string base = timestamp_compact();
  std::string file = dir + "/" + base + ".json";
  std::string tmp;
  for (int i = 1; util::read_file(file, tmp); i++) {
    file = dir + "/" + base + "_" + std::to_string(i) + ".json";
    if (i > 999) break;
  }
  return file;
}

bool save(const std::string& file, const std::string& model,
          const std::vector<prov::Message>& history) {
  json::Value root = json::Value::object();
  root.set("version", 1);
  root.set("model", model);
  root.set("saved_at", util::now_string());
  json::Value msgs = json::Value::array();
  for (const auto& m : history) msgs.push(message_to_json(m));
  root.set("messages", std::move(msgs));
  return util::write_file(file, json::serialize(root));
}

bool load(const std::string& file, std::vector<prov::Message>& history,
          std::string& model, std::string* saved_at) {
  std::string text;
  if (!util::read_file(file, text)) return false;
  try {
    json::Value root = json::parse(text);
    if (!root.isObject()) return false;
    model = root.getStr("model");
    if (saved_at) *saved_at = root.getStr("saved_at");
    auto msgs = root.get("messages", json::Value::array());
    if (msgs.type != json::Value::Array) return false;
    history.clear();
    for (const auto& m : msgs.arr) history.push_back(message_from_json(m));
    return true;
  } catch (...) {
    return false;
  }
}

// ---------- 列表 ----------

static Meta meta_from_file(const std::string& dir, const std::string& name) {
  Meta mt;
  mt.file = dir + "/" + name;
  mt.id = name;
  if (mt.id.size() > 5 && mt.id.substr(mt.id.size() - 5) == ".json")
    mt.id = mt.id.substr(0, mt.id.size() - 5);

  std::vector<prov::Message> hist;
  std::string model, saved_at;
  if (load(mt.file, hist, model, &saved_at)) {
    mt.model = model;
    mt.saved_at = saved_at;
    for (const auto& m : hist) {
      if (m.role != "user") continue;
      bool hasText = false;
      for (const auto& it : m.items) {
        if (it.type == prov::ContentItem::Text && !it.text.empty()) {
          hasText = true;
          if (mt.preview.empty()) {
            std::string p = it.text;
            // 去掉换行，截断到 36 个字符（按字节，容错多字节截断）
            for (auto& c : p) if (c == '\n' || c == '\r') c = ' ';
            if (p.size() > 36) p = p.substr(0, 36) + "…";
            mt.preview = p;
          }
        }
      }
      if (hasText) mt.turns++;
    }
  }
  return mt;
}

std::vector<Meta> list() {
  std::string dir = sessions_dir();
  std::vector<Meta> out;
  auto names = util::list_dir(dir);
  // 文件名是时间戳，倒序 = 最新在前
  for (auto it = names.rbegin(); it != names.rend(); ++it) {
    const std::string& name = *it;
    if (name.size() < 6 || name.substr(name.size() - 5) != ".json") continue;
    Meta mt = meta_from_file(dir, name);
    // 无效文件（解析失败且无预览无轮数）跳过
    if (mt.turns == 0 && mt.preview.empty()) continue;
    out.push_back(std::move(mt));
  }
  return out;
}

}  // namespace session
