# B：统一工具执行层

本轮只改服务端，不新增 MCP 网络服务、不改固件和 NVS。

## 入口与边界

- `tool_registry.py`：八项当前设备工具的目录、能力条件、严格参数校验及参数结构；四项旧倒计时工具仅用于旧固件兼容，不在当前目录公开。
- `definitions(capabilities)`：内部工具清单。说明文字从集中提示词加载。不能把这份低层清单原样暴露给不可信客户端。
- `tool_execution.execute_tool(...)`：绑定当前连接的传输回调、能力、会话、回合；每次只执行一次，不自动重试。保留传输层的 session/turn/call_id 匹配及迟到回执过滤。
- `tool_execution.create_reminder(...)`：参数校验、音频缓存准备、执行、创建结果核对；音频失败/取消不会进入写操作。
- `device_tools.answer_with_tools` / `reminder_tools.answer`：继续承担自然语言理解、澄清及业务规则；未来适配器必须经过这些规则或等价的受检业务规划层。

未来 MCP 适配需要另行实施身份授权、业务参数转换和协议测试。当前不是 MCP 服务；缓存 ID、设备选择和会话上下文不允许由模型/外部客户端随意提供。内部调用编号仅用于追踪，不是跨重启幂等键，也不代表已经具备完整商用鉴权。

## 错误和日志

`TOOL EXEC` 包含 session、turn、operation、name、outcome、elapsed_ms，不记录参数正文或凭据。
`TOOL PREPARE` 为缓存准备耗时。现有 ASR/LLM/TTS 分段计时保留。

结果包括 confirmed、capacity、spacing、not_ready、device_rejected、unknown_tool、invalid_arguments、unsupported_capability、device_unavailable、invalid_receipt、setting_receipt_mismatch、cancellation_receipt_mismatch、creation_receipt_mismatch、cancelled、read_timeout、write_result_unknown、transport_failure、internal_error。

写操作超时/连接异常只表示结果不确定，不自动重试。用户恢复网络后先查询状态。已被设备执行的动作不会因按键取消而自动撤销。
设备业务拒绝保持原来的中文提示；软件异常保持现有错误音与新按键重试流程。

## 集中说明文字

新增五个可覆盖键：`tools.description.reminders.get`、`.countdown`、`.start`、`.cancel`、`.volume`（均以前面的完整 tools.description.reminders 前缀开头）。无占位符，最大 1000 字符。
音量工具继续使用 `device_tools.prompt.global.001/002/003`。修改文字不会改变参数、能力及硬限制。

## 验收

1. 云端原 smoke_test.py --controls --math --restore，reminder_smoke.py --time-queries，公网 check_wss.py --scheduled 均须通过。
2. 真机依次查音量、设五十/六十、静音/恢复、独立设提醒音量；确认读音与实际音量。
3. 无事项一分钟倒计时，到点有铃声与“时间到啦”，按键停止；另一轮验证三分钟自动结束。
4. 一次性与指定星期提醒；缺时间追问；统一查询报事项和时间，“还剩多久”报剩余时间；取消指定条目正确。
5. 三十秒/超过二十四小时拒绝；已有倒计时不重复建；第四个定时提醒及不足五分钟间隔拒绝。测试后删除测试提醒。
6. 等待/播报打断、收尾按键、八轮历史、打断记忆、六十秒保护、连续二十轮、断网恢复。
7. 提示词覆盖与重启加载维持正常。服务重启会清空服务器内存对话，不清除设备保存的提醒/音量。
8. 网络出错且操作结果不确定时，重连后先查询，不能根据错误音断定设备没有执行。

软件回归不使用真实 Key 或付费模型。云端真实模型与硬件验收由用户执行；通过后再 Git 提交。
