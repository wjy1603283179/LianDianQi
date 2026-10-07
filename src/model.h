#pragma once
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

struct InputKey {
    bool mouse = true;
    int code = 1;
    int scan = 0;
    bool extended = false;
    bool operator==(const InputKey &other) const {
        return mouse == other.mouse && code == other.code && scan == other.scan && extended == other.extended;
    }
    QString name() const;
    QString identity() const;
    QJsonObject json() const;
    static bool parse(const QJsonObject &, InputKey &, QString &);
};
Q_DECLARE_METATYPE(InputKey)

enum class Action { Repeat, Hold, Down, Up, Click, Wait, Move, LoopBegin, LoopEnd, IfImage, Else, EndIf, ClickMatch, StopTask, BreakLoop };
constexpr int ActionCount=15;
struct Step {
    Action action = Action::Click;
    InputKey key;
    int interval = 100;
    int duration = 1000;
    int count = 10;
    bool fixedPosition = false;
    QPoint position;
    QByteArray templatePng;
    QString templateName;
    QRect searchRegion;
    bool limitRegion=false,scaleMatch=true;
    int similarity=88;
    QString title() const;
    QString detail() const;
    QJsonObject json() const;
    static bool parse(const QJsonObject &, Step &, QString &);
};
struct Script {
    QVector<Step> steps;
    int rounds = 1;
    int startDelay = 2000;
    bool latch = false; // Quick "down": retain the key until Stop, never saved in a script.
    QJsonObject json() const;
    bool validate(QString &, bool allowDraft=false) const;
    static bool parse(const QByteArray &, Script &, QString &, bool allowDraft=false);
};
QString actionName(Action);
Action actionFromId(const QString &);
bool isInputAction(Action);
bool isBlockBoundary(Action);
