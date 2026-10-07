#pragma once
#include "executionlog.h"
#include <QDialog>

class QLabel;
class QTreeWidget;
class ExecutionLogDialog : public QDialog {
public:
    explicit ExecutionLogDialog(QWidget *parent=nullptr);
    void setLog(const ExecutionLog &);
private:
    QLabel *summary, *retention;
    QTreeWidget *table;
    QString copyText;
};
