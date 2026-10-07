#include "window.h"
#include "positionpicker.h"
#include "logdialog.h"
#include <QApplication>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QPainter>
#include <QMessageBox>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QScrollArea>
#include <QTabBar>
#include <QVBoxLayout>
#include <QBuffer>
#include <QImageReader>
#include <QScopeGuard>
#include <QEventLoop>

static QLabel *label(const QString &s, const char *name=nullptr) {
    auto *w=new QLabel(s); w->setWordWrap(true); if(name) w->setObjectName(name); return w;
}
static QSpinBox *spin(int low, int high, int value) {
    auto *w=new QSpinBox; w->setRange(low,high); w->setValue(value); w->setMinimumWidth(115); w->setButtonSymbols(QAbstractSpinBox::UpDownArrows); return w;
}
static QWidget *row(std::initializer_list<QWidget *> widgets) {
    auto *w=new QWidget; auto *l=new QHBoxLayout(w); l->setContentsMargins(0,0,0,0); l->setSpacing(8);
    for(auto *c:widgets) {
        if(qobject_cast<QPushButton *>(c)) c->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        l->addWidget(c,qobject_cast<QComboBox *>(c)?1:0);
    }
    return w;
}
static QFrame *card() { auto *f=new QFrame; f->setObjectName("card"); return f; }
static QScrollArea *makeScroll(QWidget *content,const char *name) {
    auto *area=new QScrollArea; area->setObjectName(name); area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame); area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content); return area;
}

class CaptureDialog : public QDialog {
public:
    CaptureDialog(bool position, QWidget *parent) : QDialog(parent),monitor(this) {
        setWindowTitle(position?QStringLiteral("拾取屏幕坐标"):QStringLiteral("采集输入")); setMinimumWidth(440);
        auto *l=new QVBoxLayout(this); l->setContentsMargins(28,24,28,24); l->setSpacing(18);
        l->addWidget(label(position?QStringLiteral("点击需要执行的位置"):QStringLiteral("按下任意键或鼠标按钮"),"sectionTitle"));
        auto *cancel=new QPushButton(QStringLiteral("取消")); l->addWidget(cancel); connect(cancel,&QPushButton::clicked,this,&QDialog::reject);
        connect(&monitor,&InputMonitor::observed,this,[this,position](InputKey k,bool down,QPoint p,quint64) {
            if(!down || !armed || (position && !k.mouse)) return;
            // Ignore the dialog's own Cancel button, but permit keyboard capture over it.
            HWND under=WindowFromPoint(POINT{p.x(),p.y()});
            if(k.mouse && GetAncestor(under,GA_ROOT)==reinterpret_cast<HWND>(winId())) return;
            armed=false; key=k; point=p; monitor.stop(); accept();
        },Qt::QueuedConnection);
        QTimer::singleShot(200,this,[this] { QString error; if(!monitor.start(error)) { QMessageBox::warning(this,QStringLiteral("采集失败"),error); reject(); } else armed=true; });
    }
    ~CaptureDialog() override { monitor.stop(); }
    InputKey key;
    QPoint point;
protected:
    void keyPressEvent(QKeyEvent *event) override { event->accept(); } // Capture Esc/Enter through the hook as ordinary inputs.
private:
    InputMonitor monitor;
    bool armed=false;
};

StepEditor::StepEditor(bool isQuick,QWidget *parent):QWidget(parent) {
    auto *form=new QFormLayout(this); form->setContentsMargins(0,0,0,0); form->setVerticalSpacing(16); form->setHorizontalSpacing(12);
    form->setSizeConstraint(QLayout::SetMinimumSize); form->setLabelAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    action=new ComboBox;
    for(int i=0;i<(isQuick?5:ActionCount);++i) action->addItem(actionName(Action(i)),i);
    action->setObjectName(isQuick?"quickAction":"stepAction");
    form->addRow(QStringLiteral("执行动作"),action);
    input=new QLineEdit; input->setObjectName(isQuick?"quickInput":"stepInput"); input->setReadOnly(true); input->setFocusPolicy(Qt::NoFocus); input->setText(currentKey.name());
    captureKey=new QPushButton(QStringLiteral("采集")); captureKey->setObjectName(isQuick?"quickCapture":"stepCapture"); captureKey->setToolTip(QStringLiteral("采集实际键盘或鼠标输入"));
    inputRow=row({input,captureKey}); form->addRow(QStringLiteral("输入按键"),inputRow);
    interval=new TimeField(5,3600000,100); interval->setObjectName(isQuick?"quickInterval":"stepInterval");
    intervalRow=row({interval}); form->addRow(QStringLiteral("连点间隔"),intervalRow);
    count=spin(0,1000000,0); count->setSpecialValueText(QStringLiteral("无限"));
    countRow=row({count}); form->addRow(QStringLiteral("重复次数"),countRow);
    duration=new TimeField(0,86400000,1000); duration->setObjectName(isQuick?"quickDuration":"stepDuration"); duration->setSpecialValueText(QStringLiteral("直到停止"));
    durationRow=row({duration}); form->addRow(QStringLiteral("持续时间"),durationRow);
    fixed=new QCheckBox(QStringLiteral("固定鼠标位置")); fixed->setObjectName(isQuick?"quickFixed":"stepFixed"); form->addRow(QString(),fixed);
    x=spin(-100000,100000,0); y=spin(-100000,100000,0); capturePosition=new QPushButton(QStringLiteral("拾取"));
    for(auto *coordinate:{x,y}) { coordinate->setButtonSymbols(QAbstractSpinBox::NoButtons); coordinate->setProperty("coordinate",true); coordinate->setMinimumWidth(75); }
    x->setObjectName(isQuick?"quickX":"stepX"); y->setObjectName(isQuick?"quickY":"stepY"); capturePosition->setObjectName(isQuick?"quickPositionCapture":"stepPositionCapture");
    positionRow=new QWidget; positionRow->setObjectName("positionRow"); auto *pl=new QHBoxLayout(positionRow); pl->setContentsMargins(0,0,0,0); pl->setSpacing(8);
    auto *xl=label("X"); auto *yl=label("Y"); xl->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed); yl->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    pl->addWidget(xl); pl->addWidget(x,1); pl->addSpacing(4); pl->addWidget(yl); pl->addWidget(y,1); pl->addWidget(capturePosition);
    form->addRow(QStringLiteral("屏幕坐标"),positionRow);
    imageRow=new QWidget; auto *images=new QVBoxLayout(imageRow); images->setContentsMargins(0,0,0,0); images->setSpacing(8);
    imagePreview=new QLabel; imagePreview->setObjectName("imagePreview"); imagePreview->setAlignment(Qt::AlignCenter); imagePreview->setMinimumHeight(64);
    imagePreview->setStyleSheet("background:#f8f9fd;border:1px solid #e5e8f0;border-radius:8px;padding:8px;"); images->addWidget(imagePreview);
    auto *shot=new QPushButton(QStringLiteral("截图")); shot->setObjectName("templateCapture"); auto *importImage=new QPushButton(QStringLiteral("导入图像")); importImage->setObjectName("templateImport");
    images->addWidget(row({shot,importImage})); form->addRow(QStringLiteral("图像模板"),imageRow);
    similarity=new NoWheelSpinBox; similarity->setRange(60,100); similarity->setValue(88); similarity->setObjectName("imageSimilarity"); similarity->setMinimumWidth(65); similarityRow=row({similarity,label("%")}); form->addRow(QStringLiteral("相似度"),similarityRow);
    scaleMatch=new QCheckBox(QStringLiteral("适配缩放")); scaleMatch->setObjectName("imageScaleMatch"); scaleMatch->setChecked(true); form->addRow(QString(),scaleMatch);
    limitRegion=new QCheckBox(QStringLiteral("限定识别范围")); limitRegion->setObjectName("imageLimitRegion"); form->addRow(QString(),limitRegion);
    regionInfo=new QLabel; regionInfo->setWordWrap(true); regionInfo->setObjectName("imageRegionInfo"); auto *regionSelect=new QPushButton(QStringLiteral("框选")); regionSelect->setObjectName("imageRegionSelect");
    regionRow=row({regionInfo,regionSelect}); form->addRow(QStringLiteral("识别范围"),regionRow);
    connect(shot,&QPushButton::clicked,this,[this] { selectImage(true); }); connect(importImage,&QPushButton::clicked,this,[this] { selectImage(false); });
    connect(regionSelect,&QPushButton::clicked,this,&StepEditor::selectRegion);
    connect(similarity,QOverload<int>::of(&QSpinBox::valueChanged),this,[this] { if(!loading) emit changed(); });
    for(auto *check:{scaleMatch,limitRegion}) connect(check,&QCheckBox::toggled,this,[this] { updateFields(); if(!loading) emit changed(); });
    refreshImage();
    connect(action,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this] { updateFields(); if(!loading) emit changed(); });
    for(auto *w:{interval,duration}) connect(w,&TimeField::valueChanged,this,[this] { if(!loading) emit changed(); });
    for(auto *w:{count,x,y}) connect(w,QOverload<int>::of(&QSpinBox::valueChanged),this,[this] { if(!loading) emit changed(); });
    connect(fixed,&QCheckBox::toggled,this,[this] { updateFields(); if(!loading) emit changed(); });
    connect(captureKey,&QPushButton::clicked,this,[this] { capture(false); });
    connect(capturePosition,&QPushButton::clicked,this,[this] { capture(true); });
    updateFields();
}
Step StepEditor::value() const {
    Step s; s.action=Action(action->currentData().toInt()); s.key=currentKey; s.interval=interval->value(); s.duration=duration->value();
    s.count=count->value(); s.fixedPosition=fixed->isChecked(); s.position=QPoint(x->value(),y->value());
    s.templatePng=templatePng; s.templateName=templateName; s.searchRegion=searchRegion;
    s.similarity=similarity->value(); s.scaleMatch=scaleMatch->isChecked(); s.limitRegion=limitRegion->isChecked(); return s;
}
void StepEditor::setKey(const InputKey &k) {
    currentKey=k; input->setText(k.name());
}
void StepEditor::setValue(const Step &s) {
    loading=true; action->setCurrentIndex(int(s.action)); setKey(s.key); interval->setValue(s.interval); duration->setValue(s.duration);
    count->setValue(s.count); fixed->setChecked(s.fixedPosition); x->setValue(s.position.x()); y->setValue(s.position.y());
    templatePng=s.templatePng; templateName=s.templateName; searchRegion=s.searchRegion;
    similarity->setValue(s.similarity); scaleMatch->setChecked(s.scaleMatch); limitRegion->setChecked(s.limitRegion); refreshImage(); loading=false; updateFields();
}
void StepEditor::updateFields() {
    auto a=Action(action->currentData().toInt()); auto *form=qobject_cast<QFormLayout *>(layout());
    auto visible=[form](QWidget *w,bool show) { w->setVisible(show); if(auto *l=form->labelForField(w)) l->setVisible(show); };
    visible(inputRow,isInputAction(a)); visible(intervalRow,a==Action::Repeat); visible(countRow,a==Action::Repeat || a==Action::LoopBegin);
    visible(durationRow,a==Action::Hold || a==Action::Wait);
    duration->setSpecialValueText(a==Action::Wait?QString():QStringLiteral("直到停止"));
    bool mouseInput=isInputAction(a) && currentKey.mouse;
    fixed->setVisible(mouseInput); visible(positionRow,a==Action::Move || (mouseInput && fixed->isChecked()));
    visible(imageRow,a==Action::IfImage); visible(similarityRow,a==Action::IfImage);
    scaleMatch->setVisible(a==Action::IfImage); limitRegion->setVisible(a==Action::IfImage); visible(regionRow,a==Action::IfImage && limitRegion->isChecked());
}
void StepEditor::refreshImage() {
    const QImage image=QImage::fromData(templatePng,"PNG");
    if(image.isNull()) imagePreview->setText(QStringLiteral("选择图像"));
    else imagePreview->setPixmap(QPixmap::fromImage(image).scaled(200,96,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    imagePreview->setToolTip(templateName);
    regionInfo->setText(searchRegion.isEmpty()?QStringLiteral("未选择"):QStringLiteral("(%1, %2)\n%3 × %4 px").arg(searchRegion.x()).arg(searchRegion.y()).arg(searchRegion.width()).arg(searchRegion.height()));
}
static void restoreOwner(QWidget *owner) { owner->setProperty("captureInProgress",false); if(owner->property("shutdownRequested").toBool()) return; owner->show(); owner->raise(); owner->activateWindow(); SetForegroundWindow(reinterpret_cast<HWND>(owner->winId())); }
static void settleDesktop() { QEventLoop loop; QTimer::singleShot(120,&loop,&QEventLoop::quit); loop.exec(); }
void StepEditor::selectRegion() {
    auto *owner=window(); owner->setProperty("captureInProgress",true); owner->hide(); auto restore=qScopeGuard([owner] { restoreOwner(owner); }); settleDesktop();
    if(owner->property("shutdownRequested").toBool()) return;
    PositionPicker picker(owner,true); if(picker.exec()!=QDialog::Accepted) return;
    searchRegion=picker.region(); loading=true; limitRegion->setChecked(true); loading=false; refreshImage(); updateFields(); emit changed();
}
void StepEditor::selectImage(bool fromScreen) {
    QImage image; QString name,error;
    if(fromScreen) {
        auto *owner=window(); owner->setProperty("captureInProgress",true); owner->hide(); auto restore=qScopeGuard([owner] { restoreOwner(owner); }); settleDesktop();
        if(owner->property("shutdownRequested").toBool()) return;
        PositionPicker picker(owner,true); if(picker.exec()!=QDialog::Accepted) return;
        QRect region=picker.region(); bool singleScreen=false; for(const auto &screen:ScreenCapture::monitors()) if(screen.contains(region)) singleScreen=true;
        if(region.width()<8 || region.height()<8 || region.width()>1024 || region.height()>1024) error=QStringLiteral("模板尺寸须为 8～1024 像素");
        else if(!singleScreen) error=QStringLiteral("图像模板需位于同一块屏幕内");
        else { settleDesktop(); if(owner->property("shutdownRequested").toBool()) return; ScreenCapture capture; image=capture.grab(region,error).copy(); name=QStringLiteral("截图 %1 × %2").arg(region.width()).arg(region.height()); }
    } else {
        const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("导入图像模板"),QString(),QStringLiteral("图像 (*.png *.bmp)")); if(path.isEmpty()) return;
        QImageReader reader(path); const QSize size=reader.size();
        if(size.width()<8 || size.height()<8 || size.width()>1024 || size.height()>1024) error=QStringLiteral("模板尺寸须为 8～1024 像素");
        else { image=reader.read(); if(image.isNull()) error=reader.errorString(); name=QFileInfo(path).fileName(); }
    }
    TemplateMatcher check; if(error.isEmpty()) check.prepare(image,false,error);
    if(!error.isEmpty()) { QMessageBox::warning(this,QStringLiteral("图像模板"),error); return; }
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG");
    if(bytes.size()>2*1024*1024) { QMessageBox::warning(this,QStringLiteral("图像模板"),QStringLiteral("模板不能超过 2 MB")); return; }
    templatePng=bytes; templateName=name; refreshImage(); emit changed();
}
void StepEditor::capture(bool position) {
    QWidget *owner=window();
    bool accepted=false; QPoint point; InputKey key;
    if(position) { PositionPicker picker(owner); accepted=picker.exec()==QDialog::Accepted; point=picker.position(); }
    else { CaptureDialog dialog(false,this); accepted=dialog.exec()==QDialog::Accepted; key=dialog.key; }
    if(owner->isVisible()) { owner->raise(); owner->activateWindow(); SetForegroundWindow(reinterpret_cast<HWND>(owner->winId())); }
    if(!accepted) return;
    if(position) { loading=true; x->setValue(point.x()); y->setValue(point.y()); fixed->setChecked(true); loading=false; }
    else { loading=true; setKey(key); loading=false; }
    updateFields(); emit changed();
}

class StepDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &,const QModelIndex &) const override { return {240,86}; }
    void paint(QPainter *p,const QStyleOptionViewItem &o,const QModelIndex &i) const override {
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        QRect r=o.rect.adjusted(4+std::min(6,i.data(Qt::UserRole+2).toInt())*10,2,-4,-10); bool selected=o.state.testFlag(QStyle::State_Selected);
        bool running=i.data(Qt::UserRole+1).toBool();
        p->setPen(QPen(running?QColor("#32a683"):selected?QColor("#6371dc"):QColor("#e4e7ef"),selected?1.5:1));
        p->setBrush(running?QColor("#effaf5"):selected?QColor("#f1f3ff"):Qt::white); p->drawRoundedRect(r,10,10);
        QRect badge(r.left()+12,r.top()+17,36,36); p->setPen(Qt::NoPen); p->setBrush(QColor("#e9ecfb")); p->drawRoundedRect(badge,9,9);
        p->setPen(QColor("#5564c9")); auto f=o.font; f.setBold(true); p->setFont(f); p->drawText(badge,Qt::AlignCenter,QString("%1").arg(i.row()+1,2,10,QChar('0')));
        p->setPen(QColor("#20283c")); p->drawText(QRect(r.left()+61,r.top()+13,r.width()-74,25),Qt::AlignLeft|Qt::AlignVCenter,p->fontMetrics().elidedText(i.data(Qt::DisplayRole).toString(),Qt::ElideRight,r.width()-74));
        f.setBold(false); f.setPointSize(9); p->setFont(f); p->setPen(QColor("#788298"));
        QString text=i.data(Qt::ToolTipRole).toString(); p->drawText(QRect(r.left()+61,r.top()+40,r.width()-74,22),Qt::AlignLeft|Qt::AlignVCenter,p->fontMetrics().elidedText(text,Qt::ElideRight,r.width()-74));
        if(i.row()+1<i.model()->rowCount()) { p->setPen(QPen(QColor("#c3cadb"),1.5)); int c=r.center().x(); p->drawLine(c,r.bottom()+1,c,r.bottom()+9); p->drawLine(c-3,r.bottom()+6,c,r.bottom()+9); p->drawLine(c+3,r.bottom()+6,c,r.bottom()+9); }
        p->restore();
    }
};

QIcon Window::appIcon() {
    QPixmap pm(64,64); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor("#5968dc")); p.setPen(Qt::NoPen); p.drawRoundedRect(2,2,60,60,16,16);
    p.setPen(QPen(Qt::white,3.5,Qt::SolidLine,Qt::RoundCap)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(22,14,21,36,10,10); p.drawLine(32,15,32,25);
    p.drawLine(12,20,16,22); p.drawLine(15,10,19,14); p.drawLine(46,10,43,14); return QIcon(pm);
}
Window::Window(QWidget *parent,bool testing):QMainWindow(parent),engine(sink,this),hotkeys(this),monitor(this),testMode(testing) {
    monitor.setObjectName("recordingMonitor");
    setWindowTitle(QStringLiteral("点序 · 连点器")); setWindowIcon(appIcon()); resize(980,680); setMinimumSize(800,480);
    setStyleSheet(QStringLiteral(R"(
        QWidget { font-family: 'Microsoft YaHei UI', 'Segoe UI'; font-size: 10pt; color: #25304a; }
        QMainWindow { background: #f5f6fa; }
        QLabel#brand { font-size: 22pt; font-weight: 700; color: #253054; }
        QLabel#sectionTitle { font-size: 14pt; font-weight: 600; }
        QLabel#muted { color: #798397; font-size: 9pt; }
        QLabel#status { background: #e8edf6; color: #657089; border-radius: 12px; padding: 6px 16px; }
        QFrame#card { background: white; border: 1px solid #e5e8f0; border-radius: 14px; }
        QPushButton { background: white; border: 1px solid #dce1ec; border-radius: 8px; padding: 9px 14px; }
        QPushButton:hover { background: #f0f2fd; border-color: #a8b2e4; }
        QPushButton:pressed { background: #e4e8fa; }
        QPushButton:disabled { color: #adb4c1; background: #f3f4f7; }
        QPushButton#primary { color: white; background: #5c6bda; border: none; font-weight: 600; padding: 12px 28px; }
        QPushButton#primary:hover { background: #4c5dce; }
        QPushButton#primary:disabled { background: #afb6dc; }
        QPushButton#stop { color: #be5564; background: #fff4f5; border-color: #f0cfd5; padding: 12px 24px; }
        QPushButton#stop:disabled { color: #b9bdc8; background: #f3f4f7; border-color: #e5e8ef; }
        QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit { background: white; border: 1px solid #dce1ec; border-radius: 8px; padding: 7px 10px; min-height: 22px; }
        QComboBox:hover { border-color: #a8b2e4; }
        QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus { border-color: #6675db; }
        QLineEdit:read-only { background: #f8f9fd; }
        QSpinBox, QDoubleSpinBox { padding-right: 31px; selection-background-color: #e8ecff; selection-color: #25304a; }
        QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 26px; height: 18px; border: none; border-top-right-radius: 7px; background: transparent; margin: 2px 2px 0 0; }
        QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 26px; height: 18px; border: none; border-bottom-right-radius: 7px; background: transparent; margin: 0 2px 2px 0; }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: #eef1fc; }
        QSpinBox::up-button:pressed, QSpinBox::down-button:pressed, QDoubleSpinBox::up-button:pressed, QDoubleSpinBox::down-button:pressed { background: #dde3fb; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/icons/chevron-up.png); width: 10px; height: 10px; }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/icons/chevron.png); width: 10px; height: 10px; }
        QSpinBox[coordinate="true"] { padding-right: 10px; }
        QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: center right; border: none; width: 28px; margin: 4px; border-radius: 5px; background: #f2f4fa; }
        QComboBox::drop-down:hover { background: #e8ecfb; }
        QComboBox::down-arrow { image: url(:/icons/chevron.png); width: 12px; height: 12px; }
        QComboBox QAbstractItemView { background: white; border: 1px solid #dce1ec; border-radius: 8px; padding: 5px; outline: none; selection-background-color: #e8ecff; selection-color: #4c5dce; }
        QComboBox QAbstractItemView::item { min-height: 24px; padding: 6px 10px; border-radius: 5px; }
        QComboBox QAbstractItemView::item:hover { background: #f1f3fc; }
        QComboBox QAbstractItemView::item:selected { background: #e8ecff; color: #4c5dce; }
        QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
        QTabWidget::pane { border: none; background: transparent; }
        QTabBar::tab { background: #e9edf5; border: none; margin-right: 6px; border-radius: 8px; padding: 11px 22px; color: #7a849a; }
        QTabBar::tab:selected { background: #5c6bda; color: white; }
        QListWidget { background: transparent; border: none; outline: none; }
        QCheckBox { spacing: 9px; }
        QScrollBar:vertical { background: #f3f4f8; width: 9px; border-radius: 4px; }
        QScrollBar::handle:vertical { background: #cbd1de; border-radius: 4px; min-height: 25px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )"));
    auto *central=new QWidget; setCentralWidget(central); auto *outer=new QHBoxLayout(central); outer->setContentsMargins(24,20,24,18);
    auto *workspace=new QWidget; workspace->setObjectName("workspace"); workspace->setMaximumWidth(1120); workspace->setMaximumHeight(900);
    outer->addStretch(); outer->addWidget(workspace,1); outer->addStretch();
    auto *root=new QVBoxLayout(workspace); root->setContentsMargins(0,0,0,0); root->setSpacing(16);
    auto *header=new QHBoxLayout; header->addWidget(label(QStringLiteral("点序"),"brand")); header->addStretch();
    status=label(QStringLiteral("● 就绪"),"status"); header->addWidget(status); root->addLayout(header);
    tabs=new QTabWidget; tabs->setObjectName("tabs"); root->addWidget(tabs,1);
    quickPage=new QWidget; auto *ql=new QVBoxLayout(quickPage); ql->setContentsMargins(0,22,0,0); ql->setSpacing(15);
    auto *quickContent=new QWidget; auto *quickBody=new QVBoxLayout(quickContent); quickBody->setContentsMargins(0,0,10,0);
    auto *qcard=card(); qcard->setMaximumWidth(780); auto *qcl=new QVBoxLayout(qcard); qcl->setContentsMargins(24,22,24,22); qcl->setSpacing(20);
    qcl->addWidget(label(QStringLiteral("快速任务"),"sectionTitle")); quick=new StepEditor(true); qcl->addWidget(quick);
    auto *quickCenter=new QHBoxLayout; quickCenter->addStretch(); quickCenter->addWidget(qcard,1); quickCenter->addStretch();
    quickBody->addLayout(quickCenter); quickBody->addStretch(); ql->addWidget(makeScroll(quickContent,"quickScroll"),1);
    tabs->addTab(quickPage,QStringLiteral("快速任务"));
    flowPage=new QWidget; auto *fl=new QVBoxLayout(flowPage); fl->setContentsMargins(0,20,0,0); fl->setSpacing(14);
    auto *flowTop=new QHBoxLayout; flowTop->addWidget(label(QStringLiteral("脚本"),"sectionTitle")); flowTop->addStretch();
    auto *load=new QPushButton(QStringLiteral("导入")); auto *save=new QPushButton(QStringLiteral("保存")); recordButton=new QPushButton(QStringLiteral("● 录制输入"));
    flowTop->addWidget(load); flowTop->addWidget(save); flowTop->addWidget(recordButton); fl->addLayout(flowTop);
    auto *flowBody=new QHBoxLayout; auto *listCard=card(); auto *ll=new QVBoxLayout(listCard); ll->setContentsMargins(14,16,14,12);
    flow=new QListWidget; flow->setObjectName("flow"); flow->setItemDelegate(new StepDelegate(flow)); flow->setDragDropMode(QAbstractItemView::InternalMove);
    flow->setDefaultDropAction(Qt::MoveAction); flow->setSelectionMode(QAbstractItemView::SingleSelection); ll->addWidget(flow,1);
    for(auto signal:{&QAbstractItemModel::rowsInserted,&QAbstractItemModel::rowsRemoved}) connect(flow->model(),signal,this,[this] { scheduleIndent(); });
    connect(flow->model(),&QAbstractItemModel::rowsMoved,this,[this] { scheduleIndent(); });
    connect(flow->model(),&QAbstractItemModel::dataChanged,this,[this](const QModelIndex &,const QModelIndex &,const QList<int> &roles) { if(roles.isEmpty() || roles.contains(Qt::UserRole)) scheduleIndent(); });
    auto *addCombo=new ComboBox; addCombo->setObjectName("addAction"); for(int i=0;i<ActionCount;++i) addCombo->addItem(actionName(Action(i)),i);
    auto *add=new QPushButton(QStringLiteral("＋ 添加")); auto *remove=new QPushButton(QStringLiteral("删除")); auto *duplicate=new QPushButton(QStringLiteral("复制"));
    ll->addWidget(row({addCombo,add,duplicate,remove})); flowBody->addWidget(listCard,3);
    auto *editContent=new QWidget; auto *editBody=new QVBoxLayout(editContent); editBody->setContentsMargins(0,0,10,0);
    auto *editCard=card(); auto *el=new QVBoxLayout(editCard); el->setContentsMargins(20,20,20,20); el->setSpacing(18);
    el->addWidget(label(QStringLiteral("步骤属性"),"sectionTitle")); editor=new StepEditor(false); el->addWidget(editor); el->addStretch();
    editBody->addWidget(editCard); editBody->addStretch(); auto *stepScroll=makeScroll(editContent,"stepScroll"); stepScroll->setMinimumWidth(380);
    flowBody->addWidget(stepScroll,2); fl->addLayout(flowBody,1); tabs->addTab(flowPage,QStringLiteral("编排脚本"));
    auto *settings=new QWidget; auto *sl=new QVBoxLayout(settings); sl->setContentsMargins(0,22,0,0); sl->setSpacing(18);
    auto *scard=card(); scard->setMaximumWidth(780); auto *scl=new QVBoxLayout(scard); scl->setContentsMargins(24,22,24,24); scl->setSpacing(16);
    scl->addWidget(label(QStringLiteral("全局快捷键"),"sectionTitle")); auto *sf=new QFormLayout;
    sf->setVerticalSpacing(16); sf->setLabelAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    toggleShortcut=new ShortcutField(QKeySequence(Qt::Key_F6)); stopShortcut=new ShortcutField(QKeySequence(Qt::Key_F8)); recordShortcut=new ShortcutField(QKeySequence(Qt::Key_F7));
    toggleShortcut->setObjectName("toggleShortcut"); stopShortcut->setObjectName("stopShortcut"); recordShortcut->setObjectName("recordShortcut");
    auto shortcutRow=[this](ShortcutField *field,int defaultKey) {
        auto *reset=new QPushButton(QStringLiteral("重置")); reset->setObjectName(field->objectName()+"Reset"); reset->setFocusPolicy(Qt::NoFocus);
        connect(reset,&QPushButton::clicked,this,[field,defaultKey] { field->clearFocus(); field->setKeySequence(QKeySequence(defaultKey)); });
        connect(field,&ShortcutField::editingChanged,this,[this](bool editing) { QString error; if(!hotkeys.setPaused(editing,error)) notice(error,true); });
        connect(field,&ShortcutField::keySequenceChanged,this,[this] {
            QString error;
            if(hotkeys.set(toggleShortcut->keySequence(),stopShortcut->keySequence(),recordShortcut->keySequence(),error)) { settingsSave(); shortcutHint(); }
            else {
                QSignalBlocker a(toggleShortcut),b(stopShortcut),c(recordShortcut);
                toggleShortcut->setKeySequence(hotkeys.sequence(1)); stopShortcut->setKeySequence(hotkeys.sequence(2)); recordShortcut->setKeySequence(hotkeys.sequence(3)); notice(error,true);
            }
        });
        return row({field,reset});
    };
    sf->addRow(QStringLiteral("启动 / 停止任务"),shortcutRow(toggleShortcut,Qt::Key_F6)); sf->addRow(QStringLiteral("立即停止"),shortcutRow(stopShortcut,Qt::Key_F8)); sf->addRow(QStringLiteral("开始 / 结束录制"),shortcutRow(recordShortcut,Qt::Key_F7)); scl->addLayout(sf);
    minimizeOnStart=new QCheckBox(QStringLiteral("启动任务时自动最小化")); scl->addWidget(minimizeOnStart);
    auto *settingsContent=new QWidget; auto *settingsBody=new QVBoxLayout(settingsContent); settingsBody->setContentsMargins(0,0,10,0);
    auto *settingsCenter=new QHBoxLayout; settingsCenter->addStretch(); settingsCenter->addWidget(scard,1); settingsCenter->addStretch(); settingsBody->addLayout(settingsCenter); settingsBody->addStretch();
    sl->addWidget(makeScroll(settingsContent,"settingsScroll")); tabs->addTab(settings,QStringLiteral("设置"));
    auto *options=new QHBoxLayout; delay=new TimeField(0,60000,2000); delay->setObjectName("startDelay"); delay->setFixedWidth(235);
    rounds=spin(0,1000000,1); rounds->setObjectName("scriptRounds"); rounds->setSpecialValueText(QStringLiteral("无限")); options->addWidget(label(QStringLiteral("启动延迟"))); options->addWidget(delay);
    roundsRow=row({label(QStringLiteral("脚本循环")),rounds}); options->addSpacing(15); options->addWidget(roundsRow); options->addStretch(); stats=label(QStringLiteral("等待开始"),"muted"); options->addWidget(stats); root->addLayout(options);
    auto *footer=new QHBoxLayout; message=label(QStringLiteral("F6 启停 · F8 停止 · F7 录制"),"muted"); footer->addWidget(message,1);
    startButton=new QPushButton(QStringLiteral("▶  开始任务")); startButton->setObjectName("primary"); stopButton=new QPushButton(QStringLiteral("■  停止")); stopButton->setObjectName("stop"); stopButton->setEnabled(false);
    logButton=new QPushButton(QStringLiteral("上次执行日志")); logButton->setObjectName("lastExecutionLog"); logButton->setEnabled(false);
    footer->addWidget(logButton); footer->addWidget(startButton); footer->addWidget(stopButton); root->addLayout(footer);
    connect(logButton,&QPushButton::clicked,this,&Window::showExecutionLog);
    connect(&engine,&Engine::executionLogged,this,[this] {
        logButton->setEnabled(engine.lastExecutionLog().valid()); saveExecutionLog();
        if(logDialog) logDialog->setLog(engine.lastExecutionLog());
    });
    connect(startButton,&QPushButton::clicked,this,&Window::start);
    connect(stopButton,&QPushButton::clicked,this,[this] { endRecording(); engine.stop(); notice(QStringLiteral("已停止")); });
    connect(&engine,&Engine::stateChanged,this,&Window::updateState);
    connect(&engine,&Engine::failed,this,[this](const QString &s) { notice(s,true); });
    connect(&engine,&Engine::matchEvaluated,this,[this](int step,const MatchResult &r) {
        notice(QStringLiteral("步骤 %1 · %2 · 截图 %3 ms · 识别 %4 ms").arg(step+1).arg(r.found?QStringLiteral("找到图像"):QStringLiteral("未找到图像")).arg(r.captureMicros/1000.0,0,'f',2).arg(r.matchMicros/1000.0,0,'f',2));
    });
    connect(&engine,&Engine::finished,this,[this] { notice(QStringLiteral("任务完成")); stats->setText(QStringLiteral("完成 · %1 次动作").arg(engine.eventCount())); });
    connect(&engine,&Engine::progress,this,[this](int step,int round,quint64 count) {
        stats->setText(step<0?QStringLiteral("启动延迟中…"):QStringLiteral("第 %1 轮 · 步骤 %2 · %3 次动作").arg(round+1).arg(step+1).arg(count));
        if(tabs->currentIndex()==1 && step!=currentRunningStep) {
            if(currentRunningStep>=0 && currentRunningStep<flow->count()) flow->item(currentRunningStep)->setData(Qt::UserRole+1,false);
            currentRunningStep=step;
            if(step>=0 && step<flow->count()) { flow->item(step)->setData(Qt::UserRole+1,true); flow->scrollToItem(flow->item(step)); }
        }
    });
    connect(flow,&QListWidget::currentItemChanged,this,[this](QListWidgetItem *item) {
        editor->setEnabled(item && !recording && !engine.running());
        if(item) { Step s; QString error; if(Step::parse(item->data(Qt::UserRole).toJsonObject(),s,error)) editor->setValue(s); }
    });
    connect(editor,&StepEditor::changed,this,[this] { if(auto *item=flow->currentItem()) refreshItem(item,editor->value()); });
    connect(add,&QPushButton::clicked,this,[this,addCombo] { addStep(Action(addCombo->currentData().toInt())); });
    connect(remove,&QPushButton::clicked,this,[this] { delete flow->takeItem(flow->currentRow()); });
    connect(duplicate,&QPushButton::clicked,this,[this] { if(auto *item=flow->currentItem()) { auto *copy=item->clone(); flow->insertItem(flow->currentRow()+1,copy); flow->setCurrentItem(copy); } });
    connect(load,&QPushButton::clicked,this,[this] { auto file=QFileDialog::getOpenFileName(this,QStringLiteral("导入脚本"),scriptPath,QStringLiteral("点序脚本 (*.json)")); if(!file.isEmpty()) loadScript(file); });
    connect(save,&QPushButton::clicked,this,&Window::saveScript); connect(recordButton,&QPushButton::clicked,this,&Window::record);
    connect(&monitor,&InputMonitor::observed,this,&Window::appendRecorded,Qt::QueuedConnection);
    connect(&hotkeys,&Hotkeys::triggered,this,[this](int id) {
        if(QApplication::activeModalWidget()) return;
        if(id==2) { endRecording(); engine.stop(); notice(QStringLiteral("已立即停止")); }
        else if(id==3) record();
        else if(recording) endRecording();
        else if(engine.running()) engine.stop(); else start();
    });
    connect(tabs,&QTabWidget::currentChanged,this,[this](int index) { roundsRow->setVisible(index==1); rounds->setEnabled(index==1 && !engine.running() && !recording); });
    Step initial; initial.action=Action::Repeat; initial.count=0; quick->setValue(initial);
    Step first; first.action=Action::Click; refreshItem(new QListWidgetItem(flow),first);
    Step wait; wait.action=Action::Wait; wait.duration=500; refreshItem(new QListWidgetItem(flow),wait); flow->setCurrentRow(0);
    roundsRow->hide(); rounds->setEnabled(false); if(!testMode) settingsLoad();
}
Window::~Window() { shutdown(); }
void Window::showExecutionLog() {
    if(!engine.lastExecutionLog().valid()) return;
    if(!logDialog) logDialog=new ExecutionLogDialog(this);
    logDialog->setLog(engine.lastExecutionLog()); logDialog->show(); logDialog->raise(); logDialog->activateWindow();
}
void Window::saveExecutionLog() {
    if(testMode) return;
    QSettings st; st.setValue("lastExecutionLog",engine.lastExecutionLog().save()); st.sync();
}
void Window::notice(const QString &s,bool error) { message->setText(s); message->setStyleSheet(error?"color:#bd4c5b;":"color:#798397;"); message->setToolTip(s); }
void Window::shortcutHint() {
    QStringList parts;
    for(const auto &pair:QList<QPair<int,QString>>{{1,QStringLiteral("启停")},{2,QStringLiteral("停止")},{3,QStringLiteral("录制")}})
        if(!hotkeys.sequence(pair.first).isEmpty()) parts.append(hotkeys.sequence(pair.first).toString(QKeySequence::NativeText)+" "+pair.second);
    notice(parts.join(QStringLiteral(" · ")));
}
void Window::refreshItem(QListWidgetItem *item,const Step &s) { item->setText(s.title()); item->setToolTip(s.detail()); item->setData(Qt::UserRole,s.json()); }
void Window::scheduleIndent() {
    if(indentPending) return; indentPending=true;
    QTimer::singleShot(0,this,[this] {
        indentPending=false; int depth=0;
        for(int i=0;i<flow->count();++i) {
            auto *item=flow->item(i); const auto a=item->data(Qt::UserRole).toJsonObject().value("action").toString();
            if(a=="endIf" || a=="endLoop" || a=="else") depth=std::max(0,depth-1);
            item->setData(Qt::UserRole+2,depth);
            if(a=="ifImage" || a=="loop" || a=="else") ++depth;
        }
    });
}
void Window::addStep(Action a) {
    if(flow->count()+(a==Action::IfImage?3:1)>10000) { notice(QStringLiteral("步骤最多 10000 个"),true); return; }
    int at=flow->currentRow()+1;
    if(a==Action::Else) {
        QList<int> conditions;
        for(int i=0;i<=flow->currentRow();++i) {
            const auto id=flow->item(i)->data(Qt::UserRole).toJsonObject().value("action").toString();
            if(id=="ifImage") conditions.append(i);
            else if(id=="endIf" && i<flow->currentRow() && !conditions.isEmpty()) conditions.removeLast();
        }
        if(conditions.isEmpty()) { notice(QStringLiteral("请选择一个图像条件或分支内的步骤"),true); return; }
        int depth=0; at=-1;
        for(int i=conditions.last()+1;i<flow->count();++i) {
            const auto id=flow->item(i)->data(Qt::UserRole).toJsonObject().value("action").toString();
            if(id=="ifImage") ++depth;
            else if(id=="else" && depth==0) { notice(QStringLiteral("这个条件已有否则分支"),true); return; }
            else if(id=="endIf") { if(depth==0) { at=i; break; } --depth; }
        }
        if(at<0) { notice(QStringLiteral("请先补齐条件的结束步骤"),true); return; }
    }
    Step s; s.action=a; auto *item=new QListWidgetItem; refreshItem(item,s); flow->insertItem(at,item);
    if(a==Action::IfImage) { for(auto child:{Action::ClickMatch,Action::EndIf}) { Step step; step.action=child; auto *next=new QListWidgetItem; refreshItem(next,step); flow->insertItem(++at,next); } }
    flow->setCurrentItem(item);
}
Script Window::currentScript() const {
    Script s; s.startDelay=delay->value(); s.rounds=rounds->value();
    for(int i=0;i<flow->count();++i) { Step step; QString error; Step::parse(flow->item(i)->data(Qt::UserRole).toJsonObject(),step,error); s.steps.append(step); }
    return s;
}
void Window::setScript(const Script &s) {
    flow->clear(); for(const auto &step:s.steps) refreshItem(new QListWidgetItem(flow),step);
    delay->setValue(s.startDelay); rounds->setValue(s.rounds); flow->setCurrentRow(0); tabs->setCurrentIndex(1);
}
bool Window::loadScript(const QString &path,bool quiet) {
    if(engine.running() || recording) { notice(QStringLiteral("请先停止再导入"),true); return false; }
    QFile file(path); Script s; QString error;
    if(!file.open(QIODevice::ReadOnly) || file.size()>4*1024*1024) error=QStringLiteral("无法读取脚本或脚本超过 4 MB");
    else if(Script::parse(file.readAll(),s,error)) { setScript(s); scriptPath=path; notice(QStringLiteral("已导入：%1").arg(QFileInfo(path).fileName())); return true; }
    if(!quiet) notice(error,true); return false;
}
void Window::saveScript() {
    auto s=currentScript(); QString error; if(!s.validate(error)) { notice(error,true); return; }
    auto path=QFileDialog::getSaveFileName(this,QStringLiteral("保存脚本"),scriptPath.isEmpty()?"script.json":scriptPath,QStringLiteral("点序脚本 (*.json)"));
    if(path.isEmpty()) return; if(!path.endsWith(".json",Qt::CaseInsensitive)) path+=".json";
    QSaveFile f(path); if(!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(s.json()).toJson())<0 || !f.commit()) { notice(QStringLiteral("脚本保存失败"),true); return; }
    scriptPath=path; notice(QStringLiteral("脚本已保存"));
}
void Window::start() {
    if(closing || property("captureInProgress").toBool()) return;
    if(recording || engine.running()) return;
    if(QApplication::activeModalWidget()) { notice(QStringLiteral("请先完成当前弹窗操作"),true); return; }
    Script s;
    if(tabs->currentIndex()==1) s=currentScript();
    else { s.steps={quick->value()}; s.startDelay=delay->value(); s.rounds=1; s.latch=s.steps[0].action==Action::Down; }
    // Avoid self-triggering global shortcuts through injected input, regardless of modifiers.
    for(const auto &step:s.steps) if(isInputAction(step.action) && !step.key.mouse) {
        for(int id:{1,2,3}) { UINT m,v; if(Hotkeys::decode(hotkeys.sequence(id),m,v) && int(v)==step.key.code) { notice(QStringLiteral("目标按键与快捷键冲突，请先修改快捷键"),true); return; } }
    }
    QString error; if(!engine.start(s,error)) { notice(error,true); return; }
    notice(QStringLiteral("运行中"));
    if(minimizeOnStart->isChecked()) showMinimized();
}
void Window::updateState(bool running) {
    status->setText(running?QStringLiteral("● 运行中"):QStringLiteral("● 就绪")); status->setStyleSheet(running?"background:#e7f6ee;color:#258763;border-radius:12px;padding:6px 16px;":"");
    startButton->setEnabled(!running && !recording); stopButton->setEnabled(running || recording || engine.heldCount()>0);
    quickPage->setEnabled(!running && !recording); flowPage->setEnabled(!running); tabs->tabBar()->setEnabled(!running);
    delay->setEnabled(!running && !recording); rounds->setEnabled(!running && !recording && tabs->currentIndex()==1);
    if(!running && currentRunningStep>=0 && currentRunningStep<flow->count()) flow->item(currentRunningStep)->setData(Qt::UserRole+1,false);
    if(!running) currentRunningStep=-1;
}
void Window::record() {
    if(closing || property("captureInProgress").toBool()) return;
    if(recording) { endRecording(); return; }
    if(QApplication::activeModalWidget()) { notice(QStringLiteral("请先完成当前弹窗操作"),true); return; }
    if(engine.running()) { notice(QStringLiteral("请先停止任务再录制"),true); return; }
    tabs->setCurrentIndex(1);
    QString error; if(!monitor.start(error)) { notice(error,true); return; }
    recording=true; lastRecordTime=0; recordingHeld.clear(); recordButton->setText(QStringLiteral("■ 结束录制"));
    tabs->tabBar()->setEnabled(false);
    editor->setEnabled(false); flow->setEnabled(false); startButton->setEnabled(false); stopButton->setEnabled(true); quickPage->setEnabled(false); delay->setEnabled(false); rounds->setEnabled(false);
    // Keep only the recording button actionable in the flow toolbar.
    for(auto *b:flowPage->findChildren<QPushButton *>()) if(b!=recordButton) b->setEnabled(false);
    status->setText(QStringLiteral("● 录制中")); notice(QStringLiteral("录制中"));
}
void Window::appendRecorded(InputKey key,bool down,QPoint position,quint64 time) {
    if(!recording) return;
    if(!key.mouse) {
        for(int id:{1,2,3}) { UINT m,v; if(Hotkeys::decode(hotkeys.sequence(id),m,v) && int(v)==key.code) return; }
    } else {
        HWND under=WindowFromPoint(POINT{position.x(),position.y()});
        // An already recorded press still needs its release when a drag ends
        // over our window. Filtering that release loses later clicks too.
        if(down && GetAncestor(under,GA_ROOT)==reinterpret_cast<HWND>(winId())) return;
    }
    QString id=key.identity();
    if(down && recordingHeld.contains(id)) return; // Drop hardware autorepeat; Down/Up preserves the hold duration.
    if(!down && !recordingHeld.contains(id)) return;
    if(flow->count()>=9997) { endRecording(); notice(QStringLiteral("录制已达到步骤上限"),true); return; }
    if(lastRecordTime && time>lastRecordTime) { Step wait; wait.action=Action::Wait; wait.duration=int(qMin<quint64>(time-lastRecordTime,86400000)); refreshItem(new QListWidgetItem(flow),wait); }
    Step s; s.action=down?Action::Down:Action::Up; s.key=key; s.fixedPosition=key.mouse; s.position=position; refreshItem(new QListWidgetItem(flow),s);
    if(down) recordingHeld.insert(id,key); else recordingHeld.remove(id); lastRecordTime=time;
    stats->setText(QStringLiteral("已记录 %1 步").arg(flow->count())); flow->scrollToBottom();
}
void Window::endRecording() {
    if(!recording) return; recording=false; monitor.stop();
    // Balance keys still held when recording was stopped; the recorder never injects input.
    for(const auto &key:recordingHeld.values()) { if(flow->count()>=10000) break; Step s; s.action=Action::Up; s.key=key; refreshItem(new QListWidgetItem(flow),s); }
    recordingHeld.clear(); recordButton->setText(QStringLiteral("● 录制输入"));
    for(auto *b:flowPage->findChildren<QPushButton *>()) b->setEnabled(true);
    flow->setEnabled(true); editor->setEnabled(flow->currentItem()!=nullptr); updateState(false); notice(QStringLiteral("录制完成"));
}
void Window::settingsLoad() {
    QSettings st; restoreGeometry(st.value("geometry").toByteArray());
    engine.restoreExecutionLog(ExecutionLog::load(st.value("lastExecutionLog").toByteArray())); logButton->setEnabled(engine.lastExecutionLog().valid());
    Script saved; QString error;
    if(Script::parse(st.value("flow").toByteArray(),saved,error,true)) setScript(saved);
    Step q; if(Step::parse(QJsonDocument::fromJson(st.value("quick").toByteArray()).object(),q,error) && int(q.action)<5) quick->setValue(q);
    delay->setValue(st.value("delay",2000).toInt()); minimizeOnStart->setChecked(st.value("minimize",false).toBool());
    QSignalBlocker a(toggleShortcut),b(stopShortcut),c(recordShortcut);
    auto savedShortcut=[&st](const char *name,const char *fallback) {
        QKeySequence sequence(st.value(name,fallback).toString()); UINT m,v;
        if(!sequence.isEmpty() && !Hotkeys::decode(sequence,m,v)) return QKeySequence();
        return sequence;
    };
    toggleShortcut->setKeySequence(savedShortcut("toggle","F6")); stopShortcut->setKeySequence(savedShortcut("stop","F8")); recordShortcut->setKeySequence(savedShortcut("record","F7"));
    if(!hotkeys.set(toggleShortcut->keySequence(),stopShortcut->keySequence(),recordShortcut->keySequence(),error)) notice(error,true); else shortcutHint();
    tabs->setCurrentIndex(qBound(0,st.value("tab",0).toInt(),2));
}
void Window::settingsSave() {
    if(testMode) return;
    QSettings st; st.setValue("geometry",saveGeometry()); st.setValue("flow",QJsonDocument(currentScript().json()).toJson(QJsonDocument::Compact));
    st.setValue("quick",QJsonDocument(quick->value().json()).toJson(QJsonDocument::Compact)); st.setValue("delay",delay->value()); st.setValue("minimize",minimizeOnStart->isChecked()); st.setValue("tab",tabs->currentIndex());
    // A temporary registration conflict at startup must not erase the user's
    // configured bindings when closing. Invalid edits already restore fields.
    for(const auto &pair:QList<QPair<ShortcutField *,QString>>{{toggleShortcut,"toggle"},{stopShortcut,"stop"},{recordShortcut,"record"}}) {
        st.setValue(pair.second,pair.first->keySequence().toString(QKeySequence::PortableText));
    }
    st.sync();
}
void Window::shutdown() { if(closing) return; closing=true; setProperty("shutdownRequested",true); if(logDialog) { logDialog->close(); logDialog=nullptr; } endRecording(); engine.stop(); monitor.stop(); settingsSave(); hotkeys.clear(); }
void Window::closeEvent(QCloseEvent *event) { shutdown(); event->accept(); }
void Window::showWindow() { if(closing || property("captureInProgress").toBool()) return; showNormal(); raise(); activateWindow(); }
void Window::toggleTask() { if(recording) endRecording(); else if(engine.running()) engine.stop(); else start(); }
void Window::stopTask() { endRecording(); engine.stop(); }
