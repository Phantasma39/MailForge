// ============================================================================
//  client_test.cpp —— SMTP / POP3 客户端 的演示 + 自测程序
//
//  前提：MailServer 已经在运行（./mail_server，2525 + 1110）
//
//  它依次做：
//    1. SmtpClient  给 bob 发一封中文邮件（正文里带 . 开头的行，测试点填充）
//    2. Pop3Client  以 bob 登录，STAT/LIST 看邮箱
//    3. RETR 下载最新一封（就是我们刚发的），把原文打印出来
//    4. DELE 把它删掉并 QUIT，再重连验证删除生效（邮箱数量回落）
//    5. 故意用错误密码登录，演示失败时的报错信息
//
//  编译：bash build_client.sh   运行：./mail_client_test
// ============================================================================
#include "SmtpClient.h"
#include "Pop3Client.h"
#include <iostream>
#include <vector>

int main() {
    // ==================== 1. SMTP 客户端：发信 ====================
    std::cout << "========== 1. SMTP 客户端发信 ==========" << std::endl;
    SmtpClient smtp;   // 默认连 127.0.0.1:2525

    std::string subject = "来自 C++ SMTP 客户端的第一封信";
    std::string body =
        "你好，bob！\n"
        "这封邮件是用我们自己的 C++ SmtpClient 发出的。\n"
        ".这一行以点开头，用来测试 SMTP 点填充\n"
        "正文到这里结束，祝你课程设计顺利！\n";

    if (!smtp.sendMail("alice@example.com", "bob@example.com", subject, body)) {
        std::cerr << "[失败] SMTP 发信出错: " << smtp.getLastError() << std::endl;
        return 1;
    }
    std::cout << "[成功] 邮件已发给服务器（bob@example.com）" << std::endl;

    // ==================== 2. POP3 客户端：登录 + 查看邮箱 ====================
    std::cout << "\n========== 2. POP3 客户端登录收信 ==========" << std::endl;
    Pop3Client pop3;   // 默认连 127.0.0.1:1110

    if (!pop3.login("bob", "123456")) {
        std::cerr << "[失败] POP3 登录出错: " << pop3.getLastError() << std::endl;
        return 1;
    }
    std::cout << "[成功] bob 登录成功" << std::endl;

    int count = 0;
    long long totalBytes = 0;
    pop3.stat(count, totalBytes);
    std::cout << "[STAT] 收件箱共 " << count << " 封邮件，总 " << totalBytes << " 字节" << std::endl;

    std::vector<Pop3MailInfo> mails;
    if (!pop3.list(mails)) {
        std::cerr << "[失败] LIST 出错: " << pop3.getLastError() << std::endl;
        return 1;
    }
    std::cout << "[LIST] 邮件列表（编号 大小）:" << std::endl;
    for (const Pop3MailInfo& m : mails) {
        std::cout << "         " << m.number << "   " << m.size << " bytes" << std::endl;
    }

    if (mails.empty()) {
        std::cerr << "[失败] 收件箱是空的，没法演示 RETR" << std::endl;
        return 1;
    }

    // ==================== 3. RETR 下载最新一封 ====================
    std::cout << "\n========== 3. RETR 下载最新一封（编号 "
              << mails.back().number << "） ==========" << std::endl;
    int newest = mails.back().number;
    std::string rawMail;
    if (!pop3.retr(newest, rawMail)) {
        std::cerr << "[失败] RETR 出错: " << pop3.getLastError() << std::endl;
        return 1;
    }
    std::cout << "------ 邮件原文如下 ------" << std::endl;
    std::cout << rawMail << std::endl;
    std::cout << "--------------------------" << std::endl;

    // 简单校验：刚才发的主题/正文应该在邮件里
    bool subjectOk = rawMail.find("C++ SMTP") != std::string::npos;
    bool bodyOk    = rawMail.find("点填充") != std::string::npos;
    std::cout << "[检查] 主题包含'C++ SMTP'？ " << (subjectOk ? "是" : "否") << std::endl;
    std::cout << "[检查] 正文包含'点填充'？ "   << (bodyOk ? "是" : "否") << std::endl;
    std::cout << "[检查] 正文点开头行还原正确（.这一行…）？ "
              << (rawMail.find("\n.这一行以点开头") != std::string::npos ? "是" : "否")
              << std::endl;

    // ==================== 4. DELE 删除 + QUIT，再重连验证 ====================
    std::cout << "\n========== 4. 删除刚收到的邮件并验证 ==========" << std::endl;
    std::cout << "[DELE] 标记删除第 " << newest << " 封..." << std::endl;
    if (!pop3.dele(newest)) {
        std::cerr << "[失败] DELE 出错: " << pop3.getLastError() << std::endl;
        return 1;
    }
    std::cout << "[QUIT] 退出（此刻服务器才真正删掉邮件）..." << std::endl;
    pop3.quit();

    // 重连确认：数量应该 = 刚才的 count - 1
    Pop3Client check;
    if (!check.login("bob", "123456")) {
        std::cerr << "[失败] 重连登录出错: " << check.getLastError() << std::endl;
        return 1;
    }
    int afterCount = 0;
    long long afterBytes = 0;
    check.stat(afterCount, afterBytes);
    std::cout << "[验证] 删除前 " << count << " 封 → 删除后 " << afterCount << " 封" << std::endl;
    std::cout << (afterCount == count - 1
                      ? "[成功] DELE + QUIT 后邮件确实被删除"
                      : "[注意] 数量变化不符合预期，请检查")
              << std::endl;
    check.quit();

    // ==================== 5. 错误密码演示 ====================
    std::cout << "\n========== 5. 错误密码登录（应被拒绝） ==========" << std::endl;
    Pop3Client bad;
    if (bad.login("bob", "000000")) {
        std::cout << "[注意] 竟然登录成功了？密码校验可能有问题" << std::endl;
    } else {
        std::cout << "[成功] 错误密码被拒绝，报错信息: " << bad.getLastError() << std::endl;
    }

    std::cout << "\n演示结束。收件箱已恢复原样，可重复运行本程序。" << std::endl;
    return 0;
}
