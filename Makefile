# Makefile —— 邮件加密系统构建脚本
# 使用方式（MSYS2 bash / WSL / Linux）：
#   make            # 编译全部（主程序 + 测试）
#   make test       # 运行全部自测
#   make test-base64 / test-logger / test-crypto
#   make stress     # 100 次压力测试（Step 6 后可用）
#   make clean
#
# Windows 注意：请在 MSYS2 bash 中运行本 Makefile（中文路径兼容）

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g -I. \
            -finput-charset=UTF-8 -fexec-charset=UTF-8
LDLIBS   += -lpthread -lcrypto
# 平台探测：用编译器预定义宏判断是否原生 Windows(MinGW-w64)，需要链接 Winsock。
#   不用 uname：其在 MSYS2 下输出随 MSYSTEM 变化，且 WSL 可能继承 Windows 的 OS 变量。
WIN32_PROBE := $(shell echo | $(CXX) -dM -E -x c++ - | grep -c 'define _WIN32 ')
ifeq ($(WIN32_PROBE),1)
LDLIBS   += -lws2_32
endif

COMMON_SRCS = common/base64.cpp common/logger.cpp common/file_util.cpp
CRYPTO_SRCS = crypto/openssl_util.cpp crypto/aes.cpp crypto/rsa.cpp \
              crypto/envelope.cpp
SOCKET_SRCS = common/socket.cpp   # Socket 单独成组，避免旧目标被迫依赖 Winsock

COMMON_OBJS = $(COMMON_SRCS:.cpp=.o)
CRYPTO_OBJS = $(CRYPTO_SRCS:.cpp=.o)
SOCKET_OBJS = $(SOCKET_SRCS:.cpp=.o)

# ---------- 测试目标 ----------
TEST_BASE64 = tests/test_base64
TEST_LOGGER = tests/test_logger
TEST_CRYPTO = tests/test_crypto
TEST_SOCKET = tests/test_socket
TEST_DEMO   = tests/demo_envelope
TEST_VISUAL = tests/demo_visual
TEST_CLI    = tools/crypto_cli

all: $(TEST_BASE64) $(TEST_LOGGER) $(TEST_CRYPTO) $(TEST_SOCKET) $(TEST_DEMO) $(TEST_VISUAL) $(TEST_CLI)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_BASE64): tests/test_base64.cpp common/base64.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_LOGGER): tests/test_logger.cpp common/logger.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TEST_SOCKET): tests/test_socket.cpp common/socket.o common/logger.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TEST_CRYPTO): tests/test_crypto.cpp $(COMMON_OBJS) $(CRYPTO_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TEST_DEMO): tests/demo_envelope.cpp $(COMMON_OBJS) $(CRYPTO_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TEST_VISUAL): tests/demo_visual.cpp $(COMMON_OBJS) $(CRYPTO_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TEST_CLI): tools/crypto_cli.cpp $(COMMON_OBJS) $(CRYPTO_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# ---------- 运行测试 ----------
test: test-base64 test-logger test-crypto test-socket

test-base64: $(TEST_BASE64)
	./$(TEST_BASE64)

test-logger: $(TEST_LOGGER)
	./$(TEST_LOGGER)

test-socket: $(TEST_SOCKET)
	./$(TEST_SOCKET)

test-crypto: $(TEST_CRYPTO)
	./$(TEST_CRYPTO)

# ---------- 演示 ----------
demo: $(TEST_DEMO)
	./$(TEST_DEMO)

demo-visual: $(TEST_VISUAL)
	./$(TEST_VISUAL)

cli: $(TEST_CLI)

# ---------- 压力测试（Step 6 完成后启用） ----------
stress:
	@echo "压力测试将在 Step 6 接入"

clean:
	rm -f $(TEST_BASE64) $(TEST_LOGGER) $(TEST_CRYPTO) $(TEST_DEMO) $(TEST_VISUAL) $(TEST_CLI)
	rm -f $(COMMON_OBJS) $(CRYPTO_OBJS) $(SOCKET_OBJS)
	rm -f keys/*.pem

.PHONY: all test test-base64 test-logger test-crypto test-socket demo demo-visual cli stress clean

