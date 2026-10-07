"""Native monitor coverage and input regression matrix.

By default this changes only the test process's Qt scaling environment. With
--change-layout it temporarily tests a secondary monitor at 1920x1080 above,
below, left, and right of the primary monitor. Display modes are not persisted
and are restored in finally, including on a failed check. Run with no other
LianDianQi instance open. Test clicks are guarded by overlay hit-testing.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess

u = c.WinDLL("user32", use_last_error=True)
u.SetProcessDpiAwarenessContext.argtypes = [c.c_void_p]
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))


class Position(c.Structure):
    _fields_ = [("x", w.LONG), ("y", w.LONG)]


class DisplayFields(c.Structure):
    _fields_ = [("position", Position), ("orientation", w.DWORD), ("fixedOutput", w.DWORD)]


class ModeUnion(c.Union):
    _fields_ = [("display", DisplayFields), ("printer", c.c_byte * 16)]


class Mode(c.Structure):
    _anonymous_ = ("data",)
    _fields_ = [("device", w.WCHAR * 32), ("specVersion", w.WORD), ("driverVersion", w.WORD),
        ("size", w.WORD), ("driverExtra", w.WORD), ("fields", w.DWORD), ("data", ModeUnion),
        ("color", w.SHORT), ("duplex", w.SHORT), ("yResolution", w.SHORT), ("ttOption", w.SHORT),
        ("collate", w.SHORT), ("formName", w.WCHAR * 32), ("logPixels", w.WORD),
        ("bits", w.DWORD), ("width", w.DWORD), ("height", w.DWORD), ("displayFlags", w.DWORD),
        ("frequency", w.DWORD), ("icmMethod", w.DWORD), ("icmIntent", w.DWORD),
        ("mediaType", w.DWORD), ("ditherType", w.DWORD), ("reserved1", w.DWORD),
        ("reserved2", w.DWORD), ("panningWidth", w.DWORD), ("panningHeight", w.DWORD)]


class Device(c.Structure):
    _fields_ = [("size", w.DWORD), ("name", w.WCHAR * 32), ("description", w.WCHAR * 128),
        ("flags", w.DWORD), ("id", w.WCHAR * 128), ("key", w.WCHAR * 128)]


u.EnumDisplayDevicesW.argtypes = [w.LPCWSTR, w.DWORD, c.POINTER(Device), w.DWORD]
u.EnumDisplaySettingsW.argtypes = [w.LPCWSTR, w.DWORD, c.POINTER(Mode)]
u.ChangeDisplaySettingsExW.argtypes = [w.LPCWSTR, c.POINTER(Mode), w.HWND, w.DWORD, c.c_void_p]
u.ChangeDisplaySettingsExW.restype = w.LONG


def mode_for(name, index=0xFFFFFFFF):
    mode = Mode()
    mode.size = c.sizeof(mode)
    if not u.EnumDisplaySettingsW(name, index, c.byref(mode)):
        return None
    return mode


def displays():
    result = []
    index = 0
    while True:
        device = Device()
        device.size = c.sizeof(device)
        if not u.EnumDisplayDevicesW(None, index, c.byref(device), 0):
            return result
        index += 1
        if device.flags & 1 and not device.flags & 8:
            mode = mode_for(device.name)
            if mode:
                result.append((device.name, bool(device.flags & 4), mode))


def snapshot():
    return [{"device": name, "primary": primary, "position": [mode.display.position.x, mode.display.position.y],
        "size": [mode.width, mode.height], "refresh": mode.frequency} for name, primary, mode in displays()]


def set_mode(name, mode):
    # CDS_TEST first; apply only to the current session, without CDS_UPDATEREGISTRY.
    status = u.ChangeDisplaySettingsExW(name, c.byref(mode), None, 2, None)
    if status != 0:
        raise RuntimeError(f"Unsupported display test mode for {name}: {status}")
    status = u.ChangeDisplaySettingsExW(name, c.byref(mode), None, 0, None)
    if status != 0:
        raise RuntimeError(f"Display mode change failed for {name}: {status}")


def smaller_mode(name, original):
    candidates = []
    index = 0
    while (mode := mode_for(name, index)) is not None:
        index += 1
        if (mode.width, mode.height, mode.bits, mode.display.orientation) == (1920, 1080, original.bits, original.display.orientation):
            candidates.append(mode)
    if not candidates:
        raise RuntimeError("Secondary monitor does not support 1920x1080")
    return min(candidates, key=lambda mode: abs(mode.frequency - original.frequency))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default="build/clicker_tests.exe")
    parser.add_argument("--qt-bin", default=".tools/Qt/6.5.3/msvc2019_64/bin")
    parser.add_argument("--output", default="artifacts/display-smoke-1.1.0")
    parser.add_argument("--vision-exe", default="build/vision_tests.exe")
    parser.add_argument("--change-layout", action="store_true")
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = displays()
    if len(original) < 2:
        raise RuntimeError("Requires at least two active monitors")
    report = {"original": snapshot(), "checks": [], "restored": False}
    (output / "original-displays.json").write_text(json.dumps(report["original"], indent=2), encoding="utf-8")
    tests = ["nativePickerCoversEveryMonitor", "nativePickerSelectsCornersAndEdges",
        "nativePickerBlocksUnderlyingClickAndRestoresWindow", "nativeMoveUsesPhysicalPixelsAndRejectsDesktopGaps",
        "pickerWaitsForReleaseAndEscPreservesCoordinates", "pickerEscDuringMousePressAbsorbsRelease",
        "nativeRecordingKeepsMouseReleaseOverItsOwnWindow", "nativePickerEscDoesNotLeakMouseRelease",
        "controlToggleCannotStartWhilePicking"]

    def run(name, factor=None, screen_factors=None):
        env = os.environ.copy()
        env["PATH"] = str(Path(args.qt_bin).resolve()) + os.pathsep + env["PATH"]
        env["QT_QPA_PLATFORM"] = "windows"
        for key in ("QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI"):
            env.pop(key, None)
        if factor:
            env["QT_SCALE_FACTOR"] = factor
        if screen_factors:
            env["QT_SCREEN_SCALE_FACTORS"] = screen_factors
        result_file = output / f"{name}.txt"
        check = {"name": name, "displays": snapshot(), "qtScaleFactor": factor,
            "qtScreenScaleFactors": screen_factors, "result": str(result_file)}
        print(f"Testing {name}: {check['displays']}", flush=True)
        proc = subprocess.run([str(Path(args.exe).resolve()), *tests, "-o", str(result_file) + ",txt"], env=env, timeout=60)
        check["exitCode"] = proc.returncode
        report["checks"].append(check)
        if proc.returncode:
            print(result_file.read_text(encoding="utf-8"), flush=True)
            raise AssertionError(f"Native display regression failed: {name}")
        vision = Path(args.vision_exe).resolve()
        if vision.is_file():
            vision_file = output / f"{name}-vision.txt"
            result = subprocess.run([str(vision), "nativeRectangleSelectionAcrossMonitors", "nativePhysicalMatchOnEachMonitor",
                                     "-o", str(vision_file) + ",txt"], env=env, timeout=30)
            check["visionResult"] = str(vision_file)
            check["visionExitCode"] = result.returncode
            if result.returncode:
                print(vision_file.read_text(encoding="utf-8"), flush=True)
                raise AssertionError(f"Native vision display regression failed: {name}")

    changed = False
    try:
        run("current")
        run("scale-125", factor="1.25")
        run("scale-150", factor="1.5")
        run("mixed-100-150", screen_factors="1;1.5")
        run("mixed-150-100", screen_factors="1.5;1")
        if args.change_layout:
            primary = next(entry for entry in original if entry[1])
            secondary = next(entry for entry in original if not entry[1])
            name, _, saved = secondary
            base = smaller_mode(name, saved)
            px, py = primary[2].display.position.x, primary[2].display.position.y
            layouts = {"left": (px - base.width, py), "right": (px + primary[2].width, py),
                "above": (px, py - base.height), "below": (px, py + primary[2].height)}
            for layout, (x, y) in layouts.items():
                mode = Mode.from_buffer_copy(base)
                mode.display.position.x, mode.display.position.y = x, y
                mode.fields |= 0x20  # DM_POSITION
                changed = True
                set_mode(name, mode)
                run(f"1920x1080-{layout}", screen_factors="1;1.5")
    finally:
        if changed:
            errors = []
            for name, _, mode in original:
                try:
                    set_mode(name, mode)
                except Exception as error:
                    errors.append(str(error))
            if errors:
                report["restoreErrors"] = errors
        report["restored"] = snapshot() == report["original"]
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        if not report["restored"]:
            raise RuntimeError(f"Display configuration restoration failed: {report}")
    print(f"Passed {len(report['checks'])} scenarios; original display configuration restored.", flush=True)


if __name__ == "__main__":
    main()
