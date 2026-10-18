#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

inline void log_line(const std::string& msg) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char stamp[16];
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
  std::fprintf(stderr, "[%s] %s\n", stamp, msg.c_str());
  std::fflush(stderr);
}
