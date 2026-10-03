#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QKeySequence>
#include <QWidget>

class ComboBox : public QComboBox {
public:
    explicit ComboBox(QWidget *parent=nullptr);
};

class ShortcutField : public QLineEdit {
    Q_OBJECT
public:
    explicit ShortcutField(const QKeySequence &sequence, QWidget *parent=nullptr);
    QKeySequence keySequence() const { return sequence; }
    void setKeySequence(const QKeySequence &value);
signals:
    void keySequenceChanged(const QKeySequence &sequence);
    void editingChanged(bool editing);
protected:
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    void display();
    QKeySequence sequence;
};

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
