#!/bin/bash

# 配置 Yum 源
# 如果是内部源，不要做这一步
mv /etc/yum.repos.d/CentOS-Base.repo /etc/yum.repos.d/CentOS-Base.repo.backup
curl -o /etc/yum.repos.d/CentOS-Base.repo https://mirrors.aliyun.com/repo/Centos-7.repo

# 保证源可用
yum clean all
yum makecache

# 安装依赖包
yum install -y gcc-c++ wget which vim git make bzip2 perl-IPC-Cmd gcc gcc-c++ make perl wget tar

# 安装 CMake
cd /opt/
wget https://github.com/Kitware/CMake/releases/download/v3.26.0-rc4/cmake-3.26.0-rc4-linux-x86_64.sh
chmod +x /opt/cmake-3.26.0-rc4-linux-x86_64.sh
/opt/cmake-3.26.0-rc4-linux-x86_64.sh

ln -s /opt/cmake-3.26.0-rc4-linux-x86_64/bin/ccmake /usr/bin/ccmake
ln -s /opt/cmake-3.26.0-rc4-linux-x86_64/bin/cmake /usr/bin/cmake
ln -s /opt/cmake-3.26.0-rc4-linux-x86_64/bin/cmake-gui /usr/bin/cmake-gui
ln -s /opt/cmake-3.26.0-rc4-linux-x86_64/bin/cpack /usr/bin/cpack
ln -s /opt/cmake-3.26.0-rc4-linux-x86_64/bin/ctest /usr/bin/ctest

# 安装更多依赖
yum install -y perl-IPC-Cmd gcc gcc-c++ make perl wget tar m4 patch autoconf automake libtool python3 libicu libicu-devel libevent libevent-devel  curl libcurl libcurl-devel zlib-static libuuid libuuid-devel  gnutls-devel

# 克隆并编译 ProxySQL
cd /src/proxysql
git checkout v2.4.8
make