# Kernel / Forge 本地启动脚本

这些脚本只解决“从仓库根目录启动”，不绑定某个人的 WSL 发行版、Python 路径或 QEMU 安装路径。默认从 `PATH` 查找工具；找不到时用环境变量覆盖。

## Windows（PowerShell）

```powershell
# 1) 在默认 WSL 发行版里 clean build；可选 -Distro Ubuntu
.\scripts\kernel\build-kernel.ps1

# 2) 启动 QEMU；脚本固定使用 TCP 串口 5555
.\scripts\kernel\run-qemu.ps1

# 3) 启动 WebSocket 桥接；默认 5555 -> 8765
.\scripts\kernel\run-bridge.ps1

# 4) 另开终端启动 Forge 静态页面，工作目录必须是 interface\web
cd interface\web
python -m http.server 8080
```

打开 `http://127.0.0.1:8080/forge/?model=toy_offroad&v=52`，在 Inspector 的 `KERNEL BRIDGE WEBSOCKET` 面板点 `Connect`，再在 `KERNEL ARC 0x05` 面板点 `Full Circle`。整圆沿用 ARC 的 `sweep=-2π`，半径 `0.5 m`、速度 `0.30 m/s` 时，期望 `s` 从 `0` 推进到 `3.1416 m`、`t=1.0`、`DONE`，前轮约 `24°`，终点回到起点附近。

三个脚本都支持 `-Check`，只解析路径、检查依赖并打印将要执行的命令，不启动进程：

```powershell
.\scripts\kernel\build-kernel.ps1 -Check
.\scripts\kernel\run-qemu.ps1 -Check
.\scripts\kernel\run-bridge.ps1 -Check
```

## Linux / WSL

```bash
./scripts/kernel/build-kernel.sh
./scripts/kernel/run-qemu.sh
./scripts/kernel/run-bridge.sh
cd interface/web && python3 -m http.server 8080
```

## 可选覆盖项

| 变量 / 参数 | 作用 | 默认 |
| --- | --- | --- |
| `AEVUM_WSL_DISTRO` / `-Distro` | Windows 构建使用的 WSL 发行版 | WSL 默认发行版 |
| `AEVUM_QEMU` / `-Qemu` | `qemu-system-i386` 可执行文件 | `PATH`，再查常见安装目录 |
| `AEVUM_PYTHON` / `-Python` | Python 解释器 | `PATH` 的 `python` / `py`，再查仓库 `.venv` |
| `AEVUM_SERIAL_PORT` / `-SerialPort` | QEMU TCP 串口 | `5555` |
| `AEVUM_WS_PORT` / `-WsPort` | 桥接 WebSocket 端口 | `8765` |

## 依赖

- 构建：`gcc`（32 位）、`nasm`、`ld`、`make`、`objcopy`。WSL/Ubuntu 下需要能编译 `-m32` 的 multilib 工具链。
- 运行：`qemu-system-i386`，QEMU 需要支持 `-serial tcp:...` 和 GTK 显示。
- 桥接：Python 3 + `websockets`（`python -m pip install websockets`）。

脚本不读取 `build/` 下的任何本地文件；`build/` 是排除目录。构建产物仍在 `src/kernel/host/console/robot/` 下，`disk.img` 是 QEMU 的启动镜像。
