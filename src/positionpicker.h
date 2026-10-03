#pragma once
#include <QDialog>
#include <QList>

class PositionPicker : public QDialog {
    Q_OBJECT
public:
    explicit PositionPicker(QWidget *parent=nullptr);
    QPoint position() const { return selected; }
protected:
    bool eventFilter(QObject *,QEvent *) override;
    bool nativeEvent(const QByteArray &,void *,qintptr *) override;
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void done(int result) override;
private:
    QList<QWidget *> panels;
    QPoint selected;
    bool pressed=false;
};
