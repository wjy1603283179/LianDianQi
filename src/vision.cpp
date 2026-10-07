#include "vision.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <emmintrin.h>
#include <windows.h>

struct ScreenCapture::Data {
    HBITMAP bitmap=nullptr;
    void *bits=nullptr;
    QSize size;
    void releaseBitmap() { if(bitmap) DeleteObject(bitmap); bitmap=nullptr; bits=nullptr; }
    ~Data() { releaseBitmap(); }
};
ScreenCapture::ScreenCapture():data(std::make_unique<Data>()) {}
ScreenCapture::~ScreenCapture()=default;
QList<QRect> ScreenCapture::monitors() {
    QList<QRect> result;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT rect,LPARAM context)->BOOL {
        reinterpret_cast<QList<QRect> *>(context)->append(QRect(rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top));
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
    return result;
}
QImage ScreenCapture::grab(const QRect &rect,QString &error) {
    if(rect.isEmpty() || qint64(rect.width())*rect.height()>40000000) { error=QStringLiteral("截图范围无效或过大"); return {}; }
    // Common screen DCs belong to the calling thread. Never retain one after
    // a recognition worker exits; the pixel buffer can safely be reused.
    HDC screen=GetDC(nullptr); if(!screen) { error=QStringLiteral("无法访问屏幕"); return {}; }
    auto releaseScreen=qScopeGuard([&] { ReleaseDC(nullptr,screen); });
    HDC memory=CreateCompatibleDC(screen);
    if(!memory) { error=QStringLiteral("无法创建屏幕截图缓冲区"); return {}; }
    auto releaseMemory=qScopeGuard([&] { DeleteDC(memory); });
    if(!data->bitmap || data->size!=rect.size()) {
        data->releaseBitmap(); BITMAPINFO info={}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=rect.width(); info.bmiHeader.biHeight=-rect.height();
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        data->bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&data->bits,nullptr,0);
        if(!data->bitmap) { error=QStringLiteral("无法分配屏幕截图缓冲区"); return {}; }
        data->size=rect.size();
    }
    HGDIOBJ previous=SelectObject(memory,data->bitmap); auto restore=qScopeGuard([&] { SelectObject(memory,previous); });
    if(!BitBlt(memory,0,0,rect.width(),rect.height(),screen,rect.x(),rect.y(),SRCCOPY|CAPTUREBLT)) { error=QStringLiteral("屏幕截图失败：%1").arg(GetLastError()); return {}; }
    GdiFlush();
    // The view is valid until the next grab. Matching consumes it on this thread.
    return QImage(static_cast<uchar *>(data->bits),rect.width(),rect.height(),rect.width()*4,QImage::Format_RGB32);
}

namespace {
int distance(QRgb a,QRgb b) { return std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b))}); }
int absoluteRgb(QRgb a,QRgb b) { return std::abs(qRed(a)-qRed(b))+std::abs(qGreen(a)-qGreen(b))+std::abs(qBlue(a)-qBlue(b)); }
struct Sample { int x,y,weight; QRgb color; };
struct Variant { QSize size; QList<Sample> anchors,checks; qint64 weightSum=0; double scale=1; };

Variant compile(const QImage &image,double scale) {
    Variant out; out.size=image.size(); out.scale=scale;
    const int w=image.width(),h=image.height();
    // A border estimate separates a flat background from meaningful content.
    // Foreground pixels carry more weight so blank backgrounds cannot produce
    // a high score just because a word/icon occupies little of its rectangle.
    QList<QRgb> border;
    for(int x=0;x<w;x+=std::max(1,w/16)) { border.append(image.pixel(x,0)); border.append(image.pixel(x,h-1)); }
    for(int y=0;y<h;y+=std::max(1,h/16)) { border.append(image.pixel(0,y)); border.append(image.pixel(w-1,y)); }
    std::array<int,256> red={},green={},blue={};
    for(QRgb p:border) { ++red[qRed(p)]; ++green[qGreen(p)]; ++blue[qBlue(p)]; }
    auto median=[&](const auto &histogram) { int count=0; for(int i=0;i<256;++i) if((count+=histogram[i])>border.size()/2) return i; return 255; };
    QRgb background=qRgb(median(red),median(green),median(blue));
    struct Candidate { int x,y,strength,stability; QRgb color; };
    QList<Candidate> candidates;
    const int step=std::max(1,int(std::sqrt(double(w)*h/4096)));
    for(int y=0;y<h;y+=step) for(int x=0;x<w;x+=step) {
        QRgb pixel=image.pixel(x,y); int strength=distance(pixel,background);
        int variation=0;
        if(x>0 && x+1<w && y>0 && y+1<h) {
            variation=std::max({distance(pixel,image.pixel(x-1,y)),distance(pixel,image.pixel(x+1,y)),distance(pixel,image.pixel(x,y-1)),distance(pixel,image.pixel(x,y+1))});
        }
        const int weight=1+(strength>25?5:0)+(variation>30?2:0);
        out.checks.append({x,y,weight,pixel}); out.weightSum+=weight;
        if(strength>40) candidates.append({x,y,strength,variation,pixel});
    }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate &a,const Candidate &b) { return a.strength-a.stability*2>b.strength-b.stability*2; });
    const int separation=std::max(2,std::min(w,h)/6);
    for(const auto &point:candidates) {
        bool nearby=false; for(const auto &anchor:out.anchors) if(std::abs(point.x-anchor.x)+std::abs(point.y-anchor.y)<separation) { nearby=true; break; }
        if(!nearby) out.anchors.append({point.x,point.y,1,point.color});
        if(out.anchors.size()==12) break;
    }
    // Add distributed checks, including background holes and borders, to reject
    // other shapes of the same color before the more detailed scoring pass.
    for(int gy=0;gy<5;++gy) for(int gx=0;gx<5;++gx) {
        int x=gx*(w-1)/4,y=gy*(h-1)/4; out.anchors.append({x,y,1,image.pixel(x,y)});
    }
    return out;
}
}

static const std::array<int,31> scaleOrder={100,125,150,75,200,50,95,105,90,110,85,115,80,120,70,130,65,135,60,140,55,145,155,160,165,170,175,180,185,190,195};
struct TemplateMatcher::Data { QMap<int,Variant> variants; QImage original; bool allowScale=false; int preferred=0; };
TemplateMatcher::TemplateMatcher():data(std::make_unique<Data>()) {}
TemplateMatcher::~TemplateMatcher()=default;
bool TemplateMatcher::prepare(const QImage &image,bool allowScale,QString &error) {
    if(image.isNull() || image.width()<8 || image.height()<8 || image.width()>1024 || image.height()>1024) { error=QStringLiteral("模板须为 8～1024 像素的图像"); return false; }
    QImage opaque(image.size(),QImage::Format_RGB32); opaque.fill(Qt::white); { QPainter painter(&opaque); painter.drawImage(0,0,image); }
    Variant original=compile(opaque,1);
    if(original.anchors.size()<=25) { error=QStringLiteral("模板缺少可区分的图像特征"); return false; }
    data->original=opaque; data->allowScale=allowScale; data->variants.clear(); data->variants.insert(0,std::move(original)); data->preferred=0;
    return true;
}
MatchResult TemplateMatcher::find(const QImage &input,int similarity,const std::atomic_bool *cancel) {
    MatchResult result; QElapsedTimer elapsed; elapsed.start();
    QImage screen=input.format()==QImage::Format_RGB32?input:input.convertToFormat(QImage::Format_RGB32);
    if(screen.isNull() || data->variants.isEmpty()) { result.error=QStringLiteral("识别图像或模板为空"); return result; }
    const int threshold=std::clamp(similarity,60,100);
    const int tolerance=std::clamp((100-threshold)*4+12,12,140);
    const __m128i toleranceVector=_mm_set1_epi8(char(tolerance));
    const __m128i rgbMask=_mm_set1_epi32(0x00ffffff),zero=_mm_setzero_si128();
    auto stopped=[&] { return cancel && cancel->load(std::memory_order_relaxed); };
    QList<int> order{data->preferred};
    for(int i=0;i<(data->allowScale?31:1);++i) {
        if(i!=data->preferred) order.append(i);
        // Small text rendered with nearest-neighbor filtering can differ
        // substantially from a smooth half-size thumbnail.
        if(i==5 && data->preferred!=31) order.append(31);
    }
    for(int variantIndex:order) {
        if(stopped()) { result.cancelled=true; break; }
        // Compile non-native scales lazily. The common 1:1 case never pays for
        // them; repeated checks of the same template reuse compiled variants.
        if(!data->variants.contains(variantIndex)) {
            const int percent=variantIndex==31?50:scaleOrder[variantIndex];
            QSize size(qRound(data->original.width()*percent/100.0),qRound(data->original.height()*percent/100.0));
            if(size.width()<8 || size.height()<8) { data->variants.insert(variantIndex,Variant{}); continue; }
            data->variants.insert(variantIndex,compile(data->original.scaled(size,Qt::IgnoreAspectRatio,variantIndex==31?Qt::FastTransformation:Qt::SmoothTransformation),percent/100.0));
        }
        const Variant &variant=data->variants[variantIndex];
        if(variant.anchors.isEmpty()) continue;
        const int maxX=screen.width()-variant.size.width(),maxY=screen.height()-variant.size.height();
        if(maxX<0 || maxY<0) continue;
        const Sample &first=variant.anchors.first(); const __m128i reference=_mm_set1_epi32(int(first.color));
        const qint64 limit=qint64(100-threshold)*255*3*variant.weightSum/100;
        auto verify=[&](int x,int y)->bool {
            int rejected=0;
            for(qsizetype i=1;i<variant.anchors.size();++i) {
                const auto &anchor=variant.anchors[i]; const QRgb *line=reinterpret_cast<const QRgb *>(screen.constScanLine(y+anchor.y));
                // Fractional scaling moves antialiased edges between pixels.
                // A few disagreeing probes are allowed; weighted scoring still
                // verifies the entire foreground before accepting a candidate.
                if(distance(line[x+anchor.x],anchor.color)>tolerance && ++rejected>5) return false;
            }
            qint64 difference=0;
            for(qsizetype i=0;i<variant.checks.size();++i) {
                const auto &sample=variant.checks[i]; const QRgb *line=reinterpret_cast<const QRgb *>(screen.constScanLine(y+sample.y));
                difference+=qint64(absoluteRgb(line[x+sample.x],sample.color))*sample.weight;
                if(difference>limit) return false;
            }
            result.found=true; result.score=100-double(difference)*100/(255*3*variant.weightSum);
            result.bounds=QRect(QPoint(x,y),variant.size); result.scale=variant.scale; data->preferred=variantIndex; return true;
        };
        for(int y=0;y<=maxY && !result.found;++y) {
            if((y&7)==0 && stopped()) { result.cancelled=true; break; }
            const auto *line=reinterpret_cast<const QRgb *>(screen.constScanLine(y+first.y));
            int x=0;
            for(;x+3<=maxX;x+=4) {
                __m128i pixels=_mm_loadu_si128(reinterpret_cast<const __m128i *>(line+x+first.x));
                __m128i difference=_mm_or_si128(_mm_subs_epu8(pixels,reference),_mm_subs_epu8(reference,pixels));
                __m128i exceeded=_mm_and_si128(_mm_subs_epu8(difference,toleranceVector),rgbMask);
                unsigned mask=unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(exceeded,zero)));
                for(int lane=0;lane<4;++lane) if(((mask>>(lane*4))&15)==15 && verify(x+lane,y)) break;
                if(result.found) break;
            }
            for(;x<=maxX && !result.found;++x) if(distance(line[x+first.x],first.color)<=tolerance) verify(x,y);
        }
        if(result.found || result.cancelled) break;
    }
    result.matchMicros=elapsed.nsecsElapsed()/1000; return result;
}
MatchResult ScreenMatcher::recognize(const Step &step,const std::atomic_bool &cancel) {
    MatchResult result; QElapsedTimer recognitionTime; recognitionTime.start();
    if(cancel.load()) { result.cancelled=true; return result; }
    if(cachedPng!=step.templatePng || cachedScale!=step.scaleMatch) {
        QImage image=QImage::fromData(step.templatePng,"PNG");
        if(!matcher.prepare(image,step.scaleMatch,result.error)) { result.matchMicros=recognitionTime.nsecsElapsed()/1000; return result; }
        cachedPng=step.templatePng; cachedScale=step.scaleMatch;
    }
    const auto screens=ScreenCapture::monitors();
    bool any=false; qint64 captureTime=0;
    for(const QRect &monitor:screens) {
        QRect region=step.limitRegion?monitor.intersected(step.searchRegion):monitor;
        if(region.isEmpty()) continue; any=true;
        if(cancel.load()) { result.cancelled=true; break; }
        QElapsedTimer timer; timer.start(); QImage image=capture.grab(region,result.error); captureTime+=timer.nsecsElapsed()/1000;
        if(image.isNull()) break;
        result=matcher.find(image,step.similarity,&cancel);
        if(result.found) result.bounds.translate(region.topLeft());
        if(result.found || result.cancelled || !result.error.isEmpty()) break;
    }
    if(!any) result.error=QStringLiteral("识别范围不在当前显示器内");
    result.captureMicros=captureTime; result.matchMicros=recognitionTime.nsecsElapsed()/1000-captureTime; return result;
}
