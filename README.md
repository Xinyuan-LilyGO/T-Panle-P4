# T-Panel-P4

ESP32-P4 multi-board project for the Standard, Round, and Rect variants.

## Project layout

```text
components/
  t_panel_p4_bsp/   Shared buses, pins, power, and board capabilities
  display_panel/    LCD controllers and board display profiles
  touch_panel/      Touch controller implementations
  radio_hal/        LoRa hardware abstraction
main/
  examples/
    factory/        Factory application
```

Board-specific source files and managed dependencies are selected before the
ESP-IDF component dependency scan. Select both the board and example with CMake
arguments, and use a separate build directory for every combination.

## Board variants

| Board      | Display  | Touch  | XL9555 | LoRa | ESP32-C5 |
| ---------- | -------- | ------ | ------ | ---- | -------- |
| `standard` | JD9365   | Jadard | Yes    | Yes  | Yes      |
| `round`    | JD9365DA | GT911  | Yes    | Yes  | Yes      |
| `rect`     | ILI9882  | Ilitek | No     | No   | No       |

## Build

Run these commands from an ESP-IDF 5.5 environment:

```powershell
$env:IDF_TARGET = "esp32p4"

# Standard factory firmware
idf.py -B build/standard-factory `
  -D T_PANEL_P4_BOARD=standard `
  -D T_PANEL_P4_EXAMPLE=factory build

# Round factory firmware
idf.py -B build/round-factory `
  -D T_PANEL_P4_BOARD=round `
  -D T_PANEL_P4_EXAMPLE=factory build

# Rect minimal LCD and touch example
idf.py -B build/rect-lcd-touch `
  -D T_PANEL_P4_BOARD=rect `
  -D T_PANEL_P4_EXAMPLE=lcd_touch build
```

The default selection is `standard + factory`. Available example names are:

```text
factory                 xl9555                  axp517
lora_sx1276             lora_sx1262             lora_lr1121
lora_lr2021             lcd                     lcd_touch
audio_play_wav          audio_record            audio_loopback
camera                  camera_esp_video        camera_flash_led
esp32p4_host_wifi       esp32p4_host_test       sd
sgm38121                sensor                  drv2605
qmc6309                 qmi8658c                lvgl
usb_otg_msc             codex_keyboard
```

Board and example selection is done with CMake arguments, not `menuconfig`.
Every example has an `sdkconfig.defaults` and `idf_component.yml` in its own
directory. CMake combines the common, board, and example defaults into a
build-local `sdkconfig`:

```text
sdkconfig.defaults
+ components/t_panel_p4_bsp/configs/<board>.defaults
+ main/examples/<example>/sdkconfig.defaults
-> build/<board>-<example>/sdkconfig
```

Each build directory also owns its `dependencies.lock`. This prevents variants
from overwriting one another's configuration and preserves their incremental
Ninja caches when switching examples. Do not reuse one build directory for a
different board/example combination.

Build different variants sequentially because ESP-IDF still shares the
project-level `managed_components/` directory. The selected example is exposed
as the only `main` component, so its manifest pulls in only the managed
dependencies needed by that example.

The complete example name list and Rect hardware restrictions are validated in
the top-level `CMakeLists.txt`.

### VS Code

Run `Tasks: Run Build Task` (`Ctrl+Shift+B`) and select the board and example
from the two prompts. The default task builds the selected variant and updates
the ESP-IDF extension settings, so its Build, Flash, and Monitor commands use
the same build directory afterward.

To switch the active variant without building, run `Tasks: Run Task` and choose
`T-Panel-P4: Select Example Only`.


# 开发调试
## rect
### 1. 硬件
| 硬件   | 问题                       | 备注 |
| ------ | -------------------------- | ---- |
| 摄像头 | 排线顺序反                 |      |
| 传感器 | I2C多次上拉                |      |
| IO排针 | 连接之后出现短路，没有端口 |      |
