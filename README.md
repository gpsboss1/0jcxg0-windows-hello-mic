# ESP32-S3 PDM 双麦 USB UAC 麦克风

基于 ESP32-S3 + ESP-IDF v6.0.2 的 USB Audio Class（UAC）麦克风设备：两颗 PDM 数字麦克风采集双声道音频，经 I2S PDM 接收后通过 TinyUSB / `espressif/usb_device_uac` 组件以标准 USB 麦克风形式呈现给电脑。

设备仅暴露为 **USB 麦克风**（不包含扬声器端点），即插即用，免驱。

## 功能特性

- **双声道（Stereo）PDM 采集**：两颗 PDM 麦克风共享 CLK / DIN 总线，通过时钟边沿区分左右声道（ESP32-S3 立体声 PDM 模式），16-bit / 48 kHz
- **USB 标准 UAC 麦克风**：对电脑呈现为标准 USB 麦克风，无需安装驱动；可配合任意录音软件（WavePad、Audacity 等）使用
- **ALC 自动电平控制**：基于 `espressif/esp_audio_effects` ALC，自动补偿音量差异、抑制削波；可切换为内置自定义软件 AGC 后端
- **宿主音量控制（Manual Gain）**：电脑端麦克风音量滑块（0–100）映射到可配置增益范围，静音时归零
- **稳定的音频热路径**：除上电初始化外移除所有日志打印，避免 UART 阻塞导致周期性爆音

## 音频信号链路

```
PDM麦克风(左/右) → I2S PDM RX → [ALC 自动增益] → [Manual Gain 宿主音量] → USB UAC → PC
```

主链路见 [main/i2s_pdm_example_main.c](main/i2s_pdm_example_main.c) 中的 `uac_device_input_cb`（音频热路径，需保持精简，禁止在回调中加入阻塞调用/日志）。

## 硬件连接

按 PCB 适配，PDM 引脚定义在 [main/i2s_pdm_rx_stereo.c](main/i2s_pdm_rx_stereo.c)：

| 信号 | GPIO | 说明 |
| ---- | ---- | ---- |
| PDM CLK | GPIO 6 | 两颗麦克风共享时钟 |
| PDM DIN | GPIO 5 | 两颗麦克风共享数据线（左右声道由 SEL 引脚区分） |

> 左右声道交换通过 `clk_inv = true` 在硬件时钟相位层面实现

## 构建与烧录

环境要求：**ESP-IDF v6.0.2**（当前工程基于该版本编译验证，见 [dependencies.lock](dependencies.lock)）。

```bash
# 若使用 devcontainer / VS Code ESP-IDF 插件，IDF 路径见 .vscode/settings.json

idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

### 主要依赖组件

| 组件 | 版本 | 用途 |
| ---- | ---- | ---- |
| `espressif/usb_device_uac` | ^1.3.1 | UAC 设备驱动（TinyUSB 封装） |
| `espressif/esp_audio_effects` | ^1.0.0 | ALC 自动电平控制 |
| `espressif/cmake_utilities` | ^1.1.1 | ESP 组件构建工具 |
| `i2s_examples_common` | IDF 本地路径 | I2S 引脚公共定义（仅单声道路径使用） |

## 配置（menuconfig）

```
idf.py menuconfig
```

### I2S PDM Example Configuration

- `EXAMPLE_PDM_DIR`：PDM 方向选择（当前使用 PDM RX）

### AGC Configuration（自动电平控制）

选择 ALC 后端：

- `AGC_BACKEND_ESP_AE`：基于 `esp_audio_effects` ALC（**当前默认，见 [sdkconfig](sdkconfig)**），预编译 SIMD 优化库，~3-5% CPU @48kHz 立体声
- `AGC_BACKEND_CUSTOM`：内置轻量软件 AGC，峰包络检测 + 压缩比 + 噪声门 + 软限幅，零外部依赖，见 [main/agc_processor.c](main/agc_processor.c)
- `AGC_BACKEND_NONE`：关闭 ALC（透传）

当前生效参数（sdkconfig）：目标电平 `-6 dBFS`、最大增益 `12 dB`、静音阈值 `-35 dBFS`、起音 `5 ms`、释放 `200 ms`、噪声门 `-60 dBFS`。

### Manual Gain Control（宿主音量增益）

- `MANUAL_GAIN_ENABLE`：启用手动增益级（位于 ALC 之后），增益由电脑端麦克风音量滑块控制
- `MANUAL_GAIN_MIN_DB` / `MANUAL_GAIN_MAX_DB`：音量 0 / 100 对应的增益（当前为 `-5 dB` / `+5 dB`，见 [sdkconfig](sdkconfig)；Kconfig 默认 0 / 24 dB）
- 静音时增益归零（`mute` 回调将增益置为 0）

## USB 描述信息

默认描述符（可在 menuconfig 中修改）：

- VID `0x303A`，PID `0x8000`
- Manufacturer：`Espressif`，Product：`ESP UAC Device`
- 采样率 `48000 Hz`，位深 `16-bit`，通道数 `2`（仅麦克风，`UAC_SPEAKER_CHANNEL_NUM=0`）
- USB 微帧间隔 `10 ms`

## 项目结构

```
├── main/
│   ├── i2s_pdm_example_main.c   # 主入口：初始化 PDM 通道 + ALC + 手动增益 + UAC
│   ├── i2s_pdm_rx_stereo.c      # 双声道 PDM RX 初始化（GPIO6/GPIO5，clk_inv 交换声道）
│   ├── i2s_pdm_rx.c             # 单声道 PDM RX 初始化（遗留，可用于调试）
│   ├── i2s_pdm_tx.c             # PDM TX（遗留，当前未使用）
│   ├── agc_processor.c/.h       # ALC 封装（支持 esp_audio_effects / 自定义 / 关闭 三种后端）
│   ├── manual_gain.c/.h         # 宿主音量 → 线性增益
│   └── Kconfig.projbuild        # 工程配置菜单
├── sdkconfig.defaults           # 默认配置（PDM RX / 双声道 / ALC 自定义后端）
└── 16K_SAMPLE_RATE_OPTIMIZATION.md  # 16K 采样率底噪优化方案（记录用）
```

## 开发环境

- ESP-IDF 6.0.2
