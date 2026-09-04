// 用于实现 POP3 协议的代码部分，同样继承 Server 类
// 注释风格尽量和 SmtpServer.cpp 保持一致，方便对照着看

#include "Pop3Server.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <algorithm>

// 构造函数：先让基类 Server 把端口存好，再准备账号表和收件根目录
Pop3Server::Pop3Server(int port) : Server(port) {
    mkdir("./mailbox", 0755);   // 保证收件根目录存在（SmtpServer 那边也会建一次，幂等）
    loadAccounts();             // 读取 ./users.txt 账号表
}

// ==================== 底层收发工具（和 SmtpServer 完全一样） ====================

// 用于保证 send 全部发完，处理"只发了一部分"和"被信号中断"的情况
bool Pop3Server::sendAll(int fd, const char* data, size_t len) {
    size_t total_sent = 0;  // 已经发出去的字节数

    while (total_sent < len) {
        ssize_t sent = send(fd, data + total_sent, len - total_sent, 0);

        if (sent < 0) {
            if (errno == EINTR) continue;   // 被信号打断，重发即可
            perror("[POP3] send 错误");
            return false;
        }
        if (sent == 0) {
            std::cerr << "[POP3] send 返回 0，可能连接已关闭" << std::endl;
            return false;
        }
        total_sent += sent;
    }
    return true;
}

// 发一行响应：POP3 规定所有响应行必须以 \r\n 结尾，这里封装一层
void Pop3Server::sendResponse(int fd, const std::string& response) {
    std::string msg = response + "\r\n";
    if (!sendAll(fd, msg.c_str(), msg.size())) {
        std::cerr << "[POP3] 发送响应失败，即将断开连接: " << response << std::endl;
    }
}

// ==================== 账号管理 ====================

// 把 ./users.txt（格式：用户名:密码，一行一个，# 开头是注释）读进 accounts_
// 文件不存在或里面一个有效账号都没有时，用内置默认账号兜底，保证第一次能跑起来
void Pop3Server::loadAccounts() {
    std::lock_guard<std::mutex> lock(accountsMutex_);   // 重读账号表前先加锁（多线程）
    // 先塞内置默认账号（bob / alice，密码都是 123456，方便第一次运行直接测试）
    accounts_.clear();
    accounts_["bob"]   = "123456";
    accounts_["alice"] = "123456";

    std::ifstream file("./users.txt");
    if (!file.is_open()) {
        std::cerr << "[POP3] 没找到 ./users.txt，使用内置默认账号 bob/123456、alice/123456" << std::endl;
        return;
    }

    std::map<std::string, std::string> fileAccounts;   // 先从文件里读
    std::string line;
    int loaded = 0;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // 去掉 \r\n 的 \r
        // 去首尾空白
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;               // 空行跳过
        size_t e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);

        if (line.empty() || line[0] == '#') continue;       // 注释行跳过

        size_t colon = line.find(':');                      // 按第一个冒号拆成 用户名:密码
        if (colon == std::string::npos) {
            std::cerr << "[POP3] users.txt 中有非法行（缺少冒号），已跳过: " << line << std::endl;
            continue;
        }

        std::string user = line.substr(0, colon);
        std::string pass = line.substr(colon + 1);
        user = normalizeUser(user);                         // 用户名同样规范化
        if (user.empty()) {
            std::cerr << "[POP3] users.txt 中用户名无效，已跳过: " << line << std::endl;
            continue;
        }
        fileAccounts[user] = pass;
        ++loaded;
    }

    if (loaded > 0) {
        accounts_ = fileAccounts;   // 文件里有有效账号就用文件里的
        std::cout << "[POP3] 已从 ./users.txt 加载 " << loaded << " 个账号" << std::endl;
    } else {
        std::cerr << "[POP3] ./users.txt 中没有有效账号，继续使用内置默认账号" << std::endl;
    }
}

// ==================== 账号查询（支持"即时注册"） ====================

// 判断某用户名是否在账号表里。
// 如果内存表里没有：说明 users.txt 可能刚被 Web 注册接口 / 手工编辑改过，
// 于是重新读一次文件再判断 —— 这样新注册的账号不用重启服务器就能登录。
bool Pop3Server::isUserKnown(const std::string& userKey) {
    {
        std::lock_guard<std::mutex> lock(accountsMutex_);
        if (accounts_.count(userKey) > 0) return true;
    }
    loadAccounts();   // 内存表没有 → 重新加载文件（函数内部会加锁）
    std::lock_guard<std::mutex> lock(accountsMutex_);
    return accounts_.count(userKey) > 0;
}

// 校验 用户名+密码。同上：内存表里没有这个用户时先重读文件再判断。
bool Pop3Server::checkPassword(const std::string& userKey, const std::string& pass) {
    {
        std::lock_guard<std::mutex> lock(accountsMutex_);
        auto it = accounts_.find(userKey);
        if (it != accounts_.end()) return it->second == pass;
    }
    loadAccounts();
    std::lock_guard<std::mutex> lock(accountsMutex_);
    auto it = accounts_.find(userKey);
    return it != accounts_.end() && it->second == pass;
}

// 把登录名规范成"小写 + 只留 @ 前部分"：
//   "bob@example.com" → "bob"；"Bob" → "bob"；" alice " → "alice"
// 这样无论客户端填 bob 还是 Bob 还是 bob@example.com，都能对上同一个目录和账号
std::string Pop3Server::normalizeUser(const std::string& input) const {
    std::string u = input;

    // 去首尾空白
    size_t b = u.find_first_not_of(" \t");
    if (b == std::string::npos) return "";      // 全是空白，没有用户名
    size_t e = u.find_last_not_of(" \t");
    u = u.substr(b, e - b + 1);

    // 邮箱地址只取 @ 前面的"用户名"部分
    size_t at = u.find('@');
    if (at != std::string::npos) u = u.substr(0, at);

    // 统一转小写（文件夹 / 账号比较都不区分大小写，避免 Bob 收不到信的坑）
    for (auto& c : u) c = (char)tolower((unsigned char)c);

    return u;
}

// 收件目录：./mailbox/<用户名>，例如 bob → ./mailbox/bob
std::string Pop3Server::mailboxDir(const std::string& userKey) const {
    return "./mailbox/" + userKey;
}

// 读取某个用户收件目录下所有 .eml 文件，填进 mails 快照
void Pop3Server::loadUserMails(const std::string& userKey, std::vector<Pop3Mail>& mails) {
    mails.clear();
    std::string dir = mailboxDir(userKey);

    DIR* d = opendir(dir.c_str());
    if (!d) {
        // 目录不存在 = 这个用户还从来没收到过邮件，按"空邮箱"处理即可
        return;
    }

    // 第一遍：只收集 .eml 文件名
    std::vector<std::string> names;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".eml") == 0) {
            names.push_back(name);
        }
    }
    closedir(d);

    // 按文件名排序（文件名的前缀是时间戳，排序后就接近收件时间顺序），
    // 保证"第 1 封、第 2 封"每次会话都稳定，不会随机变化
    std::sort(names.begin(), names.end());

    // 第二遍：逐个取文件大小，做成邮件快照
    for (const std::string& name : names) {
        std::string path = dir + "/" + name;

        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;   // 文件刚被删掉等情况，跳过
        if (!S_ISREG(st.st_mode)) continue;           // 只收普通文件（跳过子目录等）

        Pop3Mail m;
        m.path   = path;
        m.size   = (long long)st.st_size;
        m.deleted = false;
        mails.push_back(m);
    }
}

// ==================== 工具函数 ====================

// 把一行拆成"命令 + 参数"：找第一个空格，前面是命令，后面是参数
// 命令统一转大写（POP3 命令不区分大小写）；没有参数的命令 args 为空
void Pop3Server::splitCommand(const std::string& line, std::string& cmd, std::string& args) {
    size_t space = line.find(' ');
    if (space != std::string::npos) {
        cmd = line.substr(0, space);
        args = line.substr(space + 1);
        while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) {
            args.erase(0, 1);   // 参数前面的多余空格全删掉
        }
    } else {
        cmd = line;
    }
    for (auto& c : cmd) c = (char)toupper((unsigned char)c);
}

// 把参数字符串解析成消息编号。POP3 里编号只能是正整数，所以：
//   空串 / 含非数字 / 超过 int 范围 → 返回 0（表示"解析失败"）
int Pop3Server::parseMsgNum(const std::string& args) {
    if (args.empty()) return 0;
    long long num = 0;
    for (char c : args) {
        if (!isdigit((unsigned char)c)) return 0;
        num = num * 10 + (c - '0');
        if (num > 100000000) return 0;   // 防溢出
    }
    return (int)num;
}

// 判断消息编号在当前会话里是否可操作：
//   1 ≤ num ≤ 邮件总数，且这封邮件没有被 DELE 标记删除
bool Pop3Server::isValidMsg(int num, const Pop3State& st) const {
    if (num < 1 || num > (int)st.mails.size()) return false;
    return !st.mails[num - 1].deleted;
}

// 统计还没被标记删除的邮件数量与总字节数（STAT / LIST 的总览行都会用到）
void Pop3Server::statMailbox(const Pop3State& st, int& count, long long& totalBytes) const {
    count = 0;
    totalBytes = 0;
    for (const Pop3Mail& m : st.mails) {
        if (!m.deleted) {
            ++count;
            totalBytes += m.size;
        }
    }
}

// 把一封邮件的文件内容按 POP3 的"点填充（dot-stuffing）"规则发给客户端：
//   1) 邮件里凡是"以 . 开头"的行，发送时要在前面再加一个点（客户端收到后会还原）
//      因为单独一行的 "." 是 POP3 用来表示"多行内容结束"的标记，
//      如果正文里真有以点开头的行，不处理的话客户端会提前以为内容结束了
//   2) 所有内容行发完之后，再发单独一行的 "." 表示结束
void Pop3Server::sendMailContent(int fd, const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        sendResponse(fd, "-ERR cannot open message file");
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        // getline 按 \n 切行，行尾可能残留 \r（文件里是 \r\n 换行），去掉它，
        // 下面统一补规范的 \r\n（sendResponse 会自动补）
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // 点填充：以 . 开头的行发成 .. 开头的行
        if (!line.empty() && line[0] == '.') {
            line = "." + line;
        }

        sendResponse(fd, line);   // 发这一行（自动补 \r\n），空行也能正确处理
    }

    sendResponse(fd, ".");        // 多行内容的结束标记
}

// ==================== 核心：按 POP3 协议处理命令（状态机） ====================

bool Pop3Server::processCommand(int fd, const std::string& line, Pop3State& st) {

    // 空行直接忽略（和 SmtpServer 的处理习惯一致）
    if (line.empty()) return true;

    // 拆成"命令 + 参数"
    std::string cmd;
    std::string args;
    splitCommand(line, cmd, args);

    // ---------- QUIT：任何状态下都允许退出 ----------
    if (cmd == "QUIT") {
        if (st.authed) {
            // 进入 UPDATE 阶段：把本会话里 DELE 标记要删的邮件真正从磁盘删掉
            // （POP3 规定 DELE 只是"打标记"，只有 QUIT 时才会真正删除，
            //   所以客户端就算中途断线，邮件也不会丢）
            int removed = 0;
            for (const Pop3Mail& m : st.mails) {
                if (m.deleted) {
                    if (unlink(m.path.c_str()) == 0) {
                        ++removed;
                        std::cout << "[POP3] 已删除邮件: " << m.path << std::endl;
                    } else {
                        perror("[POP3] 删除邮件失败");
                    }
                }
            }
            std::cout << "[POP3] 用户 " << st.username << " 退出，本次共删除 "
                      << removed << " 封邮件" << std::endl;
        }
        sendResponse(fd, "+OK MailForge POP3 server signing off");
        return false;   // 返回 false → handleClient 关闭连接，结束会话
    }

    // ============ AUTHORIZATION 阶段（还没登录，只能 USER / PASS） ============
    if (!st.authed) {
        if (cmd == "USER") {
            if (args.empty()) {
                sendResponse(fd, "-ERR USER requires a mailbox name");
                return true;
            }
            // 存下用户名原文（打日志用），并规范成 userKey（小写、去 @ 域名）
            st.username = args;
            st.userKey  = normalizeUser(args);
            if (st.userKey.empty()) {
                sendResponse(fd, "-ERR invalid mailbox name");
                st.username.clear();
            } else if (isUserKnown(st.userKey)) {
                sendResponse(fd, "+OK User " + st.userKey + " known");
            } else {
                // 用户不在账号表里。真实服务器通常也回 +OK（防止探测账号是否存在），
                // 这里是教学代码，直接告诉客户端比较直观
                sendResponse(fd, "-ERR User " + st.userKey + " unknown");
            }
            return true;
        }

        if (cmd == "PASS") {
            if (st.userKey.empty()) {
                sendResponse(fd, "-ERR USER command needed first");
                return true;
            }
            // 去账号表里查：用户名匹配 && 密码一致才算通过。
            // 若账号表里还没有这个用户（比如刚在网页上注册），checkPassword 会
            // 自动重读一次 users.txt，所以新账号无需重启服务器即可登录。
            if (checkPassword(st.userKey, args)) {
                st.authed = true;

                // 顺手建一下收件目录（即使还没有邮件，也让用户的邮箱目录先存在）
                mkdir(mailboxDir(st.userKey).c_str(), 0755);

                // 登录成功 = 进入 TRANSACTION 阶段，先把邮箱快照读进来
                loadUserMails(st.userKey, st.mails);

                int count = 0;
                long long bytes = 0;
                statMailbox(st, count, bytes);
                std::cout << "[POP3] 用户 " << st.username << " 认证成功，邮箱里 "
                          << count << " 封邮件" << std::endl;

                sendResponse(fd, "+OK mailbox ready, " + std::to_string(count)
                                  + " message(s), " + std::to_string(bytes) + " bytes");
            } else {
                // 密码不对：清掉用户名，让客户端重新走一遍 USER / PASS
                st.username.clear();
                st.userKey.clear();
                sendResponse(fd, "-ERR invalid password");
            }
            return true;
        }

        // 认证阶段收到别的命令（STAT / LIST / RETR...）都是不合法的
        sendResponse(fd, "-ERR not authenticated, send USER and PASS first");
        return true;
    }

    // ============ TRANSACTION 阶段（已登录，正式收发命令） ============

    // STAT：不需要参数，返回"+OK 邮件数 总字节数"（只统计没被标记删除的）
    if (cmd == "STAT") {
        int count = 0;
        long long bytes = 0;
        statMailbox(st, count, bytes);
        sendResponse(fd, "+OK " + std::to_string(count) + " " + std::to_string(bytes));
        return true;
    }

    // LIST：无参数 → 多行列出所有未删除邮件；有参数 → 只回那一封
    if (cmd == "LIST") {
        if (args.empty()) {
            int count = 0;
            long long bytes = 0;
            statMailbox(st, count, bytes);

            // 多行响应格式：先回一行总览，然后每封邮件一行"编号 字节数"，最后一行 "."
            sendResponse(fd, "+OK " + std::to_string(count) + " message(s), "
                              + std::to_string(bytes) + " bytes");
            for (size_t i = 0; i < st.mails.size(); ++i) {
                if (!st.mails[i].deleted) {
                    sendResponse(fd, std::to_string(i + 1) + " "
                                      + std::to_string(st.mails[i].size));
                }
            }
            sendResponse(fd, ".");
        } else {
            int num = parseMsgNum(args);
            if (isValidMsg(num, st)) {
                sendResponse(fd, "+OK " + std::to_string(num) + " "
                                  + std::to_string(st.mails[num - 1].size));
            } else {
                sendResponse(fd, "-ERR no such message");
            }
        }
        return true;
    }

    // RETR：下载某封邮件的完整内容（多行响应，用点填充规则发送）
    if (cmd == "RETR") {
        int num = parseMsgNum(args);
        if (!isValidMsg(num, st)) {
            sendResponse(fd, "-ERR no such message");
            return true;
        }
        sendResponse(fd, "+OK " + std::to_string(st.mails[num - 1].size) + " octets");
        sendMailContent(fd, st.mails[num - 1].path);
        return true;
    }

    // DELE：把某封邮件"标记"为删除（只是打标记，QUIT 时才真正删文件）
    if (cmd == "DELE") {
        int num = parseMsgNum(args);
        if (!isValidMsg(num, st)) {
            sendResponse(fd, "-ERR no such message (or already deleted)");
            return true;
        }
        st.mails[num - 1].deleted = true;   // 打上删除标记
        std::cout << "[POP3] 用户 " << st.username << " 标记删除第 " << num
                  << " 封邮件" << std::endl;
        sendResponse(fd, "+OK message " + std::to_string(num) + " deleted");
        return true;
    }

    // RSET：撤销本会话所有 DELE 标记（POP3 提供后悔药，删错了可以反悔）
    if (cmd == "RSET") {
        for (Pop3Mail& m : st.mails) m.deleted = false;
        sendResponse(fd, "+OK all delete marks cleared");
        return true;
    }

    // NOOP：什么都不做，只用来"保活"（防止连接被网络设备超时掐断）
    if (cmd == "NOOP") {
        sendResponse(fd, "+OK");
        return true;
    }

    // 已登录状态下再来 USER / PASS 是没有意义的
    if (cmd == "USER" || cmd == "PASS") {
        sendResponse(fd, "-ERR already authenticated");
        return true;
    }

    // 其他一律当作未知命令（POP3 没有像 SMTP 的 500 那样的码，统一 -ERR）
    sendResponse(fd, "-ERR unknown command");
    return true;
}

// ==================== 主循环：一个 POP3 会话 ====================

void Pop3Server::handleClient(int client_fd) {
    // POP3 服务器连接建立后先主动问候（+OK 是 POP3 的"成功"响应码）
    sendResponse(client_fd, "+OK MailForge POP3 server ready");

    Pop3State st;   // 本会话的状态（登录名 / 认证标记 / 邮箱快照），初始都为空

    // 行缓冲：和 SMTP 完全一样的思路。TCP 是流式协议，一次 recv 可能包含多条命令，
    // 也可能一条命令被拆成多次 recv 才到齐，所以必须按 \n 拆成一行行再处理
    std::string buf;

    while (true) {
        char chunk[4096];
        ssize_t bytes = recv(client_fd, chunk, sizeof(chunk), 0);
        if (bytes <= 0) {
            // 客户端断开或出错（注意：断线不算 QUIT，DELE 标记的邮件不会真删，
            // 这是 POP3 协议的设计，防止邮件在下载成功之前被误删）
            std::cout << "[POP3] 客户端断开" << std::endl;
            break;
        }

        buf.append(chunk, bytes);

        // 把缓冲区按 \n 拆成一行行处理，处理完的从缓冲区删掉
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);   // 取出这一行（不含 \n）
            buf.erase(0, pos + 1);                   // 从缓冲区里删掉这一行

            // 去掉行尾的 \r（\r\n 换行），Windows 客户端发来的也是这种
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // 处理命令；返回 false 表示会话结束（QUIT）
            bool cont = processCommand(client_fd, line, st);
            if (!cont) {
                close(client_fd);
                return;   // 结束本线程，退出会话
            }
        }
        // 缓冲区里残留的"半行"留在 buf 里，等下一次 recv 拼完整
    }

    close(client_fd);   // 双保险关闭（基类线程函数里也会 close 一次）
}

