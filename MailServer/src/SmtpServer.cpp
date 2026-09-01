#include "SmtpServer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <ctime>
#include <chrono>
#include <sys/socket.h>   // 提供 recv, send


// 构造函数
SmtpServer::SmtpServer(int port) : Server(port) {
    // 创建邮件存储目录（如果不存在）
    mkdir("./mailbox", 0755); // 仅 Unix/Linux
}

// 发送响应（封装 send）
void SmtpServer::sendResponse(int fd, const std::string& response) {
    std::string msg = response + "\r\n";
    send(fd, msg.c_str(), msg.size(), 0);
}

// 处理单个命令
bool SmtpServer::processCommand(int fd, const std::string& line, SmtpMail& mail, bool& dataMode) {
    // 如果处于 DATA 模式，则累积正文行
    if (dataMode) {
        // 如果收到单独的点，表示 DATA 结束
        if (line == ".") {
            dataMode = false;
            sendResponse(fd, "250 Message accepted for delivery");
            // 保存邮件
            saveMail(mail);
            // 清空邮件对象，准备下一封
            mail = SmtpMail();
        } else {
            // 将行追加到正文，保留换行
            mail.body += line + "\r\n";
        }
        return true;
    }

    // 正常命令解析
    if (line.empty()) return true;

    // 提取命令（不区分大小写）
    std::string cmd;
    std::string args;
    size_t space = line.find(' ');
    if (space != std::string::npos) {
        cmd = line.substr(0, space);
        args = line.substr(space + 1);
        // 去除可能的多余空格
        while (!args.empty() && args.front() == ' ') args.erase(0, 1);
    } else {
        cmd = line;
    }

    // 转为大写以简化比较
    for (auto& c : cmd) c = toupper(c);

    // ---------- HELO / EHLO ----------
    if (cmd == "HELO" || cmd == "EHLO") {
        std::string domain = args.empty() ? "unknown" : args;
        sendResponse(fd, "250 Hello " + domain + ", nice to meet you");
        return true;
    }

    // ---------- MAIL FROM ----------
    if (cmd == "MAIL") {
        // 简单解析格式：MAIL FROM:<xxx@xxx>
        if (args.find("FROM:") == 0) {
            std::string fromAddr = args.substr(5);
            // 去除前导空格（兼容 "MAIL FROM: <a@b>"）
            while (!fromAddr.empty() && fromAddr.front() == ' ') fromAddr.erase(0, 1);
            // 去除尖括号
            if (!fromAddr.empty() && fromAddr.front() == '<') fromAddr.erase(0, 1);
            if (!fromAddr.empty() && fromAddr.back() == '>') fromAddr.pop_back();
            if (fromAddr.empty()) {
                sendResponse(fd, "501 Syntax error in MAIL FROM");
            } else {
                mail.from = fromAddr;
                sendResponse(fd, "250 OK");
            }
        } else {
            sendResponse(fd, "501 Syntax error in MAIL FROM");
        }
        return true;
    }

    // ---------- RCPT TO ----------
    if (cmd == "RCPT") {
        if (args.find("TO:") == 0) {
            std::string toAddr = args.substr(3);
            // 去除前导空格（兼容 "RCPT TO: <a@b>"）
            while (!toAddr.empty() && toAddr.front() == ' ') toAddr.erase(0, 1);
            if (!toAddr.empty() && toAddr.front() == '<') toAddr.erase(0, 1);
            if (!toAddr.empty() && toAddr.back() == '>') toAddr.pop_back();
            if (toAddr.empty()) {
                sendResponse(fd, "501 Syntax error in RCPT TO");
            } else {
                mail.to = toAddr;
                sendResponse(fd, "250 OK");
            }
        } else {
            sendResponse(fd, "501 Syntax error in RCPT TO");
        }
        return true;
    }

    // ---------- DATA ----------
    if (cmd == "DATA") {
        if (mail.from.empty() || mail.to.empty()) {
            sendResponse(fd, "503 Bad sequence of commands (need MAIL and RCPT first)");
        } else {
            sendResponse(fd, "354 End data with <CR><LF>.<CR><LF>");
            dataMode = true; // 进入数据模式
        }
        return true;
    }

    // ---------- QUIT ----------
    if (cmd == "QUIT") {
        sendResponse(fd, "221 Bye");
        return false; // 断开连接
    }

    // ---------- 未知命令 ----------
    sendResponse(fd, "500 Unrecognized command");
    return true;
}

// 主循环：SMTP 对话
void SmtpServer::handleClient(int client_fd) {
    // 发送服务就绪消息
    sendResponse(client_fd, "220 MyMailServer ESMTP ready");

    SmtpMail mail;
    bool dataMode = false;

    // 行缓冲：TCP 是流式协议，一次 recv 可能包含多条命令
    // （例如 "EHLO x\r\nMAIL FROM:<a@b>\r\n"），也可能一条命令被拆成多次 recv。
    // 必须按 \n 拆分成行，逐行解析，否则会把多条命令拼成一条，导致响应错乱。
    std::string buf;

    while (true) {
        char chunk[4096];
        ssize_t bytes = recv(client_fd, chunk, sizeof(chunk), 0);
        if (bytes <= 0) {
            // 客户端断开或出错
            std::cout << "[SMTP] 客户端断开" << std::endl;
            break;
        }

        buf.append(chunk, bytes);

        // 把缓冲区按行拆分，逐行处理
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            // 去除行尾的 \r（\r\n 换行）
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // 处理命令
            bool cont = processCommand(client_fd, line, mail, dataMode);
            if (!cont) {
                close(client_fd);
                return; // QUIT 或出错
            }
        }
    }

    // 线程结束，client_fd 关闭
    close(client_fd);
}

// 保存邮件到文件
void SmtpServer::saveMail(const SmtpMail& mail) {
    // 生成文件名：时间戳 + 随机数（以防并发冲突）
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::string filename = "./mailbox/" + std::to_string(ts) + "_" + std::to_string(rand()) + ".eml";

    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "无法保存邮件到 " << filename << std::endl;
        return;
    }

    // 简单重建邮件头部（可以按 RFC 格式丰富）
    file << "From: " << mail.from << "\r\n";
    file << "To: " << mail.to << "\r\n";
    file << "Date: " << std::ctime(&ts); // 注意 ctime 自带换行
    file << "Subject: " << "(no subject)\r\n"; // 简单的占位
    file << "\r\n"; // 空行分隔头部和正文
    file << mail.body; // 正文已经包含换行

    file.close();
    std::cout << "[SMTP] 邮件已保存: " << filename << std::endl;
}