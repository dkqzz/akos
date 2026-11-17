#!/usr/bin/env python3
"""Exercise the three warehouse programs over real IPv4 sockets."""

from contextlib import ExitStack, contextmanager
from pathlib import Path
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

BUILD = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path("build").resolve()
TIMEOUT = 8


def read_until(path, pattern):
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        text = path.read_text()
        match = re.search(pattern, text, re.MULTILINE)
        if match:
            return match
        time.sleep(0.01)
    raise AssertionError(f"Missing {pattern!r} in {path.name}:\n{text[-4000:]}")


class Process:
    def __init__(self, path, child):
        self.path = path
        self.child = child

    def finish(self, status=0):
        actual = self.child.wait(timeout=TIMEOUT)
        out = self.path.read_text()
        assert actual == status, (actual, status, out)
        return out


class Run:
    def __init__(self, stack, directory, config, options):
        self.stack = stack
        self.directory = directory
        self.log = directory / "events.log"
        self.server = self.spawn(
            "server", "server", "--port", "0", "--config", config, "--log", self.log,
            "--connect-timeout", "2500", *options
        )
        self.port = read_until(self.server.path, r"^LISTEN 127\.0\.0\.1 (\d+)").group(1)

    def spawn(self, name, role, *options):
        path = self.directory / f"{name}.txt"
        with path.open("wb") as output:
            child = subprocess.Popen(
                [str(BUILD / f"warehouse_tcp_{role}"), *map(str, options)],
                stdout=output, stderr=subprocess.STDOUT,
            )
        self.stack.callback(self.cleanup, child)
        return Process(path, child)

    @staticmethod
    def cleanup(child):
        if child.poll() is None:
            child.kill()
            child.wait()

    def client(self, name, role, *options):
        return self.spawn(name, role, "--port", self.port, *options)

    def robot(self, robot_id, name=None):
        return self.client(name or f"robot{robot_id}", "robot", "--id", robot_id)

    def observer(self, name):
        client = self.client(name, "observer")
        read_until(client.path, r"^END SNAPSHOT$")
        return client

    def socket(self):
        return self.stack.enter_context(
            socket.create_connection(("127.0.0.1", int(self.port)), timeout=TIMEOUT)
        )


@contextmanager
def run(config="demo", *options):
    with ExitStack() as stack:
        directory = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="warehouse-tcp-")))
        yield Run(stack, directory, f"data/{config}.cfg", options)


def check_scenario(config, ids, reason, counters, *options):
    with run(config, "--delay", "0", *options) as case:
        observer = case.observer("observer")
        robots = [case.robot(robot_id) for robot_id in ids]
        server = case.server.finish()
        summary = re.search(r"^SUMMARY .*$", server, re.MULTILINE).group()
        assert f"reason={reason} " in summary and counters in summary, summary
        assert summary in observer.finish()
        for robot in robots:
            assert f"STOP OK reason={reason}" in robot.finish()
        return case.log.read_text()


def next_line(sock, reader):
    assert select.select([sock], [], [], TIMEOUT)[0], "Timed out waiting for protocol line"
    line = reader.readline()
    assert line, "Unexpected EOF"
    return line.decode().rstrip("\n")


def check_reconnect():
    with run("demo", "--delay", "70") as case:
        observer = case.observer("observer")
        replaced = case.observer("old_observer")
        replaced.child.send_signal(signal.SIGTERM)
        replaced.finish()

        # A partial registration must remain buffered until the newline arrives.
        raw = case.socket()
        raw.sendall(b"HELLO RO")
        assert not select.select([raw], [], [], 0.05)[0]
        raw.sendall(b"BOT 3\n")
        with raw.makefile("rb", buffering=0) as reader:
            assert next_line(raw, reader) == "WELCOME ROBOT"
            duplicate = case.robot(3, "duplicate")
            assert "ERROR robot already connected" in duplicate.finish(1)
            robots = [case.robot(1), case.robot(2)]
            while True:
                line = next_line(raw, reader)
                if line.startswith("STEP "):
                    tick = int(line.split()[1])
                    # Stale and duplicated commands cannot apply another action.
                    raw.sendall(f"ACT {tick + 50}\nACT {tick}\nACT {tick}\n".encode())
                elif "task=103 carrying=1" in line:
                    break
        raw.close()
        read_until(observer.path, r"^OFFLINE robot=3 ")
        replacement = case.observer("new_observer")
        snapshot = read_until(
            replacement.path, r"^ROBOT tick=(\d+) id=3 .*task=103 carrying=1 online=0$"
        )
        next_tick = int(snapshot.group(1)) + 1
        read_until(
            replacement.path,
            rf"^ROBOT tick={next_tick} id=3 .*task=103 carrying=1 online=0$",
        )
        resumed = case.robot(3, "resumed")
        read_until(resumed.path, r"^ROBOT .*id=3 .*task=103 carrying=1 online=1$")
        assert "completed=3 unreachable=0" in case.server.finish()
        for proc in [*robots, observer, replacement, resumed]:
            assert "STOP OK reason=all tasks resolved" in proc.finish()
        assert case.log.read_text().count("task 103 delivered by robot 3") == 1


def check_interrupt():
    with run("demo", "--delay", "500", "--response-timeout", "2500") as case:
        observer = case.observer("observer")
        other_observer = case.observer("other_observer")
        raw = case.socket()
        raw.sendall(b"HELLO ROBOT 3\n")
        robots = [case.robot(1), case.robot(2)]
        with raw.makefile("rb", buffering=0) as reader:
            while not next_line(raw, reader).startswith("STEP "):
                pass
            case.server.child.send_signal(signal.SIGINT)
            # This robot was still holding the tick open when shutdown began.
            assert next_line(raw, reader) == "STOP OK reason=interrupted"
        raw.close()
        assert "SUMMARY reason=interrupted" in case.server.finish()
        for proc in [*robots, observer, other_observer]:
            assert "STOP OK reason=interrupted" in proc.finish()


def check_response_timeout():
    with run("energy", "--response-timeout", "150") as case:
        observer = case.observer("observer")
        raw = case.socket()
        raw.sendall(b"HELLO ROBOT 1\n")
        read_until(observer.path, r"^OFFLINE robot=1 reason=step timeout$")
        raw.close()
        robot = case.robot(1)
        assert "completed=1 unreachable=0" in case.server.finish()
        assert "STOP OK reason=all tasks resolved" in robot.finish()
        assert "STOP OK reason=all tasks resolved" in observer.finish()


def check_base_and_missing_robots():
    with run("demo", "--no-observers", "--delay", "0") as case:
        observer = case.client("disabled", "observer")
        assert "ERROR observers disabled" in observer.finish(1)
        robots = [case.robot(robot_id) for robot_id in (1, 2, 3)]
        server = case.server.finish()
        assert "completed=3" in server and "MAP" not in server
        assert "moved" not in case.log.read_text()
        for robot in robots:
            assert "STOP OK reason=all tasks resolved" in robot.finish()

    with run("demo", "--connect-timeout", "400") as case:
        observer = case.observer("observer")
        assert "SUMMARY reason=no robots connected" in case.server.finish(1)
        assert "STOP ERROR reason=no robots connected" in observer.finish(1)


def main():
    with socket.socket() as probe:
        try:
            probe.bind(("127.0.0.1", 0))
        except PermissionError as error:
            print(f"TCP verification unavailable: {error}", file=sys.stderr)
            return 77
    check_base_and_missing_robots()
    check_scenario("demo", (1, 2, 3), "all tasks resolved", "completed=3 unreachable=0")
    conflict = check_scenario("conflict", (1, 2), "all tasks resolved", "completed=2 unreachable=0")
    assert "reserved narrow cell" in conflict and "waits at" in conflict
    energy = check_scenario("energy", (1,), "all tasks resolved", "completed=1 unreachable=0")
    assert "charged " in energy
    check_scenario("impossible", (1,), "all tasks resolved", "completed=0 unreachable=1")
    check_scenario("demo", (1, 2, 3), "tick limit", "ticks=2", "--max-ticks", "2")
    check_reconnect()
    check_response_timeout()
    check_interrupt()
    print("TCP scenarios, reconnection and shutdown passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
