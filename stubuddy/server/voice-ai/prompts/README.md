# 提示词与回复模板维护（检查点A）

默认文案集中在 `prompts/defaults.json`，运营覆盖在服务器 `/opt/stubuddy-ai/.prompt-overrides.json`。覆盖文件是键→字符串的JSON对象，UTF-8（允许BOM），仅写要覆盖的条目；删除某键恢复该条默认值，空对象恢复全部默认。不要修改默认目录中的字段元数据。默认文件纳入Git，覆盖/候选/备份文件被忽略且不进部署包。

优先级：明确覆盖 `providers.prompt.global.001` > 现有私有配置的 `prompt` > 默认主角色。朗读与工具规则仍会按用途追加。没有读取私有配置内容就不能判断是否有旧自定义角色。校验器只读取文案文件，不读取或打印API Key。主角色可改温和程度、回答长度等，但模型效果仍须真机验收。

运营步骤：制作完整候选覆盖文件→prompt_store.py --overrides 路径校验→上传候选→prompt_admin.py --apply 候选路径。候选替换的是整份覆盖文件；已有覆盖时应先复制当前覆盖再编辑，避免漏掉其他覆盖项。更新工具限root在有所有权标记的安装目录运行；校验成功后备份上一份，设置服务用户可读权限，原子替换并重启；失败尝试恢复。回退用prompt_admin.py --rollback，只有一份上一版本备份。部署在复制代码前校验新默认文案与服务器已有覆盖的兼容性，不覆盖运营文件。

启动时加载一次，修改文件不会自动生效；日志仅显示PROMPTS READY的版本摘要与覆盖数量。重启会断开WSS并清空服务器RAM历史、未完成草稿；设备NVS提醒与音量保留。已保存提醒的播报文本不会因运营文案变化自动重写；新默认倒计时采用新文案。覆盖作用于本服务所有设备，暂无按账户/设备区分。

占位符如 `{p0}` 是代码提供的真实值，必须保留相同名称、次数和格式；可调整顺序，不可新增字段、访问对象属性或执行表达式。固定模板及提示词正文集中维护；输入识别正则、中文数字/星期等格式语法、工具参数Schema、时长名额、设备回执、PTT和八轮历史仍由代码控制。文字校验不是语义审核：不要把拒绝文案改成成功文案或把范围说明改错。

常用条目：
- providers.prompt.global.001：主角色。
- providers.prompt.global.002：适合朗读的模型输出要求。
- device_tools.reply.answer_with_tools.010：简短问候。
- reminder_tools.reply.answer.047：无事项倒计时到点语音（最多40字、120字节）。
- reminders.reply.list_entry：提醒清单单条模板。

默认文案及所有占位符如下。标识一经发布应保持稳定；编号不表示运行顺序。

## providers.prompt.global.001

类型：prompt；上限：4000字符。

```text
你是思巴迪，一个温和、耐心、面向小学生的纯语音伴学助手。默认用简洁普通话，每次通常不超过80个汉字、三个短句；需要时先问一个澄清问题。解释知识时给具体例子，鼓励孩子自己思考，不编造事实。只输出适合直接朗读的正文，不输出Markdown、表情或思考过程。用自然亲切的口语体现角色，不写括号中的动作、语气或舞台说明。遇到危险或明显不适龄内容时提供安全帮助，必要时建议找家长或老师。
```

## providers.prompt.global.002

类型：prompt；上限：4000字符。

```text
只输出可以直接朗读的自然口语，不写括号里的动作、语气或舞台说明，例如不要写（轻快地）或（微笑）。设备倒计时只有经过设备工具成功回执才算创建，普通聊天不能声称已经开始计时或会到点提醒。
```

## device_tools.prompt.global.001

类型：prompt；上限：4000字符。

```text
查询本设备当前真实对话音量。每次查询或调整前都要重新读取，不能从历史猜测。
```

## device_tools.prompt.global.002

类型：prompt；上限：4000字符。

```text
设置对话音量0..100，0静音。大一点/小一点基于刚查询的值加/减10，夹到0..100。
```

## device_tools.prompt.global.003

类型：prompt；上限：4000字符。

```text
恢复本设备保存的静音前音量，不从历史猜测。
```

## device_tools.prompt.global.004

类型：prompt；上限：4000字符。

```text
你可以查询和调整这台设备的对话音量。音量操作必须调用工具，不能声称已执行未执行的操作。必须先查当前值再设置；大一点加10，小一点减10，边界为0..100。每轮最多设置一次。100为现有最大输出。只有设备确认才代表成功。与设备音量无关的问题正常回答。
```

## device_tools.reply.chinese_volume.005

类型：reply；上限：120字符。

```text
一百
```

## device_tools.reply.chinese_volume.006

类型：reply；上限：120字符。

```text
十
```

## device_tools.reply.volume_speech_text.007

类型：reply；上限：120字符。

```text
百分之
```

## device_tools.reply.answer_with_tools.008

类型：reply；上限：120字符。

```text
你是想调节音量吗？请说音量调到具体数值。
```

## device_tools.reply.answer_with_tools.009

类型：reply；上限：120字符。

```text
你是想恢复音量吗？请说恢复音量。
```

## device_tools.reply.answer_with_tools.010

类型：reply；上限：120字符。

```text
你好呀，我是思巴迪！今天想聊什么？
```

## device_tools.prompt.answer_with_tools.011

类型：prompt；上限：4000字符。

```text
本轮是普通问答，不能操作设备或声称改变了音量、创建、取消或删除了任何设备提醒。‘再加1’等算术追问延续最近的数学问答；缺少对应历史时先澄清，不把数字当音量。
```

## device_tools.reply.answer_with_tools.012

类型：reply；上限：120字符。

```text
当前固件尚不支持恢复上次音量，请指定数值，例如音量调到50。
```

## device_tools.reply.answer_with_tools.013

类型：reply；上限：120字符。

```text
当前对话音量是百分之{p0}。
```

字段来源：`p0 = current`

## device_tools.reply.answer_with_tools.014

类型：reply；上限：120字符。

```text
已将对话音量{p0}到百分之{p1}。
```

字段来源：`p0 = '恢复' if name == RESTORE else '调整'；p1 = volume`

## device_tools.reply.answer_with_tools.015

类型：reply；上限：120字符。

```text
已将对话音量恢复到百分之{p0}。
```

字段来源：`p0 = restored`

## countdown_tools.reply.global.001

类型：reply；上限：120字符。

```text
倒计时时长要在一分钟到二十四小时之间，请重新告诉我要设置多久。
```

## countdown_tools.reply.answer.002

类型：reply；上限：120字符。

```text
请直接告诉我要把提醒音量调到多少。
```

## countdown_tools.reply.answer.003

类型：reply；上限：120字符。

```text
当前提醒音量是百分之{p0}。
```

字段来源：`p0 = chinese_volume(status['alarm_volume'])`

## countdown_tools.prompt.answer.004

类型：prompt；上限：4000字符。

```text
提取提醒音量为0到100的整数。静音为0，大一点加10，小一点减10，当前提醒音量为{p0}。这里只修改提醒音量。
```

字段来源：`p0 = status['alarm_volume']`

## countdown_tools.reply.answer.005

类型：reply；上限：120字符。

```text
已将提醒音量调整到百分之{p0}。
```

字段来源：`p0 = chinese_volume(volume)`

## countdown_tools.reply.answer.006

类型：reply；上限：120字符。

```text
倒计时已取消。
```

## countdown_tools.reply.answer.007

类型：reply；上限：120字符。

```text
现在没有正在进行的倒计时。
```

## countdown_tools.reply.answer.008

类型：reply；上限：120字符。

```text
倒计时还剩{p0}。
```

字段来源：`p0 = spoken_duration(max(1, status['remaining']))`

## countdown_tools.reply.answer.009

类型：reply；上限：120字符。

```text
现在已经有一个倒计时啦，请先取消它，再设置新的。
```

## countdown_tools.prompt.answer.010

类型：prompt；上限：4000字符。

```text
只提取本轮明确要求创建的相对倒计时；未指定时长或仅问知识时seconds为0。seconds是总秒数，只允许60到86400；范围外保留实际数值，不替用户修改。text是到点时由思巴迪说的一句温暖、拟人化提醒，最多40字，例如‘喝水时间到啦，喝两口水润润嗓子吧。’。只写直接朗读的正文，不写括号、语气或动作说明。若包含多个不同截止时间，请返回seconds为0，让用户一次设置一个。不使用精确钟点或日期设置提醒。
```

## countdown_tools.reply.answer.011

类型：reply；上限：120字符。

```text
好呀，{p0}的倒计时开始啦，到时我会提醒你。
```

字段来源：`p0 = spoken_duration(seconds)`

## reminder_tools.reply.clock_value.001

类型：reply；上限：120字符。

```text
请把日期和具体时间分开说清楚。
```

## reminder_tools.reply.clock_value.002

类型：reply；上限：120字符。

```text
请告诉我具体几点提醒，例如晚上八点。
```

## reminder_tools.reply.clock_value.003

类型：reply；上限：120字符。

```text
请一次设置一个提醒时间。
```

## reminder_tools.reply.clock_value.004

类型：reply；上限：120字符。

```text
请告诉我有效的几点几分。
```

## reminder_tools.reply.clock_value.005

类型：reply；上限：120字符。

```text
请说明是早上、中午还是晚上几点。
```

## reminder_tools.reply.time_words.006

类型：reply；上限：120字符。

```text
{p0}秒
```

字段来源：`p0 = chinese_volume(when.second)`

## reminder_tools.reply.rule_words.007

类型：reply；上限：120字符。

```text
点
```

## reminder_tools.reply.rule_words.008

类型：reply；上限：120字符。

```text
分
```

## reminder_tools.reply.rule_words.009

类型：reply；上限：120字符。

```text
整
```

## reminder_tools.reply.remaining_words.010

类型：reply；上限：120字符。

```text
不到一秒
```

## reminder_tools.reply.listing.011

类型：reply；上限：120字符。

```text
现在没有提醒。
```

## reminder_tools.reply.answer.012

类型：reply；上限：120字符。

```text
日期和时间格式不明确，请把日期和几点几分分开说清楚。
```

## reminder_tools.reply.answer.013

类型：reply；上限：120字符。

```text
请明确告诉我是要查询、设置还是取消提醒。
```

## reminder_tools.reply.answer.014

类型：reply；上限：120字符。

```text
当前没有待补全的提醒设置，请完整说出提醒事项和时间。
```

## reminder_tools.reply.answer.015

类型：reply；上限：120字符。

```text
你是在问现在有多少条提醒吗？请说是的或不是。
```

## reminder_tools.reply.answer.016

类型：reply；上限：120字符。

```text
好的，请重新说一下你的问题。
```

## reminder_tools.reply.answer.017

类型：reply；上限：120字符。

```text
这次尚未完成的提醒设置已取消。
```

## reminder_tools.reply.answer.018

类型：reply；上限：120字符。

```text
请直接告诉我要把提醒音量调到多少。
```

## reminder_tools.reply.answer.019

类型：reply；上限：120字符。

```text
当前提醒音量是百分之{p0}。
```

字段来源：`p0 = chinese_volume(status['alarm_volume'])`

## reminder_tools.prompt.answer.020

类型：prompt；上限：4000字符。

```text
只提取提醒音量volume，整数0到100。静音为0，大一点加10，小一点减10。当前{p0}。
```

字段来源：`p0 = status['alarm_volume']`

## reminder_tools.reply.answer.021

类型：reply；上限：120字符。

```text
已将提醒音量调整到百分之{p0}。
```

字段来源：`p0 = chinese_volume(volume)`

## reminder_tools.reply.answer.022

类型：reply；上限：120字符。

```text
现在共有{p0}条提醒，其中{p1}个倒计时、{p2}个定时提醒。
```

字段来源：`p0 = chinese_volume(total)；p1 = chinese_volume(timers)；p2 = chinese_volume(total - timers)`

## reminder_tools.reply.answer.023

类型：reply；上限：120字符。

```text
目前一次只能取消一条，请说取消第几条提醒。
```

## reminder_tools.reply.answer.024

类型：reply；上限：120字符。

```text
现在没有定时提醒。
```

## reminder_tools.reply.answer.025

类型：reply；上限：120字符。

```text
请告诉我要操作哪一条。
```

## reminder_tools.reply.answer.026

类型：reply；上限：120字符。

```text
这条提醒已经变化，请重新确认。
```

## reminder_tools.reply.answer.027

类型：reply；上限：120字符。

```text
已取消{p0}{p1}提醒。
```

字段来源：`p0 = rule_words(chosen)；p1 = chosen['item']`

## reminder_tools.reply.answer.028

类型：reply；上限：120字符。

```text
{p0}提醒还剩{p1}。
```

字段来源：`p0 = chosen['item']；p1 = remaining_words(seconds)`

## reminder_tools.reply.answer.029

类型：reply；上限：120字符。

```text
提醒你{p0}。
```

字段来源：`p0 = chosen['item']`

## reminder_tools.reply.answer.030

类型：reply；上限：120字符。

```text
你是想查询已有提醒，还是设置新提醒？设置时请告诉我事项和时间。
```

## reminder_tools.reply.answer.031

类型：reply；上限：120字符。

```text
请明确告诉我要设置的提醒事项和时间。
```

## reminder_tools.reply.answer.032

类型：reply；上限：120字符。

```text
目前支持一次性和每周指定星期提醒，请明确说出星期几。
```

## reminder_tools.reply.answer.033

类型：reply；上限：120字符。

```text
请用一句话重新告诉我提醒事项和时间。
```

## reminder_tools.reply.answer.034

类型：reply；上限：120字符。

```text
请告诉我倒计时多久，时长为一分钟到二十四小时。
```

## reminder_tools.reply.answer.035

类型：reply；上限：120字符。

```text
现在已经有一个倒计时啦，请先取消它，再设置新的。
```

## reminder_tools.reply.answer.036

类型：reply；上限：120字符。

```text
最多只能设置三个定时提醒，请先取消一条。
```

## reminder_tools.reply.answer.037

类型：reply；上限：120字符。

```text
请告诉我有效的提醒日期。
```

## reminder_tools.reply.answer.038

类型：reply；上限：120字符。

```text
请告诉我每周哪几天提醒你。
```

## reminder_tools.reply.answer.039

类型：reply；上限：120字符。

```text
请明确说出提醒的日期，或者说每周哪几天。
```

## reminder_tools.reply.answer.040

类型：reply；上限：120字符。

```text
没有听清提醒事项，请说具体要做什么，例如喝水或吃饭。
```

## reminder_tools.reply.answer.041

类型：reply；上限：120字符。

```text
请补充要提醒你做什么，例如提醒我喝水。
```

## reminder_tools.prompt.answer.042

类型：prompt；上限：4000字符。

```text
现在北京时间{p0}。仅提取本轮要求创建的一条提醒。kind为countdown或scheduled。相对时长使用seconds；明确钟点使用time，格式HH:MM，24小时制。date为YYYY-MM-DD；今天明天按当前北京时间解析；未说日期留空。weekdays为ISO星期1到7数组：一次性为空，每周指定日填写对应数字，每天为1到7。只说每周但没有星期、七点等不明确早晚、多个不同时间、没有明确创建意图时，clarify给出追问，禁止猜测。item是用户原话中的连续短语，最多20字，例如喝水；没有事项必须clarify追问，禁止从旧对话猜测。text是到点直接朗读的温暖提醒，最多40字，不写括号舞台说明。seconds为相对时长总秒数，未指定填0；clarify无需追问时为空。
```

字段来源：`p0 = now.isoformat()`

## reminder_tools.prompt.answer.043

类型：prompt；上限：4000字符。

```text
程序已核验的倒计时为
```

## reminder_tools.prompt.answer.044

类型：prompt；上限：4000字符。

```text
秒。
```

## reminder_tools.prompt.answer.045

类型：prompt；上限：4000字符。

```text
程序已核验钟点{p0:02}:{p1:02}、星期位图{p2}、日期{p3}。
```

字段来源：`p0 = draft['minute'] // 60；p1 = draft['minute'] % 60；p2 = draft['weekdays'] or 0；p3 = draft['date'] or now.date().isoformat()`

## reminder_tools.prompt.answer.046

类型：prompt；上限：4000字符。

```text
已核验字段不需要再次追问；仅事项缺失时用clarify询问。只调整text到点播报文案：用思巴迪温暖、自然、略带俏皮的伙伴口吻，先说清提醒事项，再补一句与该事项相关的具体关怀或轻量建议。通常24到32个字符，含标点最多40个字符，1到2个短句；简单事项不强行凑字。适合小学生，避免空泛夸奖、重复称呼和固定套话；不编造观察到的行为或身体状态，不新增任务，不改时间和事项，不写括号动作、表情或Markdown。其他字段和澄清规则保持不变。示例：喝水时间到啦，先放下手里的事，喝几口水，慢慢来呀。
```

## reminder_tools.reply.answer.047

类型：reply；上限：40字符。

```text
时间到啦。
```

## reminder_tools.reply.answer.048

类型：reply；上限：120字符。

```text
没有确认清楚提醒事项，请再说一次具体要做什么。
```

## reminder_tools.reply.answer.049

类型：reply；上限：120字符。

```text
请再说清楚要提醒你做什么。
```

## reminder_tools.reply.answer.050

类型：reply；上限：120字符。

```text
请简短说出提醒事项，例如喝水或吃饭。
```

## reminder_tools.reply.answer.051

类型：reply；上限：120字符。

```text
这个时间已经过去或距离现在不足一分钟，请重新指定日期和时间。
```

## reminder_tools.reply.answer.052

类型：reply；上限：120字符。

```text
请设置二一〇〇年之前的提醒。
```

## reminder_tools.reply.answer.053

类型：reply；上限：120字符。

```text
定时提醒之间至少需要间隔五分钟，请换一个时间。
```

## reminder_tools.reply.answer.054

类型：reply；上限：120字符。

```text
好呀，已设置{p0}倒计时，到时提醒你。
```

字段来源：`p0 = old.spoken_duration(seconds)`

## reminder_tools.reply.answer.055

类型：reply；上限：120字符。

```text
好呀，{p0}{p1}提醒你{p2}。
```

字段来源：`p0 = rule_words(stored)；p1 = time_words(stored['deadline'], result['now'])；p2 = item`

## capabilities.reply.answer.001

类型：reply；上限：255字符。

```text
可以设置一个一分钟到二十四小时的倒计时，以及最多三个一次性或每周指定星期提醒。例如，每周一晚上八点提醒我看书。你也可以查询提醒，或说取消第一条提醒；提醒音量可以单独调节。
```

## capabilities.reply.answer.002

类型：reply；上限：147字符。

```text
可以设置一个一分钟到二十四小时的倒计时，例如提醒我十分钟后喝水，也能查询、取消和单独调节提醒音量。
```

## capabilities.reply.answer.003

类型：reply；上限：120字符。

```text
当前设备还没有开放提醒功能，可以先和我语音聊天或一起学习。
```

## capabilities.reply.answer.004

类型：reply；上限：120字符。

```text
当前设备还没有开放语音调节音量。
```

## capabilities.reply.answer.005

类型：reply；上限：120字符。

```text
可以问当前音量，或说音量调到五十、大一点、小一点、静音。
```

## capabilities.reply.answer.006

类型：reply；上限：120字符。

```text
说恢复音量，可以恢复静音前的音量。
```

## capabilities.reply.answer.007

类型：reply；上限：120字符。

```text
提醒音量可以单独设置。
```

## capabilities.reply.answer.008

类型：reply；上限：120字符。

```text
还能查询、调节或静音对话音量
```

## capabilities.reply.answer.009

类型：reply；上限：120字符。

```text
，并恢复静音前音量。
```

## capabilities.reply.answer.010

类型：reply；上限：123字符。

```text
可以设置一个倒计时和最多三个一次性或每周提醒，查询、逐条取消，并单独调节提醒音量。
```

## capabilities.reply.answer.011

类型：reply；上限：120字符。

```text
可以设置、查询、取消一个倒计时，并单独调节提醒音量。
```

## capabilities.reply.answer.012

类型：reply；上限：120字符。

```text
按住按键说话，松开发送；播报时重新按键可以打断。
```

## clock_answers.reply.intent.001

类型：reply；上限：120字符。

```text
现在
```

## clock_answers.reply.answer.002

类型：reply；上限：120字符。

```text
现在是北京时间{p0}点
```

字段来源：`p0 = chinese_volume(current.hour)`

## clock_answers.reply.answer.003

类型：reply；上限：120字符。

```text
{p0}分。
```

字段来源：`p0 = chinese_volume(current.minute)`

## clock_answers.reply.answer.004

类型：reply；上限：120字符。

```text
整。
```

## clock_answers.reply.answer.005

类型：reply；上限：120字符。

```text
{p0}是{p1}。
```

字段来源：`p0 = label；p1 = weekday`

## clock_answers.reply.answer.006

类型：reply；上限：120字符。

```text
{p0}是{p1}年{p2}月{p3}日，{p4}。
```

字段来源：`p0 = label；p1 = year；p2 = chinese_volume(current.month)；p3 = chinese_volume(current.day)；p4 = weekday`

## voice_server.prompt.respond.001

类型：prompt；上限：4000字符。

```text

本次连接实际开放的功能：
```

## voice_server.prompt.respond.002

类型：prompt；上限：4000字符。

```text

普通聊天不能声称已经创建、修改、取消或删除设备提醒，也不能声称已经改变设备音量。
```

## capabilities.reply.introduction

类型：reply；上限：180字符。

```text
我能陪你语音聊天、讲故事、解释知识，一起练习学习题目。
```

## reminders.reply.list_entry

类型：reply；上限：180字符。

```text
第{p0}条，{p1}，{p2}{p3}{p4}提醒。
```

字段来源：`p0 = chinese_volume(i)；p1 = row['item']；p2 = rule_words(row)；p3 = '下次' if row['weekdays'] else ''；p4 = time_words(row['deadline'], status['now'])`

## reminders.reply.quota

类型：reply；上限：180字符。

```text
提醒名额已满，请先取消旧提醒。
```

## reminders.reply.spacing

类型：reply；上限：180字符。

```text
定时提醒之间至少需要间隔五分钟。
```

## reminders.reply.not_ready

类型：reply；上限：180字符。

```text
提醒时间或音频尚未准备好，请重新设置。
```

## reminders.reply.save_failed

类型：reply；上限：180字符。

```text
提醒保存失败，请重新设置。
```
