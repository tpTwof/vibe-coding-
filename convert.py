import pyautogui
import time
import serial
import sys

#
SERIAL_PORT = "COM15"
BAUD_RATE = 115200

pyautogui.FAILSAFE = True

voice_key_down = False


#调试功能
def countdown():
    print("3")
    time.sleep(1)
    print("2")
    time.sleep(1)
    print("1")
    time.sleep(1)

def release_voice_keys():
    """
    确保 Ctrl + Win 被松开，防止程序退出后按键卡住。
    """
    global voice_key_down

    try:
        pyautogui.keyUp("winleft")
        pyautogui.keyUp("ctrlleft")
    except Exception:
        pass

    voice_key_down = False

def id2real(event: str):

    global voice_key_down

    event = event.strip()

    if event == "1":
        print("执行 Enter")
        pyautogui.press("enter")

    elif event == "2":
        print("执行 Backspace")
        pyautogui.press("backspace")

    elif event == "3D":
        if not voice_key_down:
            print("Ctrl + Win 按下，开始语音")
            pyautogui.keyDown("ctrlleft")
            pyautogui.keyDown("winleft")
            voice_key_down = True
        else:
            print(f'已经收到3D')

    elif event == "3U":
        if voice_key_down:
            print("Ctrl + Win 松开，结束语音")
            pyautogui.keyUp("winleft")
            pyautogui.keyUp("ctrlleft")
            voice_key_down = False
        else:
            print(f'按键并未按下')

    else:
        print("未知事件")


print("输入 1  = Enter")
print("输入 2  = Backspace")
print("输入 3D = Ctrl+Win 按下")
print("输入 3U = Ctrl+Win 松开")
print("输入 q  = 退出")


def main():
    print("步骤 3：串口宏键盘脚本启动")
    print(f"正在打开串口：{SERIAL_PORT}, 波特率：{BAUD_RATE}")
    print("请确保串口助手已经关闭，否则 COM7 会被占用。")
    print("按 Ctrl + C 可以退出脚本。")
    print("-" * 40)

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"串口打开失败：{e}")
        print("请检查：")
        print("1. 开发板是否插上电脑")
        print("2. COM 口是否真的是 COM7")
        print("3. 串口助手是否已经关闭")
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

            # 只处理 [event] 开头的行
            if text.startswith("[event] "):
                id2real(text[8:])
            else:
                print(f"忽略：{text}")

            time.sleep(0.01)

    except KeyboardInterrupt:
        print("\n检测到 Ctrl + C，准备退出...")

    finally:
        release_voice_keys()
        ser.close()
        print("已释放 Ctrl + Win，并关闭串口。")


if __name__ == "__main__":
    main()