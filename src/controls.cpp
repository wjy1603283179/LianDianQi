#include "controls.h"
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QListView>
#include <QWheelEvent>
#include <cmath>

void NoWheelSpinBox::wheelEvent(QWheelEvent *event) { event->ignore(); }

ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
    auto *list=new QListView;
    list->setUniformItemSizes(true); list->setSpacing(2);
    setView(list); setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(3); setMaxVisibleItems(10);
}

ShortcutField::ShortcutField(const QKeySequence &value,QWidget *parent) : QLineEdit(parent),sequence(value) {
    setReadOnly(true); setContextMenuPolicy(Qt::NoContextMenu);
    setFocusPolicy(Qt::ClickFocus); // Opening the settings tab must not begin shortcut capture.
    setMinimumWidth(180); setPlaceholderText(QStringLiteral("点击设置"));
    setAccessibleName(QStringLiteral("快捷键")); display();
}
void ShortcutField::display() { setText(sequence.toString(QKeySequence::NativeText)); }
void ShortcutField::setKeySequence(const QKeySequence &value) {
    if(sequence==value) { display(); return; }
    sequence=value; display(); emit keySequenceChanged(sequence);
}
void ShortcutField::focusInEvent(QFocusEvent *event) {
    QLineEdit::focusInEvent(event); setText(QString()); setPlaceholderText(QStringLiteral("按下快捷键"));
    emit editingChanged(true);
}
void ShortcutField::focusOutEvent(QFocusEvent *event) {
    QLineEdit::focusOutEvent(event); setPlaceholderText(QStringLiteral("点击设置")); display();
    emit editingChanged(false);
}
void ShortcutField::keyPressEvent(QKeyEvent *event) {
    if(event->isAutoRepeat()) { event->accept(); return; }
    int key=event->key();
    if(key==Qt::Key_Escape) { setKeySequence(QKeySequence()); clearFocus(); }
    else if(key!=Qt::Key_Control && key!=Qt::Key_Shift && key!=Qt::Key_Alt && key!=Qt::Key_Meta && key!=Qt::Key_AltGr && key!=Qt::Key_unknown) {
        auto modifiers=event->modifiers() & (Qt::ControlModifier|Qt::AltModifier|Qt::ShiftModifier|Qt::MetaModifier);
        setKeySequence(QKeySequence(QKeyCombination(modifiers,Qt::Key(key)))); clearFocus();
    }
    event->accept();
}

class CompactDoubleSpinBox : public QDoubleSpinBox {
protected:
    QString textFromValue(double value) const override {
        QString text=QDoubleSpinBox::textFromValue(value);
        const QString separator=locale().decimalPoint();
        if(text.contains(separator)) {
            while(text.endsWith('0')) text.chop(1);
            if(text.endsWith(separator)) text.chop(separator.size());
        }
        return text;
    }
};

TimeField::TimeField(int low,int high,int initial,QWidget *parent)
    : QWidget(parent),minimumMs(low),maximumMs(high) {
    auto *layout=new QHBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(8);
    number=new CompactDoubleSpinBox; number->setObjectName("number"); number->setMinimumWidth(105);
    number->setButtonSymbols(QAbstractSpinBox::UpDownArrows); number->setKeyboardTracking(false);
    unit=new ComboBox; unit->setObjectName("unit"); unit->addItem("ms",1); unit->addItem("s",1000); unit->addItem("min",60000);
    unit->setFixedWidth(104); unit->setToolTip(QStringLiteral("时间单位"));
    layout->addWidget(number,1); layout->addWidget(unit);
    setValue(initial);
    connect(number,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this] { emit valueChanged(value()); });
    connect(unit,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index) {
        number->interpretText(); // Commit an edited value before converting units.
        int milliseconds=value(); display(milliseconds,index);
    });
}
int TimeField::value() const {
    return qBound(minimumMs,int(std::llround(number->value()*scale)),maximumMs);
}
void TimeField::display(int milliseconds,int index) {
    QSignalBlocker blockNumber(number),blockUnit(unit);
    scale=unit->itemData(index).toInt(); unit->setCurrentIndex(index);
    number->setDecimals(index==0?0:index==1?3:6);
    number->setRange(double(minimumMs)/scale,double(maximumMs)/scale);
    number->setSingleStep(index==0?1.0:0.1);
    number->setValue(double(qBound(minimumMs,milliseconds,maximumMs))/scale);
}
void TimeField::setValue(int milliseconds) {
    int previous=value();
    display(milliseconds,milliseconds>=60000?2:milliseconds>=1000?1:0);
    if(value()!=previous) emit valueChanged(value());
}
void TimeField::setSpecialValueText(const QString &text) { number->setSpecialValueText(text); }
