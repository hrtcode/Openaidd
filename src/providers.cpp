// providers.cpp - 厂商数据库：各厂商默认端点 + 模型 + token 定价
// 定价单位：每 1M tokens 的 输入/输出 美元($) 或人民币(¥)，为公开官方价，可能随折扣变动。
// 模型列表可在向导中通过官方 /models 接口动态获取（失败时回退到本表）。
#include "providers.hpp"
#include "http.hpp"
#include "json.hpp"

namespace prov {

const std::vector<ProviderInfo>& all() {
  static const std::vector<ProviderInfo> v = {
    {
      "anthropic", "Anthropic Claude", "官方 API · claude-opus/sonnet/haiku",
      "https://api.anthropic.com/v1/messages", false,
      {
        {"claude-opus-4-1-20250805",       "$15/$75 · 200K ctx · 最强推理"},
        {"claude-sonnet-4-5-20250929",     "$3/$15 · 200K ctx · 均衡推荐"},
        {"claude-sonnet-4-20250514",       "$3/$15 · 200K ctx"},
        {"claude-haiku-4-5-20251001",      "$1/$5 · 200K ctx · 快速低价"},
        {"claude-haiku-4-20250514",        "$1/$5 · 200K ctx"},
      }
    },
    {
      "openai", "OpenAI", "官方 API · gpt-4o / o3",
      "https://api.openai.com/v1/chat/completions", false,
      {
        {"gpt-4o",            "$2.50/$10 · 128K ctx"},
        {"gpt-4o-mini",       "$0.15/$0.60 · 128K ctx · 低价"},
        {"gpt-4.1",           "$2/$8 · 1M ctx · 长上下文"},
        {"gpt-4.1-mini",      "$0.40/$1.60 · 1M ctx"},
        {"o3",                "$10/$40 · 200K ctx · 推理模型"},
      }
    },
    {
      "openai", "DeepSeek", "深度求索 · 高性价比",
      "https://api.deepseek.com/v1/chat/completions", false,
      {
        {"deepseek-chat",     "$0.27/$1.10 · 128K ctx · V3 通用"},
        {"deepseek-reasoner", "$0.55/$2.19 · 64K ctx · R1 推理"},
      }
    },
    {
      "openai", "智谱 GLM", "国内直连 · 中文强",
      "https://open.bigmodel.cn/api/paas/v4/chat/completions", false,
      {
        {"glm-4.6",     "官方定价 · 128K ctx · 最新旗舰"},
        {"glm-4.5",     "官方定价 · 128K ctx"},
        {"glm-4-air",   "官方定价 · 128K ctx · 低价"},
      }
    },
    {
      "openai", "通义千问 Qwen", "阿里云百炼 · 开源生态",
      "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions", false,
      {
        {"qwen-max",    "官方定价 · 32K ctx · 旗舰"},
        {"qwen-plus",   "官方定价 · 128K ctx · 均衡"},
        {"qwen-turbo",  "官方定价 · 1M ctx · 低价快速"},
      }
    },
    {
      "openai", "月之暗面 Kimi", "Moonshot · 长上下文",
      "https://api.moonshot.cn/v1/chat/completions", false,
      {
        {"kimi-k2-0711-preview", "官方定价 · 128K ctx · 开源旗舰"},
        {"moonshot-v1-128k",     "官方定价 · 128K ctx"},
        {"moonshot-v1-32k",      "官方定价 · 32K ctx · 低价"},
      }
    },
    {
      "openai", "MiniMax 国际版", "platform.minimax.io · api.minimax.io",
      "https://api.minimax.io/v1/chat/completions", false,
      {
        {"MiniMax-M3",              "官方定价 · 1M ctx · 原生多模态 Frontier Coding"},
        {"MiniMax-M2.7",            "官方定价 · 205K ctx · 当前旗舰"},
        {"MiniMax-M2.7-highspeed",  "官方定价 · 205K ctx · 高速版"},
        {"MiniMax-M2.5",            "$0.15/1M 输入 · 205K ctx · 高性价比"},
        {"MiniMax-M2.5-highspeed",  "$0.15/1M 输入 · 205K ctx · 高速版"},
        {"MiniMax-M2.1",            "官方定价 · 1M ctx · 多语言编程"},
        {"MiniMax-M2.1-highspeed",  "官方定价 · 1M ctx · 高速版"},
        {"MiniMax-M2",              "官方定价 · 197K ctx · 编码/Agent"},
        {"MiniMax-M2-her",          "官方定价 · 66K ctx · 角色扮演"},
        {"MiniMax-M1-80k",          "$0.40/$2.20 · 1M ctx · 推理 80k 预算"},
        {"MiniMax-M1-40k",          "$0.40/$2.20 · 1M ctx · 推理 40k 预算"},
      }
    },
    {
      "openai", "MiniMax 国内版", "platform.minimaxi.com · api.minimaxi.com",
      "https://api.minimaxi.com/v1/chat/completions", false,
      {
        {"MiniMax-M3",              "官方定价 · 1M ctx · 原生多模态 Frontier Coding"},
        {"MiniMax-M2.7",            "官方定价 · 205K ctx · 当前旗舰"},
        {"MiniMax-M2.7-highspeed",  "官方定价 · 205K ctx · 高速版"},
        {"MiniMax-M2.5",            "$0.15/1M 输入 · 205K ctx · 高性价比"},
        {"MiniMax-M2.5-highspeed",  "$0.15/1M 输入 · 205K ctx · 高速版"},
        {"MiniMax-M2.1",            "官方定价 · 1M ctx · 多语言编程"},
        {"MiniMax-M2.1-highspeed",  "官方定价 · 1M ctx · 高速版"},
        {"MiniMax-M2",              "官方定价 · 197K ctx · 编码/Agent"},
        {"MiniMax-M2-her",          "官方定价 · 66K ctx · 角色扮演"},
        {"MiniMax-M1-80k",          "$0.40/$2.20 · 1M ctx · 推理 80k 预算"},
        {"MiniMax-M1-40k",          "$0.40/$2.20 · 1M ctx · 推理 40k 预算"},
      }
    },
    {
      "custom", "本地 Ollama / 自定义端点", "免费离线 · OpenAI 兼容",
      "http://localhost:11434/v1", true,
      {}
    },
  };
  return v;
}

// 由 base_url 推导模型列表端点：/chat/completions 或 /messages 结尾时替换为 /models，
// 否则直接追加 /models（如 ollama 根端点 http://localhost:11434/v1）
static std::string models_url(const ProviderInfo& p) {
  std::string u = p.base_url;
  while (!u.empty() && u.back() == '/') u.pop_back();
  const std::string chat = "/chat/completions";
  const std::string msgs = "/messages";
  if (u.size() >= chat.size() &&
      u.compare(u.size() - chat.size(), chat.size(), chat) == 0) {
    return u.substr(0, u.size() - chat.size()) + "/models";
  }
  if (u.size() >= msgs.size() &&
      u.compare(u.size() - msgs.size(), msgs.size(), msgs) == 0) {
    return u.substr(0, u.size() - msgs.size()) + "/models";
  }
  return u + "/models";
}

bool fetch_models(const ProviderInfo& p, const std::string& api_key,
                  std::vector<ModelInfo>& out, std::string& err) {
  http::Headers h;
  if (p.id == "anthropic") {
    h["x-api-key"] = api_key;
    h["anthropic-version"] = "2023-06-01";
  } else {
    h["Authorization"] = "Bearer " + api_key;
  }

  std::string body;
  long status = 0;
  if (!http::get(models_url(p), h, body, err, &status)) return false;
  if (status < 200 || status >= 300) {
    err = "HTTP " + std::to_string(status);
    return false;
  }

  try {
    json::Value root = json::parse(body);
    if (!root.isObject()) { err = "响应不是 JSON 对象"; return false; }
    json::Value data = root.get("data");
    if (!data.isArray()) { err = "响应缺少 data 数组"; return false; }
    for (auto& item : data.arr) {
      if (item.isObject()) {
        std::string id = item.getStr("id");
        if (!id.empty()) out.push_back({id, ""});
      }
    }
    if (out.empty()) { err = "模型列表为空"; return false; }
    return true;
  } catch (const std::exception& e) {
    err = std::string("解析失败: ") + e.what();
    return false;
  }
}

}  // namespace prov
