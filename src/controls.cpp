#include "controls.h"
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <cmath>

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
    unit=new QComboBox; unit->setObjectName("unit"); unit->addItem("ms",1); unit->addItem("s",1000); unit->addItem("min",60000);
    unit->setFixedWidth(86); unit->setToolTip(QStringLiteral("时间单位"));
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
