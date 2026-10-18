#include "display.hpp"

#include "log.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace {

thread_local bool g_x_error = false;
thread_local bool g_x_quiet = false;

int x_error_handler(Display* dpy, XErrorEvent* event) {
  g_x_error = true;
  if (g_x_quiet) return 0;
  char text[256];
  XGetErrorText(dpy, event->error_code, text, sizeof(text));
  log_line(std::string("X 错误: ") + text);
  return 0;
}

int x_io_error_handler(Display*) {
  log_line("与 X 的连接断开");
  _exit(1);
  return 0;
}

int mask_shift(unsigned long mask) {
  int shift = 0;
  if (mask == 0) return 0;
  while ((mask & 1ul) == 0) {
    mask >>= 1;
    ++shift;
  }
  return shift;
}

int mask_bits(unsigned long mask) {
  int bits = 0;
  while (mask) {
    bits += static_cast<int>(mask & 1ul);
    mask >>= 1;
  }
  return bits;
}

uint8_t take_channel(uint32_t pixel, int shift, unsigned long mask) {
  const int bits = mask_bits(mask);
  if (bits <= 0) return 0;
  unsigned value = (pixel & static_cast<uint32_t>(mask)) >> shift;
  if (bits >= 8) return static_cast<uint8_t>(value >> (bits - 8));
  return static_cast<uint8_t>(value << (8 - bits));
}

uint32_t bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}

struct PixFmt {
  int bpp = 4;
  unsigned long red_mask = 0;
  unsigned long green_mask = 0;
  unsigned long blue_mask = 0;
  int rshift = 0;
  int gshift = 0;
  int bshift = 0;
  int byte_order = LSBFirst;
};

PixFmt pixfmt_from(const XImage* image) {
  PixFmt f;
  f.bpp = image->bits_per_pixel / 8;
  f.byte_order = image->byte_order;
  f.red_mask = image->red_mask;
  f.green_mask = image->green_mask;
  f.blue_mask = image->blue_mask;
  if (f.red_mask == 0 && f.green_mask == 0 && f.blue_mask == 0) {
    f.blue_mask = 0x000000ff;
    f.green_mask = 0x0000ff00;
    f.red_mask = 0x00ff0000;
  }
  f.rshift = mask_shift(f.red_mask);
  f.gshift = mask_shift(f.green_mask);
  f.bshift = mask_shift(f.blue_mask);
  return f;
}

uint32_t load_pixel(const uint8_t* px, const PixFmt& f) {
  uint32_t pixel = 0;
  if (f.bpp >= 4) {
    std::memcpy(&pixel, px, 4);
    if (f.byte_order == MSBFirst) pixel = bswap32(pixel);
  } else if (f.bpp == 3) {
    pixel = px[0] | (static_cast<uint32_t>(px[1]) << 8) | (static_cast<uint32_t>(px[2]) << 16);
  }
  return pixel;
}

void store_rgb(uint8_t* dst, uint32_t pixel, const PixFmt& f) {
  dst[0] = take_channel(pixel, f.rshift, f.red_mask);
  dst[1] = take_channel(pixel, f.gshift, f.green_mask);
  dst[2] = take_channel(pixel, f.bshift, f.blue_mask);
}

bool ximage_blank(const XImage* image) {
  if (!image || image->width < 2 || image->height < 2 || !image->data) return true;
  if (image->bits_per_pixel != 32 && image->bits_per_pixel != 24) return false;
  const PixFmt f = pixfmt_from(image);
  const int sx = std::max(1, image->width / 48);
  const int sy = std::max(1, image->height / 32);
  for (int y = 0; y < image->height; y += sy) {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(image->data) + static_cast<size_t>(y) * image->bytes_per_line;
    for (int x = 0; x < image->width; x += sx) {
      uint8_t rgb[3];
      store_rgb(rgb, load_pixel(row + static_cast<size_t>(x) * f.bpp, f), f);
      if (rgb[0] | rgb[1] | rgb[2]) return false;
    }
  }
  return true;
}

bool buffer_blank(const uint8_t* rgb, int w, int h) {
  if (!rgb || w < 2 || h < 2) return true;
  const int sx = std::max(1, w / 48);
  const int sy = std::max(1, h / 32);
  for (int y = 0; y < h; y += sy) {
    const uint8_t* row = rgb + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
    for (int x = 0; x < w; x += sx) {
      const uint8_t* p = row + static_cast<size_t>(x) * 3;
      if (p[0] | p[1] | p[2]) return false;
    }
  }
  return true;
}

void blit_at(const XImage* image, int abs_x, int abs_y, int canvas_w, int canvas_h, uint8_t* canvas) {
  if (!image || !image->data) return;
  if (image->bits_per_pixel != 32 && image->bits_per_pixel != 24) return;
  const PixFmt f = pixfmt_from(image);
  for (int y = 0; y < image->height; ++y) {
    const int dy = abs_y + y;
    if (dy < 0 || dy >= canvas_h) continue;
    const uint8_t* row = reinterpret_cast<const uint8_t*>(image->data) + static_cast<size_t>(y) * image->bytes_per_line;
    uint8_t* dst = canvas + (static_cast<size_t>(dy) * static_cast<size_t>(canvas_w)) * 3;
    for (int x = 0; x < image->width; ++x) {
      const int dx = abs_x + x;
      if (dx < 0 || dx >= canvas_w) continue;
      store_rgb(dst + static_cast<size_t>(dx) * 3, load_pixel(row + static_cast<size_t>(x) * f.bpp, f), f);
    }
  }
}

void paint_window(Display* dpy, Window w, int abs_x, int abs_y, int origin_w, int origin_h, uint8_t* canvas,
                  Window skip, int& budget) {
  if (budget <= 0 || w == skip || w == None) return;
  --budget;
  XWindowAttributes attr{};
  g_x_error = false;
  g_x_quiet = true;
  const Status got = XGetWindowAttributes(dpy, w, &attr);
  g_x_quiet = false;
  if (!got || g_x_error || attr.map_state != IsViewable) return;
  abs_x += attr.x;
  abs_y += attr.y;
  const bool shell = attr.override_redirect && attr.width >= origin_w * 8 / 10 && attr.height >= origin_h * 8 / 10;
  if (!shell && attr.c_class == InputOutput && attr.width >= 16 && attr.height >= 16) {
    g_x_error = false;
    g_x_quiet = true;
    XImage* img = XGetImage(dpy, w, 0, 0, static_cast<unsigned int>(attr.width),
                            static_cast<unsigned int>(attr.height), AllPlanes, ZPixmap);
    g_x_quiet = false;
    if (img && !g_x_error) blit_at(img, abs_x, abs_y, origin_w, origin_h, canvas);
    if (img) XDestroyImage(img);
  }
  Window root_ret = None;
  Window parent = None;
  Window* kids = nullptr;
  unsigned n = 0;
  if (XQueryTree(dpy, w, &root_ret, &parent, &kids, &n) && kids) {
    for (unsigned i = 0; i < n && budget > 0; ++i)
      paint_window(dpy, kids[i], abs_x, abs_y, origin_w, origin_h, canvas, skip, budget);
    XFree(kids);
  }
}

uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

bool decode_png(const std::string& bytes, std::vector<uint8_t>& rgb, int& width, int& height) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (bytes.size() < 8 || std::memcmp(bytes.data(), sig, 8) != 0) return false;
  int bit_depth = 0;
  int color_type = 0;
  int interlace = 0;
  width = 0;
  height = 0;
  std::vector<uint8_t> idat;
  std::vector<uint8_t> palette;
  size_t i = 8;
  while (i + 12 <= bytes.size()) {
    const uint32_t len = read_be32(reinterpret_cast<const uint8_t*>(bytes.data() + i));
    if (len > bytes.size() - i - 12) return false;
    const char* type = bytes.data() + i + 4;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data() + i + 8);
    if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
      width = static_cast<int>(read_be32(data));
      height = static_cast<int>(read_be32(data + 4));
      bit_depth = data[8];
      color_type = data[9];
      interlace = data[12];
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), data, data + len);
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      palette.assign(data, data + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      break;
    }
    i += 12u + len;
  }
  if (width < 2 || height < 2 || width > 8192 || height > 8192) return false;
  if (bit_depth != 8 || interlace != 0) return false;
  int channels = 0;
  if (color_type == 2) channels = 3;
  else if (color_type == 6) channels = 4;
  else if (color_type == 0) channels = 1;
  else if (color_type == 3) channels = 1;
  else return false;
  const size_t stride = 1u + static_cast<size_t>(width) * static_cast<size_t>(channels);
  z_stream strm{};
  strm.next_in = idat.data();
  strm.avail_in = static_cast<uInt>(idat.size());
  if (inflateInit(&strm) != Z_OK) return false;
  std::vector<uint8_t> raw(stride * static_cast<size_t>(height));
  strm.next_out = raw.data();
  strm.avail_out = static_cast<uInt>(raw.size());
  const int rc = inflate(&strm, Z_FINISH);
  const size_t produced = strm.total_out;
  inflateEnd(&strm);
  if (rc != Z_STREAM_END || produced < stride * static_cast<size_t>(height)) return false;

  rgb.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3, 0);
  std::vector<uint8_t> prev(static_cast<size_t>(width) * channels, 0);
  std::vector<uint8_t> cur(static_cast<size_t>(width) * channels, 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = raw.data() + static_cast<size_t>(y) * stride;
    const uint8_t filter = row[0];
    const uint8_t* src = row + 1;
    if (filter > 4) return false;
    for (int x = 0; x < width * channels; ++x) {
      const int left = x >= channels ? cur[x - channels] : 0;
      const int up = prev[x];
      const int ul = x >= channels ? prev[x - channels] : 0;
      int value = src[x];
      if (filter == 1) value += left;
      else if (filter == 2) value += up;
      else if (filter == 3) value += (left + up) / 2;
      else if (filter == 4) value += paeth(left, up, ul);
      cur[x] = static_cast<uint8_t>(value);
    }
    uint8_t* dst = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
    if (color_type == 2) {
      std::memcpy(dst, cur.data(), static_cast<size_t>(width) * 3);
    } else if (color_type == 6) {
      for (int x = 0; x < width; ++x) {
        dst[x * 3] = cur[x * 4];
        dst[x * 3 + 1] = cur[x * 4 + 1];
        dst[x * 3 + 2] = cur[x * 4 + 2];
      }
    } else if (color_type == 0) {
      for (int x = 0; x < width; ++x) dst[x * 3] = dst[x * 3 + 1] = dst[x * 3 + 2] = cur[x];
    } else {
      for (int x = 0; x < width; ++x) {
        const int idx = cur[x] * 3;
        if (idx + 2 >= static_cast<int>(palette.size())) return false;
        dst[x * 3] = palette[idx];
        dst[x * 3 + 1] = palette[idx + 1];
        dst[x * 3 + 2] = palette[idx + 2];
      }
    }
    prev.swap(cur);
  }
  return true;
}

std::string env_value(const std::string& blob, const char* key);

void ensure_session_bus() {
  const std::string dir = "/run/user/" + std::to_string(getuid());
  if (!getenv("XDG_RUNTIME_DIR") || !getenv("XDG_RUNTIME_DIR")[0]) setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
  std::string best;
  int best_score = -1;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (ec || !entry.is_directory()) continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
    struct stat st{};
    if (stat(entry.path().c_str(), &st) != 0 || st.st_uid != getuid()) continue;
    std::ifstream env_file("/proc/" + name + "/environ", std::ios::binary);
    if (!env_file) continue;
    const std::string blob((std::istreambuf_iterator<char>(env_file)), std::istreambuf_iterator<char>());
    const std::string bus = env_value(blob, "DBUS_SESSION_BUS_ADDRESS");
    if (bus.empty()) continue;
    std::ifstream comm_file("/proc/" + name + "/comm");
    std::string comm;
    std::getline(comm_file, comm);
    int score = 1;
    if (comm == "gnome-shell") score = 10;
    else if (comm.compare(0, 13, "gnome-session") == 0) score = 8;
    if (score > best_score) {
      best_score = score;
      best = bus;
    }
  }
  if (!best.empty()) setenv("DBUS_SESSION_BUS_ADDRESS", best.c_str(), 1);
  else if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !getenv("DBUS_SESSION_BUS_ADDRESS")[0]) {
    const std::string bus = dir + "/bus";
    if (access(bus.c_str(), F_OK) == 0) {
      const std::string addr = "unix:path=" + bus;
      setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 1);
    }
  }
}

bool gnome_screenshot(std::vector<uint8_t>& rgb, int& width, int& height) {
  ensure_session_bus();
  const char* path = "/dev/shm/netdebug-frame.png";
  ::unlink(path);
  const char* bus = getenv("DBUS_SESSION_BUS_ADDRESS");
  const std::string address_arg = bus && bus[0] ? std::string("--address=") + bus : std::string("--user");
  int pipes[2] = {-1, -1};
  if (pipe(pipes) != 0) return false;
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipes[0]);
    close(pipes[1]);
    return false;
  }
  if (pid == 0) {
    dup2(pipes[1], STDOUT_FILENO);
    const int devnull_in = open("/dev/null", O_RDONLY);
    if (devnull_in >= 0) {
      dup2(devnull_in, STDIN_FILENO);
      close(devnull_in);
    }
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[0]);
    close(pipes[1]);
    for (int fd = 3; fd < 256; ++fd) close(fd);
    char* argv[] = {
        const_cast<char*>("/usr/bin/busctl"),
        const_cast<char*>(address_arg.c_str()),
        const_cast<char*>("call"),
        const_cast<char*>("org.gnome.Shell.Screenshot"),
        const_cast<char*>("/org/gnome/Shell/Screenshot"),
        const_cast<char*>("org.gnome.Shell.Screenshot"),
        const_cast<char*>("Screenshot"),
        const_cast<char*>("bbs"),
        const_cast<char*>("false"),
        const_cast<char*>("false"),
        const_cast<char*>(path),
        nullptr};
    execv(argv[0], argv);
    _exit(127);
  }
  close(pipes[1]);
  std::string out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd pfd{pipes[0], POLLIN, 0};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (poll(&pfd, 1, left > 0 ? static_cast<int>(left) : 0) <= 0) break;
    char buf[512];
    const ssize_t n = read(pipes[0], buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  close(pipes[0]);
  int status = 0;
  if (waitpid(pid, &status, WNOHANG) == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    ::unlink(path);
    return false;
  }
  if (out.find("true") == std::string::npos) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      if (out.size() > 180) out.resize(180);
      for (char& ch : out) {
        if (ch == '\n' || ch == '\r') ch = ' ';
      }
      log_line(out.empty() ? "GNOME 截图没有返回结果" : "GNOME 截图失败: " + out);
    }
    ::unlink(path);
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ::unlink(path);
  if (!decode_png(bytes, rgb, width, height)) return false;
  if (buffer_blank(rgb.data(), width, height)) {
    static bool logged_black = false;
    if (!logged_black) {
      logged_black = true;
      log_line("GNOME 截到的仍是黑屏");
    }
    return false;
  }
  return true;
}

bool read_rgb_file(const char* path, std::vector<uint8_t>& rgb, int& width, int& height) {
  std::ifstream in(path, std::ios::binary);
  uint32_t w = 0;
  uint32_t h = 0;
  in.read(reinterpret_cast<char*>(&w), 4);
  in.read(reinterpret_cast<char*>(&h), 4);
  if (!in || w < 2 || h < 2 || w > 8192 || h > 8192) return false;
  rgb.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
  if (!in) return false;
  width = static_cast<int>(w);
  height = static_cast<int>(h);
  return !buffer_blank(rgb.data(), width, height);
}

bool capture_via_helper(std::vector<uint8_t>& rgb, int& width, int& height) {
  const char* path = "/dev/shm/netdebug-grab.bin";
  ::unlink(path);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    for (int fd = 3; fd < 256; ++fd) close(fd);
    execl("/proc/self/exe", "netdebug-service", "--grab", path, static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
  int status = 0;
  bool finished = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      finished = true;
      break;
    }
    pollfd pause{};
    poll(&pause, 0, 20);
  }
  if (!finished) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    ::unlink(path);
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ::unlink(path);
    return false;
  }
  const bool ok = read_rgb_file(path, rgb, width, height);
  ::unlink(path);
  return ok;
}

int grab_root_image_impl(const char* path) {
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) return 1;
  const int screen = DefaultScreen(dpy);
  Window root = RootWindow(dpy, screen);
  XWindowAttributes attr{};
  if (!XGetWindowAttributes(dpy, root, &attr) || attr.width < 2 || attr.height < 2) {
    XCloseDisplay(dpy);
    return 1;
  }
  XImage* image = XGetImage(dpy, root, 0, 0, static_cast<unsigned int>(attr.width),
                            static_cast<unsigned int>(attr.height), AllPlanes, ZPixmap);
  if (!image) {
    XCloseDisplay(dpy);
    return 1;
  }
  if (ximage_blank(image)) {
    XDestroyImage(image);
    XCloseDisplay(dpy);
    return 2;
  }
  std::vector<uint8_t> rgb(static_cast<size_t>(attr.width) * static_cast<size_t>(attr.height) * 3);
  blit_at(image, 0, 0, attr.width, attr.height, rgb.data());
  XDestroyImage(image);
  XCloseDisplay(dpy);
  std::ofstream out(path, std::ios::binary);
  const uint32_t w = static_cast<uint32_t>(attr.width);
  const uint32_t h = static_cast<uint32_t>(attr.height);
  out.write(reinterpret_cast<const char*>(&w), 4);
  out.write(reinterpret_cast<const char*>(&h), 4);
  out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
  return out ? 0 : 1;
}

void scale_rgb(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
  for (int y = 0; y < dh; ++y) {
    const int sy = y * sh / dh;
    const uint8_t* srow = src + static_cast<size_t>(sy) * static_cast<size_t>(sw) * 3;
    uint8_t* drow = dst + static_cast<size_t>(y) * static_cast<size_t>(dw) * 3;
    for (int x = 0; x < dw; ++x) {
      const int sx = x * sw / dw;
      const uint8_t* p = srow + static_cast<size_t>(sx) * 3;
      drow[0] = p[0];
      drow[1] = p[1];
      drow[2] = p[2];
      drow += 3;
    }
  }
}

KeySym vk_to_keysym(uint16_t vk) {
  if (vk >= 'A' && vk <= 'Z') return XK_a + (vk - 'A');
  if (vk >= '0' && vk <= '9') return XK_0 + (vk - '0');
  switch (vk) {
    case 0x08: return XK_BackSpace;
    case 0x09: return XK_Tab;
    case 0x0D: return XK_Return;
    case 0x10: return XK_Shift_L;
    case 0x11: return XK_Control_L;
    case 0x12: return XK_Alt_L;
    case 0x13: return XK_Pause;
    case 0x14: return XK_Caps_Lock;
    case 0x1B: return XK_Escape;
    case 0x20: return XK_space;
    case 0x21: return XK_Page_Up;
    case 0x22: return XK_Page_Down;
    case 0x23: return XK_End;
    case 0x24: return XK_Home;
    case 0x25: return XK_Left;
    case 0x26: return XK_Up;
    case 0x27: return XK_Right;
    case 0x28: return XK_Down;
    case 0x2C: return XK_Print;
    case 0x2D: return XK_Insert;
    case 0x2E: return XK_Delete;
    case 0x5B: return XK_Super_L;
    case 0x5C: return XK_Super_R;
    case 0x5D: return XK_Menu;
    case 0x60: return XK_KP_0;
    case 0x61: return XK_KP_1;
    case 0x62: return XK_KP_2;
    case 0x63: return XK_KP_3;
    case 0x64: return XK_KP_4;
    case 0x65: return XK_KP_5;
    case 0x66: return XK_KP_6;
    case 0x67: return XK_KP_7;
    case 0x68: return XK_KP_8;
    case 0x69: return XK_KP_9;
    case 0x6A: return XK_KP_Multiply;
    case 0x6B: return XK_KP_Add;
    case 0x6D: return XK_KP_Subtract;
    case 0x6E: return XK_KP_Decimal;
    case 0x6F: return XK_KP_Divide;
    case 0x70: return XK_F1;
    case 0x71: return XK_F2;
    case 0x72: return XK_F3;
    case 0x73: return XK_F4;
    case 0x74: return XK_F5;
    case 0x75: return XK_F6;
    case 0x76: return XK_F7;
    case 0x77: return XK_F8;
    case 0x78: return XK_F9;
    case 0x79: return XK_F10;
    case 0x7A: return XK_F11;
    case 0x7B: return XK_F12;
    case 0x90: return XK_Num_Lock;
    case 0x91: return XK_Scroll_Lock;
    case 0xA0: return XK_Shift_L;
    case 0xA1: return XK_Shift_R;
    case 0xA2: return XK_Control_L;
    case 0xA3: return XK_Control_R;
    case 0xA4: return XK_Alt_L;
    case 0xA5: return XK_Alt_R;
    case 0xBA: return XK_semicolon;
    case 0xBB: return XK_equal;
    case 0xBC: return XK_comma;
    case 0xBD: return XK_minus;
    case 0xBE: return XK_period;
    case 0xBF: return XK_slash;
    case 0xC0: return XK_grave;
    case 0xDB: return XK_bracketleft;
    case 0xDC: return XK_backslash;
    case 0xDD: return XK_bracketright;
    case 0xDE: return XK_apostrophe;
    default: return 0;
  }
}

KeyCode keycode_for_sym(Display* dpy, KeySym sym, std::unordered_map<KeySym, KeyCode>& spare) {
  if (!sym) return 0;
  if (const KeyCode direct = XKeysymToKeycode(dpy, sym)) return direct;
  int min_kc = 8;
  int max_kc = 255;
  XDisplayKeycodes(dpy, &min_kc, &max_kc);
  for (int kc = min_kc; kc <= max_kc; ++kc) {
    for (int level = 0; level < 4; ++level) {
      if (XkbKeycodeToKeysym(dpy, static_cast<KeyCode>(kc), 0, level) == sym)
        return static_cast<KeyCode>(kc);
    }
  }
  const auto found = spare.find(sym);
  if (found != spare.end()) return found->second;
  for (int kc = max_kc; kc >= min_kc; --kc) {
    bool used = false;
    for (int level = 0; level < 4; ++level) {
      if (XkbKeycodeToKeysym(dpy, static_cast<KeyCode>(kc), 0, level) != NoSymbol) {
        used = true;
        break;
      }
    }
    if (used) continue;
    bool taken = false;
    for (const auto& item : spare) {
      if (item.second == kc) taken = true;
    }
    if (taken) continue;
    KeySym syms[2] = {sym, sym};
    XChangeKeyboardMapping(dpy, kc, 2, syms, 1);
    XSync(dpy, False);
    const auto code = static_cast<KeyCode>(kc);
    spare.emplace(sym, code);
    return code;
  }
  return 0;
}

int xtest_button(uint8_t button) {
  if (button == 1) return 1;
  if (button == 2) return 3;
  if (button == 3) return 2;
  return 0;
}

struct ShmFrame {
  XImage* image = nullptr;
  XShmSegmentInfo info{};
  bool attached = false;

  void destroy(Display* dpy) {
    if (attached && dpy) {
      XShmDetach(dpy, &info);
      attached = false;
    }
    if (info.shmaddr && info.shmaddr != reinterpret_cast<char*>(-1)) {
      shmdt(info.shmaddr);
    }
    info.shmaddr = nullptr;
    if (image) {
      image->data = nullptr;
      XDestroyImage(image);
      image = nullptr;
    }
  }

  bool create(Display* dpy, int screen, int width, int height) {
    destroy(dpy);
    if (!XShmQueryExtension(dpy)) return false;
    image = XShmCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen), ZPixmap, nullptr,
                            &info, width, height);
    if (!image) return false;
    const size_t bytes = static_cast<size_t>(image->bytes_per_line) * static_cast<size_t>(image->height);
    info.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (info.shmid < 0) {
      image->data = nullptr;
      XDestroyImage(image);
      image = nullptr;
      return false;
    }
    info.shmaddr = static_cast<char*>(shmat(info.shmid, nullptr, 0));
    if (info.shmaddr == reinterpret_cast<char*>(-1)) {
      shmctl(info.shmid, IPC_RMID, nullptr);
      image->data = nullptr;
      XDestroyImage(image);
      image = nullptr;
      info.shmaddr = nullptr;
      return false;
    }
    image->data = info.shmaddr;
    info.readOnly = False;
    shmctl(info.shmid, IPC_RMID, nullptr);
    g_x_error = false;
    if (!XShmAttach(dpy, &info)) {
      destroy(dpy);
      return false;
    }
    XSync(dpy, False);
    if (g_x_error) {
      destroy(dpy);
      return false;
    }
    attached = true;
    return true;
  }
};

std::string env_value(const std::string& blob, const char* key) {
  const std::string prefix = std::string(key) + "=";
  size_t pos = 0;
  while (pos < blob.size()) {
    size_t end = blob.find('\0', pos);
    if (end == std::string::npos) end = blob.size();
    const std::string item = blob.substr(pos, end - pos);
    if (item.compare(0, prefix.size(), prefix) == 0) return item.substr(prefix.size());
    if (end == blob.size()) break;
    pos = end + 1;
  }
  return {};
}

bool find_session_env(std::string& display, std::string& xauth) {
  int best = -1;
  std::string best_display;
  std::string best_auth;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (ec) break;
    const std::string name = entry.path().filename().string();
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
    int pid = 0;
    try {
      pid = std::stoi(name);
    } catch (...) {
      continue;
    }
    if (pid == getpid()) continue;
    struct stat st {};
    if (stat(entry.path().c_str(), &st) != 0 || st.st_uid != getuid()) continue;

    std::ifstream env_file("/proc/" + name + "/environ", std::ios::binary);
    if (!env_file) continue;
    const std::string blob((std::istreambuf_iterator<char>(env_file)), std::istreambuf_iterator<char>());
    const std::string disp = env_value(blob, "DISPLAY");
    const std::string auth = env_value(blob, "XAUTHORITY");
    if (disp.empty() || auth.empty()) continue;
    std::ifstream comm_file("/proc/" + name + "/comm");
    std::string comm;
    std::getline(comm_file, comm);
    const std::string session = env_value(blob, "XDG_SESSION_TYPE");
    int score = 1;
    if (session == "x11") score += 5;
    if (comm.compare(0, 13, "gnome-session") == 0) score += 3;
    if (comm == "gnome-shell") score += 2;
    if (comm == "Xorg" || comm == "Xorg.bin") score += 4;
    if (score > best) {
      best = score;
      best_display = disp;
      best_auth = auth;
    }
  }
  if (best < 0) return false;
  display = best_display;
  xauth = best_auth;
  return true;
}

void prepare_default_env() {
  if (!getenv("DISPLAY") || !getenv("DISPLAY")[0]) setenv("DISPLAY", ":0", 1);
  if (getenv("XAUTHORITY") && getenv("XAUTHORITY")[0]) return;
  const char* home = getenv("HOME");
  const std::string uid = std::to_string(getuid());
  const std::string candidates[] = {
      home ? std::string(home) + "/.Xauthority" : "",
      "/run/user/" + uid + "/gdm/Xauthority",
      "/run/user/" + uid + "/.mutter-Xwaylandauth.XXXXXX",
  };
  for (const auto& path : candidates) {
    if (path.empty()) continue;
    if (access(path.c_str(), R_OK) == 0) {
      setenv("XAUTHORITY", path.c_str(), 1);
      return;
    }
  }
}

bool session_is_wayland() {
  const char* auth = getenv("XAUTHORITY");
  if (auth && std::string(auth).find("Xwayland") != std::string::npos) return true;
  const char* kind = getenv("XDG_SESSION_TYPE");
  return kind && std::string(kind) == "wayland";
}

void log_wayland_hint() {
  log_line("当前桌面是 Wayland，X 读不到画面，GNOME 也会拒绝截图。请注销，在登录界面右下角齿轮里选择 Ubuntu on Xorg，登录后再执行 systemctl --user restart netdebug.service");
}

bool open_display(Display*& dpy, const std::atomic<bool>& stop, std::condition_variable& cv,
                  std::mutex& mu) {
  prepare_default_env();
  while (!stop.load()) {
    dpy = XOpenDisplay(nullptr);
    if (dpy) {
      log_line(std::string("已连接 X DISPLAY=") + (getenv("DISPLAY") ? getenv("DISPLAY") : "") +
               " XAUTHORITY=" + (getenv("XAUTHORITY") ? getenv("XAUTHORITY") : ""));
      if (session_is_wayland()) log_wayland_hint();
      return true;
    }
    std::string disp;
    std::string auth;
    if (find_session_env(disp, auth)) {
      setenv("DISPLAY", disp.c_str(), 1);
      setenv("XAUTHORITY", auth.c_str(), 1);
      dpy = XOpenDisplay(nullptr);
      if (dpy) {
        log_line("已从会话进程取得 DISPLAY=" + disp);
        return true;
      }
    }
    log_line("暂时打不开 X，1 秒后重试。请确认已登录 Ubuntu on Xorg");
    std::unique_lock<std::mutex> lock(mu);
    cv.wait_for(lock, std::chrono::seconds(1), [&] { return stop.load(); });
  }
  return false;
}

}  // namespace

int grab_root_image(const char* path) { return grab_root_image_impl(path); }

XHost::XHost() = default;

XHost::~XHost() {
  request_stop();
  join();
}

void XHost::start(FrameFn on_frame, ClipFn on_clip) {
  join();
  on_frame_ = std::move(on_frame);
  on_clip_ = std::move(on_clip);
  stop_.store(false);
  {
    std::lock_guard<std::mutex> lock(mu_);
    has_settings_ = false;
    queue_.clear();
  }
  thread_ = std::thread([this] {
    try {
      run();
    } catch (const std::exception& ex) {
      log_line(std::string("画面线程异常: ") + ex.what());
    }
  });
}

void XHost::request_stop() {
  stop_.store(true);
  cv_.notify_all();
}

void XHost::join() {
  if (thread_.joinable()) thread_.join();
}

void XHost::set_settings(const ndbg::StreamSettings& settings) {
  ndbg::StreamSettings copy = settings;
  if (copy.jpeg_quality < 1) copy.jpeg_quality = 1;
  if (copy.jpeg_quality > 100) copy.jpeg_quality = 100;
  if (copy.max_fps < 1) copy.max_fps = 1;
  if (copy.max_fps > 60) copy.max_fps = 60;
  if (copy.codec > 1) copy.codec = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    settings_ = copy;
    has_settings_ = true;
  }
  cv_.notify_all();
}

void XHost::enqueue(Cmd cmd) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (cmd.type == Cmd::Mouse && cmd.action == 0 && !queue_.empty() && queue_.back().type == Cmd::Mouse &&
        queue_.back().action == 0) {
      queue_.back() = std::move(cmd);
    } else {
      queue_.push_back(std::move(cmd));
      if (queue_.size() > 1024) {
        for (auto it = queue_.begin(); it != queue_.end() && queue_.size() > 512;) {
          if (it->type == Cmd::Mouse && it->action == 0) it = queue_.erase(it);
          else ++it;
        }
      }
    }
  }
  cv_.notify_all();
}

void XHost::push_mouse(uint16_t x, uint16_t y, uint8_t button, uint8_t action, int16_t wheel) {
  Cmd cmd;
  cmd.type = Cmd::Mouse;
  cmd.x = x;
  cmd.y = y;
  cmd.button = button;
  cmd.action = action;
  cmd.wheel = wheel;
  enqueue(std::move(cmd));
}

void XHost::push_key(uint16_t vk, bool down) {
  Cmd cmd;
  cmd.type = Cmd::Key;
  cmd.vk = vk;
  cmd.down = down;
  enqueue(std::move(cmd));
}

void XHost::push_clipboard(std::string text) {
  Cmd cmd;
  cmd.type = Cmd::Clipboard;
  cmd.text = std::move(text);
  enqueue(std::move(cmd));
}

void XHost::run() {
  static std::once_flag once;
  std::call_once(once, [] {
    XInitThreads();
    XSetErrorHandler(x_error_handler);
    XSetIOErrorHandler(x_io_error_handler);
  });

  Display* dpy = nullptr;
  if (!open_display(dpy, stop_, cv_, mu_)) return;

  const int screen = DefaultScreen(dpy);
  Window root = RootWindow(dpy, screen);
  Window clip_win = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
  XSelectInput(dpy, clip_win, PropertyChangeMask);

  Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  Atom targets = XInternAtom(dpy, "TARGETS", False);
  Atom incr = XInternAtom(dpy, "INCR", False);
  Atom prop = XInternAtom(dpy, "NETDEBUG_CLIP", False);
  int xfixes_base = 0;
  int xfixes_error = 0;
  const bool have_xfixes = XFixesQueryExtension(dpy, &xfixes_base, &xfixes_error) == True;
  if (have_xfixes) {
    XFixesSelectSelectionInput(dpy, clip_win, clipboard, XFixesSetSelectionOwnerNotifyMask);
  }
  int xtest_event = 0;
  int xtest_error = 0;
  int xtest_major = 0;
  int xtest_minor = 0;
  const bool have_xtest = XTestQueryExtension(dpy, &xtest_event, &xtest_error, &xtest_major, &xtest_minor) == True;
  if (!have_xtest) log_line("XTest 不可用，键鼠注入不会生效");

  std::string owned_text;
  std::string last_sent;
  bool clip_dirty = true;
  bool logged_format = false;
  bool logged_bad_format = false;
  enum class CaptureKind { Shm, XGet, Gnome, Windows };
  CaptureKind capture_kind = CaptureKind::Shm;
  bool logged_shm_black = false;
  bool logged_gnome_fail = false;
  bool logged_still_black = false;
  ShmFrame shm;
  std::vector<uint8_t> canvas;
  uint32_t frame_id = 0;
  int origin_w = 0;
  int origin_h = 0;

  auto publish_clip = [&](const std::string& text) {
    if (text == last_sent) return;
    if (text.size() > ndbg::kMaxClipboard) return;
    last_sent = text;
    if (!on_clip_) return;
    try {
      on_clip_(text);
    } catch (const std::exception& ex) {
      log_line(std::string("剪贴板发送失败: ") + ex.what());
    }
  };

  auto handle_selection_request = [&](const XSelectionRequestEvent& req) {
    XSelectionEvent reply{};
    reply.type = SelectionNotify;
    reply.display = req.display;
    reply.requestor = req.requestor;
    reply.selection = req.selection;
    reply.target = req.target;
    reply.property = req.property == None ? req.target : req.property;
    reply.time = req.time;
    if (req.target == targets) {
      uint32_t atoms[3] = {static_cast<uint32_t>(targets), static_cast<uint32_t>(utf8),
                           static_cast<uint32_t>(XA_STRING)};
      XChangeProperty(dpy, req.requestor, reply.property, XA_ATOM, 32, PropModeReplace,
                      reinterpret_cast<unsigned char*>(atoms), 3);
    } else if (req.target == utf8 || req.target == XA_STRING) {
      XChangeProperty(dpy, req.requestor, reply.property, req.target, 8, PropModeReplace,
                      reinterpret_cast<const unsigned char*>(owned_text.data()),
                      static_cast<int>(owned_text.size()));
    } else {
      reply.property = None;
    }
    XSendEvent(dpy, req.requestor, False, 0, reinterpret_cast<XEvent*>(&reply));
    XFlush(dpy);
  };

  auto read_property = [&](Atom property, std::string& out) -> bool {
    Atom actual = None;
    int format = 0;
    unsigned long items = 0;
    unsigned long remain = 0;
    unsigned char* data = nullptr;
    const int code = XGetWindowProperty(dpy, clip_win, property, 0, 1 << 20, False, AnyPropertyType, &actual,
                                        &format, &items, &remain, &data);
    if (code != Success) return false;
    if (actual == incr) {
      if (data) XFree(data);
      XDeleteProperty(dpy, clip_win, property);
      XFlush(dpy);
      std::string all;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (std::chrono::steady_clock::now() < deadline && all.size() < ndbg::kMaxClipboard) {
        while (XPending(dpy)) {
          XEvent ev;
          XNextEvent(dpy, &ev);
          if (ev.type == PropertyNotify && ev.xproperty.atom == property &&
              ev.xproperty.state == PropertyNewValue) {
            unsigned char* part = nullptr;
            Atom part_type = None;
            int part_format = 0;
            unsigned long part_items = 0;
            unsigned long part_remain = 0;
            if (XGetWindowProperty(dpy, clip_win, property, 0, 1 << 20, True, AnyPropertyType, &part_type,
                                   &part_format, &part_items, &part_remain, &part) == Success &&
                part) {
              if (part_items == 0) {
                XFree(part);
                out = all;
                return true;
              }
              all.append(reinterpret_cast<char*>(part), part_items);
              XFree(part);
            }
          } else if (ev.type == SelectionRequest) {
            handle_selection_request(ev.xselectionrequest);
          }
        }
        pollfd pfd{ConnectionNumber(dpy), POLLIN, 0};
        poll(&pfd, 1, 20);
      }
      out = all;
      return !out.empty();
    }
    if (format == 8 && data && items > 0) {
      out.assign(reinterpret_cast<char*>(data), items);
      XFree(data);
      XDeleteProperty(dpy, clip_win, property);
      return true;
    }
    if (data) XFree(data);
    XDeleteProperty(dpy, clip_win, property);
    out.clear();
    return actual != None;
  };

  std::function<bool(std::string&, Atom)> request_selection = [&](std::string& out, Atom target) -> bool {
    XConvertSelection(dpy, clipboard, target, prop, clip_win, CurrentTime);
    XFlush(dpy);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
      while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == SelectionNotify && ev.xselection.selection == clipboard) {
          if (ev.xselection.property == None) return false;
          return read_property(ev.xselection.property, out);
        }
        if (ev.type == SelectionRequest) handle_selection_request(ev.xselectionrequest);
        if (have_xfixes && ev.type == xfixes_base + XFixesSelectionNotify) {
          auto* se = reinterpret_cast<XFixesSelectionNotifyEvent*>(&ev);
          if (se->owner != clip_win) clip_dirty = true;
        }
      }
      pollfd pfd{ConnectionNumber(dpy), POLLIN, 0};
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      poll(&pfd, 1, left > 0 ? static_cast<int>(left) : 0);
    }
    return false;
  };

  auto read_clipboard = [&](std::string& out) -> bool {
    if (XGetSelectionOwner(dpy, clipboard) == clip_win) {
      out = owned_text;
      return true;
    }
    if (request_selection(out, utf8)) return true;
    return request_selection(out, XA_STRING);
  };

  auto set_clipboard = [&](const std::string& text) {
    owned_text = text.size() > ndbg::kMaxClipboard ? text.substr(0, ndbg::kMaxClipboard) : text;
    last_sent = owned_text;
    XSetSelectionOwner(dpy, clipboard, clip_win, CurrentTime);
    XFlush(dpy);
    if (XGetSelectionOwner(dpy, clipboard) != clip_win) log_line("未能取得剪贴板所有权");
  };

  {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return stop_.load() || has_settings_; });
  }

  std::unordered_map<KeySym, KeyCode> spare_keycodes;
  bool logged_unmapped = false;
  auto next_frame = std::chrono::steady_clock::now();
  while (!stop_.load()) {
    ndbg::StreamSettings settings;
    std::deque<Cmd> local;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait_until(lock, next_frame, [&] { return stop_.load() || !queue_.empty(); });
      settings = settings_;
      local.swap(queue_);
    }
    if (stop_.load()) break;

    for (const auto& cmd : local) {
      if (cmd.type == Cmd::Clipboard) {
        set_clipboard(cmd.text);
        continue;
      }
      if (!have_xtest) continue;
      if (cmd.type == Cmd::Mouse) {
        int x = cmd.x;
        int y = cmd.y;
        if (origin_w > 0 && origin_h > 0) {
          if (x >= origin_w) x = origin_w - 1;
          if (y >= origin_h) y = origin_h - 1;
        }
        XTestFakeMotionEvent(dpy, screen, x, y, CurrentTime);
        if (cmd.action == 1 || cmd.action == 2) {
          const int button = xtest_button(cmd.button);
          if (button) XTestFakeButtonEvent(dpy, button, cmd.action == 1 ? True : False, CurrentTime);
        } else if (cmd.action == 3 && cmd.wheel != 0) {
          int notches = cmd.wheel;
          if (notches > 20) notches = 20;
          if (notches < -20) notches = -20;
          const int button = notches > 0 ? 4 : 5;
          const int count = notches > 0 ? notches : -notches;
          for (int i = 0; i < count; ++i) {
            XTestFakeButtonEvent(dpy, button, True, CurrentTime);
            XTestFakeButtonEvent(dpy, button, False, CurrentTime);
          }
        }
      } else if (cmd.type == Cmd::Key) {
        const KeySym sym = vk_to_keysym(cmd.vk);
        const KeyCode code = keycode_for_sym(dpy, sym, spare_keycodes);
        if (!code) {
          if (!logged_unmapped) {
            log_line("有按键在当前键盘布局里没有对应键");
            logged_unmapped = true;
          }
          continue;
        }
        XTestFakeKeyEvent(dpy, code, cmd.down ? True : False, CurrentTime);
      }
    }
    if (!local.empty()) XFlush(dpy);

    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      if (ev.type == SelectionRequest) handle_selection_request(ev.xselectionrequest);
      else if (have_xfixes && ev.type == xfixes_base + XFixesSelectionNotify) {
        auto* se = reinterpret_cast<XFixesSelectionNotifyEvent*>(&ev);
        if (se->owner != clip_win) clip_dirty = true;
      }
    }
    if (clip_dirty) {
      clip_dirty = false;
      std::string text;
      if (read_clipboard(text)) publish_clip(text);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame) continue;
    const int fps = std::max(1, static_cast<int>(settings.max_fps));
    const auto interval = std::chrono::microseconds(1000000 / fps);
    next_frame += interval;
    if (now - next_frame > interval) next_frame = now + interval;

    XWindowAttributes attr{};
    if (!XGetWindowAttributes(dpy, root, &attr) || attr.width <= 1 || attr.height <= 1) continue;
    origin_w = attr.width;
    origin_h = attr.height;
    int dw = origin_w;
    int dh = origin_h;
    if (settings.max_width > 0 && origin_w > settings.max_width) {
      dw = settings.max_width;
      dh = static_cast<int>(static_cast<int64_t>(origin_h) * dw / origin_w);
    }
    if (dh < 1) dh = 1;
    if (settings.codec == 0) {
      dw &= ~1;
      dh &= ~1;
    }
    if (dw < 2 || dh < 2) continue;

    if (capture_kind == CaptureKind::Gnome) {
      int sw = 0;
      int sh = 0;
      if (!gnome_screenshot(canvas, sw, sh)) {
        if (!logged_gnome_fail) {
          logged_gnome_fail = true;
          if (session_is_wayland()) log_wayland_hint();
          else log_line("GNOME 截图没有得到画面，1 秒后重试");
        }
        next_frame = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        continue;
      }
      int out_w = sw;
      int out_h = sh;
      if (settings.max_width > 0 && sw > settings.max_width) {
        out_w = settings.max_width;
        out_h = static_cast<int>(static_cast<int64_t>(sh) * out_w / sw);
      }
      if (out_h < 1) out_h = 1;
      if (settings.codec == 0) {
        out_w &= ~1;
        out_h &= ~1;
      }
      if (out_w < 2 || out_h < 2) continue;
      origin_w = sw;
      origin_h = sh;
      auto frame = std::make_shared<CapturedFrame>();
      frame->id = ++frame_id;
      frame->width = static_cast<uint16_t>(out_w);
      frame->height = static_cast<uint16_t>(out_h);
      frame->origin_w = static_cast<uint16_t>(std::min(sw, 65535));
      frame->origin_h = static_cast<uint16_t>(std::min(sh, 65535));
      frame->codec = settings.codec;
      frame->quality = settings.jpeg_quality;
      frame->rgb.resize(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 3);
      scale_rgb(canvas.data(), sw, sh, frame->rgb.data(), out_w, out_h);
      if (!on_frame_) continue;
      try {
        on_frame_(std::move(frame));
      } catch (const std::exception& ex) {
        log_line(std::string("提交画面失败: ") + ex.what());
      }
      continue;
    }

    if (capture_kind == CaptureKind::XGet) {
      int sw = 0;
      int sh = 0;
      if (!capture_via_helper(canvas, sw, sh)) {
        capture_kind = CaptureKind::Gnome;
        log_line("直接读取根窗口没有画面，改为 GNOME 截图");
        continue;
      }
      int out_w = sw;
      int out_h = sh;
      if (settings.max_width > 0 && sw > settings.max_width) {
        out_w = settings.max_width;
        out_h = static_cast<int>(static_cast<int64_t>(sh) * out_w / sw);
      }
      if (out_h < 1) out_h = 1;
      if (settings.codec == 0) {
        out_w &= ~1;
        out_h &= ~1;
      }
      if (out_w < 2 || out_h < 2) continue;
      origin_w = sw;
      origin_h = sh;
      auto frame = std::make_shared<CapturedFrame>();
      frame->id = ++frame_id;
      frame->width = static_cast<uint16_t>(out_w);
      frame->height = static_cast<uint16_t>(out_h);
      frame->origin_w = static_cast<uint16_t>(std::min(sw, 65535));
      frame->origin_h = static_cast<uint16_t>(std::min(sh, 65535));
      frame->codec = settings.codec;
      frame->quality = settings.jpeg_quality;
      frame->rgb.resize(static_cast<size_t>(out_w) * static_cast<size_t>(out_h) * 3);
      scale_rgb(canvas.data(), sw, sh, frame->rgb.data(), out_w, out_h);
      if (!on_frame_) continue;
      try {
        on_frame_(std::move(frame));
      } catch (const std::exception& ex) {
        log_line(std::string("提交画面失败: ") + ex.what());
      }
      continue;
    }

    if (capture_kind == CaptureKind::Windows) {
      canvas.assign(static_cast<size_t>(origin_w) * static_cast<size_t>(origin_h) * 3, 0);
      Window tree_root = None;
      Window tree_parent = None;
      Window* kids = nullptr;
      unsigned n = 0;
      if (XQueryTree(dpy, root, &tree_root, &tree_parent, &kids, &n) && kids) {
        int budget = 80;
        for (unsigned i = 0; i < n && budget > 0; ++i)
          paint_window(dpy, kids[i], 0, 0, origin_w, origin_h, canvas.data(), clip_win, budget);
        XFree(kids);
      }
      if (buffer_blank(canvas.data(), origin_w, origin_h)) {
        if (!logged_still_black) {
          logged_still_black = true;
          log_line("逐个窗口抓取后仍是黑屏。桌面合成器把画面留在显卡里，X 读不到");
        }
        next_frame = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        continue;
      }
      auto frame = std::make_shared<CapturedFrame>();
      frame->id = ++frame_id;
      frame->width = static_cast<uint16_t>(dw);
      frame->height = static_cast<uint16_t>(dh);
      frame->origin_w = static_cast<uint16_t>(std::min(origin_w, 65535));
      frame->origin_h = static_cast<uint16_t>(std::min(origin_h, 65535));
      frame->codec = settings.codec;
      frame->quality = settings.jpeg_quality;
      frame->rgb.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
      scale_rgb(canvas.data(), origin_w, origin_h, frame->rgb.data(), dw, dh);
      if (!on_frame_) continue;
      try {
        on_frame_(std::move(frame));
      } catch (const std::exception& ex) {
        log_line(std::string("提交画面失败: ") + ex.what());
      }
      continue;
    }

    XImage* image = nullptr;
    if (!shm.image || shm.image->width != origin_w || shm.image->height != origin_h) {
      if (!shm.create(dpy, screen, origin_w, origin_h)) {
        capture_kind = CaptureKind::Gnome;
        if (!logged_shm_black) {
          logged_shm_black = true;
          log_line("XShm 不可用，改为让 GNOME 截图");
        }
        continue;
      }
    }
    if (!XShmGetImage(dpy, root, shm.image, 0, 0, AllPlanes)) {
      shm.destroy(dpy);
      capture_kind = CaptureKind::Gnome;
      if (!logged_shm_black) {
        logged_shm_black = true;
        log_line("XShmGetImage 失败，改为让 GNOME 截图");
      }
      continue;
    }
    image = shm.image;
    if (ximage_blank(image)) {
      shm.destroy(dpy);
      capture_kind = CaptureKind::XGet;
      if (!logged_shm_black) {
        logged_shm_black = true;
        log_line("共享内存读到全黑画面，改为独立进程读取根窗口");
      }
      continue;
    }
    if (!logged_format) {
      logged_format = true;
      log_line("画面 " + std::to_string(origin_w) + "x" + std::to_string(origin_h) +
               " bpp=" + std::to_string(image->bits_per_pixel));
    }
    if (image->bits_per_pixel != 32 && image->bits_per_pixel != 24) {
      if (!logged_bad_format) {
        logged_bad_format = true;
        log_line("不支持的像素格式 bpp=" + std::to_string(image->bits_per_pixel));
      }
      continue;
    }

    unsigned long red_mask = image->red_mask;
    unsigned long green_mask = image->green_mask;
    unsigned long blue_mask = image->blue_mask;
    if (red_mask == 0 && green_mask == 0 && blue_mask == 0) {
      blue_mask = 0x000000ff;
      green_mask = 0x0000ff00;
      red_mask = 0x00ff0000;
    }
    const int rshift = mask_shift(red_mask);
    const int gshift = mask_shift(green_mask);
    const int bshift = mask_shift(blue_mask);
    auto frame = std::make_shared<CapturedFrame>();
    frame->id = ++frame_id;
    frame->width = static_cast<uint16_t>(dw);
    frame->height = static_cast<uint16_t>(dh);
    frame->origin_w = static_cast<uint16_t>(std::min(origin_w, 65535));
    frame->origin_h = static_cast<uint16_t>(std::min(origin_h, 65535));
    frame->codec = settings.codec;
    frame->quality = settings.jpeg_quality;
    frame->rgb.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);

    const int bpp = image->bits_per_pixel / 8;
    for (int y = 0; y < dh; ++y) {
      const int sy = y * origin_h / dh;
      const uint8_t* row = reinterpret_cast<uint8_t*>(image->data) + static_cast<size_t>(sy) * image->bytes_per_line;
      uint8_t* dst = frame->rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(dw) * 3;
      for (int x = 0; x < dw; ++x) {
        const int sx = x * origin_w / dw;
        const uint8_t* px = row + static_cast<size_t>(sx) * bpp;
        uint32_t pixel = 0;
        if (bpp == 4) {
          std::memcpy(&pixel, px, 4);
          if (image->byte_order == MSBFirst) pixel = bswap32(pixel);
        } else {
          pixel = px[0] | (px[1] << 8) | (px[2] << 16);
        }
        dst[0] = take_channel(pixel, rshift, red_mask);
        dst[1] = take_channel(pixel, gshift, green_mask);
        dst[2] = take_channel(pixel, bshift, blue_mask);
        dst += 3;
      }
    }
    if (!on_frame_) continue;
    try {
      on_frame_(std::move(frame));
    } catch (const std::exception& ex) {
      log_line(std::string("提交画面失败: ") + ex.what());
    }
  }

  shm.destroy(dpy);
  XDestroyWindow(dpy, clip_win);
  XCloseDisplay(dpy);
}
