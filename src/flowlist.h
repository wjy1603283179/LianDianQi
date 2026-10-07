#pragma once
#include "model.h"
#include <QListWidget>
#include <QTimer>

class FlowList : public QListWidget {
    Q_OBJECT
public:
    explicit FlowList(QWidget *parent=nullptr);
    struct Ancestor { int begin; Action action; bool alternative=false; };
    struct Node { Action action=Action::Click; int depth=0,begin=-1,end=-1,alternative=-1; QVector<Ancestor> ancestors; };
    const Node &node(int row) const { return nodes[row]; }
    void scheduleRefresh();
    void refreshStructure();
    bool addAction(Action, QString &error);
    bool duplicateSelected(QString &error);
    void removeSelected();
    bool moveBlock(int from, int to, QString &error);
    int insertionPoint(int row) const;
    QString insertionContext(int row) const;
    void reveal(int row);
signals:
    void structureChanged();
    void editRejected(QString error);
    void pasteRequested();
protected:
    void startDrag(Qt::DropActions) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    QPair<int,int> range(int row) const;
    Step step(int row) const;
    QListWidgetItem *makeItem(const Step &);
    void toggleBlock(int row);
    void showAddMenu(int row, const QPoint &globalPosition);
    int dropPoint(const QPoint &) const;
    QVector<Node> nodes;
    bool pending=false, refreshing=false;
    int dragFrom=-1, dropAt=-1;
    QPoint dragPosition;
    QTimer dragScroll;
};
