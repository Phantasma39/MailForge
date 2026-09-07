# ============================================================================
#  MailForge 统一构建脚本（根目录）
#
#  常用命令：
#    make server         编译邮件服务器（SMTP/POP3/HTTP + 自研 AES/ChaCha 加密）
#                        产物：MailServer/mail_server
#    make run            编译并启动服务器（浏览器打开 http://localhost:8080）
#    make client         编译 SMTP/POP3 客户端演示    产物：MailServer/mail_client_test
#    make test-crypto    运行加密子系统自测（AES-256-CBC + ChaCha20 官方向量）
#    make clean          清理全部产物与运行数据
#
#  环境要求：g++（C++17）、make。无需 OpenSSL —— 加密算法为纯自研实现：
#    crypto/aes.cpp（AES-256-CBC + PKCS#7）、crypto/chacha20.cpp（RFC 8439）。
#  说明：服务器主体用 Linux/POSIX socket API；原生 Windows（MinGW）下自动
#        链接 Winsock（socket 层用）。
# ============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -pthread -Wall -Wextra -O2 -g -I MailServer/include -I .
LDLIBS   :=

# 平台探测：原生 Windows（MinGW-w64 / MSYS2）下 socket 相关需要 Winsock
WIN32_PROBE := $(shell echo | $(CXX) -dM -E -x c++ - | grep -c 'define _WIN32 ')
ifeq ($(WIN32_PROBE),1)
LDLIBS += -lws2_32
endif

# ----------------------------------------------------------------------------
# 邮件服务器（MailServer/ 协议与 HTTP 实现 + crypto/ common/ 自研加密子系统）
# ----------------------------------------------------------------------------
MAIL_SRC = MailServer/main.cpp \
           MailServer/src/Server.cpp MailServer/src/SmtpServer.cpp \
           MailServer/src/Pop3Server.cpp MailServer/src/SmtpClient.cpp \
           MailServer/src/Pop3Client.cpp \
           MailServer/src/HttpServer.cpp MailServer/src/MailCrypto.cpp
CRYPTO_SRC = crypto/random.cpp crypto/aes.cpp crypto/chacha20.cpp \
             common/base64.cpp common/logger.cpp common/file_util.cpp

server: MailServer/mail_server

MailServer/mail_server: $(MAIL_SRC) $(CRYPTO_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(MAIL_SRC) $(CRYPTO_SRC) $(LDLIBS)

run: server
	cd MailServer && ./mail_server

# ----------------------------------------------------------------------------
# 客户端演示（client_test.cpp：SMTP 发信 + POP3 收信/删信全流程）
# ----------------------------------------------------------------------------
client: MailServer/mail_client_test

MailServer/mail_client_test: MailServer/client_test.cpp \
                             MailServer/src/SmtpClient.cpp MailServer/src/Pop3Client.cpp
	$(CXX) $(CXXFLAGS) -o $@ MailServer/client_test.cpp \
	     MailServer/src/SmtpClient.cpp MailServer/src/Pop3Client.cpp

# ----------------------------------------------------------------------------
# 加密子系统自测（crypto/ 目录，mail:: 命名空间）—— 纯自研，无第三方依赖
# ----------------------------------------------------------------------------
C_COMMON = common/base64.cpp common/logger.cpp common/file_util.cpp
C_CRYPTO = crypto/random.cpp crypto/aes.cpp crypto/chacha20.cpp

T_B64  = crypto/tests/test_base64
T_LOG  = crypto/tests/test_logger
T_SOCK = crypto/tests/test_socket
T_CRYP = crypto/tests/test_crypto
CRYPTO_BINS = $(T_B64) $(T_LOG) $(T_SOCK) $(T_CRYP)

$(T_B64): crypto/tests/test_base64.cpp common/base64.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(T_LOG): crypto/tests/test_logger.cpp common/logger.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(T_SOCK): crypto/tests/test_socket.cpp common/socket.cpp common/logger.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(T_CRYP): crypto/tests/test_crypto.cpp $(C_COMMON) $(C_CRYPTO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test-crypto: $(T_B64) $(T_LOG) $(T_SOCK) $(T_CRYP)
	mkdir -p keys
	./$(T_B64) && ./$(T_LOG) && ./$(T_SOCK) && ./$(T_CRYP)

all: server client test-crypto

clean:
	rm -f MailServer/mail_server MailServer/mail_client_test
	rm -f $(CRYPTO_BINS)
	rm -f common/*.o crypto/*.o crypto/tests/*.o
	rm -rf keys

.PHONY: all server run client test-crypto clean



