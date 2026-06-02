# RTFS Makefile
# 
# 使用方法：
#   make configure                             - 配置项目，默认不带 unit test
#   make configure-with-unit-test              - 配置项目并启用 unit test
#   make test                                  - 配置、构建并运行全部测试
#   make test-group TEST_GROUP=fs              - 定向运行某个测试组
#   make test-filter TEST_FILTER=Recovery      - 按测试名子串定向运行
#   make test-list                             - 启动测试程序并列出已注册测试
#   make build                                 - 构建项目
#   make run                                   - 运行项目
#   make clean                                 - 清理构建文件

-include config.mk


# 默认目标
all: build

# 配置项目，默认不带 unit test
configure:
	@echo "Configuring RTFS FileSystem..."
	@echo "Unit Test Disabled"; \
	./waf configure \
		$(WAF_CONFIG_ARGS); \

# 配置项目并启动 unit test
configure-with-unit-test:
	@echo "Configuring RTFS FileSystem..."
	@echo "Unit Test Enabled"; \
	if [ -n "$(TEST_GROUP)" ]; then echo "Test Group: $(TEST_GROUP)"; fi; \
	if [ -n "$(TEST_FILTER)" ]; then echo "Test Filter: $(TEST_FILTER)"; fi; \
	if [ "$(LIST_TESTS)" = "1" ]; then echo "List Tests Mode: Enabled"; fi; \
	./waf configure \
		$(WAF_CONFIG_ARGS) \
		$(WAF_UNIT_TEST_ARGS); \

test: configure-with-unit-test run

test-group:
	@if [ -z "$(strip $(TEST_GROUP))" ]; then \
		echo "TEST_GROUP is required, example: make test-group TEST_GROUP=fs"; \
		exit 1; \
	fi
	@$(MAKE) test TEST_GROUP="$(TEST_GROUP)" TEST_FILTER="$(TEST_FILTER)"

test-filter:
	@if [ -z "$(strip $(TEST_FILTER))" ]; then \
		echo "TEST_FILTER is required, example: make test-filter TEST_FILTER=Recovery"; \
		exit 1; \
	fi
	@$(MAKE) test TEST_GROUP="$(TEST_GROUP)" TEST_FILTER="$(TEST_FILTER)"

test-list:
	@$(MAKE) test LIST_TESTS=1 TEST_GROUP="$(TEST_GROUP)" TEST_FILTER="$(TEST_FILTER)"

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

.PHONY: all configure configure-with-unit-test test test-group test-filter test-list build run clean
