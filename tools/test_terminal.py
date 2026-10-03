#!/usr/bin/env python3
"""Exercise the shipping binary over a real PTY and decode its Kitty frames.

This small terminal peer answers the graphics probe, sends real key/mouse
reports, and tests startup, shop interaction, resize, pause, and clean exit.
It needs no display server or terminal emulator and writes reviewable PPMs.
"""
import base64
import errno
import fcntl
import os
from pathlib import Path
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
APC = re.compile(rb"\x1b_G([^;\x1b]*)(?:;([^\x1b]*))?\x1b\\")


class Terminal:
    def __init__(self, profile: str):
        self.master, self.slave = os.openpty()
        self.resize(960, 540, notify=False)
        self.initial_termios = termios.tcgetattr(self.slave)
        env = dict(os.environ, KITTYFB_TRANSPORT="inline", XDG_DATA_HOME=profile)
        env.pop("KITTYFB_SKIP_PROBE", None)
        self.process = subprocess.Popen(
            [str(ROOT / "pleb-tower"), "--mute"], cwd="/tmp", env=env,
            stdin=self.slave, stdout=self.slave, stderr=subprocess.PIPE)
        self.buffer = b""
        self.controls = b""
        self.frame = None
        self.frame_count = 0
        self.image_header = {}
        self.image_data = b""
        self.probed = False

    def resize(self, width: int, height: int, notify=True):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 30, 80, width, height))
        self.width, self.height = width, height
        if notify:
            os.kill(self.process.pid, signal.SIGWINCH)
            self.settle()

    def pump(self, seconds=0.2):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], min(0.05, max(0, deadline-time.monotonic())))
            if not ready:
                continue
            try:
                data = os.read(self.master, 262144)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            self.controls = (self.controls + data)[-4096:]
            self.buffer += data
            while True:
                match = APC.search(self.buffer)
                if not match:
                    # Keep an incomplete APC, dropping unrelated terminal text.
                    start = self.buffer.find(b"\x1b_G")
                    self.buffer = self.buffer[start:] if start >= 0 else self.buffer[-8:]
                    break
                self.buffer = self.buffer[match.end():]
                header = dict(part.split(b"=", 1) for part in match[1].split(b",") if b"=" in part)
                payload = match[2] or b""
                if header.get(b"a") == b"q":
                    os.write(self.master, b"\x1b_Gi=31;OK\x1b\\\x1b[?1;2c")
                    self.probed = True
                    continue
                if b"s" in header and b"v" in header:
                    self.image_header = header
                    self.image_data = b""
                if payload and self.image_header:
                    self.image_data += payload
                    if header.get(b"m", b"0") == b"0":
                        pixels = base64.b64decode(self.image_data)
                        if self.image_header.get(b"o") == b"z":
                            pixels = zlib.decompress(pixels)
                        width = int(self.image_header[b"s"])
                        height = int(self.image_header[b"v"])
                        assert len(pixels) == width * height * 3, "unexpected Kitty frame format"
                        self.frame = (width, height, pixels)
                        self.frame_count += 1
                        self.image_header = {}

    def settle(self):
        # Presentation can be slower on a loaded runner. Drain the frame
        # already in flight and observe subsequent frames instead of assuming
        # the input was handled within a fixed 200 ms sleep.
        target = self.frame_count + 3
        deadline = time.monotonic() + 5.0
        while self.frame_count < target and time.monotonic() < deadline:
            self.pump(0.1)
            if self.process.poll() is not None:
                return
        assert self.frame_count >= target, "terminal stopped producing frames"

    def key(self, code: int, changed=True):
        previous = self.frame[2] if self.frame else None
        os.write(self.master, f"\x1b[{code};1:1u\x1b[{code};1:3u".encode())
        self.settle()
        if changed and code != ord("q"):
            self.wait_for(lambda: self.frame[2] != previous,
                          f"key {code} did not update the screen")

    def wait_for(self, predicate, message):
        deadline = time.monotonic() + 5.0
        while not predicate() and time.monotonic() < deadline:
            self.pump(0.1)
            assert self.process.poll() is None, "game exited before the expected frame"
        assert predicate(), message

    def click(self, x: int, y: int, changed=False):
        previous = self.frame[2]
        os.write(self.master, f"\x1b[<0;{x+1};{y+1}M\x1b[<0;{x+1};{y+1}m".encode())
        self.settle()
        if changed:
            self.wait_for(lambda: self.frame[2] != previous,
                          "click did not update the screen")

    def logical_point(self, x: int, y: int):
        width, height, _ = self.frame
        scale = min(width / 480, height / 342)
        view_width, view_height = int(480 * scale), int(342 * scale)
        return ((width-view_width)//2 + int(x*view_width/480),
                (height-view_height)//2 + int(y*view_height/342))

    def logical_click(self, x: int, y: int, changed=False):
        self.click(*self.logical_point(x, y), changed=changed)

    def logical_move(self, x: int, y: int, changed=False):
        previous = self.frame[2]
        px, py = self.logical_point(x, y)
        os.write(self.master, f"\x1b[<35;{px+1};{py+1}M".encode())
        self.settle()
        if changed:
            self.wait_for(lambda: self.frame[2] != previous,
                          "pointer motion did not update selection")

    def save(self, name: str):
        assert self.frame is not None, f"no frame for {name}"
        width, height, pixels = self.frame
        (ROOT / "build" / f"terminal-{name}.ppm").write_bytes(
            f"P6\n{width} {height}\n255\n".encode() + pixels)
        return pixels

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.pump(0.3)
            self.process.wait(timeout=3)
        if self.process.stderr:
            self.process.stderr.close()
        os.close(self.master)
        os.close(self.slave)


def main():
    with tempfile.TemporaryDirectory(prefix="pleb-terminal-") as profile:
        terminal = Terminal(profile)
        try:
            terminal.pump(0.6)
            assert terminal.probed, "game did not negotiate graphics support"
            title = terminal.save("title")
            terminal.key(ord("q"))
            assert terminal.process.poll() is None and terminal.frame[2] == title, "Q quit from title"
            terminal.key(27)
            terminal.save("title-menu")
            terminal.key(27)
            assert terminal.frame[2] == title, "Escape menu did not return to title"
            terminal.key(ord("h"))
            assert terminal.save("help") != title, "help did not open"
            terminal.key(27)
            terminal.key(13)
            selection = terminal.save("campaign")
            assert selection != title, "campaign screen did not open"
            # A click on the locked campaign must keep the selection screen.
            terminal.logical_click(20, 98)
            assert terminal.save("locked") == selection, "locked campaign was launched"
            terminal.logical_click(20, 82, changed=True)
            map_selection = terminal.save("map-selection")
            assert map_selection != selection, "campaign click did not open map selection"
            terminal.logical_click(300, 44, changed=True)
            rail_selection = terminal.save("rail-selection")
            assert rail_selection != map_selection, "Rail Yard tab did not select map"
            terminal.logical_click(20, 44, changed=True)
            terminal.logical_click(100, 315, changed=True)
            ready = terminal.save("ready")
            assert ready != selection, "mouse did not deploy HOLDOUT"
            terminal.logical_click(4*16+8, 10*16+8, changed=True)
            shop = terminal.save("shop")
            assert shop != ready, "mouse did not select a build pad"
            terminal.logical_click(20, 230)
            assert terminal.frame[2] == shop, "blank shop space purchased a tower"
            terminal.logical_move(20, 78, changed=True)
            preview = terminal.save("shop-preview")
            assert preview != shop, "hover did not preview another weapon"
            terminal.logical_move(20, 230)
            assert terminal.frame[2] == preview, "hovering off the rows changed the selection"
            terminal.logical_move(20, 60, changed=True)
            assert terminal.frame[2] == shop, "hover bought a weapon or moved the board cursor"
            terminal.logical_click(20, 60, changed=True)
            inspector = terminal.save("tower")
            assert inspector != shop, "shop click did not purchase a tower"
            terminal.logical_click(20, 230)
            assert terminal.frame[2] == inspector, "blank inspector space activated a command"
            terminal.logical_click(20, 26)
            assert terminal.frame[2] == inspector, "click outside inspector activated a command"
            terminal.key(27)
            terminal.resize(1200, 720)
            terminal.pump(0.3)
            terminal.save("resized")
            assert terminal.frame[0] == 1200 and terminal.frame[1] > 540, "resize did not reach the presenter"
            terminal.logical_click(6*16+8, 10*16+8, changed=True)
            resized_shop = terminal.save("resized-shop")
            terminal.logical_click(20, 60, changed=True)
            resized_tower = terminal.save("resized-tower")
            assert resized_tower != resized_shop, "resized pointer missed the shop"
            terminal.logical_click(20, 230)
            assert terminal.frame[2] == resized_tower, "blank inspector space sold the second tower"
            terminal.key(13, changed=False)
            assert terminal.frame[2] == resized_tower, "confirming an unaffordable upgrade sold the tower"
            terminal.key(27)
            terminal.key(ord("z"))
            terminal.logical_move(400, 120, changed=True)
            zoomed = terminal.save("zoomed")
            terminal.logical_move(400, 120)
            assert terminal.frame[2] == zoomed, "same zoomed pointer position shifted the view"
            terminal.key(ord("z"))
            terminal.key(ord("h"))
            help_screen = terminal.save("resized-help")
            terminal.key(27)
            terminal.logical_click(400, 262)
            terminal.wait_for(lambda: terminal.frame[2] == help_screen,
                              "HUD Help click did not open help")
            assert terminal.save("hud-help") == help_screen, "HUD Help click did not open help"
            terminal.key(27)
            still_ready = terminal.save("still-ready")
            terminal.pump(0.3)
            assert terminal.frame[2] == still_ready, "HUD Help click started the wave"
            terminal.logical_click(100, 282, changed=True)
            intel = terminal.save("field-guide")
            terminal.pump(0.3)
            assert terminal.frame[2] == intel, "field guide did not pause the game"
            terminal.logical_click(230, 308, changed=True)
            assert terminal.save("field-guide-next") != intel, "field guide Next button failed"
            terminal.logical_click(80, 308, changed=True)
            assert terminal.frame[2] == intel, "field guide Previous button failed"
            terminal.logical_click(380, 308, changed=True)
            assert terminal.frame[2] == still_ready, "field guide did not restore the board"
            terminal.logical_click(300, 245, changed=True)
            terminal.pump(1.0)
            terminal.key(ord("f"))
            terminal.save("combat-2x")
            terminal.key(ord("i"))
            combat_intel = terminal.save("combat-guide")
            terminal.pump(0.3)
            assert terminal.frame[2] == combat_intel, "field guide did not freeze 2x combat"
            terminal.key(27)
            terminal.logical_click(440, 245, changed=True)
            combat = terminal.save("combat")
            terminal.key(27)
            pause = terminal.save("pause")
            assert pause != combat, "pause did not open"
            terminal.pump(0.2)
            assert terminal.frame[2] == pause, "paused game kept advancing"
            terminal.key(ord("q"))
            assert terminal.process.poll() is None and terminal.frame[2] == pause, "Q quit from pause menu"
            terminal.logical_click(20, 158)
            assert terminal.process.poll() is None and terminal.frame[2] == pause, "blank menu space exited"
            terminal.logical_click(20, 120, changed=True)  # Choose campaign
            terminal.key(13)
            terminal.key(57351)  # Right: choose Rail Yard
            terminal.logical_click(100, 315, changed=True)
            rail_ready = terminal.save("rail-yard-ready")
            assert rail_ready != ready, "second map reused Maple Loop scenery"
            terminal.logical_click(3*16+8, 10*16+8, changed=True)
            terminal.logical_click(20, 60, changed=True)
            terminal.key(27)  # close inspector
            terminal.key(27)  # pause
            terminal.logical_click(20, 102, changed=True)  # Restart
            assert terminal.save("rail-yard-restart") == rail_ready, "restart lost selected map or economy"
            terminal.key(27)
            terminal.logical_click(20, 138)
            terminal.pump(0.3)
            assert terminal.process.wait(timeout=3) == 0, "game did not exit successfully"
            assert termios.tcgetattr(terminal.slave) == terminal.initial_termios, "game did not restore terminal state"
            print(f"PASS terminal: probe, menus, lock, both maps, restart, hover previews, purchases, safe confirmation, zoom, HUD help, field guide, 2x combat, resize, Escape menu, Q ignored, menu exit ({terminal.frame_count} decoded frames)")
        finally:
            terminal.close()


if __name__ == "__main__":
    main()
