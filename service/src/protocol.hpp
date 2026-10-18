#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// 小端。消息头：type(u8) + length(u32) + payload。
// length 只计 payload。单条 payload 上限 32MB。
//
// Hello          客户端→服务端  magic[8]="NDBG0001"  version u16=1
// Challenge      服务端→客户端  nonce[16]
// Auth           客户端→服务端  hmac[32] = HMAC-SHA256(口令, nonce)
// AuthOk         服务端→客户端  desktop_w u16, desktop_h u16（可为 0，真实尺寸在画面帧里）
// AuthFail       服务端→客户端  reason u8（1 口令错误，2 版本不符）
// StreamConfig   客户端→服务端  codec u8, jpeg_quality u8, max_width u16, max_fps u8, reserved u8
//                              codec: 0 JPEG，1 原始 RGB24
//                              max_width: 0 表示不缩放
// Frame          服务端→客户端  codec u8, reserved u8, width u16, height u16,
//                              origin_w u16, origin_h u16, frame_id u32, timestamp_us u64,
//                              data_len u32, data
//                              原始像素为紧凑 RGB24，自上而下
// Mouse          客户端→服务端  x u16, y u16, button u8, action u8, wheel i16
//                              button: 1 左 2 右 3 中
//                              action: 0 移动 1 按下 2 抬起 3 滚轮
//                              wheel: 格数，远离用户为正
//                              x/y 是 Ubuntu 桌面像素
// Key            客户端→服务端  virtual_key u16, down u8, reserved u8
//                              virtual_key 是 Windows 虚拟键码
// Ping/Pong      双向           opaque u64，服务端原样回
// Clipboard      双向           seq u32, text_len u32, utf8。文本上限 1MB
// FileStart      双向           id u32, direction u8, reserved u8, name_len u16, size u64, name
//                              direction: 0 客户端发往服务端，1 服务端发往客户端
// FileChunk      双向           id u32, data_len u32, data。data 不超过 65536
// FileDone       双向           id u32, direction u8, reserved u8
// FileAbort      双向           id u32, direction u8, reserved u8, msg_len u16, msg
// FileError      双向           布局同 FileAbort
// ListRequest    客户端→服务端  空表示留在当前目录；否则 path_len u16, path
//                              path 是绝对路径，也可以用 ~/ 开头
// FileList       服务端→客户端  root_len u16, root,
//                              count u16，重复：kind u8, size u64, name_len u16, name
//                              kind 0 文件，1 目录。name 是传输目录内的相对路径，用 /
// FileRequest    客户端→服务端  name_len u16, name。name 同样是相对路径
// FileDelete     客户端→服务端  name_len u16, name。删除当前目录里的文件或文件夹
// FileMkdir      客户端→服务端  name_len u16, name。在当前目录里创建文件夹

namespace ndbg {

enum Msg : uint8_t {
  Hello = 1,
  Challenge = 2,
  Auth = 3,
  AuthOk = 4,
  AuthFail = 5,
  StreamConfig = 6,
  Frame = 7,
  Mouse = 8,
  Key = 9,
  Ping = 10,
  Pong = 11,
  Clipboard = 12,
  FileStart = 13,
  FileChunk = 14,
  FileDone = 15,
  FileAbort = 16,
  FileError = 17,
  ListRequest = 18,
  FileList = 19,
  FileRequest = 20,
  FileDelete = 21,
  FileMkdir = 22,
};

constexpr uint32_t kMaxPayload = 32u * 1024u * 1024u;
constexpr uint32_t kMaxClipboard = 1024u * 1024u;
constexpr uint32_t kChunkSize = 32u * 1024u;
constexpr char kMagic[8] = {'N', 'D', 'B', 'G', '0', '0', '0', '1'};

struct StreamSettings {
  uint8_t codec = 0;
  uint8_t jpeg_quality = 30;
  uint16_t max_width = 1280;
  uint8_t max_fps = 30;
};

struct ByteWriter {
  std::vector<uint8_t> b;

  void u8(uint8_t v) { b.push_back(v); }

  void u16(uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  }

  void u32(uint32_t v) {
    u16(static_cast<uint16_t>(v & 0xffff));
    u16(static_cast<uint16_t>(v >> 16));
  }

  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v & 0xffffffffu));
    u32(static_cast<uint32_t>(v >> 32));
  }

  void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }

  void raw(const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    b.insert(b.end(), c, c + n);
  }
};

struct ByteReader {
  const uint8_t* p = nullptr;
  size_t n = 0;
  size_t i = 0;
  bool ok = true;

  explicit ByteReader(const std::vector<uint8_t>& buf) : p(buf.data()), n(buf.size()) {}
  ByteReader(const uint8_t* data, size_t len) : p(data), n(len) {}

  uint8_t u8() {
    if (i + 1 > n) {
      ok = false;
      return 0;
    }
    return p[i++];
  }

  uint16_t u16() {
    if (i + 2 > n) {
      ok = false;
      return 0;
    }
    const uint16_t v = static_cast<uint16_t>(p[i] | (p[i + 1] << 8));
    i += 2;
    return v;
  }

  uint32_t u32() {
    const uint32_t lo = u16();
    const uint32_t hi = u16();
    return lo | (hi << 16);
  }

  uint64_t u64() {
    const uint64_t lo = u32();
    const uint64_t hi = u32();
    return lo | (hi << 32);
  }

  int16_t i16() { return static_cast<int16_t>(u16()); }

  bool raw(void* out, size_t len) {
    if (len > n || i > n - len) {
      ok = false;
      return false;
    }
    std::memcpy(out, p + i, len);
    i += len;
    return true;
  }

  std::string str(size_t len) {
    if (len > n || i > n - len) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p + i), len);
    i += len;
    return s;
  }

  size_t remaining() const { return i > n ? 0 : n - i; }
};

inline std::vector<uint8_t> pack(uint8_t type, const std::vector<uint8_t>& payload) {
  ByteWriter w;
  w.u8(type);
  w.u32(static_cast<uint32_t>(payload.size()));
  if (!payload.empty()) w.raw(payload.data(), payload.size());
  return w.b;
}

inline std::vector<uint8_t> pack(uint8_t type, const ByteWriter& payload) { return pack(type, payload.b); }

}  // namespace ndbg
