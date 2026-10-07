#include "engine.h"
#include "positionpicker.h"
#include <QTest>
#include <QSignalSpy>
#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QScopeGuard>
#include <QScreen>
#include <QDir>
#include <QThread>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QSettings>
#include <algorithm>

static QByteArray png(const QImage &image) { QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG"); return bytes; }
static QString fixtures() { return QStringLiteral(VISION_FIXTURES); }
static QJsonArray words() { QFile file(fixtures()+"/words.json"); if(!file.open(QIODevice::ReadOnly)) return {}; return QJsonDocument::fromJson(file.readAll()).array(); }
static Step condition() { Step step; step.action=Action::IfImage; step.templatePng=png(QImage(fixtures()+"/templates/01.png")); return step; }
static Step action(Action a) { Step step; step.action=a; return step; }
class MemoryInput : public InputSink {
public:
    QList<QPoint> positions; int clicks=0;
    bool keyEvent(const InputKey &,bool down,QString &) override { if(down) ++clicks; return true; }
    bool move(const QPoint &p,QString &) override { positions.append(p); return true; }
};
class FakeVision : public VisionSource {
public:
    QList<bool> results; std::atomic_int calls{0}; bool waitForCancel=false;
    MatchResult recognize(const Step &,const std::atomic_bool &cancel) override {
        int i=calls.fetch_add(1); MatchResult r;
        if(waitForCancel) { while(!cancel.load()) QThread::msleep(1); r.cancelled=true; return r; }
        r.found=results.value(i); r.bounds=QRect(-200,120,80,40); r.score=r.found?97.25:0; r.scale=1.25; r.captureMicros=1234; r.matchMicros=567; return r;
    }
};
class Tests : public QObject {
    Q_OBJECT
    POINT previousCursor={}; HWND previousForeground=nullptr;
private slots:
    void initTestCase() { qRegisterMetaType<MatchResult>(); QCOMPARE(words().size(),36); }
    void init() { GetPhysicalCursorPos(&previousCursor); previousForeground=GetForegroundWindow(); }
    void cleanup() { if(QGuiApplication::platformName()!="offscreen") { SetPhysicalCursorPos(previousCursor.x,previousCursor.y); if(IsWindow(previousForeground)) SetForegroundWindow(previousForeground); } }
    void imageScriptRoundtrip() {
        Script s; auto p=condition(); p.limitRegion=true; p.searchRegion=QRect(-1920,-1080,1200,700); p.templateName=QStringLiteral("大哭");
        s.steps={p,action(Action::ClickMatch),action(Action::Else),action(Action::Wait),action(Action::EndIf)};
        Script copy; QString error; QVERIFY2(Script::parse(QJsonDocument(s.json()).toJson(),copy,error),qPrintable(error)); QCOMPARE(copy.json(),s.json());
        QVERIFY(copy.steps[0].templatePng==p.templatePng);
        auto broken=p.json(); broken["templatePng"]="malformed##"; QVERIFY(!Step::parse(broken,p,error));
        broken=condition().json(); broken["similarity"]=101; QVERIFY(!Step::parse(broken,p,error));
    }
    void rejectInvalidBranches() {
        QString error; Script s;
        for(auto list:QList<QVector<Step>>{{action(Action::Else)},{condition(),action(Action::Else),action(Action::Else),action(Action::EndIf)},
                {condition(),action(Action::LoopBegin),action(Action::EndIf),action(Action::LoopEnd)},
                {condition(),action(Action::Else),action(Action::ClickMatch),action(Action::EndIf)},
                {condition(),action(Action::ClickMatch)}}) { s.steps=list; QVERIFY(!s.validate(error)); }
        auto empty=condition(); empty.templatePng.clear(); s.steps={empty,action(Action::EndIf)}; QVERIFY(!s.validate(error));
        s.steps={action(Action::BreakLoop)}; QVERIFY(!s.validate(error));
        s.steps={condition(),action(Action::StopTask),action(Action::Else),action(Action::StopTask),action(Action::EndIf)}; QVERIFY(s.validate(error));
    }
    void foundImageEndsAllRoundsAndReleasesHeldKeys() {
        MemoryInput input; FakeVision vision; vision.results={true}; Engine engine(input,nullptr,&vision); Script s; s.startDelay=0; s.rounds=0;
        s.steps={action(Action::Down),condition(),action(Action::StopTask),action(Action::Else),action(Action::Click),action(Action::EndIf),action(Action::Click)};
        QString error; QSignalSpy finished(&engine,&Engine::finished); QVERIFY(engine.start(s,error)); QTRY_COMPARE(finished.count(),1);
        QVERIFY(!engine.running()); QCOMPARE(engine.heldCount(),0); QCOMPARE(engine.eventCount(),quint64(1)); QCOMPARE(input.clicks,1); QCOMPARE(vision.calls.load(),1);
        bool end=false,release=false; for(const auto &entry:engine.lastExecutionLog().entries) { end|=entry.action==actionName(Action::StopTask); release|=entry.action==QStringLiteral("自动抬起"); }
        QVERIFY(end); QVERIFY(release);
    }
    void breakInnerLoopPreservesEnclosingMatchAndOuterLoop() {
        MemoryInput input; FakeVision vision; vision.results={true,true,true,true,true,true}; Engine engine(input,nullptr,&vision);
        Script s; s.startDelay=0; auto outer=action(Action::LoopBegin); outer.count=2; auto inner=outer; inner.count=0;
        s.steps={outer,condition(),inner,condition(),action(Action::BreakLoop),action(Action::Else),action(Action::Click),action(Action::EndIf),
            action(Action::Click),action(Action::LoopEnd),action(Action::ClickMatch),action(Action::Else),action(Action::Click),action(Action::EndIf),action(Action::LoopEnd),action(Action::Click)};
        QString error; QSignalSpy finished(&engine,&Engine::finished); QVERIFY2(engine.start(s,error),qPrintable(error)); QTRY_COMPARE(finished.count(),1);
        QCOMPARE(vision.calls.load(),4); QCOMPARE(input.clicks,3); QCOMPARE(input.positions.size(),2); QCOMPARE(engine.heldCount(),0);
        for(auto position:input.positions) QCOMPARE(position,QPoint(-160,140));
    }
    void otherwiseNestedIfRunsOnlyAfterPreviousMiss() {
        for(bool first:{true,false}) {
            MemoryInput input; FakeVision vision; vision.results={first,true}; Engine engine(input,nullptr,&vision); Script s; s.startDelay=0;
            s.steps={condition(),action(Action::ClickMatch),action(Action::Else),condition(),action(Action::StopTask),action(Action::Else),action(Action::Click),action(Action::EndIf),action(Action::EndIf),action(Action::Click)};
            QString error; QSignalSpy finished(&engine,&Engine::finished); QVERIFY(engine.start(s,error)); QTRY_COMPARE(finished.count(),1);
            QCOMPARE(vision.calls.load(),first?1:2); QCOMPARE(input.clicks,first?2:0); QCOMPARE(engine.lastExecutionLog().imageChecks,quint64(first?1:2));
        }
    }
    void logContainsMatchingScoreAndOnlyExecutedBranch() {
        MemoryInput input; FakeVision vision; vision.results={true,false}; Engine engine(input,nullptr,&vision);
        Script s; s.startDelay=0; s.rounds=2; auto wait=action(Action::Wait); wait.duration=5; auto image=condition(); image.templateName="sample";
        s.steps={image,action(Action::ClickMatch),action(Action::Else),wait,action(Action::EndIf)};
        QString error; QSignalSpy logged(&engine,&Engine::executionLogged); QVERIFY(engine.start(s,error)); QTRY_COMPARE(logged.count(),1);
        const auto &log=engine.lastExecutionLog(); QCOMPARE(log.imageChecks,quint64(2)); QCOMPARE(log.imageHits,quint64(1)); QCOMPARE(log.actions,quint64(1));
        int checks=0,clicks=0,waits=0;
        for(const auto &e:log.entries) {
            if(e.action==actionName(Action::IfImage)) {
                ++checks; QVERIFY(e.detail.contains("88%")); QVERIFY(e.detail.contains("1.23 ms")); QVERIFY(e.detail.contains("0.57 ms"));
                if(e.round==0) { QVERIFY(e.detail.contains("97.25%")); QVERIFY(e.detail.contains("125%")); QVERIFY(e.detail.contains("(-160, 140)")); }
                else { QVERIFY(e.detail.contains(QStringLiteral("未找到"))); QVERIFY(!e.detail.contains(QStringLiteral("相似度 0"))); }
            }
            if(e.action==actionName(Action::ClickMatch)) { ++clicks; QCOMPARE(e.round,0); QVERIFY(e.detail.contains("(-160, 140)")); }
            if(e.action==actionName(Action::Wait)) { ++waits; QCOMPARE(e.round,1); }
        }
        QCOMPARE(checks,2); QCOMPARE(clicks,1); QCOMPARE(waits,1); QCOMPARE(ExecutionLog::load(log.save()).save(),log.save());
    }
    void trueFalseAndNested() {
        for(bool outer:{true,false}) {
            MemoryInput input; FakeVision vision; vision.results={outer,false}; Engine engine(input,nullptr,&vision);
            Script s; s.startDelay=0; s.steps={condition(),condition(),action(Action::ClickMatch),action(Action::Else),action(Action::ClickMatch),action(Action::EndIf),action(Action::Else),action(Action::Click),action(Action::EndIf)};
            QString error; QSignalSpy finished(&engine,&Engine::finished); QVERIFY2(engine.start(s,error),qPrintable(error)); QTRY_COMPARE(finished.count(),1);
            QCOMPARE(input.clicks,1); QCOMPARE(input.positions.size(),outer?1:0);
            if(outer) QCOMPARE(input.positions.first(),QPoint(-160,140)); QCOMPARE(vision.calls.load(),outer?2:1);
        }
    }
    void conditionsInsideLoops() {
        MemoryInput input; FakeVision vision; vision.results={true,false,true,false}; Engine engine(input,nullptr,&vision);
        Script s; s.startDelay=0; s.rounds=2; auto begin=action(Action::LoopBegin); begin.count=2;
        s.steps={begin,condition(),action(Action::ClickMatch),action(Action::EndIf),action(Action::LoopEnd)};
        QString error; QSignalSpy finished(&engine,&Engine::finished); QVERIFY2(engine.start(s,error),qPrintable(error)); QTRY_COMPARE(finished.count(),1);
        QCOMPARE(input.clicks,2); QCOMPARE(vision.calls.load(),4);
    }
    void stopAndDestructionCancelRecognition() {
        MemoryInput input; FakeVision vision; vision.waitForCancel=true;
        { Engine engine(input,nullptr,&vision); Script s; s.startDelay=0; s.steps={condition(),action(Action::ClickMatch),action(Action::EndIf)};
            QString error; QVERIFY(engine.start(s,error)); QTRY_COMPARE(vision.calls.load(),1); engine.stop(); QVERIFY(!engine.running()); QTest::qWait(50); QCOMPARE(input.clicks,0);
            const auto &log=engine.lastExecutionLog(); QCOMPARE(log.imageChecks,quint64(0)); QCOMPARE(log.outcome,QStringLiteral("停止")); QCOMPARE(log.entries.last().action,QStringLiteral("取消识别"));
            QVERIFY(engine.start(s,error)); QTRY_COMPARE(vision.calls.load(),2);
        }
        QCOMPARE(input.clicks,0);
    }
    void restartFromMatchCallbackDoesNotResumeOldBranch() {
        MemoryInput input; FakeVision vision; vision.results={true}; Engine engine(input,nullptr,&vision);
        bool replacement=false; int first=-1;
        connect(&engine,&Engine::progress,this,[&](int step,int,quint64) { if(replacement && step>=0 && first<0) first=step; });
        connect(&engine,&Engine::matchEvaluated,this,[&] {
            engine.stop(); replacement=true; Script next; next.startDelay=0; auto wait=action(Action::Wait); wait.duration=30; next.steps={wait,action(Action::Click)}; QString error; QVERIFY(engine.start(next,error));
        });
        Script s; s.startDelay=0; s.steps={condition(),action(Action::ClickMatch),action(Action::EndIf)}; QString error; QSignalSpy finished(&engine,&Engine::finished);
        QVERIFY(engine.start(s,error)); QTRY_COMPARE(finished.count(),1); QCOMPARE(first,0); QCOMPARE(input.clicks,1); QVERIFY(input.positions.isEmpty());
    }
    void nativeSizeAllWords() {
        QImage screen(fixtures()+"/words.png");
        for(const auto &item:words()) {
            const auto w=item.toObject(); TemplateMatcher matcher; QString error;
            QVERIFY2(matcher.prepare(QImage(fixtures()+"/"+w["file"].toString()),false,error),qPrintable(error));
            auto result=matcher.find(screen,88); QVERIFY2(result.found,qPrintable(QString("word id=%1").arg(w["id"].toInt())));
            QPoint center(w["x"].toInt()+w["width"].toInt()/2,w["y"].toInt()+w["height"].toInt()/2);
            QVERIFY2((result.center()-center).manhattanLength()<=4,qPrintable(w["word"].toString()+" incorrect center"));
            QImage blank(screen.size(),QImage::Format_RGB32); blank.fill(screen.pixel(0,0)); QVERIFY(!matcher.find(blank,88).found);
        }
    }
    void scaleAllWords_data() { QTest::addColumn<double>("scale"); for(double scale:{.5,.75,1.25,1.5,2.0}) QTest::newRow(qPrintable(QString::number(scale)))<<scale; }
    void scaleAllWords() {
        QFETCH(double,scale); const QImage original(fixtures()+"/words.png"); QImage screen=original.scaled(qRound(original.width()*scale),qRound(original.height()*scale),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        for(const auto &item:words()) {
            auto w=item.toObject(); TemplateMatcher matcher; QString error; QVERIFY(matcher.prepare(QImage(fixtures()+"/"+w["file"].toString()),true,error));
            auto result=matcher.find(screen,88); QVERIFY2(result.found,qPrintable(w["word"].toString()));
            QPoint center(qRound((w["x"].toInt()+w["width"].toInt()/2)*scale),qRound((w["y"].toInt()+w["height"].toInt()/2)*scale));
            QVERIFY2((result.center()-center).manhattanLength()<=8,qPrintable(QString("%1 wrong center %2,%3 expected %4,%5 scale %6").arg(w["word"].toString()).arg(result.center().x()).arg(result.center().y()).arg(center.x()).arg(center.y()).arg(result.scale)));
        }
    }
    void cancellationAndLowFeatureImage() {
        TemplateMatcher matcher; QString error; QImage blank(80,40,QImage::Format_RGB32); blank.fill(Qt::white); QVERIFY(!matcher.prepare(blank,true,error));
        QVERIFY(matcher.prepare(QImage(fixtures()+"/templates/01.png"),true,error)); std::atomic_bool cancel{true}; QVERIFY(matcher.find(QImage(fixtures()+"/words.png"),88,&cancel).cancelled);
    }
    void missingWordAmongOtherWords() {
        const QImage original(fixtures()+"/words.png");
        for(const auto &item:words()) {
            const auto w=item.toObject(); QImage screen=original;
            { QPainter painter(&screen); painter.fillRect(QRect(w["x"].toInt()-1,w["y"].toInt()-1,w["width"].toInt()+2,w["height"].toInt()+2),original.pixel(0,0)); }
            for(double scale:{.75,1.0,1.25,1.5}) {
                TemplateMatcher matcher; QString error; QVERIFY(matcher.prepare(QImage(fixtures()+"/"+w["file"].toString()),true,error));
                auto r=matcher.find(screen.scaled(qRound(screen.width()*scale),qRound(screen.height()*scale),Qt::IgnoreAspectRatio,Qt::SmoothTransformation),88);
                QVERIFY2(!r.found,qPrintable(QString("False positive for removed word id=%1 scale=%2 at %3,%4 score=%5").arg(w["id"].toInt()).arg(scale).arg(r.center().x()).arg(r.center().y()).arg(r.score)));
            }
        }
    }
    void actualHyperlinks_data() { QTest::addColumn<double>("scale"); for(double scale:{.5,.75,1.0,1.25,1.5,2.0}) QTest::newRow(qPrintable(QString::number(scale)))<<scale; }
    void actualHyperlinks() {
        if(QGuiApplication::platformName()=="offscreen") QSKIP("Requires a visible Windows desktop");
        QFETCH(double,scale); QImage original(fixtures()+"/words.png"); QWidget canvas; canvas.setWindowTitle("DianXu image matching acceptance"); canvas.setWindowFlag(Qt::WindowStaysOnTopHint);
        canvas.resize(qRound(original.width()*scale),qRound(original.height()*scale)); canvas.move(40,40); canvas.setAutoFillBackground(true);
        auto palette=canvas.palette(); palette.setColor(QPalette::Window,original.pixel(0,0)); canvas.setPalette(palette);
        QList<int> clicked; Script script; script.startDelay=0;
        for(auto item:words()) {
            auto w=item.toObject(); auto *link=new QLabel(&canvas); link->setTextFormat(Qt::RichText); link->setContentsMargins(0,0,0,0); link->setMargin(0); link->setAlignment(Qt::AlignCenter);
            link->setText(QString("<a href='%1'><img src='%2' width='%3' height='%4'/></a>").arg(w["id"].toInt()).arg(fixtures()+"/"+w["file"].toString()).arg(qRound(w["width"].toInt()*scale)).arg(qRound(w["height"].toInt()*scale)));
            link->setGeometry(qRound(w["x"].toInt()*scale),qRound(w["y"].toInt()*scale),qRound(w["width"].toInt()*scale),qRound(w["height"].toInt()*scale));
            connect(link,&QLabel::linkActivated,&canvas,[&clicked](const QString &id) { clicked.append(id.toInt()); });
            auto step=condition(); step.templatePng=png(QImage(fixtures()+"/"+w["file"].toString())); step.templateName=w["word"].toString(); step.limitRegion=true;
            script.steps.append(step); script.steps.append(action(Action::ClickMatch)); script.steps.append(action(Action::EndIf));
        }
        canvas.show(); canvas.raise(); canvas.activateWindow(); QTest::qWait(200);
        POINT origin{0,0}; QVERIFY(ClientToScreen(reinterpret_cast<HWND>(canvas.winId()),&origin)); RECT rect; QVERIFY(GetClientRect(reinterpret_cast<HWND>(canvas.winId()),&rect));
        for(auto &step:script.steps) if(step.action==Action::IfImage) step.searchRegion=QRect(origin.x,origin.y,rect.right,rect.bottom);
        WindowsInput input; Engine engine(input); QString error; QSignalSpy finished(&engine,&Engine::finished),failed(&engine,&Engine::failed),matches(&engine,&Engine::matchEvaluated);
        const QString exe=qEnvironmentVariable("DIANXU_ACCEPTANCE_EXE"); QProcess process; QTemporaryDir temp;
        auto stopPortable=qScopeGuard([&] { if(process.state()!=QProcess::NotRunning) { QProcess control; control.setProcessEnvironment(process.processEnvironment()); control.start(exe,{"--quit"}); control.waitForFinished(10000); process.waitForFinished(10000); } });
        if(exe.isEmpty()) {
            QVERIFY2(engine.start(script,error),qPrintable(error)); QTRY_VERIFY_WITH_TIMEOUT(finished.count() || failed.count(),30000);
            QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().first().toString()));
        } else {
            QVERIFY(temp.isValid()); script.startDelay=1000; QFile file(temp.filePath("acceptance.json")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(script.json()).toJson()); file.close();
            auto env=QProcessEnvironment::systemEnvironment(); env.insert("LIANDIANQI_TEST_SETTINGS",temp.path()); process.setProcessEnvironment(env);
            process.start(exe,{"--script",file.fileName(),"--run"}); QVERIFY(process.waitForStarted()); QTRY_COMPARE_WITH_TIMEOUT(clicked.size(),36,30000);
        }
        if(clicked.size()!=36 && !matches.isEmpty()) {
            qWarning()<<"click ids"<<clicked;
            for(int i=0;i<matches.size();++i) { auto r=qvariant_cast<MatchResult>(matches[i][1]); if(!r.found) qWarning()<<"missing id"<<i+1; }
            canvas.grab().save(QDir::tempPath()+QString("/dianxu-vision-failure-%1.png").arg(scale));
        }
        QTRY_COMPARE(clicked.size(),36);
        for(int i=0;i<36;++i) {
            if(clicked[i]!=i+1 && !matches.isEmpty()) {
                auto r=qvariant_cast<MatchResult>(matches[i][1]);
                qWarning()<<"mismatch step"<<i+1<<"clicked"<<clicked[i]<<"bounds"<<r.bounds<<"scale"<<r.scale<<"score"<<r.score;
                canvas.grab().save(QDir::tempPath()+QString("/dianxu-vision-failure-%1.png").arg(scale));
            }
            QCOMPARE(clicked[i],i+1);
        }
        if(!exe.isEmpty()) {
            auto savedLog=[&] { QSettings settings(temp.filePath("DianXu/LianDianQi.ini"),QSettings::IniFormat); settings.sync(); return ExecutionLog::load(settings.value("lastExecutionLog").toByteArray()); };
            QTRY_VERIFY_WITH_TIMEOUT(savedLog().valid(),10000); const auto log=savedLog();
            QCOMPARE(log.outcome,QStringLiteral("完成")); QCOMPARE(log.imageChecks,quint64(36)); QCOMPARE(log.imageHits,quint64(36)); QCOMPARE(log.actions,quint64(36));
            int checked=0; for(const auto &entry:log.entries) if(entry.action==actionName(Action::IfImage)) { ++checked; QVERIFY(entry.detail.contains(QStringLiteral("相似度"))); QVERIFY(entry.detail.contains(QStringLiteral("中心"))); }
            QCOMPARE(checked,36);
            QProcess control; control.setProcessEnvironment(process.processEnvironment()); control.start(exe,{"--quit"}); QVERIFY(control.waitForFinished(10000)); QCOMPARE(control.exitCode(),0);
            QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(),0); qInfo()<<"Portable executable activated all 36 hyperlinks and exited normally at scale"<<scale; return;
        }
        QList<double> captures,times; for(auto m:matches) { auto r=qvariant_cast<MatchResult>(m[1]); QVERIFY(r.found); captures.append(r.captureMicros/1000.0); times.append(r.matchMicros/1000.0); }
        std::sort(captures.begin(),captures.end()); std::sort(times.begin(),times.end());
        qInfo().noquote()<<QString("36/36 hyperlinks scale=%1 capture p50=%2ms p95=%3ms match p50=%4ms p95=%5ms").arg(scale).arg(captures[18],0,'f',3).arg(captures[34],0,'f',3).arg(times[18],0,'f',3).arg(times[34],0,'f',3);
    }
    void nativeRegionAndScreenBenchmarks() {
        if(QGuiApplication::platformName()=="offscreen") QSKIP("Requires a visible Windows desktop");
        QLabel canvas; QImage image(fixtures()+"/words.png"); canvas.setPixmap(QPixmap::fromImage(image)); canvas.resize(image.size()); canvas.move(40,40); canvas.setWindowFlag(Qt::WindowStaysOnTopHint); canvas.show(); QTest::qWait(200);
        POINT origin{0,0}; QVERIFY(ClientToScreen(reinterpret_cast<HWND>(canvas.winId()),&origin)); const qreal dpr=canvas.devicePixelRatioF();
        auto p=condition(); p.scaleMatch=true; p.searchRegion=QRect(origin.x,origin.y,qRound(image.width()*dpr),qRound(image.height()*dpr)); p.limitRegion=true;
        ScreenMatcher matcher; std::atomic_bool cancel{false}; auto found=matcher.recognize(p,cancel); QVERIFY(found.found);
        p.searchRegion=QRect(origin.x+qRound(400*dpr),origin.y+qRound(350*dpr),qRound(50*dpr),qRound(40*dpr)); auto miss=matcher.recognize(p,cancel); QVERIFY(!miss.found); QVERIFY(miss.error.isEmpty());
        p.searchRegion=QRect(90000,90000,100,100); QVERIFY(!matcher.recognize(p,cancel).error.isEmpty());
        for(bool limited:{true,false}) {
            p.limitRegion=limited; p.searchRegion=QRect(origin.x,origin.y,qRound(170*dpr),qRound(100*dpr)); QList<double> captures,times;
            for(int i=0;i<15;++i) { auto r=matcher.recognize(p,cancel); QVERIFY(r.found); captures.append(r.captureMicros/1000.0); times.append(r.matchMicros/1000.0); }
            std::sort(captures.begin(),captures.end()); std::sort(times.begin(),times.end());
            qInfo().noquote()<<QString("warm %1 capture p50=%2ms p95=%3ms match p50=%4ms p95=%5ms").arg(limited?"170x100 ROI":"full monitor").arg(captures[7],0,'f',3).arg(captures[14],0,'f',3).arg(times[7],0,'f',3).arg(times[14],0,'f',3);
        }
    }
    void nativeRectangleSelectionAcrossMonitors() {
        if(QGuiApplication::platformName()=="offscreen") QSKIP("Requires a visible Windows desktop");
        const auto monitors=ScreenCapture::monitors(); QVERIFY(!monitors.isEmpty());
        const QPoint begin=monitors.first().topLeft()+QPoint(130,160);
        const QPoint end=monitors.size()>1?monitors.last().topLeft()+QPoint(240,250):begin+QPoint(140,80);
        PositionPicker picker(nullptr,true);
        QTimer::singleShot(200,&picker,[&] {
            QVERIFY(SetPhysicalCursorPos(begin.x(),begin.y())); INPUT press={}; press.type=INPUT_MOUSE; press.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; QCOMPARE(SendInput(1,&press,sizeof(INPUT)),UINT(1));
            QTimer::singleShot(80,&picker,[&] {
                QVERIFY(SetPhysicalCursorPos(end.x(),end.y())); INPUT release={}; release.type=INPUT_MOUSE; release.mi.dwFlags=MOUSEEVENTF_LEFTUP; QCOMPARE(SendInput(1,&release,sizeof(INPUT)),UINT(1));
            });
        });
        QTimer::singleShot(4000,&picker,&QDialog::reject); QCOMPARE(picker.exec(),int(QDialog::Accepted)); QCOMPARE(picker.region(),QRect(begin,end).normalized());
    }
    void nativePhysicalMatchOnEachMonitor() {
        if(QGuiApplication::platformName()=="offscreen") QSKIP("Requires a visible Windows desktop");
        const QImage image(fixtures()+"/templates/01.png");
        for(auto *screen:QGuiApplication::screens()) {
            QLabel target; target.setAlignment(Qt::AlignCenter); target.setPixmap(QPixmap::fromImage(image)); target.resize(image.size()); target.setWindowFlag(Qt::WindowStaysOnTopHint);
            target.move(screen->geometry().topLeft()+QPoint(150,160)); target.show(); QTest::qWait(150);
            POINT origin{0,0}; QVERIFY(ClientToScreen(reinterpret_cast<HWND>(target.winId()),&origin)); RECT rect; GetClientRect(reinterpret_cast<HWND>(target.winId()),&rect);
            auto p=condition(); p.limitRegion=true; p.searchRegion=QRect(origin.x,origin.y,rect.right,rect.bottom);
            ScreenMatcher matcher; std::atomic_bool cancel{false}; auto result=matcher.recognize(p,cancel); QVERIFY2(result.found,qPrintable(result.error));
            QVERIFY2((result.center()-(p.searchRegion.topLeft()+QPoint(p.searchRegion.width()/2,p.searchRegion.height()/2))).manhattanLength()<=6,qPrintable(QString("found %1,%2 region %3,%4 %5x%6").arg(result.center().x()).arg(result.center().y()).arg(p.searchRegion.x()).arg(p.searchRegion.y()).arg(p.searchRegion.width()).arg(p.searchRegion.height())));
        }
    }
};
QTEST_MAIN(Tests)
#include "test_vision.moc"
