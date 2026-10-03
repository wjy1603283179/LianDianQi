#pragma once
#include "model.h"
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QHash>
#include <QKeySequence>
#include <windows.h>

class InputSink {
public:
    virtual ~InputSink() = default;
    virtual bool keyEvent(const InputKey &, bool down, QString &error) = 0;
    virtual bool move(const QPoint &, QString &error) = 0;
};
class WindowsInput final : public InputSink {
public:
    bool keyEvent(const InputKey &, bool, QString &) override;
    bool move(const QPoint &, QString &) override;
    static INPUT packet(const InputKey &, bool);
};

// Hooks exist only during explicit capture/recording. No idle polling or worker process.
class InputMonitor : public QObject {
    Q_OBJECT
public:
    explicit InputMonitor(QObject *parent = nullptr) : QObject(parent) {}
    ~InputMonitor() override { stop(); }
    bool start(QString &error);
    void stop();
    static constexpr ULONG_PTR InjectionTag = 0x4c445151;
signals:
    void observed(InputKey key, bool down, QPoint position, quint64 timestamp);
private:
    static InputMonitor *active;
    static LRESULT CALLBACK keyboardHook(int, WPARAM, LPARAM);
    static LRESULT CALLBACK mouseHook(int, WPARAM, LPARAM);
    HHOOK keyboard = nullptr, mouse = nullptr;
};

class Hotkeys : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit Hotkeys(QObject *parent = nullptr);
    ~Hotkeys() override;
    bool set(const QKeySequence &toggle, const QKeySequence &stop, const QKeySequence &record, QString &error);
    void clear();
    bool setPaused(bool paused, QString &error);
    QKeySequence sequence(int id) const { return current.value(id); }
    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
    static bool decode(const QKeySequence &, UINT &modifiers, UINT &vk);
signals:
    void triggered(int id); // 1 toggle, 2 stop, 3 recording
private:
    QHash<int, QKeySequence> current;
    bool paused=false;
};
