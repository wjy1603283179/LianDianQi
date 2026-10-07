#include "flowlist.h"
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScopeGuard>
#include <QScrollBar>
#include <QMenu>

static constexpr int Indent=28;
static constexpr int CollapsedRole=Qt::UserRole+3;
static const char *BlockMime="application/x-dianxu-flow-block";
static QColor accent(Action action,bool alternative=false) { return action==Action::LoopBegin?QColor("#23967e"):alternative?QColor("#b77b2e"):QColor("#6371dc"); }
static QColor tint(Action action,bool alternative=false) { return action==Action::LoopBegin?QColor("#f1f9f5"):alternative?QColor("#fff8ed"):QColor("#f3f5ff"); }

class FlowDelegate : public QStyledItemDelegate {
public:
    explicit FlowDelegate(FlowList *list):QStyledItemDelegate(list),list(list) {}
    QSize sizeHint(const QStyleOptionViewItem &,const QModelIndex &index) const override {
        const auto id=index.data(Qt::UserRole).toJsonObject()["action"].toString();
        int height=id=="ifImage"?100:id=="loop"?80:id=="else"?58:(id=="endIf" || id=="endLoop")?36:74;
        return {260+index.data(Qt::UserRole+2).toInt()*Indent,height};
    }
    void paint(QPainter *p,const QStyleOptionViewItem &o,const QModelIndex &index) const override {
        if(index.row()>=list->count()) return;
        const auto &n=list->node(index.row()); const QRect area=o.rect;
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        // Continuous sides and backgrounds join adjacent rows into containing blocks.
        for(int level=0;level<n.ancestors.size();++level) {
            const auto &a=n.ancestors[level]; QRect r=area.adjusted(4+level*Indent,0,-4,0);
            p->fillRect(r,tint(a.action,a.alternative)); p->setPen(QPen(accent(a.action,a.alternative),1));
            p->drawLine(r.topLeft(),r.bottomLeft()+QPoint(0,1)); p->drawLine(r.topRight(),r.bottomRight()+QPoint(0,1));
        }
        const bool selected=o.state.testFlag(QStyle::State_Selected),running=index.data(Qt::UserRole+1).toBool();
        const bool header=n.action==Action::IfImage || n.action==Action::LoopBegin;
        const bool footer=n.action==Action::EndIf || n.action==Action::LoopEnd;
        QRect r=area.adjusted(4+n.depth*Indent,header?0:footer?0:4,-4,header || footer?0:-4);
        if(header || footer || n.action==Action::Else) {
            Action kind=n.action==Action::LoopBegin || n.action==Action::LoopEnd?Action::LoopBegin:Action::IfImage;
            bool alternative=n.action==Action::Else; QColor color=accent(kind,alternative);
            p->setPen(QPen(running?QColor("#23967e"):selected?color:color.lighter(155),selected?2:1)); p->setBrush(tint(kind,alternative));
            QPainterPath shape;
            if(header) { shape.addRoundedRect(r.adjusted(0,0,0,10),10,10); p->setClipRect(r); p->drawPath(shape); p->setClipping(false); }
            else if(footer) { shape.addRoundedRect(r.adjusted(0,-10,0,0),10,10); p->setClipRect(r); p->drawPath(shape); p->setClipping(false); }
            else { p->drawRect(r); }
            p->setPen(color); QFont font=o.font; font.setBold(!footer); p->setFont(font);
            if(footer) {
                p->drawText(r.adjusted(14,0,-8,0),Qt::AlignVCenter,kind==Action::LoopBegin?QStringLiteral("循环块结束"):QStringLiteral("条件块结束"));
            } else if(alternative) {
                p->drawText(r.adjusted(16,4,-8,-24),Qt::AlignVCenter,QStringLiteral("未找到时 · 否则"));
                font.setBold(false); font.setPointSize(9); p->setFont(font); p->setPen(QColor("#8c7e69"));
                p->drawText(r.adjusted(16,29,-8,0),Qt::AlignVCenter,n.end==index.row()+1?QStringLiteral("空分支"):QStringLiteral("此分支的动作"));
            } else {
                const bool collapsed=index.data(CollapsedRole).toBool();
                p->drawText(QRect(r.left()+10,r.top()+12,18,24),Qt::AlignCenter,collapsed?QStringLiteral("▸"):QStringLiteral("▾"));
                p->drawText(r.adjusted(35,9,-8,-r.height()+35),Qt::AlignVCenter,p->fontMetrics().elidedText(QStringLiteral("%1  %2").arg(index.row()+1,2,10,QChar('0')).arg(index.data().toString()),Qt::ElideRight,r.width()-48));
                font.setBold(false); font.setPointSize(9); p->setFont(font); p->setPen(QColor("#788298"));
                p->drawText(r.adjusted(35,36,-10,-r.height()+60),Qt::AlignVCenter,p->fontMetrics().elidedText(index.data(Qt::ToolTipRole).toString(),Qt::ElideRight,r.width()-48));
                if(kind==Action::IfImage) { p->setPen(color); p->drawText(r.adjusted(16,68,-76,0),Qt::AlignVCenter,p->fontMetrics().elidedText(collapsed?QStringLiteral("找到时 / 未找到时 · 已折叠"):QStringLiteral("找到时"),Qt::ElideRight,qMax(0,r.width()-92))); }
            }
            if(!footer) {
                p->setPen(color); font.setBold(false); font.setPointSize(9); p->setFont(font);
                p->drawText(QRect(r.right()-64,r.bottom()-27,58,24),Qt::AlignCenter,QStringLiteral("＋ 添加"));
            }
        } else {
            p->setPen(QPen(running?QColor("#32a683"):selected?QColor("#6371dc"):QColor("#e0e5ef"),selected?1.5:1));
            p->setBrush(running?QColor("#effaf5"):selected?QColor("#eef1ff"):Qt::white); p->drawRoundedRect(r,8,8);
            auto font=o.font; font.setBold(true); p->setFont(font); p->setPen(QColor("#6572c7"));
            p->drawText(QRect(r.left()+9,r.top()+12,28,25),Qt::AlignCenter,QString("%1").arg(index.row()+1,2,10,QChar('0')));
            p->setPen(QColor("#25304a")); p->drawText(r.adjusted(45,7,-10,-r.height()+34),Qt::AlignVCenter,p->fontMetrics().elidedText(index.data().toString(),Qt::ElideRight,r.width()-57));
            font.setBold(false); font.setPointSize(9); p->setFont(font); p->setPen(QColor("#788298"));
            p->drawText(r.adjusted(45,35,-10,-5),Qt::AlignVCenter,p->fontMetrics().elidedText(index.data(Qt::ToolTipRole).toString(),Qt::ElideRight,r.width()-57));
        }
        p->restore();
    }
private:
    FlowList *list;
};

FlowList::FlowList(QWidget *parent):QListWidget(parent) {
    setObjectName("flow"); setItemDelegate(new FlowDelegate(this)); setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::InternalMove); setDefaultDropAction(Qt::MoveAction); setDropIndicatorShown(false); setSpacing(0);
    for(auto signal:{&QAbstractItemModel::rowsInserted,&QAbstractItemModel::rowsRemoved}) connect(model(),signal,this,[this] { scheduleRefresh(); });
    connect(model(),&QAbstractItemModel::dataChanged,this,[this](const QModelIndex &,const QModelIndex &,const QList<int> &roles) { if(roles.isEmpty() || roles.contains(Qt::UserRole)) scheduleRefresh(); });
    dragScroll.setInterval(25);
    connect(&dragScroll,&QTimer::timeout,this,[this] {
        int delta=dragPosition.y()<30?-18:dragPosition.y()>viewport()->height()-30?18:0;
        if(!delta || dragFrom<0) { dragScroll.stop(); return; }
        verticalScrollBar()->setValue(verticalScrollBar()->value()+delta); dropAt=dropPoint(dragPosition); viewport()->update();
    });
}
Step FlowList::step(int row) const { Step s; QString error; Step::parse(item(row)->data(Qt::UserRole).toJsonObject(),s,error); return s; }
QListWidgetItem *FlowList::makeItem(const Step &s) { auto *item=new QListWidgetItem; item->setText(s.title()); item->setToolTip(s.detail()); item->setData(Qt::UserRole,s.json()); return item; }
void FlowList::scheduleRefresh() {
    if(pending || refreshing) return; pending=true;
    QTimer::singleShot(0,this,[this] { if(pending) refreshStructure(); });
}
void FlowList::refreshStructure() {
    if(refreshing) return; refreshing=true; pending=false; auto guard=qScopeGuard([this] { refreshing=false; });
    nodes.clear(); nodes.resize(count()); QVector<Ancestor> stack;
    for(int row=0;row<count();++row) {
        auto &n=nodes[row]; n.action=actionFromId(item(row)->data(Qt::UserRole).toJsonObject()["action"].toString());
        if(n.action==Action::EndIf || n.action==Action::LoopEnd) {
            const auto kind=n.action==Action::EndIf?Action::IfImage:Action::LoopBegin;
            if(!stack.isEmpty() && stack.last().action==kind) {
                n.begin=stack.takeLast().begin; nodes[n.begin].end=row;
                if(nodes[n.begin].alternative>=0) nodes[nodes[n.begin].alternative].end=row;
            }
        }
        n.ancestors=stack; n.depth=int(stack.size());
        if(n.action==Action::Else && !stack.isEmpty() && stack.last().action==Action::IfImage) {
            n.begin=stack.last().begin; nodes[n.begin].alternative=row;
            n.ancestors.removeLast(); --n.depth; stack.last().alternative=true;
        }
        if(n.action==Action::IfImage || n.action==Action::LoopBegin) stack.append({row,n.action,false});
        item(row)->setData(Qt::UserRole+2,n.depth);
        auto flags=item(row)->flags(); bool draggable=n.action!=Action::Else && n.action!=Action::EndIf && n.action!=Action::LoopEnd;
        flags.setFlag(Qt::ItemIsDragEnabled,draggable); if(flags!=item(row)->flags()) item(row)->setFlags(flags);
    }
    for(int row=0;row<count();++row) {
        bool hidden=false; for(const auto &a:nodes[row].ancestors) hidden|=item(a.begin)->data(CollapsedRole).toBool();
        if(nodes[row].begin>=0) hidden|=item(nodes[row].begin)->data(CollapsedRole).toBool();
        item(row)->setHidden(hidden);
    }
    doItemsLayout(); viewport()->update(); emit structureChanged();
}
QPair<int,int> FlowList::range(int row) const {
    if(row<0 || row>=nodes.size()) return {-1,-1}; const auto &n=nodes[row];
    if(n.action==Action::EndIf || n.action==Action::LoopEnd) row=n.begin>=0?n.begin:row;
    return {row,nodes[row].end>=row?nodes[row].end:row};
}
int FlowList::insertionPoint(int row) const {
    if(row<0 || row>=nodes.size()) return count(); const auto &n=nodes[row];
    if(n.action==Action::IfImage) return n.alternative>=0?n.alternative:n.end>=0?n.end:row+1;
    if(n.action==Action::LoopBegin || n.action==Action::Else) return n.end>=0?n.end:row+1;
    return row+1;
}
QString FlowList::insertionContext(int row) const {
    if(row<0 || row>=nodes.size()) return QStringLiteral("添加到：脚本"); const auto &n=nodes[row];
    if(n.action==Action::IfImage) return QStringLiteral("添加到：找到时");
    if(n.action==Action::Else) return QStringLiteral("添加到：未找到时");
    if(n.action==Action::LoopBegin) return QStringLiteral("添加到：循环");
    if(!n.ancestors.isEmpty()) { auto a=n.ancestors.last(); return a.action==Action::LoopBegin?QStringLiteral("添加到：循环"):a.alternative?QStringLiteral("添加到：未找到时"):QStringLiteral("添加到：找到时"); }
    return QStringLiteral("添加到：脚本");
}
bool FlowList::addAction(Action action,QString &error) {
    refreshStructure(); int at=insertionPoint(currentRow());
    if(action==Action::Else) {
        int begin=-1;
        if(currentRow()>=0) {
            const auto &n=nodes[currentRow()];
            if(n.action==Action::IfImage) begin=currentRow(); else if(n.begin>=0 && nodes[n.begin].action==Action::IfImage) begin=n.begin;
            else for(const auto &a:n.ancestors) if(a.action==Action::IfImage) begin=a.begin;
        }
        if(begin<0 || nodes[begin].end<0) { error=QStringLiteral("请选择一个完整的图像条件块"); return false; }
        if(nodes[begin].alternative>=0) { error=QStringLiteral("这个条件已有未找到分支"); return false; } at=nodes[begin].end;
    } else if(action==Action::EndIf || action==Action::LoopEnd) { error=QStringLiteral("结束边界由流程块自动管理"); return false; }
    QVector<Step> added; Step initial; initial.action=action; added.append(initial);
    if(action==Action::IfImage) for(auto child:{Action::ClickMatch,Action::Else,Action::EndIf}) { Step childStep; childStep.action=child; added.append(childStep); }
    if(action==Action::LoopBegin) { Step end; end.action=Action::LoopEnd; added.append(end); }
    if(count()+added.size()>10000) { error=QStringLiteral("步骤最多 10000 个"); return false; }
    Script candidate; for(int i=0;i<count();++i) candidate.steps.append(step(i));
    for(int i=0;i<added.size();++i) candidate.steps.insert(at+i,added[i]);
    if(!candidate.validate(error,true)) return false;
    QListWidgetItem *first=nullptr; for(const auto &s:added) { auto *newItem=makeItem(s); insertItem(at++,newItem); if(!first) first=newItem; }
    refreshStructure(); reveal(row(first)); setCurrentItem(first); return true;
}
void FlowList::removeSelected() {
    refreshStructure(); int selected=currentRow(); if(selected<0) return; auto span=range(selected);
    if(nodes[selected].action==Action::Else) span={selected,nodes[selected].end>selected?nodes[selected].end-1:selected};
    for(int i=span.second;i>=span.first;--i) delete takeItem(i); refreshStructure(); setCurrentRow(qMin(span.first,count()-1));
}
bool FlowList::duplicateSelected(QString &error) {
    refreshStructure(); if(currentRow()<0) return false;
    if(nodes[currentRow()].action==Action::Else) { error=QStringLiteral("请复制整个条件块"); return false; }
    auto span=range(currentRow()); int size=span.second-span.first+1;
    if(count()+size>10000) { error=QStringLiteral("步骤最多 10000 个"); return false; }
    Script candidate; for(int i=0;i<count();++i) candidate.steps.append(step(i));
    for(int i=0;i<size;++i) candidate.steps.insert(span.second+1+i,step(span.first+i));
    if(!candidate.validate(error,true)) return false;
    QList<QListWidgetItem *> copies; for(int i=span.first;i<=span.second;++i) copies.append(item(i)->clone());
    for(int i=0;i<copies.size();++i) insertItem(span.second+1+i,copies[i]); refreshStructure(); setCurrentItem(copies.first()); return true;
}
bool FlowList::moveBlock(int from,int to,QString &error) {
    refreshStructure(); if(from<0 || from>=count() || to<0 || to>count()) return false;
    if(nodes[from].action==Action::Else || nodes[from].action==Action::EndIf || nodes[from].action==Action::LoopEnd) { error=QStringLiteral("请移动整个流程块"); return false; }
    auto span=range(from); if(to>=span.first && to<=span.second+1) { error=QStringLiteral("不能将流程块移入自身"); return false; }
    Script candidate; QVector<Step> moving;
    for(int i=0;i<count();++i) { if(i>=span.first && i<=span.second) moving.append(step(i)); else candidate.steps.append(step(i)); }
    int target=to>span.second?to-int(moving.size()):to;
    for(int i=0;i<moving.size();++i) candidate.steps.insert(target+i,moving[i]);
    if(!candidate.validate(error,true)) return false;
    QList<QListWidgetItem *> items; for(int i=span.first;i<=span.second;++i) items.append(takeItem(span.first));
    for(int i=0;i<items.size();++i) insertItem(target+i,items[i]); refreshStructure(); reveal(target); setCurrentItem(items.first()); return true;
}
void FlowList::toggleBlock(int row) {
    refreshStructure(); if(row<0 || row>=count() || nodes[row].end<row || (nodes[row].action!=Action::IfImage && nodes[row].action!=Action::LoopBegin)) return;
    setCurrentRow(row); item(row)->setData(CollapsedRole,!item(row)->data(CollapsedRole).toBool()); refreshStructure();
}
void FlowList::reveal(int row) {
    if(row<0 || row>=nodes.size()) return;
    bool changed=false;
    for(const auto &a:nodes[row].ancestors) if(item(a.begin)->data(CollapsedRole).toBool()) { item(a.begin)->setData(CollapsedRole,false); changed=true; }
    if(nodes[row].begin>=0 && item(nodes[row].begin)->data(CollapsedRole).toBool()) { item(nodes[row].begin)->setData(CollapsedRole,false); changed=true; }
    if(changed) refreshStructure(); scrollToItem(item(row));
}
void FlowList::mousePressEvent(QMouseEvent *event) {
    auto *under=itemAt(event->position().toPoint());
    if(under && event->button()==Qt::LeftButton) {
        refreshStructure(); int i=row(under); int left=visualItemRect(under).left()+4+nodes[i].depth*Indent;
        const auto action=nodes[i].action; const QRect rect=visualItemRect(under);
        if((action==Action::IfImage || action==Action::LoopBegin || action==Action::Else) && event->position().x()>rect.right()-68 && event->position().y()>rect.bottom()-29) {
            showAddMenu(i,event->globalPosition().toPoint()); event->accept(); return;
        }
        if(event->position().x()>=left && event->position().x()<left+30 && (nodes[i].action==Action::IfImage || nodes[i].action==Action::LoopBegin)) { toggleBlock(i); event->accept(); return; }
    }
    QListWidget::mousePressEvent(event);
}
void FlowList::showAddMenu(int row,const QPoint &position) {
    setCurrentRow(row); const auto &n=nodes[row]; bool matched=n.action==Action::IfImage,inLoop=n.action==Action::LoopBegin;
    for(const auto &a:n.ancestors) { matched|=a.action==Action::IfImage && !a.alternative; inLoop|=a.action==Action::LoopBegin; }
    auto *menu=new QMenu(this); menu->setObjectName("branchAddMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
    for(int i=0;i<ActionCount;++i) {
        auto action=Action(i); if(action==Action::Else || action==Action::EndIf || action==Action::LoopEnd) continue;
        auto *entry=menu->addAction(actionName(action)); entry->setData(i);
        entry->setEnabled((action!=Action::ClickMatch || matched) && (action!=Action::BreakLoop || inLoop));
        connect(entry,&QAction::triggered,this,[this,menu,row,action] {
            menu->close(); if(!isEnabled()) return; setCurrentRow(row); QString error; if(!addAction(action,error)) emit editRejected(error);
        });
    }
    menu->popup(position);
}
void FlowList::mouseDoubleClickEvent(QMouseEvent *event) {
    auto *under=itemAt(event->position().toPoint());
    if(under && (nodes[row(under)].action==Action::IfImage || nodes[row(under)].action==Action::LoopBegin)) { toggleBlock(row(under)); event->accept(); }
    else QListWidget::mouseDoubleClickEvent(event);
}
void FlowList::keyPressEvent(QKeyEvent *event) {
    QString error;
    if(event->matches(QKeySequence::Paste)) { emit pasteRequested(); event->accept(); }
    else if(event->key()==Qt::Key_Delete) { removeSelected(); event->accept(); }
    else if(event->key()==Qt::Key_D && event->modifiers()==Qt::ControlModifier) { if(!duplicateSelected(error) && !error.isEmpty()) emit editRejected(error); event->accept(); }
    else QListWidget::keyPressEvent(event);
}
void FlowList::startDrag(Qt::DropActions) {
    refreshStructure(); if(currentRow()<0 || !currentItem()->flags().testFlag(Qt::ItemIsDragEnabled)) return;
    dragFrom=currentRow(); auto *drag=new QDrag(this); auto *mime=new QMimeData; mime->setData(BlockMime,"block"); drag->setMimeData(mime);
    drag->exec(Qt::MoveAction); delete drag; dragScroll.stop(); dragFrom=dropAt=-1; viewport()->update();
}
int FlowList::dropPoint(const QPoint &point) const {
    auto *under=itemAt(point); if(!under) return count(); int i=row(under); auto action=nodes[i].action;
    if(action==Action::IfImage || action==Action::LoopBegin || action==Action::Else) return point.y()<visualItemRect(under).top()+12?i:insertionPoint(i);
    return point.y()<visualItemRect(under).center().y()?i:i+1;
}
void FlowList::dragEnterEvent(QDragEnterEvent *event) { if(event->source()==this && event->mimeData()->hasFormat(BlockMime)) event->acceptProposedAction(); else event->ignore(); }
void FlowList::dragMoveEvent(QDragMoveEvent *event) {
    if(event->source()!=this || dragFrom<0) { event->ignore(); return; }
    dragPosition=event->position().toPoint(); dropAt=dropPoint(dragPosition); auto span=range(dragFrom);
    if(dragPosition.y()<30 || dragPosition.y()>viewport()->height()-30) dragScroll.start(); else dragScroll.stop();
    if(dropAt>=span.first && dropAt<=span.second+1) { dropAt=-1; event->ignore(); } else event->acceptProposedAction(); viewport()->update();
}
void FlowList::dragLeaveEvent(QDragLeaveEvent *event) { dragScroll.stop(); dropAt=-1; viewport()->update(); event->accept(); }
void FlowList::dropEvent(QDropEvent *event) {
    if(event->source()!=this || dragFrom<0) { event->ignore(); return; }
    dragScroll.stop();
    QString error; if(moveBlock(dragFrom,dropPoint(event->position().toPoint()),error)) event->acceptProposedAction(); else { event->ignore(); if(!error.isEmpty()) emit editRejected(error); }
    dropAt=-1; viewport()->update();
}
void FlowList::paintEvent(QPaintEvent *event) {
    if(pending || nodes.size()!=count()) refreshStructure(); QListWidget::paintEvent(event);
    if(dropAt>=0) {
        QPainter painter(viewport()); painter.setPen(QPen(QColor("#6371dc"),3));
        int y=dropAt<count()?visualItemRect(item(dropAt)).top():count()?visualItemRect(item(count()-1)).bottom():4;
        int depth=dropAt<count()?nodes[dropAt].depth:0;
        if(dropAt<count() && (nodes[dropAt].action==Action::Else || nodes[dropAt].action==Action::EndIf || nodes[dropAt].action==Action::LoopEnd)) ++depth;
        if(dropAt<count() && item(dropAt)->isHidden()) for(const auto &a:nodes[dropAt].ancestors) if(item(a.begin)->data(CollapsedRole).toBool()) { y=visualItemRect(item(a.begin)).bottom(); break; }
        painter.drawLine(8+depth*Indent,y,viewport()->width()-8,y);
    }
}
