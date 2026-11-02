#include "ui/screenview.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>

ScreenView::ScreenView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_placeholder = QString::fromUtf8("未连接");
}

void ScreenView::setFrame(const QImage& image, int originWidth, int originHeight) {
    m_image = image;
    m_originWidth = originWidth;
    m_originHeight = originHeight;
    update();
}

bool ScreenView::applyTiles(int imageWidth, int imageHeight, int originWidth, int originHeight,
                            const QVector<ScreenTile>& tiles) {
    if (m_image.isNull() || m_image.width() != imageWidth || m_image.height() != imageHeight) return false;
    QPainter painter(&m_image);
    if (!painter.isActive()) return false;
    for (const ScreenTile& tile : tiles) {
        if (tile.image.isNull() || tile.x < 0 || tile.y < 0) continue;
        if (tile.x >= m_image.width() || tile.y >= m_image.height()) continue;
        painter.drawImage(tile.x, tile.y, tile.image);
    }
    m_originWidth = originWidth;
    m_originHeight = originHeight;
    update();
    return true;
}

void ScreenView::setConnected(bool connected) {
    m_connected = connected;
    if (!connected) m_capturing = false;
    update();
}

void ScreenView::setCapturing(bool capturing) {
    m_capturing = capturing;
    update();
}

void ScreenView::setPlaceholder(const QString& text) {
    m_placeholder = text;
    update();
}

void ScreenView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#17191d"));
    if (!m_image.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(imageRect(), m_image);
    } else {
        painter.setPen(QColor("#f2f2f2"));
        QFont title = font();
        title.setPixelSize(18);
        painter.setFont(title);
        painter.drawText(rect(), Qt::AlignCenter, m_placeholder);
    }
    if (!m_connected) return;

    const QString hint = m_capturing
        ? QString::fromUtf8("键盘控制中，Ctrl+Alt 交回本机")
        : QString::fromUtf8("点击画面后键盘才会发到对方，Ctrl+Alt 交回本机");
    QFont hintFont = font();
    hintFont.setPixelSize(12);
    painter.setFont(hintFont);
    const int textWidth = painter.fontMetrics().horizontalAdvance(hint);
    const QRect box(10, height() - 36, textWidth + 16, 26);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawRoundedRect(box, 4, 4);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, hint);
}

QRectF ScreenView::imageRect() const {
    if (m_image.isNull() || width() < 1 || height() < 1) return {};
    const double scale = std::min(width() / double(m_image.width()), height() / double(m_image.height()));
    const double drawnW = m_image.width() * scale;
    const double drawnH = m_image.height() * scale;
    return QRectF((width() - drawnW) / 2.0, (height() - drawnH) / 2.0, drawnW, drawnH);
}

bool ScreenView::mapToDesktop(const QPointF& pos, quint16* x, quint16* y, bool allowOutside) const {
    const QRectF drawn = imageRect();
    if (drawn.width() < 1 || drawn.height() < 1) return false;
    const double frameW = m_image.width();
    const double frameH = m_image.height();
    double frameX = (pos.x() - drawn.left()) / drawn.width() * frameW;
    double frameY = (pos.y() - drawn.top()) / drawn.height() * frameH;
    const bool inside = frameX >= 0 && frameY >= 0 && frameX < frameW && frameY < frameH;
    if (!inside && !allowOutside) return false;
    frameX = std::clamp(frameX, 0.0, frameW - 1);
    frameY = std::clamp(frameY, 0.0, frameH - 1);
    const int originW = m_originWidth > 0 ? m_originWidth : int(frameW);
    const int originH = m_originHeight > 0 ? m_originHeight : int(frameH);
    const int desktopX = int(frameX / frameW * originW);
    const int desktopY = int(frameY / frameH * originH);
    *x = quint16(std::clamp(desktopX, 0, 65535));
    *y = quint16(std::clamp(desktopY, 0, 65535));
    return true;
}

void ScreenView::emitMapped(const QPointF& pos, quint8 button, quint8 action, qint16 wheel, bool allowOutside) {
    if (!m_connected) return;
    quint16 x = 0;
    quint16 y = 0;
    if (!mapToDesktop(pos, &x, &y, allowOutside)) return;
    emit mouseAction(x, y, button, action, wheel);
}

void ScreenView::mouseMoveEvent(QMouseEvent* event) {
    emitMapped(event->position(), 0, 0, 0, mouseGrabber() == this);
}

void ScreenView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    grabMouse();
    emit captureRequested();
    const quint8 button = event->button() == Qt::LeftButton ? 1
        : event->button() == Qt::RightButton ? 2
        : event->button() == Qt::MiddleButton ? 3
                                               : 0;
    if (button != 0) emitMapped(event->position(), button, 1, 0, true);
    event->accept();
}

void ScreenView::mouseReleaseEvent(QMouseEvent* event) {
    const quint8 button = event->button() == Qt::LeftButton ? 1
        : event->button() == Qt::RightButton ? 2
        : event->button() == Qt::MiddleButton ? 3
                                               : 0;
    if (button != 0) emitMapped(event->position(), button, 2, 0, true);
    const Qt::MouseButtons held = event->buttons();
    if (!(held & (Qt::LeftButton | Qt::RightButton | Qt::MiddleButton))) releaseMouse();
}

void ScreenView::wheelEvent(QWheelEvent* event) {
    int notches = event->angleDelta().y() / 120;
    if (notches == 0 && event->angleDelta().y() != 0) notches = event->angleDelta().y() > 0 ? 1 : -1;
    if (notches == 0) return;
    emitMapped(event->position(), 0, 3, qint16(notches), false);
}

void ScreenView::keyPressEvent(QKeyEvent* event) {
    emit keyDown(quint32(event->nativeVirtualKey()), event->modifiers(), event->isAutoRepeat());
    event->accept();
}

void ScreenView::keyReleaseEvent(QKeyEvent* event) {
    emit keyUp(quint32(event->nativeVirtualKey()), event->isAutoRepeat());
    event->accept();
}
