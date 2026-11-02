#pragma once

#include "protocol/protocol.h"
#include "session/session.h"
#include "ui/screenview.h"

#include <QByteArray>
#include <QMainWindow>
#include <QSet>

#include <atomic>
#include <utility>

class Discovery;
class FileTree;
class QCheckBox;
class QComboBox;
class QEventLoop;
class QFile;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSplitter;
class QThread;
class QTimer;
class QTreeWidgetItem;
class Session;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void search();
    void connectHost();
    void disconnectHost();
    void applySettings();
    void toggleFilePanel();
    void onAuthenticated();
    void onAuthFailed(const QString& reason);
    void onDropped();
    void onReconnecting(const QString& reason);
    void onSessionEnded();
    void onFrame(const QImage& image, int originWidth, int originHeight);
    void onTiles(int imageWidth, int imageHeight, int originWidth, int originHeight, const FrameTileList& tiles);
    void onClipboard(const QString& text);
    void onFileList(const QString& root, const QList<proto::FileEntry>& files);
    void onDownloadStarted(quint32 id, const QString& name, quint64 size);
    void onDownloadChunk(quint32 id, const QByteArray& data);
    void onTransferFinished(quint32 id, quint8 direction);
    void onTransferFailed(quint32 id, quint8 direction, const QString& message);
    void onMouse(quint16 x, quint16 y, quint8 button, quint8 action, qint16 wheel);
    void onCaptureRequested();
    void onKeyDown(quint32 virtualKey, Qt::KeyboardModifiers modifiers, bool repeat);
    void onKeyUp(quint32 virtualKey, bool repeat);
    void refreshFiles();
    void openTypedDirectory();
    void goUp();
    void upload();
    void downloadSelected();
    void deleteSelected();
    void cancelTransfer();
    void onTreeActivated(QTreeWidgetItem* item, int column);
    void onTreeCurrentChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
    void onFilesDropped(const QStringList& paths, QTreeWidgetItem* item);

private:
    template <class F>
    void postSession(F fn) {
        if (!m_session) return;
        QMetaObject::invokeMethod(m_session, std::move(fn), Qt::QueuedConnection);
    }
    template <class F>
    void callSession(F fn) {
        if (!m_session) return;
        QMetaObject::invokeMethod(m_session, std::move(fn), Qt::BlockingQueuedConnection);
    }

    proto::StreamSettings readSettings() const;
    void updateBandwidth();
    void notePicture(int originWidth, int originHeight);
    void updateUi();
    void releaseKeyboard();
    void sendKey(quint32 virtualKey, bool down);
    void readLocalClipboard();
    void failPending();
    void closeDownload(bool deletePartial);
    bool downloadTo(const QString& remotePath, const QString& localPath);
    void downloadNode(QTreeWidgetItem* item);
    void downloadDirectory(QTreeWidgetItem* item);
    void uploadLocals(const QStringList& paths, const QString& remoteDirectory);
    void openRemote(const QString& path);
    QString selectedDirectory() const;
    QString directoryOf(QTreeWidgetItem* item) const;
    QString remoteFull(const QString& relative) const;
    void sortItem(QTreeWidgetItem* node);
    void shutdownThreads();

    Discovery* m_discovery = nullptr;
    Session* m_session = nullptr;
    QThread* m_thread = nullptr;

    QComboBox* m_host = nullptr;
    QLineEdit* m_port = nullptr;
    QLineEdit* m_password = nullptr;
    QPushButton* m_search = nullptr;
    QPushButton* m_connect = nullptr;
    QPushButton* m_disconnect = nullptr;
    QCheckBox* m_reconnect = nullptr;
    QLabel* m_latency = nullptr;
    QLabel* m_fps = nullptr;
    QLabel* m_status = nullptr;
    QComboBox* m_codec = nullptr;
    QSlider* m_quality = nullptr;
    QLabel* m_qualityText = nullptr;
    QComboBox* m_width = nullptr;
    QComboBox* m_fpsLimit = nullptr;
    QLabel* m_bandwidth = nullptr;
    QPushButton* m_fullScreen = nullptr;
    QWidget* m_filePanel = nullptr;
    QSplitter* m_split = nullptr;
    int m_filePanelWidth = 300;
    QLineEdit* m_remotePath = nullptr;
    QLabel* m_place = nullptr;
    QPushButton* m_up = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_open = nullptr;
    QPushButton* m_upload = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_delete = nullptr;
    FileTree* m_tree = nullptr;
    class QProgressBar* m_bar = nullptr;
    QLabel* m_transfer = nullptr;
    ScreenView* m_screen = nullptr;
    QTimer* m_settingsTimer = nullptr;

    bool m_shutDown = false;
    bool m_uiReady = false;
    bool m_shown = false;
    bool m_closing = false;
    bool m_searching = false;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_keyboard = false;
    bool m_ignoreClipboard = false;
    bool m_transferBusy = false;
    std::atomic<bool> m_cancelTransfer{false};
    int m_originWidth = 0;
    int m_originHeight = 0;
    int m_frames = 0;
    qint64 m_fpsWindow = 0;
    QString m_lastRemoteClipboard = QString(QChar(1));
    QString m_lastSentClipboard = QString(QChar(1));
    QString m_remoteRoot;
    QString m_downloadPath;
    QFile* m_downloadFile = nullptr;
    QEventLoop* m_uploadLoop = nullptr;
    QEventLoop* m_downloadLoop = nullptr;
    int m_uploadResult = -1;
    int m_downloadResult = -1;
    quint32 m_uploadId = 0;
    quint32 m_downloadId = 0;
    quint64 m_downloadGot = 0;
    quint64 m_downloadSize = 0;
    quint32 m_nextUploadId = 1;
    bool m_downloadOpened = false;
    QSet<int> m_keysDown;
};
