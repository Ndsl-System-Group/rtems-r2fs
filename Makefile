# RTFS Makefile
# 
# 使用方法：
#   make configure  - 配置项目，默认不带 unit test
#   make configure-with-unit-test - 配置项目并启动 unit test
#   make build      - 构建项目
#   make run        - 运行项目
#   make clean      - 清理构建文件

-include config.mk


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
