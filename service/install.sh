#!/usr/bin/env bash
# Ubuntu 22.04：安装或更新 NetDebug 服务。
# 用桌面登录用户执行，不要直接用 root：
#   bash service/install.sh
# 已有的 ~/.config/netdebug/config.ini 会保留。

set -euo pipefail

SCRIPT=$(readlink -f "$0")
SERVICE=$(cd "$(dirname "$SCRIPT")" && pwd)

if [[ $(id -u) -eq 0 ]]; then
  if [[ -z ${SUDO_USER:-} || $SUDO_USER == root ]]; then
    echo "请用桌面登录用户运行： bash service/install.sh"
    exit 1
  fi
  exec runuser -u "$SUDO_USER" -- /bin/bash "$SCRIPT"
fi

if [[ -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  if [[ ${ID:-} != ubuntu || ${VERSION_ID:-} != 22.04 ]]; then
    echo "这个脚本针对 Ubuntu 22.04，当前是 ${PRETTY_NAME:-未知系统}"
    exit 1
  fi
fi

uid=$(id -u)
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$uid}"
BUILD="$SERVICE/build"

echo "安装编译依赖"
sudo DEBIAN_FRONTEND=noninteractive apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential \
  cmake \
  libx11-dev \
  libxext-dev \
  libxtst-dev \
  libxfixes-dev \
  libturbojpeg0-dev \
  zlib1g-dev

echo "编译"
if ! cmake -S "$SERVICE" -B "$BUILD"; then
  rm -rf "$BUILD"
  cmake -S "$SERVICE" -B "$BUILD"
fi
cmake --build "$BUILD" -j

echo "安装程序到 /usr/local"
sudo cmake --install "$BUILD"

mkdir -p "$HOME/.config/systemd/user" "$HOME/.config/autostart" "$HOME/.config/netdebug"
install -m 644 "$SERVICE/systemd/netdebug.service" "$HOME/.config/systemd/user/netdebug.service"
install -m 644 "$SERVICE/systemd/netdebug-import.desktop" "$HOME/.config/autostart/netdebug-import.desktop"
sudo loginctl enable-linger "$USER"

if [[ ! -d $XDG_RUNTIME_DIR ]]; then
  echo "程序已安装。登录桌面后会自动启动服务。"
  echo "配置文件是 ~/.config/netdebug/config.ini ，默认密码 netdebug。"
  exit 0
fi

systemctl --user daemon-reload
systemctl --user enable netdebug.service
if systemctl --user restart netdebug.service; then
  echo "服务已启动。"
else
  echo "程序已安装。进入桌面后会自动启动；若已经在桌面，可再执行："
  echo "  systemctl --user restart netdebug.service"
fi
echo "配置文件是 ~/.config/netdebug/config.ini 。已有密码不会被覆盖，默认密码 netdebug。"
echo "登录界面请选择 Ubuntu on Xorg。"
