#pragma once

#include <QObject>
#include <QStringList>
#include <atomic>

class DiscoveryWorker : public QObject {
    Q_OBJECT
public:
    int bump();

public slots:
    void scan(int port, int epoch);

signals:
    void progress(const QString& text);
    void finished(const QStringList& hosts);

private:
    std::atomic<int> m_epoch{0};
};

class Discovery : public QObject {
    Q_OBJECT
public:
    explicit Discovery(QObject* parent = nullptr);
    ~Discovery() override;
    void start(int port);
    void cancel();

signals:
    void progress(const QString& text);
    void finished(const QStringList& hosts);

private:
    class QThread* m_thread = nullptr;
    DiscoveryWorker* m_worker = nullptr;
};
