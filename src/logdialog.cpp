#include "logdialog.h"
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QScreen>
#include <QStyledItemDelegate>

class LogDelegate : public QStyledItemDelegate {
public:
    explicit LogDelegate(QTreeWidget *table):QStyledItemDelegate(table),table(table) {}
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        Q_UNUSED(option);
        QFontMetrics metrics(table->font()); int height=metrics.height();
        // QTreeView may ask only its first visible column for a row's height.
        // Always measure the result and the merged time range for the entire row.
        for(int column:{0,4}) {
            int width=qMax(20,table->columnWidth(column)-12);
            auto bounds=metrics.boundingRect(QRect(0,0,width,10000),Qt::TextWordWrap,index.siblingAtColumn(column).data().toString());
            height=qMax(height,bounds.height());
        }
        return QSize(table->columnWidth(index.column()),height+14);
    }
private:
    QTreeWidget *table;
};

static QString elapsed(qint64 ms) { return QStringLiteral("%1 s").arg(ms/1000.0,0,'f',3); }
ExecutionLogDialog::ExecutionLogDialog(QWidget *parent):QDialog(parent) {
    setObjectName("executionLogDialog"); setWindowTitle(QStringLiteral("上次执行日志")); setAttribute(Qt::WA_DeleteOnClose);
    setStyleSheet("QDialog { background:#f5f6fa; }");
    setMinimumSize(660,360); resize(940,560);
    if(parent && parent->screen()) resize(size().boundedTo(parent->screen()->availableGeometry().size()-QSize(48,80)));
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(20,20,20,16); layout->setSpacing(14);
    summary=new QLabel; summary->setObjectName("logSummary"); summary->setWordWrap(true); layout->addWidget(summary);
    table=new QTreeWidget; table->setObjectName("logEntries"); table->setRootIsDecorated(false); table->setAlternatingRowColors(true);
    table->setHeaderLabels({QStringLiteral("时间"),QStringLiteral("轮次"),QStringLiteral("步骤"),QStringLiteral("动作"),QStringLiteral("结果"),QStringLiteral("次数")});
    table->setSelectionMode(QAbstractItemView::ExtendedSelection); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setTextElideMode(Qt::ElideNone); table->setItemDelegate(new LogDelegate(table));
    table->setWordWrap(true); table->setStyleSheet("QTreeWidget { background:white; alternate-background-color:#f6f7fb; border:1px solid #e2e6ef; border-radius:8px; } QTreeWidget::item { padding:7px 4px; } QHeaderView::section { background:#edf0f8; border:none; padding:8px 4px; }");
    const int widths[]={90,48,48,125,0,65};
    for(int i=0;i<6;++i) { table->header()->setSectionResizeMode(i,i==4?QHeaderView::Stretch:QHeaderView::Interactive); if(widths[i]) table->setColumnWidth(i,widths[i]); }
    table->header()->setStretchLastSection(false); layout->addWidget(table,1);
    connect(table->header(),&QHeaderView::sectionResized,table,&QTreeWidget::doItemsLayout);
    auto *footer=new QHBoxLayout; retention=new QLabel; retention->setObjectName("logRetention"); retention->setWordWrap(true); retention->setStyleSheet("color:#798397;"); footer->addWidget(retention,1);
    footer->addStretch();
    auto *copy=new QPushButton(QStringLiteral("复制日志")); copy->setObjectName("copyExecutionLog"); footer->addWidget(copy);
    auto *close=new QPushButton(QStringLiteral("关闭")); footer->addWidget(close); layout->addLayout(footer);
    connect(copy,&QPushButton::clicked,this,[this] { QApplication::clipboard()->setText(copyText); });
    connect(close,&QPushButton::clicked,this,&QDialog::close);
}
void ExecutionLogDialog::setLog(const ExecutionLog &log) {
    QString text=QStringLiteral("%1 · %2\n耗时 %3 s · %4 次动作").arg(log.started.toString("yyyy-MM-dd HH:mm:ss")).arg(log.outcome)
        .arg(log.durationMs/1000.0,0,'f',3).arg(log.actions);
    if(log.imageChecks) text+=QStringLiteral(" · 图像命中 %1 / %2（%3%）").arg(log.imageHits).arg(log.imageChecks).arg(log.imageHits*100.0/log.imageChecks,0,'f',1);
    summary->setText(text); copyText=text+"\n\n";
    QString note=log.omitted?QStringLiteral("保留最近 %1 条，已省略 %2 条较早记录").arg(log.entries.size()).arg(log.omitted):QString();
    retention->setText(note); retention->setVisible(!note.isEmpty()); if(!note.isEmpty()) copyText+=note+"\n";
    table->setUpdatesEnabled(false); table->clear();
    for(const auto &e:log.entries) {
        QString time=elapsed(e.elapsedMs); if(e.count>1) time+="\n"+elapsed(e.lastMs);
        QStringList columns{time,e.step<0?QStringLiteral("—"):QString::number(e.round+1),e.step<0?QStringLiteral("—"):QString::number(e.step+1),e.action,e.detail,QString::number(e.count)};
        auto *item=new QTreeWidgetItem(table,columns); item->setTextAlignment(1,Qt::AlignCenter); item->setTextAlignment(2,Qt::AlignCenter); item->setTextAlignment(5,Qt::AlignCenter);
        for(int i=0;i<columns.size();++i) item->setToolTip(i,columns[i]);
        copyText+=columns.join("\t").replace('\n',QStringLiteral(" · "))+"\n";
    }
    table->setUpdatesEnabled(true);
}
