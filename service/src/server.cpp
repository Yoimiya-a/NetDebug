#include "server.hpp"

#include "display.hpp"
#include "log.hpp"
#include "protocol.hpp"
#include "sha256.hpp"

#include <turbojpeg.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_listen_fd = -1;
volatile sig_atomic_t g_client_fd = -1;

void on_signal(int) {
  g_stop = 1;
  if (g_listen_fd >= 0) ::shutdown(g_listen_fd, SHUT_RDWR);
  if (g_client_fd >= 0) ::shutdown(g_client_fd, SHUT_RDWR);
}

bool write_all(int fd, const uint8_t* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    off += static_cast<size_t>(w);
  }
  return true;
}

bool write_all(int fd, const std::vector<uint8_t>& data) {
  return data.empty() || write_all(fd, data.data(), data.size());
}

bool read_full(int fd, uint8_t* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t r = ::recv(fd, data + off, n - off, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    off += static_cast<size_t>(r);
  }
  return true;
}

bool read_message(int fd, uint8_t& type, std::vector<uint8_t>& payload) {
  uint8_t header[5];
  if (!read_full(fd, header, 5)) return false;
  type = header[0];
  const uint32_t len = (uint32_t)header[1] | ((uint32_t)header[2] << 8) | ((uint32_t)header[3] << 16) |
                       ((uint32_t)header[4] << 24);
  if (len > ndbg::kMaxPayload) {
    log_line("收到过长消息");
    return false;
  }
  payload.assign(len, 0);
  if (len == 0) return true;
  return read_full(fd, payload.data(), len);
}

std::string sanitize_relative(const std::string& raw) {
  std::filesystem::path rel;
  size_t i = 0;
  while (i < raw.size()) {
    const auto slash = raw.find_first_of("/\\", i);
    const auto part = raw.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
    i = slash == std::string::npos ? raw.size() : slash + 1;
    if (part.empty()) continue;
    if (part == "." || part == ".." || part.size() > 200) return {};
    for (unsigned char c : part) {
      if (c < 0x20 || c == 0x7f) return {};
    }
    rel /= part;
  }
  auto text = rel.generic_string();
  if (text.empty() || text.size() > 500) return {};
  return text;
}

std::filesystem::path resolve_transfer_path(const std::filesystem::path& root, const std::string& raw) {
  const auto relative = sanitize_relative(raw);
  if (relative.empty()) return {};
  const auto base = root.lexically_normal();
  const auto full = (base / relative).lexically_normal();
  auto base_s = base.generic_string();
  const auto full_s = full.generic_string();
  if (base_s.size() > 1 && base_s.back() == '/') base_s.pop_back();
  if (full_s.size() <= base_s.size() || full_s.compare(0, base_s.size(), base_s) != 0) return {};
  if (full_s[base_s.size()] != '/') return {};
  return full;
}

std::vector<uint8_t> file_message(uint8_t type, uint32_t id, uint8_t direction, const std::string& msg) {
  ndbg::ByteWriter p;
  p.u32(id);
  p.u8(direction);
  p.u8(0);
  p.u16(static_cast<uint16_t>(std::min(msg.size(), static_cast<size_t>(65535))));
  if (!msg.empty()) p.raw(msg.data(), std::min(msg.size(), static_cast<size_t>(65535)));
  return ndbg::pack(type, p);
}

class Connection {
 public:
  Connection(int fd, const Config& cfg)
      : fd_(fd), cfg_(cfg), transfer_root_(cfg.download_dir.lexically_normal()) {}

  void run() {
    if (!handshake()) return;
    struct Guard {
      Connection* self;
      ~Guard() {
        self->request_stop();
        self->join_threads();
      }
    } guard{this};

    encoder_ = std::thread([this] {
      try {
        encoder_main();
      } catch (const std::exception& ex) {
        log_line(std::string("编码线程异常: ") + ex.what());
        request_stop();
      }
    });
    writer_ = std::thread([this] {
      try {
        writer_main();
      } catch (const std::exception& ex) {
        log_line(std::string("发送线程异常: ") + ex.what());
        request_stop();
      }
    });
    x_.start([this](std::shared_ptr<CapturedFrame> frame) { on_frame(std::move(frame)); },
             [this](const std::string& text) { on_clipboard(text); });
    read_loop();
  }

 private:
  int fd_ = -1;
  const Config& cfg_;
  std::filesystem::path transfer_root_;
  std::atomic<bool> stop_{false};
  XHost x_;
  std::thread encoder_;
  std::thread writer_;

  std::mutex cap_mu_;
  std::condition_variable cap_cv_;
  std::shared_ptr<CapturedFrame> latest_;

  std::mutex out_mu_;
  std::condition_variable out_cv_;
  std::deque<std::vector<uint8_t>> hi_;
  std::vector<uint8_t> frame_;
  bool frame_ready_ = false;

  bool dl_active_ = false;
  bool dl_header_sent_ = false;
  bool dl_abort_ = false;
  uint32_t dl_id_ = 0;
  uint32_t next_dl_ = 1;
  uint64_t dl_size_ = 0;
  uint64_t dl_sent_ = 0;
  std::string dl_name_;
  std::ifstream dl_in_;

  bool up_active_ = false;
  uint32_t up_id_ = 0;
  uint64_t up_expected_ = 0;
  uint64_t up_received_ = 0;
  std::filesystem::path up_path_;
  std::ofstream up_out_;
  uint32_t clip_seq_ = 0;

  void request_stop() {
    stop_.store(true);
    out_cv_.notify_all();
    cap_cv_.notify_all();
    x_.request_stop();
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
  }

  void join_threads() {
    if (writer_.joinable()) writer_.join();
    if (encoder_.joinable()) encoder_.join();
    x_.join();
  }

  void queue_hi(std::vector<uint8_t> msg) {
    {
      std::lock_guard<std::mutex> lock(out_mu_);
      if (hi_.size() < 64) hi_.push_back(std::move(msg));
    }
    out_cv_.notify_one();
  }

  void publish_frame(std::vector<uint8_t> msg) {
    {
      std::lock_guard<std::mutex> lock(out_mu_);
      frame_ = std::move(msg);
      frame_ready_ = true;
    }
    out_cv_.notify_one();
  }

  void on_frame(std::shared_ptr<CapturedFrame> frame) {
    {
      std::lock_guard<std::mutex> lock(cap_mu_);
      latest_ = std::move(frame);
    }
    cap_cv_.notify_one();
  }

  void on_clipboard(const std::string& text) {
    if (text.size() > ndbg::kMaxClipboard) return;
    ndbg::ByteWriter p;
    p.u32(++clip_seq_);
    p.u32(static_cast<uint32_t>(text.size()));
    if (!text.empty()) p.raw(text.data(), text.size());
    queue_hi(ndbg::pack(ndbg::Clipboard, p));
  }

  bool handshake() {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    if (!read_message(fd_, type, payload) || type != ndbg::Hello) return false;
    ndbg::ByteReader reader(payload);
    uint8_t magic[8];
    if (!reader.raw(magic, 8) || !reader.ok) return false;
    const uint16_t version = reader.u16();
    if (!reader.ok || std::memcmp(magic, ndbg::kMagic, 8) != 0) {
      log_line("连接魔术字不匹配");
      return false;
    }
    if (version != 1) {
      ndbg::ByteWriter fail;
      fail.u8(2);
      write_all(fd_, ndbg::pack(ndbg::AuthFail, fail));
      return false;
    }

    uint8_t nonce[16];
    std::ifstream random_file("/dev/urandom", std::ios::binary);
    random_file.read(reinterpret_cast<char*>(nonce), 16);
    if (random_file.gcount() != 16) {
      log_line("无法读取随机数");
      return false;
    }
    ndbg::ByteWriter challenge;
    challenge.raw(nonce, 16);
    if (!write_all(fd_, ndbg::pack(ndbg::Challenge, challenge))) return false;

    if (!read_message(fd_, type, payload) || type != ndbg::Auth || payload.size() != 32) return false;
    uint8_t expect[32];
    ndbg::hmac_sha256(reinterpret_cast<const uint8_t*>(cfg_.password.data()), cfg_.password.size(), nonce, 16,
                      expect);
    if (!ndbg::ct_equal(expect, payload.data(), 32)) {
      ndbg::ByteWriter fail;
      fail.u8(1);
      write_all(fd_, ndbg::pack(ndbg::AuthFail, fail));
      log_line("口令校验失败");
      return false;
    }
    ndbg::ByteWriter ok;
    ok.u16(0);
    ok.u16(0);
    log_line("客户端已通过认证");
    return write_all(fd_, ndbg::pack(ndbg::AuthOk, ok));
  }

  void encoder_main() {
    tjhandle tj = tjInitCompress();
    if (!tj) {
      log_line("turbojpeg 初始化失败");
      request_stop();
      return;
    }
    uint32_t seen = 0;
    auto last_log = std::chrono::steady_clock::time_point{};
    while (!stop_.load()) {
      std::shared_ptr<CapturedFrame> frame;
      {
        std::unique_lock<std::mutex> lock(cap_mu_);
        cap_cv_.wait(lock, [&] { return stop_.load() || (latest_ && latest_->id != seen); });
        if (stop_.load()) break;
        frame = latest_;
        seen = frame->id;
      }
      auto packet = encode_frame(tj, *frame, last_log);
      if (!packet.empty()) publish_frame(std::move(packet));
    }
    tjDestroy(tj);
  }

  std::vector<uint8_t> encode_frame(tjhandle tj, const CapturedFrame& frame,
                                    std::chrono::steady_clock::time_point& last_log) {
    const size_t need = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3;
    if (frame.rgb.size() < need || frame.width == 0 || frame.height == 0) return {};
    const uint8_t* data = frame.rgb.data();
    size_t size = need;
    std::vector<uint8_t> jpeg;
    if (frame.codec == 0) {
      unsigned char* out = nullptr;
      unsigned long out_size = 0;
      const int rc = tjCompress2(tj, frame.rgb.data(), frame.width, 0, frame.height, TJPF_RGB, &out, &out_size,
                                 TJSAMP_420, frame.quality, TJFLAG_FASTDCT);
      if (rc != 0 || !out) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log > std::chrono::seconds(2)) {
          log_line(std::string("JPEG 压缩失败: ") + tjGetErrorStr2(tj));
          last_log = now;
        }
        if (out) tjFree(out);
        return {};
      }
      jpeg.assign(out, out + out_size);
      tjFree(out);
      data = jpeg.data();
      size = jpeg.size();
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    ndbg::ByteWriter payload;
    payload.u8(frame.codec);
    payload.u8(0);
    payload.u16(frame.width);
    payload.u16(frame.height);
    payload.u16(frame.origin_w);
    payload.u16(frame.origin_h);
    payload.u32(frame.id);
    payload.u64(static_cast<uint64_t>(us));
    payload.u32(static_cast<uint32_t>(size));
    payload.raw(data, size);
    return ndbg::pack(ndbg::Frame, payload);
  }

  void writer_main() {
    while (!stop_.load()) {
      std::vector<uint8_t> msg;
      bool read_chunk = false;
      size_t chunk_n = 0;
      uint32_t chunk_id = 0;
      {
        std::unique_lock<std::mutex> lock(out_mu_);
        out_cv_.wait(lock, [&] { return stop_.load() || !hi_.empty() || frame_ready_ || dl_active_; });
        if (stop_.load()) return;
        if (!hi_.empty()) {
          msg = std::move(hi_.front());
          hi_.pop_front();
        } else if (frame_ready_) {
          msg = std::move(frame_);
          frame_ready_ = false;
        } else if (dl_active_) {
          if (dl_abort_) {
            dl_in_.close();
            dl_active_ = false;
            dl_abort_ = false;
            continue;
          }
          if (!dl_header_sent_) {
            ndbg::ByteWriter payload;
            payload.u32(dl_id_);
            payload.u8(1);
            payload.u8(0);
            payload.u16(static_cast<uint16_t>(dl_name_.size()));
            payload.u64(dl_size_);
            payload.raw(dl_name_.data(), dl_name_.size());
            msg = ndbg::pack(ndbg::FileStart, payload);
            dl_header_sent_ = true;
          } else if (dl_sent_ >= dl_size_) {
            ndbg::ByteWriter payload;
            payload.u32(dl_id_);
            payload.u8(1);
            payload.u8(0);
            msg = ndbg::pack(ndbg::FileDone, payload);
            dl_in_.close();
            dl_active_ = false;
          } else {
            chunk_n = static_cast<size_t>(std::min<uint64_t>(ndbg::kChunkSize, dl_size_ - dl_sent_));
            chunk_id = dl_id_;
            read_chunk = true;
          }
        }
      }

      if (read_chunk) {
        std::vector<uint8_t> buf(chunk_n);
        dl_in_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(chunk_n));
        const auto got = dl_in_.gcount();
        if (got <= 0) {
          queue_hi(file_message(ndbg::FileError, chunk_id, 1, "读取文件失败"));
          std::lock_guard<std::mutex> lock(out_mu_);
          dl_in_.close();
          dl_active_ = false;
          continue;
        }
        buf.resize(static_cast<size_t>(got));
        {
          std::lock_guard<std::mutex> lock(out_mu_);
          dl_sent_ += static_cast<uint64_t>(got);
        }
        ndbg::ByteWriter payload;
        payload.u32(chunk_id);
        payload.u32(static_cast<uint32_t>(buf.size()));
        payload.raw(buf.data(), buf.size());
        msg = ndbg::pack(ndbg::FileChunk, payload);
      }

      if (!msg.empty() && !write_all(fd_, msg)) {
        request_stop();
        return;
      }
    }
  }

  void fail_upload(const std::string& message) {
    if (up_out_.is_open()) up_out_.close();
    std::error_code ec;
    if (!up_path_.empty()) std::filesystem::remove(up_path_, ec);
    queue_hi(file_message(ndbg::FileError, up_id_, 0, message));
    up_active_ = false;
    up_path_.clear();
  }

  void read_loop() {
    while (!stop_.load() && !g_stop) {
      uint8_t type = 0;
      std::vector<uint8_t> payload;
      if (!read_message(fd_, type, payload)) break;
      if (!handle(type, payload)) break;
    }
  }

  bool handle(uint8_t type, const std::vector<uint8_t>& payload) {
    ndbg::ByteReader reader(payload);
    switch (type) {
      case ndbg::StreamConfig: {
        ndbg::StreamSettings settings;
        settings.codec = reader.u8();
        settings.jpeg_quality = reader.u8();
        settings.max_width = reader.u16();
        settings.max_fps = reader.u8();
        reader.u8();
        if (!reader.ok) return false;
        x_.set_settings(settings);
        log_line(std::string("流配置 ") + (settings.codec == 0 ? "JPEG" : "原始") +
                 " 质量=" + std::to_string(settings.jpeg_quality) +
                 " 宽度=" + (settings.max_width == 0 ? "原始" : std::to_string(settings.max_width)) +
                 " 帧率=" + std::to_string(settings.max_fps));
        return true;
      }
      case ndbg::Mouse: {
        const uint16_t x = reader.u16();
        const uint16_t y = reader.u16();
        const uint8_t button = reader.u8();
        const uint8_t action = reader.u8();
        const int16_t wheel = reader.i16();
        if (!reader.ok) return false;
        x_.push_mouse(x, y, button, action, wheel);
        return true;
      }
      case ndbg::Key: {
        const uint16_t vk = reader.u16();
        const uint8_t down = reader.u8();
        reader.u8();
        if (!reader.ok) return false;
        x_.push_key(vk, down != 0);
        return true;
      }
      case ndbg::Ping: {
        if (payload.size() != 8) return false;
        queue_hi(ndbg::pack(ndbg::Pong, payload));
        return true;
      }
      case ndbg::Clipboard: {
        const uint32_t seq = reader.u32();
        (void)seq;
        const uint32_t len = reader.u32();
        if (!reader.ok || len > ndbg::kMaxClipboard || len != reader.remaining()) return false;
        x_.push_clipboard(reader.str(len));
        return reader.ok;
      }
      case ndbg::ListRequest: {
        if (!payload.empty()) {
          const uint16_t path_len = reader.u16();
          const std::string path = reader.str(path_len);
          if (!reader.ok) return false;
          if (!path.empty()) {
            std::string error;
            if (!change_directory(path, error))
              queue_hi(file_message(ndbg::FileError, 0, 2, error));
          }
        }
        send_file_list();
        return true;
      }
      case ndbg::FileRequest:
        return start_download(reader);
      case ndbg::FileDelete:
        return delete_entry(reader);
      case ndbg::FileMkdir:
        return make_directory(reader);
      case ndbg::FileStart:
        return start_upload(reader);
      case ndbg::FileChunk:
        return write_upload(reader);
      case ndbg::FileDone:
        return finish_upload(reader);
      case ndbg::FileAbort:
        return abort_transfer(reader);
      default:
        return true;
    }
  }

  bool change_directory(const std::string& raw, std::string& error) {
    if (raw.size() > 4000) {
      error = "路径过长";
      return false;
    }
    for (unsigned char c : raw) {
      if (c < 0x20 || c == 0x7f) {
        error = "路径无效";
        return false;
      }
    }
    std::string text = raw;
    if (text == "~" || text.rfind("~/", 0) == 0) {
      const char* home = std::getenv("HOME");
      if (home == nullptr || home[0] == '\0') {
        error = "找不到家目录";
        return false;
      }
      text = std::string(home) + text.substr(1);
    }
    std::filesystem::path path(text);
    if (!path.is_absolute()) {
      error = "请填写绝对路径";
      return false;
    }
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(path.lexically_normal(), ec);
    if (ec || !std::filesystem::is_directory(canon, ec)) {
      error = "打不开这个目录";
      return false;
    }
    transfer_root_ = canon.lexically_normal();
    return true;
  }

  void send_file_list() {
    struct Listed {
      std::string name;
      uint64_t size = 0;
      uint8_t kind = 0;
    };
    std::vector<Listed> files;
    const auto root = transfer_root_.lexically_normal();
    try {
      std::error_code rec_ec;
      auto it = std::filesystem::directory_iterator(
          root, std::filesystem::directory_options::skip_permission_denied, rec_ec);
      const auto end = std::filesystem::directory_iterator();
      for (; !rec_ec && it != end; it.increment(rec_ec)) {
        if (files.size() >= 4096) break;
        std::error_code ec;
        it->symlink_status(ec);
        if (ec) continue;
        const auto filename = it->path().filename().string();
        if (filename.empty() || filename == "." || filename == ".." || filename.size() > 500) continue;
        std::error_code follow_ec;
        const bool is_dir = std::filesystem::is_directory(it->path(), follow_ec);
        if (follow_ec) continue;
        if (is_dir) {
          files.push_back(Listed{filename, 0, 1});
          continue;
        }
        const auto followed = std::filesystem::status(it->path(), ec);
        if (ec || !std::filesystem::is_regular_file(followed)) continue;
        const auto size = static_cast<uint64_t>(it->file_size(ec));
        files.push_back(Listed{filename, ec ? 0 : size, 0});
      }
    } catch (const std::exception& ex) {
      log_line(std::string("列举文件失败: ") + ex.what());
    }
    std::sort(files.begin(), files.end(), [](const Listed& a, const Listed& b) { return a.name < b.name; });
    const auto root_text = root.string();
    ndbg::ByteWriter payload;
    payload.u16(static_cast<uint16_t>(std::min(root_text.size(), static_cast<size_t>(65535))));
    payload.raw(root_text.data(), std::min(root_text.size(), static_cast<size_t>(65535)));
    payload.u16(static_cast<uint16_t>(files.size()));
    for (const auto& file : files) {
      payload.u8(file.kind);
      payload.u64(file.size);
      payload.u16(static_cast<uint16_t>(file.name.size()));
      payload.raw(file.name.data(), file.name.size());
    }
    queue_hi(ndbg::pack(ndbg::FileList, payload));
  }

  bool path_in_use(const std::filesystem::path& path) {
    const auto norm = path.lexically_normal().generic_string();
    auto covers = [&](const std::filesystem::path& other) {
      if (other.empty()) return false;
      const auto text = other.lexically_normal().generic_string();
      return text == norm || text.rfind(norm + "/", 0) == 0;
    };
    if (up_active_ && covers(up_path_)) return true;
    if (dl_active_ && !dl_name_.empty()) return covers(resolve_transfer_path(transfer_root_, dl_name_));
    return false;
  }

  bool delete_entry(ndbg::ByteReader& reader) {
    const uint16_t name_len = reader.u16();
    const std::string name = reader.str(name_len);
    const auto path = resolve_transfer_path(transfer_root_, name);
    if (!reader.ok || path.empty()) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "文件名无效"));
      return reader.ok;
    }
    if (path_in_use(path)) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "文件正在传输，不能删除"));
      return true;
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "文件不存在"));
      return true;
    }
    if (std::filesystem::is_symlink(status) || std::filesystem::is_regular_file(status)) {
      std::filesystem::remove(path, ec);
    } else if (std::filesystem::is_directory(status)) {
      std::filesystem::remove_all(path, ec);
    } else {
      queue_hi(file_message(ndbg::FileError, 0, 2, "不能删除这种文件"));
      return true;
    }
    if (ec) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "无法删除"));
      return true;
    }
    queue_hi(file_message(ndbg::FileError, 0, 2, "已删除 " + path.string()));
    send_file_list();
    return true;
  }

  bool make_directory(ndbg::ByteReader& reader) {
    const uint16_t name_len = reader.u16();
    const std::string name = reader.str(name_len);
    const auto path = resolve_transfer_path(transfer_root_, name);
    if (!reader.ok || path.empty()) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "文件夹名无效"));
      return reader.ok;
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (!ec && std::filesystem::is_symlink(status)) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "不能在符号链接上建目录"));
      return true;
    }
    if (!ec && std::filesystem::is_regular_file(status)) {
      queue_hi(file_message(ndbg::FileError, 0, 2, "同名文件已存在"));
      return true;
    }
    std::filesystem::create_directories(path, ec);
    if (ec) queue_hi(file_message(ndbg::FileError, 0, 2, "无法创建文件夹"));
    return true;
  }

  bool start_download(ndbg::ByteReader& reader) {
    const uint16_t name_len = reader.u16();
    const std::string name = reader.str(name_len);
    const auto path = resolve_transfer_path(transfer_root_, name);
    if (!reader.ok || path.empty()) {
      queue_hi(file_message(ndbg::FileError, 0, 1, "文件名无效"));
      return reader.ok;
    }
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
      queue_hi(file_message(ndbg::FileError, 0, 1, "文件不存在"));
      return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      queue_hi(file_message(ndbg::FileError, 0, 1, "无法打开文件"));
      return true;
    }
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    in.seekg(0);
    if (end < 0) {
      queue_hi(file_message(ndbg::FileError, 0, 1, "无法读取文件大小"));
      return true;
    }
    bool busy = false;
    {
      std::lock_guard<std::mutex> lock(out_mu_);
      busy = dl_active_;
      if (!busy) {
        dl_in_ = std::move(in);
        dl_id_ = next_dl_++;
        if (next_dl_ == 0) next_dl_ = 1;
        dl_name_ = sanitize_relative(name);
        dl_size_ = static_cast<uint64_t>(end);
        dl_sent_ = 0;
        dl_header_sent_ = false;
        dl_abort_ = false;
        dl_active_ = true;
      }
    }
    if (busy) {
      queue_hi(file_message(ndbg::FileError, 0, 1, "已有下载任务"));
      return true;
    }
    out_cv_.notify_one();
    return true;
  }

  bool start_upload(ndbg::ByteReader& reader) {
    const uint32_t id = reader.u32();
    const uint8_t direction = reader.u8();
    reader.u8();
    const uint16_t name_len = reader.u16();
    const uint64_t size = reader.u64();
    const std::string name = reader.str(name_len);
    const auto path = resolve_transfer_path(transfer_root_, name);
    if (!reader.ok || direction != 0) return false;
    if (up_active_) {
      queue_hi(file_message(ndbg::FileError, id, 0, "已有上传任务"));
      return true;
    }
    constexpr uint64_t kMaxFile = 64ull << 30;
    if (path.empty() || size > kMaxFile) {
      queue_hi(file_message(ndbg::FileError, id, 0, "文件名无效或文件过大"));
      return true;
    }
    std::error_code make_ec;
    std::filesystem::create_directories(path.parent_path(), make_ec);
    std::error_code ec;
    const auto existing = std::filesystem::symlink_status(path, ec);
    if (!ec && std::filesystem::is_directory(existing)) {
      queue_hi(file_message(ndbg::FileError, id, 0, "不能把文件写到目录上"));
      return true;
    }
    if (std::filesystem::is_symlink(existing)) {
      queue_hi(file_message(ndbg::FileError, id, 0, "不能覆盖符号链接"));
      return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      queue_hi(file_message(ndbg::FileError, id, 0, "无法写入文件"));
      return true;
    }
    up_active_ = true;
    up_id_ = id;
    up_expected_ = size;
    up_received_ = 0;
    up_path_ = path;
    up_out_ = std::move(out);
    return true;
  }

  bool write_upload(ndbg::ByteReader& reader) {
    const uint32_t id = reader.u32();
    const uint32_t len = reader.u32();
    if (!reader.ok || len > 65536 || len != reader.remaining()) return false;
    if (!up_active_ || id != up_id_) return true;
    if (up_received_ > up_expected_ || len > up_expected_ - up_received_) {
      fail_upload("上传大小超出声明");
      return true;
    }
    const std::string bytes = reader.str(len);
    if (!reader.ok) return false;
    up_out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!up_out_) {
      fail_upload("写入失败");
      return true;
    }
    up_received_ += len;
    return true;
  }

  bool finish_upload(ndbg::ByteReader& reader) {
    const uint32_t id = reader.u32();
    const uint8_t direction = reader.u8();
    reader.u8();
    if (!reader.ok) return false;
    if (direction != 0 || !up_active_ || id != up_id_) return true;
    up_out_.close();
    if (up_received_ != up_expected_) {
      fail_upload("大小不一致");
      return true;
    }
    ndbg::ByteWriter done;
    done.u32(up_id_);
    done.u8(0);
    done.u8(0);
    queue_hi(ndbg::pack(ndbg::FileDone, done));
    up_active_ = false;
    up_path_.clear();
    log_line("已接收文件");
    return true;
  }

  bool abort_transfer(ndbg::ByteReader& reader) {
    const uint32_t id = reader.u32();
    const uint8_t direction = reader.u8();
    if (!reader.ok) return false;
    if (direction == 0 && up_active_ && (id == 0 || id == up_id_)) {
      if (up_out_.is_open()) up_out_.close();
      std::error_code ec;
      if (!up_path_.empty()) std::filesystem::remove(up_path_, ec);
      up_active_ = false;
      up_path_.clear();
      return true;
    }
    if (direction == 1) {
      std::lock_guard<std::mutex> lock(out_mu_);
      if (dl_active_ && (id == 0 || id == dl_id_)) dl_abort_ = true;
    }
    out_cv_.notify_one();
    return true;
  }
};

void tune_socket(int fd) {
  int yes = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
#ifdef TCP_USER_TIMEOUT
  unsigned int timeout_ms = 20000;
  setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
#endif
}

}  // namespace

int run_server(const Config& cfg) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    log_line("创建套接字失败");
    return 1;
  }
  int yes = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(cfg.port);
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    log_line("绑定端口 " + std::to_string(cfg.port) + " 失败");
    ::close(listen_fd);
    return 1;
  }
  if (listen(listen_fd, 2) != 0) {
    log_line("监听失败");
    ::close(listen_fd);
    return 1;
  }
  g_listen_fd = listen_fd;
  log_line("正在监听 0.0.0.0:" + std::to_string(cfg.port));

  while (!g_stop) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const int client = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (client < 0) {
      if (g_stop) break;
      if (errno == EINTR) continue;
      log_line("接受连接失败");
      break;
    }
    char ip[64] = {};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    log_line(std::string("客户端连入 ") + ip);
    tune_socket(client);
    g_client_fd = client;
    Connection(client, cfg).run();
    g_client_fd = -1;
    ::close(client);
    log_line("客户端已断开");
  }

  g_listen_fd = -1;
  ::close(listen_fd);
  log_line("服务退出");
  return 0;
}
