#include "positionpicker.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <QVariant>
#include <windows.h>

static void rememberNativePress(QWidget *surface,void *message) {
    auto *msg=static_cast<MSG *>(message);
    if(msg->message==WM_MOUSEMOVE || msg->message==WM_LBUTTONUP)
        surface->setProperty("nativePosition",QPoint(msg->pt.x,msg->pt.y));
    if(msg->message!=WM_LBUTTONDOWN && msg->message!=WM_LBUTTONDBLCLK) return;
    surface->setProperty("nativePressPosition",QPoint(msg->pt.x,msg->pt.y));
}

static void paintSurface(QWidget *surface) {
    QPainter painter(surface);
    // Alpha must stay nonzero across the screen: fully transparent Windows
    // layered-window pixels let clicks through to the application underneath.
    painter.fillRect(surface->rect(),QColor(20,28,50,22));
    painter.setRenderHint(QPainter::Antialiasing);
    const QString hint=surface->property("rectangleMode").toBool()?QStringLiteral("拖动框选 · Esc 取消"):QStringLiteral("点击选取 · Esc 取消");
    const int width=painter.fontMetrics().horizontalAdvance(hint)+36;
    QRect bubble((surface->width()-width)/2,22,width,40);
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(255,255,255,245)); painter.drawRoundedRect(bubble,10,10);
    painter.setPen(QColor("#25304a")); painter.drawText(bubble,Qt::AlignCenter,hint);
    auto selected=surface->property("selectionRectangle").toRect();
    if(!selected.isEmpty()) {
        RECT native={}; GetWindowRect(reinterpret_cast<HWND>(surface->winId()),&native);
        const qreal ratio=surface->devicePixelRatioF();
        QRectF selection((selected.x()-native.left)/ratio,(selected.y()-native.top)/ratio,selected.width()/ratio,selected.height()/ratio);
        painter.setPen(QPen(QColor("#6371dc"),2)); painter.setBrush(QColor(255,255,255,35)); painter.drawRect(selection);
    }
}
class PickerSurface : public QWidget {
public:
    explicit PickerSurface(QWidget *parent) : QWidget(parent,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint) {}
protected:
    void paintEvent(QPaintEvent *) override { paintSurface(this); }
    bool nativeEvent(const QByteArray &type,void *message,qintptr *result) override {
        rememberNativePress(this,message); return QWidget::nativeEvent(type,message,result);
    }
};

PositionPicker::PositionPicker(QWidget *parent,bool rectangle)
    : QDialog(parent,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint),rectangleMode(rectangle) {
    setObjectName("positionPicker"); setWindowTitle(QStringLiteral("拾取屏幕坐标"));
    for(auto *screen:QApplication::screens()) {
        QWidget *panel=screen==QApplication::primaryScreen()?static_cast<QWidget *>(this):new PickerSurface(this);
        if(panel!=this) panel->setObjectName("positionPickerSurface"); panel->setAttribute(Qt::WA_TranslucentBackground);
        panel->setCursor(Qt::CrossCursor); panel->setFocusPolicy(Qt::StrongFocus); panel->installEventFilter(this);
        panel->setMouseTracking(rectangle); panel->setProperty("rectangleMode",rectangle);
        panel->winId(); panel->windowHandle()->setScreen(screen); panel->setGeometry(screen->geometry());
        // A normal QDialog may be centered on its owner when exec() shows it,
        // moving the overlays away from their screens. Fullscreen placement
        // also lets the platform use each monitor's complete native bounds.
        panel->setWindowState(Qt::WindowFullScreen);
        panels.append(panel);
        connect(screen,&QScreen::geometryChanged,this,[this] { reject(); });
    }
    connect(qApp,&QGuiApplication::screenAdded,this,[this] { reject(); });
    connect(qApp,&QGuiApplication::screenRemoved,this,[this] { reject(); });
    connect(qApp,&QGuiApplication::primaryScreenChanged,this,[this] { reject(); });
}
void PositionPicker::updateSelection() {
    if(!rectangleMode) return;
    for(auto *panel:panels) { panel->setProperty("selectionRectangle",region()); panel->update(); }
}
void PositionPicker::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    for(auto *panel:panels) if(panel!=this) panel->show();
    raise(); activateWindow(); setFocus();
}
void PositionPicker::paintEvent(QPaintEvent *) { paintSurface(this); }
bool PositionPicker::nativeEvent(const QByteArray &type,void *message,qintptr *result) {
    rememberNativePress(this,message); return QDialog::nativeEvent(type,message,result);
}
bool PositionPicker::eventFilter(QObject *watched,QEvent *event) {
    if(event->type()==QEvent::KeyPress) {
        if(static_cast<QKeyEvent *>(event)->key()==Qt::Key_Escape) {
            // Absorb outstanding button releases even when cancelling midway
            // through a click; a release alone can activate some target UIs.
            if(heldButtons) cancelRequested=true; else reject();
        }
        return true;
    }
    if(event->type()==QEvent::KeyRelease || event->type()==QEvent::Wheel) return true;
    if(event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseButtonDblClick) {
        auto *mouse=static_cast<QMouseEvent *>(event);
        heldButtons|=mouse->button();
        if(mouse->button()==Qt::LeftButton) {
            auto *panel=qobject_cast<QWidget *>(watched);
            panel->setProperty("nativePosition",QVariant());
            const QVariant nativePoint=panel->property("nativePressPosition");
            if(nativePoint.isValid()) {
                // Retain the original device-pixel position before Qt scales it;
                // integer logical coordinates can lose a pixel at fractional DPI.
                selected=nativePoint.toPoint(); pressed=true; panel->setProperty("nativePressPosition",QVariant());
                endpoint=selected; updateSelection();
                return true;
            }
            const qreal scale=panel->devicePixelRatioF();
            POINT point{qRound(mouse->position().x()*scale),qRound(mouse->position().y()*scale)};
            // Convert the event's client position, not the cursor's later position.
            // Native client coordinates are physical pixels on this DPI-aware app.
            if(ClientToScreen(reinterpret_cast<HWND>(panel->winId()),&point) || GetPhysicalCursorPos(&point)) {
                selected=QPoint(point.x,point.y); pressed=true;
                endpoint=selected; updateSelection();
            }
        }
        return true;
    }
    if(rectangleMode && pressed && (event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonRelease)) {
        auto *panel=qobject_cast<QWidget *>(watched); auto *mouse=static_cast<QMouseEvent *>(event);
        auto native=panel->property("nativePosition");
        if(native.isValid()) { endpoint=native.toPoint(); panel->setProperty("nativePosition",QVariant()); }
        else {
            POINT point{qRound(mouse->position().x()*panel->devicePixelRatioF()),qRound(mouse->position().y()*panel->devicePixelRatioF())};
            if(ClientToScreen(reinterpret_cast<HWND>(panel->winId()),&point)) endpoint=QPoint(point.x,point.y);
        }
        updateSelection();
        if(event->type()==QEvent::MouseMove) return true;
    }
    if(event->type()==QEvent::MouseButtonRelease) {
        auto *mouse=static_cast<QMouseEvent *>(event);
        heldButtons&=~Qt::MouseButtons(mouse->button());
        // Keep the overlay alive until release so neither half of the click
        // reaches a target window or changes foreground applications.
        if(mouse->button()==Qt::LeftButton && pressed) { pressed=false; selectionReleased=true; }
        if(!heldButtons) {
            if(cancelRequested) reject(); else if(selectionReleased) accept();
        }
        return true;
    }
    return QDialog::eventFilter(watched,event);
}
void PositionPicker::done(int result) {
    for(auto *panel:panels) if(panel!=this) panel->hide();
    QDialog::done(result);
}
