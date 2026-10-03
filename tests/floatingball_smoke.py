"""Verify the registered FloatingBall commands and its own process status probe."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
sys.stdout.reconfigure(encoding="utf-8")

parser = argparse.ArgumentParser()
parser.add_argument("--floatingball",required=True)
args = parser.parse_args()
root = Path(args.floatingball).resolve()
config = json.loads((root / ".settings" / "tools.json").read_text(encoding="utf-8-sig"))
entry = next(item for item in config["tools"] if item["key"] == "liandianqi")
python = root / ".venv" / "Scripts" / "python.exe"
probe_code = "import json,sys; import ball; print(json.dumps(ball.probe_status(json.loads(sys.argv[1])),ensure_ascii=False))"

def probe():
    child_env = os.environ.copy()
    child_env["PYTHONIOENCODING"] = "utf-8"
    result = subprocess.run([str(python),"-c",probe_code,json.dumps(entry["status"])],cwd=root,env=child_env,capture_output=True,text=True,encoding="utf-8",timeout=15)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])

with tempfile.TemporaryDirectory(prefix="liandianqi-floatingball-test-") as temporary:
    env = os.environ.copy()
    env["LIANDIANQI_TEST_SETTINGS"] = temporary
    before = probe()
    assert not before["running"], "An instance is already running; close it before this test."
    process = subprocess.Popen(entry["start"]["cmd"],cwd=entry["start"]["cwd"],env=env)
    try:
        time.sleep(1)
        during = probe()
        assert during["running"], during
        assert during["url"].startswith("file:///"), during
        # The Open button launches this file URL via QDesktopServices/ShellExecute.
        shown = subprocess.run([entry["start"]["cmd"][0],"--show"],env=env,timeout=8)
        assert shown.returncode == 0 and process.poll() is None
        stopped = subprocess.run(entry["stop"]["cmd"],cwd=entry["stop"]["cwd"],env=env,timeout=8)
        assert stopped.returncode == 0
        assert process.wait(timeout=8) == 0
        after = probe()
        assert not after["running"], after
        print(json.dumps({"entry":entry["name"],"before":before,"during":during,"after":after,"exitCode":process.returncode},ensure_ascii=False,indent=2))
    finally:
        if process.poll() is None:
            subprocess.run(entry["stop"]["cmd"],env=env,timeout=8)
            process.wait(timeout=8)
