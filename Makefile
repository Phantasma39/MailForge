# ============================================================================
#  MailForge 统一构建脚本（根目录）
#
#  常用命令：
#    make server         编译邮件服务器（SMTP/POP3/HTTP + 两层加密）
#                        产物：MailServer/mail_server
#    make run            编译并启动服务器（浏览器打开 http://localhost:8080）
#    make client         编译 SMTP/POP3 客户端演示    产物：MailServer/mail_client_test
#    make test-crypto    加密子系统单元自测（纯自研，不依赖 OpenSSL）
#    make clean          清理全部产物与运行数据
#
#  两层加密模型：
#    层1 Web↔服务器(8080)   ：RSA-2048 数字信封（AES-256-GCM + RSA-OAEP）。
#                             服务器端调用 OpenSSL 库（crypto/rsa.* openssl_util.*）；
#                             浏览器端用原生 WebCrypto，无第三方库。
#    层2 服务器内部端口之间  ：自研 AES-256-CBC（crypto/aes.*，PKCS#7）与
#                             ChaCha20（crypto/chacha20.*，RFC 8439），纯自研不调库。
#  环境要求：g++（C++17）、make、OpenSSL 开发库（libssl-dev，层1 用；crypto 自测不需要）。
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
# 邮件服务器（MailServer/ 协议与 HTTP 实现 + crypto/ common/ 加密子系统）
# ----------------------------------------------------------------------------
MAIL_SRC = MailServer/main.cpp \
           MailServer/src/Server.cpp MailServer/src/SmtpServer.cpp \
           MailServer/src/Pop3Server.cpp MailServer/src/SmtpClient.cpp \
           MailServer/src/Pop3Client.cpp \
           MailServer/src/HttpServer.cpp MailServer/src/MailCrypto.cpp
CRYPTO_SRC = crypto/random.cpp crypto/aes.cpp crypto/chacha20.cpp \
             common/base64.cpp common/logger.cpp common/file_util.cpp
# 层1（Web↔服务器 RSA 信封）依赖 OpenSSL；仅服务器链接
WEBSSL_SRC = crypto/rsa.cpp crypto/openssl_util.cpp
WEBSSL_LIBS = -lssl -lcrypto

server: MailServer/mail_server

MailServer/mail_server: $(MAIL_SRC) $(CRYPTO_SRC) $(WEBSSL_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(MAIL_SRC) $(CRYPTO_SRC) $(WEBSSL_SRC) $(LDLIBS) $(WEBSSL_LIBS)

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

# Web↔服务器 RSA 信封单元测试（第一层，OpenSSL 库；需要 libssl-dev）
T_RSA  = crypto/tests/test_rsa_web
$(T_RSA): crypto/tests/test_rsa_web.cpp MailServer/src/MailCrypto.cpp \
          crypto/rsa.cpp crypto/openssl_util.cpp crypto/random.cpp \
          crypto/aes.cpp crypto/chacha20.cpp \
          common/base64.cpp common/logger.cpp common/file_util.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ -lssl -lcrypto

test-rsa: $(T_RSA)
	rm -rf keys
	./$(T_RSA)
	rm -rf keys

all: server client test-crypto

clean:
	rm -f MailServer/mail_server MailServer/mail_client_test
	rm -f $(CRYPTO_BINS) $(T_RSA)
	rm -f common/*.o crypto/*.o crypto/tests/*.o
	rm -rf keys

.PHONY: all server run client test-crypto test-rsa clean



