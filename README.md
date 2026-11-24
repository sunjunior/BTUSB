# btusb for Skyworth Q6011 (kernel 4.9.170, aarch64)

目标：给创维 Q6011（`eros-p1`）编一个可加载的 `btusb.ko`，让 USB 蓝牙适配器（CSR8510）能被内核识别成 `/dev/hci0`。

## 背景

```text
内核       4.9.170（Skyworth 2025-06-25 编译，Linaro GCC 5.3.1）
vermagic   4.9.170 SMP preempt mod_unload modversions aarch64
用户态     armeabi-v7a（32 位），内核是 aarch64
CONFIG_MODVERSIONS=y       → 模块里的符号 CRC 必须和内核完全一致
CONFIG_MODULE_SIG  未开     → 不需要签名
CONFIG_BT_HCIBTUSB 未编     → 本仓库补编成模块
```

## Module.symvers 是怎么来的

厂商没公开 `Module.symvers`，这里从设备 boot 分区里的内核 Image 反推：

1. `dd if=/dev/block/mmcblk0p3` 取出 boot.img，解析 Android boot header，取出内核 Image；
2. 在 Image 里定位 `__ksymtab` / `__kcrctab`（文件里全是 0，靠 RELA 重定位在启动时填充）；
3. 解析 49107 条 `R_AARCH64_RELATIVE` 重定位；
4. 用 `crc = addend + 字段地址` 还原每个导出符号的 CRC。

产出 `Module.symvers`（8962 条），例如：

```text
0x5cdb7555  usb_register_driver
0x9974444d  hci_register_dev
0xd4c44cb0  usb_submit_urb
0x63faab71  hci_alloc_dev
```

## 构建

GitHub Actions（`.github/workflows/build.yml`）跑两个变体：

| 变体 | 说明 |
|---|---|
| `default` | 完全按厂商 config（不含 `CONFIG_MODULE_REL_CRCS`） |
| `relcrc` | 额外打开 `CONFIG_MODULE_REL_CRCS=y`（实测内核的 CRC 表是重定位填充的，说明真实构建可能开了这个） |

内核源码用 `linux-4.9.170.tar.xz`（kernel.org），交叉编译 `aarch64-linux-gnu-`。

## 加载测试

```sh
adb push btusb.ko /data/local/tmp/
adb shell /system/xbin/usrchan 0 insmod /data/local/tmp/btusb.ko
adb shell ls -l /dev/hci0
adb shell dmesg | tail -20
```

失败时常见报错：

```text
disagrees about version of symbol xxx   → CRC 不对，换另一个变体
no symbol version for xxx               → Module.symvers 缺符号
version magic '...' should be '...'     → vermagic 不匹配
```

## 注意

即使 `hci0` 出现，Android 10 的蓝牙 HAL（`libbt-broadcom/realtek/sprd/xradio/seekwave`）只支持厂商串口芯片，
不会接管内核的 HCI 设备；系统音频（A2DP source）仍然走不通。本仓库只验证到"驱动可加载 + 能扫描配对"。
