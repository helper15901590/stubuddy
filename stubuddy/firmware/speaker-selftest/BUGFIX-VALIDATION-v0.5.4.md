# v0.5.4 历史重连与首次迁移验收

## 本版行为

- 点击“连接过的网络”中的名称：立即选中并使用设备内保存的密码重连；不展开密码表单、不唤起键盘。失败时也只显示状态。
- 点击“附近的Wi-Fi”：选中并展开原生密码输入区；只有点击“连接”才提交。历史密码已变化时，从附近列表选择同名网络更新密码。
- 列表保留分组标题、网络名称和选中标记；删除每行重复的重连/已保存/需要密码文字及两组说明段落。等待时不显示重复状态说明，连接/错误/成功时才显示反馈。
- 老版只有历史网络记录，没有明确的配网完成状态。本版新增NVS键stubuddy_wifi/setup_done：缺失时首次进入人工配网，历史网络仍可点选；明确连接并取得IP且持久化成功后才写1。
- 该标记是持续保留的配网状态，不按固件版本反复重置。首次迁移后普通上电可自动连接；GPIO15按住上电仍强制进入持续等待的人工配网。

这里的“首次”指尚无setup_done完成标记的设备，不是承诺每次重刷相同镜像都会清除配网完成状态。

## PowerShell编译与烧录

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
python -m serial.tools.list_ports
```

确认开发板端口；以下COM4为示例，按实际替换：

```powershell
idf.py -p COM4 -b 115200 reconfigure build flash monitor
```

预期Project build complete，随后启动版本v0.5.4。不要erase-flash，保留历史网络。
关闭手机旧配网页再重新连接stubuddy热点；未自动弹页时打开 `http://192.168.4.1`。
Ctrl+]退出串口监视。端口占用时关闭其他串口工具再试。

## 验收A：第一次从旧版升级

1. 原家庭Wi-Fi保持开启，第一次升级到本版后先不点击任何网络。
2. 旧版无完成标记时应出现：

```text
SETUP CONFIRMATION: missing; saved networks remain available for selection
BOOT MODE: first setup confirmation required; automatic reconnect paused
SETUP MODE: manual; automatic reconnect paused until explicit selection
```

3. 手机连接设备热点，页面放置45秒。预期保持呼吸、热点不关闭、没有CONNECT REQUEST或CONNECT attempt，历史列表仍可见。
4. 在尚未连接成功前断电再上电，应继续等待人工选择，不能因重启跳过首次确认。
5. 选择正确的历史网络并成功连接后应出现SETUP CONFIRMED。
6. 再普通上电，应允许source=saved自动连接；完成标记不反复写入。

## 验收B：历史项不弹密码

1. 点击历史网络名称，预期出现勾选与背景高亮，直接发起action=history-reconnect；整个过程中没有密码框，也不弹键盘。
2. 连接成功后保持原成功音和约五秒关闭热点流程。
3. 按住GPIO15上电重新进入配网，先选附近的新网络展开输入区，再点击历史项；之前的输入区必须立即收起。
4. 可选：暂时关闭某个历史网络再点选它。失败后仅显示错误，不自动弹出密码框；仍可重新选择其他历史项。
5. 要更新历史密码时，在附近列表选择同名SSID或用手动添加入口，不通过历史区弹出表单。

## 验收C：精简与回归

1. 页面上方浅灰卡片为历史网络，下方白底为附近Wi-Fi，只有标题和SSID列表，没有逐行重复说明。
2. 附近新网络点击后有选中标记，密码框可以主动点击输入；等待后台轮询不会打断输入。
3. 点击页面连接后才出现action=manual-connect；错误密码不覆盖历史。
4. 首次确认完成后，按住GPIO15上电，等待45秒仍不应连走；松开后正常浏览列表。
5. 完成一次短录放，确认原声音、按键和灯效正常。

## 软件验证与边界

22项网页检查通过，新增历史连接成功/失败不展开密码区、从已展开的新网络切到历史项立即隐藏表单、列表与待机状态文案精简。
C++编译期检查覆盖有历史但无完成标记时禁止自动连接、完成标记保留后恢复正常启动重连。
本地浏览器用真实触摸检查点击历史项时发请求瞬间和等待/失败时编辑器均隐藏，输入和选中反馈回归正常。
射频、手机软键盘和首次NVS迁移需按上面步骤真机确认。没有记录到最新完整日志时，不据此断言此前每次意外重连的唯一原因。

## 通过后提交

```powershell
Set-Location 'D:\Desktop\stubuddy'
git add -- stubuddy/firmware/speaker-selftest
git diff --cached --stat
git commit -m "feat(wifi): add captive setup with explicit connection control" -m "Add open-hotspot onboarding, direct history reconnect, setup confirmation, native password input, and compact network lists. Verify onboarding and audio regression on hardware."
git log -1 --oneline
git status --short
```

先完成本版验收再执行提交；检查生成物和实际家庭密码不在暂存清单。
