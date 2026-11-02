#include "session/discovery.h"

#include "protocol/protocol.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QThread>
#include <QThreadPool>
#include <QElapsedTimer>
#include <QSet>
#include <QMutex>

#include <algorithm>

namespace {

bool readExact(QTcpSocket& socket, char* data, int size, int timeoutMs) {
    int got = 0;
    QElapsedTimer timer;
    timer.start();
    while (got < size) {
        const int left = timeoutMs - int(timer.elapsed());
        if (left <= 0) return false;
        if (socket.bytesAvailable() < size - got && !socket.waitForReadyRead(left)) return false;
        const qint64 n = socket.read(data + got, size - got);
        if (n <= 0) return false;
        got += int(n);
    }
    return true;
}

bool probe(const QString& host, int port, const std::atomic<int>& epoch, int current) {
    if (epoch.load() != current) return false;
    QTcpSocket socket;
    socket.connectToHost(host, quint16(port));
    if (!socket.waitForConnected(400)) return false;
    proto::Writer hello;
    hello.raw("NDBG0001", 8);
    hello.u16(1);
    const QByteArray packet = proto::Writer::packet(proto::Hello, hello.take());
    if (socket.write(packet) != packet.size() || !socket.waitForBytesWritten(400)) return false;
    char header[5];
    if (!readExact(socket, header, 5, 400)) return false;
    if (quint8(header[0]) != proto::Challenge) return false;
    const quint32 length = quint32(quint8(header[1]))
        | (quint32(quint8(header[2])) << 8)
        | (quint32(quint8(header[3])) << 16)
        | (quint32(quint8(header[4])) << 24);
    return length == 16;
}

int maskPrefix(const QNetworkAddressEntry& entry) {
    const int prefix = entry.prefixLength();
    if (prefix >= 0) return prefix;
    quint32 mask = entry.netmask().toIPv4Address();
    int bits = 0;
    while (mask) {
        bits += int(mask & 1u);
        mask >>= 1;
    }
    return bits;
}

QStringList localHosts() {
    QSet<QString> self;
    QSet<QString> hosts;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& nic : interfaces) {
        if (!(nic.flags() & QNetworkInterface::IsUp)) continue;
        if (nic.flags() & QNetworkInterface::IsLoopBack) continue;
        for (const QNetworkAddressEntry& entry : nic.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            self.insert(entry.ip().toString());
            int prefix = maskPrefix(entry);
            if (prefix < 1 || prefix > 30) continue;
            if (prefix < 24) prefix = 24;
            const quint32 ip = entry.ip().toIPv4Address();
            const quint32 mask = 0xffffffffu << (32 - prefix);
            const quint32 network = ip & mask;
            const quint32 broadcast = network | ~mask;
            for (quint32 host = network + 1; host < broadcast; ++host)
                hosts.insert(QHostAddress(host).toString());
        }
    }
    for (const QString& address : self) hosts.remove(address);
    return hosts.values();
}

}  // namespace

int DiscoveryWorker::bump() { return m_epoch.fetch_add(1) + 1; }

void DiscoveryWorker::scan(int port, int epoch) {
    if (m_epoch.load() != epoch) return;
    const QStringList hosts = localHosts();
    const int total = hosts.size();
    QStringList found;
    QMutex foundLock;
    std::atomic<int> done{0};
    QThreadPool pool;
    pool.setMaxThreadCount(48);
    for (const QString& host : hosts) {
        if (m_epoch.load() != epoch) break;
        pool.start([&, host] {
            if (m_epoch.load() == epoch && probe(host, port, m_epoch, epoch)) {
                QMutexLocker lock(&foundLock);
                found.append(host);
            }
            const int finished = ++done;
            if (m_epoch.load() == epoch && (finished == total || finished % 32 == 0))
                emit progress(QString::fromUtf8("正在搜索 %1/%2").arg(finished).arg(total));
        });
    }
    pool.waitForDone();
    if (m_epoch.load() != epoch) return;
    std::sort(found.begin(), found.end(), [](const QString& a, const QString& b) {
        return QHostAddress(a).toIPv4Address() < QHostAddress(b).toIPv4Address();
    });
    found.removeDuplicates();
    emit finished(found);
}

Discovery::Discovery(QObject* parent) : QObject(parent) {
    m_worker = new DiscoveryWorker;
    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &DiscoveryWorker::progress, this, &Discovery::progress);
    connect(m_worker, &DiscoveryWorker::finished, this, &Discovery::finished);
    m_thread->start();
}

Discovery::~Discovery() {
    cancel();
    m_thread->quit();
    m_thread->wait(5000);
}

void Discovery::start(int port) {
    const int epoch = m_worker->bump();
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, port, epoch] { worker->scan(port, epoch); },
                              Qt::QueuedConnection);
}

void Discovery::cancel() {
    if (m_worker) m_worker->bump();
}
