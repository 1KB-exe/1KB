#!/usr/bin/env python3
"""End-to-end launcher and runtime execution tests."""
import argparse
import io
import zipfile
import http.server
import os
import shutil
import subprocess
import tempfile
import threading
import time
import winreg
from pathlib import Path


class Server(http.server.ThreadingHTTPServer):
    allow_reuse_address = True


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        server = self.server
        if self.path == "/1KB.ini":
            self.send_response(302)
            self.send_header("Location", f"http://localhost:{server.server_port}/manifest.ini")
            self.end_headers()
            return
        if self.path == "/manifest.ini":
            output = io.BytesIO()
            with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('application.exe', server.application)
            server.package = output.getvalue()
            body = (
                f"version={server.version}\n"
                f"download=http://localhost:{server.server_port}/application-{server.version}.zip\n"
                f"updates={server.updates}\n"
            ).encode()
        elif self.path.startswith("/application-") and self.path.endswith(".zip"):
            server.application_requests += 1
            if server.fail_application:
                self.send_response(503)
                self.end_headers()
                return
            body = server.package
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def cleanup_test_integrations():
    uninstall = r"Software\Microsoft\Windows\CurrentVersion\Uninstall"
    marker = "\\Temp\\1KB.exe runtime "
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, uninstall, 0, winreg.KEY_READ | winreg.KEY_WRITE) as parent:
        doomed = []
        index = 0
        while True:
            try:
                name = winreg.EnumKey(parent, index)
            except OSError:
                break
            index += 1
            try:
                with winreg.OpenKey(parent, name) as key:
                    location = winreg.QueryValueEx(key, "InstallLocation")[0]
                    display = winreg.QueryValueEx(key, "DisplayName")[0]
                if marker.lower() in location.lower() and display in ("capture-runtime", "capture-gui"):
                    doomed.append(name)
            except OSError:
                pass
        for name in doomed:
            winreg.DeleteKey(parent, name)
    programs = Path(os.environ["APPDATA"]) / "Microsoft" / "Windows" / "Start Menu" / "Programs"
    for name in ("capture-runtime.lnk", "capture-gui.lnk"):
        (programs / name).unlink(missing_ok=True)


def wait_for(predicate, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if predicate():
            return True
        time.sleep(0.05)
    return False


def build_launcher(builder, application, local, app_id):
    env = os.environ.copy()
    env["LOCALAPPDATA"] = str(local)
    env["TMP"] = str(local)
    result = subprocess.run(
        [str(builder), str(application)], env=env, input=f"{app_id}\n\n\nq\n",
        capture_output=True, text=True, errors="replace", timeout=60,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)
    records = list((local / "1kb").rglob("app.ini"))
    assert len(records) == 1, (result.stdout, result.stderr, records)
    launcher = records[0].parent / f"{application.stem}.exe"
    assert launcher.exists() and launcher.stat().st_size <= 1024
    assert "console_mode=" not in records[0].read_text()
    return launcher, env


def check_github_identity_casing(builder, application, root):
    launchers = []
    for index, spelling in enumerate(("gh:1KB-exe/1KB", "gh:1kb-exe/1kb", "gh:1Kb-Exe/1kB")):
        local = root / f"github-{index}"
        local.mkdir()
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(local)
        env["TMP"] = str(local)
        result = subprocess.run(
            [str(builder), str(application)], env=env, input=f"{spelling}\n\n\nq\n",
            capture_output=True, text=True, errors="replace", timeout=60,
        )
        assert result.returncode == 0, (result.stdout, result.stderr)
        record = local / "1kb" / "1kb-exe" / "1kb" / "app.ini"
        assert record.exists()
        assert "app_id=gh:1kb-exe/1kb\n" in record.read_text()
        assert "gh:1kb-exe/1kb" in result.stdout
        launcher = record.parent / f"{application.stem}.exe"
        launchers.append(launcher.read_bytes())
    assert launchers[0] == launchers[1] == launchers[2]


def detach_console_launcher(builder, application, local):
    env = os.environ.copy()
    env["LOCALAPPDATA"] = str(local)
    env["TMP"] = str(local)
    # Open the remembered app, edit Console behavior, then save and rebuild.
    result = subprocess.run(
        [str(builder), str(application)], env=env,
        input="2\n4\ndetached\n7\n\nq\nq\n", capture_output=True,
        text=True, errors="replace", timeout=60,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)
    records = list((local / "1kb").rglob("app.ini"))
    assert len(records) == 1
    assert "console_mode=detached\n" in records[0].read_text()
    return records[0].parent / f"{application.stem}.exe", env


def change_release_to_gui(builder, old_application, gui_application, local):
    env = os.environ.copy()
    env["LOCALAPPDATA"] = str(local)
    env["TMP"] = str(local)
    result = subprocess.run(
        [str(builder), str(old_application)], env=env,
        input=f"2\n2\n{gui_application}\n6\n\nq\nq\n",
        capture_output=True, text=True, errors="replace", timeout=60,
    )
    assert result.returncode == 0, (result.stdout, result.stderr)
    records = list((local / "1kb").rglob("app.ini"))
    assert len(records) == 1
    text = records[0].read_text()
    assert f"release={gui_application.resolve()}\n" in text
    assert "console_mode=" not in text


def prepare_runtime(local, runtime):
    shutil.copy2(runtime, local / "r")
    (local / "1kb").mkdir(exist_ok=True)
    (local / "1kb" / "last-runtime-update-check.txt").write_text("1\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--builder", required=True)
    parser.add_argument("--capture-runtime", required=True)
    parser.add_argument("--gui-runtime", required=True)
    parser.add_argument("--no-icon-runtime", required=True)
    parser.add_argument("--test-runtime", required=True)
    args = parser.parse_args()
    cleanup_test_integrations()
    builder = Path(args.builder).resolve()
    capture = Path(args.capture_runtime).resolve()
    gui = Path(args.gui_runtime).resolve()
    no_icon = Path(args.no_icon_runtime).resolve()
    runtime = Path(args.test_runtime).resolve()

    server = Server(("127.0.0.1", 0), Handler)
    server.application = capture.read_bytes()
    server.application_requests = 0
    server.fail_application = False
    server.version = "1.2.3"
    server.updates = "background"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="1KB.exe runtime ") as temporary:
            root = Path(temporary)
            app_id = f"http://localhost:{server.server_port}/1KB.ini#capture-runtime"

            # Concurrent recovery-cache runtimes must atomically promote a valid
            # canonical runtime without sharing a fixed download destination.
            recovery = root / "recovery"
            recovery.mkdir()
            recovery_env = os.environ.copy()
            recovery_env["LOCALAPPDATA"] = str(recovery)
            recovery_env["TMP"] = str(recovery)
            candidates = [recovery / "cache-a.exe", recovery / "cache-b.exe"]
            processes = []
            for index, candidate in enumerate(candidates):
                shutil.copy2(runtime, candidate)
                env = recovery_env.copy()
                env["ONEKB_RUNTIME_TEST_OUTPUT"] = str(recovery / f"recovery-{index}.exe")
                processes.append(subprocess.Popen([str(candidate)], env=env))
            assert all(process.wait(timeout=40) == 43 for process in processes)
            canonical = recovery / "r"
            assert canonical.read_bytes() == runtime.read_bytes()

            # Display casing never changes GitHub identity, storage, or launcher bytes.
            check_github_identity_casing(builder, capture, root)

            # A console launcher must install, forward argv and runtime state,
            # preserve standard handles, and return the application's exit code.
            local = root / "console"
            local.mkdir()
            launcher, env = build_launcher(builder, capture, local, app_id)
            prepare_runtime(local, runtime)
            output = root / "console-output.txt"
            env["BOOTSTRAP_TEST_OUTPUT"] = str(output)
            first = subprocess.run([str(launcher), "first install"], env=env, timeout=40)
            assert first.returncode == 37 and output.exists()
            text = output.read_text(encoding="utf-16-le")
            assert f"ONEKB_PATH={launcher.resolve()}\n" in text
            assert "ONEKB_VERSION=1.2.3\n" in text and "first install" in text
            assert "STARTUPFLAGS=384\n" in text
            currents = list(local.rglob("current.txt"))
            assert len(currents) == 1 and currents[0].read_text() == "1.2.3\n"
            current = currents[0]

            # A background update must atomically advance the installed version.
            server.version = "2.0.0"
            server.updates = "restart"
            server.application = capture.read_bytes() + b"VERSION-2"
            # Keep this update worker from spending its mutex hold on the
            # unrelated runtime self-update check during a long test run.
            (local / "1kb" / "last-runtime-update-check.txt").touch()
            output.unlink()
            second = subprocess.run([str(launcher), "background update"], env=env, timeout=40)
            assert second.returncode == 37 and wait_for(output.exists)
            assert wait_for(lambda: current.exists() and current.read_text() == "2.0.0\n")
            # current.txt is committed just before the detached worker releases
            # its update mutex; do not turn that intentional overlap into a race.
            time.sleep(0.5)

            # Restart mode downloads while the app is running, stops it only for
            # mutation, and starts the new current version with the original argv.
            server.version = "2.1.0"
            server.updates = "restart"
            server.application = capture.read_bytes() + b"VERSION-2.1"
            wait_file = root / "keep-running"
            wait_file.write_text("1")
            env["BOOTSTRAP_TEST_WAIT_FILE"] = str(wait_file)
            output.unlink()
            restarting = subprocess.Popen([str(launcher), "restart update"], env=env)
            try:
                updated = wait_for(lambda: output.exists() and "ONEKB_VERSION=2.1.0\n" in output.read_text(encoding="utf-16-le"), 30)
            finally:
                wait_file.unlink(missing_ok=True)
            exit_code = restarting.wait(timeout=40)
            assert updated, (current.read_text(), exit_code)
            assert exit_code == 37
            assert current.read_text() == "2.1.0\n"
            env.pop("BOOTSTRAP_TEST_WAIT_FILE")

            # A failed payload download must leave the working version active.
            server.version = "3.0.0"
            server.fail_application = True
            before = server.application_requests
            output.unlink()
            failed = subprocess.run([str(launcher), "failed update"], env=env, timeout=40)
            assert failed.returncode == 37 and wait_for(output.exists)
            assert wait_for(lambda: server.application_requests > before)
            assert current.read_text() == "2.1.0\n"
            server.fail_application = False

            # Detached console mode persists its non-default value and uses
            # the GUI core: it returns immediately while Windows gives the
            # console application a separate console.
            detached_local = root / "detached-console"
            detached_local.mkdir()
            detached_launcher, detached_env = build_launcher(
                builder, capture, detached_local, app_id
            )
            detached_launcher, detached_env = detach_console_launcher(
                builder, capture, detached_local
            )
            assert detached_launcher.stat().st_size <= 1024
            prepare_runtime(detached_local, runtime)
            detached_output = root / "detached-output.txt"
            detached_env["BOOTSTRAP_TEST_OUTPUT"] = str(detached_output)
            detached_run = subprocess.run(
                [str(detached_launcher), "detached console"],
                env=detached_env, timeout=40,
            )
            assert detached_run.returncode == 0
            assert wait_for(detached_output.exists)
            assert "STARTUPFLAGS=128\n" in detached_output.read_text(
                encoding="utf-16-le"
            )
            change_release_to_gui(builder, capture, gui, detached_local)

            # The iconless console core is a distinct tiny executable and must
            # perform the same real install and handoff.
            iconless_local = root / "iconless"
            iconless_local.mkdir()
            iconless_launcher, iconless_env = build_launcher(builder, no_icon, iconless_local, app_id)
            prepare_runtime(iconless_local, runtime)
            iconless_output = root / "iconless-output.txt"
            iconless_env["BOOTSTRAP_TEST_OUTPUT"] = str(iconless_output)
            server.version = "2.0.0"
            iconless_run = subprocess.run([str(iconless_launcher), "iconless"], env=iconless_env, timeout=40)
            assert iconless_run.returncode == 37 and iconless_output.exists()

            # GUI launchers use the GUI core and detach while still forwarding
            # the launcher path and arguments to the application.
            gui_local = root / "gui"
            gui_local.mkdir()
            gui_launcher, gui_env = build_launcher(builder, gui, gui_local, app_id)
            prepare_runtime(gui_local, runtime)
            gui_output = root / "gui-output.txt"
            gui_env["BOOTSTRAP_TEST_OUTPUT"] = str(gui_output)
            gui_run = subprocess.run([str(gui_launcher), "gui handoff"], env=gui_env, timeout=40)
            assert gui_run.returncode == 0 and wait_for(gui_output.exists)
            gui_text = gui_output.read_text(encoding="utf-16-le")
            assert f"ONEKB_PATH={gui_launcher.resolve()}\n" in gui_text
            assert "gui handoff" in gui_text and "STARTUPFLAGS=128\n" in gui_text

        print("PASS attached/detached console, iconless, and GUI launcher/runtime execution and updates")
    finally:
        cleanup_test_integrations()
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
