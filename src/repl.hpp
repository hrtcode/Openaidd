// repl.hpp - 交互式 REPL
#pragma once
#include "config.hpp"

namespace repl {

// 进入交互循环（会按需在配置变更时重建 Agent）
void run(cfg::Config& cfg);

}  // namespace repl
