# 检查点4.2：PTT音量工具与AI播报LED律动

## 验收结论：4.2合并真机通过，待用户Git提交

2026-11-05，用户明确反馈：“这一轮合并验收全部通过，50和60读音正确，音量短句、收尾按键和打断记忆正常，连续20轮没有误超时，60秒保护也正常。”据此将本检查点标记为真机验收通过。此前70%峰值、LED律动、音量保存/恢复等合并项目按用户整体验收结论通过；本轮软件84项测试及三个变体构建/核对已通过，具体结果见下文。

最终规则：仅PTT；AI/回放及提示音统一70%数字峰值；音量工具以设备真实回执为准并保存当前值/静音前值；固定音量TTS中文读数，原始文字和数学保留数字；正常收尾接受新按键，60秒原键和断网恢复保留松键保护；完整回答生成即进入最近8轮历史，播报打断不撤销。当前HEAD为4.1基线`4b41025`，本轮4.2待用户提交，完成后以实际Git日志为准。不进入下一阶段。下文各“待复验”段落是开发过程记录，以本节最新结论为准。

## 2026-11-05：音量短句、中文读数和并发计时修复（待合并真机复验）

用户确认修复范围：最近一轮已生成问答明确涉及音量时，“调到100/设成60/调整到百分之六十”等省略短句归入音量工具；无对应上下文先澄清，不交普通LLM假报无法控制。当前问题为数学“再加1”时仍不提供工具。工具先读取设备实际值、保存成功才确认。历史继续保存原始用户短句及数字格式的回复，最近8轮已生成问答加当前输入。

仅固定音量查询/调整/恢复模板在送TTS前将0～100转换为中文读数（如“百分之60”→“百分之六十”）；设备AI TEXT、工具参数、历史、普通数学不重写。兼容现有普通/克隆音色，不新增供应商参数。真实发音需用户试听确认。

固件新增板级`clock_age.h`：先快照异步更新的时间戳，再读取当前时间；对未来时间戳作防御，保留32位毫秒回绕。PTT业务进度、WS接收活跃度、hello等待均使用该计算；45秒无进展、95秒首声、155秒整轮门限保持。日志`ROUND TIMEOUT`列出turn/reason/elapsed_ms/limit_ms；解码失败输出帧序、包长和错误码，样本不符输出期望/实际值并分别标记`stream_decode_failed`或`stream_sample_mismatch`，不再冒充无进展超时。

`smoke_test.py --tts-compare-dir`仅执行4次真实付费TTS：导出60/50的阿拉伯数字和中文写法WAV，使用服务器同一私有模型/音色配置。文件为原始16kHz单声道PCM16包装WAV，不经Opus、设备归一化或70%峰值；不会改变设备音量/历史。此选项与其他自测选项分开执行。先部署本轮服务端，再在Windows PowerShell运行：

```powershell
& {
    $ErrorActionPreference = 'Stop'
    ssh -T -p 22 -o ConnectTimeout=15 root@121.41.86.40 /opt/stubuddy-ai/.venv/bin/python -u /opt/stubuddy-ai/smoke_test.py --config /opt/stubuddy-ai/.private.json --tts-compare-dir /tmp/stubuddy-tts-compare
    if ($LASTEXITCODE -ne 0) { throw '原始TTS对照生成失败，请保留错误。' }
    $stubuddyCompare = 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main\tmp\tts-compare'
    New-Item -ItemType Directory -Path $stubuddyCompare -Force | Out-Null
    scp -P 22 'root@121.41.86.40:/tmp/stubuddy-tts-compare/*.wav' $stubuddyCompare
    if ($LASTEXITCODE -ne 0) { throw '音频下载失败，请停止试听并保留错误。' }
    foreach ($stubuddyName in @('60-digits.wav','60-chinese.wav','50-digits.wav','50-chinese.wav')) {
        Write-Host "试听：$stubuddyName"
        $stubuddyPlayer = New-Object System.Media.SoundPlayer -ArgumentList (Join-Path $stubuddyCompare $stubuddyName)
        try { $stubuddyPlayer.PlaySync() } finally { $stubuddyPlayer.Dispose() }
    }
}
```

生成PASS仅表示音频导出成功，不能替代听音判断。数字版错误而中文版正确，支持数字发音问题；两版原始WAV均正确而设备有误，需继续检查设备放音；中文版仍错误则记录该结果继续排查TTS。下载目录已由Git忽略，勿强制加入提交。

合并验收：先问“现在音量多少”再说“调到100/设成60”；新会话无音量上下文说“调到60”应澄清；数学1+1后再加1仍答3。明确调到50和60时AI TEXT保留数字，设备应正确读“五十/六十”。连续至少20轮（包含短句、正常播放、收尾打断、播放中打断及静音恢复），不出现起播后误判45秒超时；若失败保留新的reason/elapsed/limit或解码/样本错误日志。70%峰值、启动就绪提示、收尾键、60秒原键保护、生成即记忆及8轮滑动按本手册其他项目回归。沿用下方完整部署/编译/烧录步骤，不提交Git直到真机通过。

本轮软件结果：54项服务端测试+30项板级/启动/回声测试全部通过，共84项。覆盖本地真实WS“查询→调到100”、原始历史不变而TTS转换中文、四个原始WAV的格式/内容传递、1ms交错/未来时间戳/32位回绕/真实45秒边界，以及70%峰值、收尾键和打断记忆回归。C++格式、Python语法、diff检查通过。WSS回声构建/核对通过（1,247,584字节），4.1通过（1,249,104字节），4.2通过（1,269,872字节/2MiB）；最终build目录为4.2。真实模型调用、原始音频听音和真机验收仍由用户执行；没有代操作云端、烧录或Git提交。

## 2026-11-05：70%峰值与收尾按键、打断记忆合并复验

用户明确要求将当前设备固件中的AI语音/录音回放和开机/联网/错误提示，统一从3%改为70%数字满幅峰值。唯一基线定义在板级`audio_levels.h`：70%、PCM16约±22937。录音回放的归一化目标及最终钳位、AI流式首段的固定/回退增益、所有提示音的正弦振幅和LED能量参考均使用此基线；LED仍取实际DMA音频，静音时不律动。

AI流式增益上限和静音前缀回退增益随基线同比提高，仍每轮固定；首段静音不会让整轮静音。70%是峰值基线及上限，不表示每段语音都持续达到70%。对话音量继续0～100并保留原平方缩放、NVS当前值和恢复值；提示音固定70%，不受对话静音影响。引脚、硬件接线及独立早期诊断工程不变。

本次与下述收尾按键和生成即记忆一并部署/烧录/复验，仍使用`ai-ptt-controls`，第1～3节完整PowerShell操作适用。先部署并完成云端自测，再烧录；已更新云端也可重跑自测确认。

合并验收顺序：
1. 冷启动：开机音明显增大、无爆音；`READY ... fixed 70% peak=22937`；AI就绪才播成功音，随后待机熄灯。
2. “音量调到30”→“调到60”→“调到100”：AI逐级增大、无明显破音，LED跟语音律动；再静音→恢复和重启，确认原保存/恢复规则。
3. 在AI最后一字、刚说完、刚熄灯时分别按住主键说新句，各3次；均接续录音、首字保留，无需再松键按下。
4. 问1+1，播报中打断再问“再加1”，应答3；连续十轮多轮打断，`HISTORY COMMIT reason=answer_ready`每轮只一次，最多8轮，当前输入另计。
5. 按住录满60秒：先灭灯再等待、自动回复；原长按不打断自身，松开重按可打断。连续十轮检查无复位、旧音恢复或内存持续下降。
6. 临时关闭所连路由器Wi-Fi或手机热点：错误提示明显增大、错误闪烁；恢复后重新松键按下可录音，重新就绪才成功提示。无需修改服务器配置。

原始录音回放的同一增益/钳位已纳入主机测试；AI变体不提供本地回放模式，不混入本轮云端AI真机验收。

2026-11-05合并软件检查：50项服务端测试、19项PTT/LED/Wi-Fi/回声主机测试、11项启动配置测试全部通过，共80项；C++格式、diff检查和板级旧峰值硬编码检查通过。4.1回退构建及核对通过（1,248,608字节），4.2构建及核对通过（1,269,376字节/2MiB），最终build目录是4.2。仍需用户云端真实模型和真机验证，未代操作部署、烧录或提交。

## 最新：收尾按键与打断历史修复（2026-11-04，待本次真机复验）

用户要求改为：完整回答文字一生成就保存本轮问答，之后播报是否被打断均保留完整回答（包括未播完部分），TTS/传输失败也不撤销已经生成的文字。ASR/LLM尚未生成回答就取消或失败的请求不占历史。最近8轮已生成问答加当前输入，生成第9轮后存2～9，生成第10轮后存3～10；重复/旧`playback_done`和`abort`不重复提交。日志`HISTORY COMMIT ... reason=answer_ready`在TTS之前出现；`PLAYBACK DONE`只表示实播结束。RAM、30分钟过期和进程重启清空规则不变。

AI播报尾部的补零及DMA排空现在持续检查30ms消抖按键，新按下可立即中止并接续录音。正常结束时保留已松开的按键及消抖进度，回待机不额外预热200ms或强制再松键；60秒原长按和网络恢复仍保留松键保护。LED其余行为不变。

本次重点验收：在最后一个字、刚说完和刚熄灯三个时点分别按住主键说一句新话，均应正常录音、首字保留；连续各做3次。让AI回答1+1，在说“2”时按键打断并问“再加1”，应回答3；十轮中多轮打断，日志历史仍递增至8后保持8。60秒原键不松仍不能自行再录音，松开再按才能开启新轮；先前成功提示、音量保存和恢复、静音灯态保持原约定。

软件测试：50项服务端测试和4项板级主机测试通过；包含首段音频前/播报中取消仍记住完整回答、TTS失败保留文字、晚到LLM不能提交已取消的未生成回合、十轮打断滑动窗口、重复回执无重复历史，以及收尾边界按键/消抖/60秒旧键保护。Python语法、C++格式与diff检查通过；4.1回退构建/核对通过（1,248,592字节），4.2构建/核对通过（1,269,360字节/2MiB），最终build目录是4.2。未执行云端部署、烧录或Git提交，等待用户真机复验；此前记录属于当时检查结果。

## 2026-11-04 收尾修正（待本次真机复验）

用户报告上一轮修复测试通过，本次只确认以下范围：
- `ai-ptt-controls`在Wi-Fi连接后暂存成功提示，校时、严格TLS和AI能力鉴权全部就绪后才播一次成功音，再熄灯进入PTT。原开机、配网和错误提示保留。
- 单说“恢复/恢复一下”时，只有最近一轮完整问答明确涉及音量/静音才按恢复音量处理；先查设备，再执行无数值恢复并等待真实回执。无对应上下文时直接澄清，不能口头假报成功。
- 只有单纯问候（例如“你好”）使用固定短句“你好呀，我是思巴迪！今天想聊什么？”。“你好，1+1是多少”等实际问题仍走原模型和8轮历史。
- ASR/LLM使用同一有界HTTP连接池（最多4连接、空闲复用30秒），取消请求仍中断其I/O，服务退出关闭连接。TTS继续沿用每轮独立、可取消的WSS任务。
- 新增`TIMING`：排队、Opus解码、ASR、LLM及工具耗时与回答字数；`STREAM FIRST`新增`server_first_ms`（服务器收到完整录音后任务开始至首个校验分段发送），`tts_ms`保留。设备`first_voice_ms`是上传收尾至开启播放的计时，均非声学首声测量，不承诺固定1秒。
- 静音灯态沿用上一轮：音量0时播报能量为0，因此无律动；静音下仍完整处理回复与播放完成确认。本次没有改为静音时可见律动。

本次复验：冷启动确认`AI READY`在`CUE: CONNECTED`之前、成功提示后待机；音量30→静音→单说“恢复”应真实恢复30（等确认播完）；最近问答改为数学后单说“恢复”应澄清；“你好”短回复、带问题问候正常作答；对固定问题做数轮观察`TIMING`/`STREAM FIRST`及设备`first_voice_ms`，保留按键打断、60秒和8轮历史回归。
现有第1～4节部署/编译/烧录命令继续适用；日志筛选需加入`TIMING`。
本次软件检查：48项服务端测试全部通过（含本地真实WebSocket、Opus、短口令、问候和HTTP取消/复用），Python语法、C++格式及diff检查通过；规范构建及`verify_stubuddy_startup.py --variant ai-ptt-controls`通过，应用1,268,592字节/2MiB。以上不替代云端真实模型自测和真机复验。未操作ECS、烧录或Git提交。

2026-11-04，用户确认只使用PTT，暂不做VAD、免按键听音和全双工。基线4.1已真机通过并提交`4b41025`。
本轮新增独立变体`stubuddy-s3-n16r8-ai-ptt-controls`，保留旧`ai-ptt`回退。当前已做首轮真机测试，LED和当前音量掉电保存正常，尚未整体验收通过。继续修复4.2，复验通过后再Git提交。

首轮真机反馈：LED律动和插拔重启后的音量保存正常；首次对话出现发送失败并自动重连，数学追问误走音量工具，静音后“恢复音量”没有恢复。当前正在4.2内修复，尚未整体验收或Git提交。
修复后普通问答不提供设备工具，保留全部8轮历史加当前输入；“恢复音量/取消静音”调用设备保存的静音前非零音量，不从模型或聊天历史猜数值。新增能力`volume_restore=1`，需要先更新服务器再刷固件。

## 功能与边界

- 对话音量0～100；无保存值时100；以新70%峰值为基线，0保持静音；大一点/小一点每次10个百分点，边界夹到0/100。
- 设备查询当前实际设置；设置在NVS写入/提交成功后才确认。命名空间`stubuddy_audio`的新版`speech_state`单一版本化Blob原子保存当前值及静音前值；从旧`speech_volume`只读迁移，保留原NVS分区和配网记录。旧值非零时保留并作为恢复值；旧版本已保存为0时无法找回未曾保存的静音前值，首次恢复采用100。新版此后会保留准确恢复值。相同值不重复写Flash。
- 每次打开I2S保留对话音量。开机、联网和错误提示使用固定70%数字峰值；静音不屏蔽这些提示音。
- 音量100对应70%数字峰值上限，不代表实际声压或硬件功率；本次实际响度与失真需真机验收。
- 录音白灯常亮；ASR/LLM/工具/首包等待均匀呼吸（2秒周期）；AI播放按实际送出DMA音频能量平滑律动；语音停顿变暗，待机/结束熄灭。
- 能量取右声道、已经包含设备音量缩放的DMA数据。I2S发送完成回调只统计整数能量，LED定时器负责平滑/PWM，避免下载音频提前驱动灯光。
- 短提示音仍常亮；配网/联网呼吸及错误闪烁和提示音保留。缓冲不足暂停放音时呼吸，恢复后再律动。
- 保留PTT松键结束、短于300ms过滤、60秒自动结束和松键保护、等待/放音按键取消接续录音、首尾/CRC校验及旧回合过滤。
- 保留最近8轮完整生成的问答加当前问题。工具消息只在本轮临时使用，不增加历史轮数；回答文字生成就提交，打断播报不丢失本轮。0音量时仍处理静音播放完成确认。
- 执行前取消则不再执行旧设置；已成功保存的设置不会因为确认语音被打断而撤销。断链回执未送达时不能声称成功，之后重新查询实际值。

## 协议与复用

复用原`AudioCodec`播放路径、ESP Opus、WSS/TLS/SNTP、LED PWM和NVS。原小智MCP的设备状态/音量工具命名和0..100参数约定用于本受限通道；没有启动通用Application或完整MCP。

```text
hello(ai/1,reply_stream=1,device_controls=1) -> hello(确认能力)
PTT listen.start -> Opus -> listen.stop -> ASR
LLM Function Calling -> stage(tools)
device_tool(session_id,turn,call_id,name,volume?)
设备音频任务查询/保存 -> tool_result(session_id,turn,call_id,ok,volume)
查询结果继续交模型决定是否设置；设置成功以设备回执生成确定的确认语音
完整回答生成 -> 提交本轮问答 -> 流式TTS -> 分段/最终校验 -> 实际播放与LED律动 -> playback_done（仅播放状态）
```

模型函数名使用`self_get_device_status`、`self_audio_speaker_set_volume`，映射到设备通道的`self.get_device_status`、`self.audio_speaker.set_volume`。每轮最多4个串行工具调用、每个5秒回执超时，设置前先查实际值；首个成功设置后直接确认，不允许同轮重复修改。
只有本次输入明确表达音量意图时才提供工具；普通问答与算术追问走不带tools的模型路径，历史不做删减。恢复命令先查询，再调用无数值参数的`self.audio_speaker.restore_volume`，由设备NVS决定恢复值。单纯改变8轮窗口大小不能解决工具误调用。
端侧缓存本轮最多4个调用结果；重复ID返回原结果，参数不同的ID复用拒绝；新按键取消时清掉本轮待办，网络回调不直接操作NVS或音频。
未协商该能力的4.1设备继续原流程；新固件要求服务端明确确认能力，因此必须先更新服务器。
4.2后台WSS发送可写等待由500ms提高至2秒，并输出失败帧序/回合/发送类型。网络任务等待不阻塞PTT，但队列满仍按原规则明确取消；不自动重发部分帧或旧回合、不关闭TLS。首次断线的具体网络原因仍需复测及对齐日志，不能仅凭一次恢复判定彻底解决。

## 1. Windows PowerShell更新服务器与云端检查

沿用已配置的Key和Token，无需重新输入。部署默认保留服务器`.private.json`；不要加`-UpdateConfig`。

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location -LiteralPath 'D:\Desktop\stubuddy'

& {
    $ErrorActionPreference = 'Stop'
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\voice-ai\Deploy-VoiceAi.ps1 -User root -Port 22 -Install
    if ($LASTEXITCODE -ne 0) { throw '4.2部署失败，请停止，不要烧录。' }

    & 'D:\Desktop\stubuddy\stubuddy\server\voice-ai\.venv\Scripts\python.exe' 'D:\Desktop\stubuddy\stubuddy\server\voice-ai\check_wss.py' --controls
    if ($LASTEXITCODE -ne 0) { throw 'WSS工具能力检查失败，请停止，不要烧录。' }

    ssh -T -p 22 -o ConnectTimeout=15 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 root@121.41.86.40 /opt/stubuddy-ai/.venv/bin/python -u /opt/stubuddy-ai/smoke_test.py --config /opt/stubuddy-ai/.private.json --controls --math --restore
    if ($LASTEXITCODE -ne 0) { throw '真实模型工具/流式自测失败，请停止，不要烧录。' }
}
```

预期`INSTALL PASS`、`device_controls=1 volume_restore=1 negotiated`、`real LLM math context 1+1 -> 2, plus 1 -> 3; device tools unavailable`、真实Function Calling/模拟恢复回执及流式TTS完整性PASS。模拟回执不改变真机音量，不验证设备Flash保存。服务重启清空RAM问答历史。
失败时查看本手册末尾服务状态和日志；SSH关闭时先核对22/443连通性，使用`-T`重试只读检查，不重复安装或关闭TLS。

## 2. 编译与核对

同一已加载IDF的PowerShell执行；新窗口需先执行上面的三行环境加载。

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
& {
    $ErrorActionPreference = 'Stop'
    python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ai-ptt-controls
    if ($LASTEXITCODE -ne 0) { throw '4.2编译失败，请保留错误，停止烧录。' }
    python scripts/verify_stubuddy_startup.py --variant ai-ptt-controls
    if ($LASTEXITCODE -ne 0) { throw '4.2板型核对失败，停止烧录。' }
    python -m serial.tools.list_ports
}
```

预期`Project build complete`，核对包含`restricted device volume tools, persistent speech volume and DMA speech LED linked; PTT only`，NVS原布局、2MiB应用分区、严格TLS、单一Board工厂均PASS。

## 3. 完整烧录与监视器

退出旧监视器Ctrl+]，关闭占用串口的程序；COM4替换为实际端口。

```powershell
$stubuddyPort = 'COM4'
& {
    $ErrorActionPreference = 'Stop'
    Push-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main\build'
    try {
        python -m esptool --chip esp32s3 -p $stubuddyPort -b 115200 --before usb-reset --after no-reset write-flash "@flash_args"
        if ($LASTEXITCODE -ne 0) { throw '烧录失败，请保留最后30行输出。' }
    }
    finally { Pop-Location }
}
```

预期`Hash of data verified`；`Staying in bootloader`正常。保留NVS，不使用erase-flash或merged整片写入。USB断连时关闭串口程序、拔插USB、重新枚举端口并更新变量，再重试同一完整写入。

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python -m esp_idf_monitor --no-reset --port $stubuddyPort --baud 115200 build/xiaozhi.elf
```

保持监视器运行后，不按主键拔插USB。预期`CHECKPOINT 4.2`、`SPEECH VOLUME loaded=100`（已有值则显示保存值）、`DEVICE CONTROLS READY`、`AI READY`和待机灯灭。没有READY则先反馈附近日志。

## 4. 真机验收

| 操作 | 通过标准 |
| --- | --- |
| 首次查询“现在音量多少” | 初始无保存值为100；端侧`VOLUME TOOL ... get ... actual=100`，云端`TOOL CONFIRMED`，播报真实值 |
| “音量调到30%”，再查询 | 先查再设；端侧`saved=30`、回执后确认，连续回复保持30且听感降低 |
| “大一点”“小一点” | 30→40→30；回复准确，边界不超0..100 |
| 掉电保存 | 设置30后等成功，直接拔插USB；保留Wi-Fi，启动loaded=30，查询仍30 |
| 静音与恢复 | PTT说静音：设置0后AI确认无声、无律动；联网提示仍正常；PTT说“音量恢复到50%”即可听到回复 |
| 静音掉电 | 设置0成功后拔插USB，loaded=0、仍可PTT恢复，不被改成默认10或100 |
| 恢复静音前值 | 设30→静音→说“恢复音量”，应恢复30；再静音、拔插USB后说“取消静音”，仍恢复30 |
| 数学追问 | 1+1的回复完整播完灯灭后，再问“再加1”，应答3；音量保持原值，云端两轮ROUTE均为chat，无音量TOOL CONFIRMED |
| 首轮连接 | 连续3次正常拔插USB启动，等AI READY后立即开始第一轮；若复发，保留WS SEND FAILED及服务端同时间段日志 |
| LED | 录音白灯常亮；等待均匀呼吸；播放跟语音强弱平滑变化，停顿变暗；结束即灭，原主键仍按住也灭 |
| 不同音量灯效 | 同类短句在100/30两档对比，声音与律动整体强度降低；不按下载进度提前亮灯 |
| 按键取消 | 等待工具/播放中均可新按下接续录音，无旧音/旧律动恢复。成功设置后取消确认语音，之后查询仍是已生效值 |
| 60秒及十轮 | 保留自动停录先灭灯、旧长按不打断自身回复、首尾音频；连续十轮无复位、杂音或异常内存下降 |
| 上下文 | 8轮完整生成问答加本次输入；工具消息不额外占轮数，播报打断仍保存；未生成回答则不提交 |
| 断网 | 未收齐回复停止，已最终校验缓存可播完；恢复需松键再按，不补播旧轮 |

## 5. 失败定位与回退

另开PowerShell，以下只读命令不需要IDF：

```powershell
ssh -T -p 22 -o ConnectTimeout=15 root@121.41.86.40 systemctl --no-pager --full status nginx.service stubuddy-ai.service
ssh -T -p 22 root@121.41.86.40 journalctl -u stubuddy-ai.service -b --no-pager -o cat |
    Select-String -Pattern 'READY|ROUTE|TOOL CONFIRMED|STREAM FIRST|STREAM END|TURN FAILED|HISTORY COMMIT|PROTOCOL REJECT|CLOSED' |
    Select-Object -Last 100
```

服务应active(running)。回执超时/写入失败没有成功确认；请反馈对应回合日志，不要输出私有配置、Token或Key。
回退仍可用新服务器兼容的4.1固件。在加载IDF的PowerShell执行，再按步骤3完整写入：

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
& {
    $ErrorActionPreference = 'Stop'
    python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ai-ptt
    if ($LASTEXITCODE -ne 0) { throw '4.1回退编译失败。' }
    python scripts/verify_stubuddy_startup.py --variant ai-ptt
    if ($LASTEXITCODE -ne 0) { throw '4.1回退核对失败，停止烧录。' }
}
```

旧变体沿用原诊断音量，不覆盖4.2保存键；再次刷回4.2会读取原保存值。只有用户真机确认通过后才Git提交，不连续跨阶段开发。

## 本轮软件检查记录

- ESP-IDF6.1：4.2固件编译与板型核对PASS，1,267,680字节/2MiB；4.1回退变体编译/核对PASS，最终build目录恢复4.2。
- 服务端41项本地测试PASS：普通/克隆TTS与流式完整性、8轮历史、真实回执前不确认、设置失败、相对10点及上下限、取消、跨会话/重复回执、成功设置后取消不撤销等。模型请求使用本地替身，不使用真实Key。
- 主机152项回归中150项通过、2项因EspVocat/MCP主机工具和头文件条件跳过（6项TLS通过IDF Python补跑）。新增实际C++灯效平滑/均匀呼吸、静音/取消包络复位、工具去重/容量/取消检查。
- Python/PowerShell语法、C++格式和git diff --check通过；私有配置继续忽略，未读取或输出实际凭据。
- 普通bread-compact-wifi完整应用兼容复编未完成：乐鑫组件注册站公共元数据不可达；离线尝试缺少完整应用依赖别名/组件。没有修改第三方组件，也不把此项记录成PASS。本次4.2与4.1目标构建不受影响。
- 当前未执行ECS真实Function Calling/模拟设备回执自测，也未验证真机NVS、音量、灯效或按键。由用户按上述步骤完成；编译通过不能替代真机验收。

以上软件检查为首轮版本记录。此次修复已新增数学追问不暴露工具/不改变音量、保留历史、恢复操作依赖设备回执、静音前状态序列化与迁移的回归；真实模型/真机修复验收仍待用户执行。用户已确认首轮LED和重启保存正常，未将其当作整轮4.2验收通过。

修复版软件结果：服务端45项通过；主机LED/音量状态检查及15项回声回归通过；4.1回退与4.2修复版构建/核对均PASS，最终4.2固件1,268,496字节，build目录恢复4.2。Python语法、C++格式与diff检查通过。首次公网发送失败的具体网络原因尚待对齐现场日志，2秒发送预算属于缓解和诊断改进，不代表已完成真机网络复验。
