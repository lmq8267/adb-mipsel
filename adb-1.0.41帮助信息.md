# Android Debug Bridge（ADB）版本 1.0.41（Android Tools 35.0.2-rc.1）完整帮助文档中文翻译

## 基本信息

- Android Debug Bridge 版本：1.0.41  
- Android Tools 版本：35.0.2-rc.1-android-tools-static  
- 安装形式：静态编译版本  

---

# 全局选项（global options）

- `-a`  
  监听所有网络接口，而不是仅监听 localhost

- `-d`  
  使用 USB 设备  
  如果连接多个设备则报错

- `-e`  
  使用 TCP/IP 设备（模拟器）  
  如果存在多个 TCP/IP 设备则报错

- `-s SERIAL`  
  指定使用某个设备序列号  
  会覆盖环境变量 `$ANDROID_SERIAL`

- `-t ID`  
  使用指定 transport ID 的设备

- `-H`  
  指定 adb server 主机名（默认 localhost）

- `-P`  
  指定 adb server 端口（默认 5037）

- `-L SOCKET`  
  指定 adb server 监听的 socket  
  默认：`tcp:localhost:5037`

- `--one-device SERIAL|USB`  
  仅允许 adb server 连接一个设备（仅用于 start-server 或 server nodaemon）  
  可通过 serial 或 USB 地址指定设备

- `--exit-on-write-error`  
  当 stdout 关闭时立即退出

---

# 通用命令（general commands）

- `devices [-l]`  
  列出已连接设备  
  `-l`：显示详细信息

- `help`  
  显示帮助信息

- `version`  
  显示版本号

---

# 网络相关（networking）

## 连接/断开设备

- `connect HOST[:PORT]`  
  通过 TCP/IP 连接设备  
  默认端口：5555

- `disconnect [HOST[:PORT]]`  
  断开指定 TCP/IP 设备连接  
  如果不指定参数，则断开全部连接

---

## 配对连接

- `pair HOST[:PORT] [PAIRING CODE]`  
  与设备进行安全 TCP/IP 配对连接

---

## 端口转发（forward）

- `forward --list`  
  列出所有 forward 连接

- `forward [--no-rebind] LOCAL REMOTE`  
  创建 socket 转发

支持类型：

- `tcp:<port>`（LOCAL 可用 tcp:0 自动分配端口）
- `localabstract:<unix socket 名称>`
- `localreserved:<unix socket 名称>`
- `localfilesystem:<unix socket 名称>`
- `dev:<字符设备>`
- `dev-raw:<字符设备（原始模式）>`
- `jdwp:<PID>`（远程）
- `vsock:<CID>:<port>`（远程）
- `acceptfd:<fd>`（仅监听）

### 删除 forward

- `forward --remove LOCAL` 删除指定转发  
- `forward --remove-all` 删除全部转发

---

## 反向端口转发（reverse）

- `reverse --list`  
  列出设备 → 主机的反向转发

- `reverse [--no-rebind] REMOTE LOCAL`  
  创建反向 socket 连接

支持类型：

- `tcp:<port>`（REMOTE 可用 tcp:0 自动分配）
- `localabstract:<unix socket>`
- `localreserved:<unix socket>`
- `localfilesystem:<unix socket>`

### 删除 reverse

- `reverse --remove REMOTE`
- `reverse --remove-all`

---

## mDNS 服务发现

- `mdns check`  
  检查 mDNS 是否可用

- `mdns services`  
  列出所有发现的服务

---

# 文件传输（file transfer）

## push（推送到设备）

```

push [--sync] [-z ALGORITHM] [-Z] LOCAL... REMOTE

```

功能：将本地文件/目录复制到设备

选项：

- `-n`：仅模拟执行（不写入文件系统）
- `-q`：不显示进度
- `-Z`：禁用压缩
- `-z`：启用指定压缩算法
  - any / none / brotli / lz4 / zstd
- `--sync`：仅同步时间戳不同的文件

---

## pull（从设备拉取）

```

pull [-a] [-z ALGORITHM] [-Z] REMOTE... LOCAL

```

选项：

- `-a`：保留时间戳与权限
- `-q`：不显示进度
- `-Z`：禁用压缩
- `-z`：启用压缩算法

---

## sync（系统同步）

```

sync [-l] [-z ALGORITHM] [-Z] [all|data|odm|oem|product|system|system_ext|vendor]

```

功能：从 `$ANDROID_PRODUCT_OUT` 同步构建系统到设备

选项：

- `-l`：仅列出将要复制的文件
- `-n`：模拟执行
- `-q`：不显示进度
- `-Z`：禁用压缩
- `-z`：指定压缩算法

---

# shell 与模拟器

## shell

```

shell [-e ESCAPE] [-n] [-Tt] [-x] [COMMAND...]

```

功能：运行远程 shell

选项：

- `-e`：设置转义字符（默认 `~`）
- `-n`：不读取 stdin
- `-T`：禁用伪终端
- `-t`：强制分配伪终端
- `-tt`：强制双重伪终端
- `-x`：关闭 stdout/stderr 分离

---

## emulator

- `emu COMMAND`  
  执行模拟器控制台命令

---

# 应用安装（app installation）

## install

```

install [-lrtsdg] [--instant] PACKAGE

```

功能：安装 APK

选项：

- `-r`：替换已有应用
- `-t`：允许测试应用
- `-d`：允许降级安装（仅 debug）
- `-g`：授予所有运行时权限
- `--abi ABI`：指定 ABI
- `--instant`：即时应用
- `--no-streaming`：关闭流式安装
- `--streaming`：强制流式安装
- `--fastdeploy`：快速部署
- `--no-fastdeploy`：禁用快速部署
- `--force-agent`：强制更新部署代理
- `--date-check-agent`：检查日期更新代理
- `--version-check-agent`：检查版本更新代理
- `--local-agent`：使用本地 agent

---

## 多 APK 安装

- `install-multiple`
- `install-multi-package`

支持参数：

- `-r` 替换应用
- `-t` 允许测试包
- `-d` 允许降级
- `-p` 部分安装
- `-g` 授权权限
- `--abi ABI`
- `--instant`

---

## uninstall

```

uninstall [-k] PACKAGE

```

- `-k` 保留数据和缓存

---

# 调试（debugging）

- `bugreport [PATH]`  
  导出完整 bugreport（默认 bugreport.zip）

- `jdwp`  
  列出支持 JDWP 的进程 PID

- `logcat`  
  查看设备日志（详细参数见 logcat）

---

# 安全（security）

- `disable-verity`  
  禁用 dm-verity（userdebug）

- `enable-verity`  
  启用 dm-verity

- `keygen FILE`  
  生成 adb 公私钥

---

# 脚本命令（scripting）

- `wait-for[-TRANSPORT]-STATE...`  
  等待设备进入指定状态  
  状态包括：
  - device
  - recovery
  - rescue
  - sideload
  - bootloader
  - disconnect

- `get-state`  
  输出状态：offline / bootloader / device

- `get-serialno`  
  输出序列号

- `get-devpath`  
  输出设备路径

- `remount [-R]`  
  重新挂载分区为可写  
  `-R`：需要重启则自动重启

- `reboot [mode]`  
  重启设备  
  支持：
  - bootloader
  - recovery
  - sideload
  - sideload-auto-reboot

- `root`  
  重启 adbd 为 root 模式

- `unroot`  
  关闭 root 模式

- `usb`  
  以 USB 模式重启 adbd

- `tcpip PORT`  
  以 TCP 模式启动 adbd

---

# 内部调试（internal debugging）

- `start-server`  
  启动 adb server

- `kill-server`  
  关闭 adb server

- `reconnect`  
  强制重连主机端

- `reconnect device`  
  强制设备端重连

- `reconnect offline`  
  重置离线/未授权设备

---

# USB 控制

- `attach`  
  连接 USB 设备

- `detach`  
  断开 USB 设备（允许其他程序使用）

---

# 环境变量（environment variables）

- `$ADB_TRACE`  
  调试日志输出类别：
  - all
  - adb
  - sockets
  - packets
  - rwx
  - usb
  - sync
  - sysdeps
  - transport
  - jdwp
  - services
  - auth
  - fdevent
  - shell
  - incremental

- `$ADB_VENDOR_KEYS`  
  adb key 文件或目录（以冒号分隔）

- `$ANDROID_SERIAL`  
  指定默认设备序列号

- `$ANDROID_LOG_TAGS`  
  logcat 标签过滤

- `$ADB_LOCAL_TRANSPORT_MAX_PORT`  
  模拟器扫描端口上限（默认 5585）

- `$ADB_MDNS_AUTO_CONNECT`  
  mDNS 自动连接服务列表（默认 adb-tls-connect）

---

# 官方文档

https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/user/adb.1.md

