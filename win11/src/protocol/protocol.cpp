#include "protocol/protocol.h"

namespace proto {

void Writer::u8(quint8 value) { m_data.append(char(value)); }

void Writer::u16(quint16 value) {
    m_data.append(char(value & 0xff));
    m_data.append(char((value >> 8) & 0xff));
}

void Writer::u32(quint32 value) {
    u16(quint16(value & 0xffff));
    u16(quint16(value >> 16));
}

void Writer::u64(quint64 value) {
    u32(quint32(value & 0xffffffffu));
    u32(quint32(value >> 32));
}

void Writer::i16(qint16 value) { u16(quint16(value)); }

void Writer::raw(const char* data, int size) {
    if (data && size > 0) m_data.append(data, size);
}

void Writer::raw(const QByteArray& data) { m_data.append(data); }

QByteArray Writer::take() { return std::move(m_data); }

QByteArray Writer::packet(quint8 type, const QByteArray& payload) {
    Writer writer;
    writer.u8(type);
    writer.u32(quint32(payload.size()));
    writer.raw(payload);
    return writer.take();
}

QByteArray StreamSettings::toPayload() const {
    Writer writer;
    writer.u8(codec);
    writer.u8(jpegQuality);
    writer.u16(maxWidth);
    writer.u8(maxFps);
    writer.u8(0);
    return writer.take();
}

Reader::Reader(QByteArray data) : m_data(std::move(data)) {}

int Reader::remaining() const { return qMax(0, m_data.size() - m_index); }

quint8 Reader::u8() {
    if (m_index + 1 > m_data.size()) {
        m_ok = false;
        return 0;
    }
    return quint8(m_data.at(m_index++));
}

quint16 Reader::u16() {
    const quint16 lo = u8();
    const quint16 hi = u8();
    return quint16(lo | (hi << 8));
}

quint32 Reader::u32() {
    const quint32 lo = u16();
    const quint32 hi = u16();
    return lo | (hi << 16);
}

quint64 Reader::u64() {
    const quint64 lo = u32();
    const quint64 hi = u32();
    return lo | (hi << 32);
}

qint16 Reader::i16() { return qint16(u16()); }

QByteArray Reader::bytes(int length) {
    if (length < 0 || m_index > m_data.size() || length > m_data.size() - m_index) {
        m_ok = false;
        return {};
    }
    const QByteArray out = m_data.mid(m_index, length);
    m_index += length;
    return out;
}

QString Reader::str(int length) {
    const QByteArray data = bytes(length);
    return m_ok ? QString::fromUtf8(data) : QString();
}

QString formatSize(quint64 size) {
    if (size < 1024) return QString::number(size) + QString::fromUtf8(" B");
    if (size < 1024ull * 1024) return QString::number(size / 1024.0, 'f', 1) + QString::fromUtf8(" KB");
    if (size < 1024ull * 1024 * 1024) return QString::number(size / 1024.0 / 1024.0, 'f', 1) + QString::fromUtf8(" MB");
    return QString::number(size / 1024.0 / 1024.0 / 1024.0, 'f', 2) + QString::fromUtf8(" GB");
}

}  // namespace proto
