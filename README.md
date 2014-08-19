# A tcp stream replay tool devoted to concurrentcy


#简单说明
tcpburn是一种针对tcp应用的回放工具，主要专注于构造客户端并发。


#特性
    1）可以保持原先会话过程中的网络特性
	2）客户端无需绑定多个ip地址，而且客户端可使用的ip地址个数不受限制
	3）并发用户数量不受os的限制，可支持的最大并发用户数,取决于该机器的带宽,cpu和内存
	4）只要是可回放的协议,一般都支持


#用途：
    1）提供压力测试所需要的并发压力
    2）为comet应用提供数百万并发用户的压力
    3）破坏性测试，可以充分暴露被测试程序在高并发压力下的各种问题


#架构

![tcpcopy](https://raw.github.com/wangbin579/auxiliary/master/images/tcpburn.GIF)

原理见上图，tcpburn默认从测试服务器（Test server）的IP层发送数据包给目标服务器（Target server），而目标服务器的应答包，则通过路由设置，被路由给辅助服务器（Assistant server），辅助服务器负责截获这些应答包，并返回给tcpburn，从而可以进行tcp session的交互。


#tcpburn configure Options
    --with-debug      compile tcpburn with debug support (saved in a log file)
    --pcap-send       send packets at the data link layer instead of the IP layer
    --single          单一实例运行方式（跟intercept一一对应），适合于高效使用
    --comet           消息推送模式

#安装使用

###1）下载编译运行intercept：

	git clone git://github.com/session-replay-tools/intercept.git
	cd intercept
	./configure --single  
	make
	make install
	
	具体执行如下：
	
	On the assistant server which runs intercept(root privilege is required):
	  ./intercept -F <filter> -i <device,> 
	
	  Note that the filter format is the same as the pcap filter.
	  For example:
	  ./intercept -i eth0 -F 'tcp and src port 80' -d
	  Intercept will capture response packets of the TCP based application which listens on port 80 
	  from device eth0 

###2）在目标服务器设置路由信息：

	On the target server which runs test server applications (root privilege is required):
	  Set route command appropriately to route response packets to the assistant server
	
	  For example:
	
	  Assume 65.135.233.161 is the IP address of the assistant server. We set the following route 
	  commands to route all responses to the 62.135.200.*'s clients to the assistant server.
	
	  route add -net 62.135.200.0 netmask 255.255.255.0 gw 65.135.233.161


###3）下载编译运行tcpburn：

	git clone git://github.com/session-replay-tools/tcpburn.git
	cd tcpburn

	如果是非comet应用：
	  ./configure --single 
	如果是comet类似的消息推送应用
	  #会过滤掉pcap文件中的连接关闭命令，由服务器来主动关闭连接
	  ./configure --single --comet  
	
	make
	make install
	
	./tcpburn -x historyServerPort-targetServerIP:targetServerPort -f <pcapfile,> 
    -s <intercept address> -u <user num> -c <ip range,>
	
	注:上述historyServerPort是录制的pcap文件中的server端口
	
	例如（假设目标服务器外网IP地址为65.135.233.160，辅助服务器内网IP地址为10.110.10.161）：
	
	./tcpburn -x 80-65.135.233.160:80 -f /home/wangbin/work/github/80.pcap -s 10.110.10.161 
    -u 10000 -c 62.135.200.*
	
	从80.pcap抓包文件中提取出访问80端口的用户会话过程，复制到65.135.233.160目标服务器中去，
    其中模拟的用户数量为10000个用户，客户端ip地址范围为62.135.200.*系列,intercept所在的辅助服务器
    内网ip地址为10.110.10.161（外网ip地址为65.135.233.161)


#注意事项：
	1）所使用的会话数据均从pcap文件中抽取，且要确保此文件尽可能完整（比如抓包内容要抓全），而且不丢包
	2）-c参数指定的ip地址范围(最好为非所在机器网段)，目前只能是最后一个为‘*'，如果需要多个网段的ip地址，则采用‘,’隔开
	3）-s参数指定intercept所在机器的地址，一般只需指定ip地址即可
	4）对于消息推送服务（comet），需要确保有ip地址能够publish主题（比如内网ip地址来publish主题，
       外网ip地址来供模拟客户端用户来访问，外网的访问，其响应走辅助服务器）
	5）对于pcap文件，还可以采用-F参数来过滤。
	6）对于消息推送服务（comet）应用，pcap文件最好不要包含publish的请求
	7）tcpburn定义的一个用户，就是一个连接的会话，从pcap文件中提取，所以用户构造会话过程，要注意连接的特性。
	8）默认采用raw socket发包，这时候需要关闭ip_conntrack模块或者采用pcap发包（"--pcap-send"）
	9）更多信息还可以见-h命令

