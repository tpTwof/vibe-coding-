"""
串口宏键盘脚本。
监听 Server 串口，收到事件 ID 后调用 pyautogui 模拟键盘操作。

事件对应：
  1  -> Enter
  2  -> Backspace
  3U -> Ctrl+Win 切换（按下/松开）
  3D -> Ctrl+Win 按下（长按兼容）

用法：
  python convert.py
  按 Ctrl+C 退出。
"""
import pyautogui
import serial
import sys
import time

# ====== 配置 ======
SERIAL_PORT = "COM15"   # Server 板的串口号
BAUD_RATE = 115200

pyautogui.FAILSAFE = True
voice_key_down = False


def release_voice_keys():
    """确保退出时释放 Ctrl+Win，防止卡键。"""
    global voice_key_down
    try:
        pyautogui.keyUp("winleft")
        pyautogui.keyUp("ctrlleft")
    except Exception:
        pass
    voice_key_down = False


def id2real(event: str):
    """把事件 ID 转成键盘操作。"""
    global voice_key_down

    if event == "1":
        # Enter
        print("Enter")
        pyautogui.press("enter")

    elif event == "2":
        # Backspace
        print("Backspace")
        pyautogui.press("backspace")

    elif event == "3U":
        # Ctrl+Win 松开
        if voice_key_down:
            print("Ctrl+Win UP")
            pyautogui.keyUp("winleft")
            pyautogui.keyUp("ctrlleft")
            voice_key_down = False

    elif event == "3D":
        # Ctrl+Win 按下（长按兼容）
        if not voice_key_down:
            print("Ctrl+Win DOWN")
            pyautogui.keyDown("ctrlleft")
            pyautogui.keyDown("winleft")
            voice_key_down = True

    else:
        print(f"未知事件: {event}")


# ====== 主循环 ======
print("1=Enter  2=Backspace  3U=Voice toggle  3D=Voice down")
print("-" * 40)


def main():
    print(f"打开串口 {SERIAL_PORT}，波特率 {BAUD_RATE} ...")
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"串口打开失败: {e}")
        sys.exit(1)

    try:
        while True:
            line = ser.readline()
            if not line:
                continue

            try:
                text = line.decode("utf-8", errors="ignore").strip()
            except Exception:
                continue

            # 匹配事件 ID（整行完全等于 1/2/3D/3U）
            if text in ("1", "2", "3D", "3U"):
                id2real(text)

            time.sleep(0.01)

    except KeyboardInterrupt:
        print("\n退出中...")

    finally:
        release_voice_keys()
        ser.close()
        print("已释放按键，串口已关闭。")


if __name__ == "__main__":
    main()