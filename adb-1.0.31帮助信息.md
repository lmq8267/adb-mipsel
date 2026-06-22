# Android Debug Bridge（ADB）版本 1.0.31 帮助文档（中文翻译）

---

## 全局选项

- `-a`  
  让 adb 监听所有网络接口以接受连接

- `-d`  
  指定命令发送到唯一连接的 USB 设备  
  如果连接了多个 USB 设备，则返回错误

- `-e`  
  指定命令发送到唯一正在运行的模拟器  
  如果运行了多个模拟器，则返回错误

- `-s <指定设备>`  
  指定命令发送到具有给定序列号或标识的设备/模拟器  
  会覆盖环境变量 `ANDROID_SERIAL`

- `-p <产品名称或路径>`  
  简单产品名，例如 `sooner`，或  
  指向产品输出目录的相对/绝对路径，例如  
  `out/target/product/sooner`  
  如果未指定 `-p`，则使用环境变量 `ANDROID_PRODUCT_OUT`  
  该变量必须是绝对路径

- `-H`  
  指定 adb server 主机名（默认：localhost）

- `-P`  
  指定 adb server 端口（默认：5037）

---

## 设备管理命令

- `devices [-l]`  
  列出所有已连接设备  
  `-l` 会同时显示设备的详细标识信息

- `connect <主机>[:<端口>]`  
  通过 TCP/IP 连接设备  
  如果未指定端口，默认使用 5555

- `disconnect [<主机>[:<端口>]]`  
  断开 TCP/IP 设备连接  
  如果未指定参数，则断开所有 TCP/IP 连接

---

## 设备操作命令

### 文件传输与同步

- `adb push <本地路径> <远程路径>`  
  将文件/目录复制到设备

- `adb pull <远程路径> [<本地路径>]`  
  从设备复制文件/目录到本地

- `adb sync [<目录>]`  
  仅在发生变化时同步 host → device  
  `-l` 表示仅列出但不复制  
  （详见 `adb help all`）

---

### 远程 shell 与调试

- `adb shell`  
  进入设备远程交互 shell

- `adb shell <命令>`  
  在设备上执行单条 shell 命令

- `adb emu <命令>`  
  执行模拟器控制台命令

- `adb logcat [<过滤规则>]`  
  查看设备日志输出

---

### 端口转发

- `adb forward --list`  
  列出所有端口转发连接  
  格式如下：  
  `<序列号> <本地端口> <远程端口>`

- `adb forward <本地> <远程>`  
  建立 socket 端口转发  
  支持的转发格式：

  - `tcp:<端口>`
  - `localabstract:<Unix域socket名>`
  - `localreserved:<Unix域socket名>`
  - `localfilesystem:<Unix域socket名>`
  - `dev:<字符设备名>`
  - `jdwp:<进程PID>`（仅远程）

- `adb forward --no-rebind <本地> <远程>`  
  与 forward 相同，但如果本地端口已存在则失败

- `adb forward --remove <本地>`  
  删除指定端口转发

- `adb forward --remove-all`  
  删除所有端口转发

---

### JDWP 调试

- `adb jdwp`  
  列出所有提供 JDWP 调试通道的进程 PID

---

### 应用安装与卸载

- `adb install [-l] [-r] [-s] [--algo <算法> --key <十六进制密钥> --iv <十六进制IV>] <文件>`  
  推送并安装 APK

  选项说明：
  - `-l`：应用 forward-lock
  - `-r`：重新安装并保留数据
  - `-s`：安装到 SD 卡
  - `--algo/--key/--iv`：表示 APK 已加密

- `adb uninstall [-k] <包名>`  
  卸载应用

  - `-k`：保留数据和缓存

---

### 系统与备份

- `adb bugreport`  
  导出设备完整调试信息（用于 bug 报告）

- `adb backup [-f <文件>] ...`  
  备份设备数据到文件  
  默认输出 `backup.ab`

  参数说明：

  - `-apk / -noapk`：是否备份 APK（默认 noapk）
  - `-shared / -noshared`：是否备份共享存储（默认 noshared）
  - `-all`：备份所有应用
  - `-system / -nosystem`：是否包含系统应用
  - `<包列表>`：指定要备份的应用

- `adb restore <文件>`  
  从备份文件恢复数据

---

### 帮助与版本

- `adb help`  
  显示帮助信息

- `adb version`  
  显示版本号

---

## 脚本控制命令

- `adb wait-for-device`  
  阻塞直到设备上线

- `adb start-server`  
  启动 adb server

- `adb kill-server`  
  关闭 adb server

- `adb get-state`  
  输出设备状态：
  - offline（离线）
  - bootloader（引导模式）
  - device（正常设备）

- `adb get-serialno`  
  输出设备序列号

- `adb get-devpath`  
  输出设备路径

- `adb status-window`  
  持续输出设备状态

- `adb remount`  
  将 `/system` 重新挂载为可读写

- `adb reboot [bootloader|recovery]`  
  重启设备  
  可选择进入 bootloader 或 recovery

- `adb reboot-bootloader`  
  重启进入 bootloader

- `adb root`  
  以 root 权限重启 adbd 守护进程

- `adb usb`  
  重启 adbd 并监听 USB

- `adb tcpip <端口>`  
  以 TCP 模式启动 adbd

---

## 网络功能

- `adb ppp <tty> [参数]`  
  通过 USB 运行 PPP

说明：  
不建议自动启动 PPP 连接

`<tty>` 示例：  
- `dev:/dev/omap_csmi_tty1`

参数示例：  
- `defaultroute`
- `debug`
- `dump`
- `local`
- `notty`
- `usepeerdns`

---

## adb sync 说明

`adb sync [<目录>]`

`<本地目录>` 可解释为：

- 如果未指定 `<目录>`：更新 `/system` 和 `/data`
- 如果指定为 `system` 或 `data`：仅更新对应分区

---

## 环境变量

- `ADB_TRACE`  
  输出调试信息（逗号分隔）：

  可选值：
  - `1` 或 `all`
  - `adb`
  - `sockets`
  - `packets`
  - `rwx`
  - `usb`
  - `sync`
  - `sysdeps`
  - `transport`
  - `jdwp`

- `ANDROID_SERIAL`  
  指定默认连接设备序列号（`-s` 优先）

- `ANDROID_LOG_TAGS`  
  logcat 过滤标签

---