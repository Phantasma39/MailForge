// Server.cpp用于实现服务器的基类
//主要用于后续SMTP和POP3的复用
//有好多新函数，好神奇

#include "Server_2.h"
#include <iostream>
#include <cstring>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

//构造函数
Server::Server(int port)    //port:端口，可以随意编写
    :server_fd(-1),port(port),is_running(false){}   //server_fd：监听套接字（socket 文件描述符）

//析构函数
Server::~Server()
{
    stop();
}    

//服务器启动，返回一个是否启动的bool值
bool Server::start(){
    server_fd = socket(AF_INET,SOCK_STREAM,0);  //向操作系统申请一个网络设备,AF_INET表示ipv4形式
                                                //SOCK_STREAM表示可靠传输流通道
                                                //0表示自动配制协议，这里由于SOCK_STREAM，自动匹配TCP协议
    if(server_fd<0){    //如果申请成功，server_fd是一个非负整数
        perror("socket创建失败");
        return false;
    }
    else{return true;}

    //设置端口复用，我有点不懂，但是大概就是如果不设置在失败后不能把端口立即服用
    int opt = 1;
    if(setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt<0))){
        perror("setsockopt 失败");
        close(server_fd);
        return false;
    } 

    //绑定地址和端口，就是吧端口所有的请求都放到server_fd里面管
    struct sockaddr_in address; //一个IPv4的结构体，里面存了IP+端口
    memset(&address,0,sizeof(address));     //清理数据
    address.sin_family = AF_INET;   //存放ip地址
    address.sin_addr.s_addr = INADDR_ANY;   //用于配制监听所有的网卡
    address.sin_port = htons(port);     //端口转为网络字节序

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {     //bind函数把我的ip地址以及端口与server_fd绑在一起
        perror("bind 失败（端口被占用？）");
        close(server_fd);
        return false;
    }

    //开始监听，最大等待队列5，操作系统内核把 server_fd 的内部状态标志位，从“主动连接”（默认）切换成了“被动监听”（TCP_LISTEN）
    if(listen(server_fd,5)<0){
        perror("listen 失败");
        close(server_fd);
        return false;
    }

    is_running=true;
    std::cout << "[服务器] 已启动，监听端口 " << port << std::endl;

    //主循环启动

    while(is_running){
        struct sockaddr_in client_addr;     //创建一个结构体，用于存储客户端的地址
        socklen_t client_len = sizeof(client_addr);     //client_in用于储存client——addr的大小，据说是为了不同平台移植

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);     //创建client_fd，把它和地址端口绑定在一起

        if (client_fd < 0) {
            if (!is_running) {
                break;   // 正常退出循环
            }
            perror("accept 失败");
            continue;    // 非致命错误，继续等待下一个连接
        }
    }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[服务器] 新连接来自 " << client_ip << ":"
                  << ntohs(client_addr.sin_port) << std::endl;

}


