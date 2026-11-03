# 检查点3.5：本地Opus编解码录放

前置检查点3.4已由用户真机验收并提交f5675ba。3.5流程已确认；本次新增独立变体
`stubuddy-s3-n16r8-wifi-opus-ptt`，保留原wifi-ptt作为PCM回退基线。
本阶段仅设备内编解码，没有ASR、LLM、TTS或云端音频连接。

2026-11-03用户真机验收通过：Opus自检正常，录放首尾清楚，录音与解码采样数一致且errors=0；
60秒自动回放、按键打断、配网与断网恢复均正常，连续十轮无异常，结束无杂音。
以下保留该检查点的可重复验收步骤；下一步先建立3.5 Git检查点，再确认3.6流程。

## 声音与按键流程

1. 开机提示音仍只播放一次，联网提示音沿用3.4。启动后在RAM里用确定的测试信号运行真实Opus编码和解码，不向扬声器输出测试信号。
2. 测量当前编码器配置的有效延迟，相关系数不足时明确报错停止，不使用未经验证的固定延迟常量。
3. 联网就绪后，按住GPIO15录音，LED常亮；16kHz/单声道/16位PCM，每960个采样编码成一个60ms的Opus包。
4. 松开或录满960000个采样后停止PDM收音，确认LED已灭。满60秒保留至少150ms灭灯交接。
5. 补齐最后不足一帧的数据，并在需要时追加零帧以取出编码器尾部。逐包解码，跳过实测延迟，只保留原始采样数量。
6. 回放使用真实解码后的PCM，沿用DC去除、限幅和淡入淡出；不能用原始PCM假装编解码成功。实际放音时LED常亮。
7. 回放中新的有效按下立即停止旧声音、使旧包流失效，继续按住录新一轮，松开后只回放新录音。少于300ms的新录音丢弃，旧声音不续播。
8. 正常回放结束一律熄灯，即使按键仍按住；仍需先松键再按下。旧长按不会打断自己触发的60秒自动回放。

编解码过渡阶段保持停录熄灯；模型等待呼吸灯到接入真实模型时验收。
网络等待暂停新录音；已经获准开始的一轮完整结束，或由用户按键主动打断。网络恢复后沿用松键保护。

## 存储与异常策略

- 继续保留1920000字节PSRAM PCM缓冲，以便与3.4对照；本阶段不宣称已经减少总RAM。
- Opus包存储按编码器返回的最大包长一次分配，最多1001包，覆盖60秒加编解码尾部；每包长度验证，无逐帧堆分配。
- 本地RAM使用两字节包长前缀，它不是WebSocket协议头；3.6应发送包载荷。
- 打断先O(1)使旧包流失效，下一轮重置编码器/解码器，不释放重建句柄；等麦克风关闭后再清除旧数据的最大使用范围。
- 仅本变体使用32KiB主任务栈；日志记录最小剩余栈。只引入esp_audio_codec，显示、字体、摄像头、BLE与ESP-SR保持排除。
- Opus固件超过原1MiB验证分区，仅本变体使用2MiB factory应用分区；NVS仍为0x9000+0x6000，PHY仍为0xf000+0x1000，应用起点仍为0x10000。此前四个变体继续使用原1MiB分区。
- 编码/解码出错、包越界、输出采样数不符或PDM接收队列溢出时明确失败并关闭音频，不能悄悄丢音或退回原始PCM。
- AUTO码率、AUDIO模式、complexity=0、VBR开、DTX/FEC关。静音帧仍逐帧保留，最终按真实采样数裁剪。
- 打印原始/Opus字节数、包数、解码采样数、延迟、编解码耗时和错误数；实际压缩率取决于输入声音。

## PowerShell编译与构建核对

若旧串口监视器仍开着，先Ctrl+]退出。以下逐步执行：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'

idf.py --version
python --version

python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wifi-opus-ptt
if ($LASTEXITCODE -ne 0) { throw '编译失败，请保留错误日志，不要继续烧录。' }

python scripts/verify_stubuddy_startup.py --variant wifi-opus-ptt
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请反馈FAIL行，不要继续烧录。' }

python -m serial.tools.list_ports
```

预期ESP-IDF v6.1、Python 3.13.7，编译显示Project build complete，核对脚本全部PASS，
包含Opus encoder and decoder linked，且排除了屏幕、摄像头、BLE、字体。

## 烧录与日志

将COM4改成上一步实际显示的开发板端口：

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

使用普通flash写入bootloader、分区表和应用，保留NVS中的配网历史。不要执行erase-flash或使用merged-binary整片写入。
如需重开监视器且不复位：

```powershell
idf.py -p $stubuddyPort monitor --no-reset
```

关键日志（数值以实际为准）：

```text
STUBUDDY_PTT: CHECKPOINT 3.5; ... variant=stubuddy-s3-n16r8-wifi-opus-ptt
STUBUDDY_OPUS: CONFIG: 16000Hz mono PCM16; frame=60ms/960 samples; ...
STUBUDDY_OPUS: SELFTEST PASS: silent RAM encode/decode; delay=... samples; correlation=...; errors=0
STUBUDDY_PTT: IDLE: hold GPIO15 to record; ...
STUBUDDY_PTT: REC STOP: samples=... duration=...; microphone OFF
STUBUDDY_OPUS: LOOPBACK: round=...; raw=... bytes; opus=... bytes; packets=...; decoded_samples=...; delay=...; errors=0
STUBUDDY_OPUS: TIMING: enc_avg=... us; enc_max=... us; dec_max=... us; stack_min_free=...
STUBUDDY_PTT: REPLAY START: ...
STUBUDDY_PTT: ROUND DONE: ... internal_free=... PSRAM_free=... recording erased
```

核对每轮decoded_samples与REC STOP的samples相等；60秒应为960000，不能多回放补零的尾部。
打断时包含REPLAY INTERRUPTED、OUTPUT ABORT、OPUS DISCARD、INPUT HANDOFF与下一轮REC START。
RAM自检通过不能代替扬声器实际音质与按键手感验收。

## 真机验收清单

| 测试 | 操作 | 通过标准 |
| --- | --- | --- |
| 上电 | 连续断电重上电三次 | 开机提示音每次仅一次，联网提示音正常，RAM自检不出声；SELFTEST PASS，网络恢复后IDLE，结束无底噪 |
| 短句 | 录2～5秒，说“第一句话，最后一个字是好” | 首字、末字清楚，语速正常，无明显多余静音；samples与decoded_samples相等 |
| 非整帧 | 多次录0.4秒、1.1秒及随意长度 | 尾字完整，不因不足60ms一帧而截尾或卡住 |
| 误触 | 小于300ms轻触 | DROP且不回放，恢复待机 |
| 静音与停顿 | 录5秒安静环境，再录有中间停顿的一句话 | 无异常爆音、不把停顿剪掉，回放长度正常，结束安静 |
| 60秒 | 持续按住超过一分钟 | 自动停录、确认灭灯交接、再常亮回放；samples与decoded_samples均为960000，旧长按不打断 |
| 打断 | 回放中重新按住并立即说“第二段开头”，然后松开 | 旧声音立即停止，首字清楚，只回放新段落 |
| 多次打断 | 连续三次打断接续录音，最后正常结束 | 无旧片段混入、无旧回放恢复，无I2S错误，最后熄灯 |
| 松键保护 | 录满60秒后始终保持原长按，直到自动回放结束 | 原长按不触发打断；结束后熄灯，不自动再录；松开再按才录音 |
| 连续十轮 | 混合短录放与按键打断 | 无复位、资源耗尽、持续内存下降、杂音或CODEC ERROR；errors保持0 |
| 网络回归 | 测正常重连、强制配网、输错密码、录放中断网后恢复 | 保留3.4门户/历史/3秒关闭热点/错误音/呼吸灯/录音门禁行为 |

若出现自检失败、首尾缺字、回放时长异常、PDM DMA queue overflow、CODEC ERROR或首字不清，
保留从CHECKPOINT 3.5或REC START到异常后的日志，留在3.5继续修复。

## 软件验证记录

- 主机测试共124项：122通过，2项因主机编译器命令/开发头文件条件跳过。网页回归包含26个Node测试用例。
- 新增主机断言覆盖有界包存储、非法长度、旧长录音被新短录音替换后的擦除、1001包上限、末帧与尾部补齐、实测延迟裁剪和60秒采样数/缓冲边界。
- startup、ptt、ptt-led、wifi-ptt四个旧变体已规范构建并通过核对，大小仍分别为205376、209568、219824、874544字节，未链接Opus。
- wifi-opus-ptt最终固件1055600字节，已完成规范构建及链接/分区核对，只有它链接Opus编解码器并使用2MiB应用分区；NVS地址保持不变。当前build目录已恢复为此变体。
- 普通ESP32-S3 Wi-Fi/OLED板型bread-compact-wifi的完整应用构建通过；裁剪条件未阻断普通小智工程。此项仅软件兼容构建，没有进行该板型真机测试。
- C++格式和Git空白检查通过。设备上的RAM编解码自检、实际音质、首尾、打断手感和连续十轮资源表现仍须由用户按上表验收。

## 回退3.4

退出monitor后，在同一工程目录执行：

```powershell
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wifi-ptt
if ($LASTEXITCODE -ne 0) { throw '回退版本编译失败。' }
python scripts/verify_stubuddy_startup.py --variant wifi-ptt
if ($LASTEXITCODE -ne 0) { throw '回退版本核对失败。' }
idf.py -p $stubuddyPort -b 115200 flash monitor
```

3.5全部真机验收通过后再建立Git检查点，之后讨论3.6私有WS回声通道。
