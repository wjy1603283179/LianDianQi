#include "window.h"
#include "positionpicker.h"
#include <QTest>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QScopeGuard>
#include <QDir>
#include <QVBoxLayout>
#include <QStyleFactory>
#include <QStyleOptionComboBox>
#include <QTabBar>
#include <QScreen>
#include <QMouseEvent>
#include <QWindow>
#include <QBuffer>
#include <QPainter>

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
class ClickTarget : public QWidget {
public:
    int clicks=0,releases=0;
protected:
    void mousePressEvent(QMouseEvent *event) override { ++clicks; event->accept(); }
    void mouseReleaseEvent(QMouseEvent *event) override { ++releases; event->accept(); }
};
static QList<QRect> nativeMonitorRects() {
    QList<QRect> rects;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT rect,LPARAM data)->BOOL {
        reinterpret_cast<QList<QRect> *>(data)->append(QRect(rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top));
        return TRUE;
    },reinterpret_cast<LPARAM>(&rects));
    return rects;
}
static QList<QPoint> monitorTestPoints(const QRect &rect) {
    return {rect.topLeft(),rect.topRight(),rect.bottomLeft(),rect.bottomRight(),rect.center(),
        QPoint(rect.center().x(),rect.top()),QPoint(rect.center().x(),rect.bottom()),
        QPoint(rect.left(),rect.center().y()),QPoint(rect.right(),rect.center().y())};
}
static UINT nativeClickAt(const QPoint &point) {
    INPUT click[3]={}; for(auto &event:click) event.type=INPUT_MOUSE;
    click[0].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    click[0].mi.dx=LONG((qint64(point.x()-GetSystemMetrics(SM_XVIRTUALSCREEN))*65536+32768)/GetSystemMetrics(SM_CXVIRTUALSCREEN));
    click[0].mi.dy=LONG((qint64(point.y()-GetSystemMetrics(SM_YVIRTUALSCREEN))*65536+32768)/GetSystemMetrics(SM_CYVIRTUALSCREEN));
    click[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; click[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    return SendInput(3,click,sizeof(INPUT));
}
class Tests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QApplication::setStyle(QStyleFactory::create("Fusion")); }
    void imageEditorAndBranchLayout() {
        Window w(nullptr,true); w.show(); auto *tabs=w.findChild<QTabWidget *>("tabs");
        Step p; p.action=Action::IfImage; QImage image(64,40,QImage::Format_RGB32); image.fill(Qt::white); { QPainter painter(&image); painter.fillRect(10,8,30,20,Qt::blue); }
        QBuffer buffer(&p.templatePng); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG"); buffer.close();
        p.templateName=QStringLiteral("测试模板"); p.limitRegion=true; p.searchRegion=QRect(-200,-80,500,300); p.similarity=93;
        Script script; Step click; click.action=Action::ClickMatch; Step alternative; alternative.action=Action::Else; Step end; end.action=Action::EndIf;
        script.steps={p,click,alternative,Step{},end}; w.setScript(script); QCoreApplication::processEvents(); QCOMPARE(w.currentScript().json(),script.json());
        auto *list=w.findChild<QListWidget *>("flow"); QCOMPARE(list->item(1)->data(Qt::UserRole+2).toInt(),1); QCOMPARE(list->item(2)->data(Qt::UserRole+2).toInt(),0);
        QCOMPARE(list->item(3)->data(Qt::UserRole+2).toInt(),1); QCOMPARE(list->item(4)->data(Qt::UserRole+2).toInt(),0);
        auto *similarity=w.findChild<QSpinBox *>("imageSimilarity"); // Quick editor has hidden vision controls too.
        auto editors=w.findChildren<StepEditor *>(); StepEditor *flowEditor=nullptr; for(auto *e:editors) if(e->findChild<QComboBox *>("stepAction")) flowEditor=e;
        QVERIFY(flowEditor); similarity=flowEditor->findChild<QSpinBox *>("imageSimilarity"); QCOMPARE(similarity->value(),93); similarity->setValue(96); QCOMPARE(w.currentScript().steps[0].similarity,96);
        QCOMPARE(w.currentScript().steps[0].templatePng,p.templatePng); QCOMPARE(w.currentScript().steps[0].searchRegion,p.searchRegion);
        for(QSize size:{QSize(800,480),QSize(980,680),QSize(2560,1440)}) {
            w.resize(size); QCoreApplication::processEvents(); QPoint navigation=tabs->tabBar()->mapTo(&w,QPoint());
            tabs->setCurrentIndex(0); QCoreApplication::processEvents(); QCOMPARE(tabs->tabBar()->mapTo(&w,QPoint()),navigation);
            tabs->setCurrentIndex(1); QCoreApplication::processEvents(); QCOMPARE(tabs->tabBar()->mapTo(&w,QPoint()),navigation);
            auto *scroll=w.findChild<QScrollArea *>("stepScroll"); scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QCoreApplication::processEvents();
            auto *choose=flowEditor->findChild<QPushButton *>("imageRegionSelect"); QVERIFY(choose->isVisible()); QVERIFY(choose->width()>=50); QVERIFY(choose->height()>=25);
        }
        script.steps={p,click,end}; w.setScript(script); auto *addCombo=w.findChild<QComboBox *>("addAction"); addCombo->setCurrentIndex(int(Action::Else));
        QPushButton *add=nullptr; for(auto *b:w.findChildren<QPushButton *>()) if(b->text()==QStringLiteral("＋ 添加")) add=b;
        QVERIFY(add); add->click(); QCOMPARE(w.currentScript().steps[2].action,Action::Else); QCOMPARE(w.currentScript().steps[3].action,Action::EndIf); QString error; QVERIFY2(w.currentScript().validate(error),qPrintable(error));
        add->click(); QCOMPARE(list->count(),4); // Duplicate else is rejected without damaging the flow.
        w.close();
    }
    void imageSelectionCancelRestoresOwnerAndBlocksControls() {
        if(QGuiApplication::platformName()=="offscreen") QSKIP("Requires the native Windows UI backend.");
        Window w(nullptr,true); Step p; p.action=Action::IfImage; p.limitRegion=true; p.searchRegion=QRect(-10,20,200,120);
        Script s; s.steps={p}; w.setScript(s); w.showMaximized(); QTest::qWait(80);
        StepEditor *editor=nullptr; for(auto *e:w.findChildren<StepEditor *>()) if(e->findChild<QComboBox *>("stepAction")) editor=e;
        QVERIFY(editor); auto *button=editor->findChild<QPushButton *>("imageRegionSelect"); QVERIFY(button);
        QTimer::singleShot(50,&w,[&] { QVERIFY(w.property("captureInProgress").toBool()); w.toggleTask(); QVERIFY(!w.execution().running()); });
        QTimer::singleShot(200,&w,[&] { auto *picker=qobject_cast<PositionPicker *>(QApplication::activeModalWidget()); QVERIFY(picker); QTest::keyClick(picker,Qt::Key_Escape); });
        button->click(); QVERIFY(w.isVisible()); QVERIFY(w.isMaximized()); QVERIFY(!w.property("captureInProgress").toBool()); QCOMPARE(w.currentScript().steps[0].searchRegion,p.searchRegion);
        QTRY_COMPARE(GetForegroundWindow(),reinterpret_cast<HWND>(w.winId())); w.close();
    }
    void imageSelectionShutdownDuringPreparation() {
        Window w(nullptr,true); Step p; p.action=Action::IfImage; p.limitRegion=true; Script s; s.steps={p}; w.setScript(s); w.show();
        StepEditor *editor=nullptr; for(auto *e:w.findChildren<StepEditor *>()) if(e->findChild<QComboBox *>("stepAction")) editor=e;
        QVERIFY(editor); QTimer::singleShot(40,&w,&Window::shutdown); editor->findChild<QPushButton *>("imageRegionSelect")->click();
        QVERIFY(!w.isVisible()); QVERIFY(!w.execution().running()); QVERIFY(!w.property("captureInProgress").toBool()); w.close();
    }
    void nativeTemplateCaptureExcludesOverlay() {
        if(QGuiApplication::platformName()=="offscreen") QSKIP("Requires the native Windows UI backend.");
        POINT previous={}; GetPhysicalCursorPos(&previous); auto restoreCursor=qScopeGuard([&] { SetPhysicalCursorPos(previous.x,previous.y); });
        QImage image(80,40,QImage::Format_RGB32); image.fill(Qt::white); { QPainter painter(&image); painter.fillRect(10,8,30,20,Qt::blue); }
        QLabel target; target.setAlignment(Qt::AlignCenter); target.setPixmap(QPixmap::fromImage(image)); target.resize(image.size()); target.move(60,60); target.setWindowFlag(Qt::WindowStaysOnTopHint); target.show(); QTest::qWait(100);
        POINT origin{0,0}; QVERIFY(ClientToScreen(reinterpret_cast<HWND>(target.winId()),&origin)); RECT client={}; GetClientRect(reinterpret_cast<HWND>(target.winId()),&client);
        const double ratio=target.devicePixelRatioF(); const QSize size(qRound(80*ratio),qRound(40*ratio));
        const QPoint begin(origin.x+(client.right-size.width())/2,origin.y+(client.bottom-size.height())/2),end=begin+QPoint(size.width()-1,size.height()-1);
        Window w(nullptr,true); Step p; p.action=Action::IfImage; Script s; s.steps={p}; w.setScript(s); w.show();
        StepEditor *editor=nullptr; for(auto *e:w.findChildren<StepEditor *>()) if(e->findChild<QComboBox *>("stepAction")) editor=e; QVERIFY(editor);
        QTimer::singleShot(200,&w,[&] {
            QVERIFY(qobject_cast<PositionPicker *>(QApplication::activeModalWidget())); QVERIFY(SetPhysicalCursorPos(begin.x(),begin.y())); INPUT press={}; press.type=INPUT_MOUSE; press.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; QCOMPARE(SendInput(1,&press,sizeof(INPUT)),UINT(1));
            QTimer::singleShot(80,&w,[&] { QVERIFY(SetPhysicalCursorPos(end.x(),end.y())); INPUT release={}; release.type=INPUT_MOUSE; release.mi.dwFlags=MOUSEEVENTF_LEFTUP; QCOMPARE(SendInput(1,&release,sizeof(INPUT)),UINT(1)); });
        });
        QTimer::singleShot(4000,&w,[&] { if(auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        editor->findChild<QPushButton *>("templateCapture")->click(); const QImage captured=QImage::fromData(w.currentScript().steps[0].templatePng,"PNG");
        QVERIFY(!captured.isNull()); QCOMPARE(captured.size(),size); QCOMPARE(captured.pixelColor(0,0),QColor(Qt::white)); QCOMPARE(captured.pixelColor(qRound(20*ratio),qRound(15*ratio)),QColor(Qt::blue));
        QVERIFY(w.isVisible()); QVERIFY(!w.property("captureInProgress").toBool()); w.close();
    }
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
    void clearedHotkeysAndEditingPause() {
        UINT m,v; QVERIFY(!Hotkeys::decode(QKeySequence("Esc"),m,v)); QVERIFY(!Hotkeys::decode(QKeySequence("Ctrl+Esc"),m,v));
        Hotkeys h; QString error;
        QVERIFY(h.set(QKeySequence("Ctrl+Alt+F19"),QKeySequence(),QKeySequence(),error));
        QVERIFY(h.setPaused(true,error)); QVERIFY(RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT,VK_F19));
        QVERIFY(!h.setPaused(false,error)); UnregisterHotKey(nullptr,51); QVERIFY(h.setPaused(false,error));
        QVERIFY(!RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT,VK_F19));
        QVERIFY(h.set(QKeySequence(),QKeySequence(),QKeySequence(),error));
        QVERIFY(h.sequence(1).isEmpty()); QVERIFY(RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT,VK_F19)); UnregisterHotKey(nullptr,51);
    }
    void shortcutCaptureIsImmediateAndEscClears() {
        QWidget parent; QVBoxLayout layout(&parent); auto *field=new ShortcutField(QKeySequence("F19")); layout.addWidget(field); parent.show(); parent.activateWindow();
        field->setFocus(); QCoreApplication::processEvents(); QSignalSpy changed(field,&ShortcutField::keySequenceChanged);
        QTest::keyClick(field,Qt::Key_F20,Qt::ControlModifier|Qt::ShiftModifier);
        QCOMPARE(field->keySequence(),QKeySequence("Ctrl+Shift+F20")); QCOMPARE(changed.count(),1); QVERIFY(!field->hasFocus());
        field->setFocus(); QTest::keyClick(field,Qt::Key_Escape); QVERIFY(field->keySequence().isEmpty()); QCOMPARE(changed.count(),2);
        field->setFocus(); QTest::keyPress(field,Qt::Key_Control); QVERIFY(field->keySequence().isEmpty()); QTest::keyRelease(field,Qt::Key_Control);
    }
    void shortcutClearPersistsAcrossRestart() {
        QTemporaryDir dir; auto previousFormat=QSettings::defaultFormat(); QString previousOrg=QCoreApplication::organizationName(),previousApp=QCoreApplication::applicationName();
        auto restore=qScopeGuard([&] { QSettings::setDefaultFormat(previousFormat); QCoreApplication::setOrganizationName(previousOrg); QCoreApplication::setApplicationName(previousApp); });
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("DianXuTests"); QCoreApplication::setApplicationName("ShortcutPersistence");
        { QSettings st; st.setValue("toggle","Ctrl+Shift+F19"); st.setValue("stop","Ctrl+Shift+F20"); st.setValue("record","Ctrl+Shift+F21"); }
        { Window w; w.show(); w.findChild<QTabWidget *>("tabs")->setCurrentIndex(2); auto *field=w.findChild<ShortcutField *>("toggleShortcut");
          field->setFocus(); QCoreApplication::processEvents(); QTest::keyClick(field,Qt::Key_Escape); QVERIFY(field->keySequence().isEmpty());
          QVERIFY(RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_SHIFT,VK_F19)); UnregisterHotKey(nullptr,51); w.close(); }
        { QSettings st; QVERIFY(st.contains("toggle")); QVERIFY(st.value("toggle").toString().isEmpty()); }
        { Window w; QVERIFY(w.findChild<ShortcutField *>("toggleShortcut")->keySequence().isEmpty()); QCOMPARE(w.findChild<ShortcutField *>("stopShortcut")->keySequence(),QKeySequence("Ctrl+Shift+F20")); w.close(); }
    }
    void shortcutResetRestoresOnlySelectedDefault() {
        Window w(nullptr,true); auto *toggle=w.findChild<ShortcutField *>("toggleShortcut"),*stop=w.findChild<ShortcutField *>("stopShortcut"),*record=w.findChild<ShortcutField *>("recordShortcut");
        toggle->setKeySequence(QKeySequence("Ctrl+Alt+F19")); stop->setKeySequence(QKeySequence("Ctrl+Alt+F20")); record->setKeySequence(QKeySequence("Ctrl+Alt+F21"));
        w.findChild<QPushButton *>("toggleShortcutReset")->click(); QCOMPARE(toggle->keySequence(),QKeySequence("F6"));
        QCOMPARE(stop->keySequence(),QKeySequence("Ctrl+Alt+F20")); QCOMPARE(record->keySequence(),QKeySequence("Ctrl+Alt+F21")); w.close();
    }
    void shortcutUnavailableAtLaunchDoesNotEraseSavedBindings() {
        QTemporaryDir dir; const auto previousFormat=QSettings::defaultFormat();
        const QString previousOrg=QCoreApplication::organizationName(),previousApp=QCoreApplication::applicationName();
        auto restore=qScopeGuard([&] {
            UnregisterHotKey(nullptr,50); UnregisterHotKey(nullptr,51); QSettings::setDefaultFormat(previousFormat);
            QCoreApplication::setOrganizationName(previousOrg); QCoreApplication::setApplicationName(previousApp);
        });
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
        QCoreApplication::setOrganizationName("DianXuTests"); QCoreApplication::setApplicationName("ShortcutConflictPersistence");
        const QString toggle="Ctrl+Alt+Shift+F19",stop="Ctrl+Alt+Shift+F20",record="Ctrl+Alt+Shift+F21";
        { QSettings settings; settings.setValue("toggle",toggle); settings.setValue("stop",stop); settings.setValue("record",record); }
        QVERIFY(RegisterHotKey(nullptr,50,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT|MOD_SHIFT,VK_F19));
        { Window window; window.close(); }
        { QSettings settings; QCOMPARE(settings.value("toggle").toString(),toggle); QCOMPARE(settings.value("stop").toString(),stop); QCOMPARE(settings.value("record").toString(),record); }
        UnregisterHotKey(nullptr,50);
        { Window window; QCOMPARE(window.findChild<ShortcutField *>("toggleShortcut")->keySequence(),QKeySequence(toggle));
          QVERIFY(!RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT|MOD_SHIFT,VK_F19)); window.close(); }
        QVERIFY(RegisterHotKey(nullptr,51,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT|MOD_SHIFT,VK_F19)); UnregisterHotKey(nullptr,51);
    }
    void responsiveCoordinatesAndPageOptions() {
        Window w(nullptr,true); w.show(); auto *tabs=w.findChild<QTabWidget *>("tabs");
        auto *quick=w.findChild<StepEditor *>(); auto *fixed=w.findChild<QCheckBox *>("quickFixed"); auto *rounds=w.findChild<QSpinBox *>("scriptRounds");
        QVERIFY(w.findChild<QLineEdit *>("quickInput")->isReadOnly()); QVERIFY(!w.findChild<QComboBox *>("quickInput")); fixed->setChecked(true);
        const QString visualOutput=qEnvironmentVariable("LIANDIANQI_VISUAL_OUTPUT"); if(!visualOutput.isEmpty()) QDir().mkpath(visualOutput);
        auto verifyCoordinates=[&w](const char *prefix,const char *areaName) {
            auto *area=w.findChild<QScrollArea *>(areaName); auto *pick=w.findChild<QPushButton *>(QString(prefix)+"PositionCapture"); auto *x=w.findChild<QSpinBox *>(QString(prefix)+"X"),*y=w.findChild<QSpinBox *>(QString(prefix)+"Y");
            area->ensureWidgetVisible(pick,0,0); QCoreApplication::processEvents();
            QVERIFY(pick->isVisible()); QVERIFY(pick->height()>=pick->minimumSizeHint().height());
            QVERIFY(area->viewport()->rect().contains(QRect(pick->mapTo(area->viewport(),QPoint()),pick->size())));
            for(auto *coordinate:{x,y}) { QCOMPARE(coordinate->buttonSymbols(),QAbstractSpinBox::NoButtons); QVERIFY(coordinate->height()>=coordinate->minimumSizeHint().height()); QVERIFY(coordinate->width()<=350); }
        };
        for(const QSize size:{QSize(800,480),QSize(980,680),QSize(2560,1440)}) {
            w.resize(size); tabs->setCurrentIndex(0); QCoreApplication::processEvents(); QVERIFY(!rounds->isVisible());
            verifyCoordinates("quick","quickScroll"); QVERIFY(w.findChild<QWidget *>("workspace")->width()<=1120); QVERIFY(quick->width()<=732);
            for(auto *time:w.findChildren<TimeField *>()) if(time->isVisible()) {
                auto *unit=time->findChild<QComboBox *>("unit"); QStyleOptionComboBox option; option.initFrom(unit);
                const QRect text=unit->style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,unit);
                QVERIFY2(text.width()>=unit->fontMetrics().horizontalAdvance("min"),qPrintable(QString("unit text area %1 px, label %2 px, widget %3 px").arg(text.width()).arg(unit->fontMetrics().horizontalAdvance("min")).arg(unit->width())));
            }
            if(!visualOutput.isEmpty()) w.grab().save(visualOutput+QString("/quick-%1.png").arg(size.width()));
            Script s; Step step; step.action=Action::Repeat; step.fixedPosition=true; step.position=QPoint(-1787,272); s.steps={step}; w.setScript(s); QCoreApplication::processEvents(); QVERIFY(rounds->isVisible());
            verifyCoordinates("step","stepScroll"); QCOMPARE(w.currentScript().steps[0].position,QPoint(-1787,272));
            if(!visualOutput.isEmpty()) w.grab().save(visualOutput+QString("/flow-%1.png").arg(size.width()));
            tabs->setCurrentIndex(2); QCoreApplication::processEvents(); QVERIFY(!rounds->isVisible());
            if(!visualOutput.isEmpty()) w.grab().save(visualOutput+QString("/settings-%1.png").arg(size.width()));
        }
        if(!visualOutput.isEmpty()) {
            w.showMaximized(); tabs->setCurrentIndex(0); QTest::qWait(100); verifyCoordinates("quick","quickScroll"); w.grab().save(visualOutput+"/quick-maximized.png");
            tabs->setCurrentIndex(1); QTest::qWait(100); verifyCoordinates("step","stepScroll"); w.grab().save(visualOutput+"/flow-maximized.png");
            tabs->setCurrentIndex(2); QTest::qWait(100); w.grab().save(visualOutput+"/settings-maximized.png");
            auto *combo=w.findChild<QComboBox *>("quickAction"); tabs->setCurrentIndex(0); combo->showPopup(); QTest::qWait(100);
            combo->view()->window()->grab().save(visualOutput+"/action-dropdown.png"); combo->hidePopup();
        }
        w.close();
    }
    void pageSwitchKeepsNavigationAndControlsStationary() {
        Window w(nullptr,true); w.show(); auto *tabs=w.findChild<QTabWidget *>("tabs");
        auto rect=[&w](QWidget *widget) { return QRect(widget->mapTo(&w,QPoint()),widget->size()); };
        for(const QSize size:{QSize(800,480),QSize(980,680),QSize(2560,1440)}) {
            w.resize(size); tabs->setCurrentIndex(0); QCoreApplication::processEvents();
            const QRect navigation=rect(tabs->tabBar()),brand=rect(w.findChild<QLabel *>("brand")),start=rect(w.findChild<QPushButton *>("primary")),delay=rect(w.findChild<TimeField *>("startDelay"));
            for(int page:{1,2,0,1,0,2,0}) {
                tabs->setCurrentIndex(page); QCoreApplication::processEvents();
                QCOMPARE(rect(tabs->tabBar()),navigation); QCOMPARE(rect(w.findChild<QLabel *>("brand")),brand);
                QCOMPARE(rect(w.findChild<QPushButton *>("primary")),start); QCOMPARE(rect(w.findChild<TimeField *>("startDelay")),delay);
            }
        }
        w.close();
    }
    void pickerWaitsForReleaseAndEscPreservesCoordinates() {
        QWidget owner; owner.show(); PositionPicker picker(&owner); POINT saved; QVERIFY(GetPhysicalCursorPos(&saved));
        auto restore=qScopeGuard([&] { SetPhysicalCursorPos(saved.x,saved.y); });
        QPoint expected;
        QTimer::singleShot(5000,&picker,&QDialog::reject);
        QTimer::singleShot(0,&picker,[&] {
            QTest::mousePress(&picker,Qt::LeftButton,Qt::NoModifier,QPoint(50,50)); QVERIFY(picker.isVisible());
            const qreal scale=picker.devicePixelRatioF(); POINT point{qRound(50*scale),qRound(50*scale)};
            QVERIFY(ClientToScreen(reinterpret_cast<HWND>(picker.winId()),&point) || GetPhysicalCursorPos(&point)); expected=QPoint(point.x,point.y);
            QTest::mouseRelease(&picker,Qt::LeftButton,Qt::NoModifier,QPoint(70,70));
        });
        QCOMPARE(picker.exec(),int(QDialog::Accepted)); QCOMPARE(picker.position(),expected); QVERIFY(!picker.isVisible());
        Window w(nullptr,true); w.show(); auto *editor=w.findChild<StepEditor *>(); Step step; step.fixedPosition=true; step.position=QPoint(-123,456); editor->setValue(step); QSignalSpy changed(editor,&StepEditor::changed);
        QTimer::singleShot(5000,&w,[&] { if(auto *dialog=QApplication::activeModalWidget()) dialog->close(); });
        QTimer::singleShot(0,&w,[&] { auto *dialog=qobject_cast<PositionPicker *>(QApplication::activeModalWidget()); QVERIFY(dialog); QTest::keyClick(dialog,Qt::Key_Escape); });
        w.findChild<QPushButton *>("quickPositionCapture")->click(); QCOMPARE(editor->value().position,step.position); QCOMPARE(changed.count(),0); QVERIFY(w.isVisible()); w.close();
    }
    void pickerEscDuringMousePressAbsorbsRelease() {
        QWidget owner; owner.show();
        for(Qt::MouseButton button:{Qt::LeftButton,Qt::RightButton}) {
            PositionPicker picker(&owner);
            QTimer::singleShot(0,&picker,[&] {
                QTest::mousePress(&picker,button,Qt::NoModifier,QPoint(50,50));
                QTest::keyClick(&picker,Qt::Key_Escape);
                QVERIFY(picker.isVisible());
                QTest::mouseRelease(&picker,button,Qt::NoModifier,QPoint(70,70));
            });
            QTimer::singleShot(5000,&picker,&QDialog::reject);
            QCOMPARE(picker.exec(),int(QDialog::Rejected)); QVERIFY(!picker.isVisible());
        }
    }
    void nativePickerBlocksUnderlyingClickAndRestoresWindow() {
        if(QGuiApplication::platformName()!=QStringLiteral("windows")) QSKIP("Requires the native Windows UI backend; run separately with QT_QPA_PLATFORM=windows.");
        POINT saved; QVERIFY(GetPhysicalCursorPos(&saved)); HWND foreground=GetForegroundWindow();
        auto restore=qScopeGuard([&] { SetPhysicalCursorPos(saved.x,saved.y); if(foreground) SetForegroundWindow(foreground); });
        ClickTarget target; target.resize(260,160); target.move(40,40); target.show();
        Window w(nullptr,true); w.show(); auto *tabs=w.findChild<QTabWidget *>("tabs");
        QPoint selected;
        for(auto *screen:QApplication::screens()) for(int page:{0,1}) {
            target.windowHandle()->setScreen(screen); target.move(screen->geometry().topLeft()+QPoint(40,40));
            if(page==1) w.showMaximized();
            tabs->setCurrentIndex(page); QCoreApplication::processEvents();
            const bool originalMaximized=w.isMaximized();
            QTimer::singleShot(5000,&w,[&] { if(auto *dialog=QApplication::activeModalWidget()) dialog->close(); });
            QTimer::singleShot(150,&w,[&] {
                auto *picker=qobject_cast<PositionPicker *>(QApplication::activeModalWidget()); QVERIFY(picker);
                RECT bounds; QVERIFY(GetWindowRect(reinterpret_cast<HWND>(target.winId()),&bounds));
                selected=QPoint((bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2);
                INPUT click[3]={}; for(auto &event:click) event.type=INPUT_MOUSE;
                click[0].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
                click[0].mi.dx=LONG((qint64(selected.x()-GetSystemMetrics(SM_XVIRTUALSCREEN))*65536+32768)/GetSystemMetrics(SM_CXVIRTUALSCREEN));
                click[0].mi.dy=LONG((qint64(selected.y()-GetSystemMetrics(SM_YVIRTUALSCREEN))*65536+32768)/GetSystemMetrics(SM_CYVIRTUALSCREEN));
                click[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; click[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
                QCOMPARE(SendInput(3,click,sizeof(INPUT)),UINT(3));
            });
            auto *button=w.findChild<QPushButton *>(page==0?"quickPositionCapture":"stepPositionCapture"); button->click();
            QCOMPARE(target.clicks,0); QCOMPARE(w.findChild<QSpinBox *>(page==0?"quickX":"stepX")->value(),selected.x());
            QCOMPARE(w.findChild<QSpinBox *>(page==0?"quickY":"stepY")->value(),selected.y());
            QTRY_COMPARE(GetForegroundWindow(),reinterpret_cast<HWND>(w.winId())); QVERIFY(w.isVisible()); QCOMPARE(w.isMaximized(),originalMaximized); QCOMPARE(tabs->currentIndex(),page);
            for(auto *widget:QApplication::topLevelWidgets()) QVERIFY(!widget->objectName().startsWith("positionPicker"));
        }
        w.close();
    }
    void nativePickerCoversEveryMonitor() {
        if(QGuiApplication::platformName()!=QStringLiteral("windows")) QSKIP("Requires the native Windows UI backend.");
        QWidget owner; owner.resize(800,480); owner.show();
        for(auto *ownerScreen:QApplication::screens()) {
            owner.windowHandle()->setScreen(ownerScreen); owner.move(ownerScreen->geometry().topLeft()+QPoint(50,50));
            PositionPicker picker(&owner); bool checked=false;
            QTimer::singleShot(250,&picker,[&] {
                auto cancel=qScopeGuard([&] { picker.reject(); });
                checked=true;
                QList<QRect> covered;
                for(auto *panel:QApplication::topLevelWidgets()) {
                    if(!panel->objectName().startsWith("positionPicker")) continue;
                    const HWND handle=reinterpret_cast<HWND>(panel->winId());
                    MONITORINFO info{sizeof(MONITORINFO)};
                    QVERIFY(GetMonitorInfoW(MonitorFromWindow(handle,MONITOR_DEFAULTTONEAREST),&info));
                    RECT bounds; QVERIFY(GetWindowRect(handle,&bounds));
                    const QRect actual(bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top);
                    const QRect expected(info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top);
                    // Fractional Qt scaling can round the backing store out by
                    // one device pixel. Require complete coverage without gaps
                    // and reject displacement larger than that rounding margin.
                    QVERIFY2(actual.contains(expected),qPrintable(QString("Overlay %1,%2 %3x%4 does not cover monitor %5,%6 %7x%8")
                        .arg(actual.x()).arg(actual.y()).arg(actual.width()).arg(actual.height())
                        .arg(expected.x()).arg(expected.y()).arg(expected.width()).arg(expected.height())));
                    const int roundingMargin=qRound(panel->devicePixelRatioF())+1;
                    QVERIFY(expected.left()-actual.left()<=roundingMargin && actual.right()-expected.right()<=roundingMargin);
                    QVERIFY(expected.top()-actual.top()<=roundingMargin && actual.bottom()-expected.bottom()<=roundingMargin);
                    QVERIFY(!covered.contains(expected)); covered.append(expected);
                    for(const QPoint point:monitorTestPoints(expected))
                        QCOMPARE(GetAncestor(WindowFromPoint(POINT{point.x(),point.y()}),GA_ROOT),handle);
                }
                const auto monitors=nativeMonitorRects(); QCOMPARE(covered.size(),monitors.size());
                for(const QRect &rect:monitors) QVERIFY(covered.contains(rect));
            });
            QTimer::singleShot(5000,&picker,&QDialog::reject);
            QCOMPARE(picker.exec(),int(QDialog::Rejected)); QVERIFY(checked);
        }
        owner.close();
    }
    void nativePickerSelectsCornersAndEdges() {
        if(QGuiApplication::platformName()!=QStringLiteral("windows")) QSKIP("Requires the native Windows UI backend.");
        POINT saved; QVERIFY(GetPhysicalCursorPos(&saved)); HWND foreground=GetForegroundWindow();
        auto restore=qScopeGuard([&] { SetPhysicalCursorPos(saved.x,saved.y); if(foreground) SetForegroundWindow(foreground); });
        QWidget owner; owner.resize(800,480); owner.show();
        const auto monitors=nativeMonitorRects();
        for(const QRect &monitor:monitors) for(const QPoint &point:monitorTestPoints(monitor)) {
            PositionPicker picker(&owner);
            QTimer::singleShot(100,&picker,[&] {
                HWND under=GetAncestor(WindowFromPoint(POINT{point.x(),point.y()}),GA_ROOT);
                QWidget *surface=QWidget::find(reinterpret_cast<WId>(under));
                if(!surface || !surface->objectName().startsWith("positionPicker")) { picker.reject(); QFAIL("Picker does not intercept this screen edge"); }
                QCOMPARE(nativeClickAt(point),UINT(3));
            });
            QTimer::singleShot(5000,&picker,&QDialog::reject);
            QCOMPARE(picker.exec(),int(QDialog::Accepted)); QCOMPARE(picker.position(),point);
        }
        owner.close();
    }
    void nativePickerEscDoesNotLeakMouseRelease() {
        if(QGuiApplication::platformName()!=QStringLiteral("windows")) QSKIP("Requires the native Windows UI backend.");
        POINT saved; QVERIFY(GetPhysicalCursorPos(&saved)); HWND foreground=GetForegroundWindow();
        auto restore=qScopeGuard([&] {
            if(GetAsyncKeyState(VK_LBUTTON)&0x8000) { INPUT release={}; release.type=INPUT_MOUSE; release.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&release,sizeof(INPUT)); }
            SetPhysicalCursorPos(saved.x,saved.y); if(foreground) SetForegroundWindow(foreground);
        });
        ClickTarget target; target.resize(240,140); target.move(40,40); target.show(); QWidget owner; owner.show();
        {
            PositionPicker picker(&owner); bool cancelledWhileVisible=false;
            QTimer::singleShot(100,&picker,[&] {
                RECT bounds; QVERIFY(GetWindowRect(reinterpret_cast<HWND>(target.winId()),&bounds));
                SetPhysicalCursorPos((bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2);
                INPUT press={}; press.type=INPUT_MOUSE; press.mi.dwFlags=MOUSEEVENTF_LEFTDOWN; QCOMPARE(SendInput(1,&press,sizeof(INPUT)),UINT(1));
            });
            QTimer::singleShot(200,&picker,[&] {
                INPUT escape[2]={}; for(auto &event:escape) { event.type=INPUT_KEYBOARD; event.ki.wVk=VK_ESCAPE; }
                escape[1].ki.dwFlags=KEYEVENTF_KEYUP; QCOMPARE(SendInput(2,escape,sizeof(INPUT)),UINT(2));
            });
            QTimer::singleShot(300,&picker,[&] {
                cancelledWhileVisible=picker.isVisible();
                INPUT release={}; release.type=INPUT_MOUSE; release.mi.dwFlags=MOUSEEVENTF_LEFTUP;
                QCOMPARE(SendInput(1,&release,sizeof(INPUT)),UINT(1));
            });
            QTimer::singleShot(5000,&picker,&QDialog::reject);
            QCOMPARE(picker.exec(),int(QDialog::Rejected)); QVERIFY(cancelledWhileVisible);
            QCoreApplication::processEvents(); QCOMPARE(target.clicks,0); QCOMPARE(target.releases,0);
        }
        owner.close(); target.close();
    }
    void controlToggleCannotStartWhilePicking() {
        Window w(nullptr,true); w.show(); auto *editor=w.findChild<StepEditor *>(); Step step; step.fixedPosition=true; editor->setValue(step);
        QTimer::singleShot(0,&w,[&] {
            auto *picker=qobject_cast<PositionPicker *>(QApplication::activeModalWidget()); QVERIFY(picker);
            auto cancel=qScopeGuard([&] { picker->reject(); w.execution().stop(); });
            w.toggleTask(); QVERIFY(!w.execution().running());
        });
        QTimer::singleShot(5000,&w,[&] { if(auto *dialog=QApplication::activeModalWidget()) dialog->close(); });
        w.findChild<QPushButton *>("quickPositionCapture")->click(); w.close();
    }
    void nativeMoveUsesPhysicalPixelsAndRejectsDesktopGaps() {
        if(QGuiApplication::platformName()!=QStringLiteral("windows")) QSKIP("Requires the native Windows UI backend.");
        POINT saved; QVERIFY(GetPhysicalCursorPos(&saved)); auto restore=qScopeGuard([&] { SetPhysicalCursorPos(saved.x,saved.y); });
        WindowsInput input; QString error; const auto monitors=nativeMonitorRects(); QRect desktop;
        for(const QRect &rect:monitors) { desktop=desktop.united(rect); for(const QPoint &point:monitorTestPoints(rect)) {
            QVERIFY2(input.move(point,error),qPrintable(error));
            QTRY_VERIFY(([&] { POINT cursor; return GetPhysicalCursorPos(&cursor) && QPoint(cursor.x,cursor.y)==point; })());
        } }
        // Unequal or staggered monitors leave holes inside the virtual desktop's bounding rectangle.
        QList<QPoint> points=monitorTestPoints(desktop);
        for(const QRect &rect:monitors) points.append(monitorTestPoints(rect.adjusted(-1,-1,1,1)));
        for(const QPoint &point:points) {
            bool onScreen=false; for(const QRect &rect:monitors) onScreen|=rect.contains(point);
            if(onScreen) continue;
            POINT before; QVERIFY(GetPhysicalCursorPos(&before));
            QVERIFY(!input.move(point,error)); POINT after; QVERIFY(GetPhysicalCursorPos(&after));
            QCOMPARE(QPoint(after.x,after.y),QPoint(before.x,before.y));
        }
    }
    void capturedKeyDisplayPreservesPhysicalMetadata() {
        StepEditor editor(true); InputKey captured{false,VK_RCONTROL,0x1d,true}; Step step; step.key=captured; editor.setValue(step);
        QCOMPARE(editor.value().key,captured); QCOMPARE(editor.findChild<QLineEdit *>("quickInput")->text(),captured.name());
        Step parsed; QString error; QVERIFY(Step::parse(editor.value().json(),parsed,error)); QCOMPARE(parsed.key,captured);
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
    void nativeRecordingKeepsMouseReleaseOverItsOwnWindow() {
        if(QGuiApplication::platformName()!=QStringLiteral("windows")) QSKIP("Requires the native Windows UI backend.");
        ClickTarget target; target.resize(180,120); target.move(40,40); target.show();
        Window w(nullptr,true); w.move(400,300); w.show(); w.raise(); w.activateWindow();
        auto *list=w.findChild<QListWidget *>("flow"); list->clear(); w.findChild<QTabWidget *>("tabs")->setCurrentIndex(1);
        QPushButton *record=nullptr;
        for(auto *button:w.findChildren<QPushButton *>()) if(button->text()==QStringLiteral("● 录制输入")) record=button;
        QVERIFY(record); record->click(); auto *monitor=w.findChild<InputMonitor *>("recordingMonitor"); QVERIFY(monitor);
        QTest::qWait(100);
        RECT outsideBounds,ownBounds;
        QVERIFY(GetWindowRect(reinterpret_cast<HWND>(target.winId()),&outsideBounds));
        QVERIFY(GetWindowRect(reinterpret_cast<HWND>(w.winId()),&ownBounds));
        const QPoint outside((outsideBounds.left+outsideBounds.right)/2,(outsideBounds.top+outsideBounds.bottom)/2);
        const QPoint inside((ownBounds.left+ownBounds.right)/2,(ownBounds.top+ownBounds.bottom)/2);
        QCOMPARE(GetAncestor(WindowFromPoint(POINT{inside.x(),inside.y()}),GA_ROOT),reinterpret_cast<HWND>(w.winId()));
        InputKey mouse{true,VK_LBUTTON,0,false};
        emit monitor->observed(mouse,true,outside,1000); emit monitor->observed(mouse,false,inside,1200);
        emit monitor->observed(mouse,true,outside,1300); emit monitor->observed(mouse,false,outside,1400);
        QCoreApplication::processEvents(); record->click();
        const Script script=w.currentScript(); QCOMPARE(script.steps.size(),7);
        QCOMPARE(script.steps[0].action,Action::Down); QCOMPARE(script.steps[2].action,Action::Up);
        QCOMPARE(script.steps[2].position,inside); QCOMPARE(script.steps[4].action,Action::Down); QCOMPARE(script.steps[6].action,Action::Up);
        w.close();
    }
};
QTEST_MAIN(Tests)
#include "test_clicker.moc"
