#include "session/session.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QTcpSocket>
#include <QTimer>

#include <chrono>
#include <cstring>

Session::Session(QObject* parent) : QObject(parent) {
    m_ping = new QTimer(this);
    m_ping->setInterval(1000);
    connect(m_ping, &QTimer::timeout, this, &Session::onPing);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, &Session::onConnectTimeout);
}

void Session::start(QString host, int port, QString password, proto::StreamSettings settings) {
    if (m_running) return;
    m_stop = false;
    m_running = true;
    m_host = std::move(host);
    m_port = port;
    m_password = std::move(password);
    m_settings = settings;
    openSocket();
}

void Session::stop() {
    m_stop = true;
    if (m_socket) {
        handleDrop(QString::fromUtf8("连接已断开"), false);
        return;
    }
    if (!m_running) return;
    m_running = false;
    m_ping->stop();
    m_connectTimer->stop();
    emit dropped();
    emit sessionEnded();
}

void Session::setAutoReconnect(bool on) { m_autoReconnect = on; }

void Session::updateSettings(proto::StreamSettings settings) {
    m_settings = settings;
    if (m_authed) send(proto::StreamConfig, m_settings.toPayload());
}

bool Session::sendMouse(quint16 x, quint16 y, quint8 button, quint8 action, qint16 wheel) {
    proto::Writer writer;
    writer.u16(x);
    writer.u16(y);
    writer.u8(button);
    writer.u8(action);
    writer.i16(wheel);
    return send(proto::Mouse, writer.take());
}

bool Session::sendKey(quint16 virtualKey, bool down) {
    proto::Writer writer;
    writer.u16(virtualKey);
    writer.u8(down ? 1 : 0);
    writer.u8(0);
    return send(proto::Key, writer.take());
}

bool Session::sendClipboard(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() > proto::kMaxClipboard) return false;
    proto::Writer writer;
    writer.u32(++m_clipSeq);
    writer.u32(quint32(bytes.size()));
    writer.raw(bytes);
    return send(proto::Clipboard, writer.take());
}

bool Session::requestFileList(const QString& path) {
    if (path.isEmpty()) return send(proto::ListRequest, {});
    const QByteArray bytes = path.toUtf8();
    if (bytes.size() > 4000) return false;
    proto::Writer writer;
    writer.u16(quint16(bytes.size()));
    writer.raw(bytes);
    return send(proto::ListRequest, writer.take());
}

bool Session::requestMkdir(const QString& name) {
    const QByteArray bytes = name.toUtf8();
    if (bytes.isEmpty() || bytes.size() > 4000) return false;
    proto::Writer writer;
    writer.u16(quint16(bytes.size()));
    writer.raw(bytes);
    return send(proto::FileMkdir, writer.take());
}

bool Session::requestDelete(const QString& name) {
    const QByteArray bytes = name.toUtf8();
    if (bytes.size() > 4000) return false;
    proto::Writer writer;
    writer.u16(quint16(bytes.size()));
    writer.raw(bytes);
    return send(proto::FileDelete, writer.take());
}

bool Session::requestDownload(const QString& name) {
    const QByteArray bytes = name.toUtf8();
    proto::Writer writer;
    writer.u16(quint16(bytes.size()));
    writer.raw(bytes);
    return send(proto::FileRequest, writer.take());
}

bool Session::beginUpload(quint32 id, const QString& name, quint64 size) {
    const QByteArray bytes = name.toUtf8();
    proto::Writer writer;
    writer.u32(id);
    writer.u8(0);
    writer.u8(0);
    writer.u16(quint16(bytes.size()));
    writer.u64(size);
    writer.raw(bytes);
    return send(proto::FileStart, writer.take());
}

bool Session::sendChunk(quint32 id, const QByteArray& data) {
    proto::Writer writer;
    writer.u32(id);
    writer.u32(quint32(data.size()));
    writer.raw(data);
    return send(proto::FileChunk, writer.take());
}

bool Session::finishUpload(quint32 id) {
    proto::Writer writer;
    writer.u32(id);
    writer.u8(0);
    writer.u8(0);
    return send(proto::FileDone, writer.take());
}

bool Session::abortTransfer(quint32 id, quint8 direction) {
    proto::Writer writer;
    writer.u32(id);
    writer.u8(direction);
    writer.u8(0);
    writer.u16(0);
    return send(proto::FileAbort, writer.take());
}

void Session::openSocket() {
    if (m_stop || !m_running || m_socket) return;
    m_buffer.clear();
    m_authed = false;
    m_phase = Phase::WaitChallenge;
    m_lastError.clear();
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &Session::onConnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &Session::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &Session::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &Session::onSocketError);
    m_socket->connectToHost(m_host, quint16(m_port));
    m_connectTimer->start(5000);
}

void Session::onConnected() {
    m_connectTimer->stop();
    m_lastError.clear();
    if (!m_socket) return;
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    proto::Writer hello;
    hello.raw("NDBG0001", 8);
    hello.u16(1);
    if (!send(proto::Hello, hello.take())) handleDrop(QString::fromUtf8("发送握手失败"), false);
}

void Session::onReadyRead() {
    if (!m_socket) return;
    m_buffer.append(m_socket->readAll());
    while (m_socket && m_buffer.size() >= 5) {
        const quint32 length = quint32(quint8(m_buffer.at(1)))
            | (quint32(quint8(m_buffer.at(2))) << 8)
            | (quint32(quint8(m_buffer.at(3))) << 16)
            | (quint32(quint8(m_buffer.at(4))) << 24);
        if (length > quint32(proto::kMaxPayload)) {
            badPacket();
            return;
        }
        if (m_buffer.size() < 5 + int(length)) return;
        const quint8 type = quint8(m_buffer.at(0));
        const QByteArray payload = m_buffer.mid(5, int(length));
        m_buffer.remove(0, 5 + int(length));
        handlePacket(type, payload);
    }
}

void Session::onDisconnected() {
    if (!m_socket) return;
    const QString reason = m_lastError.isEmpty() ? QString::fromUtf8("连接已断开") : m_lastError;
    handleDrop(reason, false);
}

void Session::onSocketError() {
    if (!m_socket) return;
    m_lastError = m_socket->errorString();
    if (m_socket->state() == QAbstractSocket::UnconnectedState)
        handleDrop(m_lastError.isEmpty() ? QString::fromUtf8("连接失败") : m_lastError, false);
}

void Session::onConnectTimeout() {
    if (!m_socket || m_socket->state() == QAbstractSocket::ConnectedState) return;
    handleDrop(QString::fromUtf8("连接超时"), false);
}

void Session::onPing() {
    if (!m_authed) return;
    proto::Writer writer;
    writer.u64(nowTick());
    if (!send(proto::Ping, writer.take())) handleDrop(QString::fromUtf8("连接已断开"), false);
}

void Session::handleDrop(const QString& message, bool authFail) {
    if (m_dropping) return;
    m_dropping = true;
    m_ping->stop();
    m_connectTimer->stop();
    m_authed = false;
    m_phase = Phase::Idle;
    m_buffer.clear();
    m_latest = {};
    if (m_socket) {
        QTcpSocket* socket = m_socket;
        m_socket = nullptr;
        socket->disconnect(this);
        socket->abort();
        delete socket;
    }

    const bool finish = m_stop || authFail || !m_autoReconnect || !m_running;
    emit dropped();
    if (finish) {
        m_running = false;
        m_dropping = false;
        emit sessionEnded();
        return;
    }
    emit reconnecting(message);
    m_dropping = false;
    QTimer::singleShot(2000, this, [this] {
        if (!m_stop && m_running) openSocket();
    });
}

bool Session::send(quint8 type, const QByteArray& payload) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) return false;
    const QByteArray packet = proto::Writer::packet(type, payload);
    const qint64 written = m_socket->write(packet);
    if (written != packet.size()) {
        m_lastError = QString::fromUtf8("发送失败");
        return false;
    }
    return true;
}

void Session::badPacket() { handleDrop(QString::fromUtf8("连接已断开"), false); }

void Session::handlePacket(quint8 type, const QByteArray& payload) {
    if (type == proto::AuthFail) {
        const quint8 reason = payload.isEmpty() ? 1 : quint8(payload.at(0));
        emit authFailed(reason == 2 ? QString::fromUtf8("版本不符") : QString::fromUtf8("密码错误"));
        handleDrop(QString::fromUtf8("认证失败"), true);
        return;
    }

    if (m_phase == Phase::WaitChallenge) {
        if (type != proto::Challenge || payload.size() != 16) {
            handleDrop(QString::fromUtf8("服务端握手异常"), false);
            return;
        }
        const QByteArray mac = QMessageAuthenticationCode::hash(
            payload, m_password.toUtf8(), QCryptographicHash::Sha256);
        if (!send(proto::Auth, mac)) {
            handleDrop(QString::fromUtf8("发送认证失败"), false);
            return;
        }
        m_phase = Phase::WaitAuth;
        return;
    }

    if (m_phase != Phase::Live && type != proto::AuthOk) return;

    proto::Reader reader(payload);
    switch (type) {
    case proto::AuthOk: {
        m_authed = true;
        m_phase = Phase::Live;
        send(proto::StreamConfig, m_settings.toPayload());
        m_ping->start();
        emit authenticated();
        break;
    }
    case proto::Frame: {
        proto::RemoteFrame frame;
        frame.codec = reader.u8();
        reader.u8();
        frame.width = reader.u16();
        frame.height = reader.u16();
        frame.originWidth = reader.u16();
        frame.originHeight = reader.u16();
        frame.id = reader.u32();
        reader.u64();
        const quint32 dataLen = reader.u32();
        if (!reader.ok() || int(dataLen) != reader.remaining()) return;
        frame.data = reader.bytes(int(dataLen));
        if (!reader.ok()) return;
        m_latest = std::move(frame);
        deliverFrame();
        break;
    }
    case proto::Tiles: {
        const int imageW = reader.u16();
        const int imageH = reader.u16();
        const int originW = reader.u16();
        const int originH = reader.u16();
        reader.u32();
        reader.u64();
        const quint16 count = reader.u16();
        if (!reader.ok() || imageW <= 0 || imageH <= 0 || count > 4096) {
            badPacket();
            return;
        }
        FrameTileList tiles;
        tiles.reserve(count);
        for (int i = 0; i < count; ++i) {
            const int x = reader.u16();
            const int y = reader.u16();
            const int w = reader.u16();
            const int h = reader.u16();
            const quint32 jpegLen = reader.u32();
            if (!reader.ok() || w <= 0 || h <= 0 || jpegLen == 0 || jpegLen > quint32(proto::kMaxPayload)) {
                badPacket();
                return;
            }
            const QByteArray jpeg = reader.bytes(int(jpegLen));
            if (!reader.ok()) {
                badPacket();
                return;
            }
            QImage image;
            if (!image.loadFromData(jpeg, "JPEG") || image.isNull()) continue;
            FrameTile tile;
            tile.x = x;
            tile.y = y;
            tile.image = image;
            tiles.append(tile);
        }
        if (!reader.ok() || reader.remaining() != 0) {
            badPacket();
            return;
        }
        if (!tiles.isEmpty()) emit tilesReady(imageW, imageH, originW, originH, tiles);
        break;
    }
    case proto::Pong: {
        const quint64 stamp = reader.u64();
        if (!reader.ok()) break;
        const quint64 now = nowTick();
        if (now < stamp) break;
        const double ms = double(now - stamp) / 1'000'000.0;
        if (ms >= 0 && ms < 60000) emit latencyUpdated(ms);
        break;
    }
    case proto::Clipboard: {
        reader.u32();
        const quint32 len = reader.u32();
        if (!reader.ok() || len > quint32(proto::kMaxClipboard) || int(len) != reader.remaining()) {
            badPacket();
            return;
        }
        emit clipboardReceived(reader.str(int(len)));
        break;
    }
    case proto::FileList: {
        const quint16 rootLen = reader.u16();
        const QString root = reader.str(rootLen);
        const quint16 count = reader.u16();
        QList<proto::FileEntry> files;
        files.reserve(count);
        for (int i = 0; i < count; ++i) {
            proto::FileEntry entry;
            const quint8 kind = reader.u8();
            entry.size = reader.u64();
            const quint16 nameLen = reader.u16();
            entry.name = reader.str(nameLen);
            entry.isDirectory = kind == 1;
            if (!reader.ok()) {
                badPacket();
                return;
            }
            files.append(entry);
        }
        emit fileListReceived(root, files);
        break;
    }
    case proto::FileStart: {
        const quint32 id = reader.u32();
        const quint8 direction = reader.u8();
        reader.u8();
        const quint16 nameLen = reader.u16();
        const quint64 size = reader.u64();
        const QString name = reader.str(nameLen);
        if (!reader.ok()) {
            badPacket();
            return;
        }
        if (direction == 1) emit downloadStarted(id, name, size);
        break;
    }
    case proto::FileChunk: {
        const quint32 id = reader.u32();
        const quint32 len = reader.u32();
        if (!reader.ok() || len > 65536 || int(len) != reader.remaining()) {
            badPacket();
            return;
        }
        emit downloadChunk(id, reader.bytes(int(len)));
        break;
    }
    case proto::FileDone: {
        const quint32 id = reader.u32();
        const quint8 direction = reader.u8();
        if (!reader.ok()) {
            badPacket();
            return;
        }
        emit transferFinished(id, direction);
        break;
    }
    case proto::FileAbort:
    case proto::FileError: {
        const quint32 id = reader.u32();
        const quint8 direction = reader.u8();
        reader.u8();
        const quint16 msgLen = reader.u16();
        const QString message = reader.str(msgLen);
        if (!reader.ok()) {
            badPacket();
            return;
        }
        emit transferFailed(id, direction, message);
        break;
    }
    default:
        break;
    }
}

void Session::deliverFrame() {
    proto::RemoteFrame frame = std::move(m_latest);
    m_latest = {};
    QImage image;
    if (frame.codec == 0) {
        if (!image.loadFromData(frame.data, "JPEG")) return;
    } else {
        const int stride = frame.width * 3;
        if (frame.width <= 0 || frame.height <= 0 || frame.data.size() < stride * frame.height) return;
        image = QImage(frame.width, frame.height, QImage::Format_RGB888);
        for (int y = 0; y < frame.height; ++y)
            memcpy(image.scanLine(y), frame.data.constData() + y * stride, size_t(stride));
    }
    if (image.isNull()) return;
    emit frameReady(image, frame.originWidth, frame.originHeight);
}

quint64 Session::nowTick() {
    return quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
}
