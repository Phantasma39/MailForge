# ============================================================================
#  MailForge 统一构建脚本（根目录）
#
#  常用命令：
#    make server         编译邮件服务器（含 Web 后端 + 数字信封加密）
#                        产物：MailServer/mail_server
#    make run            编译并启动服务器（浏览器打开 http://localhost:8080）
#    make client         编译 SMTP/POP3 客户端演示    产物：MailServer/mail_client_test
#    make test-crypto    运行加密子系统自测（crypto/，需要 keys/ 目录，会自动创建）
#    make demo-crypto    编译并运行加密演示工具（信封演示 / 可视化 / CLI）
#    make clean          清理全部产物与运行数据
#
#  环境要求：g++（C++17）、make、OpenSSL 3.0（libssl-dev / -lssl -lcrypto）
#  说明：服务器主体用 Linux/POSIX socket API；加密子系统的自测可跨平台
#        （MSYS2/Windows 下会自动链接 Winsock）。
# ============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -pthread -Wall -Wextra -O2 -g -I MailServer/include -I .
LDLIBS   := -lssl -lcrypto

# 平台探测：原生 Windows（MinGW-w64 / MSYS2）下 crypto 子系统需要 Winsock
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
CRYPTO_SRC = crypto/openssl_util.cpp crypto/aes.cpp crypto/rsa.cpp \
             crypto/envelope.cpp \
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
# 加密子系统（crypto/ 目录，mail:: 命名空间）—— 自测与演示工具
# ----------------------------------------------------------------------------
C_COMMON = common/base64.cpp common/logger.cpp common/file_util.cpp
C_CRYPTO = crypto/openssl_util.cpp crypto/aes.cpp crypto/rsa.cpp crypto/envelope.cpp

T_B64  = crypto/tests/test_base64
T_LOG  = crypto/tests/test_logger
T_SOCK = crypto/tests/test_socket
T_CRYP = crypto/tests/test_crypto
D_DEMO = crypto/tests/demo_envelope
D_VIS  = crypto/tests/demo_visual
C_CLI  = crypto/tools/crypto_cli
CRYPTO_BINS = $(T_B64) $(T_LOG) $(T_SOCK) $(T_CRYP) $(D_DEMO) $(D_VIS) $(C_CLI)

$(T_B64): crypto/tests/test_base64.cpp common/base64.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

$(T_LOG): crypto/tests/test_logger.cpp common/logger.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(T_SOCK): crypto/tests/test_socket.cpp common/socket.cpp common/logger.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(T_CRYP): crypto/tests/test_crypto.cpp $(C_COMMON) $(C_CRYPTO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(D_DEMO): crypto/tests/demo_envelope.cpp $(C_COMMON) $(C_CRYPTO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(D_VIS): crypto/tests/demo_visual.cpp $(C_COMMON) $(C_CRYPTO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(C_CLI): crypto/tools/crypto_cli.cpp $(C_COMMON) $(C_CRYPTO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test-crypto: $(T_B64) $(T_LOG) $(T_SOCK) $(T_CRYP)
	mkdir -p keys
	./$(T_B64) && ./$(T_LOG) && ./$(T_SOCK) && ./$(T_CRYP)

demo-crypto: $(D_DEMO) $(D_VIS) $(C_CLI)
	./$(D_DEMO)

all: server client test-crypto

clean:
	rm -f MailServer/mail_server MailServer/mail_client_test
	rm -f $(CRYPTO_BINS)
	rm -f common/*.o crypto/*.o crypto/tests/*.o crypto/tools/*.o
	rm -rf keys

.PHONY: all server run client test-crypto demo-crypto clean


