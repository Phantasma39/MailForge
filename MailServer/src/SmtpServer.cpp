//用于实现SMTP协议的代码部分，需要继承Server类

#include "SmtpServer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/stat.h>
#include <ctime>
#include <chrono>
#include <sys/socket.h> 

SmtpServer::SmtpServer(int port):Server(port){      //初始化SmptServer类，同时创建文件夹来储存邮件，0755是权限，我也不是很理解
    mkdir("./mailbox",0755);
}


//用于保证send全部发完，处理中断问题
bool SmtpServer::sendAll(int fd, const char* data, size_t len) {
    size_t total_sent = 0;  // 已经发出去多少个字节
    
    while (total_sent < len) {
        // 从 data + total_sent 地址开始发送剩余内容
        ssize_t sent = send(fd, data + total_sent, len - total_sent, 0);
        
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("send 错误");
            return false;
        }
        if (sent == 0) {
            std::cerr << "send 返回 0，可能连接已关闭" << std::endl;
            return false;
        }
        total_sent += sent;  // 累加已发送的字节数
    }
    return true;  // 全部发送完毕
}

void SmtpServer::sendResponse(int fd, const std::string&response){
    std::string msg=response+"\r\n";
  // 调用sendAll函数，确保完全发送完毕
    if (!sendAll(fd, msg.c_str(), msg.size())) {
        std::cerr << "[SMTP] 发送响应失败，即将断开连接: " << response << std::endl;
    }
}

//按照SMTP协议处理命令
bool SmtpServer::processCommand(int fd,const std::string& line,SmtpMail& mail,bool& dataMode){

    if(dataMode){
        if(line =="."){     //smtp协议，在DATA模式下，如果出现一个"."，表示结束
            dataMode = false;   //  退出DATA模式

            parseMailData(mail);

            sendResponse(fd,"250 Message accepted for delivery");       //告知客户端接受到DATA
            saveMail(mail);
            mail=SmtpMail();//清空邮件对象
        }else{      //没有结束，就继续加上"\r\n""
            // 注意：还要还原"点填充"。SMTP 规定邮件内容里任何以 "." 开头的行，
            // 客户端都必须写成 ".."，否则会被误认为是结束标志 "."，
            // 所以服务器要把多余的那个点还原回去，例如 "..abc" → ".abc"
            std::string dataLine = line;
            if (dataLine.size() > 1 && dataLine[0] == '.' && dataLine[1] == '.') {
                dataLine = dataLine.substr(1);
            }
            mail.body += dataLine + "\r\n";
        }
        return true;
    }

    // 不在 DATA 模式：line 是一条真正的 SMTP 命令，继续往下解析
    //正常命令
    if(line.empty()) return true;//忽略空行

    //把一行拆成“命令+参数”模式

    std::string cmd;    //命令
    std::string args;    //参数
    size_t space =line.find(' ');     //找到第一个空格的位置
    if (space !=std::string::npos){     //表示找到了第一个空格的位置
        cmd = line.substr(0,space);
        args=line.substr(space+1);
        while (!args.empty()&&args.front()==' ')    args.erase(0,1);    //删掉所有的空格
    }else{
        cmd=line;   //没有参数的单独命令
    }

    for(auto& c:cmd) c=toupper(c);      //全部转化成大写，因为SMTP协议不分大小写

    //HELO或者EHLO命令
    if(cmd=="HELO"||cmd=="EHLO"){ 

        std::string domain ;
        if(args.empty())    domain= "unknown" ;
        else domain = args;
    
        sendResponse(fd,"250 Hello "+domain+",nice to meet you");
        return true;
    }

    //MAIL FROM:<a@example.com>标准情况
    //MAIL命令，后面跟FROM:<a@example.com>
    if(cmd == "MAIL"){
        if (args.find("FROM:") == 0){   //必须要以FROM:开头
            std::string fromAddr = args.substr(5);  //定位到FROM:后面
            while (!fromAddr.empty() && fromAddr.front()==' ')    fromAddr.erase(0,1);    //删掉前面的空格，跟之前一样
            //删除掉<>，因为SMTP格式都是这样的
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

    //同理写RCPT TO：<b@example.com>
    if(cmd == "RCPT"){
        if (args.find("TO:") == 0){   //必须要以TO:开头
            std::string toAddr = args.substr(3);  //定位到TO:后面
            while (!toAddr.empty() && toAddr.front()==' ')    toAddr.erase(0,1);    //删掉前面的空格，跟之前一样
            //删除掉<>，因为SMTP格式都是这样的
            if (!toAddr.empty() && toAddr.front() == '<') toAddr.erase(0, 1);

            if (!toAddr.empty() && toAddr.back() == '>') toAddr.pop_back();

            if (toAddr.empty()) {
                sendResponse(fd, "501 Syntax error in RCPT TO");   //语法错误
            } else {
                mail.to = toAddr;    // 存入会话状态（跨命令共享）
                sendResponse(fd, "250 OK");
            } 
        } else {
            sendResponse(fd, "501 Syntax error in RCPT TO");
        }
        return true;
    }

    //DATA命令，表示开始发送正文内容
    if(cmd =="DATA"){
        if(mail.from.empty()||mail.to.empty()){     //看看有没有来去的地址
           sendResponse(fd, "503 Bad sequence of commands (need MAIL and RCPT first)"); 
        } else {
            sendResponse(fd,"354 End data with <CR><LF>.<CR><LF>");
            dataMode = true;    //可以写邮件内容了
        }
        return true;
    }

    if (cmd == "QUIT") {
        sendResponse(fd, "221 Bye");
        return false;   // 返回 false → handleClient 会关闭连接，结束会话
    }

        // ============ 未知命令 ============
    sendResponse(fd, "500 Unrecognized command");
    
    return true;
}

//处理DATA收到的邮件原文，里面会有From：,To：,subject，正文

void SmtpServer::parseMailData(SmtpMail& mail){

    //除去开头的空行和无意义内容

    size_t pos=0;      //指针位置
    while(pos<mail.body.size()){
        size_t eol=mail.body.find('\n',pos);    //从pos开始寻找第一个换行符
        if (eol == std::string::npos) { pos = mail.body.size(); break; }                //找不到了，退出循环
        size_t len=eol-pos;     //这一条的长度
        bool blank=true;
        for(size_t i=0;i<len;++i){      //判断这一行是不是全为空
            char c=mail.body[pos+i];
            if(c!='\r' && c!='\n' && c!=' ' && c!='\t'){blank=false;break;}
        }
        if (!blank) break;      //遇到第一个非空行从这里开始
        pos=eol+1;      //否则跳过这个空行
    }

    std::string content=mail.body.substr(pos);      //去掉开头空行的完整内容
    //将内容里面的头部，from，to，suject拆出来
    size_t sep = content.find("\r\n\r\n");      //标准协议需要在头部和正文之间加两个换行符
    if (sep != std::string::npos) {
        mail.headers = content.substr(0, sep);     // 空行之前是头部区
        mail.text    = content.substr(sep + 4);    // 空行之后是正文
    }

    mail.headerFrom = getHeaderValue(mail.headers, "From");
    mail.headerTo   = getHeaderValue(mail.headers, "To");
    mail.subject    = getHeaderValue(mail.headers, "Subject");
    mail.headerDate = getHeaderValue(mail.headers, "Date");
}

//处理头部文件

std::string SmtpServer::getHeaderValue(const std::string& headers, const std::string& name) {
    std::string value;
    bool found = false;

    std::istringstream iss(headers);   // 将它转换成一个逐行读的头部原文
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // 去掉行尾 \r
        if (line.empty()) continue;

        // 续行（以空格或 Tab 开头）：如果已经找到目标字段，把续行拼到值后面
        if (line[0] == ' ' || line[0] == '\t') {
            if (found) {
                value += " " + line.substr(1);   // 去掉续行前的一个空白再拼接
            }
            continue;
        }

        // 本行是一个新字段：若目标字段上一行已找齐（值可能跨了多行），直接返回
        if (found) return value;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;   // 不是"字段名: 值"，跳过

        std::string field = line.substr(0, colon);   // 冒号左边是字段名

        // 大小写不敏感地比较字段名
        bool match = (field.size() == name.size());
        if (match) {
            for (size_t i = 0; i < name.size(); ++i) {
                if (toupper(field[i]) != toupper(name[i])) { match = false; break; }
            }
        }
        if (!match) continue;

        // 命中目标字段：取冒号后面的内容，去掉前导空格 / Tab
        value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(0, 1);
        }
        found = true;
        // 先不返回：后面若还有续行，要继续拼到 value 上
    }
    return found ? value : "";
}

//主循环smtp对话

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

void SmtpServer::saveMail(const SmtpMail& mail) {
    // 生成文件名：时间戳 + 随机数（以防并发冲突）
    // 因为服务器是多线程的，两个客户端可能同时发邮件，
    // 只用时间戳可能撞名，加上随机数（rand()）进一步降低冲突概率
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // ===================== 按收件人分目录投递（配合 POP3 多用户收信） =====================
    // 邮件不再平铺在 ./mailbox/ 根目录，而是投到 ./mailbox/<收件人@前面的部分>/
    // 例如 RCPT TO 是 bob@example.com → 存到 ./mailbox/bob/xxx.eml。
    // 这样 POP3 服务器那边，用户 bob 登录后读 ./mailbox/bob/ 就是自己的邮件，
    // 实现了 README 里要求的"每个用户独立邮箱目录 / 多邮箱账户隔离"。
    std::string user = mail.to;                        // 信封收件人（RCPT TO 里的地址）
    size_t atPos = user.find('@');                     // 取 @ 前面的"用户名"部分
    if (atPos != std::string::npos) user = user.substr(0, atPos);

    // 去掉可能残留的首尾空白，并统一转小写（和 Pop3Server 里的 normalizeUser 保持一致）
    while (!user.empty() && (user.front() == ' ' || user.front() == '\t')) user.erase(0, 1);
    while (!user.empty() && (user.back() == ' ' || user.back() == '\t')) user.pop_back();
    for (auto& c : user) c = (char)tolower((unsigned char)c);

    // 拼接保存目录：./mailbox/用户名 或（解析失败时兜底）./mailbox 根目录
    std::string dir = "./mailbox";
    if (!user.empty()) {
        dir += "/" + user;
        mkdir(dir.c_str(), 0755);   // 该用户的收件目录不存在就创建（已存在也无妨）
    } else {
        std::cerr << "[SMTP] 收件人地址无法解析出用户名（" << mail.to
                  << "），邮件兜底保存到 ./mailbox/" << std::endl;
    }

    std::string filename = dir + "/" + std::to_string(ts) + "_" + std::to_string(rand()) + ".eml";

    // 以"写模式"打开文件（ofstream 默认是覆盖写）
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "无法保存邮件到 " << filename << std::endl;
        return;
    }

    // ===================== 组装标准邮件（RFC 5322：头部区 + 空行 + 正文） =====================
    // 头部区优先使用客户端在 DATA 阶段发来的原文（parseMailData() 拆出的 mail.headers），
    // 里面已经包含客户端真正写的 From / To / Subject / Cc / Date 等头——原样保留，不再忽略。
    // 如果客户端没写某个标准头（有些测试客户端只在 MAIL FROM / RCPT TO 里声明地址），
    // 服务器再按信封信息（mail.from / mail.to）和当前时间补全，保证 .eml 能正常显示。
    std::string header;   // 最终写进 .eml 的头部区

    // 1) 先补缺失的标准头（按 Date → From → To → Subject 的常见顺序）
    if (mail.headerDate.empty()) {
        std::string dateStr = std::ctime(&ts);      // 例："Wed Sep  2 10:00:52 2026\n"
        if (!dateStr.empty() && dateStr.back() == '\n') dateStr.pop_back();  // 去掉 ctime 自带的换行
        header += "Date: " + dateStr + "\r\n";      // 统一用 CRLF
    }
    if (mail.headerFrom.empty()) {
        header += "From: " + mail.from + "\r\n";    // 用 MAIL FROM 信封地址兜底
    }
    if (mail.headerTo.empty()) {
        header += "To: " + mail.to + "\r\n";        // 用 RCPT TO 信封地址兜底
    }
    if (mail.subject.empty()) {
        header += "Subject: (no subject)\r\n";      // 客户端没写主题时的占位
    }

    // 2) 再追加客户端在 DATA 里发的头部原文
    //    （若客户端已写 From/To/Subject，上面的补全就不会执行，保证以客户端的为准）
    header += mail.headers;

    // 3) 确保头部区最后一行也以 \r\n 结尾
    //    因为 header 里拼接的 mail.headers 可能不带行尾换行，
    //    不补上的话，下面的空行分隔符会把"最后一行头"和空行混在一起，
    //    导致邮件客户端认为头部没结束、正文被当成头部
    if (!header.empty() &&
        !(header.size() >= 2 && header.compare(header.size() - 2, 2, "\r\n") == 0)) {
        header += "\r\n";
    }

    // 4) 输出：头部区 + 空行 + 正文
    file << header;          // 头部区（每行末尾已带 \r\n）
    file << "\r\n";          // 头部与正文之间的分隔空行（邮件格式规定）
    file << mail.text;       // 正文（parseMailData() 从 DATA 原文中拆分出来的部分）

    file.close();   // 关闭文件，数据落盘
    std::cout << "[SMTP] 邮件已保存: " << filename;
    if (!mail.subject.empty()) {
        std::cout << " | 主题: " << mail.subject;   // 顺手打一下提取到的主题，便于调试
    }
    std::cout << std::endl;
}

