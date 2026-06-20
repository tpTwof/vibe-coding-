# 星闪 SLE 键盘 — 双板无线宏键盘

## 项目简介

用两块**九联星闪开发板（海思 WS63E）**，通过**星闪 SLE** 无线协议，实现三按键无线宏键盘：

```
┌──────────────────┐       SLE 无线        ┌──────────────────┐
│   板 A (Client)   │ ◄──────────────────► │   板 B (Server)   │
│   烧录 work2      │                       │   烧录 work1      │
│                   │   扫描→连接→发送按键   │                   │
│  GPIO10/11/12 ───►│   发送 1/2/3D/3U      │  收到→串口→电脑   │
└──────────────────┘                       └────────┬─────────┘
                                                    │ USB 串口
                                                    ▼
                                             ┌──────────────┐
                                             │   电脑        │
                                             │ convert.py    │
                                             │ 读取串口      │
                                             │ pyautogui     │
                                             │ 模拟键盘操作  │
                                             └──────────────┘
```

## 硬件接线

### Server 板（work1）

| 引脚 | 连接 |
|------|------|
| USB | 电脑 USB（串口 + 供电） |
| 无需额外接线 | — |

### Client 板（work2）

| GPIO | 连接 | 按键功能 |
|------|------|---------|
| GPIO10 | → 按键 → 3.3V | Enter |
| GPIO11 | → 按键 → 3.3V | Backspace |
| GPIO12 | → 按键 → 3.3V | 语音键（Ctrl+Win） |

按键按下时 GPIO 读到高电平，松开时内部下拉拉低。

---

## 按键功能

| GPIO | 按下发送 | 松开发送 | PC 动作 |
|------|---------|---------|---------|
| 10 | `1` | — | Enter |
| 11 | `2` | — | Backspace |
| 12 | `3D` | `3U` | Ctrl+Win 按下 / 松开 |

---

## 串口协议

Server 收到 SLE 数据后，把数据原样转成字符通过串口输出，每行一个事件：

| 串口输出 | Python 动作 |
|---------|------------|
| `1` | `pyautogui.press("enter")` |
| `2` | `pyautogui.press("backspace")` |
| `3D` | `keyDown("ctrl")` + `keyDown("win")` |
| `3U` | `keyUp("win")` + `keyUp("ctrl")` |

---

## 构建与烧录

```bash
cd /root/ws63_ohos

# 编译 Server
python build.py -p nearlink_dk_3863@hihope -T "applications/sample/wifi-iot/app/work1:work1_demo"

# 编译 Client
python build.py -p nearlink_dk_3863@hihope -T "applications/sample/wifi-iot/app/work2:work2_demo"
```

两块板分别烧录对应固件。

---

## convert.py — PC 端串口宏键盘

### 安装依赖

```bash
pip install pyautogui pyserial
```

### 配置

编辑 `convert.py` 顶部：

```python
SERIAL_PORT = "COM15"   # Server 板串口号
BAUD_RATE = 115200
```

### 运行

```bash
python convert.py
```

按 `Ctrl+C` 退出，脚本会自动释放按键防止卡键。

---

## 目录结构

```
app/
├── README.md
├── .gitignore
├── convert.py              # PC 端串口→键盘脚本
├── BUILD.gn
├── bundle.json
├── startup/
├── work1/                  # SLE Server（接收板）
│   ├── BUILD.gn
│   ├── sle_uuid_server.h
│   ├── sle_uuid_server.c
│   ├── sle_server_adv.h
│   └── sle_server_adv.c
└── work2/                  # SLE Client（按键板）
    ├── BUILD.gn
    ├── sle_uuid_client.h
    └── sle_uuid_client.c
```

## 依赖

- **芯片**：海思 WS63E（九联星闪开发板）
- **系统**：OpenHarmony LiteOS-M（RISC-V 32 位）
- **编译器**：`riscv32-linux-musl-gcc`（或 `gcc-riscv64-linux-gnu` + 符号链接）
- **PC 端**：Python 3 + pyautogui + pyserial
