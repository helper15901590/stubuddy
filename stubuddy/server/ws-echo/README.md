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
3.6.1已真机验收并提交5df51a4。

3.6.2新增独立云端配置 `configure_cloud.py` 和PowerShell部署入口 `Deploy-CloudEcho.ps1`。
完整部署顺序、编译烧录和验收见
[WSS-ECHO-VALIDATION.md](../../../xiaozhi-esp32-main/xiaozhi-esp32-main/main/boards/stubuddy-s3-n16r8/WSS-ECHO-VALIDATION.md)。

公网入口使用Nginx/TLS443，回声程序由systemd以专用用户运行、只监听127.0.0.1:8765。
安装工具先检查Ubuntu24.04、目录归属和端口；已占用时停止，不调整SSH、安全组或UFW。
Certbot固定5.9.0，ACME测试与生产配置分开；每小时检查短期IP证书并在续期后重载Nginx。
云端Token和部署包分别为被忽略的 `.cloud.json`、`.cloud-deploy.tar.gz`，固件头为tmp/stubuddy_echo_cloud.h。
重复配置保留云端Token，局域网Token与头文件继续独立保留；打包只包含明确列出的源码及云端配置。
2026-11-03用户反馈ECS部署、证书续期演练和电脑公网WSS自测PASS，成功烧录后确认测试成功；当前待建立3.6.2 Git检查点。
正式AI服务适配与商用设备激活/鉴权仍在后续检查点实施。
