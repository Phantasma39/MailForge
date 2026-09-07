CXX      = g++
CXXFLAGS = -std=c++17 -pthread -Wall -Wextra -I include -I ..
TARGET   = mail_server

# MailForge 服务器本体源码
MAIN_SRC = main.cpp \
           src/Server.cpp src/SmtpServer.cpp src/Pop3Server.cpp \
           src/SmtpClient.cpp src/Pop3Client.cpp \
           src/HttpServer.cpp src/MailCrypto.cpp

# 加密子系统源码（contrib/crypto-project 合并进来，mail:: 命名空间，依赖 OpenSSL）
CRYPTO_SRC = ../crypto/openssl_util.cpp ../crypto/aes.cpp ../crypto/rsa.cpp \
             ../crypto/envelope.cpp \
             ../common/base64.cpp ../common/logger.cpp ../common/file_util.cpp

LDLIBS   = -lssl -lcrypto

all: $(TARGET)

$(TARGET): $(MAIN_SRC) $(CRYPTO_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(MAIN_SRC) $(CRYPTO_SRC) $(LDLIBS)

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
