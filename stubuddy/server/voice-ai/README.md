2026-11-06本轮服务端修复：真实时钟报时/日期/星期、提醒事项问法、提醒上下文内识别错字的确认澄清。114项软件回归通过；固件/NVS未改，无需重烧。云端/真机待复验，未提交。详见[REMINDERS.md](REMINDERS.md)。

4.3.2本轮口语修复已完成软件检查（106项）：能力介绍、缺时间追问、数量只读、短句真实取消、草稿补全、提醒继续查询、有限HTTP重试及分段计时。仅更新服务端，无需重新编译/烧录；云端与真机待复验，不提交Git。详见[REMINDERS.md](REMINDERS.md)。

本轮4.3.2：新增一次性/按周指定星期定时提醒（最多三个）与一个倒计时共存，统一事项查询，循环上限三分钟。详见[REMINDERS.md](REMINDERS.md)。本轮软件检查完成后交用户真机验收，尚未验收/提交。

# stubuddy：DashScope PTT完整语音检查点4.1

2026-11-05验收更新：用户确认倒计时本轮四项修复及其他测试正常，进入Git提交；“有哪些提醒/倒计时”应准确报出事项的问题记录为下一轮修复与验证，本次保留该已知问题。PTT、8轮历史及独立提醒音量保持，暂不进入定时提醒。结论与待办见[倒计时手册](COUNTDOWN.md)。以下待验收表述属于历史记录。

2026-11-05当前检查点：4.2已验收并提交`f3b1f01`。经用户确认，4.3.1仅实现单个1分钟～24小时倒计时、拟人化循环提醒（最多60秒、每轮间隔3秒）、提醒优先打断和独立掉电保存音量；保持PTT及8轮生成即保存历史。当前软件检查完成后交付用户实测，尚未真机验收/提交，不进入定时提醒。完整部署、编译、烧录和合并验收见[倒计时手册](COUNTDOWN.md)。下文为较早阶段记录。

最新结论（2026-11-05）：4.2合并真机验收已由用户确认全部通过，50/60读音、音量短句、收尾键和打断记忆正常，连续20轮无误超时，60秒保护正常；软件84项测试和WSS回声/4.1/4.2构建核对通过。当前准备由用户提交Git，停在4.2，不进入下一阶段。最终规则与验收结论见[4.2手册](PTT-CONTROLS.md)；下文“待复验”属当时开发记录。

2026-11-05本轮4.2待复验：修复音量上下文短句漏判（调到100/设成60）、仅TTS音量模板转换中文读数，并提供原始50/60数字/中文WAV对照；固件修复共享时间戳读取竞态，细分超时/解码/样本错误日志。70%峰值、收尾键与生成即记忆保持，操作见[4.2手册](PTT-CONTROLS.md)。

最新4.2修复（2026-11-04，待复验）：AI收尾排空时持续检查按键，已松键后的新按压跨过播报结束也可录音；用户新确认历史改为完整回答文字生成后立即提交一次，打断、TTS失败或断链仍保留已生成问答，未生成就取消的不计入8轮。`playback_done`只记播放状态。当前操作与测试见[4.2手册](PTT-CONTROLS.md)，下文早期验收记录按当时行为理解。

当前4.2已按用户确认新增PTT音量查询/调节、NVS保存及AI播报LED律动；软件检查后等待用户部署、烧录和真机验收，尚未Git提交。使用独立`ai-ptt-controls`变体，完整操作见[4.2手册](PTT-CONTROLS.md)。VAD、连续听音和全双工暂不排期。下文为已验收4.1基线说明。

4.2首轮实测反馈：灯效和重启音量保存通过；数学追问误用音量工具、静音恢复及首次发送超时在本检查点内修复。现普通对话不暴露工具，8轮历史不删减；新增设备端静音前音量保存与恢复、volume_restore能力协商、2秒有界发送及发送失败诊断。补丁等待云端/真机复验，不进入下一阶段。

本检查点按用户确认合并原ASR、LLM和TTS三个小检查点。2026-11-04已完成流式修复、ECS部署、真实模型及公网WSS自测、编译和烧录；用户明确反馈真机验收通过，首声等待明显缩短，按键打断、60秒和连续十轮正常。保留最近8轮完整历史，当前问题额外加入。本检查点已真机验收，Git提交状态以实际日志为准；按用户要求停在当前检查点。3.6.2已在6176ac9提交。

修复记录：原整段版本首声约8秒且部分回复无法播放；已复现云端首帧244字节超过沿用的227字节接收上限。现使用独立1275字节下行容量和流式播放。用户提供的新日志中，真机回合TTS首个校验块用时518～681ms（这是TTS阶段耗时，不是松键到扬声器首声的总延迟）；233～244字节帧正常发送，完整播放后有HISTORY COMMIT，stored_rounds达到8后保持8。

## 默认配置与边界

| 项目 | 默认值 |
| --- | --- |
| ASR | `qwen3-asr-flash`，录音完整收齐、校验、解码后识别 |
| LLM | `qwen-plus`，关闭思考，最多256输出Token，短句回答 |
| TTS | `cosyvoice-v2`、系统音色`longxiaochun_v2`、语速1.0 |
| 克隆兼容 | `tts_mode=clone`，使用已注册VoiceID，要求创建音色的目标模型与合成模型一致 |
| 记忆 | 最近8轮已完成的完整问答；本次输入额外加入；固定系统提示词始终保留 |
| 窗口滑动 | 第9轮请求=历史1～8轮+问题9；完整回答生成后保存2～9轮；第10轮请求=历史2～9轮+问题10 |
| 入记忆条件 | 完整回答文字生成后提交一次；播报被打断或失败仍保留，未生成回答就取消/失败则不提交 |
| 保存方式 | 每设备独立RAM窗口；网络重连保留；30分钟无交互或AI进程重启清空 |
| 长度边界 | 历史加系统提示与当前输入共64000 UTF-8字节；超限明确失败，不悄悄缩短窗口 |
| 录音/回复 | 录音0.3～60秒；回复PCM上限60秒，模型默认要求80字以内 |
| 播放方式 | LLM完整短回答后，TTS边合成/传输/解码/播放；每5个60ms帧校验一次累计帧序、字节、CRC后释放播放 |
| 起播与停顿 | 首批约294ms有效音频即可起播；缓冲耗尽时排空功放后呼吸等待，补足后接续；整轮结束熄灯 |
| 编解码状态 | 单轮连续Opus状态，首端去lookahead、尾端精确裁剪；每帧1275字节硬上限，独立于上传227字节容量 |

普通TTS默认不依赖克隆VoiceID。克隆配置有误时明确失败，不静默改用系统音色。此检查点支持使用已有克隆音色，不自动上传录音或创建新音色。

本轮继续采用专用板级入口；没有启用通用Application、MCP、VAD、全双工、NFC、学习档案或OTA。
服务最多允许2个同时连接，用于本检查点验收；设备测试Token尚不是商用激活体系，尚未做商用并发验收。

## 协议及成熟组件复用

设备仍复用原PTT、SoftAP、LED、I2S、ESP音频编解码、WSS与SNTP代码；旧echo变体保持独立协商与独立配置。
新入口`/stubuddy/ai`明确协商`ai_protocol:1`。底层复用已验收SB/版本/turn/sequence/length封装与CRC实现，不把echo/1帧直接送进原小智未适配的连接入口。

```text
hello(ai/1,reply_stream=1,max_reply_packet_bytes=1275,16000Hz/mono/60ms) → hello(session_id,确认流式参数)
listen.start(manual,turn) → SB二进制Opus帧 → listen.stop(采样数/延迟/帧数/CRC)
stage(asr) → stt → stage(llm) → reply_text → stage(tts)
reply_start(stream=1,delay,上限) → [SB二进制回复帧×最多5 → reply_chunk(累计元数据)] → reply_end(最终元数据)
完整回答文字生成时服务端滑动提交记忆；设备分段校验后连续解码/实际播放 → 最终完整性检查且实际播完 → playback_done（仅播放状态）
```

所有回合响应带会话与回合标识，二进制帧所属会话由当前连接绑定。换连接使旧连接代际失效；端侧先使旧回合失效，再接续录音。取消不等待云端确认。

TTS使用自己的采样数和编码器lookahead，回复可比问题长或短。上传CRC校验上传数据，回复CRC校验回复数据，两者不要求相等。
服务端90秒整轮生成上限；端侧45秒无业务进度、95秒未起播或155秒整轮上限取消。心跳不延长业务进度计时。
CRC检查通过的小段可以在整轮结束前播放；如果之后发生错误，立即停止剩余音频，已经生成的完整文字保留在历史中。增益取首批音频后整轮固定；2026-11-05用户要求统一提高到70%峰值（±22937），包含AI/录音回放、提示音和LED参考；实际音量/音质与收尾按键、打断记忆合并重新真机验收。
沿用已验收断网规则：整轮音频及最终CRC已收齐时，缓存可在断网后继续播完；未收齐时失败停止，恢复后不补播旧轮。`playback_done`未送达不影响已生成文字的历史，新录音等连接恢复并重新松键按下。
先升级服务器，再烧录流式固件。新固件必须握手确认流式支持，旧服务器不会被误认为支持；新服务器兼容过渡期未升级固件的整段请求，并对其输出限定227字节。

原服务端Provider的参数、音频格式、结束及取消行为已阅读核对。本入口使用成熟`httpx.AsyncClient`、`websockets`和`opuslib_next/libopus`，不整体启动原服务端的Torch/VAD/记忆/后台依赖。普通小智WebSocket、MQTT的共享Protocol契约未修改。

## 操作前提

- Windows PowerShell，按顺序运行。每段失败即停止，不继续烧录。
- ECS现有3.6.2证书、Nginx和续期服务已经正常运行。本次只新增AI服务及AI路径。
- 服务器内部监听`127.0.0.1:8766`，无需新开安全组端口；旧回声8765保持原用途。
- 此处默认百炼北京地域；API Key必须与地域、业务空间匹配。新加坡使用后面的替换说明。
- 只有GPIO15主键，没有BOOT/RST；本阶段沿用既有接线。

## 1. Windows环境、依赖及私有Key

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location -LiteralPath 'D:\Desktop\stubuddy'

python -m venv stubuddy/server/voice-ai/.venv
if ($LASTEXITCODE -ne 0) { throw 'AI独立Python环境创建失败，请保留错误。' }
$stubuddyAiPython = 'D:\Desktop\stubuddy\stubuddy\server\voice-ai\.venv\Scripts\python.exe'
& $stubuddyAiPython -m pip install -r stubuddy/server/voice-ai/requirements.txt
if ($LASTEXITCODE -ne 0) { throw 'AI依赖安装失败，请停止后续步骤。' }

& $stubuddyAiPython stubuddy/server/voice-ai/configure.py --server-ip 121.41.86.40 --region beijing --set-key
if ($LASTEXITCODE -ne 0) { throw '私有配置失败，请不要上传或烧录。' }
```

最后一条会提示输入API Key，输入时隐藏。不要把Key写入命令行、聊天或Git。预期输出`API Key configured: yes`；不会显示Key或设备Token。Key仅保存在忽略的`.private.json`及私有上传包内，固件头只含独立设备测试Token，不含DashScope Key。

若你的账户属于新加坡，最后一条中的`--region beijing`改为`--region singapore`。
若使用业务空间专属域名，在最后一条增加`--workspace-id 你的WorkspaceID`；这个ID不是API Key。无需把ID发到聊天。

此时普通合成已经是默认模式。后续命令默认保留已选地域、Key和Token，不覆盖旧回声私有文件。

## 2. ECS检查、部署及真实模型自测

以下均从Windows PowerShell发起，SSH用户名root、端口22：

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\voice-ai\Deploy-VoiceAi.ps1 -User root -Port 22
if ($LASTEXITCODE -ne 0) { throw '服务器预检查失败，请保留STOP行，先不安装。' }
```

预期`CHECK PASS`。该步骤上传私有包并检查服务器，不写系统服务或Nginx。SSH密码隐藏输入正常。端口8766被其他程序占用、原网关不匹配或已有不属于本项目的目录/服务时，检查会停止。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\voice-ai\Deploy-VoiceAi.ps1 -User root -Port 22 -Install
if ($LASTEXITCODE -ne 0) { throw 'AI部署失败，请先处理STOP行，不要烧录。' }

ssh -t -p 22 root@121.41.86.40 '/opt/stubuddy-ai/.venv/bin/python /opt/stubuddy-ai/smoke_test.py --config /opt/stubuddy-ai/.private.json'
if ($LASTEXITCODE -ne 0) { throw '真实模型自测失败，请先不烧录。' }

$stubuddyAiPython = 'D:\Desktop\stubuddy\stubuddy\server\voice-ai\.venv\Scripts\python.exe'
& $stubuddyAiPython stubuddy/server/voice-ai/check_wss.py
if ($LASTEXITCODE -ne 0) { throw '公网WSS验证失败，请保持TLS校验并反馈错误。' }
```

安装会下载Ubuntu依赖、在独立虚拟环境安装三个Python依赖，并新增`stubuddy-ai.service`。只扩展原stubuddy Nginx网关的`/stubuddy/ai`路径；Nginx检查失败时恢复其原文件。不修改证书、续期逻辑、安全组、UFW、SSH或旧回声服务。

模型自测会调用真实DashScope服务，按账户计费：先合成一句已知测试语音，再走ASR→LLM→TTS及Opus往返校验；不需要录音文件，不操作硬件，不伪造设备实际播放完成。

预期包含：

```text
INSTALL PASS: isolated AI service, loopback 8766, existing TLS gateway retained
PASS: DashScope TTS generated test input
PASS: DashScope ASR returned text
PASS: DashScope LLM returned text
PASS: first verified stream chunk before reply_end; elapsed_ms=...
PASS: real ASR/LLM/STREAM TTS, Opus decoding, checkpoint/final CRC/sample integrity; max_packet=...
PASS: WSS certificate/IP validation, ai/1 STREAM handshake (1275 bytes), heartbeat and wrong-token rejection
```

失败时先使用以下只读命令查看服务状态和已脱敏日志，不要显示私有配置内容：

```powershell
ssh -t -p 22 root@121.41.86.40 'systemctl status stubuddy-ai.service --no-pager'
ssh -t -p 22 root@121.41.86.40 'journalctl -u stubuddy-ai.service -n 50 --no-pager'
```

`stage=asr/llm/tts`指明失败环节。核对私有Key所在地域、模型是否可调用、配额和音色与模型匹配；不要关闭TLS，也不要把Key贴出来。若需重新输入Key，重跑步骤1最后一条，再使用`-Install -UpdateConfig`重新部署，明确将电脑配置更新到服务器。
公网WSS失败时先区分证书/IP验证、443连通、网关路径和服务状态，保持旧回声作为参照。

## 3. 编译和核对

仅在上述两类云端检查均PASS后继续。新开PowerShell需重新加载环境：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'

python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ai-ptt
if ($LASTEXITCODE -ne 0) { throw 'AI固件编译失败，请停止烧录。' }
python scripts/verify_stubuddy_startup.py --variant ai-ptt
if ($LASTEXITCODE -ne 0) { throw '板型/协议/分区核对失败，请反馈FAIL行。' }
python -m serial.tools.list_ports
```

预期`Project build complete`和所有核对PASS，包括`private ai/1 enabled`、严格TLS/SNTP、NVS原布局、恰好一个Board工厂。当前应用仍使用2MiB分区。

## 4. 完整写入、拔插USB启动、查看日志

关闭所有串口工具；COM4替换成实际端口。以下采用上阶段实测成功的完整写入方式，不使用差分写入、不擦NVS、不写merged整片文件：

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

预期`Hash of data verified`；`Staying in bootloader`正常。成功后不按主键拔插USB启动。USB断连时关闭串口、换可靠数据线、拔插USB并重新枚举端口，再用同一完整写入命令重试。不要使用GPIO15当下载按键。

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python -m esp_idf_monitor --no-reset --port $stubuddyPort --baud 115200 build/xiaozhi.elf
```

监视器保持打开后可再拔插USB观察完整启动日志；Ctrl+]退出。正常应看到`CHECKPOINT 4.1`、`variant=stubuddy-s3-n16r8-ai-ptt`、`TIME READY`、`AI READY: authenticated ai/1`及待机日志。

## 5. 用户真机验收

| 操作 | 通过标准 |
| --- | --- |
| 待机与按住说话 | 待机灯灭，录音常亮；松开先灭灯，识别/生成/合成等待呼吸 |
| 正常提问 | 串口有`AI STT`、`AI TEXT`、`AI STREAM START`、`AI VERIFIED`、`AI STREAM DONE ... errors=0`；实际听到完整回答 |
| 首声延迟 | `AI STREAM START first_voice_ms=...`记录松键上传结束后的起播时间；用较长回答验证START出现在AI VERIFIED之前，并与原7.7～8.1秒对比 |
| 已复现失败句 | 追问“我的玩具熊叫什么”，以及“现在音量多少”；不再因首帧244字节被拒收。音量查询/调整工具仍未实现，不以模型声称有此能力为验收依据 |
| 流式缓冲不足 | 出现`AI STREAM BUFFERING`时停止实际放音、呼吸等待，恢复后接续且不重播首句；若频繁出现，视为音频体验未验收通过并反馈日志 |
| 实际播音 | 放音常亮；完全播完关闭功放、灯灭；服务日志出现`HISTORY COMMIT` |
| 回答长于问题/短于问题 | 回复完整，不按录音长度截断，不补播多余填充；首尾清楚、结束无杂音 |
| 等待时新按下 | 取消旧回合并接续录音；松开后只回答新问题，旧结果不能恢复 |
| 放音时新按下 | 停旧音、清旧DMA/缓冲、继续按住收新音；旧回复完整文字保留历史 |
| 60秒长按 | 自动停录、先灭灯再等待并自动回答；原长按不打断自己的回复；答完仍熄灯 |
| 少于300ms/无文字 | 误触不触发模型；无识别文字正常结束，不虚构答案 |
| 上下文 | 先告诉一个测试信息再追问，能根据历史回答；使用最近8轮已完成问答，当前另计 |
| 第9/10轮窗口 | 软件测试精确核对请求与存储；真机可用明确事实进行回归，但不能只凭模型一句“忘记了”证明淘汰 |
| 录音或未收齐流式回复时断网 | 当前录音按原规则结束，未收齐的回复停止并提示错误；恢复后先松键再重新按下，旧轮不补播 |
| 回复已最终校验后断网 | 已缓存音频可播完；新录音暂停；已生成文字保留历史，恢复后需松键再重新按下 |
| 网络重连记忆 | 同一设备已完成历史保留；服务进程重启清空；不同设备不混用 |
| 连续十轮 | 无复位、杂音、持续内存下降或CODEC ERROR；错误取消不误推进窗口 |
| 配网与TLS回归 | 上电按住主键立即重新配网；历史/新Wi-Fi及成功3秒关闭热点保持；错误证书不能放行 |

服务可控重启命令（会清空当前RAM历史，先用于恢复验收）：

```powershell
ssh -t -p 22 root@121.41.86.40 'systemctl restart stubuddy-ai.service'
```

编译或电脑模型自测通过不能代替本表真机验收。请反馈失败项、对应回合号及已脱敏日志。本次真机通过后才讨论Git提交，不直接开始VAD/MCP/NFC。

## 切换已有克隆音色（可选，默认普通合成）

先拥有同账户、同地域下可用的克隆VoiceID，并知道创建它时指定的目标模型。把下面两处占位内容换成实际值；不要发送API Key：

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy'
$stubuddyAiPython = 'D:\Desktop\stubuddy\stubuddy\server\voice-ai\.venv\Scripts\python.exe'
& $stubuddyAiPython stubuddy/server/voice-ai/configure.py --tts-mode clone --tts-model 'cosyvoice-v2' --clone-voice-id '替换为已注册VoiceID' --clone-target-model 'cosyvoice-v2'
if ($LASTEXITCODE -ne 0) { throw '克隆音色配置失败，请核对模型对应关系。' }
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\voice-ai\Deploy-VoiceAi.ps1 -User root -Port 22 -Install -UpdateConfig
if ($LASTEXITCODE -ne 0) { throw '音色更新部署失败，请保留错误。' }
ssh -t -p 22 root@121.41.86.40 '/opt/stubuddy-ai/.venv/bin/python /opt/stubuddy-ai/smoke_test.py --config /opt/stubuddy-ai/.private.json'
if ($LASTEXITCODE -ne 0) { throw '克隆音色模型自测失败，先不进行真机验收。' }
```

仅切换云端音色不需要重新烧录。恢复默认普通音色：

```powershell
& $stubuddyAiPython stubuddy/server/voice-ai/configure.py --tts-mode builtin --tts-model cosyvoice-v2 --tts-voice longxiaochun_v2
if ($LASTEXITCODE -ne 0) { throw '普通音色配置失败。' }
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\voice-ai\Deploy-VoiceAi.ps1 -User root -Port 22 -Install -UpdateConfig
if ($LASTEXITCODE -ne 0) { throw '普通音色更新失败。' }
```

## 以后直接修改服务器配置

运行配置位于`/opt/stubuddy-ai/.private.json`。可调整已支持的模型、音色、语速、提示词和历史窗口参数，无需重新烧录。当前启动时读取配置，修改后需要重启服务；重启清空8轮内存历史，尚未支持热更新。设备Token和连接地址涉及固件同步，不能当普通模型参数随意更换。

首次安装使用电脑上传配置；后续`-Install`默认保留服务器现有配置。需要明确从电脑更新配置时使用`-Install -UpdateConfig`，覆盖前备份服务器旧配置到`/opt/stubuddy-ai/.private.before-update.json`。设备连接信息与电脑不一致时，默认部署会停止，避免新固件无法鉴权。

服务器修改配置后，从PowerShell重启并核对状态：

```powershell
ssh -t -p 22 root@121.41.86.40 'systemctl restart stubuddy-ai.service'
if ($LASTEXITCODE -ne 0) { throw '服务重启失败，请保留错误。' }
ssh -t -p 22 root@121.41.86.40 'systemctl status stubuddy-ai.service --no-pager'
```

预期`active (running)`；失败时查看本手册第2节的脱敏日志。不要把整个私有配置文件输出到终端或聊天。需要调整具体参数时，使用只更新指定字段的命令。

## 回退既有WSS回声固件

退出监视器后，在已加载IDF的PowerShell运行，再按步骤4完整写入。旧云端回声服务和私有配置均保留：

```powershell
Set-Location -LiteralPath 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wss-echo
if ($LASTEXITCODE -ne 0) { throw '旧WSS回声编译失败。' }
python scripts/verify_stubuddy_startup.py --variant wss-echo
if ($LASTEXITCODE -ne 0) { throw '回退核对失败，先不烧录。' }
```

若未来重新执行旧echo安装器，它可能重建整个旧网关文件；之后重跑AI安装步骤恢复AI路径。证书日常自动续期不会移除AI路径。

## 软件检查记录

- 流式AI固件ESP-IDF6.1构建与板型核对通过，约1.25MB，仍小于2MiB应用分区；C++格式检查通过。
- 服务端30项测试通过，增加未完成合成前释放首块、流式取消关闭上游、尾端精确裁剪、60秒上限、不规则PCM分块、流式十轮历史滑动、部分音频后失败不提交及下一轮恢复。包含真实libopus；本机测试DLL来自核对SHA256的Xiph官方1.5.2源码，仅在忽略的tmp内编译。
- 普通/克隆TTS事件契约、ASR/LLM请求构造使用本地替身测试，不代表用户账户已调用通过。
- 仓库主机151项回归中149项通过、2项因EspVocat/MCP主机工具和头文件条件跳过（6项TLS用IDF Python补跑通过）。包含244字节首帧存储、分段采样数/序号边界、LED/PTT/网络与旧回声；另补测静音前缀不会导致整轮静音、播放峰值限制。使用已有Git Bash/MSVC与临时目录清理适配，不改原断言。
- 旧WSS回声变体构建/核对及普通bread-compact-wifi完整应用兼容构建通过；构建目录恢复为AI变体。
- 2026-11-04用户确认本次流式云端两项自测PASS，编译、烧录和真机验收全部通过：首声明显缩短，按键打断、60秒及连续十轮正常。日志包含大于227字节的回复帧及完整播放后的历史提交，窗口达到8后保持8；精确第9/10轮消息内容由软件测试核对。Git状态以实际提交日志为准。

## 官方协议参考

- [Qwen-ASR API](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)
- [Qwen文本生成API](https://help.aliyun.com/zh/model-studio/qwen-api-reference/)
- [CosyVoice WebSocket](https://help.aliyun.com/zh/model-studio/cosyvoice-websocket-api)
- [系统音色列表](https://help.aliyun.com/zh/model-studio/cosyvoice-voice-list)
- [克隆音色与合成目标模型匹配](https://help.aliyun.com/en/model-studio/cosyvoice-clone-api-reference)
