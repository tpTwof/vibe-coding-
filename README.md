# 星闪 SLE 键盘 — 双板无线宏键盘

## 项目简介

用两块**九联星闪开发板（海思 WS63E）**，通过**星闪 SLE** 无线协议，实现一个物理键盘宏：

```
┌──────────────────┐       SLE 无线        ┌──────────────────┐
│   板 A (Client)   │ ◄──────────────────► │   板 B (Server)   │
│   烧录 work2      │                       │   烧录 work1      │
│                   │   扫描→连接→发送按键   │                   │
│  GPIO12 按键 ────►│   发送 "1" "3D" 等    │  收到→串口→电脑   │
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

| 引脚 | 连接 |
|------|------|
| USB | 供电即可 |
| GPIO12 | 按键（一端接 GPIO12，一端接 GND）|

按键为**低电平有效**，实际检测逻辑为上升沿（松开时触发）。

---

## 按键功能

| 按键 | SLE 发送 | 串口输出 | PC 动作 |
|------|---------|---------|---------|
| GPIO12 按下松开 | `1` | `1` | `Enter` |
| GPIO12 长按 | `3D` | `3D` | `Ctrl+Win` 按下（语音输入） |
| GPIO12 长按松开 | `3U` | `3U` | `Ctrl+Win` 松开 |

---

## 软件架构

```
work2 (Client)                    work1 (Server)                PC
──────────────                    ──────────────                ──
上电                             上电
  │                                │
  ├─ key_enter_task_start()        ├─ sle_uuid_server_init()
  │   (GPIO12 轮询线程)             │    (注册 UUID 服务)
  │                                │
  ├─ enable_sle()                  ├─ 开始广播
  │   → sle_start_scan()           │   "sle_uuid_server"
  │                                │
  ├─ 扫到设备 ──→ 连接 ──→ 配对     │
  │                                │
  ├─ 发现服务结构                    │
  │                                │
  ├─ g_sle_ready = 1               │
  │   "[ssap client] sle ready"    │
  │                                │
  ├─ GPIO12 按下                    │
  │   sle_client_send_key1()       │
  │   → ssapc_write_req("1") ──────┤→ 收到 "1"
  │                                │   → printf("1\r\n")
  │                                │       │
  │                                │       └──→ USB 串口
  │                                │            │
  │                                │       convert.py 读取
  │                                │       → pyautogui.press("enter")
```

---

## 构建与烧录

### 编译

```bash
cd /root/ws63_ohos

# 编译 Server
python build.py -p nearlink_dk_3863@hihope -T "applications/sample/wifi-iot/app/work1:work1_demo"

# 编译 Client
python build.py -p nearlink_dk_3863@hihope -T "applications/sample/wifi-iot/app/work2:work2_demo"
```

### 烧录

用海思烧录工具分别烧录两块板：
- 板 A → `work2` 的固件
- 板 B → `work1` 的固件

---

## convert.py — 串口宏键盘脚本

运行在 **PC 端**，读取 Server 板的串口输出，用 `pyautogui` 模拟键盘操作。

### 安装依赖

```bash
pip install pyautogui pyserial
```

### 配置

编辑 `convert.py` 顶部：

```python
SERIAL_PORT = "COM15"   # 改成 Server 板的实际 COM 口
BAUD_RATE = 115200
```

### 运行

```bash
python convert.py
```

**使用前先关闭串口助手**，否则 COM 口会被占用。

### 串口协议

Server 通过串口发送单行事件：

| 串口数据 | 含义 | pyautogui 动作 |
|---------|------|---------------|
| `1` | Enter 键 | `pyautogui.press("enter")` |
| `2` | Backspace 键 | `pyautogui.press("backspace")` |
| `3D` | 语音键按下 | `keyDown("ctrl")` + `keyDown("win")` |
| `3U` | 语音键松开 | `keyUp("win")` + `keyUp("ctrl")` |

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

---

## 依赖

- **芯片**：海思 WS63E（九联星闪开发板）
- **系统**：OpenHarmony LiteOS-M（RISC-V 32 位）
- **编译器**：`riscv32-linux-musl-gcc`（或 `gcc-riscv64-linux-gnu` + 符号链接）
- **PC 端**：Python 3 + pyautogui + pyserial
