#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

struct ScreenTile {
    int x = 0;
    int y = 0;
    QImage image;
};

class ScreenView : public QWidget {
    Q_OBJECT
public:
    explicit ScreenView(QWidget* parent = nullptr);
    void setFrame(const QImage& image, int originWidth, int originHeight);
    bool applyTiles(int imageWidth, int imageHeight, int originWidth, int originHeight, const QVector<ScreenTile>& tiles);
    void setConnected(bool connected);
    void setCapturing(bool capturing);
    void setPlaceholder(const QString& text);

signals:
    void mouseAction(quint16 x, quint16 y, quint8 button, quint8 action, qint16 wheel);
    void captureRequested();
    void keyDown(quint32 virtualKey, Qt::KeyboardModifiers modifiers, bool repeat);
    void keyUp(quint32 virtualKey, bool repeat);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    QRectF imageRect() const;
    bool mapToDesktop(const QPointF& pos, quint16* x, quint16* y, bool allowOutside) const;
    void emitMapped(const QPointF& pos, quint8 button, quint8 action, qint16 wheel, bool allowOutside);

    QImage m_image;
    int m_originWidth = 0;
    int m_originHeight = 0;
    bool m_connected = false;
    bool m_capturing = false;
    QString m_placeholder;
};
