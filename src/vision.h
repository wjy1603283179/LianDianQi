#pragma once
#include "model.h"
#include <QImage>
#include <QList>
#include <memory>
#include <atomic>

struct MatchResult {
    bool found=false,cancelled=false;
    QRect bounds;
    double score=0,scale=1;
    qint64 captureMicros=0,matchMicros=0;
    QString error;
    QPoint center() const { return bounds.topLeft()+QPoint(bounds.width()/2,bounds.height()/2); }
};
Q_DECLARE_METATYPE(MatchResult)

// GDI captures only the requested device-pixel rectangle. The DIB is reused
// between recognition steps; thread-owned DCs are released on that thread.
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    QImage grab(const QRect &physicalRect,QString &error);
    static QList<QRect> monitors();
private:
    struct Data;
    std::unique_ptr<Data> data;
};

class TemplateMatcher {
public:
    TemplateMatcher();
    ~TemplateMatcher();
    bool prepare(const QImage &,bool allowScale,QString &error);
    MatchResult find(const QImage &,int similarity,const std::atomic_bool *cancel=nullptr);
private:
    struct Data;
    std::unique_ptr<Data> data;
};

class VisionSource {
public:
    virtual ~VisionSource()=default;
    virtual MatchResult recognize(const Step &,const std::atomic_bool &cancel)=0;
};
class ScreenMatcher final : public VisionSource {
public:
    MatchResult recognize(const Step &,const std::atomic_bool &) override;
private:
    ScreenCapture capture;
    TemplateMatcher matcher;
    QByteArray cachedPng;
    bool cachedScale=false;
};
