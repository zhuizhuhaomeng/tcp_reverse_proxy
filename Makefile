all:proxy_agent proxy_server
CC=gcc
CFLAGS=-g -O2 -Wall -pthread 
CPPFLAGS=-DSPROXY_THREAD_NUM=20

proxy_process: proxy_agent.c
	$(CC) -o proxy_agent proxy_agent.c $(CFLAGS) $(CPPFLAGS)
	
proxy_server: proxy_server.c
	$(CC) -o proxy_server proxy_server.c $(CFLAGS) $(CPPFLAGS)
	
clean:
	rm -rf proxy_agent proxy_server
