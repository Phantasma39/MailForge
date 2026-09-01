// ============================================================================
//  SmtpServer.cpp —— SMTP 服务器实现（协议层）
//
//  SMTP（简单邮件传输协议）是互联网上发邮件的标准协议。
//  它的本质是"基于 TCP 的文本对话"：
//    客户端 → 一行命令（HELO / MAIL FROM / RCPT TO / DATA / QUIT）
//    服务器 → 一行状态码（250 / 354 / 221 ...）
//
//  本文件实现的核心函数：
//    processCommand()  —— 命令状态机，整个服务器的"大脑"
//    handleClient()    —— 一个客户端连接的完整生命周期（含行缓冲）
//    saveMail()        —— 把拼好的邮件以 .eml 格式存盘
//
//  网络层（socket/bind/accept/多线程）由 Server 基类完成，
//  本文件只关心"收到一行文本后怎么应答"
// ============================================================================
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

// ============================================================================
//  构造函数
//  作用：
//    1. 把端口号交给 Server 基类（基类负责 socket/bind/listen/accept）
//    2. 创建邮件存储目录 ./mailbox（如果不存在）
// ============================================================================
SmtpServer::SmtpServer(int port) : Server(port) {
    // 创建邮件存储目录（仅 Unix/Linux）
    // mkdir 第二个参数 0755 是权限位：拥有者可读写执行(7)，
    // 同组用户可读执行(5)，其他人可读执行(5)
    // 注意：如果目录已存在，mkdir 会返回 -1，这里不检查返回值也没关系
    mkdir("./mailbox", 0755);
}

// ============================================================================
//  发送响应（封装 send）
//  作用：给响应字符串末尾补上 SMTP 协议要求的 \r\n，然后通过 socket 发出
//  为什么必须是 \r\n：
//    SMTP 是文本协议，RFC 规定所有命令/响应行必须以 CRLF（回车+换行）结尾，
//    如果只发 \n，很多严格的邮件客户端会解析失败
// ============================================================================
void SmtpServer::sendResponse(int fd, const std::string& response) {
    std::string msg = response + "\r\n";
    // send() 是系统调用，把数据写入 TCP 发送缓冲区
    // 返回值是实际发送的字节数；这里数据量小，简单起见不检查是否发完
    // （严格的做法是循环调用 send 直到全部发完，或处理半包情况）
    send(fd, msg.c_str(), msg.size(), 0);
}

// ============================================================================
//  处理单个命令 —— 整个 SMTP 服务器的"大脑"（核心状态机）
//
//  返回 true  = 继续会话（还可以继续收发命令）
//  返回 false = 结束会话（通常是收到 QUIT）
//
//  参数说明：
//    fd       : 客户端 socket，用于发送响应
//    line     : 客户端发来的一行内容（已去掉 \r\n）
//    mail     : 当前会话正在拼装的邮件（跨命令共享状态，引用传参）
//    dataMode : 是否处于 DATA 模式（引用传参，函数内可能修改它）
//
//  SMTP 协议状态机（简化）：
//    (连接) → 220 → [HELO/EHLO] → [MAIL FROM] → [RCPT TO] → [DATA] → 正文 → [QUIT]
//    每一步必须按顺序来，否则服务器会返回错误码（如 503、501）
// ============================================================================
bool SmtpServer::processCommand(int fd, const std::string& line, SmtpMail& mail, bool& dataMode) {
    // ============ DATA 模式：此时 line 不是命令，而是正文的一行 ============
    if (dataMode) {
        // 如果收到单独的点（"."），表示 DATA 正文结束（RFC 5321 的规定）
        if (line == ".") {
            dataMode = false;   // 退出 DATA 模式
            sendResponse(fd, "250 Message accepted for delivery");   // 告知客户端邮件已接收
            saveMail(mail);     // 把拼好的邮件落盘保存
            mail = SmtpMail();  // 清空邮件对象，准备接收下一封
        } else {
            // 普通正文行：追加到邮件正文，保留换行（\r\n）
            // 这就是为什么收到的邮件里每一行都有换行分隔
            mail.body += line + "\r\n";
        }
        return true;   // DATA 模式下不解析任何命令，始终继续会话
    }

    // ============ 正常命令解析 ============
    if (line.empty()) return true;   // 空行直接忽略，不响应

    // 把一行拆成"命令 + 参数"两部分
    // 例如："MAIL FROM:<alice@qq.com>" → cmd="MAIL", args="FROM:<alice@qq.com>"
    std::string cmd;
    std::string args;
    size_t space = line.find(' ');   // 找第一个空格的位置
    if (space != std::string::npos) {
        cmd = line.substr(0, space);          // 空格之前是命令名
        args = line.substr(space + 1);        // 空格之后是参数
        // 去除可能的多余空格（兼容 "MAIL FROM:   <a@b>" 这种写法）
        while (!args.empty() && args.front() == ' ') args.erase(0, 1);
    } else {
        cmd = line;   // 没有参数的命令，如 "QUIT"、"DATA"
    }

    // 转为大写以简化比较（SMTP 命令不区分大小写）
    // 所以客户端发 "helo" 或 "HELO" 都能正确识别
    for (auto& c : cmd) c = toupper(c);

    // ============ HELO / EHLO：打招呼，开始一段 SMTP 会话 ============
    // EHLO 是扩展版 HELO，现在客户端基本都用 EHLO
    if (cmd == "HELO" || cmd == "EHLO") {
        std::string domain = args.empty() ? "unknown" : args;   // 客户端自我介绍（域名）
        sendResponse(fd, "250 Hello " + domain + ", nice to meet you");
        return true;
    }

    // ============ MAIL FROM：声明发件人 ============
    // 标准格式：MAIL FROM:<sender@example.com>
    if (cmd == "MAIL") {
        if (args.find("FROM:") == 0) {   // 参数必须以 FROM: 开头
            std::string fromAddr = args.substr(5);   // 去掉 "FROM:" 前缀
            // 去除前导空格（兼容 "MAIL FROM: <a@b>"）
            while (!fromAddr.empty() && fromAddr.front() == ' ') fromAddr.erase(0, 1);
            // 去除尖括号 < >（SMTP 里地址常写成 <a@b> 形式）
            if (!fromAddr.empty() && fromAddr.front() == '<') fromAddr.erase(0, 1);
            if (!fromAddr.empty() && fromAddr.back() == '>') fromAddr.pop_back();
            if (fromAddr.empty()) {
                sendResponse(fd, "501 Syntax error in MAIL FROM");   // 地址为空 → 语法错误
            } else {
                mail.from = fromAddr;    // 存入会话状态（跨命令共享）
                sendResponse(fd, "250 OK");
            }
        } else {
            sendResponse(fd, "501 Syntax error in MAIL FROM");
        }
        return true;
    }

    // ============ RCPT TO：声明收件人 ============
    // 标准格式：RCPT TO:<receiver@example.com>
    // 可以多次调用 RCPT TO 来添加多个收件人（群发）
    if (cmd == "RCPT") {
        if (args.find("TO:") == 0) {
            std::string toAddr = args.substr(3);   // 去掉 "TO:" 前缀
            // 同样处理前导空格和尖括号
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

    // ============ DATA：开始发送邮件正文 ============
    // 规范顺序是：MAIL FROM → RCPT TO → DATA
    if (cmd == "DATA") {
        // 检查前置条件：如果没有先声明发件人/收件人就进入 DATA，返回 503
        if (mail.from.empty() || mail.to.empty()) {
            sendResponse(fd, "503 Bad sequence of commands (need MAIL and RCPT first)");
        } else {
            // 354 是"可以开始发送数据"的专用响应码，
            // 后面客户端会逐行发送正文，直到单独一行 "." 结束
            sendResponse(fd, "354 End data with <CR><LF>.<CR><LF>");
            dataMode = true;   // 进入数据模式：之后收到的一行行内容都算正文
        }
        return true;
    }

    // ============ QUIT：结束会话 ============
    if (cmd == "QUIT") {
        sendResponse(fd, "221 Bye");
        return false;   // 返回 false → handleClient 会关闭连接，结束会话
    }

    // ============ 未知命令 ============
    sendResponse(fd, "500 Unrecognized command");
    return true;
}

// ============================================================================
//  主循环：SMTP 对话（处理一个客户端连接的完整生命周期）
//
//  流程：
//    1. 先发 220 问候语（服务器就绪）
//    2. 循环接收客户端发来的数据，按行拆分成命令
//    3. 逐行交给 processCommand 处理
//    4. 直到 QUIT（processCommand 返回 false）或客户端断开，关闭连接
//
//  难点：TCP 是"流"而不是"消息"
//    recv() 一次收到的数据可能包含多条命令，也可能只包含半条命令：
//      - 例如一次 recv 可能收到 "EHLO x\r\nMAIL FROM:<a@b>\r\n" 两条命令
//      - 也可能 "MAIL FR" 只来了一半，要等下一次 recv 拼完整
//    所以不能"recv 一次就当一条命令"，必须用缓冲区积累 + 按 \n 拆分
//    这就是下面 buf 变量的意义：它是"跨 recv 的粘包/拆包缓冲区"
// ============================================================================
void SmtpServer::handleClient(int client_fd) {
    // 发送服务就绪消息（220 是 SMTP 服务器可以开始接收命令的问候码）
    sendResponse(client_fd, "220 MyMailServer ESMTP ready");

    SmtpMail mail;          // 本会话要拼装的邮件（初始为空，跨命令共享）
    bool dataMode = false;  // 初始不处于 DATA 模式

    // 行缓冲：TCP 是流式协议，一次 recv 可能包含多条命令
    // （例如 "EHLO x\r\nMAIL FROM:<a@b>\r\n"），也可能一条命令被拆成多次 recv。
    // 必须按 \n 拆分成行，逐行解析，否则会把多条命令拼成一条，导致响应错乱。
    std::string buf;   // buf 用来积累"还没拆完"的数据

    while (true) {
        char chunk[4096];   // 每次从内核读取的数据块
        // recv() 从 TCP 接收缓冲区读取数据：
        //   > 0 : 读到了 bytes 个字节
        //   = 0 : 对方已正常关闭连接（收到 FIN）
        //   < 0 : 出错
        ssize_t bytes = recv(client_fd, chunk, sizeof(chunk), 0);
        if (bytes <= 0) {
            // 客户端断开或出错
            std::cout << "[SMTP] 客户端断开" << std::endl;
            break;
        }

        buf.append(chunk, bytes);   // 把新数据追加到缓冲区尾部

        // 把缓冲区按行拆分，逐行处理
        // 每次找 \n，把它之前的内容当作一行；处理完后从缓冲区里删掉这一行
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);   // 取出这一行（不含 \n）
            buf.erase(0, pos + 1);                   // 从缓冲区里删掉这一行

            // 去除行尾的 \r（\r\n 换行）
            // Windows 风格的行尾是 \r\n，我们把 \r 去掉只留内容
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // 处理命令
            // processCommand 返回 false 表示会话结束（QUIT）
            bool cont = processCommand(client_fd, line, mail, dataMode);
            if (!cont) {
                close(client_fd);   // 关闭客户端 socket
                return;             // 结束本线程，退出会话
            }
        }
        // 注意：如果缓冲区里最后残留的是"半行"（还没有 \n），
        // 它会留在 buf 里等下一次 recv 拼完整，这就是行缓冲的意义
    }

    // 线程结束，client_fd 关闭
    // （Server 基类的线程函数里也会 close 一次，这里再 close 是双保险）
    close(client_fd);
}

// ============================================================================
//  保存邮件到文件
//  作用：把一封拼装好的邮件以 .eml 格式写到 ./mailbox 目录
//  为什么是 .eml 格式：.eml 是标准邮件文件格式，
//  可以用 Outlook、Foxmail 等客户端直接双击打开查看，方便调试和验证
// ============================================================================
void SmtpServer::saveMail(const SmtpMail& mail) {
    // 生成文件名：时间戳 + 随机数（以防并发冲突）
    // 因为服务器是多线程的，两个客户端可能同时发邮件，
    // 只用时间戳可能撞名，加上随机数（rand()）进一步降低冲突概率
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::string filename = "./mailbox/" + std::to_string(ts) + "_" + std::to_string(rand()) + ".eml";

    // 以"写模式"打开文件（ofstream 默认是覆盖写）
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "无法保存邮件到 " << filename << std::endl;
        return;
    }

    // 简单重建邮件头部（可以按 RFC 格式丰富）
    file << "From: " << mail.from << "\r\n";       // 发件人
    file << "To: " << mail.to << "\r\n";           // 收件人
    file << "Date: " << std::ctime(&ts);            // 发送时间（注意 ctime 自带换行）
    file << "Subject: " << "(no subject)\r\n";     // 简单的占位标题
    file << "\r\n";                                // 空行分隔头部和正文（邮件格式规定）
    file << mail.body;                              // 正文已经包含换行

    file.close();   // 关闭文件，数据落盘
    std::cout << "[SMTP] 邮件已保存: " << filename << std::endl;
}
