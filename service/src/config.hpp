#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct Config {
  uint16_t port = 17611;
  std::string password = "netdebug";
  std::filesystem::path download_dir;
  std::filesystem::path file_path;
};

Config load_config();
