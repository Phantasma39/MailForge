#!/bin/bash
# ============================================================================
#  MailServer 一键编译脚本
#
#  用法（任选其一）：
#     方式1：bash build.sh          ← 推荐，最简单
#     方式2：chmod +x build.sh
#            ./build.sh
#
#  作用：把 4 个 .cpp 源码编译成可执行文件 ./mail_server
# ============================================================================

# 不管你在哪个目录敲命令，都先切到「本脚本所在目录」，
# 这样 ./mailbox 和 ./users.txt 的相对路径才不会出错
cd "$(dirname "$0")"

# g++ 编译命令逐段拆解：
#   g++                    调用 C++ 编译器
#   -std=c++17             使用 C++17 标准
#   -pthread               链接多线程库（代码里用了 std::thread）
#   -Wall -Wextra          打开编译器警告，帮你发现潜在问题
#   -o mail_server         指定输出文件名为 mail_server
#   main.cpp src/...       要编译的源文件
#   -I include             告诉编译器头文件在 include/ 目录
g++ -std=c++17 -pthread -Wall -Wextra \
    -o mail_server \
    main.cpp \
    src/Server.cpp src/SmtpServer.cpp src/Pop3Server.cpp \
    src/SmtpClient.cpp src/Pop3Client.cpp src/MailCrypto.cpp \
    src/HttpServer.cpp \
    -I include

# 检查 g++ 是否成功（$? 是上一条命令的退出码），失败就停下来，别往下跑了
if [ $? -ne 0 ]; then
    echo ""
    echo "❌ 编译出错！请根据上面的报错信息修改代码。"
    exit 1
fi

echo ""
echo "✅ 编译完成！现在运行：  ./mail_server"
echo "  浏览器打开 http://localhost:8080 可以测试 Web 邮箱页面"
