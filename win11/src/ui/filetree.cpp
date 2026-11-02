#include "ui/filetree.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QMimeData>
#include <QUrl>

FileTree::FileTree(QWidget* parent) : QTreeWidget(parent) {
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->installEventFilter(this);
    setHeaderHidden(true);
    setSelectionMode(SingleSelection);
    setExpandsOnDoubleClick(true);
}

bool FileTree::eventFilter(QObject* watched, QEvent* event) {
    if (watched != viewport()) return QTreeWidget::eventFilter(watched, event);
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto* drag = static_cast<QDropEvent*>(event);
        if (!drag->mimeData()->hasUrls()) return false;
        drag->acceptProposedAction();
        return true;
    }
    if (event->type() != QEvent::Drop) return QTreeWidget::eventFilter(watched, event);

    auto* drop = static_cast<QDropEvent*>(event);
    QStringList paths;
    for (const QUrl& url : drop->mimeData()->urls()) {
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty()) emit filesDropped(paths, itemAt(drop->position().toPoint()));
    drop->acceptProposedAction();
    return true;
}
