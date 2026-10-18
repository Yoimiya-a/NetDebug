#include "config.hpp"

#include "log.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

std::string home_dir() {
  if (const char* home = std::getenv("HOME")) {
    if (*home) return home;
  }
  if (passwd* pw = getpwuid(getuid())) {
    if (pw->pw_dir) return pw->pw_dir;
  }
  return ".";
}

std::filesystem::path config_path() {
  return std::filesystem::path(home_dir()) / ".config" / "netdebug" / "config.ini";
}

void write_default(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  ::chmod(path.parent_path().c_str(), 0700);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "# NetDebug 服务配置\n"
         "port=17611\n"
         "password=netdebug\n"
         "# 文件传输目录，留空则使用 ~/Downloads\n"
         "download_dir=\n";
  out.close();
  ::chmod(path.c_str(), 0600);
  log_line("已生成配置 " + path.string() + " ，默认口令 netdebug ，请修改 password");
}

std::string expand_path(std::string value) {
  if (value.empty() || value[0] != '~') return value;
  if (value.size() == 1 || value[1] == '/') return home_dir() + value.substr(1);
  return value;
}

}  // namespace

Config load_config() {
  Config cfg;
  cfg.file_path = config_path();
  std::error_code ec;
  if (!std::filesystem::exists(cfg.file_path, ec)) write_default(cfg.file_path);

  std::ifstream in(cfg.file_path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
      (unsigned char)data[2] == 0xBF) {
    data.erase(0, 3);
  }

  std::string port_text = "17611";
  std::string download;
  bool have_password = false;
  size_t pos = 0;
  while (pos < data.size()) {
    size_t end = data.find('\n', pos);
    if (end == std::string::npos) end = data.size();
    std::string line = trim(data.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (key == "port") port_text = val;
    else if (key == "password") {
      cfg.password = val;
      have_password = true;
    } else if (key == "download_dir") download = val;
  }
  if (!have_password) cfg.password = "netdebug";

  try {
    int port = std::stoi(port_text);
    if (port > 0 && port <= 65535) cfg.port = static_cast<uint16_t>(port);
  } catch (...) {
    log_line("port 无效，使用 17611");
  }

  download = expand_path(download);
  if (download.empty()) download = home_dir() + "/Downloads";
  cfg.download_dir = download;
  std::filesystem::create_directories(cfg.download_dir, ec);
  if (ec) {
    log_line("无法创建传输目录 " + cfg.download_dir.string() + " ，改用家目录");
    cfg.download_dir = home_dir();
    std::filesystem::create_directories(cfg.download_dir, ec);
  }
  log_line("配置 " + cfg.file_path.string() + " 端口 " + std::to_string(cfg.port) + " 传输目录 " +
           cfg.download_dir.string());
  return cfg;
}
