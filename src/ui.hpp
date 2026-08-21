// ui.hpp - 交互式菜单/输入组件（方向键导航，零依赖）
#pragma once
#include <string>
#include <vector>

namespace ui {

struct MenuItem {
  std::string label;   // 主显示文本
  std::string hint;    // 右侧灰色说明（如 token 定价）
};

// 是否交互终端（stdin 为 tty）。非交互时 select_menu 自动降级为数字输入。
bool interactive();

// 方向键/数字选择菜单，返回选中下标；Esc/取消 返回 -1。
// 支持 ↑↓ / w s / k j 移动，Enter 确认，Esc 取消，1-9 直选。
int select_menu(const std::string& title, const std::vector<MenuItem>& items, int def = 0);

// 带默认值的文本输入：回车取默认值（默认值为空则需手动输入）。
std::string input(const std::string& prompt, const std::string& def = "");

// 方向键确认：↑/↓ 或 w/s 切换 同意/拒绝，Enter 确认，Esc 拒绝。
// def 为默认选中项（true=同意）。非交互终端自动降级为 y/n 行输入。
bool confirm(const std::string& prompt, bool def = false);

}  // namespace ui
