#pragma once

#include "protocol/protocol.h"

#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

struct FrameTile {
    int x = 0;
    int y = 0;
    QImage image;
};
using FrameTileList = QList<FrameTile>;
Q_DECLARE_METATYPE(FrameTile)
Q_DECLARE_METATYPE(FrameTileList)

class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);

public slots:
    void start(QString host, int port, QString password, proto::StreamSettings settings);
    void stop();
    void setAutoReconnect(bool on);
    void updateSettings(proto::StreamSettings settings);
    bool sendMouse(quint16 x, quint16 y, quint8 button, quint8 action, qint16 wheel);
    bool sendKey(quint16 virtualKey, bool down);
    bool sendClipboard(const QString& text);
    bool requestFileList(const QString& path);
    bool requestMkdir(const QString& name);
    bool requestDelete(const QString& name);
    bool requestDownload(const QString& name);
    bool beginUpload(quint32 id, const QString& name, quint64 size);
    bool sendChunk(quint32 id, const QByteArray& data);
    bool finishUpload(quint32 id);
    bool abortTransfer(quint32 id, quint8 direction);

signals:
    void authenticated();
    void authFailed(const QString& reason);
    void dropped();
    void reconnecting(const QString& reason);
    void sessionEnded();
    void frameReady(const QImage& image, int originWidth, int originHeight);
    void tilesReady(int imageWidth, int imageHeight, int originWidth, int originHeight, const FrameTileList& tiles);
    void clipboardReceived(const QString& text);
    void fileListReceived(const QString& root, const QList<proto::FileEntry>& files);
    void downloadStarted(quint32 id, const QString& name, quint64 size);
    void downloadChunk(quint32 id, const QByteArray& data);
    void transferFinished(quint32 id, quint8 direction);
    void transferFailed(quint32 id, quint8 direction, const QString& message);
    void latencyUpdated(double milliseconds);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void onConnectTimeout();
    void onPing();
    void deliverFrame();

private:
    enum class Phase { Idle, WaitChallenge, WaitAuth, Live };

    void openSocket();
    void handleDrop(const QString& message, bool authFail);
    bool send(quint8 type, const QByteArray& payload);
    void handlePacket(quint8 type, const QByteArray& payload);
    void badPacket();
    static quint64 nowTick();

    class QTcpSocket* m_socket = nullptr;
    class QTimer* m_ping = nullptr;
    class QTimer* m_connectTimer = nullptr;

    QString m_host;
    int m_port = 0;
    QString m_password;
    proto::StreamSettings m_settings;
    QByteArray m_buffer;
    proto::RemoteFrame m_latest;
    Phase m_phase = Phase::Idle;
    QString m_lastError;
    quint32 m_clipSeq = 0;
    bool m_autoReconnect = true;
    bool m_running = false;
    bool m_stop = false;
    bool m_authed = false;
    bool m_dropping = false;
};
