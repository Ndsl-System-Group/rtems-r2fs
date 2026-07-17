# RTFS Makefile
# 
# 使用方法：
#   make configure                             - 配置项目，默认不带 unit test
#   make configure-with-unit-test              - 配置项目并启用 unit test
#   make configure-with-unit-test-coverage     - 配置项目并启用 unit test 和 coverage
#   make test                                  - 配置、构建并运行全部测试
#   make integration-test                      - 运行 test/integration/ 目录下的全部测试
#   make test-group TEST_GROUP=fs              - 定向运行某个测试组
#   make test-filter TEST_FILTER=Recovery      - 按测试名子串定向运行
#   make test ITEST_DEVICE_MODE=external       - 集成测试切到真实块设备并自动扫描 /dev
#   make perf-test                             - 运行全部性能测试
#   make perf-streaming                        - 运行大文件流式性能测试
#   make perf-metadata                         - 运行小文件/元数据性能测试
#   make test-list                             - 启动测试程序并列出已注册测试
#   make coverage-run                          - 运行带 coverage 的程序并保存 QEMU 日志
#   make coverage-report COVERAGE_SOURCE=...   - 从覆盖率日志提取指定源文件报告
#   make coverage-report-all                   - 为指定前缀下的全部源文件生成覆盖率报告
#   make coverage COVERAGE_SOURCE=...          - 配置、运行并生成指定源文件覆盖率报告
#   make coverage-all                          - 配置、运行并生成指定前缀下的全部覆盖率报告
#   make build                                 - 构建项目
#   make build-unit-test                       - 配置并构建 unit test 版本
#   make build-d2000-unit-test-image           - 构建 unit test 版本并生成 D2000 exe/bin，默认 external 模式
#   make build-d2000-integration-image         - 构建 integration 全量场景版本并生成 D2000 exe/bin，默认 external 模式
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
	if [ "$(ENABLE_COVERAGE)" = "1" ]; then echo "Coverage: Enabled"; else echo "Coverage: Disabled"; fi; \
	if [ -n "$(TEST_GROUP)" ]; then echo "Test Group: $(TEST_GROUP)"; fi; \
	if [ -n "$(TEST_FILTER)" ]; then echo "Test Filter: $(TEST_FILTER)"; fi; \
	if [ "$(LIST_TESTS)" = "1" ]; then echo "List Tests Mode: Enabled"; fi; \
	if [ -n "$(ITEST_DEVICE_MODE)" ]; then echo "ITest Device Mode: $(ITEST_DEVICE_MODE)"; fi; \
	if [ -n "$(ITEST_DEVICE_PATH)" ]; then echo "ITest Device Path: $(ITEST_DEVICE_PATH)"; fi; \
	./waf configure \
		$(WAF_CONFIG_ARGS) \
		$(WAF_UNIT_TEST_ARGS); \

configure-with-unit-test-coverage:
	@$(MAKE) configure-with-unit-test ENABLE_COVERAGE=1 TEST_GROUP="$(TEST_GROUP)" TEST_FILTER="$(TEST_FILTER)" LIST_TESTS="$(LIST_TESTS)"

test: configure-with-unit-test run

integration-test:
	@$(MAKE) test TEST_GROUP="integration,performance" TEST_FILTER="$(TEST_FILTER)"

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

coverage-run: build
	@echo "Running RTFS coverage..."
	@echo "Coverage Log: $(COVERAGE_LOG)"
	@mkdir -p $(dir $(COVERAGE_LOG)) "$(COVERAGE_OUTPUT_DIR)"
	@export QEMU_AUDIO_DRV="none" && \
	$(QEMU) $(QEMU_OPTS) -kernel $(QEMU_KERNEL) | tee "$(COVERAGE_LOG)"

coverage-report:
	@echo "Generating coverage report..."
	@echo "Coverage Source: $(COVERAGE_SOURCE)"
	@if [ ! -f "$(COVERAGE_LOG)" ]; then \
		echo "Coverage log not found: $(COVERAGE_LOG)"; \
		exit 1; \
	fi
	@python3 tools/module_coverage.py \
		--qemu-output "$(COVERAGE_LOG)" \
		--build-dir build \
		--source "$(COVERAGE_SOURCE)" \
		--gcov-tool "$(GCOV_TOOL)" \
		--gcov "$(GCOV)" \
		--output-dir "$(COVERAGE_OUTPUT_DIR)"

coverage-report-all:
	@echo "Generating coverage reports..."
	@echo "Coverage Prefixes: $(COVERAGE_SOURCE_PREFIXES)"
	@if [ ! -f "$(COVERAGE_LOG)" ]; then \
		echo "Coverage log not found: $(COVERAGE_LOG)"; \
		exit 1; \
	fi
	@python3 tools/module_coverage.py \
		--qemu-output "$(COVERAGE_LOG)" \
		--build-dir build \
		--all-sources \
		--source-prefixes "$(COVERAGE_SOURCE_PREFIXES)" \
		--gcov-tool "$(GCOV_TOOL)" \
		--gcov "$(GCOV)" \
		--output-dir "$(COVERAGE_OUTPUT_DIR)"

coverage:
	@$(MAKE) configure-with-unit-test-coverage TEST_GROUP="$(TEST_GROUP)" TEST_FILTER="$(TEST_FILTER)" LIST_TESTS="$(LIST_TESTS)"
	@$(MAKE) coverage-run ENABLE_COVERAGE=1
	@$(MAKE) coverage-report ENABLE_COVERAGE=1 COVERAGE_SOURCE="$(COVERAGE_SOURCE)"

coverage-all:
	@$(MAKE) configure-with-unit-test-coverage TEST_GROUP="$(TEST_GROUP)" TEST_FILTER="$(TEST_FILTER)" LIST_TESTS="$(LIST_TESTS)"
	@$(MAKE) coverage-run ENABLE_COVERAGE=1
	@$(MAKE) coverage-report-all ENABLE_COVERAGE=1 COVERAGE_SOURCE_PREFIXES="$(COVERAGE_SOURCE_PREFIXES)"

perf-test:
	@$(MAKE) perf-streaming
	@$(MAKE) perf-metadata

perf-streaming:
	@$(MAKE) test-filter TEST_GROUP=performance TEST_FILTER=PerformanceStreaming

perf-metadata:
	@$(MAKE) test-filter TEST_GROUP=performance TEST_FILTER=PerformanceMetadata

# 构建项目
build:
	@echo "Building RTFS FileSystem..."
	./waf

build-unit-test:
	@$(MAKE) configure-with-unit-test
	@$(MAKE) build

package-d2000-unit-image:
	@echo "Packaging RTFS D2000 unit-test image..."
	@if [ ! -f "$(ELF_PATH)" ]; then \
		echo "ELF not found: $(ELF_PATH)"; \
		echo "Run: make build-unit-test"; \
		exit 1; \
	fi
	@mkdir -p "$(TEST_IMAGES_DIR)"
	@cp -f "$(ELF_PATH)" "$(OUT_EXE)"
	@"$(OBJCOPY)" -O binary "$(ELF_PATH)" "$(OUT_BIN)"
	@chmod +x "$(OUT_EXE)" "$(OUT_BIN)"
	@echo "Generated:"
	@echo "  $(OUT_EXE)"
	@echo "  $(OUT_BIN)"

build-d2000-unit-test-image:
	@$(MAKE) build-unit-test ITEST_DEVICE_MODE="$(if $(strip $(ITEST_DEVICE_MODE)),$(ITEST_DEVICE_MODE),external)"
	@$(MAKE) package-d2000-unit-image

build-d2000-integration-image:
	@$(MAKE) build-unit-test TEST_GROUP="integration,performance" TEST_FILTER="$(TEST_FILTER)" ITEST_DEVICE_MODE="$(if $(strip $(ITEST_DEVICE_MODE)),$(ITEST_DEVICE_MODE),external)"
	@$(MAKE) package-d2000-unit-image IMAGE_BASENAME=main_phytium_d2000_test_integration

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

.PHONY: all configure configure-with-unit-test configure-with-unit-test-coverage test integration-test test-group test-filter test-list coverage-run coverage-report coverage-report-all coverage coverage-all perf-test perf-streaming perf-metadata build build-unit-test package-d2000-unit-image build-d2000-unit-test-image build-d2000-integration-image run clean
