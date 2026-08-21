// setup.hpp - 首次配置向导（厂商 → 配置方式 → 模型 → 地址 → API Key）
#pragma once
#include "config.hpp"

namespace setup {

// 运行配置向导并写回 cfg。完成后自动保存到当前目录 agentconfig.toml。
// 用户取消（Esc/取消）返回 false，成功返回 true。
bool first_run(cfg::Config& cfg);

}
