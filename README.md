# BTUSB — 创维 Q6011（Allwinner H616）USB 蓝牙驱动实验 + Linux 结论

给创维 Q6011 机顶盒（Android 10，内核 4.9.170 aarch64）补编 `btusb.ko`，让 CSR8510 USB 蓝牙适配器
能被内核识别成 `/dev/hci0`。

**结论先说**：

- ✅ 驱动能编出来、能加载、适配器能被内核识别（`hci0` 出现，BD 地址/Features 都能读）。
- ❌ 厂商内核拒绝把 HCI 设备打开（`HCIDEVUP` 返回 `EBADRQC(56)`），Android 的蓝牙 HAL 也不会接管 `hci0`。
- ✅ **换到正常 Linux 系统就是开箱即用**——`btusb` 是主线默认模块，BlueZ + PipeWire/PulseAudio 直接跑 A2DP。

---

## 1. 设备信息

```text
机型       创维 Q6011（Skyworth），产品名 eros-p1
SoC        Allwinner H616（sun50iw9p1），4×Cortex-A53 @1.34GHz，1GB RAM
系统       Android 10，内核 4.9.170（Skyworth 2025-06-25 编译，Linaro GCC 5.3.1）
用户态     armeabi-v7a（32 位），内核是 aarch64（64 位）
vermagic   4.9.170 SMP preempt mod_unload modversions aarch64
分区       bootloader / env / boot / super / recovery / vbmeta / vbmeta_system / vbmeta_vendor ...
WiFi       XR819（SDIO，mmc2，xradio 驱动）
蓝牙       无板载硬件（设备树无 bt 节点，persist.vendor.overlay.bluetooth_support=0）
```

内核关键配置：

```text
CONFIG_MODULES=y
CONFIG_MODVERSIONS=y            # 模块符号 CRC 必须和内核完全一致
CONFIG_MODULE_SIG 未开           # 不需要签名
CONFIG_BT=y
CONFIG_BT_HCIUART=y             # 只有串口蓝牙
# CONFIG_BT_HCIBTUSB is not set  # USB 蓝牙驱动没编 ← 本仓库要补的就是这个
```

---

## 2. 为什么要自己造 `Module.symvers`

`CONFIG_MODVERSIONS=y` 时，模块的 `__versions` 段里每个符号都带一个 CRC，内核加载时会逐个比对，
不一致就报 `disagrees about version of symbol xxx`。这些 CRC 只存在于厂商编译内核时的
`Module.symvers` 里，官方没有公开。

这里的做法是**从设备 boot 分区里的内核 Image 反推**：

```text
1. dd if=/dev/block/mmcblk0p3 取出 boot.img
2. 解析 Android boot header（page size 2048），取出 19MB 未压缩内核 Image
3. 从 /proc/kallsyms 拿到 __ksymtab / __kcrctab / 每个符号的 __kcrctab_<name> 地址
4. Image 里的 __kcrctab 是 0（启动时由 R_AARCH64_RELATIVE 重定位填充）
   解析 49107 条重定位，CRC = addend（8 字节 unsigned long）
5. 生成 8962 条 Module.symvers
```

抽样结果（可在 `Module.symvers` 里查）：

```text
0x2e1445dd  module_layout
0x53f13a5d  usb_register_driver
0x908ade3d  hci_register_dev
0xcbda1040  usb_submit_urb
0x5b1145c1  hci_alloc_dev
```

> 踩坑记录：一开始把 CRC 算成 `addend + 字段地址`，加载时报
> `btusb: disagrees about version of symbol module_layout`。
> `__kcrctab` 的条目是 **8 字节 `unsigned long`**，且重定位写入的就是 CRC 本身，不是相对值。

---

## 3. 构建

`config.txt` = 设备 `/proc/config.gz` 原样导出 + `CONFIG_BT_HCIBTUSB=m`。

GitHub Actions（`.github/workflows/build.yml`）流程：

```text
ubuntu-22.04
  apt install gcc-aarch64-linux-gnu bc flex bison libssl-dev
  下载 linux-4.9.170.tar.xz
  cp config.txt .config && make ARCH=arm64 olddefconfig
  make ARCH=arm64 HOSTCFLAGS="-O2 -fcommon" modules_prepare     # -fcommon 是 gcc>=10 编 dtc 的坑
  cp Module.symvers .
  make ARCH=arm64 M=drivers/bluetooth modules
```

产物（artifact `btusb-ko`）：

```text
btusb.ko   625,416 B   vermagic=4.9.170 SMP preempt mod_unload modversions aarch64
btbcm.ko   417,744 B
btrtl.ko   410,648 B
btintel.ko 441,224 B
hcitool    916,320 B   （BlueZ 5.55 静态编译，aarch64）
hciconfig  957,472 B
btscan     651,736 B   （自写，HCI user channel 扫描）
```

`HOSTCFLAGS=-fcommon` 是必须的：Linux 4.9 的 `scripts/dtc` 在 gcc 10+ 下会报
`multiple definition of 'yylloc'`。

---

## 4. 加载结果（Android 上）

```sh
insmod btbcm.ko; insmod btrtl.ko; insmod btintel.ko; insmod btusb.ko
```

```text
usbcore: registered new interface driver btusb
/sys/bus/usb/drivers/btusb/5-1:1.0  -> 5-1:1.0（bInterfaceClass=e0）
/sys/bus/usb/drivers/btusb/5-1:1.1  -> 5-1:1.1（bInterfaceClass=e0）
/sys/class/bluetooth/hci0
```

`hciconfig -a`：

```text
hci0:   Type: Primary  Bus: USB
        BD Address: 00:1A:7D:DA:71:13  ACL MTU: 679:8  SCO MTU: 48:16
        DOWN
        Features: 0xbf 0x3e 0x4d 0xfa 0xd8 0x3d 0x7b 0xc7
```

说明：**驱动、USB 枚举、HCI 命令通道全部正常**，控制器能应答 `Read BD Addr` / `Read Local Features`。

---

## 5. 卡住的地方

```text
hciconfig hci0 up      → Can't init device hci0: Invalid request code (56)   # EBADRQC
btscan（user channel）  → bind: Device or resource busy                      # EBUSY
hcitool cmd            → Invalid device: Network is down                     # 设备确实没 UP
```

排查过程：

```text
1. rfkill：sunxi-bt(rfkill0) soft=1，已用 /dev/rfkill 解除；hci0 自己的 rfkill soft=0
2. 停掉厂商 HAL（vendor.bluetooth-1-0）后重试：仍然 EBADRQC / EBUSY
3. 重新加载驱动、重启设备后立刻测试：仍然失败
4. 反汇编 hci_sock_ioctl：
   0x400448c9(HCIDEVUP) → capable(CAP_NET_ADMIN) 通过 → hci_dev_open()
5. 反汇编 hci_dev_open：只会返回 -19(ENODEV) / -95(EOPNOTSUPP)，其余交给 hci_dev_do_open
6. 全内核扫描 movn wN,#0x37(-56)：22 处，没有一处在蓝牙代码里
   → EBADRQC 不是主线蓝牙代码会返回的错误，是厂商内核加的门
```

Android 侧还有第二道墙：

```text
/vendor/lib/libbt-broadcom.so  libbt-realtek.so  libbt-sprd.so
/vendor/lib/libbt-xradio.so    libbt-vendor-seekwave.so
persist.vendor.overlay.bluetooth_support = 0
persist.vendor.overlay.bluetooth_vendor  = NULL
```

Android 10 的 BT HAL 走 `VendorInterface` 加载这些**厂商串口驱动**，只认自家 UART 芯片，
不会接管内核的 `hci0`；系统音频（A2DP source）走 HAL + AudioFlinger，所以即使把 `hci0` 打开，
Android 也不会把电视声音送到这个适配器。

---

## 6. 换成 Linux 系统：能用，开箱即用

这次所有坑都是 Android/厂商内核特有的。标准 Linux 上链路是：

```text
CSR8510 (USB class 0xE0/0x01/0x01)
  → btusb（主线默认 CONFIG_BT_HCIBTUSB=m）
  → BlueZ bluetoothd + /sys/class/bluetooth/hci0
  → PipeWire(bluez5) 或 PulseAudio(module-bluetooth-discover) 的 A2DP source
  → 蓝牙音箱
```

CSR8510 是标准 HCI 控制器，**不需要额外固件**，`btusb` 直接绑定。

### 6.1 在 Linux PC 上验证（推荐先做这个）

```sh
# 1) 看适配器有没有被识别
lsusb | grep -i bluetooth          # 应该看到 0a12:0001 CSR8510A10
hciconfig -a                       # hci0: Bus: USB, BD Address: 00:1A:7D:DA:71:13
rfkill list bluetooth              # 确认没有被 soft/hard block

# 2) 打开、扫描、配对
bluetoothctl
  power on
  agent on
  default-agent
  scan on                          # 让音箱进入配对模式
  pair  XX:XX:XX:XX:XX:XX
  trust XX:XX:XX:XX:XX:XX
  connect XX:XX:XX:XX:XX:XX
  quit

# 3) 看 A2DP sink 有没有出现
pactl list short sinks | grep bluez
# 或（PipeWire）
wpctl status | grep -i bluez
```

切默认输出：

```sh
# PulseAudio
pactl set-default-sink bluez_sink.XX_XX_XX_XX_XX_XX.a2dp_sink

# PipeWire / WirePlumber
wpctl status
wpctl set-default <sink-id>
```

然后随便放个音频，声音就出在音箱上。

### 6.2 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `hciconfig` 没有 hci0 | `lsmod \| grep btusb`；`modprobe btusb`；`dmesg \| tail` 看 USB 枚举 |
| `rfkill list` 显示 blocked | `rfkill unblock bluetooth`（或 `rfkill unblock all`） |
| `bluetoothctl` 扫不到设备 | 音箱没进配对模式；或适配器是 4.0 老芯片，扫描时等 10–20 秒 |
| 连上但没声音 | 没设默认 sink；或播放器直接接了 ALSA（绕过 PulseAudio/PipeWire） |
| 声音有延迟（150–300ms） | A2DP/SBC 的正常延迟；换 aptX-LL / aptX Adaptive 适配器和音箱可降到 ~40ms |
| 每次开机要手动连 | `bluetoothctl trust` 之后一般会自动连；不行就写个 udev/服务脚本 |
| PipeWire 下看不到 bluez 节点 | 装 `libspa-0.2-bluetooth`（Debian/Ubuntu）或 `pipewire-pulse` |

### 6.3 关键区别

| 环节 | Windows | Android（Q6011） | 标准 Linux |
|---|---|---|---|
| USB 驱动 | 自带 `BthUsb.sys` | 内核没编 `CONFIG_BT_HCIBTUSB` | 主线默认 `btusb` |
| 协议栈 | Windows 蓝牙栈 | 厂商 HAL 只认自家 UART 芯片 | BlueZ |
| A2DP source | 自带 | HAL 不接管 hci0 | PipeWire/PulseAudio 的 bluez 模块 |
| 结果 | 插上即用 | 驱动能加载，但打不开设备 | 插上即用 |

### 6.4 注意：chroot 不算

在 Q6011 现有的 Android 内核上跑 Linux chroot **没有用**：`EBADRQC` 是内核里的门，
chroot 里的 Linux 还是同一个内核，`hciconfig up` 一样失败。必须**换掉内核**（主线/Armbian）才行。

---

## 7. 如果真要在 Q6011 上跑 Linux

```text
SoC        Allwinner H616（sun50iw9p1），主线 5.10+ 有支持
但         没有 Q6011 这块板的 DTB，需要自己写/改：
           IR 遥控、LED、音频 codec 路由、HDMI、eMMC、USB、电源
WiFi       XR819 只有 out-of-tree 的 xradio 驱动，主线没有
启动       U-Boot 2018.05；cmdline: secure_os_exist=0, oem_unlock_supported=1, selinux=permissive
           有 vbmeta/vbmeta_system/vbmeta_vendor 分区，换 boot 有变砖风险
性能       1GB RAM / 4×A53
```

工作量评估：DTS 适配 + WiFi 驱动 + 启动链，几天到几周，且可能卡在 U-Boot 或 WiFi 上。
性价比不高——同样的钱买一块 H616/H618 SBC（Orange Pi Zero 3 之类）或迷你主机更快。

---

## 8. 仓库内容

```text
config.txt                 设备 /proc/config.gz + CONFIG_BT_HCIBTUSB=m
Module.symvers             从内核 Image 反推的 8962 条符号 CRC
tools/btscan.c             自写扫描工具（HCI user channel，不依赖 HCIDEVUP）
.github/workflows/build.yml 交叉编译 btusb + hcitool/hciconfig/btscan
```

手动复现构建：

```sh
curl -sSLo linux.tar.xz https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.9.170.tar.xz
tar xf linux.tar.xz && cd linux-4.9.170
cp ../config.txt .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- HOSTCFLAGS="-O2 -fcommon" modules_prepare -j$(nproc)
cp ../Module.symvers .
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- HOSTCFLAGS="-O2 -fcommon" M=drivers/bluetooth modules -j$(nproc)
```

## 9. 免责声明

- `Module.symvers` 是从设备固件反推的，只对 **同一固件版本**（4.9.170，2025-06-25 编译）有效。
- 加载来源不明的内核模块有崩溃/不稳定风险；本仓库产物仅用于实验。
- 修改 boot.img / 换内核有变砖风险，自行承担。
