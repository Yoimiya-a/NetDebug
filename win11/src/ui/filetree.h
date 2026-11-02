#pragma once

#include <QTreeWidget>

class QDropEvent;

class FileTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit FileTree(QWidget* parent = nullptr);

signals:
    void filesDropped(const QStringList& paths, QTreeWidgetItem* item);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
