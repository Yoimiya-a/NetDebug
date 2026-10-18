#pragma once

#include "protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CapturedFrame {
  uint32_t id = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t origin_w = 0;
  uint16_t origin_h = 0;
  uint8_t codec = 0;
  uint8_t quality = 30;
  std::vector<uint8_t> rgb;
};

int grab_root_image(const char* path);

class XHost {
 public:
  using FrameFn = std::function<void(std::shared_ptr<CapturedFrame>)>;
  using ClipFn = std::function<void(const std::string&)>;

  XHost();
  ~XHost();

  XHost(const XHost&) = delete;
  XHost& operator=(const XHost&) = delete;

  void start(FrameFn on_frame, ClipFn on_clip);
  void request_stop();
  void join();
  void set_settings(const ndbg::StreamSettings& settings);
  void push_mouse(uint16_t x, uint16_t y, uint8_t button, uint8_t action, int16_t wheel);
  void push_key(uint16_t vk, bool down);
  void push_clipboard(std::string text);

 private:
  struct Cmd {
    enum Type { Mouse, Key, Clipboard } type = Mouse;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t vk = 0;
    uint8_t button = 0;
    uint8_t action = 0;
    bool down = false;
    int16_t wheel = 0;
    std::string text;
  };

  void run();
  void enqueue(Cmd cmd);

  FrameFn on_frame_;
  ClipFn on_clip_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
  bool has_settings_ = false;
  ndbg::StreamSettings settings_{};
  std::deque<Cmd> queue_;
};
