# stubuddy S3 N16R8：检查点3.1

4.2最新结论（2026-11-05）：用户确认合并真机验收全部通过，音量50/60读音、短句控制、收尾按键、打断记忆和60秒保护正常，连续20轮无误超时。`stubuddy-s3-n16r8-ai-ptt-controls`包括真实设备音量工具、掉电保存/静音恢复、70%数字峰值、LED律动及并发计时修复；软件84项测试和三个变体构建/核对通过。4.1已提交`4b41025`，4.2待用户Git提交，停在本检查点。详见[4.2手册](../../../../../stubuddy/server/voice-ai/PTT-CONTROLS.md)。下文保留历史开发记录。

最新状态（2026-11-04）：3.6.2已真机通过并提交`6176ac9`。当前4.1合并PTT ASR/LLM/TTS，
新增`stubuddy-s3-n16r8-ai-ptt`；已修复为流式TTS和独立1275字节下行上限。用户确认新版云端自测、编译、烧录及真机验收通过，首声明显缩短，按键打断、60秒和连续十轮正常；保留8轮完整历史加当前问题。Git状态以实际日志为准，停在本检查点。
操作见[AI-PTT-VALIDATION.md](AI-PTT-VALIDATION.md)。下文保留各历史检查点的说明。

实物约束（2026-11-03确认）：只有GPIO15主按键，没有BOOT/RST实体按键。
GPIO15上电按住用于配网，不是芯片下载按键。烧录使用原生USB-Serial/JTAG自动下载，
需要重新上电时拔插USB。USB断连恢复步骤见 [WSS-ECHO-VALIDATION.md](WSS-ECHO-VALIDATION.md)。

本板型位于小智主工程中，通过唯一的 `DECLARE_BOARD(StubuddyBoard)` 创建，复用原有 `Board`、`AudioCodec::OutputData()` 接口。
本次只验收启动和一次本地提示音。独立诊断工程已验收的PTT、灯效、配网随后逐项迁移。

3.1已由用户完成三次重新上电验收，提交为 `dac5504`。
当前新增的3.2按键录放使用独立变体 `stubuddy-s3-n16r8-ptt`，见 [PTT-VALIDATION.md](PTT-VALIDATION.md)。
本页保留3.1启动变体的操作说明；3.2仍保持LED熄灭，灯效迁移在3.3进行。

3.2已由用户验收并提交 `e6e553c`。当前3.3新增 `stubuddy-s3-n16r8-ptt-led`，
见 [LED-VALIDATION.md](LED-VALIDATION.md)：回放结束后无论按键是否仍按住，一律熄灯；仍需先松键再重新按下才能录音。

3.3已验收并提交 `e8ec267`。当前3.4新增 `stubuddy-s3-n16r8-wifi-ptt`，见
[WIFI-VALIDATION.md](WIFI-VALIDATION.md)：迁移已验收的SoftAP页面及历史记录，网络等待暂停新录音，已经开始的一轮录放完整结束。

3.4配网和3.4.2按键打断接续收音、满60秒灭灯交接已由用户验收并提交 `f5675ba`，见
[PTT-INTERRUPT-VALIDATION.md](PTT-INTERRUPT-VALIDATION.md)。正式AI端云取消及旧音频过滤在私有WS阶段验收。

当前3.5新增 `stubuddy-s3-n16r8-wifi-opus-ptt`：在设备内增量Opus编码/解码回放，
启动执行不出声的编解码延迟自检；操作和真机验收见 [OPUS-VALIDATION.md](OPUS-VALIDATION.md)。
3.5已于2026-11-03由用户完成真机验收：首尾清楚、采样数一致、errors=0，60秒自动回放、按键打断、配网与断网恢复及连续十轮正常，结束无杂音。
此前四个验证变体继续保留，3.4回退变体仍为 `stubuddy-s3-n16r8-wifi-ptt`。

3.5已提交 `a49b3ff`。当前3.6.1新增 `stubuddy-s3-n16r8-ws-echo`，
配套Windows私有回声服务位于工作区的stubuddy/server/ws-echo；两端校验真实音频往返后才播放。
本地配置、编译烧录和验收见 [WS-ECHO-VALIDATION.md](WS-ECHO-VALIDATION.md)。
3.6.1已于2026-11-03通过用户真机验收：帧数、CRC和采样数一致，errors=0；60秒自动回放、等待/回放打断、服务重启、断网恢复及连续十轮均正常，无异常、无杂音，已提交5df51a4。

当前3.6.2新增 `stubuddy-s3-n16r8-wss-echo`：联网校时、严格TLS验证、独立云端Token，
配套ECS部署前检查和IP证书续期。操作见 [WSS-ECHO-VALIDATION.md](WSS-ECHO-VALIDATION.md)。
本机软件验证通过；ECS部署、证书续期演练和电脑公网WSS自测PASS。2026-11-03用户在成功烧录后反馈测试成功，已提交`6176ac9`。

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

2026-11-05按用户要求，将固定提示音和AI/录音回放统一改为70% PCM16满幅峰值（约±22937），共享板级`audio_levels.h`；归一化增益和AI律动参考同步调整，合并收尾按键/打断记忆等待真机复验。通过单声道AudioCodec送入32位立体声总线：左槽为零、右槽为声音。
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

Ctrl+]退出串口监视。端口占用时先关闭其他串口工具；连接失败时先拔插USB并重新枚举COM端口，使用USB自动下载。当前实物没有BOOT/RESET按键。
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
