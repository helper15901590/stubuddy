# stubuddy S3 N16R8：检查点3.1

本板型位于小智主工程中，通过唯一的 `DECLARE_BOARD(StubuddyBoard)` 创建，复用原有 `Board`、`AudioCodec::OutputData()` 接口。
本次只验收启动和一次本地提示音。独立诊断工程已验收的PTT、灯效、配网随后逐项迁移。

3.1已由用户完成三次重新上电验收，提交为 `dac5504`。
当前新增的3.2按键录放使用独立变体 `stubuddy-s3-n16r8-ptt`，见 [PTT-VALIDATION.md](PTT-VALIDATION.md)。
本页保留3.1启动变体的操作说明；3.2仍保持LED熄灭，灯效迁移在3.3进行。

3.2已由用户验收并提交 `e6e553c`。当前3.3新增 `stubuddy-s3-n16r8-ptt-led`，
见 [LED-VALIDATION.md](LED-VALIDATION.md)：回放结束后无论按键是否仍按住，一律熄灯；仍需先松键再重新按下才能录音。

| 身份 | 值 |
| --- | --- |
| 物理板型/后续OTA族 | `stubuddy-s3-n16r8` |
| 当前验证固件变体 | `stubuddy-s3-n16r8-startup` |
| 芯片与环境 | ESP32-S3 N16R8，ESP-IDF 6.1，配套Python 3.13.7 |
| 工程版本 | 沿用小智2.5.2；日志另标明CHECKPOINT 3.1与唯一变体 |

## 上电流程

```text
Board工厂 → GPIO10功放关闭、GPIO18灯灭
         → 检查16MiB Flash和8MiB PSRAM
         → AudioCodec启动I2S1，等待两秒
         → 播放一次660Hz、180ms提示音，首尾10ms淡入淡出
         → 写入静音、关闭功放、释放I2S1 → 本地待机
```

固定提示音沿用实测约3% PCM16满幅峰值，通过单声道AudioCodec送入32位立体声总线：左槽为零、右槽为声音。
出现初始化或输出错误时，尝试将GPIO10拉低并停止输出，不自动重复播放。
物理是否听得清楚、是否有底噪仍须真机判断。

本验证入口不启动Application对话循环、网络/OTA、PTT或麦克风；GPIO18始终熄灭。
返回app_main后FreeRTOS待机任务继续运行，`Returned from app_main()`在这里正常。

## 引脚

| 功能 | GPIO |
| --- | --- |
| NS4168 BCLK / LRCLK / DATA | 46 / 9 / 8 |
| NS4168 CTRL | 10，低电平关机，高电平选择右声道 |
| PDM CLK / DATA | 3 / 42，保留配置，本次不启动采集 |
| 主按键 | 15，保留配置，本次不接入PTT |
| 白色LED | 18，低电平有效，本次保持高电平熄灭 |

保持已测试的接线及NS4168供电条件，不需要重新接线。

## 电脑PowerShell：编译

以下目录为小智主工程，和此前speaker-selftest诊断工程不同。

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-startup
```

预期出现 `Project build complete`，并生成 `build\xiaozhi.bin`。随后核对构建结果：

```powershell
python scripts/verify_stubuddy_startup.py
python -m serial.tools.list_ports
```

核对脚本应全部输出PASS；串口列表找到开发板端口。如果核对失败，反馈FAIL行，先检查板型选择。
该变体通过专用defaults与依赖条件裁剪，构建中不包含屏幕、字体、摄像头、Wi-Fi、BLE等当前不使用的组件。
其他板型保留各自原有依赖；条件在root CMake配置早期设置，避免在组件解析时等待Kconfig。
参考[ESP-IDF组件条件依赖说明](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/manifest_file.html)。

## 烧录与日志

下面COM4为示例，请替换成电脑实际端口：

```powershell
idf.py -p COM4 -b 115200 flash monitor
```

正常情况下不需要擦除Flash。分区保持与此前诊断工程一致：NVS在0x9000、大小0x6000，应用在0x10000。
烧录清单仅写bootloader、分区表和应用；本验证入口不初始化NVS，也不读取/改写既有配网记录。
使用上面的常规flash命令，不使用包含填充区域的merged-binary整片写入，以保留NVS。

关键日志顺序应包含：

```text
STUBUDDY_BOARD: board=stubuddy-s3-n16r8; variant=stubuddy-s3-n16r8-startup; amplifier OFF; LED OFF; ready=yes
STUBUDDY_STARTUP: CHECKPOINT 3.1; firmware=2.5.2; board=stubuddy-s3-n16r8; variant=stubuddy-s3-n16r8-startup
STUBUDDY_STARTUP: MEMORY: flash=16 MiB; PSRAM=8 MiB
STUBUDDY_CODEC: AudioCodec ready: I2S1, 16000 Hz, 32-bit stereo; LEFT=0, RIGHT=PCM
STUBUDDY_STARTUP: TONE START 1/1: 660 Hz; mono PCM -> AudioCodec -> RIGHT I2S slot
STUBUDDY_CODEC: OUTPUT OFF: CTRL=LOW; I2S1 released
STUBUDDY_STARTUP: DONE: local standby; amplifier OFF; LED OFF; no automatic repeat
```

Ctrl+]退出串口监视。端口占用时先关闭其他串口工具；若烧录连接失败，按原先已验证的BOOT/RESET方式进入下载。
看到旧的三次提示音或配网热点时，先核对工程目录与CHECKPOINT/variant日志是否正确。

## 软件验证记录

- ESP-IDF6.1：canonical build脚本构建stubuddy变体成功；应用约196KiB，1MiB应用分区内有约81%空余。
- 核对脚本确认56个SDK/主组件，无屏幕、LVGL、字体、摄像头、Wi-Fi或BLE组件；恰好一个Board工厂和app_main。
- 普通bread-compact-wifi ESP32-S3构建也成功，在独立build/compat-bread目录验证正常显示/网络构建路径。
- 主机unittest套件115项完成，2项因本机缺少对应开发工具/库跳过。Windows验证采用临时Git Bash/MSVC入口和临时目录清理适配，测试断言未改。
- 修改过的C/C++文件通过仓库clang-format检查。
- 以上不替代实际提示音、静音收尾与三次上电验收；麦克风/唤醒/VAD/双工等尚未在本配置启动。

## 真机验收

1. 每次上电只听到一次短提示音，无破音或明显爆音。
2. 播放结束后安静，功放关闭；LED保持熄灭。
3. 保持供电30秒，无再次提示音、复位循环或持续杂音。
4. 断电重新上电三次，以上行为均正常。
5. 日志中无GPIO10冲突、I2S错误、内存不足或panic。

本版按键不录音，也不开设备热点，这是3.1验证入口的预期范围。下一检查点再迁移PTT。

需要回到已验收的配网/录放版本时，可退出监视后执行：

```powershell
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
idf.py -p COM4 -b 115200 flash monitor
```

## 确认通过后Git检查点

以下命令仅在用户完成三次上电验收后执行：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git add -- xiaozhi-esp32-main/xiaozhi-esp32-main stubuddy/firmware/speaker-selftest/DEVELOPMENT-ROADMAP.md
git diff --cached --stat
git commit -m "feat(board): add stubuddy audio-only startup variant" -m "Reuse the XiaoZhi Board and AudioCodec interfaces with NS4168 right-channel output and a trimmed startup build. Verify one prompt, quiet shutdown, and three successful power cycles on hardware."
git log -1 --oneline
git status --short
```

检查暂存源码和文档，构建产物、sdkconfig、managed_components、tmp不应在提交中。
