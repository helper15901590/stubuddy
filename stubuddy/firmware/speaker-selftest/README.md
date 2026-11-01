# stubuddy 按住录音、松开回放自检 v0.3.0

基线：扬声器三组提示音验收提交 `c19755c`；五秒录音回放验收提交 `abe579f`。
当前原子检查点：GPIO15 按住录音、松开回放，连续操作无需重新上电。
软件检查不代替本次按键真机验收。本工程仍是独立硬件诊断工程，不启动小智主程序。

## 硬件与行为

| 功能 | 配置 |
| --- | --- |
| 环境 | ESP-IDF 6.1，配套 Python 3.13.7 |
| 主控 | ESP32-S3-WROOM-1-N16R8，16 MiB Flash、8 MiB Octal PSRAM |
| 主按键 | GPIO15，输入上拉，按下接地，按下/松开均需稳定30ms |
| 扬声器 I2S1 | BCLK=GPIO46、LRCLK=GPIO9、SDATA=GPIO8 |
| NS4168 CTRL | GPIO10 高电平使能右声道，低电平关闭 |
| 麦克风 I2S0 | PDM CLK=GPIO3、DATA=GPIO42，沿用左采集时隙 |
| 录音格式 | 16kHz、16位单声道 |
| 录音范围 | 少于300ms丢弃，最长10秒 |
| 缓存 | 一次分配320000字节PSRAM，逐轮复用并清除已用录音 |

仍按已确认的 NS4168 5V VDD 条件测试；数字输入电平裕量未作为量产电气验收通过。
扬声器右声道和麦克风采集时隙是独立配置。音量沿用上个验收版本：去直流，
峰值约3%数字满幅、增益最多8倍、首尾10ms淡入淡出，不修改用户持久化音量。
没有联网、上传、Flash录音、屏幕、摄像头或蓝牙。

## 功能流程

1. 上电检查容量，等待两秒，播放一次就绪提示音，关闭功放。
2. 麦克风预热200ms；待机期间持续读取并丢弃采样，避免按下时预热丢失人声开头。
3. 等按键稳定松开，输出 `IDLE`，接受下一次按下。
4. 确认按下后开始将声音保存到PSRAM；功放始终关闭。
5. 确认松开时停止录音。少于300ms则打印 `DROP`，清除录音并准备下一轮。
6. 到十秒时自动停止采集，打印 `LIMIT`；持续按住不会继续录音，松开才开始回放。
7. 有效录音自动回放一次。回放期间的按下和松开均不排队执行。
8. 关闭功放、清除这轮录音。再次预热并确认按键松开后，返回 `IDLE`。

如果按键从回放期间一直按到回放结束，停在 `WAIT RELEASE`，松开后才返回待机。
上电时一直按住也是如此，避免意外触发。只有看到下一次 `IDLE` 后的新按下才录音。
本模式正常运行不会出现上一个自动测试的 `DONE` 或 `Returned from app_main()`。
错误时打印 `FAIL` 并停止，不自动重试。

## 电脑 PowerShell 编译与烧录

逐条执行，无需 VS Code 插件：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
idf.py --version
idf.py reconfigure build
python -m serial.tools.list_ports
```

版本预期为 `ESP-IDF v6.1`，编译结束为 `Project build complete`。
新增按键模式默认开启，重新配置后应生成 `CONFIG_STUBUDDY_BUTTON_PTT=y`。
确认开发板端口，例如COM4，然后执行（端口不同请替换）：

```powershell
idf.py -p COM4 -b 115200 flash monitor
```

烧录会替换设备当前程序。端口占用时关闭其他串口工具再试。
停在 `Connecting...` 时按住BOOT，按RESET/EN再松开BOOT；没有复位键则按住BOOT插USB、松开后重试。
日志窗口按 `Ctrl+T` 再按 `Ctrl+R` 可复位；按 `Ctrl+]` 退出。

## 预期日志

```text
stubuddy-s3-n16r8 audio self-test v0.3.0 / BUTTON-PTT / speaker RIGHT
WAIT RELEASE: release GPIO15 before the next recording
IDLE: hold GPIO15 to record; release to replay (0.3-10 seconds)
REC START: round=1; amplifier OFF
REC STOP: samples=..., duration=... ms, reason=RELEASE; microphone OFF
REPLAY START: round=1; button presses ignored until next IDLE
CAPTURE: samples=..., DC=..., peak=..., RMS=..., clipped=..., gain=...
REPLAY STOP: amplifier OFF
ROUND DONE: 1; internal_free=..., PSRAM_free=...
WAIT RELEASE: release GPIO15 before the next recording
IDLE: hold GPIO15 to record; release to replay (0.3-10 seconds)
```

误触时应看到 `DROP` 而没有这轮的 `REPLAY START`；极短、未稳定30ms的触碰可能完全没有录音日志。
十秒上限时应为 `samples=160000, duration=10000 ms, reason=LIMIT`。

## 人工验收

| 测试 | 操作与预期 |
| --- | --- |
| 待机 | 不按键，不自动录音或回放 |
| 基本录放 | 看到IDLE后按住说一句话，松开自动回放一次，时长随按住变化 |
| 误触 | 快速轻点约0.1秒，不回放；可能有DROP或无录音事件 |
| 十秒上限 | 持续按住超过十秒，看到LIMIT后再说一句话；松开后回放不包含这句后说的话 |
| 回放期间按键 | 回放时轻点不启动下一轮；按住跨过回放结束时等待松开，不自动录音 |
| 开机按住 | 按住GPIO15再上电，等待松开后才进入IDLE，不自动录音 |
| 连续操作 | 完成十轮录放，无异常复位、无持续杂音；检查ROUND DONE内存是否持续下降 |

按键需使用GPIO15的业务按键，BOOT/IO0是下载键。无反应时反馈完整启动日志与按下时的日志。
回放无声或噪声时一并反馈CAPTURE统计，先保持当前已验收的音频配置。

## 回测旧模式

通过PowerShell执行 `idf.py menuconfig`，进入 `stubuddy audio diagnostics`：

- 关闭 `Hold GPIO15 to record, release to replay`：恢复自动五秒录音回放。
- 再关闭 `Enable microphone record and replay diagnostics`：恢复三组扬声器提示音。
- `Use the right PDM microphone slot` 仅控制麦克风时隙，默认保持关闭。

更改后保存配置，再编译烧录。旧模式是回归入口，本次主要验收默认按键模式。

## 自动检查与 Git 检查点

`tests/ptt_button_test.cc` 使用与设备相同的按键类，在编译期执行消抖与松开门控测试，
覆盖30ms边界、接点抖动、开机按住、回放期间操作和32位毫秒计数回绕。
这些测试随默认工程一起编译，不占用运行期录音缓存；失败会阻止构建。

2026-11-01：软件完整编译、分区检查和按键逻辑检查已通过。
还验证了测试能拒绝故意注入的30ms边界错误。本次物理按键、采音质量和连续操作等待用户验收。

用户确认验收通过后才执行：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git add -- stubuddy/firmware/speaker-selftest
git diff --cached --stat
git commit -m "feat(audio): add hold-to-record button self-test" -m "Verify GPIO15 debounce, short-tap filtering, ten-second limit, and repeated record-replay cycles on hardware."
git status --short
```

提交列表应只包含本次自检工程源码、测试和说明，不含构建生成物或无关文件。
