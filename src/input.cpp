#include "input.h"
#include <QCoreApplication>

static bool send(INPUT &input, QString &error) {
    if (SendInput(1, &input, sizeof(INPUT)) == 1) return true;
    error = QStringLiteral("Windows 拒绝输入（错误 %1）。目标程序为管理员权限时，请以管理员身份运行连点器。").arg(GetLastError());
    return false;
}
INPUT WindowsInput::packet(const InputKey &k, bool down) {
    INPUT in = {};
    if (k.mouse) {
        in.type = INPUT_MOUSE;
        switch (k.code) {
        case VK_LBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case VK_RBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case VK_MBUTTON: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        default: in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; in.mi.mouseData = k.code == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2; break;
        }
        in.mi.dwExtraInfo = InputMonitor::InjectionTag;
    } else {
        in.type = INPUT_KEYBOARD;
        UINT sc = k.scan ? UINT(k.scan) : MapVirtualKeyW(UINT(k.code), MAPVK_VK_TO_VSC_EX);
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (k.extended || (sc & 0xff00) == 0xe000 ? KEYEVENTF_EXTENDEDKEY : 0);
        if ((sc & 0xff) && k.code != VK_PAUSE && k.code != VK_SNAPSHOT) {
            in.ki.wScan = WORD(sc & 0xff); in.ki.dwFlags |= KEYEVENTF_SCANCODE;
        } else in.ki.wVk = WORD(k.code);
        in.ki.dwExtraInfo = InputMonitor::InjectionTag;
    }
    return in;
}
bool WindowsInput::keyEvent(const InputKey &k, bool down, QString &error) { INPUT in = packet(k,down); return send(in,error); }
bool WindowsInput::move(const QPoint &p, QString &error) {
    int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (p.x() < left || p.x() >= left+width || p.y() < top || p.y() >= top+height || width < 2 || height < 2) {
        error = QStringLiteral("鼠标坐标不在当前屏幕范围内"); return false;
    }
    INPUT in = {}; in.type = INPUT_MOUSE;
    // Map each physical pixel to the center of its absolute-coordinate interval.
    in.mi.dx = LONG((qint64(p.x()-left)*65536 + 32768) / width);
    in.mi.dy = LONG((qint64(p.y()-top)*65536 + 32768) / height);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo = InputMonitor::InjectionTag;
    return send(in,error);
}
InputMonitor *InputMonitor::active = nullptr;
bool InputMonitor::start(QString &error) {
    if (active) { error = QStringLiteral("已有输入采集正在运行"); return false; }
    active = this;
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
    mouse = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
    if (keyboard && mouse) return true;
    error = QStringLiteral("无法安装输入采集钩子：%1").arg(GetLastError()); stop(); return false;
}
void InputMonitor::stop() {
    if (keyboard) UnhookWindowsHookEx(keyboard);
    if (mouse) UnhookWindowsHookEx(mouse);
    keyboard = mouse = nullptr;
    if (active == this) active = nullptr;
}
LRESULT CALLBACK InputMonitor::keyboardHook(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && active) {
        auto *k = reinterpret_cast<KBDLLHOOKSTRUCT *>(l);
        if (!(k->flags & LLKHF_INJECTED)) {
            bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
            InputKey key {false, int(k->vkCode), int(k->scanCode & 0xff), bool(k->flags & LLKHF_EXTENDED)};
            POINT p {}; GetCursorPos(&p);
            emit active->observed(key, down, QPoint(p.x,p.y), GetTickCount64());
        }
    }
    return CallNextHookEx(nullptr,n,w,l);
}
LRESULT CALLBACK InputMonitor::mouseHook(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && active) {
        auto *m = reinterpret_cast<MSLLHOOKSTRUCT *>(l);
        if (!(m->flags & LLMHF_INJECTED)) {
            int code = 0; bool down = false;
            switch (w) {
            case WM_LBUTTONDOWN: code=1; down=true; break; case WM_LBUTTONUP: code=1; break;
            case WM_RBUTTONDOWN: code=2; down=true; break; case WM_RBUTTONUP: code=2; break;
            case WM_MBUTTONDOWN: code=4; down=true; break; case WM_MBUTTONUP: code=4; break;
            case WM_XBUTTONDOWN: code=HIWORD(m->mouseData)==XBUTTON1?5:6; down=true; break;
            case WM_XBUTTONUP: code=HIWORD(m->mouseData)==XBUTTON1?5:6; break;
            }
            if (code) emit active->observed(InputKey {true,code,0,false}, down, QPoint(m->pt.x,m->pt.y), GetTickCount64());
        }
    }
    return CallNextHookEx(nullptr,n,w,l);
}
Hotkeys::Hotkeys(QObject *parent) : QObject(parent) { QCoreApplication::instance()->installNativeEventFilter(this); }
Hotkeys::~Hotkeys() { clear(); QCoreApplication::instance()->removeNativeEventFilter(this); }
void Hotkeys::clear() { for (int id : current.keys()) UnregisterHotKey(nullptr,id); current.clear(); }
bool Hotkeys::decode(const QKeySequence &s, UINT &mod, UINT &vk) {
    if (s.count()!=1) return false;
    auto c = s[0]; int key = int(c.key()); auto m=c.keyboardModifiers();
    mod = MOD_NOREPEAT | (m.testFlag(Qt::ControlModifier)?MOD_CONTROL:0) | (m.testFlag(Qt::AltModifier)?MOD_ALT:0)
        | (m.testFlag(Qt::ShiftModifier)?MOD_SHIFT:0) | (m.testFlag(Qt::MetaModifier)?MOD_WIN:0);
    if ((key>='A'&&key<='Z') || (key>='0'&&key<='9')) vk=UINT(key);
    else if (key>=Qt::Key_F1&&key<=Qt::Key_F24) vk=VK_F1+UINT(key-Qt::Key_F1);
    else switch (key) {
    case Qt::Key_Space: vk=VK_SPACE; break; case Qt::Key_Escape: vk=VK_ESCAPE; break;
    case Qt::Key_Pause: vk=VK_PAUSE; break; case Qt::Key_Insert: vk=VK_INSERT; break;
    case Qt::Key_Delete: vk=VK_DELETE; break; case Qt::Key_Home: vk=VK_HOME; break;
    case Qt::Key_End: vk=VK_END; break; case Qt::Key_PageUp: vk=VK_PRIOR; break;
    case Qt::Key_PageDown: vk=VK_NEXT; break;
    default: return false;
    }
    return vk != VK_F12;
}
bool Hotkeys::set(const QKeySequence &toggle, const QKeySequence &stop, const QKeySequence &record, QString &error) {
    QHash<int,QKeySequence> next {{1,toggle},{2,stop},{3,record}};
    if (toggle==stop || toggle==record || stop==record) { error=QStringLiteral("三个快捷键必须不同"); return false; }
    for (const auto &s : next) { UINT m,v; if (!decode(s,m,v)) { error=QStringLiteral("请使用单组字母、数字或功能键（F12 为系统保留键）"); return false; } }
    auto previous=current; clear();
    for (int id : {1,2,3}) {
        UINT m,v; decode(next[id],m,v);
        if (!RegisterHotKey(nullptr,id,m,v)) {
            error=QStringLiteral("%1 已被其他程序占用，原设置已恢复").arg(next[id].toString(QKeySequence::NativeText));
            clear();
            for (int oldId : previous.keys()) {
                decode(previous[oldId],m,v);
                if (RegisterHotKey(nullptr,oldId,m,v)) current[oldId]=previous[oldId];
            }
            return false;
        }
        current[id]=next[id];
    }
    return true;
}
bool Hotkeys::nativeEventFilter(const QByteArray &, void *message, qintptr *) {
    auto *msg=static_cast<MSG *>(message);
    if (msg->message!=WM_HOTKEY || !current.contains(int(msg->wParam))) return false;
    emit triggered(int(msg->wParam)); return true;
}
