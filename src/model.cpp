#include "model.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QBuffer>
#include <QImageReader>
#include <cmath>
#include <windows.h>

static const char *actionIds[] = {"repeat", "hold", "down", "up", "click", "wait", "move", "loop", "endLoop", "ifImage", "else", "endIf", "clickMatch"};
QString actionName(Action a) {
    static const QStringList names = {QStringLiteral("连点"), QStringLiteral("长按"), QStringLiteral("按下"),
        QStringLiteral("抬起"), QStringLiteral("按一次"), QStringLiteral("等待"), QStringLiteral("移动鼠标"),
        QStringLiteral("循环开始"), QStringLiteral("循环结束"), QStringLiteral("如果找到图像"),
        QStringLiteral("否则"), QStringLiteral("结束条件"), QStringLiteral("点击匹配中心")};
    return names.value(int(a));
}
bool isInputAction(Action a) { return int(a)>=0 && int(a) <= int(Action::Click); }

QString InputKey::identity() const {
    if (mouse) return QString("mouse:%1").arg(code);
    UINT sc = scan ? UINT(scan) : MapVirtualKeyW(UINT(code), MAPVK_VK_TO_VSC_EX);
    bool ext = extended || (sc & 0xff00) == 0xe000;
    if ((sc & 0xff) && code != VK_PAUSE && code != VK_SNAPSHOT) return QString("scan:%1:%2").arg(sc & 0xff).arg(ext);
    return QString("vk:%1:%2").arg(code).arg(ext);
}
QString InputKey::name() const {
    if (mouse) {
        switch (code) {
        case VK_LBUTTON: return QStringLiteral("鼠标左键");
        case VK_RBUTTON: return QStringLiteral("鼠标右键");
        case VK_MBUTTON: return QStringLiteral("鼠标中键");
        case VK_XBUTTON1: return QStringLiteral("鼠标侧键 1");
        case VK_XBUTTON2: return QStringLiteral("鼠标侧键 2");
        }
    }
    if (code >= 'A' && code <= 'Z') return QString(QChar(code));
    if (code >= '0' && code <= '9') return QString(QChar(code));
    if (code >= VK_F1 && code <= VK_F24) return QString("F%1").arg(code - VK_F1 + 1);
    if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) return QStringLiteral("小键盘 %1").arg(code - VK_NUMPAD0);
    switch (code) {
    case VK_SPACE: return QStringLiteral("空格");
    case VK_RETURN: return extended ? QStringLiteral("小键盘 Enter") : QStringLiteral("Enter");
    case VK_ESCAPE: return QStringLiteral("Esc");
    case VK_LCONTROL: return QStringLiteral("左 Ctrl");
    case VK_RCONTROL: return QStringLiteral("右 Ctrl");
    case VK_LSHIFT: return QStringLiteral("左 Shift");
    case VK_RSHIFT: return QStringLiteral("右 Shift");
    case VK_LMENU: return QStringLiteral("左 Alt");
    case VK_RMENU: return QStringLiteral("右 Alt");
    }
    UINT mapped = MapVirtualKeyW(UINT(code), MAPVK_VK_TO_VSC_EX);
    int sc = scan ? scan : int(mapped & 0xff);
    bool ext = extended || (mapped & 0xff00) == 0xe000;
    wchar_t buffer[128] = {};
    if (GetKeyNameTextW((sc << 16) | (ext ? (1 << 24) : 0), buffer, 128)) return QString::fromWCharArray(buffer);
    return QString("VK 0x%1").arg(code, 2, 16, QChar('0')).toUpper();
}
QJsonObject InputKey::json() const {
    return {{"device", mouse ? "mouse" : "keyboard"}, {"code", code}, {"scan", scan}, {"extended", extended}};
}
static bool integer(const QJsonObject &o, const char *field, int fallback, int low, int high, int &out, QString &error) {
    if (!o.contains(field)) { out = fallback; return true; }
    auto v = o.value(field);
    double n = v.toDouble(-1e30);
    if (!v.isDouble() || !std::isfinite(n) || n != std::floor(n) || n < low || n > high) {
        error = QStringLiteral("字段 %1 必须是 %2 到 %3 的整数").arg(field).arg(low).arg(high);
        return false;
    }
    out = int(n); return true;
}
bool InputKey::parse(const QJsonObject &o, InputKey &k, QString &error) {
    QString device = o.value("device").toString();
    if (device != "mouse" && device != "keyboard") { error = QStringLiteral("输入设备无效"); return false; }
    k.mouse = device == "mouse";
    if (!integer(o, "code", 1, 1, 254, k.code, error) || !integer(o, "scan", 0, 0, 255, k.scan, error)) return false;
    if (o.contains("extended") && !o.value("extended").isBool()) { error = QStringLiteral("extended 必须为布尔值"); return false; }
    k.extended = o.value("extended").toBool();
    if (k.mouse && k.code != 1 && k.code != 2 && k.code != 4 && k.code != 5 && k.code != 6) {
        error = QStringLiteral("鼠标按钮无效"); return false;
    }
    return true;
}
QString Step::title() const { return actionName(action) + (isInputAction(action) ? " · " + key.name() : QString()); }
QString Step::detail() const {
    QString s;
    switch (action) {
    case Action::Repeat: s = QStringLiteral("每 %1 ms · %2").arg(interval).arg(count ? QStringLiteral("%1 次").arg(count) : QStringLiteral("持续运行")); break;
    case Action::Hold: s = duration ? QStringLiteral("保持 %1 ms 后释放").arg(duration) : QStringLiteral("保持到手动停止"); break;
    case Action::Down: s = QStringLiteral("保持按下，直到抬起或停止"); break;
    case Action::Up: s = QStringLiteral("释放这个按键"); break;
    case Action::Click: s = QStringLiteral("按下并立即抬起"); break;
    case Action::Wait: s = QStringLiteral("等待 %1 ms").arg(duration); break;
    case Action::Move: s = QStringLiteral("屏幕坐标 (%1, %2)").arg(position.x()).arg(position.y()); break;
    case Action::LoopBegin: s = count ? QStringLiteral("重复后续区块 %1 次").arg(count) : QStringLiteral("持续重复后续区块"); break;
    case Action::LoopEnd: s = QStringLiteral("返回配对的循环开始"); break;
    case Action::IfImage: s=QStringLiteral("%1 · 相似度 %2% · %3").arg(templateName.isEmpty()?QStringLiteral("未设置模板"):templateName).arg(similarity).arg(limitRegion?QStringLiteral("指定范围"):QStringLiteral("全部屏幕")); break;
    case Action::Else: s=QStringLiteral("没有找到图像时执行"); break;
    case Action::EndIf: s=QStringLiteral("继续后续步骤"); break;
    case Action::ClickMatch: s=QStringLiteral("左键点击当前条件匹配区域的中心"); break;
    }
    if (fixedPosition && key.mouse && isInputAction(action)) s += QStringLiteral(" · (%1, %2)").arg(position.x()).arg(position.y());
    return s;
}
QJsonObject Step::json() const {
    QJsonObject object{{"action", int(action)>=0 && int(action)<ActionCount?actionIds[int(action)]:"invalid"}, {"input", key.json()}, {"intervalMs", interval},
        {"durationMs", duration}, {"count", count}, {"fixedPosition", fixedPosition}, {"x", position.x()}, {"y", position.y()}};
    if(action==Action::IfImage) {
        object["templatePng"]=QString::fromLatin1(templatePng.toBase64()); object["templateName"]=templateName;
        object["similarity"]=similarity; object["scaleMatch"]=scaleMatch; object["limitRegion"]=limitRegion;
        object["region"]=QJsonObject{{"x",searchRegion.x()},{"y",searchRegion.y()},{"width",searchRegion.width()},{"height",searchRegion.height()}};
    }
    return object;
}
bool Step::parse(const QJsonObject &o, Step &s, QString &error) {
    QString id = o.value("action").toString();
    int found = -1;
    for (int i=0; i<ActionCount; ++i) if (id == actionIds[i]) found = i;
    if (found < 0) { error = QStringLiteral("未知步骤：%1").arg(id); return false; }
    s.action = Action(found);
    if (isInputAction(s.action) && (!o.value("input").isObject() || !InputKey::parse(o.value("input").toObject(), s.key, error))) return false;
    int x, y;
    if (!integer(o,"intervalMs",100,5,3600000,s.interval,error) || !integer(o,"durationMs",1000,0,86400000,s.duration,error)
        || !integer(o,"count",10,0,1000000,s.count,error) || !integer(o,"x",0,-100000,100000,x,error)
        || !integer(o,"y",0,-100000,100000,y,error)) return false;
    if (o.contains("fixedPosition") && !o.value("fixedPosition").isBool()) { error = QStringLiteral("fixedPosition 必须为布尔值"); return false; }
    s.fixedPosition = o.value("fixedPosition").toBool(); s.position = QPoint(x,y);
    if(s.action==Action::IfImage) {
        if(!integer(o,"similarity",88,60,100,s.similarity,error)) return false;
        for(const char *name:{"scaleMatch","limitRegion"}) if(o.contains(name) && !o[name].isBool()) { error=QStringLiteral("%1 必须为布尔值").arg(name); return false; }
        s.scaleMatch=o.value("scaleMatch").toBool(true); s.limitRegion=o.value("limitRegion").toBool();
        s.templateName=o.value("templateName").toString().left(128);
        auto bytes=QByteArray::fromBase64Encoding(o.value("templatePng").toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(!bytes || bytes.decoded.size()>2*1024*1024) { error=QStringLiteral("模板图像无效或超过 2 MB"); return false; }
        s.templatePng=bytes.decoded;
        if(!s.templatePng.isEmpty()) {
            QBuffer buffer(&s.templatePng); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer,"PNG"); QSize size=reader.size();
            if(size.width()<8 || size.height()<8 || size.width()>1024 || size.height()>1024 || !reader.canRead()) { error=QStringLiteral("模板须为 8～1024 像素的 PNG 图像"); return false; }
        }
        if(o.contains("region") && !o.value("region").isObject()) { error=QStringLiteral("识别范围无效"); return false; }
        const auto region=o.value("region").toObject(); int rx,ry,rw,rh;
        if(!integer(region,"x",0,-100000,100000,rx,error) || !integer(region,"y",0,-100000,100000,ry,error)
            || !integer(region,"width",0,0,32768,rw,error) || !integer(region,"height",0,0,32768,rh,error)) return false;
        s.searchRegion=QRect(rx,ry,rw,rh);
    }
    return true;
}
QJsonObject Script::json() const {
    QJsonArray array; for (const auto &s : steps) array.append(s.json());
    return {{"format", "liandianqi-script"}, {"version", 1}, {"rounds", rounds}, {"startDelayMs", startDelay}, {"steps", array}};
}
bool Script::validate(QString &error) const {
    if (steps.isEmpty() || steps.size() > 10000) { error = QStringLiteral("脚本必须包含 1 到 10000 个步骤"); return false; }
    if (rounds < 0 || rounds > 1000000 || startDelay < 0 || startDelay > 60000) { error = QStringLiteral("循环次数或启动延迟无效"); return false; }
    struct Block { Action kind; int begin; bool hasElse=false; bool trueBranch=true; };
    QVector<Block> stack;
    for (int i=0; i<steps.size(); ++i) {
        Step checked;
        if (!Step::parse(steps[i].json(), checked, error)) { error = QStringLiteral("第 %1 步：%2").arg(i+1).arg(error); return false; }
        if (steps[i].action == Action::LoopBegin) {
            stack.append({Action::LoopBegin,i});
        } else if (steps[i].action == Action::LoopEnd) {
            if(stack.isEmpty() || stack.last().kind!=Action::LoopBegin || stack.last().begin==i-1) { error=QStringLiteral("第 %1 步的循环未配对、交叉或循环体为空").arg(i+1); return false; }
            stack.removeLast();
        } else if(steps[i].action==Action::IfImage) {
            if(steps[i].templatePng.isEmpty() || (steps[i].limitRegion && steps[i].searchRegion.isEmpty())) { error=QStringLiteral("第 %1 步未设置图像模板或识别范围").arg(i+1); return false; }
            stack.append({Action::IfImage,i});
        } else if(steps[i].action==Action::Else) {
            if(stack.isEmpty() || stack.last().kind!=Action::IfImage || stack.last().hasElse) { error=QStringLiteral("第 %1 步的否则未配对或重复").arg(i+1); return false; }
            stack.last().hasElse=true; stack.last().trueBranch=false;
        } else if(steps[i].action==Action::EndIf) {
            if(stack.isEmpty() || stack.last().kind!=Action::IfImage) { error=QStringLiteral("第 %1 步的条件未配对或交叉").arg(i+1); return false; }
            stack.removeLast();
        } else if(steps[i].action==Action::ClickMatch) {
            bool match=false; for(const auto &block:stack) if(block.kind==Action::IfImage && block.trueBranch) match=true;
            if(!match) { error=QStringLiteral("第 %1 步的匹配中心点击须放在找到图像的分支内").arg(i+1); return false; }
        }
        if(stack.size()>16) { error=QStringLiteral("循环和条件合计最多嵌套 16 层"); return false; }
    }
    if (!stack.isEmpty()) { error = QStringLiteral("循环或条件缺少结束步骤"); return false; }
    return true;
}
bool Script::parse(const QByteArray &bytes, Script &out, QString &error, bool allowDraft) {
    if (bytes.size() > 4*1024*1024) { error = QStringLiteral("脚本超过 4 MB"); return false; }
    QJsonParseError e; auto doc = QJsonDocument::fromJson(bytes, &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject()) { error = QStringLiteral("JSON 格式错误：%1").arg(e.errorString()); return false; }
    auto o = doc.object(); Script s;
    if (o.value("format").toString() != "liandianqi-script" || o.value("version").toInt() != 1 || !o.value("steps").isArray()) {
        error = QStringLiteral("不支持的脚本格式或版本"); return false;
    }
    if (!integer(o,"rounds",1,0,1000000,s.rounds,error) || !integer(o,"startDelayMs",2000,0,60000,s.startDelay,error)) return false;
    for (const auto &v : o.value("steps").toArray()) {
        Step step;
        if (!v.isObject() || !Step::parse(v.toObject(), step, error)) return false;
        s.steps.append(step);
        if (s.steps.size() > 10000) { error = QStringLiteral("步骤超过 10000 个"); return false; }
    }
    if (!allowDraft && !s.validate(error)) return false;
    out = s; return true;
}
