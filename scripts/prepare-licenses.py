"""Save matching Qt source offer, Qt license texts and bundled third-party notices."""
import argparse
import io
import json
from pathlib import Path
import tarfile
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--version", default="6.5.3")
parser.add_argument("--output", default="docs/qt-licenses")
args = parser.parse_args()
version = args.version
major_minor = ".".join(version.split(".")[:2])
url = f"https://download.qt.io/archive/qt/{major_minor}/{version}/submodules/qtbase-everywhere-src-{version}.tar.xz"
cache = Path(".tools") / f"qtbase-{version}.tar.xz"
cache.parent.mkdir(exist_ok=True)
if not cache.exists():
    print(f"Downloading matching Qt source: {url}", flush=True)
    urllib.request.urlretrieve(url, cache)
out = Path(args.output)
out.mkdir(parents=True, exist_ok=True)
notices = []
with tarfile.open(cache) as archive:
    for member in archive:
        if not member.isfile() or member.size > 2_000_000:
            continue
        relative = "/".join(member.name.split("/")[1:])
        basename = relative.split("/")[-1].lower()
        if relative.startswith("LICENSES/"):
            data = archive.extractfile(member).read()
            (out / relative.split("/")[-1]).write_bytes(data)
        elif relative.startswith("src/3rdparty/") and (basename.startswith(("license", "copying", "copyright", "notice")) or basename == "qt_attribution.json"):
            text = archive.extractfile(member).read().decode("utf-8", "replace")
            notices.append(f"\n\n{'=' * 72}\n{relative}\n{'=' * 72}\n{text}")
(out / "THIRD-PARTY-NOTICES.txt").write_text("QtBase third-party attribution and license texts\n" + "".join(notices), encoding="utf-8")
(out / "SOURCE-OFFER.txt").write_text(
    f"This release dynamically links to unmodified Qt {version} (Core, GUI, Widgets, Network and Windows platform plugin).\n"
    "Qt is provided under LGPL-3.0; its complete corresponding source code, including third-party code, is available without charge at:\n"
    f"{url}\nhttps://code.qt.io/cgit/qt/qtbase.git/\n\n"
    "You may replace these shared libraries with compatible modified builds. Reverse engineering for debugging changes to Qt is permitted.\n"
    "Application source and CMake build instructions are included in the public repository. The application's MIT license does not change Qt's license.\n"
    "The package also includes Microsoft Visual C++ Redistributable runtime DLLs.\n"
    "https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files\n", encoding="utf-8")
print(f"Prepared Qt {version} licenses and {len(notices)} third-party notices in {out}")
