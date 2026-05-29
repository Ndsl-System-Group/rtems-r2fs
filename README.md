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
