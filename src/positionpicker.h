#pragma once
#include <QDialog>
#include <QList>

class PositionPicker : public QDialog {
    Q_OBJECT
public:
    explicit PositionPicker(QWidget *parent=nullptr,bool rectangle=false);
    QPoint position() const { return selected; }
    QRect region() const { return QRect(selected,endpoint).normalized(); }
protected:
    bool eventFilter(QObject *,QEvent *) override;
    bool nativeEvent(const QByteArray &,void *,qintptr *) override;
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void done(int result) override;
private:
    QList<QWidget *> panels;
    QPoint selected;
    QPoint endpoint;
    bool rectangleMode=false;
    void updateSelection();
    bool pressed=false;
    Qt::MouseButtons heldButtons;
    bool cancelRequested=false,selectionReleased=false;
};
