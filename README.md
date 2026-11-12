# ESP32-S3 最小 BLE 网关

使用 ESP-IDF v6.1，从官方空白模板建立。S3 扫描并连接 C3，每秒写入一次 PING，接收 C3 的 PONG 通知，断连后重新扫描。

## 编译与烧录

用 VS Code 的 ESP-IDF 插件打开本目录，选择已安装的 v6.1，在 ESP-IDF 终端编译。项目已固定目标为 esp32s3。

编译：

```powershell
idf.py build
```

烧录并查看串口日志，将 COM7 替换为 S3 的实际串口：

```powershell
idf.py -p COM7 flash monitor
```

配套 C3 固件也需烧录并上电。两板通过蓝牙通信，不需要连接彼此的串口。S3 完成连接与订阅后，日志持续出现 `TX: PING` 和 `RX: PONG`。退出串口监视器使用 Ctrl+]。

## 通信方式

BLE 是 Bluetooth Low Energy，低功耗蓝牙。S3 担任中央设备，负责扫描和连接；C3 担任外围设备，负责广播。GATT 是 Generic Attribute Profile，通用属性规范，通过“服务”和“特征”组织可读写的数据。

- S3 只连接广播了本项目服务 UUID（通用唯一标识符）的设备，不按名称或固定蓝牙地址连接。
- 服务发现直接调用 ESP-IDF 的 nimble_central_utils，不复制或重写其发现算法。
- 两端共用一个可读、可写、可通知的特征。S3 先订阅通知，收到订阅成功回调后才启动发送定时器。省略订阅会导致收不到 PONG。
- CCCD 是 Client Characteristic Configuration Descriptor，客户端特征配置描述符；S3 写入通知开关，C3 才能向它发送通知。
- 消息都是四字节 ASCII 文本，不附加字符串结束符或换行。当前单连接、无配对认证，服务 UUID 仅用于匹配协议，不代表设备身份认证。

服务 UUID：

```text
d58f7e60-4c93-4f7b-9ba7-664e114dab7c
```

特征 UUID：

```text
d58f7e61-4c93-4f7b-9ba7-664e114dab7c
```

请求文本：

```text
PING
```

请求十六进制字节：

```text
50 49 4E 47
```

回复文本：

```text
PONG
```

回复十六进制字节：

```text
50 4F 4E 47
```

## 核心代码与配置

- main/main.c：初始化 NVS 和 NimBLE，启动官方主机任务。NVS 是 Non-Volatile Storage，片上持久存储，官方例程用它保存射频校准等数据。
- main/ble_gateway.c：扫描、连接、服务发现、通知订阅、定时发送和断连恢复。定时器与蓝牙回调都运行在 NimBLE 事件队列中，避免额外任务共享连接状态。
- main/ble_protocol.h：服务 UUID、特征 UUID 与 PING/PONG 定义。两个独立仓库各保存一份，修改协议时同步两份。
- sdkconfig.defaults：开启 NimBLE，限制为一个连接。S3 按 N16R8 的 16 MB Flash 配置；PSRAM（外部运行内存）暂不启用，本阶段用不到它。
- 串口日志同时输出到默认 UART0 和原生 USB Serial/JTAG；具体使用开发板接出来的串口即可。
- .clang-format：Allman 风格，左大括号独占一行，四空格缩进；.vscode/settings.json 让 C/C++ 扩展按此文件格式化。

修改前这里只是空白 app_main。现在先由官方协议栈完成连接与通知订阅，再由事件定时器调用 ble_gattc_write_flat 发送 PING；收到通知后检查长度与内容，只有合法 PONG 才计为回复。

## 验证

编译验证和板端验证是两件事。编译成功只能说明代码、配置、链接与固件空间检查通过，不能证明真实无线链路可用。

板端快速验收：先给两板烧录并上电，关闭占用 S3 串口的监视器，再运行下列脚本。它使用 ESP-IDF 环境已有的 pyserial，默认在 30 秒内要求至少三个完整收发周期。

```powershell
python tests/verify_ble.py --port COM7
```

随后保持 S3 上电，重启 C3，确认 S3 日志先断连再重新连接，并恢复 PING/PONG。再交换顺序重启 S3，检查同样的恢复行为。

首次交付的验证结果见 [docs/verification.md](docs/verification.md)。本阶段不接入 INMP441、SG90 或电源控制。

## 官方来源

- [空白项目模板](https://github.com/espressif/esp-idf/tree/v6.1/tools/templates/sample_project)
- [blecent 中央设备示例](https://github.com/espressif/esp-idf/tree/v6.1/examples/bluetooth/nimble/blecent)
- [nimble_central_utils 设备发现辅助组件](https://github.com/espressif/esp-idf/tree/v6.1/examples/bluetooth/nimble/common/nimble_central_utils)

本机 SDK 标签为 v6.1，对应配置头文件版本 6.1.0。代码复用对应版本的官方示例，并保留 Apache-2.0 许可与 NOTICE；未新增第三方蓝牙库。
