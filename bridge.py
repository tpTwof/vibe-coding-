# -*- coding: utf-8 -*-
"""
PC 端统一桥接脚本：

1. 读取开发板串口事件，根据 keymap.json 执行键盘动作
2. 启动本地 HTTP 服务，接收 Claude Code hooks
3. 把 Claude Code 三态通过平台 AT 命令发给开发板，例如 AT+LED=RUNNING

运行：
  python bridge.py

Claude Code hook URL：
  http://127.0.0.1:8765/claude/event
"""

import sys
import os
import json
import signal
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import msvcrt
except ImportError:
    msvcrt = None

import serial
from pynput.keyboard import Key, Controller


# ===== 串口配置 =====
SERIAL_PORT = "COM5"
SERIAL_BAUD = 115200

# ===== Claude Code Hook HTTP 服务配置 =====
HTTP_HOST = "127.0.0.1"
HTTP_PORT = 8765
CLAUDE_EVENT_PATH = "/claude/event"

# ===== 映射文件路径（与本脚本同目录） =====
KEYMAP_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "keymap.json")


keyboard = Controller()
held_keys = set()

serial_conn = None
serial_write_lock = threading.Lock()

current_claude_state = "DONE"
last_claude_event = "-"


KEY_MAP = {
    "enter": Key.enter,
    "backspace": Key.backspace,
    "esc": Key.esc,
    "space": Key.space,
    "tab": Key.tab,
    "ctrl": Key.ctrl,
    "ctrlleft": Key.ctrl_l,
    "ctrlright": Key.ctrl_r,
    "alt": Key.alt,
    "altleft": Key.alt_l,
    "altright": Key.alt_r,
    "shift": Key.shift,
    "shiftleft": Key.shift_l,
    "shiftright": Key.shift_r,
    "win": Key.cmd,
    "winleft": Key.cmd_l,
    "winright": Key.cmd_r,
    "f1": Key.f1,
    "f2": Key.f2,
    "f3": Key.f3,
    "f4": Key.f4,
    "f5": Key.f5,
    "f6": Key.f6,
    "f7": Key.f7,
    "f8": Key.f8,
    "f9": Key.f9,
    "f10": Key.f10,
    "f11": Key.f11,
    "f12": Key.f12,
    "delete": Key.delete,
    "home": Key.home,
    "end": Key.end,
    "pageup": Key.page_up,
    "pagedown": Key.page_down,
    "insert": Key.insert,
    "up": Key.up,
    "down": Key.down,
    "left": Key.left,
    "right": Key.right,
}


def parse_key(name):
    name = name.strip().lower()
    if name in KEY_MAP:
        return KEY_MAP[name]
    if len(name) == 1:
        return name
    return None


# ===== keymap 加载 =====
keymap = {}
keymap_mtime = 0


def load_keymap():
    global keymap, keymap_mtime
    try:
        mtime = os.path.getmtime(KEYMAP_FILE)
        if mtime == keymap_mtime:
            return

        with open(KEYMAP_FILE, "r", encoding="utf-8") as f:
            keymap = json.load(f)

        keymap_mtime = mtime
        print(f"[keymap] loaded: {list(keymap.keys())}")

    except FileNotFoundError:
        print(f"[keymap] file not found: {KEYMAP_FILE}")

    except json.JSONDecodeError as e:
        print(f"[keymap] parse error: {e}")


def check_keymap_reload():
    try:
        mtime = os.path.getmtime(KEYMAP_FILE)
        if mtime != keymap_mtime:
            load_keymap()
    except OSError:
        pass


# ===== 键盘动作执行 =====
def do_press(keys):
    for k_str in keys:
        k = parse_key(k_str)
        if k is None:
            print(f"[keyboard] unknown key: {k_str}")
            continue

        keyboard.press(k)
        keyboard.release(k)


def do_hotkey(keys):
    parsed = [parse_key(k) for k in keys]
    parsed = [k for k in parsed if k is not None]

    if not parsed:
        return

    for k in parsed:
        keyboard.press(k)
        time.sleep(0.02)

    for k in reversed(parsed):
        keyboard.release(k)


def do_hold_down(keys):
    for k_str in keys:
        k = parse_key(k_str)
        if k is None:
            print(f"[keyboard] unknown key: {k_str}")
            continue

        keyboard.press(k)
        held_keys.add(k)
        time.sleep(0.02)


def do_hold_up(keys):
    parsed = [parse_key(k) for k in keys]
    parsed = [k for k in parsed if k is not None]

    for k in reversed(parsed):
        keyboard.release(k)
        held_keys.discard(k)


def release_all_held():
    for k in list(held_keys):
        try:
            keyboard.release(k)
        except Exception:
            pass

    held_keys.clear()


def process_keymap_event(event):
    """
    处理旧模式：
      串口收到 1 / 2 / 3D / 3U
      再从 keymap.json 查对应动作。
    """
    event = event.strip()

    if event not in keymap:
        return False

    binding = keymap[event]
    action_type = binding.get("type", "").upper()
    keys = binding.get("keys", [])

    print(f"[keymap] {event} -> {action_type} {keys}")

    if action_type == "PRESS":
        do_press(keys)
    elif action_type == "HOTKEY":
        do_hotkey(keys)
    elif action_type == "HOLD_DOWN":
        do_hold_down(keys)
    elif action_type == "HOLD_UP":
        do_hold_up(keys)
    else:
        print(f"[keymap] unknown action type: {action_type}")

    return True


def parse_cmd_keys(raw):
    return [key.strip() for key in raw.split(",") if key.strip()]


def process_action_command(line):
    """
    处理新模式：
      Server 直接输出 PRESS / HOTKEY / HOLD_DOWN / HOLD_UP 动作命令。
    """
    if line.startswith("PRESS:"):
        key = line.removeprefix("PRESS:").strip()
        print(f"[cmd] PRESS {key}")
        do_press([key])
        return True

    if line.startswith("HOTKEY:"):
        keys = parse_cmd_keys(line.removeprefix("HOTKEY:"))
        print(f"[cmd] HOTKEY {keys}")
        do_hotkey(keys)
        return True

    if line.startswith("HOLD_DOWN:"):
        keys = parse_cmd_keys(line.removeprefix("HOLD_DOWN:"))
        print(f"[cmd] HOLD_DOWN {keys}")
        do_hold_down(keys)
        return True

    if line.startswith("HOLD_UP:"):
        keys = parse_cmd_keys(line.removeprefix("HOLD_UP:"))
        print(f"[cmd] HOLD_UP {keys}")
        do_hold_up(keys)
        return True

    return False


def process_serial_line(line):
    """
    串口输入统一入口。

    兼容两种模式：
    1. 旧模式：1 / 2 / 3D / 3U + keymap.json
    2. 新模式：PRESS / HOTKEY / HOLD_DOWN / HOLD_UP
    """
    line = line.strip()

    if not line:
        return

    print(f"[serial rx] {line}")

    try:
        if process_action_command(line):
            return

        if process_keymap_event(line):
            return

        print(f"[serial] ignored: {line}")

    except Exception as e:
        print(f"[serial] process error: {e}")


# ===== Claude Code 状态处理 =====
def claude_event_to_state(data):
    """
    Claude Code hook 事件 -> 三态 LED 状态。

    RUNNING:
      Claude 正在处理用户请求或调用工具。

    DONE:
      当前轮完成，回到空闲/等待下一次输入。

    ATTENTION:
      需要用户确认、等待用户操作、工具失败或会话异常。
    """
    event = data.get("hook_event_name", "") or data.get("event", "")
    lower_text = json.dumps(data, ensure_ascii=False).lower()

    if event == "PermissionRequest":
        return "ATTENTION"

    if event in ("UserPromptSubmit", "PreToolUse", "PostToolUse"):
        return "RUNNING"

    if event in ("StopFailure", "PostToolUseFailure"):
        return "ATTENTION"

    if event == "Stop":
        return "DONE"

    if event == "SessionEnd":
        return "DONE"

    if event == "Notification":
        if "idle" in lower_text:
            return "DONE"

        return "ATTENTION"

    return "UNKNOWN"


def send_led_state(state):
    """
    把 Claude Code 三态发给开发板。

    三态定义：
      AT+LED=RUNNING    黄灯：Claude 正在处理
      AT+LED=DONE       绿灯：Claude 完成/空闲
      AT+LED=ATTENTION  红灯：需要确认、等待输入或出错
    """
    global current_claude_state

    current_claude_state = state
    cmd = f"AT+LED={state}\r\n"

    if serial_conn is None or not serial_conn.is_open:
        print(f"[claude] serial not open, cannot send: {cmd.strip()}")
        return

    with serial_write_lock:
        serial_conn.write(cmd.encode("utf-8"))
        serial_conn.flush()

    print(f"[claude] LED state -> {state}")


class ClaudeHookHandler(BaseHTTPRequestHandler):
    def _send_json(self, status_code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            self._send_json(200, {
                "ok": True,
                "state": current_claude_state,
                "lastEvent": last_claude_event,
            })
            return

        self._send_json(404, {
            "ok": False,
            "error": "not found",
        })

    def do_POST(self):
        global last_claude_event

        if self.path != CLAUDE_EVENT_PATH:
            self._send_json(404, {
                "ok": False,
                "error": "not found",
            })
            return

        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length)

        try:
            data = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception as e:
            self._send_json(400, {
                "ok": False,
                "error": f"invalid json: {e}",
            })
            return

        event = data.get("hook_event_name", "") or data.get("event", "")
        last_claude_event = event or "-"

        print(f"[claude hook] event={last_claude_event}")
        print(json.dumps(data, ensure_ascii=False, indent=2))

        state = claude_event_to_state(data)

        if state != "UNKNOWN":
            send_led_state(state)

        self._send_json(200, {
            "ok": True,
            "event": last_claude_event,
            "state": state,
        })

    def log_message(self, fmt, *args):
        # 减少 HTTP server 默认日志噪声
        return


def start_http_server():
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), ClaudeHookHandler)
    print(f"[http] Claude hook server: http://{HTTP_HOST}:{HTTP_PORT}{CLAUDE_EVENT_PATH}")
    print(f"[http] status endpoint: http://{HTTP_HOST}:{HTTP_PORT}/status")
    server.serve_forever()


# ===== 主循环 =====
def cleanup_and_exit(*_):
    release_all_held()

    try:
        if serial_conn is not None and serial_conn.is_open:
            serial_conn.close()
    except Exception:
        pass

    print("[exit] released held keys and closed serial")
    sys.exit(0)


def main():
    global serial_conn

    signal.signal(signal.SIGINT, cleanup_and_exit)
    signal.signal(signal.SIGTERM, cleanup_and_exit)

    load_keymap()

    print(f"[serial] opening {SERIAL_PORT}, baud={SERIAL_BAUD}")
    serial_conn = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=1)

    http_thread = threading.Thread(target=start_http_server, daemon=True)
    http_thread.start()

    buffer = ""

    try:
        while True:
            check_keymap_reload()

            if msvcrt is not None and msvcrt.kbhit():
                ch = msvcrt.getwch()
                if ch == "\x03":
                    break

            data = serial_conn.read(64)

            if data:
                text = data.decode("utf-8", errors="replace")
                buffer += text

                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    process_serial_line(line)

    except KeyboardInterrupt:
        pass

    finally:
        cleanup_and_exit()


if __name__ == "__main__":
    main()
