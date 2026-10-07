#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QVector>

struct ExecutionEntry {
    qint64 elapsedMs=0, lastMs=0;
    int step=-1, round=0;
    QString action, detail;
    quint64 count=1;
};

struct ExecutionLog {
    static constexpr int MaxEntries=2000;
    QDateTime started, ended;
    QString outcome;
    qint64 durationMs=0;
    quint64 actions=0, imageChecks=0, imageHits=0, omitted=0;
    QVector<ExecutionEntry> entries;
    bool valid() const { return started.isValid() && ended.isValid(); }
    void append(ExecutionEntry entry, bool merge=false);
    QByteArray save() const;
    static ExecutionLog load(const QByteArray &);
};
