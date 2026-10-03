#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QWidget>

class TimeField : public QWidget {
    Q_OBJECT
public:
    explicit TimeField(int minimumMs, int maximumMs, int initialMs, QWidget *parent=nullptr);
    int value() const;
    void setValue(int milliseconds);
    void setSpecialValueText(const QString &text);
signals:
    void valueChanged(int milliseconds);
private:
    void display(int milliseconds, int index);
    QDoubleSpinBox *number;
    QComboBox *unit;
    int minimumMs, maximumMs, scale=1;
};
