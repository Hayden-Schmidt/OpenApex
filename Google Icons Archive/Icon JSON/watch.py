"""
Watches maneuvers.json and reruns build_icons.py the instant it changes.

Browser JS can't launch a local process (no filesystem/process access from a
page, least of all a file:// one) -- so "rebuild on page reload" isn't
literally possible without switching from double-clicking index.html to
running a local server. This is the practical equivalent for the current
file:// workflow: leave this running in a terminal while you edit
maneuvers.json. Save -> it regenerates Demo/icons-data.js immediately ->
refresh the browser tab.

Run: python watch.py   (Ctrl+C to stop)
"""
import subprocess, sys, time, os

HERE = os.path.dirname(__file__)
MANEUVERS = os.path.join(HERE, "maneuvers.json")
BUILD = os.path.join(HERE, "build_icons.py")
POLL_SECONDS = 0.5


def run_build():
    print("--- maneuvers.json changed, rebuilding ---", flush=True)
    result = subprocess.run([sys.executable, BUILD], cwd=HERE)
    if result.returncode == 0:
        print("--- done, refresh the browser ---\n", flush=True)
    else:
        print("--- build_icons.py failed, see above ---\n", flush=True)


def main():
    print(f"watching {MANEUVERS} (Ctrl+C to stop)")
    run_build()
    last_mtime = os.path.getmtime(MANEUVERS)
    try:
        while True:
            time.sleep(POLL_SECONDS)
            try:
                mtime = os.path.getmtime(MANEUVERS)
            except FileNotFoundError:
                continue
            if mtime != last_mtime:
                last_mtime = mtime
                run_build()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
