# wifi-iot/app

星闪（SLE）示例应用，基于海思 WS63E 芯片。

## 模块

| 目录 | 角色 | 入口 | 说明 |
|------|------|------|------|
| `work1/` | SLE Server | `app_run` | UUID Server，广播 `sle_uuid_server`，接收写入并转发串口 |
| `work2/` | SLE Client | `app_run` | UUID Client，扫描并连接 Server，发送按键数据 |

## 构建

```bash
cd /root/ws63_ohos
python build.py -p nearlink_dk_3863@hihope -T "applications/sample/wifi-iot/app/work1:work1_demo"
python build.py -p nearlink_dk_3863@hihope -T "applications/sample/wifi-iot/app/work2:work2_demo"
```

## 依赖

- RISC-V 交叉编译器（`riscv32-linux-musl-gcc`）

## 目录结构

```
app/
├── README.md
├── .gitignore
├── work1/
│   ├── BUILD.gn
│   ├── sle_uuid_server.h
│   ├── sle_uuid_server.c
│   ├── sle_server_adv.h
│   └── sle_server_adv.c
└── work2/
    ├── BUILD.gn
    ├── sle_uuid_client.h
    └── sle_uuid_client.c
```
