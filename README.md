# openaidd

C++ 终端智能体 · 零依赖 · 单文件 exe

v0.0.0.1

openaidd 是一个用纯 C++17（无第三方依赖）实现的命令行 AI 智能体，内置工具调用（执行命令、读写文件）、流式输出、思考过程（`<think>`）过滤显示、内嵌 WebUI 和会话持久化。一个 exe 即是全部。

## 快速开始

```
openaidd.exe            双击或直接运行：终端 REPL + 后台 WebUI 同时启动
```

首次运行自动在 exe 旁边创建 `workspace/`（默认工作目录）和 `resume/`（历史会话），并自动打开浏览器访问 http://127.0.0.1:8080/。

**首次初始化**：若 exe 所在目录（如 `d:/openaidd`）没有任何配置文件（`openaidd.toml` / `agentconfig.toml`），启动时自动进入交互式配置向导——依次选择厂商（Anthropic / OpenAI / DeepSeek / 智谱 / 通义 / Kimi / MiniMax / Ollama 自定义）、配置方式（token plan 套餐或普通 API Key）、模型、请求地址、API Key，完成后在 exe 旁边生成 `openaidd.toml` 并直接进入主界面。命令行显式传 `-k <key>` 时跳过向导。

### 运行模式

| 运行方式 | 行为 |
|---|---|
| `openaidd` | 终端 REPL（原版界面）+ 后台 WebUI（8080 端口）同时运行 |
| `openaidd --web [端口]` | 只启动 WebUI，不进终端 |
| `openaidd --cli` | 只进终端 REPL，不开 Web 服务 |
| `openaidd "你的问题"` | 单次提问后退出 |

### 常用选项

```
-p, --provider <p>     后端: anthropic | openai（OpenAI 兼容端点均可）
-m, --model <name>     模型名
-k, --key <key>        API Key（也可用环境变量）
-b, --base-url <url>   自定义请求地址（兼容端点）
-c, --cwd <dir>        工作目录（默认使用 workspace）
-w, --workspace <dir>  工作区目录（默认 <exe目录>/workspace，自动创建）
-v, --version          显示版本
-h, --help             显示帮助
```

## 功能特性

### 智能体能力
- **工具调用**：`run_command`（执行 shell 命令）、读文件、写文件、列目录等，多轮自动循环直到任务完成
- **流式输出**：token 级实时显示；命令执行过程逐行实时回显
- **思考过程**：自动识别并剔除 `<think>`/`</think>` 标签（含跨 chunk 切断场景）；终端中思考内容以黑色显示、正文绿色；WebUI 中收进黑色可折叠"思考过程"块
- **命令确认**：执行命令前询问确认，可全局开启免确认（`auto_approve_commands`）

### WebUI（http://127.0.0.1:8080/）
- 聊天界面：流式回复、工具调用卡片（参数/输出可展开）、命令执行确认按钮
- **左侧历史会话栏**：列出全部历史会话（预览/时间/轮数），点击加载并完整回放（含思考块和工具卡片），支持一键新建会话
- 免确认执行命令开关、SSE 实时事件推送（断线重连自动回放日志）

### 会话持久化
- 历史会话统一保存在 **`<exe目录>/resume/`**，终端与 WebUI 共用
- 每轮对话自动落盘；恢复后模型保留完整上下文，续写写入同一文件
- 终端用 `/resume` 召唤历史会话（`/resume last` 恢复最近一次，`/resume <编号>` 选择恢复）

## 终端 REPL 命令

```
/help        帮助
/resume      恢复历史会话（last / 编号）
/clear       清空当前上下文
/model       切换模型
/provider    切换后端（anthropic | openai）
/approve     切换命令确认
/allaccept   一律免确认
/save        保存会话
/multiline   多行输入模式
/exit        退出（/quit 同）
```

## 配置

优先级：**环境变量 > `openaidd.toml`（exe 当前目录）> `agentconfig.toml`（旧版兼容）> `~/.agentcli/config.json`**。

`openaidd.toml` 示例：

```toml
provider = "openai"                # anthropic | openai（兼容端点）
model = "MiniMax-M3"
api_key = "sk-..."
base_url = "https://api.example.com/v1/chat/completions"
max_tokens = 8192
auto_approve_commands = true       # 免确认执行命令
cwd = "."                          # 工作目录
extra_system = ""                  # 追加 system 提示词
workspace = ""                     # 工作区目录（默认 <exe目录>/workspace）
```

环境变量（`OPENAIDD_*` 优先，旧 `AGENTCLI_*` 兼容）：

```
OPENAIDD_PROVIDER / OPENAIDD_MODEL / OPENAIDD_BASE_URL / OPENAIDD_API_KEY
OPENAI_API_KEY / ANTHROPIC_API_KEY
```

## 目录结构

```
openaidd/
├── openaidd.exe      主程序（单文件，~890KB）
├── openaidd.toml     配置（首次运行自动生成）
├── workspace/        默认工作目录（自动创建）
└── resume/           历史会话（JSON，终端/WebUI 共用，自动创建）
```

## 从源码构建

依赖：MinGW-w64 g++（C++17）、CMake（可选）。仅链接 Windows 系统库（winhttp / shell32 / ws2_32），无任何第三方库。

```bash
# 方式一：直接编译
g++ -std=c++17 -O2 -Isrc src/*.cpp -o openaidd.exe -lwinhttp -lshell32 -lws2_32

# 方式二：CMake
cmake -B build && cmake --build build --config Release
```

源码结构：

```
src/
├── main.cpp        入口、参数解析、模式分发
├── repl.cpp        终端交互式 REPL
├── webui.cpp       内嵌 WebUI（Winsock HTTP + SSE，含内嵌前端页面）
├── agent.cpp       智能体循环（工具调用多轮、hooks 回调）
├── provider.cpp    provider 抽象 + 流式解析（SSE）
├── openai.cpp      OpenAI 兼容后端
├── anthropic.cpp   Anthropic 后端
├── tools.cpp       工具实现（run_command / 文件读写）
├── session.cpp     会话持久化（resume/ 目录）
├── think.hpp       流式 <think> 标签过滤器（header-only）
├── config.cpp      配置加载（env / TOML / 旧 JSON）
├── http.cpp        WinHTTP HTTPS 客户端
├── json.cpp        内置 JSON 解析/序列化
└── utils.cpp       编码转换（GBK/UTF-8）、目录工具等
```

## 已知约定

- Windows 平台专用（WinHTTP / Winsock / ANSI 颜色）
- WebUI 仅监听 127.0.0.1，不对外网开放
- 8080 端口被占用时提示绑定失败，REPL 不受影响
- MinGW 需 win32 线程模型（本项目用 CreateThread / CONDITION_VARIABLE，不依赖 std::thread）

## 版本

- v0.0.0.1 — 首个正式版：终端 REPL + WebUI 双模式、`<think>` 标签过滤、历史会话栏与 `/resume`
