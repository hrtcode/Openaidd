// config.cpp
#include "config.hpp"
#include "json.hpp"
#include "utils.hpp"
#include <cstdlib>
#include <map>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif

namespace cfg {

// 当前目录的配置文件（保存用正式名称；读取时兼容旧版 agentconfig.toml）
std::string config_path() {
  return "openaidd.toml";
}

std::string legacy_config_path() {
  return util::home_dir() + "/.agentcli/config.json";
}

// ---- 最小 TOML 子集：仅顶层 key = value（字符串/数字/布尔，# 注释）----
static void unquote(std::string& v) {
  v = util::trim(v);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    v = v.substr(1, v.size() - 2);
    // 反转义常见转义符
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
      if (v[i] == '\\' && i + 1 < v.size()) {
        char n = v[++i];
        if (n == 'n') out += '\n';
        else if (n == 't') out += '\t';
        else if (n == '"') out += '"';
        else if (n == '\\') out += '\\';
        else { out += '\\'; out += n; }
      } else {
        out += v[i];
      }
    }
    v = out;
  } else if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
    v = v.substr(1, v.size() - 2);
  }
}

// 解析 TOML 文本到 key->value 映射（顶层扁平键）
static void parse_toml_kv(const std::string& text, std::map<std::string, std::string>& kv) {
  std::string line;
  std::string section;
  size_t i = 0;
  while (i <= text.size()) {
    size_t nl = text.find('\n', i);
    if (nl == std::string::npos) nl = text.size();
    line = text.substr(i, nl - i);
    i = nl + 1;

    std::string t = util::trim(line);
    if (t.empty() || t[0] == '#') continue;
    if (t[0] == '[') {
      size_t close = t.find(']');
      if (close != std::string::npos) section = util::trim(t.substr(1, close - 1));
      continue;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = util::trim(t.substr(0, eq));
    std::string val = util::trim(t.substr(eq + 1));
    // 去掉行尾注释（引号内 # 不做特殊处理，够用）
    if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'')) {
      char q = val.front();
      size_t qend = std::string::npos;
      for (size_t j = 1; j < val.size(); j++) {
        if (val[j] == q && (j == 0 || val[j - 1] != '\\')) { qend = j; break; }
      }
      if (qend != std::string::npos) val = val.substr(0, qend + 1);
    } else {
      size_t hash = val.find('#');
      if (hash != std::string::npos) val = util::trim(val.substr(0, hash));
    }
    unquote(val);
    std::string full = section.empty() ? key : section + "." + key;
    kv[full] = val;
  }
}

static std::string toml_quote(const std::string& s) {
  std::string r = "\"";
  for (char ch : s) {
    if (ch == '\\') r += "\\\\";
    else if (ch == '"') r += "\\\"";
    else if (ch == '\n') r += "\\n";
    else if (ch == '\t') r += "\\t";
    else r += ch;
  }
  r += '"';
  return r;
}

// 从当前目录 agentconfig.toml 读配置；成功返回 true
static bool load_toml(const std::string& path, Config& c) {
  std::string text;
  if (!util::read_file(path, text)) return false;
  std::map<std::string, std::string> kv;
  parse_toml_kv(text, kv);
  auto get = [&](const char* k) -> std::string {
    auto it = kv.find(k);
    return it == kv.end() ? std::string() : it->second;
  };
  std::string p = get("provider");
  if (!p.empty()) c.provider = p;
  if (!get("model").empty()) c.model = get("model");
  if (!get("api_key").empty()) c.api_key = get("api_key");
  if (!get("base_url").empty()) c.base_url = get("base_url");
  auto mt = kv.find("max_tokens");
  if (mt != kv.end()) { try { c.max_tokens = std::stoi(mt->second); } catch (...) {} }
  auto aa = kv.find("auto_approve_commands");
  if (aa != kv.end()) c.auto_approve_commands = (aa->second == "true");
  if (!get("cwd").empty()) c.cwd = get("cwd");
  if (!get("extra_system").empty()) c.extra_system = get("extra_system");
  if (!get("workspace").empty()) c.workspace = get("workspace");
  return true;
}

Config load() {
  Config c;

  // 环境变量
  auto env = [](const char* n) -> std::string {
    const char* v = getenv(n);
    return v ? v : "";
  };

  std::string prov = env("OPENAIDD_PROVIDER");
  std::string model = env("OPENAIDD_MODEL");
  std::string base = env("OPENAIDD_BASE_URL");
  std::string key;
  if (prov == "openai") key = env("OPENAI_API_KEY");
  else key = env("ANTHROPIC_API_KEY");
  if (key.empty()) key = env("OPENAIDD_API_KEY");
  // 兼容旧环境变量名
  if (prov.empty()) prov = env("AGENTCLI_PROVIDER");
  if (model.empty()) model = env("AGENTCLI_MODEL");
  if (base.empty()) base = env("AGENTCLI_BASE_URL");
  if (key.empty()) key = env("AGENTCLI_API_KEY");

  bool prov_from_env = !prov.empty();
  if (!prov.empty()) c.provider = prov;
  if (!model.empty()) c.model = model;
  if (!base.empty()) c.base_url = base;
  if (!key.empty()) c.api_key = key;

  // 当前目录 openaidd.toml（正式），回退旧版 agentconfig.toml
  Config toml_c;
  if (load_toml("openaidd.toml", toml_c) || load_toml("agentconfig.toml", toml_c)) {
    if (!prov_from_env) c.provider = toml_c.provider;  // 默认值 anthropic 允许被 TOML 覆盖
    if (c.model.empty()) c.model = toml_c.model;
    if (c.api_key.empty()) c.api_key = toml_c.api_key;
    if (c.base_url.empty()) c.base_url = toml_c.base_url;
    c.max_tokens = toml_c.max_tokens;
    c.auto_approve_commands = toml_c.auto_approve_commands;
    if (c.cwd == ".") c.cwd = toml_c.cwd;
    if (c.extra_system.empty()) c.extra_system = toml_c.extra_system;
    if (c.workspace.empty()) c.workspace = toml_c.workspace;
    if (!c.api_key.empty()) return c;  // 本目录配置已完整，直接生效
  }

  // 回退：旧版 ~/.agentcli/config.json
  std::string text;
  if (util::read_file(legacy_config_path(), text)) {
    try {
      json::Value v = json::parse(text);
      if (v.isObject()) {
        if (c.provider.empty() || (env("OPENAIDD_PROVIDER").empty() &&
                                   env("AGENTCLI_PROVIDER").empty()))
          c.provider = v.getStr("provider", c.provider);
        if (c.model.empty()) c.model = v.getStr("model", c.model);
        if (c.api_key.empty()) c.api_key = v.getStr("api_key", c.api_key);
        if (c.base_url.empty()) c.base_url = v.getStr("base_url", c.base_url);
        c.max_tokens = (int)v.getLong("max_tokens", c.max_tokens);
        c.auto_approve_commands = v.get("auto_approve_commands", json::Value(c.auto_approve_commands)).asBool();
        if (c.cwd == "." ) c.cwd = v.getStr("cwd", c.cwd);
        c.extra_system = v.getStr("extra_system", c.extra_system);
      }
    } catch (...) {}
  }

  // 默认值
  if (c.model.empty()) c.model = c.is_anthropic() ? "claude-sonnet-4-20250514" : "gpt-4o";
  if (c.base_url.empty())
    c.base_url = c.is_anthropic()
        ? "https://api.anthropic.com/v1/messages"
        : "https://api.openai.com/v1/chat/completions";

  return c;
}

void save(const Config& c) {
  std::string toml =
      "# openaidd 配置（当前目录）\n"
      "provider = " + toml_quote(c.provider) + "\n"
      "model = " + toml_quote(c.model) + "\n"
      "api_key = " + toml_quote(c.api_key) + "\n"
      "base_url = " + toml_quote(c.base_url) + "\n"
      "max_tokens = " + std::to_string(c.max_tokens) + "\n"
      "auto_approve_commands = " + (c.auto_approve_commands ? "true" : "false") + "\n"
      "cwd = " + toml_quote(c.cwd) + "\n"
      "extra_system = " + toml_quote(c.extra_system) + "\n"
      "workspace = " + toml_quote(c.workspace) + "\n";
  util::write_file(config_path(), toml);
}

}  // namespace cfg
