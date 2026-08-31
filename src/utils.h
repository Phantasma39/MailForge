#ifndef UTILS_H
#define UTILS_H

#include <string>

// 服务器监听端口（用 8888，避开系统保留端口）
#define SERVER_PORT 8888
// 接收缓冲区大小
#define BUFFER_SIZE 4096

// 工具函数：去除字符串末尾的回车换行
std::string trim(const std::string& str);

#endif