# 固件构建与烧录 / Firmware build and flashing

当前公开版本为 **0.7.103-p4-r2**，对应桌面端 **0.1.168**，基于既有公开版本维护。不包含 0.7.108 或尚未验收的框架改动。部分 Windows 环境下的串口打开问题仍在排查，不因源码同步而标记解决。

目标为 ESP32-P4、32 MiB Flash；硬件参数见 `boards/`、`sdkconfig.defaults`、`partitions.csv`。完整技术说明见 [README.md](README.md)。

## 获取与构建 / Build

只想使用设备，无需安装编译环境：下载对应芯片的 [完整烧录包](https://github.com/YizhengWw/HachimoDock/releases/latest)，按包内中文指南操作。烧录需要 Python **3.10+**，包内脚本会安装并检查 **esptool 5.4.0**；不要用旧版工具或 `--force` 绕过芯片检查。

自行构建需要 Git、Python 3.10+、PlatformIO Core **6.1.19**（6.1.18 不支持 v3 平台）。当前素材由普通 Git 保存，不依赖 LFS 额度。在独立 Python 环境中安装：`python -m pip install platformio==6.1.19 pytest Pillow`。项目通过 `platformio.ini` 固定平台版本，由其安装 ESP-IDF 和 P4 工具链；首次构建需要联网下载依赖。

| 项目 | v1 构建 | v3 构建 |
| --- | --- | --- |
| PlatformIO 环境 | `esp32_p4_evboard` | `esp32_p4_evboard_v3` |
| pioarduino 平台 | `55.03.32` | `55.03.38-1` |
| ESP-IDF | `5.5.1` | `5.5.4` |
| RISC-V 工具链 | `14.2.0_20250730` | `14.2.0_20260121` |
| 依赖锁 | `dependencies.lock` | `dependencies.v3.lock` |
| 出厂烧录工具（不同于编译环境自带版本） | esptool **5.4.0** | esptool **5.4.0** |

两种环境请依次构建，不要在同一工作目录并行构建，避免依赖环境相互覆盖。Windows 建议把仓库和构建缓存放在较短的纯英文路径（例如 `C:\work\HachimoDock`），并在 PowerShell 使用下方相同的 `python -m platformio` 命令；`tools/p4.ps1` 的默认构建目标仍是 v1。

```sh
# 仓库根目录 / repository root
python firmware/tools/build_builtin_bundle.py --check
python -m pytest firmware/tests
cd firmware
python -m platformio run -e esp32_p4_evboard
python -m platformio run -e esp32_p4_evboard_v3
```

`esp32_p4_evboard` 构建旧版芯片固件（0.1～1.99）；`esp32_p4_evboard_v3` 构建 v3.0～3.99 固件，使用 ESP-IDF 5.5.4。`esp32_p4_evboard_windows` 仅是默认环境的兼容别名。**固件按芯片版本区分，不按 Windows/macOS 区分**；两端都使用 4M UART。

微雪 ESP32-P4-WIFI6-M 的 M 表示焊接排针，不能据此判断芯片版本。**v1 与 v3 的镜像不可互刷，具体版本可以咨询客服进行确认。** 也不要通过修改最低芯片版本或强制烧录绕过检查。每次切换环境均使用对应的 `sdkconfig.<环境名>`，不要复制另一个环境的 sdkconfig。

CPU 时钟遵循 ESP-IDF 的芯片配置：v1 保持 360 MHz，v3 使用 400 MHz（v3 配置不提供 360 MHz 选项）。没有为旧芯片开启实验性超频。画面、游戏与输入继续按原有时间逻辑运行，UART 速率不变。参见 [ESP-IDF 5.5.4 时钟配置](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_system/port/soc/esp32p4/Kconfig.cpu)。

PC 打包前，将两个环境的 `firmware.bin` 分别放入 `pc/src-tauri/firmware/esp32-p4/firmware.bin` 和 `firmware-v3.bin`。资源检查要求版本一致、芯片范围正确且来自干净提交；不要用空文件或旧镜像代替。客户端按实际芯片版本选择升级文件，未知版本不会猜测。

屏幕排线必须接 **DSI**（显示输出），不能接 CSI（摄像头输入）；请断电后插拔排线。针对配套屏幕的 v3 固件采用 24 MHz 显示时序，相关修复已在 v3 板确认恢复宠物动画。未连接摇杆或启动校准无效时，方向输入停用，连接摇杆并保持居中后重启可重新校准。

0.7.53-p4 修复了部分形象循环播放或切换状态后停止的问题。v3.2 已完成保留数据的 OTA，以及休息、工作、等待、结束和返回休息的连续播放验证；v1 已完成编译和共享播放代码的主机回归，尚未完成本版实机验证。以上不代表全部功能已验收，按键/摇杆、麦克风、完整形象下发、断线重连及断电恢复仍需分别验证。

## 完整出厂镜像 / Complete factory image

先运行 `pio run -e esp32_p4_evboard_v3 -t factory`（旧芯片改用 `esp32_p4_evboard`）。PC/设备仍使用同一套形象、组件、分区和交互配置。

`factory-config.json` 读取 `../pc/public/terrier-clips/` 与 `../pc/builtin-clawpkgs/`，把形象和组件放入出厂存储。默认构建的 factory-image 后处理脚本生成完整镜像及清单；不要将仅应用分区的 `.bin` 当作完整烧录包。

在仓库根目录执行 / From the repository root:

```sh
node pc/scripts/package-factory-release.mjs \
  --build-dir firmware/.pio/build/esp32_p4_evboard_v3 \
  --output /path/to/empty-output-directory
```

输出 ZIP 名称包含 v1/v3，内含中文烧录指南、完整镜像、分段 bin、形象/组件资料、校验清单及 macOS/Windows 烧录入口。工具读取实际芯片版本，与 Bootloader 和应用镜像都匹配后才擦除。首次烧录使用完整镜像及包内工具；日常 OTA 使用 PC 随包的应用升级镜像。

## 更换屏幕：3.5 寸 ST7796 / Replacement 3.5" panel

原配微雪 4.3 寸 ST7701（MIPI DSI，480x640）已停产。替换屏为鸿讯电子
**CL35BC1017-40A**（ST7796S，320x480 玻璃，FT6336U 电容触摸）。

默认走 **8080 8 位并口**（LCD_CAM i80）。选它不是因为并口更"正宗"，而是
带宽差 8 倍：SPI 40 MHz 推满一帧 480x320 RGB565 要 62 ms，8 位并口只要约
8 ms，而屏自身的刷新率才 60 Hz。更重要的是 LCD_CAM 有硬件字节交换
（`flags.swap_color_bytes`，见 `lcd_ll_enable_swizzle`），RGB565 从 PSRAM
直接 DMA 出去即可，SPI 分支那套 16 行中转缓冲和逐像素 swap 全部省掉。

`main/Kconfig.projbuild` 的 `PET P4 hardware` 菜单里三选一：DSI 原屏、
ST7796 SPI、ST7796 8080 并口（默认）。

```sh
cd firmware
idf.py menuconfig     # PET P4 hardware -> Installed LCD panel
idf.py build
```

### LCD 40PIN FPC 接线

| 脚 | 信号 | 接到 | 说明 |
| --- | --- | --- | --- |
| 1–4 | XL / YU / XR / YD | 不接 | 电阻触摸的电极脚。本屏是电容触摸，这四脚空置 |
| 5 | GND | GND | |
| 6 | IOVCC | 3.3 V | I/O 电源，1.65–3.3 V |
| 7 | VCC | 3.3 V | 模拟电源，2.5–3.3 V |
| 8 | FMARK | 可不接 | TE 撕裂信号，当前固件不使用 |
| 9 | CS | **GPIO43** | `PET_P4_LCD_I80_PIN_CS` |
| 10 | RS | **GPIO42** | 命令/数据选择，等同 SPI 版的 D/C |
| 11 | WR | **GPIO41** | 写选通，每个字节翻转一次 |
| 12 | RD | 3.3 V | 只写不读，固定为无效电平 |
| 13 | SDA | 不接 | SPI 模式的串行数据，并口模式不用 |
| 14 | SDO | 不接 | 同上 |
| 15 | RESET | **GPIO44** | 接板级复位时可把配置项设为 -1 |
| 16 | GND | GND | |
| 17–24 | DB0–DB7 | **GPIO33–GPIO40** | 依次对应，8 根连续走线 |
| 25–32 | DB8–DB15 | 不接 | 8 位模式只用低 8 位 |
| 33 | LEDA | 背光正极 | 见下方背光电路 |
| 34–36 | LEDK | 背光负极 | 三脚并接 |
| 37 | GND | GND | |
| 38 | IM0 | **3.3 V** | 必须为 1 |
| 39 | IM1 | **3.3 V** | 必须为 1 |
| 40 | IM2 | **GND** | 必须为 0 |

**IM0/IM1/IM2 接错屏就完全不响应**，而且现象和"背光没亮""初始化失败"一模一样，
排查时会绕很远。规格书里的对应关系：

| IM0 | IM1 | IM2 | 模式 |
| --- | --- | --- | --- |
| 0 | 1 | 0 | 80-16bit 并口 |
| 1 | 1 | 0 | **80-8bit 并口（本项目）** |
| 1 | 0 | 1 | 3 线 SPI |
| 1 | 1 | 1 | 4 线 SPI |

### CTP 6PIN FPC 接线

触摸是**独立的一根 6 针排线**，不在那 40 针里面。

| 脚 | 信号 | 接到 | 说明 |
| --- | --- | --- | --- |
| 1 | VCC | 3.3 V | |
| 2 | SDA | **GPIO7** | `PET_P4_TOUCH_I2C_SDA`，FT6336U 地址 0x38 |
| 3 | SCL | **GPIO8** | `PET_P4_TOUCH_I2C_SCL`，规格 100 kHz |
| 4 | INT | 可不接 | 固件按 15 ms 轮询，不依赖中断；建议留测试点 |
| 5 | RST | 3.3 V | 低有效。接 3.3 V 即保持释放；想留硬复位手段就改接一个 GPIO |
| 6 | GND | GND | |

### 背光电路

规格书 2.3 节：VLED 正向压降 2.9–3.3 V（典型 3.1 V），电流 120–150 mA，
所以 **3.3 V 直驱，不需要升压**。Vf 的离散性会让直接硬接的电流飘得厉害，
务必串限流电阻，取值 1–2 Ω（按 3.3 V − 3.1 V ÷ 0.15 A ≈ 1.3 Ω 起算）。

要 PWM 调光时：LEDA 接 3.3 V，三个 LEDK 并接后接 NMOS 漏极，源极接地，
栅极接 **GPIO26**（`PET_P4_LCD_I80_PIN_BACKLIGHT`），PWM 频率放在 2 kHz 以上避免可见闪烁。

150 mA 对板上的 3.3 V LDO 不算小，背光单独一路供电、加 10 µF 去耦，别和
P4 的 I/O 电源挤同一个走线。

### 配置项一览

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `PET_P4_LCD_I80_PIN_D0`..`D7` | 33..40 | 数据总线 |
| `PET_P4_LCD_I80_PIN_WR` | 41 | 写选通 |
| `PET_P4_LCD_I80_PIN_RS` | 42 | 命令/数据 |
| `PET_P4_LCD_I80_PIN_CS` | 43 | 片选 |
| `PET_P4_LCD_I80_PIN_RST` | 44 | 复位 |
| `PET_P4_LCD_I80_PIN_BACKLIGHT` | 26 | 背光 PWM |
| `PET_P4_LCD_I80_PCLK_HZ` | 40000000 | WR 时钟，上限是 LCD_CAM 的 80 MHz |
| `PET_P4_LCD_I80_QUEUE_DEPTH` | 10 | 事务队列深度 |
| `PET_P4_TOUCH_I2C_SDA` / `SCL` | 7 / 8 | CTP |
| `PET_P4_TOUCH_I2C_HZ` | 100000 | FT6336U 规格值 |

### 排障与其它

- 花屏或颜色反：先看 `swap_color_bytes`。字节序反了会呈现"红蓝互换 + 明暗错位"，
  和单纯的 RGB/BGR 互换不同；后者改 `rgb_ele_order`。
- 数据和时钟沿对不上时，把 `pclk_active_neg` 和 `pclk_idle_low` 两个标志**成对**翻转。
  ST7796 在 WR 上升沿锁存数据，当前配置是低电平空闲 + 上升沿有效。
- 画面上下/左右翻：`PET_P4_LCD_MIRROR_X` / `PET_P4_LCD_MIRROR_Y`。
  触摸方向相反时改 `PET_P4_TOUCH_MIRROR_X` / `PET_P4_TOUCH_MIRROR_Y`（两组互相独立）。
- 屏幕在驱动里按横屏（MADCTL MV）寻址，即 480x320。UI 仍是 640x480，渲染时等比缩到
  424x320 居中，左右各留 28 px 黑边；不想留黑边就得重排 UI，见
  `pet_p4_renderer.c` 的 `PET_P4_PANEL_FIT_*`。
- 触摸沿用工程里既有的 FT6336U 寄存器路径（0x38，寄存器 0x02），不需要 GT911 组件。
- 手头已有旧 SPI 转接板时：menuconfig 选 `CL35BC1017-40A ST7796 (4-wire SPI)`，
  引脚表在 `sdkconfig.spi-backup` 对应的那一组配置项里。
- 回到原微雪屏：menuconfig 里选 `Waveshare 4.3" ST7701`，BSP 路径原样保留。

## 注意 / Caution

完整出厂烧录会覆盖设备上的原有配置和素材，操作前自行备份。烧录前核对板型、Flash 容量和 SHA-256，传输过程中不要拔线。Live-device tests and flashing are manual opt-in operations and are not run by source CI.

ESP-IDF 可能把本地驱动路径写入 `dependencies.lock`。提交前运行 `python tools/normalize_dependency_lock.py`，将本机路径恢复为 `$PET_P4_PROJECT_DIR/components/…`；勿提交绝对路径。
