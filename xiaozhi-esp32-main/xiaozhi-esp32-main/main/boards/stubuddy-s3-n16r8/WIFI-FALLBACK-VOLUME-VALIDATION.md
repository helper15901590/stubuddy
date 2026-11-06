# 已保存 Wi-Fi 顺序重连、删除网络与 80% 音量基线

2026-11-06：本轮按用户要求核实并修复网络切换故障，增加删除入口，将主工程数字峰值改为 80%。只修改板级固件、配网页及测试/文档。软件检查完成；用户已确认 ESP-IDF 编译、烧录和真机验收通过，本记录随源码一并提交。

用户后续反馈：其他验收通过；第一轮语音发送队列满另立检查点处理，见 [UPLINK-CARE-VALIDATION.md](UPLINK-CARE-VALIDATION.md)。以下编译/验收步骤保留作记录。

## 原因与最终行为

原代码虽保存最多五个成功连接的网络，但普通开机、连接断开都固定使用 `history_.items[0]`。失败只重试这一项，认证失败或二十秒超时后直接进入人工配网，没有尝试第二项。因此，公司网络排第一、家庭网络排第二时，回家无法自动改连家庭网络。

- 普通开机/断线后按最近成功连接的顺序尝试保存的网络；每项最多二十秒，找不到热点、认证失败可提前进入下一项。主动停止上一轮时，先等待断开事件或有界等待结束。失败不改变顺序。
- 任一项获取有效 IP 并保存后结束尝试，成功项移到最前。全部失败才打开配网热点，等待人工选择；中间失败不逐项播放错误音。最多五项，不无限轮询。
- 按住 GPIO15 上电仍进入人工配网，等待家长选择，不抢连旧网络。尚未确认首次配网的历史记录也不触发自动连接。
- “连接过的网络”每行有独立“×”。点击只删除该 SSID 的保存信息，不触发连接。设备保存成功并回执后页面才移除；失败保留当前列表并提示。连接中/已连接时禁用删除。
- 删除沿用 `stubuddy_wifi/history` 原版本、尺寸；清空腾出的整个槽位，保留其他网络顺序/密码，同时清理旧 `station`、`ap_password` 键，避免旧格式恢复记录。删除最后一项后继续等待人工配网，下次上电也进入配网。不擦除整片 NVS，不改动提醒和音量。
- 主工程统一数字峰值为 80%，PCM16 约 ±26214；AI、录音回放、开机/联网/错误音、提醒铃声和 LED 包络参考共用该常量。对话和提醒音量仍独立、掉电保存，已有数值不被重置。早期独立 speaker-selftest 工程未改。

沿用 `stubuddy-s3-n16r8-ai-ptt-reminders` 和 ESP-IDF 6.1。PTT、八轮生成即保存历史、提醒优先级/三分钟上限、WSS 和分区保留。本轮无需更新 ECS 服务端。

## 软件检查结果

- 配网页 Node.js 回归 31 项通过：包括删除按钮、只提交目标 SSID、阻止重复点击、迟到回执隔离、保存失败/HTTP 拒绝、成功回执与列表一致性、焦点下刷新及原有交互。
- 六项 C++ 主机回归通过：Wi-Fi 历史/顺序候选/删除与布局、PTT/PCM、LED、音频帧/CRC、倒计时、定时提醒/NVS 适配器及缓存。新增历史用例覆盖公司候选失败后轮到家庭候选、成功后提升顺序、尝试耗尽、删除中间/最后一项、腾出槽位的密码清空和空记录重载。
- 共 37 项通过。主机检查使用 MSVC 适配；Python 3.13 在 Windows 沙箱中的临时目录 ACL 问题通过本次检查进程的临时目录创建适配解决，没有修改断言或跳过用例。修改的 C/C++ 格式和 Git 空白检查通过。
- 用户已确认本轮 ESP-IDF 构建、烧录和真机验收通过；后续日志发现的发送队列问题亦已修复并经用户验收通过。

## 1. 编译和核对

关闭此前占用串口的监视器，在 Windows PowerShell 完整运行：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    $env:PYTHONUTF8 = '1'
    . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
    $env:IDF_CCACHE_ENABLE = '0'
    Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'

    python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ai-ptt-reminders
    if ($LASTEXITCODE -ne 0) { throw '编译失败，请停止后续步骤并保留完整错误，不要烧录旧产物。' }
    python scripts/verify_stubuddy_startup.py --variant ai-ptt-reminders
    if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请停止烧录并保留 FAIL 行。' }
    python -m serial.tools.list_ports -v
    if ($LASTEXITCODE -ne 0) { throw '串口枚举失败，请检查 ESP-IDF 环境。' }
}
```

预期：`Project build complete`、核对全部 PASS，并列出实际 COM 号。沿用现有被 Git 忽略的真实设备配置头；若提示缺失或配置错误，停止并反馈错误，不输出头文件内容、不重新生成 Token。若 Python/serial 缺失，先重新完整加载上面的 IDF 环境。

## 2. 烧录并监视

复制整块到 PowerShell，提示时输入实际端口，例如 COM4；输入 4 也会归一化：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    $env:PYTHONUTF8 = '1'
    . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
    $env:IDF_CCACHE_ENABLE = '0'
    $stubuddyFirmware = 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
    $stubuddyPort = (Read-Host '输入实际串口，例如 COM4').Trim().ToUpperInvariant()
    if ($stubuddyPort -match '^\d+$') { $stubuddyPort = 'COM' + $stubuddyPort }
    if ($stubuddyPort -notmatch '^COM[1-9]\d*$') { throw '端口格式不正确，请输入 COM4 这样的完整端口。' }

    Set-Location -LiteralPath (Join-Path $stubuddyFirmware 'build')
    if (-not (Test-Path -LiteralPath 'flash_args')) { throw '缺少 flash_args，请先完成编译和核对。' }
    python -m esptool --chip esp32s3 -p $stubuddyPort -b 115200 --before usb-reset --after no-reset write-flash '@flash_args'
    if ($LASTEXITCODE -ne 0) { throw '烧录失败，请保留错误；端口忙时先关闭占用它的监视器，再重新枚举。' }

    Set-Location -LiteralPath $stubuddyFirmware
    Write-Host '预期已显示 Hash of data verified。Staying in bootloader 是正常结果。'
    Write-Host '监视器启动后，不按主键，拔插 USB 启动，观察完整启动日志。'
    python -m esp_idf_monitor --no-reset --port $stubuddyPort --baud 115200 build/xiaozhi.elf
    if ($LASTEXITCODE -ne 0) { throw '监视器异常退出，请保留错误、检查串口占用并重新枚举端口。' }
}
```

预期烧录校验成功。保留 NVS（0x9000，大小 0x6000），不用 erase-flash 或 merged 整片写入。实物没有 BOOT/RST 按钮，不需要按键进入下载。监视器内 Ctrl+] 退出。

如需单独重开监视器，复制整块：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    $env:PYTHONUTF8 = '1'
    . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
    Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
    python -m serial.tools.list_ports -v
    if ($LASTEXITCODE -ne 0) { throw '串口枚举失败。' }
    $stubuddyPort = (Read-Host '输入实际串口，例如 COM4').Trim().ToUpperInvariant()
    if ($stubuddyPort -match '^\d+$') { $stubuddyPort = 'COM' + $stubuddyPort }
    if ($stubuddyPort -notmatch '^COM[1-9]\d*$') { throw '端口格式不正确。' }
    python -m esp_idf_monitor --no-reset --port $stubuddyPort --baud 115200 build/xiaozhi.elf
    if ($LASTEXITCODE -ne 0) { throw '日志打开失败，请保留错误并检查端口占用。' }
}
```

保持监视器运行后拔插 USB，可避免错过启动日志。端口忙时关闭占用该端口的监视器；USB 重连后端口变化则重新枚举。不要随意结束其他 Python 进程。

## 3. 真机验收

1. 新固件识别：日志有 `SETUP REVISION: saved-network fallback + forget`，开机提示音日志显示 `80% peak=26214`。
2. 开机回退：准备两个已保存的 2.4GHz 测试网络 A/B（可用路由器或手机热点），最后成功连接 A，使其排第一。关闭 A、保持 B 可用，不按主键拔插 USB。应看到 `SAVED TRY: index=1 total=...` 失败后 `index=2`，随后连接 B；成功后 AI 服务就绪提示音正常，可 PTT 问答。
3. 断线回退：在已连 A 时关闭 A、保持 B 可用，不重新上电也应尝试 B。恢复后先松主键再新按录音，旧回复不补播。B 成功后下次普通开机优先 B。
4. 全部不可用：关闭所有保存的测试网络。一轮全部失败后出现配网热点并等待选择；页面刷新/扫描不自动抢连。重新上电可重新尝试保存列表。
5. 人工配网：按住 GPIO15 并拔插 USB，进入配网后松键。手机连接 stubuddy 热点，必要时访问 http://192.168.4.1/。即使旧 Wi-Fi 可用，也等待手动选择。
6. 删除：在人工配网页删除一个明确的测试网络。只移除所点项，保留其余项/顺序，不出现连接或密码框；重新进入人工配网后该项仍不存在，可重新输入密码加入。用临时网络验证最后一项删除后仍可配网，避免误删日常网络。
7. 页面回归：已保存项一键重连、附近列表、手动输入、原生密码键盘、错误密码提示、成功三秒关闭热点均正常。连接期间“×”禁用，不重复删除/连接。
8. 音量：可语音设对话音量一百后比较 AI、录音回放和开机/联网/错误提示音；提醒音量仍独立，可设五十比较。确认较原基线增大、首尾无爆音/明显破音，LED 状态/律动正常。数字峰值增加不代表主观响度按相同比例增加。
9. 原功能：静音/恢复、两种音量掉电保存、等待/播报打断与收尾按键、60秒保护、八轮历史、提醒抢占/三分钟停止和未来计划重启恢复正常。删除 Wi-Fi 不应清空音量或提醒。

失败时保留同一时段 `STUBUDDY_WIFI`、`STUBUDDY_CODEC`、PTT 和提醒日志，不输出私有配置或真实凭据。Wi-Fi 驱动和音质以本次真机结果为准。用户已确认验收通过，本记录随源码一并提交；后续功能另行讨论。
