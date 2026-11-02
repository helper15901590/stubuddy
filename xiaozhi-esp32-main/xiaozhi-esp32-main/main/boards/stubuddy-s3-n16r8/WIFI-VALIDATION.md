# 检查点3.4：SoftAP配网与本地录放

前置检查点3.3已由用户验收并提交 `e8ec267`。
本次新增 `stubuddy-s3-n16r8-wifi-ptt`；3.1、3.2、3.3变体继续保留。
仍是本地按键录放，联网后不会上传语音或调用模型。

当前修订为3.4.2：配网3.4.1修复已由用户验收；本次补验按键打断接续收音，以及60秒停录后确认熄灯再回放。
见 [PTT-INTERRUPT-VALIDATION.md](PTT-INTERRUPT-VALIDATION.md)。此前配网修复记录见 [WIFI-BUGFIX-VALIDATION.md](WIFI-BUGFIX-VALIDATION.md)。

## 已确认的工作流

| 场景 | 行为 |
| --- | --- |
| 无已确认的历史配置 | 开启免密码stubuddy-XXXX热点，呼吸灯等待选择 |
| 普通上电，有已确认配置 | 自动连接最近成功网络，热点保持关闭；约20秒内未连上则进入人工配网 |
| 按住GPIO15主按键上电 | 在提示音和网络初始化之前采样并锁定强制配网，仅30ms消抖，无5秒长按门槛 |
| 人工配网 | 页面刷新、扫描、等待和输入法动作都不能自动提交旧网络；必须点击列表或连接按钮 |
| 成功获取IP且保存完成 | 提示成功，松键保护通过后启用本地录放；热点另延后约3秒关闭，供手机接收成功结果 |
| 空闲时断网 | 关闭麦克风，暂停新录音；后台最多重连约20秒，失败后进入人工配网 |
| 录音或回放时断网 | 当前一轮完整结束，用户主动打断除外；之后等待网络，恢复后必须先松键，再按下录音 |

人工配网始终优先于历史自动重连。强制配网的意图只在启动时锁定，提示音期间松开主按键不会撤销。
正常运行中的主按键仍用于录音，不会因为长按而进入重新配网。

## 页面与存储

- 页面沿用已验收的白色极简设计，设备内嵌全部资源，不加载外部字体或脚本。
- “连接过的网络”和“附近的Wi-Fi”分别直接展示，选中行有标记，无下拉菜单。
- 点击历史列表中的网络立即重连，不打开密码框；新网络使用原生可编辑密码框。
- 页面进入、列表刷新和后台轮询不主动聚焦、不抢输入焦点、不清除正在输入的密码。
- 成功历史最多五条，最近成功的排最前。页面和串口均不返回或打印保存的密码。
- 沿用NVS命名空间stubuddy_wifi，以及history、setup_done和旧station记录迁移方式；二进制结构保持兼容。
- 新网络获取IP并保存成功后才更新历史；输错密码或超时保留旧配置。NVS异常时保留数据并报错，不自动擦除。

设备热点地址为 `http://192.168.4.1/`，只在连接设备热点时使用。
设备连入家庭网络后的STA地址由路由器分配，日志中的另一IP并不是配网页面地址。
系统是否自动弹出门户由手机决定；未弹出时，在保持连接设备热点的状态下手动打开上述HTTP地址。
如果手机提示“无互联网”，选择保持连接，以便访问本地页面。

## 灯效和提示音

网络等待/连接中呼吸；错误短暂三连闪后回到等待灯效。
网络正常时保留3.3全部录放灯效：待机灭、录放常亮、准备回放灭、回放结束无论按键是否按住均熄灭。
处于网络等待时显示呼吸灯，与联网后的本地待机熄灯区分。

开机提示音仅一次。进入配网为440→440Hz，连接成功660→880Hz，连接失败880→660→440Hz下降三音，沿用3%峰值和安静收尾。
网络任务不操作I2S；由唯一录放任务在音频空闲时播放提示，期间暂停新录音。
配网/成功提示音常亮，失败提示音闪烁，结束后恢复当前网络状态。普通网络状态合并为最新一条；失败提示单独保留到播放，快速重试成功不会覆盖掉错误音。

断网事件立即撤销新录音许可，并记录连接轮次。即使网络快速恢复，也要重新通过松键保护。
录音已经开始后不再按网络状态截断本轮，仍遵守松开回放、60秒上限和误触过滤。

## 电脑PowerShell：编译与核对

先Ctrl+]退出旧串口监视，然后执行：

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'

python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wifi-ptt
if ($LASTEXITCODE -ne 0) { throw '编译失败，请停止烧录并反馈最后的错误日志。' }

python scripts/verify_stubuddy_startup.py --variant wifi-ptt
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请反馈 FAIL 行。' }

python -m serial.tools.list_ports
```

预期 `Project build complete`，核对全部PASS，首行包含stubuddy-s3-n16r8-wifi-ptt，
另有Wi-Fi enabled with SoftAP provisioning。核对同时检查BLE禁用、无屏幕/摄像头/字体、唯一工厂与入口、NVS及烧录范围。

## 首次烧录：直接验证强制配网

将COM4改成实际端口。执行烧录命令前按住此前录音用的GPIO15主按键，
保持到串口出现 `BOOT BUTTON: setup=FORCED` 后松开。这能检查烧录后第一次启动不会抢连旧Wi-Fi。

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

常规flash保留NVS；无需erase-flash，也不要使用merged-binary整片写入。
已有有效历史和确认标记时，不按键普通上电自动重连是预期行为，不代表首次配网失效。

强制配网关键日志：

```text
STUBUDDY_PTT: BOOT BUTTON: setup=FORCED; sampled before startup prompt; history retained
STUBUDDY_PTT: CHECKPOINT 3.4; ... variant=stubuddy-s3-n16r8-wifi-ptt
STUBUDDY_WIFI: SETUP MODE: manual; automatic reconnect paused until explicit selection
STUBUDDY_WIFI: SETUP AP AUTH: OPEN (no hotspot password)
STUBUDDY_WIFI: SETUP URL: http://192.168.4.1 (2.4 GHz, SoftAP only)
STUBUDDY_PTT: NETWORK WAIT: new recordings paused; microphone OFF
```

手机连接stubuddy-XXXX，打开页面后先等待至少60秒并刷新列表。
未点击连接前不应出现CONNECT REQUEST、source=portal或source=saved，也不应出现REC START。

历史点击重连会看到action=reconnect；新网络提交为action=connect。成功后应出现：

```text
STUBUDDY_WIFI: CONNECTED: ip=...; saved=...; AP closes after 3 seconds
STUBUDDY_WIFI_AUDIO: CUE: CONNECTED; ... serialized with PTT
STUBUDDY_PTT: NETWORK READY: local PTT available after a fresh release gate
STUBUDDY_PTT: WAIT RELEASE: ...
STUBUDDY_PTT: IDLE: ...
STUBUDDY_WIFI: SETUP AP CLOSED; saved WiFi will reconnect after power cycle
```

## 真机验收清单

1. 按上面的首次烧录方式强制配网：免密码热点、呼吸灯；等待60秒、刷新或轻点按键，都不自动连接旧网或开始录音。
2. 页面首次进入不主动弹键盘；两类列表可直接查看且区分明显，选中效果明确。
3. 点击“连接过的网络”中的可用SSID：直接重连，无密码弹窗；成功后热点约3秒关闭。
4. 再次强制配网，选一个新网络：手动点密码框能正常输入；刷新状态不抢焦点、不清空密码；成功后进入历史。
5. 新网络故意输错密码：失败提示、错误闪烁，随后恢复配网呼吸；旧历史仍保留，等待用户重试，不自行连接旧网。
6. 退出monitor，不按主按键重新上电：自动连最近成功网络；日志source=saved，正常启动不开放设备热点。
7. 正在录音时关闭测试路由器Wi-Fi或测试手机热点，开发板保持供电：继续说完并松键，当前录音完整回放，之后不接受新录音。
8. 单独测试短时断网：在约20秒重连窗口内恢复网络，应自动重连；也可在配网刚成功、热点尚未关闭的3秒内测试一次。网络等待期间按住键，恢复后先松开再重新按下才能录音。
9. 单独测试持续断网：保持关闭超过25秒，应进入配网呼吸等待；恢复路由器后仍等人工点击历史网络，不抢连。重新连接后录放恢复。
10. 网络正常时回归误触过滤、60秒自动回放、回放期间按键和结束后松键保护；连续10轮录放清楚安静，无复位/资源持续下降。
11. 检查GPIO10/GPIO18无冲突、无I2S错误、无panic/内存不足；网络提示音与录音/回放不能重叠。

第7项录音断网测试与第8/9项短断网、长断网测试可分开做。网络恢复提示要等当前录放结束才播放。
新设备无记录时的启动分支由规则测试覆盖；现有开发板保留历史即可验证强制重新配网，无需为测试而擦除记录。

## 重新打开日志与故障处理

按住主按键插USB重新配网时，无需等5秒；看到配网呼吸灯即可松手。
如果在设备已经进入配网后才重新打开monitor，使用不复位的命令，以免额外复位变成普通上电：

```powershell
idf.py -p $stubuddyPort monitor --no-reset
```

只有连接设备热点时192.168.4.1才是本地入口。手机自动离开无互联网热点时，重新连接并选择保持连接。
端口占用先退出其他monitor/串口工具；下载连接失败使用此前验证过的BOOT/RESET方法。
页面已过期提示时关闭后重新打开本地页面，获取本轮令牌。偶发recv 104通常是手机主动关闭连接；若配网正常可继续验收。
若出现FAILED、panic、GPIO冲突、手动配网中抢连或网络等待仍触发录音，保留从启动开始的日志反馈。

需要回到3.3做对照，在同一主工程目录执行：

```powershell
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-ptt-led
if ($LASTEXITCODE -ne 0) { throw '3.3编译失败。' }
python scripts/verify_stubuddy_startup.py --variant ptt-led
if ($LASTEXITCODE -ne 0) { throw '3.3核对失败。' }
idf.py -p $stubuddyPort -b 115200 flash monitor
```

## 软件验证及提交

主机套件122项，120项通过、2项因本机编译器/头文件条件跳过；页面回归包含26个JavaScript用例，涵盖连接进度显示与结束状态。
历史校验、损坏记录、NVS结构兼容、输入校验、DNS边界、强制配网、人工等待和网络录音门禁均有测试覆盖。
普通ESP32-S3 Wi-Fi/OLED板型及3.1、3.2、3.3、3.4四个stubuddy变体均构建并通过身份核对。
3.4镜像仍在原1MiB应用分区内，实际大小以核对脚本为准；NVS仍为0x9000+0x6000，未扩展或擦除原有分区。
核对确认唯一管理组件为cJSON，未引入屏幕/摄像头/BLE/字体；格式、Python语法和Git空白检查通过。
无线连接、手机弹页、音质、断网过程和实际资源稳定性仍待用户真机验收。

仅在用户确认本阶段验收通过后执行：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git status --short
git add -- xiaozhi-esp32-main/xiaozhi-esp32-main stubuddy/firmware/speaker-selftest/DEVELOPMENT-ROADMAP.md
git diff --cached --stat
git commit -m "feat(board): integrate Wi-Fi setup and interruptible PTT" -m "Verify captive onboarding, three-second portal completion, saved history, network-gated PTT, held-key interrupt-to-record handoff, and confirmed LED-off transition at sixty seconds."
git log -1 --oneline
git status --short
```

暂存区只包含源码、测试和文档，不应包含build、sdkconfig、managed_components或tmp。
实测通过并提交后，再讨论3.5 Opus编解码检查点。
