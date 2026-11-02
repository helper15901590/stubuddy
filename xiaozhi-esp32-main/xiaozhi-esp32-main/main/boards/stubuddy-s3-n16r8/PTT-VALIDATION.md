# 检查点3.2：主工程按键录音与回放

3.1用户实测通过并已提交 `dac5504`。本次新增独立变体 `stubuddy-s3-n16r8-ptt`，
在小智主工程的 Board / AudioCodec 接口中迁移已验证的本地PTT流程。
启动变体 `stubuddy-s3-n16r8-startup` 继续保留。

3.2已由用户完成全部按键流程和连续十轮真机验收，结束无杂音，提交为 `e6e553c`。
本页保留3.2无灯效变体说明；当前3.3灯效验收见 [LED-VALIDATION.md](LED-VALIDATION.md)。

## 本次功能流程

```text
上电 → 检查N16R8 → 一次提示音 → PDM预热200ms
     → 等待按键松开并稳定30ms → IDLE
     → 按住GPIO15 → 录音（16kHz PCM16，单声道）
     → 松开 / 收满60秒 → 先关闭麦克风
       ├─ 不足300ms：丢弃，不回放
       └─ 300ms～60秒：去直流、限幅、淡入淡出 → 回放一次 → 关闭功放
     → 清除本轮录音 → 重新等待松键 → IDLE
```

- 满60秒自动停止收音并回放，即使按键仍按住；缓冲区最多960000采样、1920000字节。
- 回放期间的按键不排队，也不打断回放。回放后仍按住时必须先松开，再重新按下才会录音。
- 按键30ms消抖，少于300ms的录音被过滤；判断以日志中的采样时长为准。
- 始终复用一块PSRAM缓冲区。音频仅在内存中处理，不写Flash、不上传；本轮处理结束清除已用录音。
- 待机时PDM持续读取到一个10ms临时块后丢弃，保证按下后能及时开始采集；待机不保留录音。
- 收音与放音在本检查点顺序运行。下一步验证Opus、WS后再接入云端对话。
- GPIO18本阶段始终熄灭。灯效迁移单独在3.3验收；本版也不启动Wi-Fi、BLE、屏幕、摄像头或OTA。
- 任一I2S读写/关闭失败会停止PTT，尝试关闭功放与麦克风，日志输出FAILED，需要复位后再测。

保持此前接线：PDM CLK=GPIO3、DATA=GPIO42、LEFT槽；按键GPIO15低有效、内部上拉；
扬声器BCLK=46、LRCLK=9、DATA=8、CTRL=10，高电平使能右声道。
提示音与回放峰值沿用已验收的约3% PCM16满幅上限；回放去直流、增益最多8倍、首尾10ms淡入淡出。

## PowerShell编译和核对

先退出旧串口监视（Ctrl+]）。以下是小智主工程目录，注意不是独立speaker-selftest目录。

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ptt
if ($LASTEXITCODE -ne 0) { throw '编译失败，请停止烧录并反馈错误末尾日志。' }
python scripts/verify_stubuddy_startup.py --variant ptt
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请停止烧录并反馈 FAIL 行。' }
python -m serial.tools.list_ports
```

预期：`Project build complete`；核对脚本全部PASS，首行应为 `stubuddy-s3-n16r8-ptt`。
核对项目包含PTT模式、唯一入口/板型工厂、无屏幕/摄像头/BLE/Wi-Fi依赖、应用分区及烧录范围。
若提示Wrong startup/PTT mode，请使用上面的完整build.py命令，不要只执行idf.py build。

## 烧录与日志

串口列表找到开发板端口，将下面示例COM4替换为实际端口。

```powershell
idf.py -p COM4 -b 115200 flash monitor
```

只执行常规flash命令，不要erase-flash或写merged-binary。NVS分区仍为0x9000+0x6000，
应用0x10000；本验证入口不初始化NVS，也不读写配网记录。
构建脚本最后虽会生成合并镜像，本次不使用该镜像烧录。

启动关键日志：

```text
STUBUDDY_PTT: CHECKPOINT 3.2; ... variant=stubuddy-s3-n16r8-ptt
STUBUDDY_STARTUP: TONE START 1/1: 660 Hz; ...
STUBUDDY_PTT: PTT CONFIG: max=60 seconds, buffer=1920000 bytes; timeout=auto-replay; LED OFF
STUBUDDY_CODEC: INPUT ON: I2S0 PDM CLK=3 DATA=42; 16000 Hz PCM16 mono, LEFT slot
STUBUDDY_PTT: WAIT RELEASE: release GPIO15 before the next recording
STUBUDDY_PTT: IDLE: hold GPIO15 to record; release to replay (0.3-60 seconds); LED OFF
```

正常录放应依次出现：

```text
REC START: round=1
INPUT OFF: I2S0 released
REC STOP: ... reason=RELEASE; microphone OFF
CAPTURE: ...
REPLAY START: round=1
OUTPUT OFF: CTRL=LOW; I2S1 released
REPLAY STOP: amplifier OFF; LED OFF
ROUND DONE: 1; ... recording erased
WAIT RELEASE: ...
IDLE: ...
```

## 真机验收

| 操作 | 预期 |
| --- | --- |
| 不按键上电，等待IDLE | 只有一次提示音，之后安静；LED保持熄灭 |
| 按住键说话3～5秒，松开 | 人声清晰，只回放一次，结束后无持续杂音 |
| 快速轻点（录音不足300ms） | 日志DROP，不回放这次误触 |
| 一直按住超过60秒，不松手 | samples=960000、duration=60000 ms、reason=LIMIT；无需松键就自动回放 |
| 仍按住直到60秒回放结束 | 停在WAIT RELEASE，不自动录第二次；松开后回IDLE，再按才能录音 |
| 在普通回放期间按下再松开 | 回放不受影响，不追加录音；结束后等IDLE再按 |
| 回放期间按住，一直保持到结束 | 等待松键，再重新按下才开始新录音 |
| 连续10轮短录放 | 无复位、I2S错误、GPIO10冲突或内存不足；ROUND DONE的可用内存无持续下降 |
| 按住键上电 | 仍只提示一次；WAIT RELEASE等待松手，之后重新按键才录音 |

60秒回放也需要约60秒，前后另有少量音频分析和静音收尾时间；这是本地原速回放。
轻点不足30ms可能完全不触发REC START，是正常消抖。
需听声判断清晰度与底噪，构建/主机测试不能代替硬件验收。

## 异常处理与回退

- 没出现CHECKPOINT 3.2：核对目录和变体名，再完整编译/核对/烧录。
- 等待WAIT RELEASE：先松开GPIO15按键，稳定后出现IDLE。
- 端口占用：Ctrl+]退出其他monitor并关闭其他串口工具；下载连接失败沿用已验证的BOOT/RESET方法。
- 看到Very low input、FAILED、panic、看门狗复位或GPIO10冲突：保留从上电到故障的日志反馈，不继续跨阶段修改。
- 需要回3.1：使用以下命令重编译后再烧录，不需要回退或删除源码。

```powershell
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-startup
if ($LASTEXITCODE -ne 0) { throw '3.1编译失败。' }
python scripts/verify_stubuddy_startup.py
if ($LASTEXITCODE -ne 0) { throw '3.1核对失败。' }
idf.py -p COM4 -b 115200 flash monitor
```

## 软件验证与Git检查点

当前软件验证记录：

- ESP-IDF6.1规范构建：3.2 PTT变体209296字节，3.1启动变体205184字节，均在1MiB应用分区内。
- 两种模式分别通过构建核对；PTT镜像会被默认的startup核对拒绝，避免模式混淆。
- 普通bread-compact-wifi ESP32-S3（Wi-Fi/OLED）在独立build/compat-bread目录构建通过，确认原有完整工程路径仍可构建。
- 主机套件运行117项，115项通过、2项因本机编译器/开发头文件条件跳过；新增的消抖、松键门禁、计时回绕、60秒DC与极值PCM测试已通过。
- C/C++格式检查和Python语法检查通过。
- 录音清晰度、按键手感、60秒行为、杂音和连续10轮资源稳定性仍等待用户真机验收。

真机验收通过后再执行以下Git命令；
没有通过时先修复本检查点，不提前进入3.3。

```powershell
Set-Location 'D:\Desktop\stubuddy'
git status --short
git add -- xiaozhi-esp32-main/xiaozhi-esp32-main stubuddy/firmware/speaker-selftest/DEVELOPMENT-ROADMAP.md
git diff --cached --stat
git commit -m "feat(audio): add stubuddy board PTT record and replay" -m "Port validated PDM capture and hold-to-record logic to Board/AudioCodec. Verify short-tap filtering, sixty-second automatic replay, playback release gating, and ten hardware record-replay cycles."
git log -1 --oneline
git status --short
```

暂存区应只有源码、测试和文档，不包含build、sdkconfig、managed_components或tmp。
用户实测确认并提交后，再讨论3.3状态LED迁移流程。
