# 阿里云ECS实例信息
## 实例基础信息
| 项目 | 内容 |
| ---- | ---- |
| 实例规格 | ecs.e-c1m1.large |
| CPU & 内存 | 2核(vCPU)，2 GiB，支持更改CPU选项、调整配置 |
| 公网IP | 121.41.86.40，可转换为弹性公网IP |
| 主私网IP | 172.31.191.238 |
| 操作系统 | Ubuntu 24.04 64位，支持更换系统 |
| 公网带宽 | 3 Mbps，带宽计费方式：按固定带宽，支持更改带宽、查看带宽变更历史 |
| 系统盘 | d-bp1h3dzwkmn100vvc4aw，ESSD Entry 云盘，40 GiB |

## 防火墙（安全组）信息
安全组ID：`sg-bp15jk0ig08stpl0rrto`
安全组类型：普通安全组
查看标签：入方向

| 授权策略 | 协议类型 | 访问来源 | 访问目的(本实例)端口 |
| ---- | ---- | ---- | ---- |
| 允许 | TCP | 115.195.68.123/32 | 8000/8000 |
| 允许 | TCP | 115.195.68.123/32 | 8003/8003 |

# 本地开发电脑信息
Windows11专业版本 64位
IDF 6.1 + VS Code 
Python 3.13.7：
java version "1.8.0_511"
Java(TM) SE Runtime Environment (build 1.8.0_511-b33)
Java HotSpot(TM) 64-Bit Server VM (build 25.511-b33, mixed mode)
DashScope：Key 放云端私有配置文件即可。