# R2FS Makefile
# 
# 使用方法：
#   make configure  - 配置项目，默认不带 unit test
#   make configure-with-unit-test - 配置项目并启动 unit test
#   make build      - 构建项目
#   make run        - 运行项目
#   make clean      - 清理构建文件

# 默认配置
RTEMS_ROOT ?= /home/rtems/rtems_arm
BSP ?= arm/realview_pbx_a9_qemu

# 项目配置
PROJECT_NAME = main
TARGET = $(PROJECT_NAME).exe

# 默认目标
all: build

# 配置项目，默认不带 unit test
configure:
	@echo "Configuring R2FS FileSystem..."
	@echo "Unit Test Disabled"; \
	./waf configure \
		--rtems=$(RTEMS_ROOT) \
		--rtems-bsp=$(BSP); \

# 配置项目并启动 unit test
configure-with-unit-test:
	@echo "Configuring R2FS FileSystem..."
	@echo "Unit Test Enabled"; \
	./waf configure \
		--rtems=$(RTEMS_ROOT) \
		--rtems-bsp=$(BSP) \
		--enable-unit-test; \

# 构建项目
build:
	@echo "Building R2FS FileSystem..."
	./waf

# 运行项目
run: build
	@echo "Running R2FS FileSystem..."
	@export QEMU_AUDIO_DRV="none" && \
	qemu-system-arm -no-reboot -nographic -M realview-pbx-a9 -m 256M \
		-kernel ./build/arm-rtems6-realview_pbx_a9_qemu/$(TARGET)

# 清理构建文件
clean:
	@echo "Cleaning build files..."
	./waf clean
	rm -rf build/

.PHONY: all configure configure-with-unit-test build run clean
