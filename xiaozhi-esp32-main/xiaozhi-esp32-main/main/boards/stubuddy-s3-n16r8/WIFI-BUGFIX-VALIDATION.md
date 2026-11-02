# 3.4.1：首次门户探测、连接进度与失败音

本次仍属于未提交的3.4配网检查点，待用户验收后与3.4一起提交。

## 修复内容与边界

- 首次门户：Wi-Fi控制任务提前准备，AP接口启动后先启用HTTP和DNS，最后放行DHCP；不再让刚获取IP的手机探测到尚未就绪的门户。常见HTTP探测使用302跳转。
- 扫描/提交/连接中：页面显示轻量动画进度条和当前状态，不显示虚构百分比、不抢焦点，不触发自动连接；成功或失败后停止。
- 成功：统一改为3秒关闭设备热点，并更新页面文案。手机系统控制门户窗口是否自动收起，设备不能保证关闭普通浏览器标签页。
- 失败：LED闪烁并播放880→660→440Hz下降三音，使用此前验证过的音频频段；错误提示不会被快速重试成功覆盖。音效仍由同一音频任务串行播放，当前录放完成后再播。
- 保留历史、人工配网等待、无自动聚焦、原生密码输入、网络录音门禁、60秒上限和回放结束熄灯规则。

## PowerShell编译烧录

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$env:IDF_CCACHE_ENABLE = '0'
Set-Location 'D:\Desktop\stubuddy\xiaozhi-esp32-main\xiaozhi-esp32-main'
python scripts/build.py stubuddy-s3-n16r8 --name stubuddy-s3-n16r8-wifi-ptt
if ($LASTEXITCODE -ne 0) { throw '编译失败，请停止烧录并反馈错误日志。' }
python scripts/verify_stubuddy_startup.py --variant wifi-ptt
if ($LASTEXITCODE -ne 0) { throw '构建核对失败，请反馈 FAIL 行。' }
python -m serial.tools.list_ports
```

COM4替换成实际端口。执行烧录前按住GPIO15主按键，保持到串口显示setup=FORCED再松开：

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

预期有 `CHECKPOINT 3.4` 及 `SETUP REVISION: 3.4.1`。
不擦除Flash，不写merged-binary，保留NVS历史。

## 本次重点验收

1. 看到 `PORTAL READY: HTTP and DNS listening; releasing AP DHCP` 后直接连接stubuddy热点，不再次拔插USB。串口中该行应先于DHCP分配客户端IP的日志出现。
2. 确认手机首次连接能识别配网门户并弹页。探测到达设备会记录 `PORTAL PROBE: redirect=302`，实际访问页面有 `SETUP PAGE: served`。
3. 页面扫描附近网络时有进度条；点击连接后立即显示提交/连接进度。等待不自动聚焦密码框、不自动提交旧网络。
4. 输入一个长度合法但错误的密码（例如至少8位），点击连接：失败后进度条停止、显示错误，LED闪烁，并听到一次明显的下降三音。串口应有 `CUE: FAILED; tones=3; 880/660/440 Hz`。
5. 立即用正确密码重试：仍能听到失败提示及随后成功提示，连接成功文案显示3秒，热点约3秒后关闭。没有提示音重叠或关机杂音。
6. 待机/录音/回放灯效、网络等待暂停新录音、当前一轮完整结束、60秒自动回放和松键保护保持正常。
7. 重复普通上电与强制配网，验证人工配网不会抢连旧网络，历史记录仍可一键重连。

手机已经记住某次门户处理结果时，可能使用缓存。若仍不弹页，先保留本次从启动到手机连接的日志，再测试“忽略/忘记此网络”后的首次加入。
手动入口仍为连接设备热点后的 `http://192.168.4.1/`。系统是否自动弹窗仍须实际手机验收，软件构建不能替代这一项。

重新打开日志使用不复位命令，避免无意触发一次普通开机：

```powershell
idf.py -p $stubuddyPort monitor --no-reset
```

主机测试122项：120通过、2项因环境跳过；其中页面回归26个JS用例。
新增规则测试验证HTTP→DNS→DHCP顺序、服务失败时不放行DHCP、3秒常量、失败音保留和下降音型。
本阶段全部真机验收通过后，再按 [3.4完整验收说明](WIFI-VALIDATION.md) 建立Git检查点。
