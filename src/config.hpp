// config.hpp - 配置（环境变量 + 当前目录 ./openaidd.toml，回退 ./agentconfig.toml 与 ~/.agentcli/config.json）
#pragma once
#include <string>

namespace cfg {

struct Config {
  std::string provider = "anthropic";   // anthropic | openai
  std::string model;                    // 模型名
  std::string api_key;                  // API Key
  std::string base_url;                 // 完整请求 URL
  int max_tokens = 8192;
  bool auto_approve_commands = false;   // 是否免确认执行 run_command
  std::string cwd = ".";                // 工作目录
  std::string extra_system;             // 额外系统提示（可选）
  std::string workspace;                // 工作区目录（会话存储位置；空=exe目录/workspace）

  bool is_anthropic() const { return provider == "anthropic"; }
};

// 加载：环境变量优先，其次当前目录 ./openaidd.toml（兼容 ./agentconfig.toml），再回退 ~/.agentcli/config.json
Config load();

// 写回配置（保存 /model、/provider 等运行时改动），写入当前目录 ./openaidd.toml
void save(const Config& c);

// 当前目录配置文件名（完整路径）
std::string config_path();

// 旧版用户目录配置（~/.agentcli/config.json），仅作读取回退
std::string legacy_config_path();

}  // namespace cfg
