#include "executionlog.h"
#include <QJsonArray>
#include <QJsonDocument>

void ExecutionLog::append(ExecutionEntry entry,bool merge) {
    if(merge && !entries.isEmpty()) {
        auto &last=entries.last();
        if(last.step==entry.step && last.round==entry.round && last.action==entry.action && last.detail==entry.detail) {
            last.count+=entry.count; last.lastMs=entry.lastMs; return;
        }
    }
    // Trim in batches rather than shifting the entire buffer on every action.
    if(entries.size()>=MaxEntries) {
        omitted+=MaxEntries/2; entries.remove(0,MaxEntries/2);
    }
    entries.append(std::move(entry));
}
QByteArray ExecutionLog::save() const {
    if(!valid()) return {};
    QJsonArray rows;
    for(const auto &e:entries) rows.append(QJsonObject{{"ms",e.elapsedMs},{"lastMs",e.lastMs},{"step",e.step},{"round",e.round},
        {"action",e.action},{"detail",e.detail},{"count",QString::number(e.count)}});
    QJsonObject object{{"version",1},{"started",started.toString(Qt::ISODateWithMs)},{"ended",ended.toString(Qt::ISODateWithMs)},
        {"outcome",outcome},{"durationMs",durationMs},{"actions",QString::number(actions)},
        {"imageChecks",QString::number(imageChecks)},{"imageHits",QString::number(imageHits)},
        {"omitted",QString::number(omitted)},{"entries",rows}};
    auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact); quint64 skipped=0;
    // Even unusually long template names must not create an unbounded settings value.
    while(bytes.size()>2*1024*1024 && !rows.isEmpty()) {
        int remove=qMin(500,int(rows.size())); skipped+=remove; while(remove--) rows.removeFirst();
        object["entries"]=rows; object["omitted"]=QString::number(omitted+skipped); bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
    return bytes;
}
ExecutionLog ExecutionLog::load(const QByteArray &bytes) {
    ExecutionLog log; if(bytes.isEmpty() || bytes.size()>2*1024*1024) return log;
    auto o=QJsonDocument::fromJson(bytes).object(); if(o["version"].toInt()!=1) return {};
    log.started=QDateTime::fromString(o["started"].toString(),Qt::ISODateWithMs);
    log.ended=QDateTime::fromString(o["ended"].toString(),Qt::ISODateWithMs);
    log.outcome=o["outcome"].toString(); log.durationMs=o["durationMs"].toInteger(-1);
    auto number=[&o](const char *name,quint64 &value) { bool ok=false; value=o[name].toString().toULongLong(&ok); return ok; };
    if(!log.valid() || log.outcome.isEmpty() || log.outcome.size()>128 || log.durationMs<0 ||
        !number("actions",log.actions) || !number("imageChecks",log.imageChecks) || !number("imageHits",log.imageHits) ||
        !number("omitted",log.omitted) || log.imageHits>log.imageChecks || !o["entries"].isArray()) return {};
    auto rows=o["entries"].toArray(); if(rows.size()>MaxEntries) return {};
    for(auto value:rows) {
        auto r=value.toObject(); ExecutionEntry e; bool ok=false;
        e.elapsedMs=r["ms"].toInteger(-1); e.lastMs=r["lastMs"].toInteger(-1);
        e.step=r["step"].toInt(-2); e.round=r["round"].toInt(-1);
        e.action=r["action"].toString(); e.detail=r["detail"].toString(); e.count=r["count"].toString().toULongLong(&ok);
        if(!ok || !e.count || e.elapsedMs<0 || e.lastMs<e.elapsedMs || e.lastMs>log.durationMs || e.step< -1 ||
            e.step>=10000 || e.round<0 || e.action.isEmpty() || e.action.size()>128 || e.detail.size()>2048) return {};
        log.entries.append(std::move(e));
    }
    return log;
}
