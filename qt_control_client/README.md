# Qt5 上位机客户端

这是一个基于 Qt5 Widgets 的 Linux 上位机程序，用于通过 TCP 控制流媒体广播客户端核心程序。

## 支持的命令

- `LIST`
- `PLAY <id>`
- `STOP`
- `QUIT`

## 构建方式

### qmake

```bash
cd qt_control_client
qmake qt_control_client.pro
make
./qt_control_client
```

### Qt Creator

1. 打开 `qt_control_client.pro`
2. 选择 Qt 5.15 或兼容 Kit
3. 点击构建并运行

## 默认配置

- IP: `127.0.0.1`
- Port: `9000`

如果你的客户端核心程序监听的 TCP 端口不同，启动后直接在界面里修改即可。
