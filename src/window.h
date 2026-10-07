#pragma once
#include "engine.h"
#include "controls.h"
#include <QMainWindow>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QCloseEvent>

class StepEditor : public QWidget {
    Q_OBJECT
public:
    explicit StepEditor(bool quick, QWidget *parent=nullptr);
    Step value() const;
    void setValue(const Step &);
signals:
    void changed();
private:
    void updateFields();
    void capture(bool position);
    void setKey(const InputKey &);
    void selectImage(bool fromScreen);
    void selectRegion();
    void refreshImage();
    QComboBox *action;
    QLineEdit *input;
    TimeField *interval, *duration;
    QSpinBox *count, *x, *y;
    QCheckBox *fixed;
    QPushButton *captureKey, *capturePosition;
    QWidget *inputRow, *intervalRow, *durationRow, *countRow, *positionRow;
    InputKey currentKey;
    QByteArray templatePng;
    QString templateName;
    QRect searchRegion;
    QWidget *imageRow,*similarityRow,*regionRow;
    QLabel *imagePreview,*regionInfo;
    QSpinBox *similarity;
    QCheckBox *scaleMatch,*limitRegion;
    bool loading=false;
};

class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(QWidget *parent=nullptr, bool testing=false);
    ~Window() override;
    void shutdown();
    void showWindow();
    void toggleTask();
    void stopTask();
    bool loadScript(const QString &path, bool quiet=false);
    Engine &execution() { return engine; }
    Script currentScript() const;
    void setScript(const Script &);
    static QIcon appIcon();
protected:
    void closeEvent(QCloseEvent *) override;
private:
    void start();
    void updateState(bool);
    void addStep(Action);
    void refreshItem(QListWidgetItem *, const Step &);
    void scheduleIndent();
    void saveScript();
    void record();
    void endRecording();
    void appendRecorded(InputKey, bool, QPoint, quint64);
    void settingsLoad();
    void settingsSave();
    void notice(const QString &, bool error=false);
    void shortcutHint();
    WindowsInput sink;
    Engine engine;
    Hotkeys hotkeys;
    InputMonitor monitor;
    QTabWidget *tabs;
    StepEditor *quick, *editor;
    QWidget *quickPage, *flowPage;
    QListWidget *flow;
    QLabel *status, *message, *stats;
    QPushButton *startButton, *stopButton, *recordButton;
    TimeField *delay;
    QSpinBox *rounds;
    ShortcutField *toggleShortcut, *stopShortcut, *recordShortcut;
    QWidget *roundsRow;
    QCheckBox *minimizeOnStart;
    bool recording=false, closing=false, testMode=false;
    QString scriptPath;
    quint64 lastRecordTime=0;
    QMap<QString,InputKey> recordingHeld;
    int currentRunningStep=-1;
    bool indentPending=false;
};
