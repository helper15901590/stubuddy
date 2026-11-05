# 4.3.1：单个倒计时提醒

基线提交：`f3b1f01`。2026-11-05用户确认本轮四项修复及其他真机测试正常，**本轮进入Git提交；保留“提醒列表查询只报剩余时间”的已知问题，按用户要求放入下一轮修复与验证**。暂不开发定时提醒、VAD、NFC或后台。下方“待复验”等为历史过程记录，以本段及文末验收结论为准。

## 已确认范围与实际行为

- 只使用PTT；一次只允许一个活动倒计时，时长60～86400秒（含边界）。已有倒计时时创建新的，只提示先取消旧的，不覆盖。
- 例：“设置一分钟后喝水”“提醒我30分钟后休息”“提醒我半小时后休息”“倒计时还剩多久”“现在有什么提醒”“取消倒计时”。支持ASR写成30s、1min、1h；最近一轮明确谈倒计时时支持“还有多久”“还有多少秒”“取消它”。当前明确时长先做独立范围校验，不让模型把30秒自动改成60秒；普通算术追问不进入设备控制。
- 先让模型生成一句简短、温暖的拟人化提醒，再使用当前普通/克隆音色合成，限制12秒。复用现有libopus编码，单包最多227字节；设备独立47KB PSRAM缓存校验序号、样本数和CRC。缓存完整后设备写入NVS并开始计时，回执后才确认成功。准备失败不创建假倒计时。
- 倒计时从设备确认开始计时，包含随后“开始啦”确认语音的播放时间。等待时LED维持普通PTT状态，待机仍熄灭。
- 到点优先于录音、模型等待和AI播报：丢弃当前录音/取消旧回复并清空DMA，不排队、不自动续播、不记错过。录音需自己重新录；已生成回答可再说“继续”“重复刚才回答”。
- 提醒是短提示音加一句拟人化语音，循环播报；每轮播放排空后静默3秒，从第一轮开始最多持续60秒（包含间隔），到上限立即停止，不等待最后一轮播完。复用同一段缓存PCM，不重复请求TTS。提示音常亮，语音随实际音量律动，3秒间隔和结束熄灭。播报或间隔中新按键停止并删除；继续按住接续新录音。到点前原来一直按着的键不算停止，先松开再重新按下。
- 60秒自动停止、按键停止、播放失败均删除，不另行补报。设备在第一轮播报前先持久化领取/删除，防止循环期间断电后重新响铃；一次触发内允许上述循环，停止后不再恢复。
- 到期时断网/AI通道未就绪，或重启校时后已过期：删除且恢复后不补报。未到期重启继续原截止时间，重新联网后按NVS内提醒文本恢复语音缓存；到期时缓存仍不可用也跳过，不等待补报。
- 提醒音量独立0～100，首次默认50，掉电保存。命令“提醒音量多少”“提醒音量调到60”“提醒音量大一点/小一点”“提醒音量静音”。最近话题为提醒音量时，“调到60”继续调提醒音量；“恢复”要求明确指定提醒音量，不误改对话音量。零音量仍按时消费、删除，实际声能LED为零。
- 普通回复与提醒文本入历史/传给TTS前清除括号里的动作、语气和舞台说明，例如“（轻快地）”；保留数学分组括号。即使服务器已有旧自定义Prompt也补充直接朗读规则。自然亲切通过正文体现。
- 70%峰值基线、对话音量/静音前恢复、收尾按键、首尾字、60秒保护均保留。服务端仍保存最近8轮生成完整回答的问答对，本次输入额外计算；回答生成后即入历史，播报被打断不撤回。提醒自动播报本身不虚构用户问答，不占历史；设置/查询倒计时的实际对话正常入历史。

## 实现边界

新固件变体：`stubuddy-s3-n16r8-ai-ptt-countdown`，独立`CONFIG_STUBUDDY_COUNTDOWN`，4.2与WSS回声变体保留。板级新增`countdown_state.h / countdown.cc`，只保存一个小型版本化NVS记录`stubuddy_audio/countdown_v1`（截止时间、ID、提醒文本、提醒音量）。不改变Wi-Fi或`speech_state`，不改变分区，不每秒写Flash。

沿用ai/1认证、TLS、心跳、录音上传、回复流式通道及限制工具回执。新能力`countdown=1`；新`self.countdown.get/start/cancel/volume`工具仍经音频所有者任务执行，重复call ID不重复写入。缓存走有会话校验的`countdown_cache/countdown_packet`控制消息（每条小于1024字节），不混入当前回答的Opus存储。`countdown_sync`只用于重启/重连后尚未到期的缓存恢复，不设置新截止时间。先更新服务器，再刷新固件。

当前仍是开发期设备Token和单机功能检查，不代表商用设备激活、可靠闹钟或并发压测通过。

## PowerShell部署与烧录

以下命令用于原工作目录`D:\Desktop\stubuddy`，先确保本检查点源码已经同步到该目录。**逐块执行；任何块报错就停止后续步骤。**已有私有配置沿用，不打印、不复制进Git。

### 1. 更新云端

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location 'D:\Desktop\stubuddy'
    $env:PYTHONUTF8 = '1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File '.\stubuddy\server\voice-ai\Deploy-VoiceAi.ps1' -User root -Port 22 -Install
    if ($LASTEXITCODE -ne 0) { throw 'AI更新失败，请停止，不要烧录。' }
}
```

预期`CHECK PASS`、`INSTALL PASS`，已有443网关/回声/证书续期保留，服务器私有配置不覆盖。SSH密码在本机提示时输入；`Permission denied`停止并解决SSH登录，不重复部署。

### 2. 云端真实模型与公网验证

会产生真实模型调用费用；使用模拟设备，不改变真机倒计时或音量。

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location 'D:\Desktop\stubuddy'
    & ssh.exe -T -p 22 -o ConnectTimeout=15 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 root@121.41.86.40 /opt/stubuddy-ai/.venv/bin/python -u /opt/stubuddy-ai/countdown_smoke.py --config /opt/stubuddy-ai/.private.json
    if ($LASTEXITCODE -ne 0) { throw '倒计时真实模型自测失败，请停止，不要烧录。' }
    & ssh.exe -T -p 22 -o ConnectTimeout=15 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 root@121.41.86.40 /opt/stubuddy-ai/.venv/bin/python -u /opt/stubuddy-ai/smoke_test.py --config /opt/stubuddy-ai/.private.json --controls --math --restore
    if ($LASTEXITCODE -ne 0) { throw '现有PTT模型回归失败，请停止，不要烧录。' }
    & '.\stubuddy\server\voice-ai\.venv\Scripts\python.exe' '.\stubuddy\server\voice-ai\check_wss.py' --countdown
    if ($LASTEXITCODE -ne 0) { throw '公网WSS验证失败，请停止，不要烧录。' }
}
```

预期倒计时6轮口语创建/查询/提醒音量/取消/30秒拒绝/空提醒查询PASS，缓存和回复Opus/CRC/样本PASS；普通PTT自测PASS；WSS证书/IP/鉴权/心跳与`countdown=1` PASS。超时或断连不能跳过，保持TLS验证并按下方日志步骤排查。

### 3. 编译并核对新变体

新开PowerShell，完整执行：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    $env:PYTHONUTF8 = '1'
    . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
    $env:IDF_CCACHE_ENABLE = '0'
    Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
    python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ai-ptt-countdown
    if ($LASTEXITCODE -ne 0) { throw '倒计时固件编译失败，不要烧录。' }
    python scripts/verify_stubuddy_startup.py --variant ai-ptt-countdown
    if ($LASTEXITCODE -ne 0) { throw '固件变体/分区核对失败，不要烧录。' }
    python -m serial.tools.list_ports -v
    if ($LASTEXITCODE -ne 0) { throw '串口枚举失败，请保留错误输出。' }
}
```

预期`Project build complete`，核对脚本所有PASS，应用小于2097152字节、NVS保留、倒计时缓存及独立音量已链接。以实际枚举端口为准。

### 4. 完整写入并保留NVS

先关闭其他串口监视器（IDF监视器`Ctrl+]`）。把下面`COM4`替换为实际完整端口号，不要只写`4`。

```powershell
& {
    $ErrorActionPreference = 'Stop'
    $stubuddyPort = 'COM4'
    if ($stubuddyPort -notmatch '^COM[0-9]+$') { throw '请填写COM4这样的完整串口号。' }
    Push-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main\build'
    try {
        python -m esptool --chip esp32s3 -p $stubuddyPort -b 115200 --before usb-reset --after no-reset write-flash '@flash_args'
        if ($LASTEXITCODE -ne 0) { throw '烧录失败，请停止验收并保留错误输出。' }
    }
    finally {
        Pop-Location
    }
}
```

预期`Hash of data verified`；`Staying in bootloader`正常。不要按主键，拔插USB启动。没有BOOT/RST按钮。端口忙/不存在/WinError31时停止，关闭监视器、拔插USB、重新枚举端口并修改后重试，不擦Flash。

### 5. 查看日志

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
    python -m esp_idf_monitor --no-reset --port COM4 --baud 115200 build/xiaozhi.elf
    if ($LASTEXITCODE -ne 0) { throw '监视器结束异常，请保留错误输出。' }
}
```

串口相应替换；保持监视器运行后拔插USB可看完整启动日志。应见`CHECKPOINT 4.3.1`、`AI READY`，AI就绪后联网成功提示音，随后待机灭灯。

## 合并真机验收

所有语音均按住GPIO15说、松开发送。每项从无活动倒计时开始。1分钟到点从`TOOL: action=2 ... remaining=60`开始算，确认播报也计入这一分钟。

| 项目 | 操作 | 预期 |
| --- | --- | --- |
| 基本/单个限制 | “设置一分钟喝水倒计时”，再查“倒计时还剩多久”，再设置另一个 | 先`COUNTDOWN CACHE READY`再确认开始；剩余时间真实减少；第二个被拒绝，不覆盖原提醒 |
| 循环及自动结束 | 等待到点，整段不按键；一分钟结束后查询 | 短音+“喝水时间到啦……”循环，每轮间隔3秒；间隔灭灯，语音律动；最多60秒后`reason=timeout`停止并删除，不再重响 |
| 取消 | 新设一分钟，随后“取消倒计时”并查询 | 确认取消、无活动倒计时，原到点时间不响 |
| 时长边界 | 分别要求30秒、25小时；设置24小时后查询再取消 | 前两项要求1分钟～24小时且不创建；24小时可创建、查询、取消，无需等一天才检查设置上界 |
| 播报/间隔按键 | 分别在提醒播报和3秒静默间隔中，新按住主键说“你好”，松开 | 立即停止并删除提醒；继续按住新录音，正常回答；提醒不恢复。快速点按也停止，短录音按300ms规则丢弃 |
| 录音抢占 | 新设一分钟，约50秒开始按住录音并一直按过到点 | 提醒优先、旧录音丢弃；原来按住的键不误停止提醒；先松开再重新按下才能停止/重录；旧录音不被AI回答 |
| 等待抢占 | 新设一分钟，约58秒发起普通问答，观察等待灯 | 若到点时仍在模型等待，应取消旧回合并播提醒；旧回答不自动冒出，需自己重新问 |
| AI播报抢占/记忆 | 新设一分钟，在约50秒请求稍长故事，让到点落在播报中 | 旧播报立即停，提醒先播；不自动续播。之后说“重复刚才的故事”，可使用已生成完整回答的历史；提醒本身不占8轮问答 |
| 独立音量/掉电 | 查询提醒音量；“提醒音量调到60”，再“调到50”；查询普通对话音量；拔插重启再查两种音量 | 新记录默认50；短句只改提醒音量；两种音量互不覆盖、掉电保留；对话静音时提醒仍按独立音量响 |
| 未到期重启 | 设3分钟倒计时，几十秒后重启 | 联网/校时后恢复缓存，剩余时间继续减少，不从3分钟重开，到期按循环规则提醒 |
| 断电过期 | 设1分钟，确认后断电；超过原截止时间再上电 | `SKIPPED ... powered off`，不补报；查询无倒计时 |
| 断网过期 | 设1分钟，关闭对应路由/热点Wi-Fi，保持设备供电过期，再恢复网络 | `SKIPPED ... offline`，不响、不补报；恢复后先松键再新按，查询无倒计时 |
| 旧功能回归 | 50/60普通音量读音、静音恢复、1+1→再加1、收尾按键、AI按键打断后追问、连续20轮、按住60秒 | 与4.2一致，峰值70%，无误超时/旧音恢复；60秒自动停止、原按住键不能打断自己触发的回复，结束灭灯 |

每项需看到实际声音、LED、按键和删除行为符合预期，编译或云端模拟PASS不等于这些硬件项目已通过。若发现问题先保留同一回合串口及服务端日志，停止Git提交。

## 诊断与回退

```powershell
& {
    $ErrorActionPreference = 'Stop'
    & ssh.exe -T -p 22 -o ConnectTimeout=15 root@121.41.86.40 journalctl -u stubuddy-ai.service -n 120 --no-pager
    if ($LASTEXITCODE -ne 0) { throw '服务端日志读取失败，请保留SSH错误。' }
}
```

不用输出`.private.json`。保留串口中`STUBUDDY_TIMER`、`COUNTDOWN CACHE READY`、`ROUND TIMEOUT`、`RX/TRANSPORT REJECT`，以及服务器`TURN FAILED/TIMING`附近行。

需要回退时，在已加载IDF环境的PowerShell执行以下编译核对，然后沿用第4步写入、第5步监视：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
    python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ai-ptt-controls
    if ($LASTEXITCODE -ne 0) { throw '4.2回退编译失败，不要烧录。' }
    python scripts/verify_stubuddy_startup.py --variant ai-ptt-controls
    if ($LASTEXITCODE -ne 0) { throw '4.2回退核对失败，不要烧录。' }
}
```

新服务器兼容旧4.2能力协商，回退固件不会处理倒计时。NVS记录保留，再回到新版时已过期记录删除、不补报。

## 软件检查记录（2026-11-05）

- 服务端63项测试通过，包含真实本地WebSocket/libopus、倒计时全链路模拟设备回执、缓存完整性、重启缓存恢复、边界拒绝、独立音量及上下文、原8轮生成记忆和取消回归。全部使用模拟上游，不读取真实API Key或调用付费模型。
- 板级主机43项测试通过，包含实际C++倒计时边界/NVS记录字节恢复、到期在线/离线/缓存/重启策略、缓存序号/CRC、工具重复ID和原按住键释放规则，以及原PTT/LED/配网/协议/变体隔离。
- C++格式、Python语法、Git空白检查通过。真实模型调用及麦克风/喇叭/LED/按键优先级由用户按本手册验证；软件通过不代表真机验收。
- ESP-IDF 6.1 canonical构建及板型/分区核对通过：倒计时1282720字节、4.2回退1270128字节、WSS回声回退1247856字节，均小于2MiB。最终build目录恢复倒计时变体。本地隔离构建使用全零占位鉴权头，只作编译检查；用户必须在D盘通过既有私有配置生成头并重新编译，不直接烧录本工作树的检查产物。
- 修改位于Codex工作树`C:\Users\Administrator\.codex\worktrees\46b5\stubuddy`。原D盘工程只读核对仍在相同`f3b1f01`且干净；按交付命令只同步本轮源码/测试/文档，不同步tmp、build、依赖缓存或任何私有配置。

## 本次四项修复与复验（2026-11-05）

- 日志证明“设置一分钟后，喝水。”没有进入tools；“30s后提醒我喝水”也漏路由。现补口语入口、ASR单位归一化、真实设备剩余秒数查询，明确时长独立校验。成功确认仍必须晚于缓存与设备start回执。
- 原先一次播放是上一轮确认的范围；按本轮要求改为循环、3秒间隔、整段最多60秒。按键停止信号锁存，避免一次新按压被后续多次轮询吞掉。保持原按住键先松开规则。
- 清理普通回答、历史和提醒缓存中的括号说明；附加运行时口语约束，覆盖现有服务器私有Prompt。数学分组不删。
- 本次扩充服务端日志复现、显式30秒拒绝、半小时/组合时长、去舞台说明和本地WebSocket六轮通道测试；主机测试包含60秒边界、三秒间隔、毫秒回绕、间隔接近截止时间、停止锁存。云端真实模型自测与真机复验仍由用户执行。
- 重点复验：“设置1分钟后喝水”后可查；“30s后提醒我喝水”明确拒绝且无start工具；“还有多少秒”读设备回执；AI/提醒不读括号说明；到点循环、3秒间隔、60秒自动停、间隔按键也停；其余已通过功能简要回归。

本次软件检查：服务端新增后共69项通过（完整68项及新增普通聊天/TTS/历史清理用例和全部15项倒计时测试补跑），板级主机43项通过，合计112项。倒计时固件ESP-IDF 6.1构建核对通过，1283152字节；本地使用占位鉴权头，仅作编译检查。C++格式、Python语法及Git空白检查通过。云端真实模型与循环一分钟、间隔按键等硬件项目尚待用户复验。

## 用户验收结论与下一轮计划（2026-11-05）

用户反馈：除下述提醒事项查询问题外，本轮测试全部正常。口语创建、30秒拒绝、去除括号舞台说明、3秒间隔循环、提醒触发后最多60秒停止，以及其他已约定功能通过用户真机验收；现按用户要求建立本轮Git检查点。`Replay defined but not used`是AI流式入口保留旧回放函数产生的非阻断编译警告，不代表烧录失败。

已知问题留到下一轮：问“我有哪些提醒或倒计时”时，当前只回答“倒计时还剩xx秒”，没有准确报出提醒事项。本次提交不修改这项业务行为，也不声称此问题已修复。

下一轮仅围绕查询语义讨论和修复，暂不进入定时提醒：

1. “有哪些提醒/倒计时”“现在有什么提醒”应以设备真实保存的事项为准，例如“你有一个喝水提醒”，不从八轮聊天历史猜测，也不只报剩余秒数。
2. “还剩多久/还有多少秒”继续返回真实剩余时间；没有活动提醒时明确说明。
3. 计划验证：口语设置喝水提醒后查询事项、剩余时间查询、取消后查询、重启未到期后的事项恢复，以及旧提醒删除后新设休息提醒不会串用旧事项。补软件回归及真机验收后再提交下一检查点。
