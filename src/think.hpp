// think.hpp - 流式 <think> 标签过滤器
//
// 推理模型（如 MiniMax-M3）会在正文前输出 <think>...</think> 思考过程。
// 本过滤器以增量方式喂入文本：
//   - 剔除 <think> / </think> 标签本身（用户不需要看到标签）
//   - 通过 sink(text, is_think) 区分思考内容与正文
//   - 标签跨 chunk 被切断时自动缓冲拼接（如 "<thi" + "nk>"）
#pragma once
#include <functional>
#include <string>

namespace util {

class ThinkFilter {
public:
  // sink(文本, 是否思考内容)
  explicit ThinkFilter(std::function<void(const std::string&, bool)> sink)
      : sink_(std::move(sink)) {}

  // 喂入一段增量文本
  void feed(const std::string& delta) {
    buf_.append(delta);
    size_t i = 0;
    while (i < buf_.size()) {
      if (match_ == 0) {
        // 扫描模式：找下一个 '<'
        size_t lt = buf_.find('<', i);
        if (lt == std::string::npos) {
          emit(buf_.substr(i), in_think_);
          i = buf_.size();
          break;
        }
        if (lt > i) emit(buf_.substr(i, lt - i), in_think_);
        i = lt;
        match_ = 1;   // buf_[i, i+match_) 是标签前缀 "<"
      }
      // 匹配模式：逐字符判断 "<think>" / "</think>"
      bool need_more = false;
      while (match_ > 0) {
        if (i + match_ >= buf_.size()) { need_more = true; break; }  // 等下一批数据
        std::string cand = buf_.substr(i, match_ + 1);
        if (cand == kOpen) {
          in_think_ = true;
          i += match_ + 1; match_ = 0;
        } else if (cand == kClose) {
          in_think_ = false;
          i += match_ + 1; match_ = 0;
        } else if (is_prefix(kOpen, cand) || is_prefix(kClose, cand)) {
          match_++;
        } else {
          // 不是标签：'<' 按普通文本吐出，从下一个字符重新扫描
          emit("<", in_think_);
          i++;
          match_ = 0;
        }
      }
      if (need_more) break;
    }
    buf_.erase(0, i);
  }

  // 流结束：把残留的不完整标签按原文吐出
  void flush() {
    if (!buf_.empty()) emit(buf_, in_think_);
    buf_.clear();
    match_ = 0;
  }

  // 新的一段流式输出开始：清空状态（上一段若未闭合则丢弃）
  void reset() {
    buf_.clear();
    match_ = 0;
    in_think_ = false;
  }

  bool in_think() const { return in_think_; }

private:
  static bool is_prefix(const std::string& full, const std::string& p) {
    return full.size() >= p.size() && full.compare(0, p.size(), p) == 0;
  }

  void emit(const std::string& text, bool think) {
    if (text.empty() || !sink_) return;
    sink_(text, think);
  }

  static constexpr const char* kOpen = "<think>";
  static constexpr const char* kClose = "</think>";

  std::function<void(const std::string&, bool)> sink_;
  std::string buf_;     // 待处理缓冲（可能含不完整标签）
  size_t match_ = 0;    // >0 表示正在匹配标签，buf_[0..match_) 已匹配
  bool in_think_ = false;
};

}  // namespace util
