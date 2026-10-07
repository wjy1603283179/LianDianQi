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
        if (sink.keyEvent(k,false,error)) held.remove(k.identity()); else ok=false;
    }
    if (!ok) emit failed(QStringLiteral("按键释放失败，请恢复目标窗口权限后再次停止：%1").arg(error));
    return ok;
}
void Engine::stop() {
    timer.stop(); if(cancel) cancel->store(true); bool wasActive=active; active=false; repeating=holding=false; loops.clear(); conditions.clear();
    if(!visionThread) screenMatcher.reset();
    releaseAll(); if (wasActive) emit stateChanged(false);
}
void Engine::fail(const QString &s) { stop(); emit failed(s); }
bool Engine::key(const InputKey &k, bool down) {
    QString error;
    if (down && held.contains(k.identity())) return true;
    if (!sink.keyEvent(k,down,error)) { fail(error); return false; }
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
    active=false; conditions.clear(); screenMatcher.reset(); releaseAll(); emit stateChanged(false); emit finished();
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
        emit matchEvaluated(at,*result);
        if(!active || token->load() || cancel!=token) return; // Stop/restart must never resume the old branch.
        index=result->found?at+1:alternatives.contains(at)?alternatives.value(at)+1:ends.value(at);
        timer.start(1);
    });
    worker->start();
}
void Engine::tick() {
    if (!active) return;
    if (holding) { holding=false; if (!key(holdKey,false)) return; }
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
        if (s.count && --repeats==0) { repeating=false; ++index; }
        timer.start(s.interval); return;
    case Action::Hold:
        if (!key(s.key,true)) return;
        ++events; ++index;
        if (s.duration==0) return;
        holding=true; holdKey=s.key; timer.start(s.duration); return;
    case Action::Down: if (!key(s.key,true)) return; ++events; break;
    case Action::Up: if (!key(s.key,false)) return; ++events; break;
    case Action::Click: if (!click(s.key)) return; break;
    case Action::Wait: ++index; timer.start(s.duration); return;
    case Action::Move: ++events; break;
    case Action::LoopBegin: loops.append({index,s.count}); break;
    case Action::LoopEnd:
        if (loops.isEmpty()) { fail(QStringLiteral("循环栈错误")); return; }
        if (loops.last().remaining==0 || --loops.last().remaining>0) {
            index=loops.last().begin+1; timer.start(1); return;
        }
        loops.removeLast(); break;
    case Action::IfImage: recognize(s); return;
    case Action::Else:
        if(conditions.isEmpty()) { fail(QStringLiteral("条件栈错误")); return; }
        index=conditions.last().end; timer.start(1); return;
    case Action::EndIf:
        if(conditions.isEmpty()) { fail(QStringLiteral("条件栈错误")); return; }
        conditions.removeLast(); break;
    case Action::ClickMatch: {
        const Condition *match=nullptr;
        for(int i=int(conditions.size())-1;i>=0;--i) if(conditions[i].found) { match=&conditions[i]; break; }
        if(!match) { fail(QStringLiteral("当前没有有效的图像匹配位置")); return; }
        const QRect bounds=match->bounds;
        if(!sink.move(bounds.topLeft()+QPoint(bounds.width()/2,bounds.height()/2),error)) { fail(error); return; }
        if(!click(InputKey{})) return; break;
    }
    }
    ++index; timer.start(1); // Yield between immediate actions, including endless loops.
}
