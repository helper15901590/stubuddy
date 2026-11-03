# 检查点3.6.1：私有局域网WebSocket音频回声

3.5已真机验收并提交a49b3ff。当前新增独立固件变体 `stubuddy-s3-n16r8-ws-echo`。
本检查点已于2026-11-03通过用户真机验收，已提交5df51a4；服务运行在自己的Windows电脑，不调用ASR、LLM或TTS。
3.6.2私有ECS/WSS部署在本检查点验收和Git提交后另行讨论。

用户实测结果：局域网回声正常，帧数、CRC和采样数一致，errors=0；60秒自动回放、等待和回放时打断、服务重启及断网恢复均正常，连续十轮无异常、无杂音。下方操作步骤保留供复测和回退后验证。

## 工作流程

```text
同一路由器下的ESP32与Windows电脑
  Wi-Fi连接 → WS鉴权与hello协商 → 待机熄灯
  按住GPIO15 → PDM/Opus录音，每60ms上传一帧 → LED常亮
  松开或满60秒 → 麦克风关闭、LED先熄灭 → 发送真实长度和结束标记
  等待电脑返回 → 呼吸灯 → 收齐本轮并核对帧数/字节数/CRC
  真实返回的Opus解码 → 按原始采样数裁剪 → 回放常亮 → 结束熄灯
```

- 继续保留300ms误触过滤、60秒上限、至少150ms的满时灭灯交接和松键保护。
- 等待回声或解码时的新有效按下取消旧回合；继续按住录新一轮，松开结束。回放中的打断同样保留。
- 满60秒后始终保持原按键，不会打断自己触发的回声回放；需先松开、重新按下才算新打断。
- 旧回合编号的晚到音频会被丢弃，旧回声不会重新播放。
- 录音过程中失去连接：当前收音继续到松开/60秒，当轮失败，不自动重传，也不切回本地回环。
- 回声已完整接收后断网：本轮解码/回放可完成；新录音等待连接恢复，恢复后先松键再重新按。
- WS故障提示由同一音频任务在空闲时播放，每次故障持续期间只提示一次；后台逐渐延长重试间隔，最多约30秒。
- 固件沿用2MiB应用分区，NVS仍为0x9000+0x6000，应用起点0x10000；配网记录保留。

## 准备两个PowerShell窗口

设备与电脑连接同一家庭路由器，电脑接网线或Wi-Fi均可。不要使用会隔离设备的访客网络。
窗口A持续运行回声服务；窗口B用于配置、自测、编译和烧录。

先在窗口B执行：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location 'D:\Desktop\stubuddy'

python -m pip install -r stubuddy/server/ws-echo/requirements.txt
if ($LASTEXITCODE -ne 0) { throw '回声服务依赖安装失败，请反馈错误输出。' }

python stubuddy/server/ws-echo/configure.py
if ($LASTEXITCODE -ne 0) { throw '局域网配置失败，请按下方说明手动选择电脑IPv4。' }
```

预期输出 `Configured: ws://电脑IPv4:8765/stubuddy/echo`，不打印Token。
这不是配网页面的192.168.4.1；它是设备连接的电脑服务地址。

脚本选择有网关、已连接的物理网卡，避开VPN/虚拟网卡。若电脑同时连接多块物理网卡或无法查询时，手动查看本机地址：

```powershell
Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway } | Select-Object InterfaceAlias, @{Name='IPv4'; Expression={$_.IPv4Address.IPAddress}}
```

如该命令受电脑策略限制，可执行ipconfig，查看正在使用的WLAN或以太网中的IPv4和默认网关。

将下面示例替换成连接该家庭路由器的电脑IPv4，再执行：

```powershell
python stubuddy/server/ws-echo/configure.py --server-ip 192.168.1.100
```

配置脚本写入两份被Git忽略的本地文件：

- `stubuddy/server/ws-echo/.local.json`：电脑端地址、端口和随机测试Token。
- 小智工程的 `tmp/stubuddy_echo_local.h`：固件编译时读取同一地址与Token。

再次运行配置会保留原Token。改变电脑IP后重新配置、编译和烧录。Token不放进配网页面或串口日志。
本地测试Token用于当前通道验收，设备激活、账户绑定和商用Token管理仍在后续阶段。

## 窗口A：启动回声服务

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy'
python stubuddy/server/ws-echo/echo_server.py
```

预期显示 `LISTEN 0.0.0.0:8765/stubuddy/echo`、`authentication enabled` 和 `RAM-only echo`。
保持此窗口运行。Ctrl+C停止服务。

Windows第一次询问是否允许Python通过防火墙时，选择当前受信任的专用网络。
正常运行时服务器输出会话、帧数、字节数、采样数和CRC，不保存录音文件，不打印Token。

## 窗口B：先验证电脑服务，再编译

```powershell
Set-Location 'D:\Desktop\stubuddy'
python stubuddy/server/ws-echo/smoke_test.py
if ($LASTEXITCODE -ne 0) { throw '电脑端WS自测失败，请先检查窗口A、电脑IPv4和防火墙。' }
```

预期 `PASS: WS connection, authentication, framing and CRC match.`。
此自测发送合成测试字节，验证网络协议；真实Opus和音质由开发板验收。

先Ctrl+]退出任何旧串口监视器，再执行：

```powershell
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ws-echo
if ($LASTEXITCODE -ne 0) { throw '编译失败，请停止烧录并保留错误日志。' }

python scripts/verify_stubuddy_startup.py --variant ws-echo
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请反馈FAIL行。' }

python -m serial.tools.list_ports
```

预期Project build complete，核对全部PASS，包含WebSocket private echo/1 enabled。
没有生成本地配置头文件时，编译会明确提示先运行configure.py。

## 烧录和观察日志

COM4替换为实际端口：

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

沿用普通flash命令，不擦除Flash，也不用merged-binary整片写入。窗口A保持运行。

启动应出现：

```text
STUBUDDY_PTT: CHECKPOINT 3.6.1; ... variant=stubuddy-s3-n16r8-ws-echo
STUBUDDY_OPUS: SELFTEST PASS: silent RAM encode/decode; ... errors=0
STUBUDDY_WS: LAN ECHO: ws://电脑IPv4:8765/stubuddy/echo; token not logged; ...
STUBUDDY_WS: WS READY: authenticated echo/1; 16000Hz mono/60ms; ...
STUBUDDY_PTT: IDLE: hold GPIO15 to record; ...
```

每轮有效录音应出现：

```text
STUBUDDY_PTT: REC STOP: samples=...; microphone OFF
STUBUDDY_OPUS: UPLINK ENCODED: round=...; samples=...; frames=...; ... errors=0
STUBUDDY_WS: ECHO VERIFIED: turn=...; frames=...; bytes=...; samples=...; crc32=...; source=SERVER
STUBUDDY_OPUS: REMOTE DECODE: round=...; decoded_samples=...; errors=0; ...
STUBUDDY_PTT: REPLAY START: ...
STUBUDDY_PTT: REPLAY STOP: amplifier OFF; LED OFF
STUBUDDY_PTT: ROUND DONE: ... internal_free=... PSRAM_free=... recording erased
```

窗口A有相同turn、frames、bytes、samples、crc32的 `ECHO OK`。
每轮REC STOP、ECHO VERIFIED和REMOTE DECODE采样数应一致，60秒为960000。

## 真机验收

| 测试 | 操作 | 通过标准 |
| --- | --- | --- |
| 基本往返 | 录2～5秒，说“开头清楚，最后一个字是好” | 首尾清楚、时长正确；服务端/设备帧数、CRC一致；结束无杂音 |
| 误触 | 小于300ms轻点 | DROP、不回放，服务端取消该轮，随后能正常录放 |
| 随意长度 | 多次录0.4秒、1.1秒、较长句子和中间停顿 | 不截尾，不多播补零尾部，采样数一致 |
| 一分钟 | 原按键始终按住超过60秒 | 先停录灭灯交接，再等待回声/自动回放；960000采样；原长按不打断，回放结束后仍熄灯 |
| 回放打断 | 播放时重新按住，立即说“第二段开头”，再松开 | 旧回声立即停止，只回放新段，首字清楚 |
| 连续打断 | 连续三次回放中打断，最后正常结束 | 无旧声音混入或恢复，无I2S错误，最后熄灯 |
| 等待打断 | 按下方方法让服务器延迟3秒，等待时按住说新句子 | 等待呼吸，可取消并立即接续收音，旧编号声音不播放 |
| 服务关闭/重启 | 窗口A Ctrl+C，再启动同一服务 | 设备错误灯/音提示一次，然后等待呼吸、暂停新录音；恢复后先松键再按 |
| 收音时断网 | 录音中断开测试Wi-Fi，继续说完并松开 | 收音完整结束，当轮失败且不回放本地录音，不自动补传；恢复后新按键正常 |
| 回放时断网 | 完整回声已收到、正在播放时断网 | 本轮可播完，新录音暂停；恢复后通过松键保护 |
| 连续十轮 | 混合短录放与打断 | 无复位、资源耗尽、持续内存下降、CODEC ERROR或杂音；errors保持0 |
| 配网回归 | 按主键上电进入配网，测历史网络/错密码 | 保留首连弹页、进度、3秒关闭热点、错误音与灯效行为 |

等待打断测试：窗口A Ctrl+C停止服务，重新执行：

```powershell
python stubuddy/server/ws-echo/echo_server.py --echo-delay 3
```

完成后Ctrl+C，再以不带参数的普通命令启动，恢复正常回声延迟。

## 连接失败排查

1. 检查窗口A是否仍运行、配置脚本输出的IPv4是否属于连接家庭路由器的网卡。
2. 先在窗口B重跑smoke_test.py；出现拒绝连接通常是服务未启动/端口不一致。
3. 自测通过但设备连接失败时，检查电脑与ESP是否在同一路由器，路由器是否限制设备互访。
4. 如需建立防火墙规则，在管理员PowerShell中仅为专用网络、本地子网、Python程序放行8765端口：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$stubuddyPython = python -c "import ctypes; p=ctypes.create_unicode_buffer(4096); ctypes.windll.kernel32.GetModuleFileNameW(None,p,len(p)); print(p.value)"
Get-NetConnectionProfile
if (-not (Get-NetFirewallRule -DisplayName 'stubuddy WS echo - private LAN' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName 'stubuddy WS echo - private LAN' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8765 -Program $stubuddyPython -RemoteAddress LocalSubnet -Profile Private
}
```

这条规则只对专用网络生效。若当前家庭网络显示Public，请先确认这是自己的受信任网络，再在Windows网络设置中选择“专用”。
不要用关闭整个防火墙的方式排查。更换服务端口时，规则也应调整相同端口。
规则使用Python实际运行的程序路径，避免虚拟环境启动器与真实Python进程路径不同而导致放行无效。

认证一直失败时，用同一份.local.json重新运行configure.py，再编译/烧录，使两端测试Token一致。不要把Token或.local.json内容贴到日志里。

## 协议和实现范围

- hello沿用小智的transport/audio_params表达，另明确协商echo_protocol=1；二进制头为16字节 `SB/version/kind/turn/sequence/payload_length`，整数字段使用网络字节序。
- 这是双方实现的stubuddy echo/1测试扩展，不能直接把该二进制流接到未适配的原小智AI服务。
- WebSocket传输采用固定版本官方esp_websocket_client 1.8.0，参考[官方API](https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html)。此前通用小智WebSocket/MQTT实现与旧板型保持原用途。
- 板级WsEcho接入现有WifiSetup，仍使用专用验证入口；不启动通用Application、MCP、ASR或模型会话。
- 最多1001个Opus包，一次分配的返回包缓存复用3.5的存储；上传队列8项存PSRAM，音频任务不等待网络发送。
- WS回调只组装有大小/JSON深度限制的消息，校验回合/帧序/CRC；实际播放和提示音由同一音频任务串行拥有。
- 每5秒应用心跳；握手10秒、回声15秒无进展判失败；发送500ms与网络操作2秒超时，后台带随机错开的退避重连。
- 服务器默认最多4个连接，每连接至多一分钟音频，默认不压缩WebSocket，不保存录音文件。当前服务为局域网测试，不宣称已完成商用部署。

## 软件验证记录

- 最终主机测试共140项：138通过、2项因主机编译器命令/开发头文件条件跳过；包含真实本机WS连接、鉴权拒绝、60秒包数、CRC错误拒绝、取消部分录音、等待/正在回传时取消、旧帧丢弃及双客户端隔离。
- C++主机断言覆盖同一份实际协议头解析、CRC增量计算、16字节网络字节序、分块重组、大小/JSON深度限制和采样数/帧数关系。
- 本地配置测试覆盖物理网卡选择歧义、IP校验、重复配置保留Token、不打印Token及无效输入不覆盖配置。
- 已在本机临时启动实际echo_server.py，并使用同一份真实本地Token运行smoke_test.py，结果PASS；服务随后停止。此项只验证电脑端，不代表开发板网络或音质已验收。
- 当前电脑有VPN/虚拟网卡，配置脚本实际识别出有网关的物理WLAN地址；本地Token文件与配置头的Git忽略规则已核对。
- 新固件1170064字节，编译/链接/板型/2MiB分区检查通过，NVS布局保持不变；五个旧stubuddy验证变体及普通小智完整应用兼容构建通过，当前build目录已恢复为ws-echo。
- C++格式与Git空白检查通过。用户随后完成真机验收，结果见本文开头；电脑端软件自测与设备实测分别记录。

## 回退3.5

先Ctrl+]退出monitor，在小智主工程执行：

```powershell
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wifi-opus-ptt
if ($LASTEXITCODE -ne 0) { throw '回退版本编译失败。' }
python scripts/verify_stubuddy_startup.py --variant wifi-opus-ptt
if ($LASTEXITCODE -ne 0) { throw '回退版本核对失败。' }
idf.py -p $stubuddyPort -b 115200 flash monitor
```

3.6.1全部真机验收通过后先提交Git，再讨论3.6.2私有ECS的WSS加密通道。
