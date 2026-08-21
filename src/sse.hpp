// sse.hpp - 服务端推送事件（SSE）增量解析器
#pragma once
#include <string>
#include <functional>

namespace sse {

// 每解析出一个完整事件回调：name 可能为空（OpenAI 风格只有 data），data 为负载
class Parser {
public:
  using EventCallback = std::function<void(const std::string& name, const std::string& data)>;

  explicit Parser(EventCallback cb) : cb_(std::move(cb)) {}

  // 喂入流式数据块，自动按行解析并分发完整事件
  void feed(const std::string& chunk) {
    buf_ += chunk;
    size_t nl;
    while ((nl = buf_.find('\n')) != std::string::npos) {
      std::string line = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      process_line(line);
    }
  }

  // 流结束时调用：处理残余数据
  void flush() {
    if (!buf_.empty()) {
      std::string line = buf_;
      buf_.clear();
      if (!line.empty() && line.back() == '\r') line.pop_back();
      process_line(line);
    }
    // 若仍有挂起事件（结尾没有空行）
    if (!cur_data_.empty() || !cur_event_.empty()) {
      dispatch();
    }
  }

private:
  void process_line(const std::string& line) {
    if (line.empty()) {
      dispatch();
      return;
    }
    if (line[0] == ':') return;  // 注释

    if (line.compare(0, 5, "data:") == 0) {
      std::string val = line.substr(6);
      if (!val.empty() && val[0] == ' ') val.erase(0, 1);
      cur_data_ = val;
      // OpenAI 兼容流式格式：每个 data 行即一个完整事件（部分厂商如 MiniMax
      // 的 SSE 事件之间没有空行分隔），必须立即派发，不能等空行/末尾再处理。
      dispatch();
    } else if (line.compare(0, 6, "event:") == 0) {
      std::string val = line.substr(6);
      if (!val.empty() && val[0] == ' ') val.erase(0, 1);
      cur_event_ = val;
    }
    // 忽略 id: / retry: 等
  }

  void dispatch() {
    if (cb_) cb_(cur_event_, cur_data_);
    cur_event_.clear();
    cur_data_.clear();
  }

  std::string buf_;
  std::string cur_event_;
  std::string cur_data_;
  EventCallback cb_;
};

}  // namespace sse
