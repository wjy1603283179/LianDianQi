#include "window.h"
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
#include <QTabBar>
#include <QVBoxLayout>

static QLabel *label(const QString &s, const char *name=nullptr) {
    auto *w=new QLabel(s); w->setWordWrap(true); if(name) w->setObjectName(name); return w;
}
static QSpinBox *spin(int low, int high, int value, const QString &suffix={}) {
    auto *w=new QSpinBox; w->setRange(low,high); w->setValue(value); w->setSuffix(suffix); w->setMinimumWidth(115); w->setButtonSymbols(QAbstractSpinBox::PlusMinus); return w;
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

class CaptureDialog : public QDialog {
public:
    CaptureDialog(bool position, QWidget *parent) : QDialog(parent) {
        setWindowTitle(position?QStringLiteral("拾取屏幕坐标"):QStringLiteral("采集输入")); setMinimumWidth(440);
        auto *l=new QVBoxLayout(this); l->setContentsMargins(28,24,28,24); l->setSpacing(18);
        l->addWidget(label(position?QStringLiteral("点击需要执行的位置"):QStringLiteral("按下任意键或鼠标按钮"),"sectionTitle"));
        l->addWidget(label(position?QStringLiteral("可以点击其他窗口。将记录屏幕物理坐标，支持多屏和负坐标。"):
            QStringLiteral("可采集左右 Ctrl / Shift / Alt、小键盘、功能键及鼠标按钮。输入会正常传递到当前窗口。"),"muted"));
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
    auto *form=new QFormLayout(this); form->setContentsMargins(0,0,0,0); form->setVerticalSpacing(16);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    action=new QComboBox;
    for(int i=0;i<(isQuick?5:9);++i) action->addItem(actionName(Action(i)),i);
    action->setObjectName(isQuick?"quickAction":"stepAction");
    form->addRow(QStringLiteral("执行动作"),action);
    input=new QComboBox; input->setObjectName(isQuick?"quickInput":"stepInput");
    for(int code:{1,2,4,5,6}) { InputKey k{true,code,0,false}; input->addItem(k.name(),k.json()); }
    for(int code='A';code<='Z';++code) { InputKey k{false,code,0,false}; input->addItem(k.name(),k.json()); }
    for(int code='0';code<='9';++code) { InputKey k{false,code,0,false}; input->addItem(k.name(),k.json()); }
    for(int code=8;code<=254;++code) {
        if((code>='A'&&code<='Z') || (code>='0'&&code<='9') || code==16 || code==17 || code==18) continue;
        InputKey k{false,code,0,false}; input->addItem(k.name(),k.json());
    }
    captureKey=new QPushButton(QStringLiteral("采集")); captureKey->setToolTip(QStringLiteral("采集实际键盘或鼠标输入"));
    inputRow=row({input,captureKey}); form->addRow(QStringLiteral("输入按键"),inputRow);
    interval=spin(5,3600000,100,QStringLiteral(" ms")); interval->setObjectName(isQuick?"quickInterval":"stepInterval");
    intervalRow=row({interval,label(QStringLiteral("两次连点的间隔"),"muted")}); form->addRow(QStringLiteral("连点间隔"),intervalRow);
    count=spin(0,1000000,0); count->setSpecialValueText(QStringLiteral("持续 / 无限"));
    countRow=row({count}); form->addRow(QStringLiteral("重复次数"),countRow);
    duration=spin(0,86400000,1000,QStringLiteral(" ms")); duration->setSpecialValueText(QStringLiteral("直到停止"));
    durationRow=row({duration}); form->addRow(QStringLiteral("持续时间"),durationRow);
    fixed=new QCheckBox(QStringLiteral("在指定坐标执行鼠标动作")); form->addRow(QString(),fixed);
    x=spin(-100000,100000,0); y=spin(-100000,100000,0); capturePosition=new QPushButton(QStringLiteral("拾取"));
    x->setMinimumWidth(78); y->setMinimumWidth(78);
    positionRow=new QWidget; auto *pl=new QVBoxLayout(positionRow); pl->setContentsMargins(0,0,0,0); pl->setSpacing(8);
    pl->addWidget(row({label("X"),x,label("Y"),y})); pl->addWidget(capturePosition,0,Qt::AlignLeft); form->addRow(QStringLiteral("屏幕坐标"),positionRow);
    connect(action,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this] { updateFields(); if(!loading) emit changed(); });
    connect(input,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this] {
        QString error; InputKey k; if(InputKey::parse(input->currentData().toJsonObject(),k,error)) currentKey=k;
        updateFields(); if(!loading) emit changed();
    });
    for(auto *w:{interval,duration,count,x,y}) connect(w,QOverload<int>::of(&QSpinBox::valueChanged),this,[this] { if(!loading) emit changed(); });
    connect(fixed,&QCheckBox::toggled,this,[this] { updateFields(); if(!loading) emit changed(); });
    connect(captureKey,&QPushButton::clicked,this,[this] { capture(false); });
    connect(capturePosition,&QPushButton::clicked,this,[this] { capture(true); });
    updateFields();
}
Step StepEditor::value() const {
    Step s; s.action=Action(action->currentData().toInt()); s.key=currentKey; s.interval=interval->value(); s.duration=duration->value();
    s.count=count->value(); s.fixedPosition=fixed->isChecked(); s.position=QPoint(x->value(),y->value()); return s;
}
void StepEditor::setKey(const InputKey &k) {
    int found=-1;
    for(int i=0;i<input->count();++i) {
        QString error; InputKey item;
        if(InputKey::parse(input->itemData(i).toJsonObject(),item,error) && item.identity()==k.identity()) { found=i; break; }
    }
    if(found<0) { input->addItem(k.name(),k.json()); found=input->count()-1; }
    else input->setItemData(found,k.json());
    input->setCurrentIndex(found); currentKey=k;
}
void StepEditor::setValue(const Step &s) {
    loading=true; action->setCurrentIndex(int(s.action)); setKey(s.key); interval->setValue(s.interval); duration->setValue(s.duration);
    count->setValue(s.count); fixed->setChecked(s.fixedPosition); x->setValue(s.position.x()); y->setValue(s.position.y()); loading=false; updateFields();
}
void StepEditor::updateFields() {
    auto a=Action(action->currentData().toInt()); auto *form=qobject_cast<QFormLayout *>(layout());
    auto visible=[form](QWidget *w,bool show) { w->setVisible(show); if(auto *l=form->labelForField(w)) l->setVisible(show); };
    visible(inputRow,isInputAction(a)); visible(intervalRow,a==Action::Repeat); visible(countRow,a==Action::Repeat || a==Action::LoopBegin);
    visible(durationRow,a==Action::Hold || a==Action::Wait);
    duration->setSpecialValueText(a==Action::Wait?QStringLiteral("0 ms"):QStringLiteral("直到停止"));
    bool mouseInput=isInputAction(a) && currentKey.mouse;
    fixed->setVisible(mouseInput); visible(positionRow,a==Action::Move || (mouseInput && fixed->isChecked()));
}
void StepEditor::capture(bool position) {
    CaptureDialog d(position,this);
    if(d.exec()!=QDialog::Accepted) return;
    if(position) { loading=true; x->setValue(d.point.x()); y->setValue(d.point.y()); fixed->setChecked(true); loading=false; }
    else { loading=true; setKey(d.key); loading=false; }
    updateFields(); emit changed();
}

class StepDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &,const QModelIndex &) const override { return {360,86}; }
    void paint(QPainter *p,const QStyleOptionViewItem &o,const QModelIndex &i) const override {
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        QRect r=o.rect.adjusted(4,2,-4,-10); bool selected=o.state.testFlag(QStyle::State_Selected);
        bool running=i.data(Qt::UserRole+1).toBool();
        p->setPen(QPen(running?QColor("#32a683"):selected?QColor("#6371dc"):QColor("#e4e7ef"),selected?1.5:1));
        p->setBrush(running?QColor("#effaf5"):selected?QColor("#f1f3ff"):Qt::white); p->drawRoundedRect(r,10,10);
        QRect badge(r.left()+12,r.top()+17,36,36); p->setPen(Qt::NoPen); p->setBrush(QColor("#e9ecfb")); p->drawRoundedRect(badge,9,9);
        p->setPen(QColor("#5564c9")); auto f=o.font; f.setBold(true); p->setFont(f); p->drawText(badge,Qt::AlignCenter,QString("%1").arg(i.row()+1,2,10,QChar('0')));
        p->setPen(QColor("#20283c")); p->drawText(QRect(r.left()+61,r.top()+13,r.width()-74,25),Qt::AlignLeft|Qt::AlignVCenter,i.data(Qt::DisplayRole).toString());
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
    setWindowTitle(QStringLiteral("点序 · 连点器")); setWindowIcon(appIcon()); resize(1060,780); setMinimumSize(960,720);
    setStyleSheet(QStringLiteral(R"(
        QWidget { font-family: 'Microsoft YaHei UI', 'Segoe UI'; font-size: 10pt; color: #25304a; }
        QMainWindow { background: #f5f6fa; }
        QLabel#brand { font-size: 22pt; font-weight: 700; color: #253054; }
        QLabel#sectionTitle { font-size: 14pt; font-weight: 600; }
        QLabel#hero { font-size: 22pt; font-weight: 600; }
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
        QComboBox, QSpinBox, QKeySequenceEdit { background: white; border: 1px solid #dce1ec; border-radius: 7px; padding: 7px 10px; min-height: 22px; }
        QComboBox:focus, QSpinBox:focus, QKeySequenceEdit:focus { border-color: #6675db; }
        QComboBox::drop-down { border: none; width: 23px; }
        QComboBox::down-arrow { image: url(:/icons/chevron.png); width: 12px; height: 12px; }
        QComboBox QAbstractItemView { background: white; selection-background-color: #e8ecff; selection-color: #25304a; }
        QTabWidget::pane { border: none; background: transparent; }
        QTabBar::tab { background: #e9edf5; border: none; margin-right: 6px; border-radius: 8px; padding: 11px 22px; color: #7a849a; }
        QTabBar::tab:selected { background: #5c6bda; color: white; }
        QListWidget { background: transparent; border: none; outline: none; }
        QCheckBox { spacing: 9px; }
        QScrollBar:vertical { background: #f3f4f8; width: 9px; border-radius: 4px; }
        QScrollBar::handle:vertical { background: #cbd1de; border-radius: 4px; min-height: 25px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )"));
    auto *central=new QWidget; setCentralWidget(central); auto *root=new QVBoxLayout(central); root->setContentsMargins(28,22,28,20); root->setSpacing(18);
    auto *header=new QHBoxLayout; header->addWidget(label(QStringLiteral("点序"),"brand")); header->addWidget(label(QStringLiteral("LIANDIANQI\n轻量输入自动化"),"muted")); header->addStretch();
    status=label(QStringLiteral("● 就绪"),"status"); header->addWidget(status); root->addLayout(header);
    tabs=new QTabWidget; tabs->setObjectName("tabs"); root->addWidget(tabs,1);
    quickPage=new QWidget; auto *ql=new QVBoxLayout(quickPage); ql->setContentsMargins(0,22,0,0); ql->setSpacing(15);
    ql->addWidget(label(QStringLiteral("一个按键，交给点序。"),"hero"));
    ql->addWidget(label(QStringLiteral("选择鼠标或键盘输入，设置动作，按快捷键即可启停。"),"muted"));
    auto *quickBody=new QHBoxLayout; auto *qcard=card(); auto *qcl=new QVBoxLayout(qcard); qcl->setContentsMargins(24,22,24,22); qcl->setSpacing(20);
    qcl->addWidget(label(QStringLiteral("快速任务"),"sectionTitle")); quick=new StepEditor(true); qcl->addWidget(quick); qcl->addStretch(); quickBody->addWidget(qcard,3);
    auto *guide=card(); auto *gl=new QVBoxLayout(guide); gl->setContentsMargins(22,24,22,24); gl->setSpacing(20);
    gl->addWidget(label(QStringLiteral("轻松开始"),"sectionTitle"));
    gl->addWidget(label(QStringLiteral("01  选择输入\n\n可以直接选键，也可以点「采集」读取真实输入。"),"muted"));
    gl->addWidget(label(QStringLiteral("02  设置动作\n\n0 次表示持续连点；长按 0 ms 表示保持到停止。"),"muted"));
    gl->addWidget(label(QStringLiteral("03  切到目标窗口\n\n启动延迟为你留出切换时间。鼠标默认跟随当前位置。"),"muted"));
    gl->addStretch(); gl->addWidget(label(QStringLiteral("点 × 会停止任务、释放按键并退出程序。"),"muted")); quickBody->addWidget(guide,2); ql->addLayout(quickBody,1);
    tabs->addTab(quickPage,QStringLiteral("快速任务"));
    flowPage=new QWidget; auto *fl=new QVBoxLayout(flowPage); fl->setContentsMargins(0,20,0,0); fl->setSpacing(14);
    auto *flowTop=new QHBoxLayout; flowTop->addWidget(label(QStringLiteral("把动作串成一条流程"),"sectionTitle")); flowTop->addStretch();
    auto *load=new QPushButton(QStringLiteral("导入")); auto *save=new QPushButton(QStringLiteral("保存")); recordButton=new QPushButton(QStringLiteral("● 录制输入"));
    flowTop->addWidget(load); flowTop->addWidget(save); flowTop->addWidget(recordButton); fl->addLayout(flowTop);
    auto *flowBody=new QHBoxLayout; auto *listCard=card(); auto *ll=new QVBoxLayout(listCard); ll->setContentsMargins(14,16,14,12);
    ll->addWidget(label(QStringLiteral("按顺序执行 · 拖动卡片调整顺序"),"muted"));
    flow=new QListWidget; flow->setObjectName("flow"); flow->setItemDelegate(new StepDelegate(flow)); flow->setDragDropMode(QAbstractItemView::InternalMove);
    flow->setDefaultDropAction(Qt::MoveAction); flow->setSelectionMode(QAbstractItemView::SingleSelection); ll->addWidget(flow,1);
    auto *addCombo=new QComboBox; for(int i=0;i<9;++i) addCombo->addItem(actionName(Action(i)),i);
    auto *add=new QPushButton(QStringLiteral("＋ 添加")); auto *remove=new QPushButton(QStringLiteral("删除")); auto *duplicate=new QPushButton(QStringLiteral("复制"));
    ll->addWidget(row({addCombo,add,duplicate,remove})); flowBody->addWidget(listCard,3);
    auto *editCard=card(); auto *el=new QVBoxLayout(editCard); el->setContentsMargins(22,20,22,20); el->setSpacing(20);
    el->addWidget(label(QStringLiteral("步骤属性"),"sectionTitle")); editor=new StepEditor(false); el->addWidget(editor); el->addStretch();
    el->addWidget(label(QStringLiteral("循环开始 / 结束之间是循环体，可嵌套。\n\n按下与抬起之间可以插入等待或其他动作。\n\n录制会保留按键、点击位置与间隔；不采集鼠标移动轨迹和滚轮。"),"muted"));
    flowBody->addWidget(editCard,2); fl->addLayout(flowBody,1); tabs->addTab(flowPage,QStringLiteral("编排脚本"));
    auto *settings=new QWidget; auto *sl=new QVBoxLayout(settings); sl->setContentsMargins(0,22,0,0); sl->setSpacing(18);
    auto *scard=card(); auto *scl=new QVBoxLayout(scard); scl->setContentsMargins(24,22,24,24); scl->setSpacing(16);
    scl->addWidget(label(QStringLiteral("全局快捷键"),"sectionTitle")); auto *sf=new QFormLayout;
    toggleShortcut=new QKeySequenceEdit(QKeySequence(Qt::Key_F6)); stopShortcut=new QKeySequenceEdit(QKeySequence(Qt::Key_F8)); recordShortcut=new QKeySequenceEdit(QKeySequence(Qt::Key_F7));
    sf->addRow(QStringLiteral("启动 / 停止任务"),toggleShortcut); sf->addRow(QStringLiteral("立即停止"),stopShortcut); sf->addRow(QStringLiteral("开始 / 结束录制"),recordShortcut); scl->addLayout(sf);
    minimizeOnStart=new QCheckBox(QStringLiteral("启动任务时自动最小化")); scl->addWidget(minimizeOnStart); auto *apply=new QPushButton(QStringLiteral("应用快捷键")); scl->addWidget(apply,0,Qt::AlignLeft); sl->addWidget(scard);
    auto *info=card(); auto *il=new QVBoxLayout(info); il->setContentsMargins(24,22,24,22); il->setSpacing(12);
    il->addWidget(label(QStringLiteral("使用说明"),"sectionTitle"));
    il->addWidget(label(QStringLiteral("• F6 启停任务，F8 立即停止，F7 启停录制（可修改）。\n• 使用自己的快捷键作为连点目标时，请先更改快捷键，避免任务自行触发。\n• 坐标使用 Windows 屏幕物理像素；更改显示器布局后需重新拾取。\n• 系统安全界面和部分游戏不接受模拟输入；管理员窗口要求同级权限。\n• FloatingBall 的「启 / 停」控制程序进程；任务由本窗口或快捷键控制。\n• 空闲时无输入钩子、无执行定时器；不安装服务、不创建开机项。"),"muted")); sl->addWidget(info); sl->addStretch(); tabs->addTab(settings,QStringLiteral("快捷键与说明"));
    auto *options=new QHBoxLayout; delay=spin(0,60000,2000,QStringLiteral(" ms")); delay->setObjectName("startDelay");
    rounds=spin(0,1000000,1); rounds->setSpecialValueText(QStringLiteral("无限")); options->addWidget(label(QStringLiteral("启动延迟"))); options->addWidget(delay);
    options->addSpacing(15); options->addWidget(label(QStringLiteral("脚本循环"))); options->addWidget(rounds); options->addStretch(); stats=label(QStringLiteral("等待开始"),"muted"); options->addWidget(stats); root->addLayout(options);
    auto *footer=new QHBoxLayout; message=label(QStringLiteral("默认 F6 启停 · F8 立即停止 · F7 录制"),"muted"); footer->addWidget(message,1);
    startButton=new QPushButton(QStringLiteral("▶  开始任务")); startButton->setObjectName("primary"); stopButton=new QPushButton(QStringLiteral("■  停止")); stopButton->setObjectName("stop"); stopButton->setEnabled(false);
    footer->addWidget(startButton); footer->addWidget(stopButton); root->addLayout(footer);
    connect(startButton,&QPushButton::clicked,this,&Window::start);
    connect(stopButton,&QPushButton::clicked,this,[this] { endRecording(); engine.stop(); notice(QStringLiteral("已停止，程序按住的键已释放")); });
    connect(&engine,&Engine::stateChanged,this,&Window::updateState);
    connect(&engine,&Engine::failed,this,[this](const QString &s) { notice(s,true); });
    connect(&engine,&Engine::finished,this,[this] { notice(QStringLiteral("任务完成，程序按住的键已释放")); stats->setText(QStringLiteral("完成 · %1 次动作").arg(engine.eventCount())); });
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
    connect(apply,&QPushButton::clicked,this,[this] { QString error; if(hotkeys.set(toggleShortcut->keySequence(),stopShortcut->keySequence(),recordShortcut->keySequence(),error)) { settingsSave(); notice(QStringLiteral("快捷键已应用")); } else notice(error,true); });
    connect(&hotkeys,&Hotkeys::triggered,this,[this](int id) {
        if(QApplication::activeModalWidget()) return;
        if(id==2) { endRecording(); engine.stop(); notice(QStringLiteral("已立即停止")); }
        else if(id==3) record();
        else if(recording) endRecording();
        else if(engine.running()) engine.stop(); else start();
    });
    connect(tabs,&QTabWidget::currentChanged,this,[this](int index) { rounds->setEnabled(index==1 && !engine.running() && !recording); });
    Step initial; initial.action=Action::Repeat; initial.count=0; quick->setValue(initial);
    Step first; first.action=Action::Click; refreshItem(new QListWidgetItem(flow),first);
    Step wait; wait.action=Action::Wait; wait.duration=500; refreshItem(new QListWidgetItem(flow),wait); flow->setCurrentRow(0);
    rounds->setEnabled(false); if(!testMode) settingsLoad();
}
Window::~Window() { shutdown(); }
void Window::notice(const QString &s,bool error) { message->setText(s); message->setStyleSheet(error?"color:#bd4c5b;":"color:#798397;"); message->setToolTip(s); }
void Window::refreshItem(QListWidgetItem *item,const Step &s) { item->setText(s.title()); item->setToolTip(s.detail()); item->setData(Qt::UserRole,s.json()); }
void Window::addStep(Action a) {
    if(flow->count()>=10000) { notice(QStringLiteral("步骤最多 10000 个"),true); return; }
    Step s; s.action=a; auto *item=new QListWidgetItem; refreshItem(item,s); int at=flow->currentRow()+1; flow->insertItem(at,item); flow->setCurrentItem(item);
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
    if(recording || engine.running()) return;
    Script s;
    if(tabs->currentIndex()==1) s=currentScript();
    else { s.steps={quick->value()}; s.startDelay=delay->value(); s.rounds=1; s.latch=s.steps[0].action==Action::Down; }
    // Avoid self-triggering global shortcuts through injected input, regardless of modifiers.
    for(const auto &step:s.steps) if(isInputAction(step.action) && !step.key.mouse) {
        for(int id:{1,2,3}) { UINT m,v; if(Hotkeys::decode(hotkeys.sequence(id),m,v) && int(v)==step.key.code) { notice(QStringLiteral("目标按键与快捷键冲突，请先修改快捷键"),true); return; } }
    }
    QString error; if(!engine.start(s,error)) { notice(error,true); return; }
    notice(QStringLiteral("切换到目标窗口，按 %1 可随时停止").arg(hotkeys.sequence(2).toString(QKeySequence::NativeText)));
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
    if(recording) { endRecording(); return; }
    if(engine.running()) { notice(QStringLiteral("请先停止任务再录制"),true); return; }
    tabs->setCurrentIndex(1);
    QString error; if(!monitor.start(error)) { notice(error,true); return; }
    recording=true; lastRecordTime=0; recordingHeld.clear(); recordButton->setText(QStringLiteral("■ 结束录制"));
    tabs->tabBar()->setEnabled(false);
    editor->setEnabled(false); flow->setEnabled(false); startButton->setEnabled(false); stopButton->setEnabled(true); quickPage->setEnabled(false); delay->setEnabled(false); rounds->setEnabled(false);
    // Keep only the recording button actionable in the flow toolbar.
    for(auto *b:flowPage->findChildren<QPushButton *>()) if(b!=recordButton) b->setEnabled(false);
    status->setText(QStringLiteral("● 录制中")); notice(QStringLiteral("正在追加记录，按 %1 结束；鼠标移动与滚轮不会记录").arg(hotkeys.sequence(3).toString(QKeySequence::NativeText)));
}
void Window::appendRecorded(InputKey key,bool down,QPoint position,quint64 time) {
    if(!recording) return;
    if(!key.mouse) {
        for(int id:{1,2,3}) { UINT m,v; if(Hotkeys::decode(hotkeys.sequence(id),m,v) && int(v)==key.code) return; }
    } else {
        HWND under=WindowFromPoint(POINT{position.x(),position.y()});
        if(GetAncestor(under,GA_ROOT)==reinterpret_cast<HWND>(winId())) return;
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
    flow->setEnabled(true); editor->setEnabled(flow->currentItem()!=nullptr); updateState(false); notice(QStringLiteral("录制完成，可调整步骤后保存或运行"));
}
void Window::settingsLoad() {
    QSettings st; restoreGeometry(st.value("geometry").toByteArray());
    Script saved; QString error;
    if(Script::parse(st.value("flow").toByteArray(),saved,error,true)) setScript(saved);
    Step q; if(Step::parse(QJsonDocument::fromJson(st.value("quick").toByteArray()).object(),q,error) && int(q.action)<5) quick->setValue(q);
    delay->setValue(st.value("delay",2000).toInt()); minimizeOnStart->setChecked(st.value("minimize",false).toBool());
    toggleShortcut->setKeySequence(QKeySequence(st.value("toggle","F6").toString())); stopShortcut->setKeySequence(QKeySequence(st.value("stop","F8").toString())); recordShortcut->setKeySequence(QKeySequence(st.value("record","F7").toString()));
    if(!hotkeys.set(toggleShortcut->keySequence(),stopShortcut->keySequence(),recordShortcut->keySequence(),error)) notice(error,true);
    tabs->setCurrentIndex(qBound(0,st.value("tab",0).toInt(),2));
}
void Window::settingsSave() {
    if(testMode) return;
    QSettings st; st.setValue("geometry",saveGeometry()); st.setValue("flow",QJsonDocument(currentScript().json()).toJson(QJsonDocument::Compact));
    st.setValue("quick",QJsonDocument(quick->value().json()).toJson(QJsonDocument::Compact)); st.setValue("delay",delay->value()); st.setValue("minimize",minimizeOnStart->isChecked()); st.setValue("tab",tabs->currentIndex());
    for(const auto &pair:QList<QPair<int,QString>>{{1,"toggle"},{2,"stop"},{3,"record"}}) {
        if(!hotkeys.sequence(pair.first).isEmpty()) st.setValue(pair.second,hotkeys.sequence(pair.first).toString(QKeySequence::PortableText));
    }
    st.sync();
}
void Window::shutdown() { if(closing) return; closing=true; endRecording(); engine.stop(); monitor.stop(); settingsSave(); hotkeys.clear(); }
void Window::closeEvent(QCloseEvent *event) { shutdown(); event->accept(); }
void Window::showWindow() { showNormal(); raise(); activateWindow(); }
void Window::toggleTask() { if(recording) endRecording(); else if(engine.running()) engine.stop(); else start(); }
void Window::stopTask() { endRecording(); engine.stop(); }
