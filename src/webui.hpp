// webui.hpp - 内嵌 Web 界面（零依赖：Winsock2 + SSE）
#pragma once
#include "config.hpp"

namespace webui {

// 启动 WebUI 服务器（阻塞运行，Ctrl+C 退出）。返回退出码。
int run(cfg::Config& cfg, int port);

// 后台线程启动 WebUI 服务器（与终端 REPL 并存），立即返回。
// 成功返回 true；失败（如端口被占用）返回 false，调用方可继续。
// 两边是各自独立的会话，历史统一保存在 resume/ 目录。
bool start_background(cfg::Config cfg, int port);

}  // namespace webui
