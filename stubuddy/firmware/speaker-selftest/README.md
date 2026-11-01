# stubuddy 扬声器三次自动自检

这是阶段 1 的第一个原子检查点：只验证 ESP32-S3 的 I2S 输出、NS4168 功放与扬声器。
它是同一 Git 仓库中的独立诊断工程，拥有独立的构建目录和配置，不启动小智主程序。
完成真机验收和 Git 提交后，再讨论下一检查点。

## 已确认的测试条件

- 开发环境：ESP-IDF 6.1、其配套 Python 3.13.7 环境。
- 模组：ESP32-S3-WROOM-1-N16R8，16 MiB Flash、8 MiB Octal PSRAM。
- 功放：按用户指定的 5V VDD 条件测试；测试不代表数字输入电平裕量已经合格。
- 声道：用户确认采用右声道，GPIO10 高电平选择右声道并使能，低电平关闭功放。
- 不采集麦克风，不启动 Wi-Fi、Bluetooth/BLE、屏幕、摄像头或 NFC。

NS4168 规格书给出的数字输入高电平下限为 `0.7 × VDD`，5V 时算得 3.5V。
ESP32 的 3.3V 信号不能据此视为满足保证条件。因此无声时不能只归因于程序或扬声器，
能响也不能据此认定量产电气条件合格。本检查点按已确认条件记录实测结果。
左声道需要 CTRL 约 0.9～1.15V 的外部偏置，不能通过把 GPIO10 拉低来选择。

## 接线与输出格式

沿用现有接线；这里的编号是 GPIO 编号，不是模组封装脚号。

| 信号 | GPIO |
| --- | --- |
| NS4168 BCLK | 46 |
| NS4168 LRCLK | 9 |
| NS4168 SDATA | 8 |
| NS4168 CTRL | 10 |

I2S1 使用标准 Philips I2S：16 kHz 采样率、左右各 32 位的时隙。
左时隙始终为零，右时隙输出音调，预期 LRCLK 为 16 kHz、BCLK 为 1.024 MHz。
I2S0 留给后续单独验收的 PDM 采集。

音调峰值固定为约 3% 数字满幅，不读取或写入用户音量。
这是数字幅度，不表示旋钮百分比或实际声压。

## 上电流程

1. GPIO10 保持低电平，初始化 I2S，预装静音数据。
2. 检查实际 Flash 和 PSRAM 容量，等待两秒。
3. 先发送静音，再使能功放。
4. 播放 440Hz 180ms、静音 80ms、660Hz 180ms，组成一次“低音 → 高音”。
5. 每组结束后间隔两秒，共播放三组；音调首尾各有 10ms 淡入淡出。
6. 发送尾部静音，等待 DMA 数据排空，拉低 GPIO10，关闭 I2S。
7. 保持等待，复位或重新上电可重测。错误时停止，不自动重试。

串口 `DONE` 只表示程序完成数据发送与关闭流程，不表示软件能确认扬声器有声音。

## 编译

沿用已经能运行 `idf.py --version` 的 ESP-IDF PowerShell 终端，逐条执行：

```powershell
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
$env:PYTHONUTF8 = '1'
idf.py --version
idf.py build
```

第一条版本检查预期为 `ESP-IDF v6.1`。
构建结束预期包含 `Project build complete`，并生成：

```text
build/stubuddy_speaker_selftest.bin
```

这个工程默认目标已经固定为 ESP32-S3，第一次构建会自动生成配置。
`build/`、`sdkconfig` 等生成物已通过本工程的 `.gitignore` 排除。

如果提示找不到 `idf.py`，用 VS Code 命令面板重新打开 **ESP-IDF: Open ESP-IDF Terminal**。
如果这台电脑的终端环境未加载，也可以在 PowerShell 中先执行已核对的环境脚本：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
```

然后重新执行上面的编译步骤。环境路径与本机安装一致时才使用此脚本。
编译失败时，保留从第一个 `error` / `FAILED` 开始的完整错误和末尾日志。

## 确认串口并烧录

连接 USB 数据线，执行：

```powershell
python -m serial.tools.list_ports
```

输出会列出实际串口，比如 `COM5`。如果有多个，拔插开发板后再次执行，比较新增的串口。
如果显示没有端口，先确认 USB 线可传输数据，并检查 Windows 设备管理器中的设备状态。

下面以 `COM5` 举例，必须换成上一步确认的端口：

```powershell
idf.py -p COM5 -b 115200 flash monitor
```

这会用自检程序替换开发板当前程序。需要恢复时重新烧录相应固件。
成功后会看到写入、校验通过的信息，随后进入日志窗口。
如果端口被占用，关闭其他串口工具后重试。
如果卡在 `Connecting...`，按住 BOOT，按一下 RESET/EN（板上有此键时），松开 BOOT 后重试。
没有 RESET/EN 键时，可以按住 BOOT 插入 USB，松开 BOOT，然后重试。

日志同时输出到 UART0（115200）和原生 USB Serial/JTAG。
若错过上电日志，在监视器中按 `Ctrl+T`、再按 `Ctrl+R` 复位并重测。
按 `Ctrl+]` 退出监视器。

## 预期日志与人工验收

时间戳会变化，关键内容应类似：

```text
STUBUDDY_SPEAKER: stubuddy-s3-n16r8 speaker self-test v0.1.0 / RIGHT channel
STUBUDDY_SPEAKER: I2S1 ready: 16000 Hz, 32-bit L/R slots; LEFT=silence, RIGHT=tone
STUBUDDY_SPEAKER: MEMORY: flash=16 MiB, PSRAM=8 MiB
STUBUDDY_SPEAKER: START in 2 seconds; fixed 3% PCM peak; no networking or recording
STUBUDDY_SPEAKER: CTRL=HIGH: amplifier enabled, RIGHT channel selected
STUBUDDY_SPEAKER: PLAY 1/3: 440 Hz -> 660 Hz
STUBUDDY_SPEAKER: PLAY 2/3: 440 Hz -> 660 Hz
STUBUDDY_SPEAKER: PLAY 3/3: 440 Hz -> 660 Hz
STUBUDDY_SPEAKER: DONE: amplifier off; waiting for reset (audibility is a manual check)
```

| 验收项 | 通过条件 | 实测记录 |
| --- | --- | --- |
| 容量 | 日志为 Flash 16 MiB、PSRAM 8 MiB | 待测 |
| 声音 | 清楚听到三组“低音 → 高音” | 待测 |
| 间隔 | 组间约两秒 | 待测 |
| 完成状态 | 播放后保持安静，无持续杂音、无异常复位 | 待测 |
| 重复启动 | 连续十次重新上电均能正常完成 | 待测 |

有 `DONE` 但无声时，反馈完整日志和听到的现象，先保留固定测试幅度。
出现 `FAIL`、PSRAM 初始化错误、持续爆音或反复复位时，这次验收不通过。

## 本次软件核验记录（2026-11-01）

- 使用本机 ESP-IDF v6.1 和配套 Python 3.13.7 完成完整构建及最终重新配置构建。
- 应用源码以 `-Wall -Wextra -Werror` 编译通过，按上游固件的 C++ 格式配置检查通过。
- 应用二进制为 182256 字节（约 178 KiB），分区大小检查通过。
- 检查实际构建组件：不包含 `bt`、`esp_wifi`、LVGL、LCD、摄像头或视频组件。
- 生成配置确认为 ESP32-S3、16MB Flash、Octal PSRAM、80MHz PSRAM 和双串口日志。
- 尚未烧录设备；表中的声音、电气条件和重复上电项目均等待用户真机验收。

## 验收通过后保存检查点

只有在用户确认真机测试通过后，才在仓库根目录逐条执行：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git status --short
git add -- stubuddy/firmware/speaker-selftest
git diff --cached --stat
git commit -m "feat(audio): add three-pass speaker self-test"
git status --short
```

暂存列表应只包含本自检工程的源码、配置和说明文件，不应包含构建生成物。
最后工作区无输出，表示这些改动已记录。
若提示未设置 Git 身份，设置你自己的 `user.name` 和 `user.email` 后重试提交。
有其他改动时保留它们，确认本次提交没有混入无关文件。

## 参考

- 项目硬件引脚表：`../../esp32s3_n16r8.md`。
- 原始芯片规格书：`../../meta_machine/` 中的主控和 NS4168 文件。
- [ESP-IDF 6.1 I2S API](https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32s3/api-reference/peripherals/i2s.html)。
