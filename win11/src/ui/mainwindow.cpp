#include "ui/mainwindow.h"

#include "session/discovery.h"
#include "session/session.h"
#include "ui/filetree.h"
#include "ui/screenview.h"
#include "ui/uploadpicker.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QSizePolicy>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSlider>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <QtConcurrent>

#include <algorithm>

namespace {

enum TreeRole { RoleRel = Qt::UserRole, RoleDir, RoleName, RoleSize };

QString zh(const char* text) { return QString::fromUtf8(text); }

QComboBox* addChoices(QWidget* parent, std::initializer_list<std::pair<const char*, int>> items) {
    auto* box = new QComboBox(parent);
    for (const auto& item : items) box->addItem(zh(item.first), item.second);
    return box;
}

QString parentPath(const QString& relative) {
    const int slash = relative.lastIndexOf('/');
    return slash < 0 ? QString() : relative.left(slash);
}

QString joinRemote(const QString& directory, const QString& name) {
    QString normalized = name;
    normalized.replace('\\', '/');
    while (normalized.startsWith('/')) normalized.remove(0, 1);
    if (directory.isEmpty()) return normalized;
    QString base = directory;
    while (base.endsWith('/')) base.chop(1);
    return base + "/" + normalized;
}

bool unsafeRelative(const QString& relative) {
    const QStringList parts = relative.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return true;
    for (const QString& part : parts) {
        if (part == "." || part == "..") return true;
    }
    return false;
}

void collectFiles(QTreeWidgetItem* node, QList<QTreeWidgetItem*>& files) {
    const bool dir = node->data(0, RoleDir).toBool();
    const QString rel = node->data(0, RoleRel).toString();
    if (!dir && !rel.isEmpty()) files.append(node);
    for (int i = 0; i < node->childCount(); ++i) collectFiles(node->child(i), files);
}

QString localRelative(QTreeWidgetItem* folder, QTreeWidgetItem* file) {
    const QString folderRel = folder->data(0, RoleRel).toString();
    const QString fileRel = file->data(0, RoleRel).toString();
    const QString folderName = folder->data(0, RoleName).toString();
    QString tail = file->data(0, RoleName).toString();
    if (folderRel.isEmpty()) tail = fileRel;
    else if (fileRel.startsWith(folderRel + "/")) tail = fileRel.mid(folderRel.size() + 1);
    return folderName + "/" + tail;
}

void collectUploads(const QString& path, const QString& remoteDirectory, QStringList& directories,
                    QList<QPair<QString, QString>>& jobs) {
    QFileInfo info(path);
    if (info.isSymLink()) return;
    if (info.isDir()) {
        const QString folder = QDir(path).dirName();
        if (folder.isEmpty() || folder == "." || folder == "..") return;
        directories.append(joinRemote(remoteDirectory, folder));
        QDirIterator dirs(path, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (dirs.hasNext()) {
            const QString dirPath = dirs.next();
            if (dirs.fileInfo().isSymLink()) continue;
            const QString rel = QDir(path).relativeFilePath(dirPath).replace('\\', '/');
            if (unsafeRelative(rel)) continue;
            directories.append(joinRemote(remoteDirectory, folder + "/" + rel));
        }
        QDirIterator files(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString filePath = files.next();
            if (files.fileInfo().isSymLink()) continue;
            const QString rel = QDir(path).relativeFilePath(filePath).replace('\\', '/');
            if (unsafeRelative(rel)) continue;
            jobs.append({filePath, joinRemote(remoteDirectory, folder + "/" + rel)});
        }
        return;
    }
    if (!info.isFile()) return;
    const QString name = info.fileName();
    if (name.isEmpty() || name == "." || name == "..") return;
    jobs.append({path, joinRemote(remoteDirectory, name)});
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    qRegisterMetaType<proto::FileEntry>();
    qRegisterMetaType<ProtoFileList>();
    qRegisterMetaType<FrameTile>();
    qRegisterMetaType<FrameTileList>();

    setWindowTitle(zh("局域网远程桌面"));
    resize(1180, 780);
    setMinimumSize(880, 560);
    setStyleSheet(zh(
        "QWidget { font-family: 'Microsoft YaHei UI', 'Segoe UI'; font-size: 13px; color: #1a1a1a; }"
        "QMainWindow, #chrome { background: #ffffff; }"
        "#toolbar { background: #ffffff; border-bottom: 1px solid #e6e6e6; }"
        "#bottomBar { background: #ffffff; border-top: 1px solid #e6e6e6; }"
        "#picturePopup { background: #ffffff; border: 1px solid #d8d8d8; border-radius: 6px; }"
        "#picturePopup QLabel#bandwidthHint { color: #5c6570; }"
        "QPushButton#pictureButton:checked { background: #e7f0ff; border-color: #9db7ef; }"
        "#filePanel { background: #ffffff; }"
        "QSplitter#bodySplit::handle { background: #e6e6e6; }"
        "QSplitter#bodySplit::handle:hover { background: #2f6fed; }"
        "QLineEdit, QComboBox { background: #ffffff; border: 1px solid #d0d0d0; border-radius: 4px; padding: 4px 6px; min-height: 18px; }"
        "QComboBox::drop-down { border: none; width: 18px; }"
        "QPushButton { background: #ffffff; border: 1px solid #d0d0d0; border-radius: 4px; padding: 5px 12px; }"
        "QPushButton:hover { background: #f4f7fb; }"
        "QPushButton:disabled { color: #9a9a9a; background: #fafafa; }"
        "QPushButton#connectButton { background: #2f6fed; color: white; border: none; }"
        "QPushButton#connectButton:hover { background: #245ed4; }"
        "QPushButton#connectButton:disabled { background: #b7c9f3; color: white; }"
        "QTreeWidget { border: none; background: #ffffff; }"
        "QTreeWidget::item { height: 22px; }"
        "QTreeWidget::item:selected { background: #e7f0ff; color: #1a1a1a; }"
        "QProgressBar { border: none; background: #eceff3; border-radius: 3px; }"
        "QProgressBar::chunk { background: #2f6fed; border-radius: 3px; }"
        "QCheckBox { spacing: 6px; }"));

    auto* chrome = new QWidget(this);
    chrome->setObjectName("chrome");
    auto* root = new QVBoxLayout(chrome);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* top = new QWidget(chrome);
    top->setObjectName("toolbar");
    auto* topRow = new QHBoxLayout(top);
    topRow->setContentsMargins(10, 8, 10, 8);
    m_pictureButton = new QPushButton(zh("画面 ▾"), top);
    m_pictureButton->setObjectName("pictureButton");
    m_pictureButton->setCheckable(true);
    m_pictureButton->setToolTip(zh("编码、画质、宽度、帧率和全屏"));
    topRow->addWidget(m_pictureButton);
    topRow->addStretch();
    root->addWidget(top);

    m_picturePanel = new QFrame(this, Qt::Tool | Qt::FramelessWindowHint);
    m_picturePanel->setObjectName("picturePopup");
    m_picturePanel->setFrameShape(QFrame::NoFrame);
    m_picturePanel->setAttribute(Qt::WA_StyledBackground, true);
    m_picturePanel->setStyleSheet(styleSheet());
    m_picturePanel->setFixedWidth(320);
    m_picturePanel->hide();
    auto* picture = new QGridLayout(m_picturePanel);
    picture->setContentsMargins(12, 10, 12, 10);
    picture->setHorizontalSpacing(10);
    picture->setVerticalSpacing(8);
    picture->setColumnStretch(1, 1);
    auto addPictureRow = [&](int row, const char* name, QWidget* field) {
        picture->addWidget(new QLabel(zh(name), m_picturePanel), row, 0);
        picture->addWidget(field, row, 1);
    };
    m_codec = addChoices(m_picturePanel, {{"压缩", 0}, {"不压缩", 1}});
    addPictureRow(0, "编码", m_codec);
    m_quality = new QSlider(Qt::Horizontal, m_picturePanel);
    m_quality->setRange(1, 100);
    m_quality->setValue(30);
    m_qualityText = new QLabel(zh("30"), m_picturePanel);
    m_qualityText->setFixedWidth(28);
    auto* qualityWrap = new QWidget(m_picturePanel);
    auto* qualityRow = new QHBoxLayout(qualityWrap);
    qualityRow->setContentsMargins(0, 0, 0, 0);
    qualityRow->setSpacing(6);
    qualityRow->addWidget(m_quality, 1);
    qualityRow->addWidget(m_qualityText);
    addPictureRow(1, "JPEG 质量", qualityWrap);
    m_width = addChoices(m_picturePanel, {{"640", 640}, {"960", 960}, {"1280", 1280}, {"1600", 1600}, {"与桌面同宽", 0}});
    m_width->setCurrentIndex(2);
    addPictureRow(2, "最大宽度", m_width);
    m_fpsLimit = addChoices(m_picturePanel, {{"10", 10}, {"15", 15}, {"20", 20}, {"30", 30}});
    m_fpsLimit->setCurrentIndex(3);
    addPictureRow(3, "帧率上限", m_fpsLimit);
    m_bandwidth = new QLabel(m_picturePanel);
    m_bandwidth->setObjectName("bandwidthHint");
    m_bandwidth->setWordWrap(true);
    picture->addWidget(m_bandwidth, 4, 0, 1, 2);
    m_fullScreen = new QPushButton(zh("全屏"), m_picturePanel);
    m_fullScreen->setToolTip(zh("收起左侧目录和下边栏"));
    picture->addWidget(m_fullScreen, 5, 0, 1, 2, Qt::AlignLeft);

    m_filePanel = new QWidget(chrome);
    m_filePanel->setObjectName("filePanel");
    m_filePanel->setMinimumWidth(180);
    auto* files = new QVBoxLayout(m_filePanel);
    files->setContentsMargins(8, 8, 8, 8);
    auto* title = new QLabel(zh("远程目录"), m_filePanel);
    title->setStyleSheet("font-weight: 600;");
    files->addWidget(title);
    auto* pathRow = new QHBoxLayout;
    m_remotePath = new QLineEdit(zh("未连接"), m_filePanel);
    m_remotePath->setToolTip(zh("绝对路径，回车跳转。例如 /home/ye 或 ~/Documents"));
    m_open = new QPushButton(zh("打开"), m_filePanel);
    pathRow->addWidget(m_remotePath);
    pathRow->addWidget(m_open);
    files->addLayout(pathRow);
    m_place = new QLabel(m_filePanel);
    m_place->setWordWrap(true);
    m_place->setStyleSheet("color: #5c6570;");
    files->addWidget(m_place);
    auto* actions = new QGridLayout;
    actions->setHorizontalSpacing(6);
    actions->setVerticalSpacing(6);
    m_up = new QPushButton(zh("上级"), m_filePanel);
    m_refresh = new QPushButton(zh("刷新"), m_filePanel);
    m_upload = new QPushButton(zh("上传"), m_filePanel);
    m_download = new QPushButton(zh("下载"), m_filePanel);
    m_delete = new QPushButton(zh("删除"), m_filePanel);
    auto* cancel = new QPushButton(zh("取消"), m_filePanel);
    QPushButton* actionButtons[] = {m_up, m_refresh, m_upload, m_download, m_delete, cancel};
    for (int i = 0; i < 6; ++i) {
        actionButtons[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        actionButtons[i]->setMinimumWidth(0);
        actionButtons[i]->setStyleSheet("padding: 5px 4px;");
        actions->addWidget(actionButtons[i], i / 3, i % 3);
        actions->setColumnStretch(i % 3, 1);
    }
    files->addLayout(actions);
    auto* dropHint = new QLabel(zh("可拖入文件或文件夹。上传时也能直接选文件夹。"), m_filePanel);
    dropHint->setWordWrap(true);
    dropHint->setStyleSheet("color: #5c6570;");
    files->addWidget(dropHint);
    m_tree = new FileTree(m_filePanel);
    files->addWidget(m_tree, 1);
    m_transfer = new QLabel(m_filePanel);
    m_transfer->setWordWrap(true);
    files->addWidget(m_transfer);
    m_bar = new QProgressBar(m_filePanel);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    files->addWidget(m_bar);
    m_screen = new ScreenView(chrome);
    m_screen->setMinimumWidth(240);
    m_split = new QSplitter(Qt::Horizontal, chrome);
    m_split->setObjectName("bodySplit");
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(6);
    m_split->addWidget(m_filePanel);
    m_split->addWidget(m_screen);
    m_split->setStretchFactor(0, 0);
    m_split->setStretchFactor(1, 1);
    m_split->setSizes({m_filePanelWidth, 860});
    root->addWidget(m_split, 1);

    m_bottomBar = new QWidget(chrome);
    m_bottomBar->setObjectName("bottomBar");
    auto* bottom = m_bottomBar;
    auto* bottomRow = new QHBoxLayout(bottom);
    bottomRow->setContentsMargins(10, 8, 10, 8);
    bottomRow->addWidget(new QLabel(zh("地址"), bottom));
    m_host = new QComboBox(bottom);
    m_host->setEditable(true);
    m_host->setMinimumWidth(180);
    m_host->setMaximumWidth(260);
    m_host->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_host->setToolTip(zh("找到一台会自动填入。多台时点开此框选择"));
    bottomRow->addWidget(m_host);
    m_search = new QPushButton(zh("搜索"), bottom);
    m_search->setToolTip(zh("在局域网里查找 NetDebug"));
    bottomRow->addWidget(m_search);
    bottomRow->addWidget(new QLabel(zh("端口"), bottom));
    m_port = new QLineEdit(zh("17611"), bottom);
    m_port->setFixedWidth(72);
    bottomRow->addWidget(m_port);
    bottomRow->addWidget(new QLabel(zh("密码"), bottom));
    m_password = new QLineEdit(bottom);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setFixedWidth(120);
    bottomRow->addWidget(m_password);
    m_connect = new QPushButton(zh("连接"), bottom);
    m_connect->setObjectName("connectButton");
    bottomRow->addWidget(m_connect);
    m_disconnect = new QPushButton(zh("断开"), bottom);
    bottomRow->addWidget(m_disconnect);
    m_reconnect = new QCheckBox(zh("断线重连"), bottom);
    m_reconnect->setChecked(true);
    bottomRow->addWidget(m_reconnect);
    m_statusGroup = new QWidget(bottom);
    auto* statusRow = new QHBoxLayout(m_statusGroup);
    statusRow->setContentsMargins(16, 0, 0, 0);
    statusRow->setSpacing(12);
    m_latency = new QLabel(zh("延迟 --"), m_statusGroup);
    m_fps = new QLabel(zh("收帧 --"), m_statusGroup);
    m_status = new QLabel(zh("未连接"), m_statusGroup);
    statusRow->addWidget(m_latency);
    statusRow->addWidget(m_fps);
    statusRow->addWidget(m_status);
    bottomRow->addWidget(m_statusGroup);
    bottomRow->addStretch(1);
    root->addWidget(bottom);
    setCentralWidget(chrome);

    m_settingsTimer = new QTimer(this);
    m_settingsTimer->setSingleShot(true);
    m_settingsTimer->setInterval(120);

    m_session = new Session;
    m_thread = new QThread(this);
    m_session->moveToThread(m_thread);
    connect(m_session, &Session::authenticated, this, &MainWindow::onAuthenticated);
    connect(m_session, &Session::authFailed, this, &MainWindow::onAuthFailed);
    connect(m_session, &Session::dropped, this, &MainWindow::onDropped);
    connect(m_session, &Session::reconnecting, this, &MainWindow::onReconnecting);
    connect(m_session, &Session::sessionEnded, this, &MainWindow::onSessionEnded);
    connect(m_session, &Session::frameReady, this, &MainWindow::onFrame);
    connect(m_session, &Session::tilesReady, this, &MainWindow::onTiles);
    connect(m_session, &Session::clipboardReceived, this, &MainWindow::onClipboard);
    connect(m_session, &Session::fileListReceived, this, &MainWindow::onFileList);
    connect(m_session, &Session::downloadStarted, this, &MainWindow::onDownloadStarted);
    connect(m_session, &Session::downloadChunk, this, &MainWindow::onDownloadChunk);
    connect(m_session, &Session::transferFinished, this, &MainWindow::onTransferFinished);
    connect(m_session, &Session::transferFailed, this, &MainWindow::onTransferFailed);
    connect(m_session, &Session::latencyUpdated, this, [this](double ms) {
        m_latency->setText(zh("延迟 %1 ms").arg(QString::number(ms, 'f', 0)));
    });
    m_thread->start();
    postSession([session = m_session] { session->setAutoReconnect(true); });

    m_discovery = new Discovery(this);
    connect(m_discovery, &Discovery::progress, this, [this](const QString& text) {
        if (m_searching && !m_connected && !m_connecting) m_status->setText(text);
    });
    connect(m_discovery, &Discovery::finished, this, [this](const QStringList& found) {
        if (!m_searching) return;
        m_searching = false;
        const QString typed = m_host->currentText().trimmed();
        m_host->clear();
        m_host->addItems(found);
        if (found.size() == 1) m_host->setCurrentText(found.first());
        else if (found.size() > 1) {
            m_host->setCurrentText(found.contains(typed) ? typed : QString());
            m_host->showPopup();
        }
        if (!m_connected && !m_connecting)
            m_status->setText(found.isEmpty() ? zh("没有找到设备") : zh("找到 %1 台设备").arg(found.size()));
        updateUi();
    });

    connect(m_search, &QPushButton::clicked, this, &MainWindow::search);
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::connectHost);
    connect(m_disconnect, &QPushButton::clicked, this, &MainWindow::disconnectHost);
    connect(m_reconnect, &QCheckBox::toggled, this, [this](bool on) {
        postSession([session = m_session, on] { session->setAutoReconnect(on); });
    });
    connect(m_codec, &QComboBox::currentIndexChanged, this, &MainWindow::applySettings);
    connect(m_width, &QComboBox::currentIndexChanged, this, &MainWindow::applySettings);
    connect(m_fpsLimit, &QComboBox::currentIndexChanged, this, &MainWindow::applySettings);
    connect(m_quality, &QSlider::valueChanged, this, &MainWindow::applySettings);
    connect(m_settingsTimer, &QTimer::timeout, this, [this] {
        const proto::StreamSettings settings = readSettings();
        postSession([session = m_session, settings] { session->updateSettings(settings); });
    });
    connect(m_pictureButton, &QPushButton::toggled, this, [this](bool on) {
        m_pictureButton->setText(on ? zh("画面 ▴") : zh("画面 ▾"));
        if (!on) {
            m_picturePanel->hide();
            return;
        }
        placePictureMenu();
        m_picturePanel->show();
        m_picturePanel->raise();
    });
    connect(m_open, &QPushButton::clicked, this, &MainWindow::openTypedDirectory);
    connect(m_remotePath, &QLineEdit::returnPressed, this, &MainWindow::openTypedDirectory);
    connect(m_remotePath, &QLineEdit::textEdited, this, [this] { if (m_keyboard) releaseKeyboard(); });
    connect(m_up, &QPushButton::clicked, this, &MainWindow::goUp);
    connect(m_refresh, &QPushButton::clicked, this, &MainWindow::refreshFiles);
    connect(m_upload, &QPushButton::clicked, this, &MainWindow::upload);
    connect(m_download, &QPushButton::clicked, this, &MainWindow::downloadSelected);
    connect(m_delete, &QPushButton::clicked, this, &MainWindow::deleteSelected);
    connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelTransfer);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onTreeActivated);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &MainWindow::onTreeCurrentChanged);
    connect(m_tree, &FileTree::filesDropped, this, &MainWindow::onFilesDropped);
    connect(m_screen, &ScreenView::mouseAction, this, &MainWindow::onMouse);
    connect(m_screen, &ScreenView::captureRequested, this, &MainWindow::onCaptureRequested);
    connect(m_screen, &ScreenView::keyDown, this, &MainWindow::onKeyDown);
    connect(m_screen, &ScreenView::keyUp, this, &MainWindow::onKeyUp);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::readLocalClipboard);
    m_tree->installEventFilter(this);

    QCoreApplication::instance()->installEventFilter(this);

    m_uiReady = true;
    updateBandwidth();
    updateUi();
}

MainWindow::~MainWindow() {
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
    shutdownThreads();
    closeDownload(false);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (m_shown) return;
    m_shown = true;
    QTimer::singleShot(0, this, &MainWindow::search);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    m_closing = true;
    shutdownThreads();
    closeDownload(true);
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_picturePanel && m_picturePanel->isVisible()) placePictureMenu();
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    if (m_picturePanel && m_picturePanel->isVisible()) placePictureMenu();
}

void MainWindow::placePictureMenu() {
    if (!m_picturePanel || !m_pictureButton) return;
    m_picturePanel->adjustSize();
    QPoint pos = m_pictureButton->mapToGlobal(QPoint(0, m_pictureButton->height() + 4));
    if (QScreen* screen = m_pictureButton->screen()) {
        const QRect area = screen->availableGeometry();
        const int maxX = area.right() - m_picturePanel->width() + 1;
        const int maxY = area.bottom() - m_picturePanel->height() + 1;
        pos.setX(qBound(area.left(), pos.x(), qMax(area.left(), maxX)));
        if (pos.y() > maxY) pos.setY(qMax(area.top(), maxY));
    }
    m_picturePanel->move(pos);
    m_picturePanel->raise();
}

void MainWindow::shutdownThreads() {
    if (m_shutDown) return;
    m_shutDown = true;
    if (m_discovery) m_discovery->cancel();
    if (m_thread && m_thread->isRunning() && m_session) {
        QThread* ui = QThread::currentThread();
        callSession([session = m_session, ui] {
            session->stop();
            session->moveToThread(ui);
        });
        m_thread->quit();
        m_thread->wait(3000);
    }
    if (m_session) {
        QCoreApplication::removePostedEvents(m_session);
        delete m_session;
        m_session = nullptr;
    }
}

void MainWindow::search() {
    if (m_searching || m_connected || m_connecting) return;
    bool portOk = false;
    const int port = m_port->text().trimmed().toInt(&portOk);
    if (!portOk || port < 1 || port > 65535) {
        m_status->setText(zh("端口无效"));
        return;
    }
    m_searching = true;
    m_status->setText(zh("正在搜索局域网"));
    updateUi();
    m_discovery->start(port);
}

void MainWindow::connectHost() {
    if (m_connecting || m_connected) return;
    m_discovery->cancel();
    m_searching = false;
    const QString host = m_host->currentText().trimmed();
    if (host.isEmpty()) {
        m_status->setText(zh("请填写地址"));
        updateUi();
        return;
    }
    bool portOk = false;
    const int port = m_port->text().trimmed().toInt(&portOk);
    if (!portOk || port < 1 || port > 65535) {
        m_status->setText(zh("端口无效"));
        return;
    }
    const QString password = m_password->text();
    const proto::StreamSettings settings = readSettings();
    const bool reconnect = m_reconnect->isChecked();
    m_connecting = true;
    m_cancelTransfer = false;
    m_status->setText(zh("正在连接"));
    updateUi();
    postSession([session = m_session, host, port, password, settings, reconnect] {
        session->setAutoReconnect(reconnect);
        session->start(host, port, password, settings);
    });
}

void MainWindow::disconnectHost() {
    postSession([session = m_session] { session->stop(); });
    releaseKeyboard();
}

void MainWindow::applySettings() {
    if (!m_uiReady) return;
    m_qualityText->setText(QString::number(m_quality->value()));
    m_quality->setEnabled(m_codec->currentData().toInt() == 0);
    updateBandwidth();
    m_settingsTimer->start();
}

void MainWindow::placeStatus(bool onTop) {
    if (!m_statusGroup || !m_pictureButton || !m_bottomBar) return;
    auto* topRow = qobject_cast<QHBoxLayout*>(m_pictureButton->parentWidget()->layout());
    auto* bottomRow = qobject_cast<QHBoxLayout*>(m_bottomBar->layout());
    if (!topRow || !bottomRow) return;
    topRow->removeWidget(m_statusGroup);
    bottomRow->removeWidget(m_statusGroup);
    if (onTop) {
        topRow->addWidget(m_statusGroup);
    } else if (bottomRow->count() > 0) {
        bottomRow->insertWidget(bottomRow->count() - 1, m_statusGroup);
    } else {
        bottomRow->addWidget(m_statusGroup);
    }
    m_statusGroup->show();
}

void MainWindow::toggleFilePanel() {
    m_immersive = !m_immersive;
    if (m_immersive) {
        m_filePanelWidth = qMax(m_filePanel->width(), m_filePanel->minimumWidth());
        m_filePanel->setMinimumWidth(0);
        m_split->setChildrenCollapsible(true);
        placeStatus(true);
        m_filePanel->hide();
        m_bottomBar->hide();
        m_split->setSizes({0, qMax(1, m_split->width())});
    } else {
        m_filePanel->setMinimumWidth(180);
        m_filePanel->show();
        m_bottomBar->show();
        placeStatus(false);
        m_split->setChildrenCollapsible(false);
        const int total = qMax(m_split->width(), m_filePanelWidth + m_screen->minimumWidth());
        m_split->setSizes({m_filePanelWidth, qMax(1, total - m_filePanelWidth)});
    }
    m_fullScreen->setText(m_immersive ? zh("退出全屏") : zh("全屏"));
    m_fullScreen->setToolTip(m_immersive ? zh("展开左侧目录和下边栏") : zh("收起左侧目录和下边栏"));
}

proto::StreamSettings MainWindow::readSettings() const {
    proto::StreamSettings settings;
    settings.codec = quint8(std::clamp(m_codec->currentData().toInt(), 0, 255));
    settings.jpegQuality = quint8(std::clamp(m_quality->value(), 1, 100));
    settings.maxWidth = quint16(std::clamp(m_width->currentData().toInt(), 0, 65535));
    settings.maxFps = quint8(std::clamp(m_fpsLimit->currentData().toInt(), 1, 60));
    return settings;
}

void MainWindow::updateBandwidth() {
    const int codec = m_codec->currentData().toInt();
    const int quality = std::clamp(m_quality->value(), 1, 100);
    const int maxWidth = m_width->currentData().toInt();
    const int fps = std::clamp(m_fpsLimit->currentData().toInt(), 1, 60);
    const int originW = m_originWidth > 0 ? m_originWidth : 1920;
    const int originH = m_originHeight > 0 ? m_originHeight : 1080;
    int width = maxWidth == 0 ? originW : std::min(maxWidth, originW);
    int height = std::max(1, originH * width / std::max(1, originW));
    if (codec == 0) {
        width &= ~1;
        height &= ~1;
    }
    if (width < 2) width = 2;
    if (height < 2) height = 2;

    QString text;
    bool warn = false;
    if (codec == 0) {
        const double mid = width * double(height) * 0.25 * (quality / 100.0) * fps * 8 / 1000000.0;
        const double low = mid * 0.5;
        const double high = mid * 3;
        warn = high > 80;
        text = zh("预计 %1–%2 Mbps").arg(QString::number(low, 'f', 1), QString::number(high, 'f', 1));
    } else {
        const double mbps = width * double(height) * 3 * fps * 8 / 1000000.0;
        warn = mbps > 80;
        text = zh("预计 %1 Mbps").arg(QString::number(mbps, 'f', 1));
    }
    if (m_originWidth == 0) text += zh("（按 1920×1080 估算）");
    if (warn) text += zh("，可能超过百兆");
    m_bandwidth->setText(text);
    if (m_picturePanel && m_picturePanel->isVisible()) placePictureMenu();
}

void MainWindow::updateUi() {
    const bool busy = m_connecting || m_connected;
    m_connect->setEnabled(!busy);
    m_search->setEnabled(!busy && !m_searching);
    m_disconnect->setEnabled(busy);
    m_refresh->setEnabled(m_connected);
    m_open->setEnabled(m_connected);
    m_up->setEnabled(m_connected && m_remoteRoot.size() > 1);
    m_upload->setEnabled(m_connected);
    m_download->setEnabled(m_connected);
    m_delete->setEnabled(m_connected);
}

void MainWindow::onAuthenticated() {
    if (!m_session || m_closing) return;
    m_connected = true;
    m_status->setText(zh("已连接"));
    m_screen->setPlaceholder(zh("等待画面"));
    m_screen->setConnected(true);
    m_latency->setText(zh("延迟 --"));
    m_fps->setText(zh("收帧 --"));
    m_frames = 0;
    m_fpsWindow = QDateTime::currentMSecsSinceEpoch();
    updateUi();
    bool listed = false;
    callSession([session = m_session, &listed] { listed = session->requestFileList({}); });
    if (!listed) m_transfer->setText(zh("切换目录失败"));
}

void MainWindow::onAuthFailed(const QString& reason) { m_status->setText(reason); }

void MainWindow::onDropped() {
    m_connected = false;
    m_keyboard = false;
    m_keysDown.clear();
    m_screen->setCapturing(false);
    m_screen->setConnected(false);
    m_screen->setPlaceholder(zh("未连接"));
    failPending();
    updateUi();
}

void MainWindow::onReconnecting(const QString& reason) { m_status->setText(zh("重连中：") + reason); }

void MainWindow::onSessionEnded() {
    m_connecting = false;
    m_connected = false;
    m_keyboard = false;
    m_screen->setCapturing(false);
    m_screen->setConnected(false);
    failPending();
    const QString status = m_status->text();
    if (status != zh("密码错误") && status != zh("版本不符")) m_status->setText(zh("未连接"));
    updateUi();
}

void MainWindow::notePicture(int originWidth, int originHeight) {
    if (originWidth != m_originWidth || originHeight != m_originHeight) {
        m_originWidth = originWidth;
        m_originHeight = originHeight;
        updateBandwidth();
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ++m_frames;
    if (now - m_fpsWindow >= 1000) {
        m_fps->setText(zh("收帧 %1 fps").arg(m_frames));
        m_frames = 0;
        m_fpsWindow = now;
    }
}

void MainWindow::onFrame(const QImage& image, int originWidth, int originHeight) {
    m_screen->setFrame(image, originWidth, originHeight);
    notePicture(originWidth, originHeight);
}

void MainWindow::onTiles(int imageWidth, int imageHeight, int originWidth, int originHeight, const FrameTileList& tiles) {
    QVector<ScreenTile> view;
    view.reserve(tiles.size());
    for (const FrameTile& tile : tiles) view.append({tile.x, tile.y, tile.image});
    if (!m_screen->applyTiles(imageWidth, imageHeight, originWidth, originHeight, view)) return;
    notePicture(originWidth, originHeight);
}

void MainWindow::onClipboard(const QString& text) {
    m_lastRemoteClipboard = text;
    m_ignoreClipboard = true;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (text.isEmpty()) clipboard->clear();
    else clipboard->setText(text);
    m_ignoreClipboard = false;
}

void MainWindow::readLocalClipboard() {
    if (m_ignoreClipboard || !m_connected || m_closing) return;
    const QString text = QGuiApplication::clipboard()->text();
    if (text == m_lastRemoteClipboard || text == m_lastSentClipboard) return;
    m_lastSentClipboard = text;
    postSession([session = m_session, text] { session->sendClipboard(text); });
}

void MainWindow::onMouse(quint16 x, quint16 y, quint8 button, quint8 action, qint16 wheel) {
    if (!m_connected) return;
    postSession([session = m_session, x, y, button, action, wheel] {
        session->sendMouse(x, y, button, action, wheel);
    });
}

void MainWindow::onCaptureRequested() {
    m_keyboard = true;
    m_screen->setCapturing(true);
}

void MainWindow::onKeyDown(quint32 virtualKey, Qt::KeyboardModifiers modifiers, bool) {
    if (!m_keyboard || !m_connected || virtualKey == 0) return;
    if ((modifiers & Qt::ControlModifier) && (modifiers & Qt::AltModifier)) {
        releaseKeyboard();
        return;
    }
    m_keysDown.insert(int(virtualKey));
    sendKey(virtualKey, true);
}

void MainWindow::onKeyUp(quint32 virtualKey, bool repeat) {
    if (repeat || virtualKey == 0 || !m_keysDown.remove(int(virtualKey))) return;
    sendKey(virtualKey, false);
}

void MainWindow::releaseKeyboard() {
    const QList<int> keys = m_keysDown.values();
    m_keysDown.clear();
    for (int key : keys) sendKey(quint32(key), false);
    m_keyboard = false;
    m_screen->setCapturing(false);
}

void MainWindow::sendKey(quint32 virtualKey, bool down) {
    postSession([session = m_session, virtualKey, down] { session->sendKey(quint16(virtualKey), down); });
}

void MainWindow::refreshFiles() {
    bool ok = false;
    callSession([session = m_session, &ok] { ok = session->requestFileList({}); });
    if (!ok) m_transfer->setText(zh("切换目录失败"));
}

void MainWindow::openTypedDirectory() {
    const QString path = m_remotePath->text().trimmed();
    if (path.isEmpty() || path == zh("未连接")) return;
    openRemote(path);
}

void MainWindow::goUp() {
    if (m_remoteRoot.size() <= 1) return;
    const int slash = m_remoteRoot.lastIndexOf('/');
    openRemote(slash <= 0 ? QString("/") : m_remoteRoot.left(slash));
}

void MainWindow::openRemote(const QString& path) {
    if (!m_connected) return;
    bool ok = false;
    callSession([session = m_session, path, &ok] { ok = session->requestFileList(path); });
    if (!ok) m_transfer->setText(zh("切换目录失败"));
}

void MainWindow::onTreeActivated(QTreeWidgetItem* item, int) {
    if (!item) return;
    if (m_keyboard) releaseKeyboard();
    if (item->data(0, RoleDir).toBool()) {
        const QString rel = item->data(0, RoleRel).toString();
        if (!rel.isEmpty()) openRemote(remoteFull(rel));
        return;
    }
    downloadNode(item);
}

void MainWindow::onTreeCurrentChanged(QTreeWidgetItem* current, QTreeWidgetItem*) {
    if (!current || m_remoteRoot.isEmpty()) {
        m_place->clear();
        return;
    }
    m_place->setText(remoteFull(current->data(0, RoleRel).toString()));
}

void MainWindow::onFileList(const QString& root, const QList<proto::FileEntry>& files) {
    QString normalized = root;
    normalized.replace('\\', '/');
    while (normalized.endsWith('/') && normalized.size() > 1) normalized.chop(1);
    if (normalized.isEmpty()) normalized = "/";
    m_remoteRoot = normalized;
    m_remotePath->setText(m_remoteRoot);

    QString rootName = QFileInfo(m_remoteRoot).fileName();
    if (rootName.isEmpty()) rootName = m_remoteRoot;
    m_tree->clear();
    auto* top = new QTreeWidgetItem(m_tree);
    top->setText(0, rootName);
    top->setData(0, RoleName, rootName);
    top->setData(0, RoleRel, QString());
    top->setData(0, RoleDir, true);
    top->setExpanded(true);

    QList<proto::FileEntry> ordered = files;
    std::sort(ordered.begin(), ordered.end(), [](const proto::FileEntry& a, const proto::FileEntry& b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    for (const proto::FileEntry& file : ordered) {
        QString normalizedName = file.name;
        normalizedName.replace('\\', '/');
        const QStringList parts = normalizedName.split('/', Qt::SkipEmptyParts);
        QTreeWidgetItem* node = top;
        QString rel;
        for (int i = 0; i < parts.size(); ++i) {
            rel = rel.isEmpty() ? parts.at(i) : rel + "/" + parts.at(i);
            const bool last = i == parts.size() - 1;
            QTreeWidgetItem* child = nullptr;
            for (int c = 0; c < node->childCount(); ++c) {
                if (node->child(c)->data(0, RoleName).toString() == parts.at(i)) {
                    child = node->child(c);
                    break;
                }
            }
            if (!child) {
                child = new QTreeWidgetItem(node);
                child->setData(0, RoleName, parts.at(i));
                child->setData(0, RoleRel, rel);
                child->setData(0, RoleDir, !last || file.isDirectory);
                child->setData(0, RoleSize, !last || file.isDirectory ? QVariant::fromValue(quint64(0)) : QVariant::fromValue(file.size));
                const QString size = (!last || file.isDirectory) ? QString() : zh("    ") + proto::formatSize(file.size);
                child->setText(0, parts.at(i) + size);
            } else if (last && !file.isDirectory) {
                child->setData(0, RoleDir, false);
                child->setData(0, RoleSize, QVariant::fromValue(file.size));
                child->setText(0, parts.at(i) + zh("    ") + proto::formatSize(file.size));
            }
            node = child;
        }
    }
    sortItem(top);
    if (QTreeWidgetItem* selected = m_tree->currentItem()) m_place->setText(remoteFull(selected->data(0, RoleRel).toString()));
    else m_place->setText(m_remoteRoot);
    updateUi();
}

void MainWindow::sortItem(QTreeWidgetItem* node) {
    QList<QTreeWidgetItem*> kids;
    while (node->childCount() > 0) kids.append(node->takeChild(0));
    std::sort(kids.begin(), kids.end(), [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
        const bool ad = a->data(0, RoleDir).toBool();
        const bool bd = b->data(0, RoleDir).toBool();
        if (ad != bd) return ad && !bd;
        return QString::compare(a->data(0, RoleName).toString(), b->data(0, RoleName).toString(), Qt::CaseInsensitive) < 0;
    });
    for (QTreeWidgetItem* child : kids) {
        sortItem(child);
        node->addChild(child);
    }
}

QString MainWindow::selectedDirectory() const { return directoryOf(m_tree->currentItem()); }

QString MainWindow::directoryOf(QTreeWidgetItem* item) const {
    if (!item) return {};
    const QString rel = item->data(0, RoleRel).toString();
    return item->data(0, RoleDir).toBool() ? rel : parentPath(rel);
}

QString MainWindow::remoteFull(const QString& relative) const {
    if (relative.isEmpty()) return m_remoteRoot;
    if (m_remoteRoot.isEmpty() || m_remoteRoot == "/") return "/" + relative.mid(relative.startsWith('/') ? 1 : 0);
    return m_remoteRoot + "/" + (relative.startsWith('/') ? relative.mid(1) : relative);
}

void MainWindow::upload() {
    if (!m_connected || m_transferBusy) return;
    const QStringList paths = pickUploads(this);
    if (paths.isEmpty()) return;
    uploadLocals(paths, selectedDirectory());
}

void MainWindow::onFilesDropped(const QStringList& paths, QTreeWidgetItem* item) {
    if (!m_connected || paths.isEmpty() || m_transferBusy) return;
    uploadLocals(paths, directoryOf(item ? item : m_tree->currentItem()));
}

void MainWindow::downloadSelected() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!item) return;
    if (item->data(0, RoleDir).toBool()) downloadDirectory(item);
    else downloadNode(item);
}

void MainWindow::downloadNode(QTreeWidgetItem* item) {
    if (!m_connected || !item || item->data(0, RoleDir).toBool()) return;
    const QString rel = item->data(0, RoleRel).toString();
    if (rel.isEmpty()) return;
    const QString local = QFileDialog::getSaveFileName(this, zh("保存到本机，远程位置 ") + remoteFull(rel),
                                                       item->data(0, RoleName).toString());
    if (local.isEmpty()) return;
    downloadTo(rel, local);
}

void MainWindow::downloadDirectory(QTreeWidgetItem* item) {
    if (!m_connected || !item) return;
    QList<QTreeWidgetItem*> files;
    collectFiles(item, files);
    if (files.isEmpty()) {
        m_transfer->setText(zh("这个目录里没有文件"));
        return;
    }
    const QString folder = QFileDialog::getExistingDirectory(this, zh("保存到本机的文件夹"));
    if (folder.isEmpty()) return;
    m_cancelTransfer = false;
    int saved = 0;
    for (QTreeWidgetItem* file : files) {
        if (m_cancelTransfer || !m_connected) break;
        const QString relative = localRelative(item, file);
        const QString local = QDir(folder).filePath(QString(relative).replace('/', QDir::separator()));
        QDir().mkpath(QFileInfo(local).path());
        if (!downloadTo(file->data(0, RoleRel).toString(), local)) break;
        ++saved;
    }
    if (m_cancelTransfer) m_transfer->setText(zh("已取消"));
    else if (saved == files.size()) m_transfer->setText(zh("已下载 %1 个文件到 %2").arg(saved).arg(folder));
}

bool MainWindow::downloadTo(const QString& remotePath, const QString& localPath) {
    if (!m_connected) return false;
    m_downloadResult = -1;
    m_downloadOpened = false;
    m_downloadPath = localPath;
    m_downloadId = 0;
    m_bar->setValue(0);
    m_transfer->setText(zh("正在下载 ") + remoteFull(remotePath));
    bool sent = false;
    callSession([session = m_session, remotePath, &sent] { sent = session->requestDownload(remotePath); });
    if (!sent) {
        m_transfer->setText(zh("下载请求发送失败"));
        return false;
    }
    QEventLoop loop;
    m_downloadLoop = &loop;
    if (m_downloadResult < 0) loop.exec();
    m_downloadLoop = nullptr;
    return m_downloadResult == 1 && !m_cancelTransfer;
}

void MainWindow::uploadLocals(const QStringList& paths, const QString& remoteDirectory) {
    if (!m_connected || m_transferBusy) return;
    QStringList directories;
    QList<QPair<QString, QString>> jobs;
    for (const QString& path : paths) collectUploads(path, remoteDirectory, directories, jobs);
    directories.removeDuplicates();
    if (jobs.isEmpty() && directories.isEmpty()) {
        m_transfer->setText(zh("没有可上传的文件"));
        return;
    }
    m_transferBusy = true;
    m_cancelTransfer = false;
    for (const QString& directory : directories) {
        if (m_cancelTransfer || !m_connected) {
            m_transferBusy = false;
            return;
        }
        bool ok = false;
        callSession([session = m_session, directory, &ok] { ok = session->requestMkdir(directory); });
        if (!ok) {
            m_transfer->setText(zh("创建文件夹失败"));
            m_transferBusy = false;
            return;
        }
    }
    if (jobs.isEmpty()) {
        m_transfer->setText(zh("已上传文件夹"));
        refreshFiles();
        m_transferBusy = false;
        return;
    }

    int uploaded = 0;
    for (const auto& job : jobs) {
        if (m_cancelTransfer || !m_connected) break;
        const quint64 size = quint64(QFileInfo(job.first).size());
        const quint32 id = m_nextUploadId++;
        m_uploadId = id;
        m_uploadResult = -1;
        m_bar->setValue(0);
        m_transfer->setText(zh("正在上传到 ") + remoteFull(job.second));
        bool started = false;
        callSession([session = m_session, id, remote = job.second, size, &started] {
            started = session->beginUpload(id, remote, size);
        });
        if (!started) {
            m_transfer->setText(zh("上传请求发送失败"));
            break;
        }

        std::atomic<bool> failed{false};
        QFuture<void> future = QtConcurrent::run([this, id, local = job.first, size, &failed] {
            QFile file(local);
            if (!file.open(QIODevice::ReadOnly)) {
                failed = true;
                return;
            }
            quint64 sent = 0;
            while (!m_cancelTransfer) {
                const QByteArray chunk = file.read(proto::kChunkSize);
                if (chunk.isEmpty()) break;
                bool ok = false;
                callSession([session = m_session, id, chunk, &ok] { ok = session->sendChunk(id, chunk); });
                if (!ok) {
                    failed = true;
                    return;
                }
                sent += quint64(chunk.size());
                const int percent = size == 0 ? 100 : int(sent * 100 / size);
                QMetaObject::invokeMethod(this, [this, percent] { m_bar->setValue(percent); }, Qt::QueuedConnection);
            }
            if (m_cancelTransfer) {
                callSession([session = m_session, id] { session->abortTransfer(id, 0); });
                failed = true;
                return;
            }
            callSession([session = m_session, id] { session->finishUpload(id); });
        });
        while (!future.isFinished()) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (failed || m_cancelTransfer) {
            m_transfer->setText(zh("已取消"));
            break;
        }
        if (m_uploadResult < 0) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            m_uploadLoop = &loop;
            timer.start(30000);
            if (m_uploadResult < 0) loop.exec();
            m_uploadLoop = nullptr;
        }
        if (m_uploadResult == 1) {
            ++uploaded;
            m_bar->setValue(100);
            m_transfer->setText(zh("已上传到 ") + remoteFull(job.second));
        } else if (m_transfer->text().startsWith(zh("正在上传"))) {
            m_transfer->setText(zh("上传未确认"));
        }
    }
    if (uploaded > 1) m_transfer->setText(zh("已上传 %1 个文件").arg(uploaded));
    if (m_connected) refreshFiles();
    m_transferBusy = false;
}

void MainWindow::deleteSelected() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (!m_connected || !item) return;
    const QString rel = item->data(0, RoleRel).toString();
    if (rel.isEmpty()) return;
    const QString full = remoteFull(rel);
    const QString question = item->data(0, RoleDir).toBool()
        ? zh("删除这个文件夹以及里面的内容？\n") + full
        : zh("删除这个文件？\n") + full;
    if (QMessageBox::warning(this, zh("删除"), question, QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
        return;
    bool ok = false;
    callSession([session = m_session, rel, &ok] { ok = session->requestDelete(rel); });
    if (!ok) m_transfer->setText(zh("删除请求发送失败"));
}

void MainWindow::cancelTransfer() {
    m_cancelTransfer = true;
    const quint32 uploadId = m_uploadId;
    const quint32 downloadId = m_downloadId;
    if (uploadId != 0) postSession([session = m_session, uploadId] { session->abortTransfer(uploadId, 0); });
    if (downloadId != 0) postSession([session = m_session, downloadId] { session->abortTransfer(downloadId, 1); });
    closeDownload(true);
    failPending();
    m_transfer->setText(zh("已取消"));
}

void MainWindow::failPending() {
    if (m_uploadResult < 0) m_uploadResult = 0;
    if (m_downloadResult < 0) m_downloadResult = 0;
    if (m_uploadLoop) m_uploadLoop->quit();
    if (m_downloadLoop) m_downloadLoop->quit();
}

void MainWindow::onDownloadStarted(quint32 id, const QString& name, quint64 size) {
    auto* file = new QFile(m_downloadPath);
    if (!file->open(QIODevice::WriteOnly)) {
        const QString message = file->errorString();
        delete file;
        postSession([session = m_session, id] { session->abortTransfer(id, 1); });
        m_transfer->setText(message);
        m_downloadResult = 0;
        if (m_downloadLoop) m_downloadLoop->quit();
        return;
    }
    delete m_downloadFile;
    m_downloadFile = file;
    m_downloadId = id;
    m_downloadSize = size;
    m_downloadGot = 0;
    m_downloadOpened = true;
    m_bar->setValue(size == 0 ? 100 : 0);
    m_transfer->setText(zh("正在下载 ") + name);
}

void MainWindow::onDownloadChunk(quint32 id, const QByteArray& data) {
    if (!m_downloadFile || id != m_downloadId) return;
    m_downloadFile->write(data);
    m_downloadGot += quint64(data.size());
    const double percent = m_downloadSize == 0 ? 100.0 : m_downloadGot * 100.0 / double(m_downloadSize);
    m_bar->setValue(std::min(100, int(percent)));
}

void MainWindow::onTransferFinished(quint32 id, quint8 direction) {
    if (direction == 0 && id == m_uploadId) {
        m_uploadResult = 1;
        if (m_uploadLoop) m_uploadLoop->quit();
        return;
    }
    if (direction == 1 && (id == m_downloadId || id == 0)) {
        closeDownload(false);
        m_bar->setValue(100);
        m_transfer->setText(zh("下载完成"));
        m_downloadId = 0;
        m_downloadResult = 1;
        if (m_downloadLoop) m_downloadLoop->quit();
    }
}

void MainWindow::onTransferFailed(quint32 id, quint8 direction, const QString& message) {
    if (direction == 2) {
        m_transfer->setText(message.trimmed().isEmpty() ? zh("打不开这个目录") : message);
        return;
    }
    if (direction == 0 && (id == m_uploadId || id == 0)) {
        m_uploadResult = 0;
        if (m_uploadLoop) m_uploadLoop->quit();
    }
    if (direction == 1) {
        closeDownload(true);
        m_downloadId = 0;
        m_downloadResult = 0;
        if (m_downloadLoop) m_downloadLoop->quit();
    }
    m_transfer->setText(message.trimmed().isEmpty() ? zh("传输失败") : message);
}

void MainWindow::closeDownload(bool deletePartial) {
    if (m_downloadFile) {
        m_downloadFile->close();
        delete m_downloadFile;
        m_downloadFile = nullptr;
    }
    if (deletePartial && m_downloadOpened && !m_downloadPath.isEmpty()) QFile::remove(m_downloadPath);
    m_downloadOpened = false;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease && m_swallowMouseRelease) {
        m_swallowMouseRelease = false;
        return true;
    }
    if (m_picturePanel && m_picturePanel->isVisible() && qobject_cast<QWidget*>(watched)) {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape && !key->isAutoRepeat()) {
                m_pictureButton->setChecked(false);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            const QPoint global = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
            auto hits = [global](QWidget* widget) {
                return widget && widget->isVisible() && widget->rect().contains(widget->mapFromGlobal(global));
            };
            if (hits(m_fullScreen)) {
                toggleFilePanel();
                m_swallowMouseRelease = true;
                m_pictureButton->setChecked(false);
                return true;
            }
            bool inside = hits(m_picturePanel) || hits(m_pictureButton);
            for (auto* widget = static_cast<QWidget*>(watched); widget && !inside; widget = widget->parentWidget()) {
                if (widget == m_picturePanel || widget == m_pictureButton) inside = true;
            }
            QWidget* popup = QApplication::activePopupWidget();
            if (!inside && !(popup && hits(popup))) m_pictureButton->setChecked(false);
        }
    }
    if (watched == m_tree && event->type() == QEvent::FocusIn && m_keyboard) releaseKeyboard();
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Delete) {
            deleteSelected();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
