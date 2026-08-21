// setup.cpp - 配置向导实现（厂商 → 配置方式 → 模型 → 地址 → API Key）
#include "setup.hpp"
#include "providers.hpp"
#include "ui.hpp"
#include "utils.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace setup {

namespace {
using namespace util::color;

void print_banner() {
  std::cout << util::color::bold << util::color::cyan
            << "\n"
            << "     ___                    _____ _      ___\n"
            << "    / _ \\ _ __   ___ _ __ / ___| |    / _|\n"
            << "   | | | | '_ \\ / _ \\ '_ \\ \\___| |   | |_\n"
            << "   | |_| | |_) |  __/ | | |___| |___|  _|\n"
            << "    \\___/| .__/ \\___|_| |_|____|_____|_|\n"
            << "         |_|  _      _      _\n"
            << "        / \\    (_) ___| | __| |\n"
            << "       / _ \\   | |/ __| |/ _` |\n"
            << "      / ___ \\  | | (__| | (_| |\n"
            << "     /_/   \\_\\_|_|\\___|_|\\__,_|  v1.0\n"
            << "\n"
            << util::color::reset;
  std::cout << util::color::gray
            << "  C++ Terminal Agent — Zero-dependency | WinHTTP + SSE\n"
            << util::color::reset;
}
}  // namespace

bool first_run(cfg::Config& cfg) {
  print_banner();
  std::cout << "\n";

  std::cout << yellow << bold
            << "  首次使用 — 配置你的 API\n"
            << reset;
  std::cout << gray
            << "  先用 ↑/↓ 选择厂商，再选择 token plan 套餐或普通 API Key。\n"
            << "  配置将保存到当前目录 " << cfg::config_path() << "。\n\n"
            << reset;

  // ---- 1. 选择厂商 ----
  const auto& providers = prov::all();
  std::vector<ui::MenuItem> prov_items;
  for (const auto& p : providers) prov_items.push_back({p.display, p.tag});
  int pidx = ui::select_menu("  选择厂商", prov_items, 0);
  if (pidx < 0) { std::cout << gray << "  已取消。\n" << reset; return false; }
  const prov::ProviderInfo& p = providers[pidx];
  std::cout << "\n  -> 厂商: " << green << bold << p.display << reset << "\n";

  // ---- 2. 选择配置方式：token plan 套餐 / 普通 API Key ----
  std::string model;
  bool use_plan = false;
  if (p.id != "custom") {
    std::vector<ui::MenuItem> mode_items = {
      {"token plan 套餐", "内置/官方模型定价列表，↑↓ 选择"},
      {"普通 API Key",    "手动输入模型名，跳过套餐列表"},
    };
    int mode = ui::select_menu("  选择配置方式", mode_items, 0);
    if (mode < 0) { std::cout << gray << "  已取消。\n" << reset; return false; }
    use_plan = (mode == 0);
    std::cout << "\n  -> 方式: " << green << bold
              << (use_plan ? "token plan 套餐" : "普通 API Key") << reset << "\n";
  }

  if (!use_plan) {
    // 普通 API Key / custom（Ollama）：手动输入模型名
    model = ui::input("  输入模型名（如 qwen2.5:14b / deepseek-r1:8b）", "");
    if (model.empty()) { std::cout << red << "  模型名不能为空。\n" << reset; return false; }
  } else {
    // token plan：有环境变量 Key 时尝试拉官方模型列表；失败/无 Key 则用内置定价表
    std::vector<prov::ModelInfo> official;
    bool fetched = false;
    const char* ek = getenv(p.id == "anthropic" ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY");
    std::string env_key = ek ? ek : "";
    if (!env_key.empty()) {
      std::cout << "\n  " << gray << "正在从官方 API 获取模型列表..." << reset;
      std::cout.flush();
      std::string ferr;
      fetched = prov::fetch_models(p, env_key, official, ferr);
      if (fetched) {
        std::cout << "\r  " << green << "已获取官方模型列表 (" << official.size() << " 个)" << reset << "\n";
      } else {
        std::cout << "\r  " << yellow << "官方列表获取失败，使用内置定价表" << reset << "\n";
      }
    }

    std::vector<ui::MenuItem> m_items;
    if (fetched) {
      for (const auto& m : official) m_items.push_back({m.name, "official"});
    } else {
      for (const auto& m : p.models) m_items.push_back({m.name, m.plan});
    }
    int midx = ui::select_menu("  选择模型 / token plan（每 1M tokens 输入/输出价格）", m_items, 0);
    if (midx < 0) { std::cout << gray << "  已取消。\n" << reset; return false; }
    model = fetched ? official[midx].name : p.models[midx].name;
  }
  std::cout << "\n  -> 模型: " << green << bold << model << reset << "\n";

  // ---- 3. 请求地址 ----
  std::string base_url;
  if (p.custom_base) {
    base_url = ui::input("  请求地址 (OpenAI 兼容端点)", p.base_url);
    if (base_url.empty()) { std::cout << red << "  请求地址不能为空。\n" << reset; return false; }
  } else {
    base_url = ui::input("  请求地址 (Enter 用默认)", p.base_url);
  }

  // ---- 4. API Key ----
  std::string key = ui::input("  API Key", "");
  if (key.empty() && p.id != "custom") {
    std::cout << red << "\n  API Key 不能为空。\n" << reset;
    return false;
  }

  // ---- 写回并保存到当前目录 openaidd.toml ----
  cfg.provider = (p.id == "custom") ? "openai" : p.id;
  cfg.model = model;
  cfg.base_url = base_url;
  cfg.api_key = key;
  cfg::save(cfg);

  std::cout << "\n" << green << bold
            << "  配置已保存到 " << cfg::config_path() << "! 启动中...\n"
            << reset;
  std::cout << gray << "  (以后修改可用 /model /provider /save /return)\n\n"
            << reset;
  return true;
}

}  // namespace setup
