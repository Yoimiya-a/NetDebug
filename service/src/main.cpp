#include "config.hpp"
#include "display.hpp"
#include "log.hpp"
#include "server.hpp"
#include "sha256.hpp"

#include <cstring>
#include <pwd.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--grab") == 0) return grab_root_image(argv[2]);
  if (!ndbg::sha256_self_test()) {
    log_line("SHA256 自检失败，服务没有启动");
    return 1;
  }
  if (!getenv("HOME") || getenv("HOME")[0] == '\0') {
    if (passwd* pw = getpwuid(getuid())) {
      if (pw->pw_dir && pw->pw_dir[0]) setenv("HOME", pw->pw_dir, 1);
    }
  }
  const Config cfg = load_config();
  return run_server(cfg);
}
