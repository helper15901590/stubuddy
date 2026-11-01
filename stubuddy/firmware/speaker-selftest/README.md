# stubuddy 本地录音回放自检 v0.2.0

本检查点：上电提示音 → 五秒录音 → 自动回放一次 → 关闭功放等待复位。
沿用同一独立 ESP-IDF 工程；先前的三组提示音已由用户验收，并提交为 `c19755c`。
新版默认进入录音回放模式，尚待本次真机验收。

## 硬件与行为

| 功能 | 配置 |
| --- | --- |
| 开发环境 | ESP-IDF 6.1，配套 Python 3.13.7 |
| 模组 | ESP32-S3-WROOM-1-N16R8，16 MiB Flash、8 MiB Octal PSRAM |
| 功放 | NS4168；按已确认的 5V VDD 测试 |
| 扬声器 I2S1 | BCLK=GPIO46、LRCLK=GPIO9、SDATA=GPIO8 |
| 功放控制 | GPIO10 高电平使能右声道，低电平关闭 |
| 麦克风 I2S0 | PDM CLK=GPIO3、DATA=GPIO42，默认采集左时隙 |
| 录音 | 16kHz、16位单声道；80000 个样本、160000 字节 |
| 存储 | PSRAM；流程结束后释放，不写入 Flash，不上传 |

功放右声道与麦克风 PDM 左/右时隙是两项独立配置。
麦克风时隙必须匹配其 L/R 硬件连接，目前先以左时隙实测。
NS4168 的 5V 供电条件下，3.3V 数字信号的输入电平裕量仍未验收；
前一检查点声音正常不能替代量产电气验证。

## 工作流程

1. 上电检查容量，等待两秒。
2. 以约 3% 数字满幅峰值播放一次短提示音。
3. 尾部静音排空，关闭功放和扬声器 I2S，等待 300ms 余响消退。
4. 启动 PDM 采集，丢弃最初 200ms 数据，使麦克风和转换器稳定。
5. 输出 `REC START`，录制五秒。录音期间功放保持关闭。
6. 停止采集并关闭 PDM，输出 `REC STOP`。
7. 开启右声道，回放这五秒声音一次。
8. 关闭功放和 I2S，释放录音缓存，输出 `DONE`，等待复位。

回放去除直流偏移，以整段录音的峰值计算播放增益：峰值不超过约 3% 数字满幅，
增益上限 8 倍，首尾各淡入淡出 10ms。不会改动持久化用户音量。
低输入电平会打印警告；`DONE` 只表示流程完成，是否清楚听到人声需要人工验收。
初始化、内存分配、采集或播放出错时停止，不自动重试。

## 电脑 PowerShell 编译与烧录

逐条执行：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
idf.py --version
idf.py reconfigure build
python -m serial.tools.list_ports
```

预期版本为 `ESP-IDF v6.1`，构建结束显示 `Project build complete`。
环境脚本是本机已确认的安装路径，不需要 VS Code 插件。
新版配置默认开启录音回放；重新配置后开机日志应包含 `RECORD-REPLAY`。

确认开发板端口，例如 COM4，然后执行：

```powershell
idf.py -p COM4 -b 115200 flash monitor
```

把 COM4 替换为实际端口。烧录会替换设备上的当前程序。
端口被占用时关闭其他串口工具；停在 `Connecting...` 时按住 BOOT，按 RESET/EN 后松开 BOOT再重试；
没有复位键则按住 BOOT 插入 USB、松开后重试。
没有端口时检查数据线和设备管理器。日志同时输出到 UART0 和原生 USB Serial/JTAG。
在日志窗口中按 `Ctrl+T` 再按 `Ctrl+R` 重测，按 `Ctrl+]` 退出。

## 人工验证

提示音结束后说“你好，我是思巴迪”，保持在麦克风前正常说话。
看到 `REC START` 后五秒为有效录音时间，之后应听到自己的声音回放一次。

```text
STUBUDDY_SPEAKER: stubuddy-s3-n16r8 audio self-test v0.2.0 / RECORD-REPLAY / speaker RIGHT
STUBUDDY_SPEAKER: PDM ready: I2S0 CLK=3 DATA=42, 16000 Hz PCM, slot=LEFT
STUBUDDY_SPEAKER: REC START: speak now, 5 seconds; amplifier OFF
STUBUDDY_SPEAKER: REC STOP: 80000 samples / 160000 bytes; microphone OFF
STUBUDDY_SPEAKER: REPLAY START: playing the 5-second recording once
STUBUDDY_SPEAKER: CAPTURE: samples=80000, DC=..., peak=..., RMS=..., clipped=..., gain=...
STUBUDDY_SPEAKER: REPLAY STOP: amplifier OFF; recording buffer released on return
STUBUDDY_SPEAKER: DONE: amplifier off; waiting for reset (audibility is a manual check)
```

验收标准：提示音正常、录音期间没有扬声器播放、完整回放五秒且人声可辨，
语速音调自然，结束安静，无异常复位。重新上电三次确认均能完成。
没有回放或只有噪声时，反馈从启动到 DONE/FAIL 的完整日志，尤其 CAPTURE 统计，先保持当前参数。

如果左时隙实测无有效输入，需要单独核对/试验麦克风时隙。
通过纯命令行 `idf.py menuconfig`，进入 `stubuddy audio diagnostics`，
将 `Use the right PDM microphone slot` 开启，保存后重新编译烧录。
此选项只改变麦克风采集，不改变已验证的扬声器右声道。
同一菜单关闭 `Record five seconds and play back once` 可回测旧的三组提示音。

## 软件核验与提交

2026-11-01：默认录音回放模式已在本机完整编译通过，应用约 185 KiB，分区检查通过。
构建组件仍不包含 Wi-Fi、Bluetooth/BLE、LVGL、LCD 或摄像头。
真实采音质量、麦克风时隙以及重复上电结果等待用户验证。

用户确认通过后，才保存下一 Git 检查点：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git add -- stubuddy/firmware/speaker-selftest
git diff --cached --stat
git commit -m "feat(audio): add five-second PDM record and replay self-test"
git status --short
```

暂存列表应只含本自检工程的源码和文档，不含 build、sdkconfig 或其他无关文件。
