#include "window.h"
#include <QTest>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QTemporaryDir>

class FakeInput : public InputSink {
public:
    QStringList events;
    int failAt=-1;
    int calls=0;
    bool keyEvent(const InputKey &k,bool down,QString &e) override {
        if(calls++==failAt) { e="injected failure"; return false; }
        events<<QString("%1:%2").arg(k.code).arg(down?"down":"up"); return true;
    }
    bool move(const QPoint &p,QString &) override { events<<QString("move:%1,%2").arg(p.x()).arg(p.y()); return true; }
};
class Tests : public QObject {
    Q_OBJECT
private slots:
    void roundTrip() {
        Script s; for(int i=0;i<9;++i) { Step p; p.action=Action(i); s.steps.append(p); }
        s.steps.insert(8,Step{}); // Nonempty loop body.
        Script parsed; QString error; QVERIFY2(Script::parse(QJsonDocument(s.json()).toJson(),parsed,error),qPrintable(error));
        QCOMPARE(parsed.steps.size(),10); QCOMPARE(parsed.json(),s.json());
    }
    void rejectMalformed() {
        QString error; Script parsed;
        QVERIFY(!Script::parse("{}",parsed,error));
        Script s; Step p; p.action=Action::LoopEnd; s.steps={p}; QVERIFY(!s.validate(error));
        p.action=Action::LoopBegin; s.steps={p}; QVERIFY(!s.validate(error));
        Step end; end.action=Action::LoopEnd; s.steps={p,end}; QVERIFY(!s.validate(error));
        auto o=p.json(); o["count"]=1.5; QVERIFY(!Step::parse(o,p,error));
        o["count"]="2"; QVERIFY(!Step::parse(o,p,error));
        InputKey k; QVERIFY(!InputKey::parse({{"device","mouse"},{"code",3}},k,error));
    }
    void unfinishedDraftPreserved() {
        Script draft, parsed; Step loop; loop.action=Action::LoopBegin; draft.steps={loop}; QString error;
        auto bytes=QJsonDocument(draft.json()).toJson();
        QVERIFY(!Script::parse(bytes,parsed,error)); QVERIFY(Script::parse(bytes,parsed,error,true)); QCOMPARE(parsed.json(),draft.json());
        draft.steps.clear(); QVERIFY(Script::parse(QJsonDocument(draft.json()).toJson(),parsed,error,true)); QVERIFY(parsed.steps.isEmpty());
    }
    void finiteRepeat() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; Step p; p.action=Action::Repeat; p.count=3; p.interval=5; s.steps={p};
        QSignalSpy finished(&e,&Engine::finished); QString error; QVERIFY(e.start(s,error)); QTRY_COMPARE(finished.count(),1);
        QCOMPARE(sink.events.size(),6); QCOMPARE(e.eventCount(),quint64(3)); QCOMPARE(e.heldCount(),0); QVERIFY(!e.timerActive());
    }
    void longHoldStop() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; Step p; p.action=Action::Hold; p.duration=60000; s.steps={p};
        QString error; QVERIFY(e.start(s,error)); QTRY_COMPARE(e.heldCount(),1); e.stop(); QCOMPARE(sink.events,QStringList({"1:down","1:up"})); QVERIFY(!e.timerActive());
    }
    void infiniteHoldNoPolling() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; Step p; p.action=Action::Hold; p.duration=0; s.steps={p};
        QString error; QVERIFY(e.start(s,error)); QTRY_COMPARE(e.heldCount(),1); QVERIFY(e.running()); QVERIFY(!e.timerActive()); e.stop(); QCOMPARE(e.heldCount(),0);
    }
    void cancelCountdown() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=60000; s.steps={Step{}};
        QString error; QVERIFY(e.start(s,error)); e.stop(); QVERIFY(sink.events.isEmpty()); QVERIFY(!e.timerActive());
    }
    void nestedLoops() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; s.rounds=2;
        Step b; b.action=Action::LoopBegin; b.count=2; Step end; end.action=Action::LoopEnd;
        s.steps={b,b,Step{},end,end}; QString error; QSignalSpy f(&e,&Engine::finished); QVERIFY(e.start(s,error)); QTRY_COMPARE(f.count(),1); QCOMPARE(e.eventCount(),quint64(8));
    }
    void pressWaitRelease() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0;
        Step d; d.action=Action::Down; Step w; w.action=Action::Wait; w.duration=30; Step u; u.action=Action::Up;
        s.steps={d,w,u}; QString error; QSignalSpy f(&e,&Engine::finished); QVERIFY(e.start(s,error)); QTRY_COMPARE(f.count(),1);
        QCOMPARE(sink.events,QStringList({"1:down","1:up"}));
    }
    void releaseAtNaturalEnd() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; Step d; d.action=Action::Down; s.steps={d};
        QString error; QSignalSpy f(&e,&Engine::finished); QVERIFY(e.start(s,error)); QTRY_COMPARE(f.count(),1); QCOMPARE(sink.events,QStringList({"1:down","1:up"}));
    }
    void latchedDown() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; s.latch=true; Step d; d.action=Action::Down; s.steps={d};
        QString error; QVERIFY(e.start(s,error)); QTRY_COMPARE(e.heldCount(),1); QTest::qWait(20); QVERIFY(e.running()); QVERIFY(!e.timerActive()); e.stop(); QCOMPARE(e.heldCount(),0);
    }
    void releaseAfterInputFailure() {
        FakeInput sink; sink.failAt=1; Engine e(sink); Script s; s.startDelay=0; s.steps={Step{}};
        QString error; QSignalSpy failed(&e,&Engine::failed); QVERIFY(e.start(s,error)); QTRY_VERIFY(failed.count()>0);
        QVERIFY(!e.running()); QCOMPARE(sink.events,QStringList({"1:down","1:up"})); QCOMPARE(e.heldCount(),0);
    }
    void destructionReleases() {
        FakeInput sink; { Engine e(sink); Script s; s.startDelay=0; Step p; p.action=Action::Hold; p.duration=0; s.steps={p}; QString error; QVERIFY(e.start(s,error)); QTRY_COMPARE(e.heldCount(),1); }
        QCOMPARE(sink.events,QStringList({"1:down","1:up"}));
    }
    void infiniteRepeatStop() {
        FakeInput sink; Engine e(sink); Script s; s.startDelay=0; Step p; p.action=Action::Repeat; p.count=0; p.interval=5; s.steps={p};
        QString error; QVERIFY(e.start(s,error)); QTRY_VERIFY(e.eventCount()>3); e.stop(); auto count=sink.events.size(); QTest::qWait(30); QCOMPARE(sink.events.size(),count); QCOMPARE(e.heldCount(),0);
    }
    void extendedKeyAndSides() {
        InputKey right{false,VK_RCONTROL,0,true}; auto p=WindowsInput::packet(right,true);
        QVERIFY(p.ki.dwFlags & KEYEVENTF_EXTENDEDKEY); QVERIFY(p.ki.dwFlags & KEYEVENTF_SCANCODE);
        QVERIFY(WindowsInput::packet(right,false).ki.dwFlags & KEYEVENTF_KEYUP);
        InputKey captured{false,VK_RCONTROL,0x1d,true}; QCOMPARE(right.identity(),captured.identity());
        InputKey left{false,VK_LCONTROL,0,false}; QVERIFY(left.identity()!=right.identity());
        InputKey mouse{true,VK_XBUTTON2,0,false}; QCOMPARE(WindowsInput::packet(mouse,true).mi.mouseData,DWORD(XBUTTON2));
    }
    void hotkeyValidationAndRollback() {
        UINT m,v; QVERIFY(Hotkeys::decode(QKeySequence("Ctrl+F6"),m,v)); QCOMPARE(v,UINT(VK_F6)); QVERIFY(m & MOD_CONTROL);
        QVERIFY(!Hotkeys::decode(QKeySequence("F12"),m,v));
        Hotkeys h; QString error; QVERIFY(h.set(QKeySequence("Ctrl+Shift+F19"),QKeySequence("Ctrl+Shift+F20"),QKeySequence("Ctrl+Shift+F21"),error));
        QVERIFY(!h.set(QKeySequence("F6"),QKeySequence("F6"),QKeySequence("F7"),error));
        QVERIFY(RegisterHotKey(nullptr,50,MOD_NOREPEAT,VK_F22));
        QVERIFY(!h.set(QKeySequence("F19"),QKeySequence("F22"),QKeySequence("F21"),error));
        UnregisterHotKey(nullptr,50);
        QVERIFY(!RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_SHIFT,VK_F19));
        h.clear(); QVERIFY(RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_SHIFT,VK_F19)); UnregisterHotKey(nullptr,51);
    }
    void uiEditAndPersist() {
        Window w(nullptr,true); w.show(); auto *tabs=w.findChild<QTabWidget *>("tabs"); tabs->setCurrentIndex(1);
        auto *list=w.findChild<QListWidget *>("flow"); QCOMPARE(list->count(),2); list->setCurrentRow(0);
        auto *action=w.findChild<QComboBox *>("stepAction"); action->setCurrentIndex(int(Action::Wait));
        QCOMPARE(w.currentScript().steps[0].action,Action::Wait);
        Script s; s.steps={Step{}}; w.setScript(s); QCOMPARE(list->count(),1);
        QTemporaryDir dir; QFile f(dir.filePath("test.json")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QJsonDocument(s.json()).toJson()); f.close();
        QVERIFY(w.loadScript(f.fileName())); QCOMPARE(w.currentScript().json(),s.json());
        w.close(); QVERIFY(!w.execution().timerActive()); QVERIFY(!w.execution().running());
    }
    void timeUnitsPreserveScriptTiming() {
        StepEditor editor(false); Step s; s.action=Action::Wait; s.duration=1234; editor.setValue(s);
        auto *time=editor.findChild<TimeField *>("stepDuration"); QVERIFY(time);
        auto *units=time->findChild<QComboBox *>("unit"); auto *number=time->findChild<QDoubleSpinBox *>("number");
        QCOMPARE(units->currentText(),QString("s")); QCOMPARE(number->value(),1.234);
        units->setCurrentIndex(0); QCOMPARE(number->value(),1234.0); QCOMPARE(editor.value().duration,1234);
        units->setCurrentIndex(2); QCOMPARE(editor.value().duration,1234);
        number->setValue(0.125); QCOMPARE(editor.value().duration,7500);
        units->setCurrentIndex(1); QCOMPARE(number->value(),7.5); QCOMPARE(editor.value().duration,7500);
        number->setValue(0); QCOMPARE(editor.value().duration,0); QVERIFY(number->specialValueText().isEmpty());
        TimeField interval(5,3600000,5); interval.findChild<QComboBox *>("unit")->setCurrentIndex(2); QCOMPARE(interval.value(),5);
        interval.setValue(3600000); QCOMPARE(interval.value(),3600000);
    }
    void recordingBalancesAndSkipsAutorepeat() {
        Window w(nullptr,true); w.show(); auto *list=w.findChild<QListWidget *>("flow"); list->clear();
        auto *tabs=w.findChild<QTabWidget *>("tabs"); tabs->setCurrentIndex(1);
        QPushButton *record=nullptr;
        for(auto *b:w.findChildren<QPushButton *>()) if(b->text()==QStringLiteral("● 录制输入")) record=b;
        QVERIFY(record); QTest::mouseClick(record,Qt::LeftButton);
        auto *monitor=w.findChild<InputMonitor *>("recordingMonitor"); QVERIFY(monitor);
        InputKey a{false,'Q',0,false}; emit monitor->observed(a,true,QPoint(0,0),1000);
        emit monitor->observed(a,true,QPoint(0,0),1100); // Autorepeat must not duplicate Down.
        emit monitor->observed(a,false,QPoint(0,0),1200);
        InputKey b{false,'W',0,false}; emit monitor->observed(b,true,QPoint(0,0),1500);
        QCoreApplication::processEvents(); QTest::mouseClick(record,Qt::LeftButton);
        auto s=w.currentScript(); QCOMPARE(s.steps.size(),6);
        QCOMPARE(s.steps[1].duration,200); QCOMPARE(s.steps[3].duration,300);
        QCOMPARE(s.steps.last().action,Action::Up); QCOMPARE(s.steps.last().key.code,int('W'));
        QVERIFY(!w.execution().running()); w.close();
    }
};
QTEST_MAIN(Tests)
#include "test_clicker.moc"
