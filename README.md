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

# 定向运行测试

启用测试构建：

```bash
./waf configure --enable-unit-test --rtems=$HOME/quick-start/rtems/6 --rtems-bsp=arm/realview_pbx_a9_qemu
./waf
```

当前测试框架是单一测试二进制。对 RTEMS/QEMU 这类运行方式，最可靠的筛选入口是 `./waf configure` 时写入配置头；如果后续运行环境支持 `getenv()`，也可以在运行时覆盖。

- `--test-group=...` / `RTFS_TEST_GROUP`：按测试组精确筛选。组名默认从 `test/<group>/...` 路径推导，例如 `test/fs/*.c` 属于 `fs` 组，`test/integration/*.c` 属于 `integration` 组。
- `--test-filter=...` / `RTFS_TEST_FILTER`：按测试名子串筛选。
- `--list-tests` / `RTFS_TEST_LIST=1`：只列出已注册测试，不执行。
- `--itest-device-mode=ramdisk|external` / `RTFS_ITEST_DEVICE_MODE`：RTEMS 挂载类集成测试使用内存盘或真实块设备。默认 `ramdisk`。
- `--itest-device-path=/dev/...` / `RTFS_ITEST_DEVICE_PATH`：外部块设备路径。未指定时，`external` 模式会在运行时扫描 `/dev`，若只找到一个可用块设备则自动使用；若找到多个，则打印候选列表并退出，避免误格式化错误磁盘。

示例：

```bash
./waf configure --enable-unit-test --test-group=fs --rtems=$HOME/quick-start/rtems/6 --rtems-bsp=arm/realview_pbx_a9_qemu
./waf
qemu-system-arm -no-reboot -nographic -M realview-pbx-a9 -m 256M -kernel ./build/arm-rtems6-realview_pbx_a9_qemu/main.exe
```

```bash
./waf configure --enable-unit-test --test-group=integration --test-filter=Recovery --rtems=$HOME/quick-start/rtems/6 --rtems-bsp=arm/realview_pbx_a9_qemu
./waf
qemu-system-arm -no-reboot -nographic -M realview-pbx-a9 -m 256M -kernel ./build/arm-rtems6-realview_pbx_a9_qemu/main.exe
```

```bash
./waf configure --enable-unit-test --list-tests --rtems=$HOME/quick-start/rtems/6 --rtems-bsp=arm/realview_pbx_a9_qemu
./waf
qemu-system-arm -no-reboot -nographic -M realview-pbx-a9 -m 256M -kernel ./build/arm-rtems6-realview_pbx_a9_qemu/main.exe
```

如果你使用仓库根目录的 `Makefile`，可以直接用短命令：

```bash
make test-group TEST_GROUP=fs
make test-filter TEST_FILTER=Recovery
make test-group TEST_GROUP=integration TEST_FILTER=Recovery
make test-list
```

其中 `make test-list` 仍然会启动 RTEMS/QEMU，只是测试程序进入“列出后退出”模式，不执行测试体。

真实板子上跑 RTEMS 挂载类集成测试时，可以先不提供设备路径，先让程序探测：

```bash
make integration-test ITEST_DEVICE_MODE=external
```

如果日志里发现多个可用块设备，再明确指定其中一个：

```bash
make integration-test ITEST_DEVICE_MODE=external ITEST_DEVICE_PATH=/dev/sdX
```
