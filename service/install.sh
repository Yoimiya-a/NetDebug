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

configure_unattended_login() {
  local session=ubuntu-xorg
  if [[ ! -f /usr/share/xsessions/${session}.desktop ]]; then
    session=ubuntu
  fi
  sudo python3 - "$USER" "$session" <<'PY'
import pathlib
import sys

user, session = sys.argv[1], sys.argv[2]


def update_section(text, section, values):
    lines = text.splitlines()
    header = f"[{section}]"
    result = []
    in_section = False
    found = False
    seen = set()

    def flush():
        for key, value in values.items():
            if key not in seen:
                result.append(f"{key}={value}")
                seen.add(key)

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if in_section:
                flush()
            in_section = stripped == header
            if in_section:
                found = True
                seen = set()
            result.append(line)
            continue
        body = stripped.lstrip("#").strip()
        if in_section and "=" in body:
            key = body.split("=", 1)[0].strip()
            if key in values:
                if key not in seen:
                    result.append(f"{key}={values[key]}")
                    seen.add(key)
                continue
        result.append(line)
    if in_section:
        flush()
    elif not found:
        if result and result[-1] != "":
            result.append("")
        result.append(header)
        for key, value in values.items():
            result.append(f"{key}={value}")
    return "\n".join(result) + "\n"


gdm = pathlib.Path("/etc/gdm3/custom.conf")
gdm.parent.mkdir(parents=True, exist_ok=True)
current = gdm.read_text() if gdm.exists() else "[daemon]\n"
gdm.write_text(update_section(current, "daemon", {
    "AutomaticLoginEnable": "True",
    "AutomaticLogin": user,
    "WaylandEnable": "false",
}))
gdm.chmod(0o644)

account = pathlib.Path("/var/lib/AccountsService/users") / user
account.parent.mkdir(parents=True, exist_ok=True)
existing = account.read_text() if account.exists() else "[User]\n"
account.write_text(update_section(existing, "User", {
    "Session": session,
    "XSession": session,
}))
account.chmod(0o600)
PY
  echo "已设置开机自动进入 ${USER} 的 Xorg 桌面，重启后不用在被控机上登录。"
}

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
  zlib1g-dev \
  python3

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
configure_unattended_login

if [[ ! -d $XDG_RUNTIME_DIR ]]; then
  echo "程序已安装。重启进入桌面后会自动启动服务。"
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
echo "请重启一次。之后开机就会自动进入桌面并启动服务，不用在被控机前登录。"
