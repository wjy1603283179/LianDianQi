"""Windows integration tests against the actual portable executable.

Uses uncommon F24 as a harmless keyboard target and a temporary test window for
mouse input. Watches actual tagged SendInput events, process exits, shortcut
messages, and resource use. Never loads or modifies the user's saved settings.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

u = c.WinDLL("user32", use_last_error=True)
k = c.WinDLL("kernel32", use_last_error=True)
ps = c.WinDLL("psapi", use_last_error=True)
PTR = c.c_size_t
HOOK = c.WINFUNCTYPE(c.c_ssize_t, c.c_int, w.WPARAM, w.LPARAM)
ENUM = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.SetWindowsHookExW.argtypes = [c.c_int, HOOK, w.HINSTANCE, w.DWORD]
u.SetWindowsHookExW.restype = w.HANDLE
u.CallNextHookEx.argtypes = [w.HANDLE, c.c_int, w.WPARAM, w.LPARAM]
u.CallNextHookEx.restype = c.c_ssize_t
u.UnhookWindowsHookEx.argtypes = [w.HANDLE]
k.GetModuleHandleW.argtypes = [w.LPCWSTR]
k.GetModuleHandleW.restype = w.HMODULE
u.CreateWindowExW.argtypes = [w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.c_int,c.c_int,c.c_int,c.c_int,w.HWND,w.HMENU,w.HINSTANCE,c.c_void_p]
u.CreateWindowExW.restype = w.HWND
u.SetWindowPos.argtypes = [w.HWND,w.HWND,c.c_int,c.c_int,c.c_int,c.c_int,w.UINT]
u.DestroyWindow.argtypes = [w.HWND]
u.ShowWindow.argtypes = [w.HWND,c.c_int]
u.GetForegroundWindow.restype = w.HWND
u.SetForegroundWindow.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND,w.UINT,w.WPARAM,w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND,c.POINTER(w.DWORD)]
u.IsWindowVisible.argtypes = [w.HWND]
u.EnumWindows.argtypes = [ENUM,w.LPARAM]
u.PeekMessageW.argtypes = [c.POINTER(w.MSG),w.HWND,w.UINT,w.UINT,w.UINT]
u.TranslateMessage.argtypes = [c.POINTER(w.MSG)]
u.DispatchMessageW.argtypes = [c.POINTER(w.MSG)]
u.GetAsyncKeyState.argtypes = [c.c_int]
u.GetAsyncKeyState.restype = c.c_short

class KBD(c.Structure):
    _fields_ = [("vkCode",w.DWORD),("scanCode",w.DWORD),("flags",w.DWORD),("time",w.DWORD),("extra",PTR)]
class MOUSE(c.Structure):
    _fields_ = [("pt",w.POINT),("data",w.DWORD),("flags",w.DWORD),("time",w.DWORD),("extra",PTR)]
class MI(c.Structure):
    _fields_ = [("dx",w.LONG),("dy",w.LONG),("data",w.DWORD),("flags",w.DWORD),("time",w.DWORD),("extra",PTR)]
class KI(c.Structure):
    _fields_ = [("vk",w.WORD),("scan",w.WORD),("flags",w.DWORD),("time",w.DWORD),("extra",PTR)]
class UNION(c.Union):
    _fields_ = [("mi",MI),("ki",KI)]
class INPUT(c.Structure):
    _anonymous_ = ("data",)
    _fields_ = [("type",w.DWORD),("data",UNION)]
u.SendInput.argtypes = [w.UINT,c.POINTER(INPUT),c.c_int]
u.SendInput.restype = w.UINT

class COUNTERS(c.Structure):
    _fields_ = [("cb",w.DWORD),("faults",w.DWORD)] + [(name,PTR) for name in
        ("peakWorking","working","peakPaged","paged","peakNonpaged","nonpaged","private","peakPrivate")]
ps.GetProcessMemoryInfo.argtypes = [w.HANDLE,c.POINTER(COUNTERS),w.DWORD]
k.GetProcessTimes.argtypes = [w.HANDLE,c.POINTER(w.FILETIME),c.POINTER(w.FILETIME),c.POINTER(w.FILETIME),c.POINTER(w.FILETIME)]

events = []
TAG = 0x4C445151

@HOOK
def keyboard(n,wp,lp):
    if n == 0:
        event = c.cast(lp,c.POINTER(KBD)).contents
        if event.extra == TAG:
            events.append(("keyboard",event.vkCode,not bool(event.flags & 0x80),None,time.perf_counter()))
    return u.CallNextHookEx(None,n,wp,lp)

@HOOK
def mouse(n,wp,lp):
    if n == 0:
        event = c.cast(lp,c.POINTER(MOUSE)).contents
        if event.extra == TAG and wp in (0x201,0x202,0x204,0x205):
            events.append(("mouse",1 if wp in (0x201,0x202) else 2,wp in (0x201,0x204),(event.pt.x,event.pt.y),time.perf_counter()))
    return u.CallNextHookEx(None,n,wp,lp)

def pump():
    msg = w.MSG()
    while u.PeekMessageW(c.byref(msg),None,0,0,1):
        u.TranslateMessage(c.byref(msg))
        u.DispatchMessageW(c.byref(msg))

def wait_for(predicate, timeout=6):
    end = time.perf_counter() + timeout
    while time.perf_counter() < end:
        pump()
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError("Timed out waiting for native test condition")

def pump_for(seconds):
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        pump()
        time.sleep(.01)

def cpu_time(proc):
    times = [w.FILETIME() for _ in range(4)]
    assert k.GetProcessTimes(int(proc._handle),*[c.byref(x) for x in times])
    return sum((x.dwHighDateTime << 32) + x.dwLowDateTime for x in times[2:]) / 10_000_000

def resources(proc):
    memory = COUNTERS()
    memory.cb = c.sizeof(memory)
    assert ps.GetProcessMemoryInfo(int(proc._handle),c.byref(memory),memory.cb)
    before = cpu_time(proc)
    start = time.perf_counter()
    pump_for(2)
    cpu = (cpu_time(proc) - before) / (time.perf_counter() - start) * 100
    return {"workingSetMiB":round(memory.working / 1048576,2),"privateMiB":round(memory.private / 1048576,2),"cpuPercentOfOneCore":round(cpu,3)}

def window_for(pid):
    found = []
    @ENUM
    def each(hwnd,lp):
        process_id = w.DWORD()
        u.GetWindowThreadProcessId(hwnd,c.byref(process_id))
        if process_id.value == pid and u.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    u.EnumWindows(each,0)
    return found[0] if found else None

def press(vk):
    inputs = (INPUT * 2)()
    for i in range(2):
        inputs[i].type = 1
        inputs[i].ki = KI(vk,0,2 if i else 0,0,0x534D4F4B)
    assert u.SendInput(2,inputs,c.sizeof(INPUT)) == 2

def run_tests(exe):
    report = {"executable":str(exe),"checks":[]}
    previous_foreground = u.GetForegroundWindow()
    previous_cursor = w.POINT()
    u.GetCursorPos(c.byref(previous_cursor))
    hooks = [u.SetWindowsHookExW(13,keyboard,k.GetModuleHandleW(None),0),u.SetWindowsHookExW(14,mouse,k.GetModuleHandleW(None),0)]
    assert all(hooks), f"Cannot install test observers: {c.get_last_error()}"
    target = None
    processes = []
    with tempfile.TemporaryDirectory(prefix="liandianqi-smoke-") as temporary:
        folder = Path(temporary)
        env = os.environ.copy()
        for name in ("QT_QPA_PLATFORM","QT_PLUGIN_PATH","QT_QPA_PLATFORM_PLUGIN_PATH","QML2_IMPORT_PATH"):
            env.pop(name,None)
        env["PATH"] = os.path.join(os.environ.get("SystemRoot",r"C:\Windows"),"System32")
        env["LIANDIANQI_TEST_SETTINGS"] = temporary
        settings = folder / "DianXu"
        settings.mkdir()
        (settings / "LianDianQi.ini").write_text("[General]\ntoggle=F19\nstop=F20\nrecord=F21\n",encoding="utf-8")

        def control(option):
            completed = subprocess.run([str(exe),option],env=env,timeout=8)
            return completed.returncode

        def launch(steps):
            script = folder / "script.json"
            script.write_text(json.dumps({"format":"liandianqi-script","version":1,"rounds":1,"startDelayMs":300,"steps":steps}),encoding="utf-8")
            p = subprocess.Popen([str(exe),"--script",str(script),"--run"],env=env)
            processes.append(p)
            wait_for(lambda:window_for(p.pid) is not None)
            return p

        def close(p):
            hwnd = window_for(p.pid)
            assert hwnd
            assert u.PostMessageW(hwnd,0x10,0,0) # Same native WM_CLOSE as clicking the title-bar ×.
            wait_for(lambda:p.poll() is not None)
            assert p.returncode == 0

        try:
            assert control("--status") == 1
            assert control("--quit") == 0
            report["checks"].append("control commands do not start a background instance")
            key = {"device":"keyboard","code":135}
            baseline = len(events)
            p = launch([{"action":"hold","input":key,"durationMs":0}])
            wait_for(lambda:len(events)>baseline)
            assert events[-1][0:3] == ("keyboard",135,True)
            # The low-level hook runs before Windows commits asynchronous key state.
            # https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc
            wait_for(lambda:u.GetAsyncKeyState(135) & 0x8000)
            assert control("--show") == 0 and p.poll() is None
            report["checks"].append("single-instance relaunch displays existing window")
            baseline = len(events)
            press(0x83) # F20: registered Stop shortcut in the isolated profile.
            wait_for(lambda:len(events)>baseline)
            assert events[-1][0:3] == ("keyboard",135,False)
            wait_for(lambda:not (u.GetAsyncKeyState(135) & 0x8000))
            report["checks"].append("real global Stop shortcut releases an indefinite F24 hold")
            report["idle"] = resources(p)
            baseline = len(events)
            assert control("--toggle") == 0
            wait_for(lambda:len(events)>baseline)
            assert events[-1][2]
            report["holding"] = resources(p)
            baseline = len(events)
            assert control("--stop-task") == 0
            wait_for(lambda:len(events)>baseline)
            assert not events[-1][2]
            report["checks"].append("local IPC toggle and Stop release held input")
            baseline = len(events)
            assert control("--toggle") == 0
            wait_for(lambda:len(events)>baseline)
            before_close = time.perf_counter()
            close(p)
            wait_for(lambda:events[-1][2] is False)
            report["closeMilliseconds"] = round((time.perf_counter()-before_close)*1000,1)
            wait_for(lambda:not (u.GetAsyncKeyState(135) & 0x8000))
            assert control("--status") == 1
            assert u.RegisterHotKey(None,61,0x4000,0x83)
            u.UnregisterHotKey(None,61)
            report["checks"].append("title-bar close releases input, exits process and frees shortcuts")
            baseline = len(events)
            p = launch([{"action":"repeat","input":key,"count":5,"intervalMs":20}])
            wait_for(lambda:len(events)>=baseline+10)
            assert [e[2] for e in events[baseline:baseline+10]] == [True,False]*5
            close(p)
            report["checks"].append("five real keyboard presses produce paired down/up events")
            target = u.CreateWindowExW(0x80,"STATIC","DianXu mouse test target",0x10CF0000,20,20,400,220,None,None,k.GetModuleHandleW(None),None)
            assert target
            baseline = len(events)
            p = launch([{"action":"repeat","input":{"device":"mouse","code":1},"count":4,"intervalMs":30,"fixedPosition":True,"x":120,"y":120}])
            u.SetWindowPos(target,c.c_void_p(-1),20,20,400,220,0x40)
            wait_for(lambda:len(events)>=baseline+8)
            assert [e[2] for e in events[baseline:baseline+8]] == [True,False]*4
            assert all(e[3] == (120,120) for e in events[baseline:baseline+8]), events[baseline:baseline+8]
            close(p)
            report["checks"].append("four real fixed-position mouse clicks hit the test window at physical pixel (120,120)")
            u.DestroyWindow(target)
            target = None
            baseline = len(events)
            p = launch([{"action":"hold","input":key,"durationMs":0}])
            wait_for(lambda:len(events)>baseline)
            assert control("--quit") == 0
            wait_for(lambda:p.poll() is not None)
            assert p.returncode == 0
            wait_for(lambda:not (u.GetAsyncKeyState(135) & 0x8000))
            assert all(proc.poll() is not None for proc in processes)
            report["checks"].append("FloatingBall --quit command gracefully exits with no retained test processes")
            screenshot = folder / "preview.png"
            subprocess.run([str(exe),"--screenshot",str(screenshot)],env=env,check=True,timeout=8)
            assert screenshot.stat().st_size > 1000
            report["checks"].append("portable GUI runs with only System32 on PATH and no Qt development environment")
        finally:
            for p in processes:
                if p.poll() is None:
                    control("--quit")
                    wait_for(lambda:p.poll() is not None)
            for hook in hooks:
                u.UnhookWindowsHookEx(hook)
            if target:
                u.DestroyWindow(target)
            u.SetCursorPos(previous_cursor.x,previous_cursor.y)
            if previous_foreground:
                u.SetForegroundWindow(previous_foreground)
    return report

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe",required=True)
    parser.add_argument("--report",default="artifacts/native-smoke.json")
    args = parser.parse_args()
    result = run_tests(Path(args.exe).resolve())
    destination = Path(args.report)
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(result,ensure_ascii=False,indent=2))
