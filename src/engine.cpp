#include "engine.h"

Engine::Engine(InputSink &s, QObject *parent,VisionSource *vision) : QObject(parent), sink(s),injectedVision(vision) {
    timer.setSingleShot(true); timer.setTimerType(Qt::PreciseTimer);
    connect(&timer,&QTimer::timeout,this,&Engine::tick);
}
Engine::~Engine() { stop(); if(visionThread) { visionThread->wait(); delete visionThread; visionThread=nullptr; } }
bool Engine::start(const Script &s, QString &error) {
    if (active || !held.isEmpty()) { error=QStringLiteral("请先停止当前任务并释放按键"); return false; }
    if(visionThread) { error=QStringLiteral("识别正在停止，请稍后重试"); return false; }
    if (!s.validate(error)) return false;
    script=s; active=true; index=round=repeats=0; events=0; repeating=holding=false; loops.clear(); conditions.clear(); reporting.start();
    currentLog={}; currentLog.started=QDateTime::currentDateTime(); logTimer.start(); logging=true; releaseFailed=false;
    if(s.startDelay) logEntry(QStringLiteral("启动延迟"),QStringLiteral("%1 ms").arg(s.startDelay),-1);
    ends.clear(); alternatives.clear(); QVector<int> blocks;
    for(int i=0;i<s.steps.size();++i) {
        auto a=s.steps[i].action;
        if(a==Action::IfImage) blocks.append(i);
        else if(a==Action::Else) alternatives[blocks.last()]=i;
        else if(a==Action::EndIf) { ends[blocks.last()]=i; blocks.removeLast(); }
    }
    emit stateChanged(true); emit progress(-1,0,0); timer.start(s.startDelay); return true;
}
bool Engine::releaseAll() {
    QString error; bool ok=true;
    // Keep failures tracked so Stop/destruction can retry; never claim they were released.
    for (const auto &k : held.values()) {
        if (sink.keyEvent(k,false,error)) { held.remove(k.identity()); logEntry(QStringLiteral("自动抬起"),k.name(),-1); }
        else { ok=false; logEntry(QStringLiteral("抬起失败"),k.name()+QStringLiteral(" · ")+error,-1); }
    }
    releaseFailed=!ok;
    if (!ok) emit failed(QStringLiteral("按键释放失败，请恢复目标窗口权限后再次停止：%1").arg(error));
    return ok;
}
void Engine::stop() { stopWithOutcome(QStringLiteral("停止")); }
void Engine::stopWithOutcome(const QString &outcome) {
    timer.stop(); if(cancel) cancel->store(true); bool wasActive=active; active=false; repeating=holding=false; loops.clear(); conditions.clear();
    if(logging && visionThread) logEntry(QStringLiteral("取消识别"),QStringLiteral("已取消，未执行后续分支"));
    if(!visionThread) screenMatcher.reset();
    releaseAll(); endLog(releaseFailed?QStringLiteral("失败"):outcome); if (wasActive) emit stateChanged(false);
}
void Engine::fail(const QString &s) {
    QString detail=s;
    if(index<script.steps.size()) detail=script.steps[index].title()+QStringLiteral(" · ")+script.steps[index].detail()+QStringLiteral(" · ")+s;
    logEntry(QStringLiteral("执行失败"),detail); stopWithOutcome(QStringLiteral("失败")); emit failed(s);
}
void Engine::logEntry(const QString &action,const QString &detail,int step,bool merge) {
    if(!logging) return;
    qint64 ms=logTimer.elapsed(); currentLog.append({ms,ms,step==-2?qMin(index,int(script.steps.size())-1):step,round,action.left(128),detail.left(2048),1},merge);
}
void Engine::logStep(const Step &s,const QString &detail) {
    if(!logging) return;
    // A repeating step has invariant properties. Avoid formatting/log allocation per click.
    if(s.action==Action::Repeat && !currentLog.entries.isEmpty()) {
        auto &last=currentLog.entries.last();
        if(last.step==index && last.round==round && last.action==actionName(Action::Repeat)) {
            ++last.count; last.lastMs=logTimer.elapsed(); return;
        }
    }
    QString text=detail.isNull()?s.detail():detail;
    if(isInputAction(s.action)) text=s.key.name()+QStringLiteral(" · ")+text;
    logEntry(actionName(s.action),text,index,s.action==Action::Repeat);
}
void Engine::endLog(const QString &outcome) {
    if(!logging) return;
    currentLog.ended=QDateTime::currentDateTime(); currentLog.durationMs=logTimer.elapsed();
    currentLog.actions=events; currentLog.outcome=outcome; logging=false;
    lastLog=std::move(currentLog); emit executionLogged();
}
bool Engine::key(const InputKey &k, bool down) {
    QString error;
    if (down && held.contains(k.identity())) return true;
    if (!sink.keyEvent(k,down,error)) {
        if(!down && held.contains(k.identity())) logEntry(QStringLiteral("仍处于按下状态"),k.name()+QStringLiteral(" · 抬起未发送成功"));
        fail(error); return false;
    }
    if (down) held.insert(k.identity(),k); else held.remove(k.identity());
    return true;
}
bool Engine::click(const InputKey &k) {
    if (held.contains(k.identity())) { fail(QStringLiteral("不能连点一个仍被按下的键，请先添加抬起步骤")); return false; }
    if (!key(k,true) || !key(k,false)) return false;
    ++events; return true;
}
void Engine::finish() {
    timer.stop();
    if (script.latch && !held.isEmpty()) { emit progress(index-1,round,events); return; }
    active=false; conditions.clear(); screenMatcher.reset(); bool ok=releaseAll();
    endLog(ok?QStringLiteral("完成"):QStringLiteral("失败")); emit stateChanged(false); if(ok) emit finished();
}
void Engine::recognize(const Step &step) {
    if(!injectedVision && !screenMatcher) screenMatcher=std::make_unique<ScreenMatcher>();
    auto *source=injectedVision?injectedVision:screenMatcher.get();
    cancel=std::make_shared<std::atomic_bool>(false); auto token=cancel;
    auto result=std::make_shared<MatchResult>(); const int at=index;
    auto *worker=QThread::create([source,step,token,result] {
        try { *result=source->recognize(step,*token); }
        catch(...) { result->error=QStringLiteral("图像识别失败，无法分配资源"); }
    });
    visionThread=worker; worker->setParent(this);
    connect(worker,&QThread::finished,this,[this,worker,token,result,at] {
        worker->wait(); visionThread=nullptr; worker->deleteLater();
        if(!active || token->load() || cancel!=token) { screenMatcher.reset(); return; }
        if(!result->error.isEmpty()) { fail(result->error); return; }
        if(result->cancelled) { stop(); return; }
        conditions.append({ends.value(at),result->found,result->bounds});
        ++currentLog.imageChecks; if(result->found) ++currentLog.imageHits;
        const auto &step=script.steps[at];
        QString detail=step.templateName.isEmpty()?QStringLiteral("图像模板"):step.templateName.left(160);
        if(result->found) {
            const auto p=result->center();
            detail+=QStringLiteral(" · 找到 · 相似度 %1%（阈值 %2%） · 中心 (%3, %4) · 缩放 %5%")
                .arg(result->score,0,'f',2).arg(step.similarity).arg(p.x()).arg(p.y()).arg(result->scale*100,0,'f',0);
        } else detail+=QStringLiteral(" · 未找到（阈值 %1%） · %2").arg(step.similarity)
            .arg(alternatives.contains(at)?QStringLiteral("进入否则"):QStringLiteral("跳过条件分支"));
        detail+=QStringLiteral("\n截图 %1 ms · 识别 %2 ms").arg(result->captureMicros/1000.0,0,'f',2).arg(result->matchMicros/1000.0,0,'f',2);
        logEntry(actionName(Action::IfImage),detail,at);
        emit matchEvaluated(at,*result);
        if(!active || token->load() || cancel!=token) return; // Stop/restart must never resume the old branch.
        index=result->found?at+1:alternatives.contains(at)?alternatives.value(at)+1:ends.value(at);
        timer.start(1);
    });
    worker->start();
}
void Engine::tick() {
    if (!active) return;
    if (holding) { holding=false; if (!key(holdKey,false)) return; logEntry(QStringLiteral("抬起"),holdKey.name()+QStringLiteral(" · 长按结束"),holdStep); }
    if (index>=script.steps.size()) {
        ++round;
        if (script.rounds && round>=script.rounds) { finish(); return; }
        index=0; loops.clear(); conditions.clear();
    }
    const auto &s=script.steps[index];
    if (reporting.elapsed()>=100 || !repeating) { emit progress(index,round,events); reporting.restart(); }
    QString error;
    if ((s.action==Action::Move || (s.fixedPosition && s.key.mouse && isInputAction(s.action))) && !sink.move(s.position,error)) { fail(error); return; }
    switch (s.action) {
    case Action::Repeat:
        if (!repeating) { repeats=s.count; repeating=true; }
        if (!click(s.key)) return;
        logStep(s);
        if (s.count && --repeats==0) { repeating=false; ++index; }
        timer.start(s.interval); return;
    case Action::Hold:
        if (!key(s.key,true)) return;
        logStep(s);
        ++events; ++index;
        if (s.duration==0) return;
        holding=true; holdKey=s.key; holdStep=index-1; timer.start(s.duration); return;
    case Action::Down: {
        const bool alreadyHeld=held.contains(s.key.identity());
        if (!key(s.key,true)) return;
        logStep(s,alreadyHeld?QStringLiteral("已按下，未重复发送"):s.detail()); ++events; break;
    }
    case Action::Up: if (!key(s.key,false)) return; logStep(s); ++events; break;
    case Action::Click: if (!click(s.key)) return; logStep(s); break;
    case Action::Wait: logStep(s,s.detail()+QStringLiteral(" · 开始等待")); ++index; timer.start(s.duration); return;
    case Action::Move: logStep(s); ++events; break;
    case Action::LoopBegin: loops.append({index,s.count}); logStep(s); break;
    case Action::LoopEnd:
        if (loops.isEmpty()) { fail(QStringLiteral("循环栈错误")); return; }
        if (loops.last().remaining==0 || --loops.last().remaining>0) {
            logStep(s,QStringLiteral("进入下一次循环"));
            index=loops.last().begin+1; timer.start(1); return;
        }
        loops.removeLast(); logStep(s,QStringLiteral("循环已结束")); break;
    case Action::IfImage: recognize(s); return;
    case Action::Else:
        if(conditions.isEmpty()) { fail(QStringLiteral("条件栈错误")); return; }
        logStep(s,QStringLiteral("已找到图像，跳过否则分支"));
        index=conditions.last().end; timer.start(1); return;
    case Action::EndIf:
        if(conditions.isEmpty()) { fail(QStringLiteral("条件栈错误")); return; }
        conditions.removeLast(); logStep(s); break;
    case Action::ClickMatch: {
        const Condition *match=nullptr;
        for(int i=int(conditions.size())-1;i>=0;--i) if(conditions[i].found) { match=&conditions[i]; break; }
        if(!match) { fail(QStringLiteral("当前没有有效的图像匹配位置")); return; }
        const QRect bounds=match->bounds;
        if(!sink.move(bounds.topLeft()+QPoint(bounds.width()/2,bounds.height()/2),error)) { fail(error); return; }
        if(!click(InputKey{})) return;
        const auto p=bounds.topLeft()+QPoint(bounds.width()/2,bounds.height()/2);
        logStep(s,QStringLiteral("鼠标左键 · (%1, %2)").arg(p.x()).arg(p.y())); break;
    }
    }
    ++index; timer.start(1); // Yield between immediate actions, including endless loops.
}
