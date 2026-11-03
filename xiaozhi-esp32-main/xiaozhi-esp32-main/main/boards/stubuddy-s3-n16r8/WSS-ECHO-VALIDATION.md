# 检查点3.6.2：私有ECS/WSS音频回声

3.6.1已真机验收并提交 `5df51a4`。本检查点新增独立变体
`stubuddy-s3-n16r8-wss-echo`；局域网变体和它的Token/配置继续独立保留。
用户已反馈ECS部署、生产证书签发、续期演练和电脑公网WSS自测PASS。开发板刷写曾在应用写入29%时USB断连，随后使用USB自动下载完整写入1246128字节并通过校验。2026-11-03用户反馈“测试成功”，待建立Git检查点。
验收确认按用户本次反馈记录；下方分项表保留供后续复测。

实物约束：只有一个GPIO15主按键，没有BOOT或RST实体按键。主键上电用于配网，不能替代GPIO0下载信号。

## 工作流程

```text
Wi-Fi → 联网SNTP校时 → 证书链/目标IP/有效期校验 → WSS鉴权与hello → 待机熄灯
按住PTT → 增量Opus上传 → 松开或满60秒停录、先灭灯
等待ECS回声呼吸 → 完整接收并校验帧数/CRC/采样数 → 解码回放常亮 → 结束熄灯
等待或播放时新按下 → 取消旧回合并接续收音 → 松开结束
```

公网路径：设备 → ECS TCP443/Nginx TLS → 127.0.0.1:8765回声程序。
校时失败时保持网络等待；30秒后提示故障一次并持续等待，恢复后继续。
校时服务定期更新系统时间，TLS始终校验证书有效期。错误证书不会回退明文连接。
满60秒、误触过滤、松键保护、断网和旧回合处理沿用已验收的3.6.1。

没有域名时采用Let’s Encrypt的公网IPv4证书。证书约六天有效，部署包括每小时检查续期、
续期后验证并重载Nginx、续期演练及剩余有效期检查。设备内使用SDK根证书包，
不会将六天有效的服务器叶证书固定进固件。

## 一、电脑环境和SSH信息

在已登录ECS的终端执行 `whoami` 和 `echo "$SSH_CONNECTION"`：
用户名是第一条的输出，SSH端口是第二条四项中的最后一项。
无需发送密码、私钥或Token。电脑后续操作均从PowerShell执行。

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location 'D:\Desktop\stubuddy'

python -m pip install -r stubuddy/server/ws-echo/requirements.txt
if ($LASTEXITCODE -ne 0) { throw '依赖安装失败，请保留错误输出。' }

$stubuddySshUser = 'root'
$stubuddySshPort = 22
Test-NetConnection -ComputerName 121.41.86.40 -Port $stubuddySshPort -InformationLevel Quiet
```

## 二、只检查服务器

上一步预期True。若False，先在安全组核对TCP22，只允许电脑当前公网IP/32，
并核对服务器SSH监听和防火墙。通过云端工作台登录成功不等于电脑能直接连通公网22。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\ws-echo\Deploy-CloudEcho.ps1 -User $stubuddySshUser -Port $stubuddySshPort
if ($LASTEXITCODE -ne 0) { throw '服务器检查未通过，请先保留STOP错误，不要烧录。' }
```

脚本生成独立云端Token及固件头，将明确列出的源码和配置打包，上传到当前SSH用户的
`.stubuddy-wss-stage`目录（目录权限700）。安装前的检查不会修改系统服务。
SSH和sudo可能分别询问登录密码，输入时不显示字符是正常现象。

预期包含 `CHECK PASS: Ubuntu 24.04`。80、443或8765被其他程序占用时会STOP；
已有不同IP/不属于本项目的安装目录也会STOP。保留错误输出，先核对已有业务再安排兼容部署。
安装脚本不改变SSH、安全组和UFW。

## 三、放行入口并部署

阿里云控制台进入该ECS的安全组 → 入方向 → 添加TCP80、TCP443，来源0.0.0.0/0。
80只提供证书自动验证路径，443提供WSS；8765只监听服务器内部，无需添加安全组规则。
保留已有8000/8003等规则。

如服务器另有UFW防火墙，先查看状态：

```powershell
$stubuddySshTarget = "${stubuddySshUser}@121.41.86.40"
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo ufw status verbose'
```

若显示 `Status: active`，再执行下面两行；若inactive则继续部署：

```powershell
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo ufw allow 80/tcp'
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo ufw allow 443/tcp'
```

部署命令会在电脑端提示填写证书账户联系邮箱，保存在被Git忽略的云端配置中：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\stubuddy\server\ws-echo\Deploy-CloudEcho.ps1 -User $stubuddySshUser -Port $stubuddySshPort -Install
if ($LASTEXITCODE -ne 0) { throw '部署或公网自测失败，请保留最后错误，先不要烧录。' }
```

预期依次包含：

- `INSTALL PASS: verified TLS entry, echo service, hourly renewal and renewal dry-run`
- `PASS: WSS certificate/IP verification, authentication, framing and CRC match.`

首次安装会下载Ubuntu依赖、Python依赖和Certbot 5.9.0，需要几分钟。
先用ACME测试环境签发验证，但不安装测试证书；再获取生产可信证书供Nginx使用。
签发超时/验证失败时核对公网80、安全组、UFW和错误详情，修正后重跑同一命令；云端Token保持不变。
脚本只为自身配置留备份到 `/var/backups/stubuddy-echo/`，不替换已有业务配置。

## 四、编译和烧录

必须先让上一节公网WSS自测PASS。关闭电脑上的局域网回声服务，避免混淆测试路径。
Ctrl+]退出旧串口监视器，再在同一个已加载IDF环境的PowerShell执行：

```powershell
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wss-echo
if ($LASTEXITCODE -ne 0) { throw 'WSS编译失败，请停止烧录并保留错误。' }

python scripts/verify_stubuddy_startup.py --variant wss-echo
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请反馈FAIL行。' }

python -m serial.tools.list_ports
```

预期全部PASS，包括证书包/目标校验/有效期/SNTP以及原NVS布局。
将COM4改为实际串口：

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

普通flash保留已配网记录。启动日志应包含：

```text
CHECKPOINT 3.6.2; ... variant=stubuddy-s3-n16r8-wss-echo
WSS ECHO: wss://121.41.86.40/stubuddy/echo; token not logged
TIME WAIT: synchronize before verified TLS; recording paused
TIME READY: epoch=...; CA/peer/validity checks enabled
WS READY: authenticated echo/1; 16000Hz mono/60ms
IDLE: hold GPIO15 to record
```

一直TIME WAIT：核对该Wi-Fi能否上网、DNS和UDP123/NTP；校时失败时不会跳过验证。
TIME READY之后TLS失败：检查服务器证书、IP是否改变、服务器时间和续期状态。
不要把 `.cloud.json`、压缩包、生成头文件或Token内容贴到聊天/串口日志里。

## 烧录过程中USB断连恢复（无BOOT/RST按键）

当前日志识别接口为USB-Serial/JTAG，使用原生USB自动下载。写入途中断连时，
关闭其他串口工具，拔插USB重新上电，不按主键；重新枚举COM端口。
优先使用可靠数据线直连电脑USB接口，排查传输连接和供电稳定性。
在已加载IDF环境的PowerShell执行，将COM4换成实际串口：

```powershell
python -m serial.tools.list_ports
$stubuddyPort = 'COM4'
& {
    $ErrorActionPreference = 'Stop'
    Push-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main\build'
    try {
        python -m esptool --chip esp32s3 -p $stubuddyPort -b 115200 --before usb-reset --after no-reset write-flash "@flash_args"
        if ($LASTEXITCODE -ne 0) { throw '重新烧录失败，请保留最后30行输出。' }
    }
    finally {
        Pop-Location
    }
}
```

此命令使用USB自动进入下载模式，完整重写已有三个固件文件，不使用差分写入。
清单不包含NVS。各文件写入和校验成功后，拔插USB正常启动；需要观察日志时使用
`python -m esp_idf_monitor --no-reset --port COM4 --baud 115200 build/xiaozhi.elf`，
该监视命令从小智主工程根目录执行，串口也须改为当前实际值。
自动下载仍失败或没有COM端口时，先记录设备枚举和最后错误，不能用GPIO15主键当作BOOT键。

## 五、真机验收

| 测试 | 通过标准 |
| --- | --- |
| 跨网络 | 电脑局域网服务关闭，设备连手机热点或其他网络，仍收到ECS真实回声 |
| 音频完整 | 服务端与设备帧数、CRC、字节数、采样数一致，errors=0，首尾清楚、结束无杂音 |
| 60秒 | 停录先灭灯，再等待呼吸/自动回放；960000采样，原长按不打断自身回放 |
| 等待/回放打断 | 新有效按下取消旧回合，继续按住收音，松开后只回放新回合 |
| 服务恢复 | 重启回声服务后设备自动重连，恢复后先松键再重新录音，旧回合不补播 |
| 断网恢复 | 收音时断网仍完整结束本地收音，当轮失败；连接恢复后新按键正常 |
| 连续十轮 | 无复位、杂音、持续内存下降或CODEC ERROR，errors=0 |
| 配网回归 | 首次弹页、历史网络、错误密码提示、成功3秒关闭热点均正常 |
| 证书续期 | timer已启用，续期服务成功，dry-run通过；剩余有效期足够 |

每轮仍核对 `ECHO VERIFIED ... source=SERVER`、`REMOTE DECODE ... errors=0`、
`REPLAY STOP: amplifier OFF; LED OFF`，与3.6.1相同。

从PowerShell查看或重启服务：

```powershell
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo systemctl status stubuddy-echo.service stubuddy-cert-renew.timer --no-pager'
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo journalctl -u stubuddy-echo.service -n 40 --no-pager'
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo systemctl restart stubuddy-echo.service'
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo systemctl list-timers stubuddy-cert-renew.timer --no-pager'
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo systemctl status stubuddy-cert-renew.service --no-pager'
```

如果回声太快，不容易测试等待时打断，可在PowerShell执行下面第一行，临时增加3秒等待。
完成等待打断测试后执行第二行恢复0秒。命令只重启本项目回声服务，设备会自动重连：

```powershell
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo python3 .stubuddy-wss-stage/install_ecs.py --test-delay 3'
ssh -t -p $stubuddySshPort $stubuddySshTarget 'sudo python3 .stubuddy-wss-stage/install_ecs.py --test-delay 0'
```

服务器整机重启后的开机自启需要另做实测，不能用enable成功替代此项。

## 实现与软件验证记录

- 设备新增严格WSS变体，SDK根证书包校验，两个SNTP服务器；云端与局域网私有配置分离。
- 新WSS固件编译和链接核对通过，1246128字节，沿用2MiB应用分区和NVS布局。
- 本机真实TLS套接字测试通过正常IP证书回声、未知CA拒绝、错误IP拒绝、过期拒绝、错误Token拒绝和明文降级拒绝。
- 部署检查测试覆盖占用端口拒绝、Nginx配置检测失败时恢复原文件；打包测试覆盖明确文件清单、局域网配置不变、云端Token重用与无效输入保护。
- PowerShell部署脚本语法检查通过。ECS上的Nginx/systemd/Certbot实际运行与证书签发由部署步骤验证；本机TLS测试不替代设备WSS实测。
- 完整主机回归151项：149通过、2项因主机编译器命令/开发头文件条件跳过；云端配置、部署检查和真实TLS正反例10项另行复测通过。
- 局域网回声、原Opus和启动变体兼容编译/核对通过；普通bread-compact-wifi完整应用兼容构建通过，当前build目录已恢复为wss-echo。

## 回退局域网版本

Ctrl+]退出串口，确认原电脑服务仍可启动，再执行：

```powershell
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ws-echo
if ($LASTEXITCODE -ne 0) { throw '局域网回退编译失败。' }
python scripts/verify_stubuddy_startup.py --variant ws-echo
if ($LASTEXITCODE -ne 0) { throw '回退构建核对失败。' }
idf.py -p $stubuddyPort -b 115200 flash monitor
```

## 官方参考

- [Let’s Encrypt IP证书与Certbot要求](https://letsencrypt.org/2026/03/11/shorter-certs-certbot)
- [Certbot续期说明](https://eff-certbot.readthedocs.io/en/stable/using.html#renewing-certificates)
- [Nginx WebSocket代理与超时](https://nginx.org/en/docs/http/websocket.html)
