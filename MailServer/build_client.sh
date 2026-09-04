#!/bin/bash
# ============================================================================
#  MailServer 客户端演示程序 一键编译脚本
#
#  用法：bash build_client.sh
#  作用：把 client_test.cpp 连同 SMTP/POP3 客户端库编译成 ./mail_client_test
# ============================================================================
cd "$(dirname "$0")"

g++ -std=c++17 -pthread -Wall -Wextra \
    -o mail_client_test \
    client_test.cpp src/SmtpClient.cpp src/Pop3Client.cpp \
    -I include

echo ""
echo "编译完成！先启动服务器（./mail_server），再运行：  ./mail_client_test"
