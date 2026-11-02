#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

namespace proto {

constexpr int kMaxPayload = 32 * 1024 * 1024;
constexpr int kMaxClipboard = 1024 * 1024;
constexpr int kChunkSize = 32 * 1024;

enum Msg : quint8 {
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
    Tiles = 23,
};

struct StreamSettings {
    quint8 codec = 0;
    quint8 jpegQuality = 30;
    quint16 maxWidth = 1280;
    quint8 maxFps = 30;

    QByteArray toPayload() const;
};

struct RemoteFrame {
    quint8 codec = 0;
    int width = 0;
    int height = 0;
    int originWidth = 0;
    int originHeight = 0;
    quint32 id = 0;
    QByteArray data;
};

struct FileEntry {
    QString name;
    quint64 size = 0;
    bool isDirectory = false;
};

struct Packet {
    quint8 type = 0;
    QByteArray payload;
};

class Writer {
public:
    void u8(quint8 value);
    void u16(quint16 value);
    void u32(quint32 value);
    void u64(quint64 value);
    void i16(qint16 value);
    void raw(const char* data, int size);
    void raw(const QByteArray& data);
    QByteArray take();
    static QByteArray packet(quint8 type, const QByteArray& payload);

private:
    QByteArray m_data;
};

class Reader {
public:
    explicit Reader(QByteArray data);
    bool ok() const { return m_ok; }
    int remaining() const;
    quint8 u8();
    quint16 u16();
    quint32 u32();
    quint64 u64();
    qint16 i16();
    QByteArray bytes(int length);
    QString str(int length);

private:
    QByteArray m_data;
    int m_index = 0;
    bool m_ok = true;
};

QString formatSize(quint64 size);

}  // namespace proto

using ProtoFileList = QList<proto::FileEntry>;
Q_DECLARE_METATYPE(proto::FileEntry)
Q_DECLARE_METATYPE(ProtoFileList)
