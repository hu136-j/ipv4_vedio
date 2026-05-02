# 嵌入式Linux优化特性

本文档介绍为提升项目在嵌入式Linux环境中的竞争力而添加的特性。

## 目录

1. [资源监控和自适应机制](#资源监控和自适应机制)
2. [看门狗集成](#看门狗集成)
3. [配置说明](#配置说明)
4. [使用示例](#使用示例)

---

## 资源监控和自适应机制

### 功能概述

资源监控模块实时监控系统的CPU、内存和网络使用情况，并根据资源状态自动调整应用行为，确保在资源受限的嵌入式设备上稳定运行。

### 核心特性

#### 1. 实时资源监控

- **CPU使用率监控**：通过 `/proc/stat` 读取系统CPU使用率
- **内存使用监控**：通过 `/proc/meminfo` 读取内存使用情况
- **网络流量监控**：通过 `/proc/net/dev` 或 `/sys/class/net/` 读取网络流量统计

#### 2. 资源等级评估

系统根据CPU和内存使用率将资源状态分为四个等级：

| 等级 | 描述 | 触发条件 |
|------|------|----------|
| **ABUNDANT** | 资源充足 | CPU < 35%, 内存 < 37.5% |
| **NORMAL** | 正常 | 介于充足和紧张之间 |
| **LOW** | 资源紧张 | CPU ≥ 70% 或 内存 ≥ 75% |
| **CRITICAL** | 严重不足 | CPU ≥ 90% 或 内存 ≥ 90% |

#### 3. 自适应策略

根据资源等级自动调整应用参数：

| 资源等级 | Jitter Buffer | 最大比特率 | 丢包阈值 | 额外缓冲 |
|----------|---------------|------------|----------|----------|
| CRITICAL | 16 | 64 kbps | 5 | 禁用 |
| LOW | 32 | 128 kbps | 8 | 禁用 |
| NORMAL | 64 | 320 kbps | 10 | 启用 |
| ABUNDANT | 128 | 512 kbps | 15 | 启用 |

### API接口

```c
/* 创建资源监控器 */
resource_monitor_t* resource_monitor_create(const resource_monitor_config_t *config);

/* 更新资源统计 */
int resource_monitor_update(resource_monitor_t *monitor);

/* 获取资源统计信息 */
int resource_monitor_get_stats(resource_monitor_t *monitor, resource_stats_t *stats);

/* 获取当前资源等级 */
resource_level_t resource_monitor_get_level(resource_monitor_t *monitor);

/* 获取自适应策略建议 */
int resource_monitor_get_policy(resource_monitor_t *monitor, adaptive_policy_t *policy);

/* 销毁资源监控器 */
void resource_monitor_destroy(resource_monitor_t *monitor);
```

### 配置参数

在 `client.conf` 中配置：

```ini
# 启用资源监控（0=禁用，1=启用）
enable_resource_monitor=1

# 采样间隔（毫秒）
resource_sample_interval_ms=1000

# CPU严重阈值（百分比）
resource_cpu_critical=90.0

# CPU紧张阈值（百分比）
resource_cpu_low=70.0

# 内存严重阈值（百分比）
resource_mem_critical=90.0

# 内存紧张阈值（百分比）
resource_mem_low=70.0
```

### 工作原理

1. **定期采样**：每隔 `resource_sample_interval_ms` 毫秒采样一次系统资源
2. **等级评估**：根据CPU和内存使用率计算资源等级
3. **策略生成**：根据资源等级生成自适应策略
4. **日志记录**：当资源紧张或严重不足时记录警告日志

---

## 看门狗集成

### 功能概述

看门狗（Watchdog）是嵌入式系统中的重要可靠性保障机制。当应用程序死锁或挂起时，看门狗会自动重启系统，确保设备持续运行。

### 核心特性

#### 1. 硬件看门狗支持

- 支持标准Linux硬件看门狗设备（`/dev/watchdog`）
- 自动检测和配置看门狗超时时间
- 支持魔术关闭（Magic Close）机制

#### 2. 定期喂狗

- 应用程序定期向看门狗发送"喂狗"信号
- 可配置喂狗间隔
- 自动检测是否需要喂狗

#### 3. 安全关闭

- 支持魔术关闭字符 'V'
- 正常退出时安全关闭看门狗，避免系统重启

### API接口

```c
/* 创建看门狗实例 */
watchdog_t* watchdog_create(const watchdog_config_t *config);

/* 启动看门狗 */
int watchdog_start(watchdog_t *wdt);

/* 停止看门狗 */
int watchdog_stop(watchdog_t *wdt);

/* 喂狗 */
int watchdog_feed(watchdog_t *wdt);

/* 检查是否需要喂狗 */
int watchdog_should_feed(watchdog_t *wdt);

/* 获取/设置超时时间 */
int watchdog_get_timeout(watchdog_t *wdt);
int watchdog_set_timeout(watchdog_t *wdt, uint32_t timeout_sec);

/* 获取剩余时间 */
int watchdog_get_timeleft(watchdog_t *wdt);

/* 销毁看门狗 */
void watchdog_destroy(watchdog_t *wdt);
```

### 配置参数

在 `client.conf` 中配置：

```ini
# 启用看门狗（0=禁用，1=启用）
enable_watchdog=0

# 看门狗设备路径
watchdog_device=/dev/watchdog

# 看门狗超时时间（秒）
watchdog_timeout_sec=30

# 喂狗间隔（毫秒）
watchdog_feed_interval_ms=10000
```

### 工作原理

1. **初始化**：打开 `/dev/watchdog` 设备，设置超时时间
2. **定期喂狗**：主循环中每隔 `watchdog_feed_interval_ms` 毫秒喂一次狗
3. **超时保护**：如果超过 `watchdog_timeout_sec` 秒未喂狗，系统自动重启
4. **安全退出**：程序正常退出时写入魔术字符 'V' 并关闭设备

### 注意事项

⚠️ **重要提示**：

1. **硬件要求**：需要硬件支持看门狗功能，并且内核已加载看门狗驱动
2. **权限要求**：需要root权限或对 `/dev/watchdog` 有写权限
3. **测试建议**：在生产环境启用前，务必在测试环境充分测试
4. **默认禁用**：看门狗默认禁用（`enable_watchdog=0`），避免意外重启

### 启用看门狗的步骤

1. **检查硬件支持**：
   ```bash
   ls -l /dev/watchdog
   ```

2. **加载驱动**（如果未加载）：
   ```bash
   sudo modprobe softdog  # 软件看门狗（测试用）
   # 或
   sudo modprobe iTCO_wdt  # Intel硬件看门狗
   ```

3. **修改配置文件**：
   ```ini
   enable_watchdog=1
   ```

4. **以root权限运行**：
   ```bash
   sudo ./clinet -f client.conf
   ```

---

## 配置说明

### 完整配置示例

```ini
# 客户端配置文件

# 组播配置
multicast_group=224.1.1.1
multicast_port=8888

# TCP 控制端口
control_port=9000
control_listen_ip=127.0.0.1

# 播放器配置
player_path=mpg123

# 缓冲区配置
jitter_buffer_size=64
start_buffer_packets=6
skip_threshold=12
outbuf_capacity=1048576

# 心跳/重连配置
heartbeat_timeout=10
heartbeat_check_interval=500
max_reconnect_attempts=0
reconnect_delay_ms=2000

# 资源监控配置
enable_resource_monitor=1
resource_sample_interval_ms=1000
resource_cpu_critical=90.0
resource_cpu_low=70.0
resource_mem_critical=90.0
resource_mem_low=70.0

# 看门狗配置
enable_watchdog=0
watchdog_device=/dev/watchdog
watchdog_timeout_sec=30
watchdog_feed_interval_ms=10000

# 日志配置
log_level=INFO
log_file=
log_use_color=1
log_show_timestamp=1
log_show_thread_id=0

# 启动选项
auto_channel=0
interactive_mode=0
enable_control=1
```

### 针对不同场景的配置建议

#### 1. 低端嵌入式设备（如树莓派Zero）

```ini
enable_resource_monitor=1
resource_sample_interval_ms=2000
resource_cpu_critical=85.0
resource_cpu_low=60.0
resource_mem_critical=85.0
resource_mem_low=65.0

enable_watchdog=1
watchdog_timeout_sec=60
watchdog_feed_interval_ms=20000
```

#### 2. 中端嵌入式设备（如树莓派4）

```ini
enable_resource_monitor=1
resource_sample_interval_ms=1000
resource_cpu_critical=90.0
resource_cpu_low=70.0
resource_mem_critical=90.0
resource_mem_low=70.0

enable_watchdog=1
watchdog_timeout_sec=30
watchdog_feed_interval_ms=10000
```

#### 3. 高性能设备或开发环境

```ini
enable_resource_monitor=0
enable_watchdog=0
```

---

## 使用示例

### 示例1：启用资源监控

```bash
# 编辑配置文件
vim client.conf

# 启用资源监控
enable_resource_monitor=1

# 运行客户端
./clinet -f client.conf

# 观察日志输出
# [INFO] [resource_monitor] resource monitor created (cpu_crit=90.0%, mem_crit=90.0%)
# [WARN] [resource_monitor] resource level: LOW (cpu=75.2% mem=68.3%)
# [INFO] [resource_monitor] adaptive policy: buffer=32 bitrate=128kbps
```

### 示例2：启用看门狗（需要root权限）

```bash
# 检查看门狗设备
ls -l /dev/watchdog

# 加载软件看门狗驱动（测试用）
sudo modprobe softdog

# 编辑配置文件
vim client.conf

# 启用看门狗
enable_watchdog=1

# 以root权限运行
sudo ./clinet -f client.conf

# 观察日志输出
# [INFO] [watchdog] watchdog created (type=hardware, device=/dev/watchdog, feed_interval=10000ms)
# [INFO] [watchdog] watchdog started
# [INFO] [watchdog] watchdog actual timeout: 30 seconds
# [DEBUG] [watchdog] watchdog fed
```

### 示例3：模拟资源紧张场景

```bash
# 使用stress工具模拟高CPU负载
stress --cpu 4 --timeout 60s &

# 运行客户端（资源监控已启用）
./clinet -f client.conf

# 观察自适应策略的触发
# [WARN] [resource_monitor] resource level: CRITICAL (cpu=92.5% mem=45.2%)
# [INFO] [resource_monitor] adaptive policy: buffer=16 bitrate=64kbps
```

---

## 技术细节

### 资源监控实现

- **CPU使用率计算**：通过两次采样 `/proc/stat` 计算CPU空闲时间差值
- **内存使用率计算**：优先使用 `MemAvailable`，不存在时使用 `MemFree + Buffers + Cached`
- **网络流量统计**：支持单网卡和全网卡统计，自动排除 `lo` 接口

### 看门狗实现

- **ioctl命令**：
  - `WDIOC_GETSUPPORT`：获取设备信息
  - `WDIOC_SETTIMEOUT`：设置超时时间
  - `WDIOC_GETTIMEOUT`：获取超时时间
  - `WDIOC_KEEPALIVE`：喂狗
  - `WDIOC_GETTIMELEFT`：获取剩余时间

- **魔术关闭**：写入字符 'V' 后关闭设备，避免系统重启

---

## 性能影响

### 资源监控

- **CPU开销**：< 0.5%（采样间隔1秒）
- **内存开销**：< 1KB
- **I/O开销**：每次采样读取3个 `/proc` 文件

### 看门狗

- **CPU开销**：< 0.1%
- **内存开销**：< 512B
- **I/O开销**：每次喂狗一次 ioctl 调用

---

## 故障排查

### 资源监控无法启动

**问题**：日志显示 "resource_monitor_create failed"

**解决方案**：
1. 检查 `/proc/stat` 和 `/proc/meminfo` 是否可读
2. 确认配置参数合法（阈值在0-100之间）

### 看门狗无法启动

**问题**：日志显示 "watchdog_create failed"

**解决方案**：
1. 检查 `/dev/watchdog` 是否存在：`ls -l /dev/watchdog`
2. 检查权限：`sudo chmod 666 /dev/watchdog`（临时）
3. 加载驱动：`sudo modprobe softdog`
4. 以root权限运行：`sudo ./clinet`

### 系统意外重启

**问题**：启用看门狗后系统频繁重启

**解决方案**：
1. 增加超时时间：`watchdog_timeout_sec=60`
2. 减少喂狗间隔：`watchdog_feed_interval_ms=5000`
3. 检查应用是否有长时间阻塞操作
4. 临时禁用看门狗：`enable_watchdog=0`

---

## 未来改进

1. **资源监控**：
   - 支持GPU使用率监控
   - 支持磁盘I/O监控
   - 支持温度监控
   - 更精细的自适应策略

2. **看门狗**：
   - 支持软件看门狗模式
   - 支持多级看门狗
   - 支持看门狗事件通知

3. **集成优化**：
   - 资源监控触发看门狗喂狗频率调整
   - 基于资源状态的动态超时调整

---

## 参考资料

- [Linux Watchdog API](https://www.kernel.org/doc/html/latest/watchdog/watchdog-api.html)
- [/proc filesystem documentation](https://www.kernel.org/doc/html/latest/filesystems/proc.html)
- [Embedded Linux Best Practices](https://elinux.org/Main_Page)
