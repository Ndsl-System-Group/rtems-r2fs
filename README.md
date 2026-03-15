# rtfs

Rtems 下的一个简单的由 f2fs 迁移而来的文件系统。

# 如何构建

使用 Waf 的 configure 命令配置应用程序：

```bash
./waf configure --rtems=$HOME/quick-start/rtems/6 --rtems-bsp=arm/realview_pbx_a9_qemu
```

构建应用程序：

```bash
./waf
```

运行程序：

```bash
export QEMU_AUDIO_DRV="none"

qemu-system-arm -no-reboot -nographic -M realview-pbx-a9 -m 256M -kernel ./build/arm-rtems6-realview_pbx_a9_qemu/main.exe
```

