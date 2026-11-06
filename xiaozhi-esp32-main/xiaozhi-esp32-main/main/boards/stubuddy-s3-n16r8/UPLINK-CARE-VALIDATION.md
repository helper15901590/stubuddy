# 上行队列与提醒关怀验收

2026-11-06：用户确认本检查点 ESP-IDF 编译、烧录和真机验收全部通过；发送队列满未再出现，提醒关怀正常。软件检查与用户实测均完成，本记录随源码一并提交。以下命令保留作复现和维护参考。

## 修改和边界

- PSRAM 固定队列从8项改为64项，音频额度56项（约3.36秒），其余8项留给控制消息。单次网络发送预算仍为2秒；音频入队不等待网络，不无限积累。
- 单一FIFO保留start→音频→stop顺序，不将stop提前。每次出队归还音频额度，包括取消回合或旧连接的数据；入队失败归还额度。持续拥塞仍明确失败并重连，不丢帧后假装成功、不重试可能部分发送的数据。
- 故障清理不再向已不就绪的连接排入abort，避免再次队列满。旧回合/旧连接过滤、CRC、录音上限、PTT、提醒优先级和音量不变。
- 增加 UPLINK QUEUED（pending/high_water/capacity）、UPLINK BACKLOG（kind/turn/seq/wait_ms）和 WS SEND SLOW（elapsed_ms）日志。high_water是本次启动以来采样到的最大水位；日志不含音频、凭据。Kind数字依次为0 Start、1 Audio、2 Stop、3 Abort、4 Played、5 ToolResult、6 TimerSync。
- 这是针对已确认容量不足的修复，不能保证持续断网仍能完成对话；原日志不足以确定最初发送积压来自网络还是调度，需用新增日志确认。
- 只丰富 reminder_tools.prompt.answer.046 的到点播报关怀，建议24～32字符，仍受40字符/120字节/12秒限制。时间提取、澄清、事项、工具回执、无事项倒计时“时间到啦”均保留。已有提醒不自动改文案，同一提醒循环播放仍使用缓存语音。

## 软件检查

- 固件7项C++主机回归全部通过：Wi-Fi、PTT、LED、协议/CRC、倒计时、定时提醒、新增上行额度测试。覆盖2秒发送暂停和尾帧、FIFO停止顺序、56音频+8控制边界、重复拒绝不泄漏额度、100轮取消/旧连接数据释放。
- 服务端153项回归全部通过，无跳过：真实本地WebSocket与Opus，模型调用使用测试替身，没有调用付费云模型。
- Windows首轮因未找到Opus DLL失败；在检查进程内指定本机已有Opus DLL后重跑通过。MSVC与临时目录ACL适配仅用于本地主机检查，没有修改测试断言或产品代码。
- 修改的C/C++格式、PowerShell脚本语法及Git空白检查通过。ESP-IDF目标编译、烧录及真机验收由用户执行并确认通过。

## 0. 应用提醒关怀提示词

在设备结束当前对话后，从Windows PowerShell完整执行：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location -LiteralPath 'D:\Desktop\stubuddy'
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\voice-ai\Apply-ReminderCare.ps1
    if ($LASTEXITCODE -ne 0) { throw '提示词更新失败，请保留错误并停止后续步骤。' }
}
```

预期 `PROMPTS APPLIED: revision=...; service active; previous override saved`。脚本读取本地公开默认文案，只替换服务器覆盖文件中.046条目，保留所有其他覆盖项；候选校验通过后备份、安装并重启。已有.046自定义内容会替换成本轮审核文本。无需整包重新部署、不改.private.json。重启短暂断开WSS，清空服务器内存历史和草稿，保留设备NVS提醒和音量。该风格对本服务全部设备生效。

提示词失败时保留错误文字，不发送私有配置内容。需要恢复上一次提示词版本时：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    ssh -T -p 22 root@121.41.86.40 /opt/stubuddy-ai/.venv/bin/python /opt/stubuddy-ai/prompt_admin.py --rollback
    if ($LASTEXITCODE -ne 0) { throw '回退失败，请保留错误文字。' }
}
```

预期同样显示 `PROMPTS APPLIED`。仅有一份上一版本备份；回退不修改固件。

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

## 3. 本轮真机验收

1. 启动看到 `queue=64`，TLS验证通过和AI READY。之前Wi-Fi/80%音量验收不要求全部重做。
2. 连续20轮短问答，尤其联网后第一轮、空闲10秒后的第一轮；每轮松键后有 `UPLINK QUEUED ... capacity=64`，能完整回答，无SEND QUEUE FULL、缺字或错误音。
3. 分别录5秒、15秒、一次60秒，验证尾字、最长录音自动停止、停止后按键行为仍正常。持续按住触发60秒停止时，不应误取消自身回复。
4. 等待回答时重新按键、回答中重新按键，各重复5次。新轮可以录音，旧回答不能恢复，不能因反复取消耗尽队列。
5. 录音或等待时关闭当前路由/热点再恢复，允许该轮明确失败并提示；连接恢复后先松键再新按，新轮正常，旧音频不补播。若有另一个已保存可用网络，可切换后继续测试。无需人为修改路由或注入网络延迟。
6. 新建“一分钟后提醒我喝水”，到点应是事项+一句自然关怀，无括号舞台说明；按主键停止。再说“倒计时一分钟”，应保持“时间到啦”。测试定时提醒时使用至少两分钟后的明确时刻，确认时间/事项/创建回执与到点关怀均正确。
7. 观察已有提醒、两种音量、提醒抢占和LED行为保持正常。新风格只影响新创建的提醒；重启不会给旧提醒换文案。模型措辞有随机性，不要求与示例逐字相同。

若再失败，保留从REC START到重连AI READY的完整日志，重点包含UPLINK QUEUED、UPLINK BACKLOG、WS SEND SLOW、WS SEND FAILED、SEND QUEUE FULL和RX/TRANSPORT REJECT。SLOW/BACKLOG单独出现表示被缓存吸收的延迟，不等于回合失败；结合最终完整回答判断。

2026-11-06 用户验收结论：编译、烧录和真机验收均通过，发送队列满未再出现，提醒关怀正常。本检查点完成；后续功能另行讨论。
