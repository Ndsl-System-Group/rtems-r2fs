# RTFS Makefile
# 
# 使用方法：
#   make configure  - 配置项目，默认不带 unit test
#   make configure-with-unit-test - 配置项目并启动 unit test
#   make build      - 构建项目
#   make run        - 运行项目
#   make clean      - 清理构建文件

# 默认配置
RTEMS_ROOT ?= # 填入你自己的 RTEMS 安装路径，例如：$(HOME)/quick-start/rtems/6
BSP ?= # 填入你自己的 BSP，例如：arm/realview_pbx_a9_qemu

# 项目配置
PROJECT_NAME = main
TARGET = $(PROJECT_NAME).exe

# QEMU 配置
QEMU ?= # 填入你自己的 qemu 程序，例如：qemu-system-arm
QEMU_OPTS ?= # 填入你自己的板级、内存、显示等参数，例如：-no-reboot -nographic -M realview-pbx-a9 -m 256M
QEMU_KERNEL ?= # 填入最终运行的 ELF，例如：./build/arm-rtems6-realview_pbx_a9_qemu/$(TARGET)

# 默认目标
all: build

# 配置项目，默认不带 unit test
configure:
	@echo "Configuring RTFS FileSystem..."
	@echo "Unit Test Disabled"; \
	./waf configure \
		--rtems=$(RTEMS_ROOT) \
		--rtems-bsp=$(BSP); \

# 配置项目并启动 unit test
configure-with-unit-test:
	@echo "Configuring RTFS FileSystem..."
	@echo "Unit Test Enabled"; \
	./waf configure \
		--rtems=$(RTEMS_ROOT) \
		--rtems-bsp=$(BSP) \
		--enable-unit-test; \

# 构建项目
build:
	@echo "Building RTFS FileSystem..."
	./waf

# 运行项目
run: build
	@echo "Running RTFS FileSystem..."
	@export QEMU_AUDIO_DRV="none" && \
	$(QEMU) $(QEMU_OPTS) -kernel $(QEMU_KERNEL)

# 清理构建文件
clean:
	@echo "Cleaning build files..."
	./waf clean
	rm -rf build/

.PHONY: all configure configure-with-unit-test build run clean
