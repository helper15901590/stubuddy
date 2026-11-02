# 3.4.2：按键打断接续收音、60秒熄灯确认

本次按用户确认的两个规则实现：按下打断后继续按住说话、松开结束；满60秒先停录灭灯，再自动回放。
仍在3.4检查点内，配网修复已由用户验收，但本次新增行为还需验收，之后一起提交Git。

## 当前功能与正式AI的边界

当前stubuddy变体是联网后的本地录放，没有ASR、LLM或TTS云端会话。
本次让本地回放模拟AI播报，验证按键打断与收音切换。原3.1/3.2/3.3验证变体仍保留原来的用途；
新增按键打断只在wifi-ptt变体启用。

小智主工程已有手动PTT处理入口：StartListening事件在Speaking状态发送AbortSpeaking并切入手动收音。
ToggleChatState的Speaking分支只发中止，并不与上述手动PTT入口等价。
本次没有启动通用Application对话循环，也没有宣称端云打断已验证。

正式AI接入时遵循以下约定，作为私有WS阶段的验收项：

1. 有效按下GPIO15时，立即停止本机旧TTS、清除播放/解码排队数据，取消云端旧回复。
2. 同一次按下进入新一轮收音；继续按住说话，松开结束；有效短按同样会停播。
3. 新回合使用独立回合标识，云端晚到的旧回复/旧音频不能重新播放。
4. 最长60秒自动停止收音，先灭灯交接；等待模型时按既定规则呼吸，实际TTS播放时常亮。
5. 分别验证端侧停播速度、云端取消、旧帧过滤、首字清晰度及断网中止。

## 本次本地行为

| 操作 | 行为 |
| --- | --- |
| 待机按住、松开 | 原PTT录放流程 |
| 回放中重新按下并继续按住 | 30ms消抖后静音，删除I2S1/TX DMA，再打开PDM收音；松开后只回放新录音 |
| 回放中仅短点一下 | 停止旧回放；新录音不足300ms则丢弃，返回待机，旧回放不会续播 |
| 满60秒时按键仍按住 | 旧按键不会打断自己触发的自动回放；需先松开，再次按下才算新打断 |
| 回放正常结束时仍按住 | 保持熄灯，仍需先松开再按下 |
| 网络未就绪时按键打断 | 停止旧回放；暂停新录音，回到联网等待，恢复后重新通过松键保护 |

有效短按需要超过30ms消抖时间。开机、配网和错误提示音仍串行完成，配网等待不启动新录音。
链路已恢复时，用户主动打断接续收音优先于尚未播放的网络状态提示；这些提示等音频空闲后再播。

快速停播复用AudioCodec接口，stubuddy适配器先关闭功放再删除TX，不走正常停播的150ms静音排空。
新收音立即保留采样，不再丢弃200ms预热数据或等待同一按键先释放。
缓冲区复用原有单块PSRAM，新录音只按自己的采样数量回放；中止链结束后统一清除旧尾部数据。
首字清晰度、实际切换时间及打断有无爆音必须真机验证。

## 60秒灯效修复

停录使用独立CAPTURE_STOPPED状态，普通网络等待不能把它改回呼吸或常亮；错误闪烁仍优先。
停录后等待LED定时器确认PWM已经更新，再允许后续回放亮灯。
满60秒额外保留至少150ms的熄灯交接；实际回放开始后才常亮。
正常松键也等PWM熄灯确认，但不增加150ms交接时间。

对应日志顺序：

```text
STUBUDDY_LED: CAPTURE_STOPPED/OFF
STUBUDDY_PTT: REC STOP: samples=960000, duration=60000 ms, reason=LIMIT; microphone OFF
STUBUDDY_PTT: LIMIT: 60 seconds reached; LED OFF applied + 150ms; automatic replay
STUBUDDY_LED: PLAYBACK/ON
STUBUDDY_PTT: REPLAY START: ...
```

## 电脑PowerShell

退出旧monitor（Ctrl+]），执行：

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

预期Project build complete和全部PASS。COM4替换实际端口：

```powershell
$stubuddyPort = 'COM4'
idf.py -p $stubuddyPort -b 115200 flash monitor
```

确认日志包含SETUP REVISION: 3.4.2。已有保存的网络会正常重连，无需擦除NVS。
若需强制配网，沿用按住GPIO15再上电的方式。重新打开日志可使用：

```powershell
idf.py -p $stubuddyPort monitor --no-reset
```

## 本次真机验收

1. 网络连接后录音5秒并松开，进入回放；回放中重新按下，继续按住说“这是第二段录音”，松开。旧回放应立即停止，只回放第二段。
2. 检查第二段第一个字是否清楚；日志有REPLAY INTERRUPTED、OUTPUT ABORT、INPUT HANDOFF及新REC START，切换时间会打印在INPUT HANDOFF中。
3. 回放中快速轻点但超过30ms：旧回放停止，不会续播。新录音不足300ms应DROP，松开后恢复待机。
4. 连续做三次“回放中按下接续录音”，最后正常结束。不能混入旧片段，也不能卡在等待松键或发生I2S错误。
5. 按满60秒且不松手：停录后看到明显灭灯交接，再自动回放并常亮，旧按键不能把回放立刻打断。
6. 在这次自动回放中先松开，再重新按下并说话：应能打断并接续新录音。
7. 回放正常结束后无论按键是否按住都熄灭，待机不自行亮灯；联网等待另按呼吸灯规则。
8. 已在回放时关闭测试网络，再按键：旧回放可停止，但不能开始新录音；恢复网络后松开再按才录音。
9. 连续十轮录放/打断：音质清晰、首字可辨、正常结束安静、打断无明显爆音，无复位/GPIO冲突/资源持续下降。
10. 配网首次弹页、等待进度、成功3秒关闭热点及错误音继续正常。

如果出现首字缺失、切换爆音、60秒后没有灭灯交接或旧音频续播，请保留从REC START到异常后的完整日志，继续修复本检查点。
本地回放不会产生真实模型回复；端云取消和晚到旧音频过滤到私有WS接入时实测。

主机测试122项：120通过、2项环境条件跳过；新增断言验证旧长按不误打断、消抖、新按下单次打断、松开结束、停录熄灯和错误优先级。
普通小智ESP32-S3 Wi-Fi/OLED及stubuddy的3.1、3.2、3.3、3.4变体构建/核对通过。
本阶段全部实测通过后，先按 [3.4提交说明](WIFI-VALIDATION.md) 提交Git，再讨论3.5 Opus。
