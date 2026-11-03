# 检查点3.3：按键录放状态LED

3.2已经用户真机验收并提交 `e6e553c`。本次新增 `stubuddy-s3-n16r8-ptt-led`，
保留3.1启动变体和3.2无灯效录放变体。沿用原来的接线和录放流程。

3.3已由用户完成灯效、60秒自动回放、松键保护和连续十轮验收，结束无杂音，已提交 `e8ec267`。
当前3.4配网录放验收见 [WIFI-VALIDATION.md](WIFI-VALIDATION.md)；本页保留3.3无网络变体的说明。

## 功能规则

| 场景 | GPIO18白色LED |
| --- | --- |
| 上电初始化、无操作待机 | 熄灭 |
| 一次开机提示音 | 常亮，结束熄灭 |
| 有效按下按键、录音中 | 常亮 |
| 松键停止录音、分析并准备回放 | 熄灭 |
| 正在回放 | 常亮 |
| 满60秒 | 停止收音并熄灯，随后自动回放并常亮，无需松键 |
| 回放结束，按键已松开 | 熄灭，返回待机 |
| 回放结束，按键仍按住 | 同样熄灭并保持熄灭；等待松键，再次按下才开始新录音 |
| 回放期间按键 | 不打断、不排队；回放常亮，结束熄灭 |
| 录放故障 | 停止录放后显示周期性三连闪，复位后再测 |

按键维持30ms消抖，录音不足300ms不回放；满60秒自动回放、回放后松键保护与录音清除均保持原规则。
不足30ms的触碰可能完全不触发灯光；超过消抖但不足300ms的误触可能短暂亮灯，随后丢弃录音。

回放结束后日志可能显示 `WAIT RELEASE`：这是内部的松键保护，外部表现仍为熄灯待机。
即使一直按住也不会重新亮灯或继续保存录音。松开后出现 `IDLE`，再重新按下才能开始新一轮。
待机PDM仍按已验收的3.2方式读取临时块后丢弃，不保留待机录音。

预留的模型等待、配网等待及连接中状态使用两秒呼吸灯，只有错误使用闪烁，错误优先级最高。
本检查点不启动网络或模型，也不插入模拟等待。呼吸曲线/错误优先级由主机测试覆盖；
配网灯效在3.4实测，模型等待灯效接入模型时再测。普通真机验收无需人为制造硬件故障。

## 实现边界

- GPIO18低电平点亮，LEDC启用输出反相：逻辑亮度0对应GPIO高电平熄灭。
- 使用20kHz PWM，每20ms更新。灯效定时器不分配录音内存、不等待音频锁、不延迟录放任务。
- 板型先将GPIO18安全拉高，再由LEDC接管一次引脚所有权，避免GPIO18重复占用警告。
- 不设置ESP-IDF6.1已废弃的LEDC `intr_type` 字段。
- 开机提示音通过可选指示回调接入；3.1调用不带回调，3.2使用空指示器，保留原有表现。
- 变体名通过IDF构建属性传入早期依赖解析，保证LED源码与LEDC依赖同时选择；旧变体不引入LEDC。
- 本变体仍不构建屏幕、摄像头、字体、BLE或Wi-Fi组件；NVS分区与烧录范围保持原样。

## 电脑PowerShell：编译和核对

若旧串口监视仍在运行，先用Ctrl+]退出，然后执行：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'

python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ptt-led
if ($LASTEXITCODE -ne 0) { throw '编译失败，请停止烧录并反馈错误日志。' }

python scripts/verify_stubuddy_startup.py --variant ptt-led
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请反馈 FAIL 行。' }

python -m serial.tools.list_ports
```

预期 `Project build complete`；核对全部PASS，首行包含 `stubuddy-s3-n16r8-ptt-led`。
核对也会检查PTT/LED开关、LED驱动源码与LEDC组件，防止误烧3.2无灯效变体。

## 烧录和关键日志

找到开发板串口，将下面COM4替换为实际端口：

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

使用这个常规flash命令；无需擦除Flash，也不使用merged-binary整片写入。
NVS仍为0x9000+0x6000，应用从0x10000开始；本变体不初始化、读取或改写原有配网记录。

启动日志应包含：

```text
STUBUDDY_PTT: CHECKPOINT 3.3; ... variant=stubuddy-s3-n16r8-ptt-led
STUBUDDY_LED: GPIO18 ready: active-LOW, PWM=20kHz, update=20ms; IDLE/OFF
STUBUDDY_LED: PLAYBACK/ON
STUBUDDY_STARTUP: TONE START 1/1: 660 Hz; ...
STUBUDDY_LED: IDLE/OFF
STUBUDDY_PTT: PTT CONFIG: ... LED=stateful
STUBUDDY_PTT: WAIT RELEASE: ... LED OFF
STUBUDDY_PTT: IDLE: ... LED OFF
```

正常录放的灯效日志应按以下顺序出现，中间夹有录放采样等日志：

```text
STUBUDDY_LED: RECORDING/ON
STUBUDDY_PTT: REC START: ...
STUBUDDY_LED: IDLE/OFF
STUBUDDY_PTT: REC STOP: ...
STUBUDDY_LED: PLAYBACK/ON
STUBUDDY_PTT: REPLAY START: ...
STUBUDDY_LED: IDLE/OFF
STUBUDDY_PTT: REPLAY STOP: amplifier OFF; LED OFF
STUBUDDY_PTT: WAIT RELEASE: ... LED OFF
```

灯光更新允许约20ms的调度延迟。播放后的静音收尾与关闭功放仍按3.2执行。

## 真机验收

1. 不按键上电：初始化灯灭；提示音期间常亮；之后熄灭，待机30秒不自行亮灯。
2. 按住说话3～5秒：录音常亮；松开后准备期间熄灭；回放常亮；结束熄灭且安静。
3. 快速轻点：不足300ms不回放，不会卡在常亮状态。
4. 按住超过60秒且不松手：自动停止收音并熄灯，自动回放并常亮；日志显示 `duration=60000 ms, reason=LIMIT`。
5. 继续按住直到这一分钟回放结束，再保持按住至少10秒：LED始终熄灭，不自动录第二次。松开后仍熄灭，再按才亮灯录音。
6. 普通回放期间按下并一直按到回放结束：同样结束即熄灭；先松开、再按才能重新录音。
7. 回放期间按下后提前松开：回放不中断、不追加录音；结束熄灭。
8. 连续10轮短录放：灯效顺序一致，人声清晰，结束无杂音、无异常复位或资源持续下降。
9. 按住键上电：提示音后熄灭，等待松手；松开并重新按下才亮灯录音。再完成三次上电检查。
10. 日志无GPIO10/GPIO18占用冲突、I2S/LEDC错误、内存不足或panic。

一分钟录音回放本身也需要约一分钟，另有少量分析和静音收尾时间。
以上灯光和音质需要用户真机判断；软件测试不能代替硬件验收。

## 异常处理与回退

- 仍显示CHECKPOINT 3.2且所有状态灯灭：核对变体名，重新执行完整的build.py和`--variant ptt-led`核对命令。
- 日志是RECORDING/ON而实际灯灭，或IDLE/OFF实际亮：反馈从上电开始的日志和实际灯态，不先改接线或反复改电平。
- 回放后按住导致灯亮：反馈REPLAY STOP至下一次IDLE的日志，属于本检查点必须修复的问题。
- 出现ERROR/TRIPLE-FLASH：同时查前面的FAILED/I2S错误；保留日志后复位再测。
- LED初始化失败会停止进入录放，检查最早的PWM或LED timer错误。
- 端口被占用时先Ctrl+]关闭其他monitor或退出其他串口工具；烧录连接失败时拔插USB、重新枚举COM端口并使用USB自动下载。当前实物没有BOOT/RST按键。

需要回3.2进行对照时，在同一主工程目录执行：

```powershell
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ptt
if ($LASTEXITCODE -ne 0) { throw '3.2编译失败。' }
python scripts/verify_stubuddy_startup.py --variant ptt
if ($LASTEXITCODE -ne 0) { throw '3.2核对失败。' }
idf.py -p $stubuddyPort -b 115200 flash monitor
```

## 软件验证记录

- 主机测试119项，117项通过、2项因本机编译器/开发头文件条件跳过。新增灯效规则与原有按键/PCM测试通过。
- 新灯效测试覆盖持续按住的回放结束门禁、松键抖动、重新按下、常亮/熄灭、呼吸曲线与三连闪边界、错误优先级。
- 普通bread-compact-wifi ESP32-S3 Wi-Fi/OLED构建通过，使用独立build/compat-bread目录。
- ESP-IDF6.1规范构建与身份核对全部通过：3.1为205216字节，3.2为209392字节，3.3为219328字节，均小于1MiB应用分区。
- 3.3构建无新增编译警告；C/C++格式、Python语法和Git空白检查通过。当前构建已停在3.3，真机验收待用户执行。

## 真机通过后Git检查点

以下命令只在用户确认本阶段验收通过后执行：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git status --short
git add -- xiaozhi-esp32-main/xiaozhi-esp32-main stubuddy/firmware/speaker-selftest/DEVELOPMENT-ROADMAP.md
git diff --cached --stat
git commit -m "feat(led): add stubuddy recording and playback status" -m "Verify idle-off, record/playback-on, sixty-second automatic replay transitions, held-key playback completion with LED off, and ten hardware record-replay cycles."
git log -1 --oneline
git status --short
```

暂存区应只有源码、测试和文档，不包含build、sdkconfig、managed_components或tmp。
本阶段实测并提交后，再讨论3.4 SoftAP迁移流程。
