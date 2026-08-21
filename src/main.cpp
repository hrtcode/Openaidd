// main.cpp
#include "config.hpp"
#include "agent.hpp"
#include "repl.hpp"
#include "session.hpp"
#include "setup.hpp"
#include "utils.hpp"
#include "version.hpp"
#include "webui.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

static void usage() {
  std::cout << OPENAIDD_TITLE " - C++ 终端智能体\n\n";
  std::cout << "用法:\n";
  std::cout << "  openaidd                终端 REPL + 后台自动启动 WebUI（端口 8080）\n";
  std::cout << "  openaidd --web [端口]   只启动 WebUI（默认端口 8080，仅本机访问）\n";
  std::cout << "  openaidd --cli          只进入终端交互式 REPL（不开 WebUI）\n";
  std::cout << "  openaidd \"你的问题\"     单次提问后退出\n\n";
  std::cout << "选项:\n";
  std::cout << "  -p, --provider <p>   后端: anthropic | openai\n";
  std::cout << "  -m, --model <name>   模型名\n";
  std::cout << "  -k, --key <key>      API Key（也可设环境变量 ANTHROPIC_API_KEY / OPENAI_API_KEY）\n";
  std::cout << "  -b, --base-url <url> 自定义请求地址（兼容端点）\n";
  std::cout << "  -c, --cwd <dir>      工作目录（默认使用 workspace）\n";
  std::cout << "  -w, --workspace <dir> 工作区目录（默认 <exe目录>/workspace，自动创建）\n";
  std::cout << "                       历史会话统一保存在 <exe目录>/resume\n";
  std::cout << "  -v, --version        显示版本\n";
  std::cout << "  -h, --help           显示本帮助\n";
}

int main(int argc, char** argv) {
  util::enable_vt();

  cfg::Config cfg = cfg::load();

  // Windows 下 main() 的 argv 是系统代码页(GBK)编码，直接当 UTF-8 用会让中文参数乱码。
  // 用 GetCommandLineW 重建 UTF-8 参数；非 Windows / 失败时回退到 argv。
  std::vector<std::string> args = util::command_line_args();
  if (args.empty()) {
    for (int i = 0; i < argc; i++) args.push_back(argv[i]);
  }

  std::vector<std::string> positional;
  bool show_help = false;
  bool show_version = false;
  bool web_mode = false;
  bool cli_mode = false;
  int web_port = 8080;
  bool cwd_explicit = false;

  for (size_t i = 1; i < args.size(); i++) {
    std::string a = args[i];
    auto next = [&](const std::string& def) -> std::string {
      if (i + 1 < args.size()) return args[++i];
      return def;
    };
    if (a == "-h" || a == "--help") { show_help = true; }
    else if (a == "-v" || a == "--version") { show_version = true; }
    else if (a == "-i" || a == "--cli") { cli_mode = true; }
    else if (a == "--web") {
      web_mode = true;
      // 可选端口：--web 8080 或 --web=8080
      if (i + 1 < args.size() && !args[i + 1].empty() &&
          args[i + 1].find_first_not_of("0123456789") == std::string::npos)
        web_port = atoi(next("8080").c_str());
    }
    else if (a.rfind("--web=", 0) == 0) {
      web_mode = true;
      web_port = atoi(a.c_str() + 6);
    }
    else if (a == "-p" || a == "--provider") cfg.provider = next(cfg.provider);
    else if (a == "-m" || a == "--model") cfg.model = next(cfg.model);
    else if (a == "-k" || a == "--key") cfg.api_key = next(cfg.api_key);
    else if (a == "-b" || a == "--base-url") cfg.base_url = next(cfg.base_url);
    else if (a == "-c" || a == "--cwd") { cfg.cwd = next(cfg.cwd); cwd_explicit = true; }
    else if (a == "-w" || a == "--workspace") cfg.workspace = next(cfg.workspace);
    else positional.push_back(a);
  }

  if (show_help) { usage(); return 0; }
  if (show_version) { std::cout << OPENAIDD_TITLE << "\n"; return 0; }

  // ---- 工作区：未显式指定时默认 <exe目录>/workspace，自动创建 ----
  if (cfg.workspace.empty()) cfg.workspace = util::exe_dir() + "/workspace";
  util::ensure_dir(cfg.workspace);
  // 历史会话目录：<exe目录>/resume（与 workspace 解耦，WebUI/REPL 共用）
  util::ensure_dir(session::sessions_dir());
  // 一次性迁移：旧位置 <workspace>/.sessions 里的历史挪到 resume/
  {
    std::string old_dir = cfg.workspace + "/.sessions";
    for (const auto& name : util::list_dir(old_dir)) {
      if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
        std::string from = old_dir + "/" + name;
        std::string to = session::sessions_dir() + "/" + name;
        std::string tmp;
        if (!util::read_file(to, tmp) && util::read_file(from, tmp) &&
            util::write_file(to, tmp)) {
          std::remove(from.c_str());
        }
      }
    }
  }
  // 未显式指定 cwd（且 TOML 里也没配）时，默认在 workspace 里干活
  if (!cwd_explicit && cfg.cwd == ".") cfg.cwd = cfg.workspace;

  // 没有 API Key 时进入首次配置向导
  bool from_setup = false;
  if (cfg.api_key.empty()) {
    if (!setup::first_run(cfg)) {
      std::cout << "\n" << util::color::gray << "Press Enter to exit..." << util::color::reset;
      std::cout.flush();
      std::cin.get();
      return 1;
    }
    from_setup = true;
  }

  // ---- 默认行为：不带任何参数直接运行（如双击 exe）----
  // 终端进入 REPL，同时后台自动启动 WebUI（浏览器访问 http://127.0.0.1:8080/）
  if (!web_mode && !cli_mode && positional.empty()) {
    webui::start_background(cfg, web_port);  // 失败（如端口被占用）仅提示，REPL 照常
  }

  // WebUI 模式（--web：纯 Web，不进 REPL）
  if (web_mode) {
    return webui::run(cfg, web_port);
  }

  // 单次提问模式
  if (!positional.empty()) {
    std::string prompt;
    for (size_t i = 0; i < positional.size(); i++) {
      if (i) prompt += " ";
      prompt += positional[i];
    }
    agent::Agent a(cfg);
    a.chat(prompt);
    if (!from_setup) {
      std::cout << "\n" << util::color::gray << "Press Enter to exit..." << util::color::reset;
      std::cout.flush();
      std::cin.get();
    }
    return 0;
  }

  // 交互模式
  repl::run(cfg);

  // 退出提示（双击运行时防止窗口闪退）
  std::cout << util::color::gray << "Press Enter to exit..." << util::color::reset;
  std::cout.flush();
  std::cin.get();
  return 0;
}
