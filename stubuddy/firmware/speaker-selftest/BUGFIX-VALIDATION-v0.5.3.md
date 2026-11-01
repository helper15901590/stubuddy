# v0.5.3 历史验收记录

本文件保留旧版说明。当前以 [v0.5.4验收说明](BUGFIX-VALIDATION-v0.5.4.md) 为准。

本版修复配网时意外连接历史网络、密码框不能正常输入，并加入选中反馈和清晰的列表分类。
仍处于SoftAP配网检查点；不开始云端、NFC或下一固件阶段。

## 连接规则

| 情况 | 设备行为 |
| --- | --- |
| 普通开机，存在已保存网络 | 先仅启用STA连接最近成功网络，不广播配网热点 |
| 按住GPIO15主按键上电 | 立即进入人工配网，持续等待选择，不自动连接历史网络 |
| 无配置、或自动连接失败后进入配网 | 人工配网持续等待；扫描和等待不会启动历史连接 |
| 点击附近网络 | 仅选中并展开可编辑表单；即使该SSID已保存，也不自动连接 |
| 点击表单“连接” | 提交用户输入，日志action=manual-connect |
| 点击历史区“重连” | 使用设备保存的密码，日志action=history-reconnect |
| 自动填充、回车、轮询、刷新 | 不提交连接请求 |

进入人工配网后，只有显式连接成功且配网服务关闭，才恢复正常运行的后台重连策略。
网页输入框使用原生可编辑控件，没有readonly开关或针对某类手机的焦点补丁；初次输入区隐藏且不自动focus。

用户补充日志source=portal表明当时请求来自网页，旧日志不能区分是密码提交还是历史重连。
本版分别记录action便于定位；action请求头是界面版本/动作检查，不是新的设备身份鉴权。

## PowerShell编译与烧录

```powershell
$env:PYTHONUTF8 = '1'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'D:\Desktop\stubuddy\stubuddy\firmware\speaker-selftest'
python -m serial.tools.list_ports
```

找到开发板串口号。以下COM4仅为示例：

```powershell
idf.py -p COM4 -b 115200 reconfigure build flash monitor
```

预期Project build complete，烧录成功后日志版本v0.5.3。不要erase-flash，保留既有网络历史。
Ctrl+]退出日志。如果端口被占用先关闭其他串口窗口。
页面出现“配网页已更新”提示时，关闭旧配网页后重新打开 `http://192.168.4.1`，以加载本版页面。

## 必测1：人工配网保持等待

1. 保持之前成功连接的家庭Wi-Fi开启，作为能否误重连的测试条件。
2. 按住GPIO15主按键再插USB，上电出现下列提示后松开：

```text
BOOT BUTTON: immediate WiFi setup requested; saved networks retained
SETUP MODE: manual; automatic reconnect paused until explicit selection
```

3. 手机连接stubuddy热点，进入页面，保持该热点连接，不点任何网络，等待至少45秒。
4. 预期热点不关闭、页面不跳为成功、LED持续呼吸；该SETUP MODE之后没有CONNECT REQUEST或CONNECT attempt日志。
5. 点击“附近的Wi-Fi”中的已保存网络，预期只显示选中背景、左边框和勾选标记，展开输入区；再等10秒仍不连接。

若手机自行切回家庭网络，先确认串口是否真的有新的CONNECT REQUEST，区分手机网络选择和设备发起连接。

## 必测2：密码框原生输入

1. 在附近列表选择一个需要密码的新网络，确认只选中该项，之前的选中项取消。
2. 选中时不自动弹键盘；主动点击密码框，应可输入。
3. 输入密码后等10秒，确认字符、光标和键盘不因后台轮询被清空或关闭。
4. 点击键盘回车/完成不应发起连接；随后点击页面“连接”，才出现：

```text
CONNECT REQUEST: action=manual-connect; accepted from setup page; credentials not logged
CONNECT attempt=1; source=portal; credentials not logged
```

5. 正确密码应取得IP、成功音、约五秒后关闭热点。
6. 若仅用现有网络测试，可通过“手动添加网络”输入SSID和密码；该入口同样必须允许原生键盘输入。

## 必测3：历史重连与分类

1. 再次按住上电进入配网。
2. 上方浅灰卡片为“连接过的网络”，标注“已保存密码 · 点击重连直接连接”；下方白底为“附近的Wi-Fi”，标注“先选择，再点击连接”。
3. 点击历史区目标网络，预期只发起一次请求，不需重新输入密码：

```text
CONNECT REQUEST: action=history-reconnect; accepted from setup page; credentials not logged
```

4. 成功前保留所选项的背景/边框/勾选。连续快速点击不能重复提交。

## 必测4：正常开机与回归

1. 不按主按键重新上电，已存网络可用时应出现：

```text
BOOT MODE: saved-network connect; setup AP stays off
CONNECT attempt=1; source=saved; credentials not logged
CONNECTED: ... setup AP stays off
```

2. 正常自动连接过程中不会先开放一个随后又被抢走的配网页。
3. 完成一次短录放，确认声音清楚、录放常亮、待机灭灯。
4. 可选：普通启动时关掉测试路由，20秒失败后出现配网热点；恢复路由，等待45秒仍不应自动连走，明确点击历史重连后才连接。

如果仍异常，反馈v0.5.3启动行、SETUP MODE之后的CONNECT REQUEST/action和CONNECT/source行；不要提供家庭密码。

## 软件检查与实际边界

网页脚本测试20项；C++编译期测试新增普通启动、强制配网、失败进入配网与完成退出的连接策略检查。
无窗口浏览器使用真实触摸事件检查原生输入、隐式提交拦截、附近已保存项不自动重连、历史显式重连和小屏布局。
这些检查不等同于所有手机系统或真实射频环境均已验收，以上真机步骤仍需完成。

## 通过后建立Git检查点

仅在上述必测确认通过后执行：

```powershell
Set-Location 'D:\Desktop\stubuddy'
git add -- stubuddy/firmware/speaker-selftest
git diff --cached --stat
git commit -m "feat(wifi): add captive setup with explicit connection control" -m "Add open-hotspot onboarding, saved-network history, native password input, selection feedback, and stable manual setup sessions. Verify setup, reconnect, and audio regression on hardware."
git log -1 --oneline
git status --short
```

核对暂存清单包含源码、网页、测试、验收和全景文档；不应有build、浏览器预览数据或实际Wi-Fi密码。
通过、提交后，先讨论小智产品固件整合的最小工作流，再进入下一检查点。
