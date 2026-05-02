# 心跳/重连机制说明

## 概述

为 UDP 组播音频流媒体系统添加了心跳和自动重连机制，提高系统可靠性。

## 功能特性

### 1. 数据包超时检测
- 监控最后收到有效音频数据包的时间
- 当超过配置的超时时间未收到数据时，触发重连机制
- 默认超时时间：10 秒

### 2. 自动重连机制
- 检测到超时后，自动尝试重新加入组播组
- 支持配置最大重连次数（0 表示无限重试）
- 支持配置重连延迟时间
- 重连成功后自动恢复正常工作

### 3. 播放器健康检查
- 定期检查播放器进程是否存活
- 检测到播放器崩溃时自动重启
- 重启后清空缓冲区，重新开始播放

## 配置参数

在 `client.conf` 中添加以下配置：

```
# 心跳/重连配置
heartbeat_timeout=10              # 超时时间（秒），默认 10
heartbeat_check_interval=500      # 检查间隔（毫秒），默认 500
max_reconnect_attempts=0          # 最大重连次数，0 表示无限重试
reconnect_delay_ms=2000           # 重连延迟（毫秒），默认 2000
```

## 工作原理

### 数据包超时检测流程

1. 客户端启动时初始化心跳状态
2. 每次收到有效音频数据包时更新心跳时间戳
3. 主事件循环定期检查心跳超时（根据 `heartbeat_check_interval` 配置）
4. 如果超时，记录日志并触发重连

### 重连流程

1. 检测到超时后，先离开当前组播组
2. 立即重新加入组播组
3. 等待 `reconnect_delay_ms` 毫秒
4. 如果收到数据，重连成功，重置重连计数器
5. 如果仍然超时，继续重连，直到达到最大重连次数

### 播放器健康检查流程

1. 主事件循环定期检查播放器进程状态
2. 使用 `waitpid(WNOHANG)` 非阻塞检查进程是否退出
3. 如果播放器退出，记录日志并尝试重启
4. 重启成功后清空缓冲区，继续播放当前频道

## 日志输出示例

正常运行：
```
[2026-05-02 10:00:00] [INFO][client] heartbeat enabled: timeout=10s check_interval=500ms max_reconnect=0
[2026-05-02 10:00:00] [INFO][client] entering main event loop
```

检测到超时并重连：
```
[2026-05-02 10:05:15] [WARN][client] no data received for 10 seconds, attempting reconnect
[2026-05-02 10:05:15] [INFO][client] attempting to rejoin multicast group 224.1.1.1
[2026-05-02 10:05:15] [INFO][client] successfully rejoined multicast group
```

播放器崩溃并重启：
```
[2026-05-02 10:10:30] [WARN][client] player process exited unexpectedly, pid=12345
[2026-05-02 10:10:30] [WARN][client] player died, attempting restart for channel 3
[2026-05-02 10:10:30] [INFO][client] player restarted successfully
```

达到最大重连次数：
```
[2026-05-02 10:15:45] [WARN][client] no data received for 10 seconds, attempting reconnect
[2026-05-02 10:15:50] [WARN][client] no data received for 10 seconds, attempting reconnect
[2026-05-02 10:15:55] [WARN][client] no data received for 10 seconds, attempting reconnect
[2026-05-02 10:16:00] [ERROR][client] max reconnect attempts reached, giving up
```

## 测试建议

### 测试超时重连

1. 启动客户端并开始播放
2. 停止服务端
3. 观察客户端日志，应该看到超时和重连尝试
4. 重新启动服务端
5. 客户端应该自动恢复播放

### 测试播放器重启

1. 启动客户端并开始播放
2. 手动 kill 播放器进程：`kill -9 <player_pid>`
3. 观察客户端日志，应该看到播放器重启
4. 播放应该自动恢复

### 测试配置

使用 `client_test_heartbeat.conf` 进行快速测试（5秒超时，最多3次重连）：

```bash
./clinet -f client_test_heartbeat.conf -c 1
```

## 注意事项

1. **超时时间设置**：不要设置过短，避免网络抖动导致频繁重连
2. **重连次数**：设置为 0 表示无限重试，适合长期运行的场景
3. **检查间隔**：影响 epoll_wait 超时时间，不要设置过短以避免 CPU 占用过高
4. **播放器重启**：播放器重启会清空缓冲区，可能导致短暂的音频中断

## 实现文件

- `common/heartbeat.h` - 心跳机制头文件
- `common/heartbeat.c` - 心跳机制实现
- `client/client.c` - 客户端集成心跳和重连逻辑
