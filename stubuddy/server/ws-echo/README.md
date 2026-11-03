# stubuddy局域网回声服务（3.6.1）

配套固件变体：stubuddy-s3-n16r8-ws-echo。设备使用16kHz/单声道/60ms Opus。
本服务只在RAM缓存并原样返回一轮音频，不启动模型或保存录音文件。

从工作区根目录D:\Desktop\stubuddy执行：

```powershell
python -m pip install -r stubuddy/server/ws-echo/requirements.txt
python stubuddy/server/ws-echo/configure.py
python stubuddy/server/ws-echo/echo_server.py
```

另一个已加载相同Python环境的PowerShell窗口执行：

```powershell
python stubuddy/server/ws-echo/smoke_test.py
```

完整环境准备、编译、烧录、防火墙和真机验收见
[WS-ECHO-VALIDATION.md](../../../xiaozhi-esp32-main/xiaozhi-esp32-main/main/boards/stubuddy-s3-n16r8/WS-ECHO-VALIDATION.md)。

configure.py默认选择有网关、已连接的物理网卡IPv4，避开VPN/虚拟网卡，也可用--server-ip指定；两端共享的随机测试Token存于被Git忽略的.local.json，固件配置头写入小智工程tmp目录。
echo_server.py默认监听0.0.0.0:8765，Ctrl+C退出；--echo-delay 3可模拟等待回声以验收取消。

echo/1是明确协商的stubuddy测试扩展：16字节二进制头加Opus包载荷，附带回合编号、帧序、真实采样数和CRC校验。
后续WSS、正式AI服务适配与商用鉴权在后续检查点实施。
