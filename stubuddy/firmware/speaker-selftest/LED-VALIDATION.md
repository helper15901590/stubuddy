# GPIO18 白色LED验证

当前版本v0.4.3，基线为已验收的按键录放提交 `9ea4039`。
本检查点添加灯光反馈，并按用户补充要求将录音上限从十秒延长为六十秒。按键消抖和误触过滤沿用基线；到六十秒上限时停止采集、灭灯，然后自动回放，不再等待松开。

## 规则

- `IDLE/OFF`：待机熄灭。
- `KEY_HELD/ON`：有效按住按键时常亮。到限后有明确例外：先强制灭灯，再进入自动回放。
- `RECORDING/ON`、`PLAYBACK/ON`：录音、录音回放和提示音播放时均常亮。
- `MODEL_WAITING/BREATH`：仅供未来等待模型回复时使用；本轮离线自检不触发、不要求真机测试。
- 无按键的本地准备、预热和等待均熄灭；本地流程不会借用模型等待状态。
- `ERROR/TRIPLE-FLASH`：音频发生错误并停止时，三次100ms短闪，然后暗1.5秒，循环。

灯光使用LEDC定时器0、通道0，20kHz硬件PWM，由独立ESP定时器任务每20ms更新亮度。
根据用户反馈（待机常亮、按住仅微亮），LED默认改为低电平点亮，预留模型等待呼吸采用平方亮度斜坡。
极性使用必选配置，默认低电平，避免旧的未勾选布尔配置继续保留错误的高电平有效设置。
LED控制器为静态生命周期，音频错误或软件演示从app_main返回后，错误灯仍会继续运行。
如果LED初始化本身失败，串口报错，音频测试不启动。

## 编译烧录（电脑 PowerShell）

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
idf.py reconfigure build
python -m serial.tools.list_ports
```

编译应出现 `Project build complete`。确认实际串口，下面以COM4为例，端口不同请替换：

```powershell
idf.py -p COM4 -b 115200 flash monitor
```

预期启动日志：

```text
STUBUDDY_LED: GPIO18 ready: active-LOW, PWM=20kHz, update=20ms
STUBUDDY_LED: IDLE/OFF
STUBUDDY_SPEAKER: stubuddy-s3-n16r8 audio self-test v0.4.3 / BUTTON-PTT / speaker RIGHT
```

## 确认程序已经烧录到设备

`Project build complete` 只代表电脑上的编译完成。推荐确认串口后使用一条命令完成构建、烧录和日志：

```powershell
idf.py -p COM4 -b 115200 reconfigure build flash monitor
```

烧录后设备启动日志必须同时出现：

```text
v0.4.3 / BUTTON-PTT
GPIO18 ready: active-LOW
PTT CONFIG: max=60 seconds, buffer=1920000 bytes
```

一分钟达到上限时应为：

```text
REC STOP: samples=960000, duration=60000 ms, reason=LIMIT; microphone OFF
LIMIT: 60 seconds reached; microphone OFF; LED OFF; automatic replay
```

低电平极性、软件版本或60秒配置不符时，先反馈这些日志，再判断灯光表现。
本版保留移除弃用字段的修正，并将GPIO10输出配置改为只执行一次。
启动时应只出现一次 `CTRL GPIO10 configured once`，连续回放不应再出现GPIO10冲突告警。

## 真机验收

1. 启动提示音结束后，不操作至少十秒，白灯应一直熄灭，不出现呼吸。
2. 按住说话约2～3秒：`RECORDING/ON` 时常亮，松开后 `PLAYBACK/ON` 也常亮，人声清楚。
3. 回放结束、按键已松开时应熄灭，准备下一轮期间不呼吸。松开后准备回放的短暂间隔也是灭灯，真正回放时常亮。
4. 按住超过60秒且不松开：出现 `LIMIT` 后停止采集、LED熄灭；分析音量并准备输出后，自动回放最多60秒且LED常亮。按键是否松开不影响这次自动回放。
5. 回放时按住并保持到结束：灯继续常亮；松开回待机后灯灭，不自动录音。
6. 快速误触不回放，处理完成进入IDLE后灯灭。
7. 连续十轮短录放，确认灯光和人声正常，无异常复位，且不再出现 `conflict found for GPIO[10]`。
8. 如果到限后一直按到回放结束，应等待松开，不自动启动下一次录音；松开再按才进入下一轮。

当前离线测试不应看到 `MODEL_WAITING/BREATH`。呼吸灯的真机验收等接入模型后再进行。
按 `Ctrl+]` 退出监视器；按 `Ctrl+T` 再按 `Ctrl+R` 可复位重测。

## 如果灯光亮灭相反

仅在状态逻辑正确但物理LED仍然亮灭相反时，才调整有效电平。先退出日志窗口，在PowerShell执行：

```powershell
idf.py menuconfig
```

进入 `stubuddy audio diagnostics` → `Status LED on GPIO18 polarity`，选择 `Active-low (stubuddy board)`，保存退出。默认已选此项；其他接线才选择Active-high。
然后重新编译烧录：

```powershell
idf.py reconfigure build
idf.py -p COM4 -b 115200 flash monitor
```

预期启动日志改为 `active-LOW`。如果完全不亮，反馈完整启动日志与实际灯光表现，先保持当前接线。

## 错误灯的可选软件演示

无需制造硬件故障。在PowerShell运行 `idf.py menuconfig`，进入同一菜单，开启：

```text
Show error LED pattern instead of running audio diagnostics
```

保存后重新编译烧录。设备应显示 `LED ERROR DEMO`，跳过音频诊断，循环三连闪。
此模式不会进入正常PTT流程，`Returned from app_main()` 日志不代表错误灯停止。
演示验证后关闭该选项，再编译烧录以恢复正常录放模式。

## 提交前记录

软件完整编译与编译期逻辑检查已通过；本轮验收低电平极性下的灯光、60秒自动回放（不等松开）、GPIO10告警消除和音频回归。模型等待呼吸的真机测试暂缓。
通过后按README中的Git命令保存检查点，不把build或sdkconfig生成物加入提交。
