#pragma once
#include "input.h"
#include "vision.h"
#include <QElapsedTimer>
#include <QMap>
#include <QTimer>
#include <QThread>

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(InputSink &sink, QObject *parent = nullptr, VisionSource *vision = nullptr);
    ~Engine() override;
    bool start(const Script &, QString &error);
    void stop();
    bool running() const { return active; }
    int heldCount() const { return held.size(); }
    quint64 eventCount() const { return events; }
    bool timerActive() const { return timer.isActive(); }
signals:
    void stateChanged(bool);
    void progress(int step, int round, quint64 actions);
    void finished();
    void failed(QString);
    void matchEvaluated(int step, MatchResult result);
private:
    void tick();
    bool key(const InputKey &, bool);
    bool click(const InputKey &);
    void fail(const QString &);
    bool releaseAll();
    void finish();
    void recognize(const Step &);
    InputSink &sink;
    QTimer timer;
    QElapsedTimer reporting;
    Script script;
    bool active=false;
    int index=0, round=0, repeats=0;
    quint64 events=0;
    bool repeating=false, holding=false;
    InputKey holdKey;
    QMap<QString,InputKey> held;
    struct Loop { int begin; int remaining; };
    QVector<Loop> loops;
    struct Condition { int end; bool found; QRect bounds; };
    QVector<Condition> conditions;
    QMap<int,int> ends, alternatives;
    VisionSource *injectedVision;
    std::unique_ptr<ScreenMatcher> screenMatcher;
    QThread *visionThread=nullptr;
    std::shared_ptr<std::atomic_bool> cancel;
};
