.PHONY: all clean

all: server client
server: server.c
	chmod 0777 convert.sh
	gcc -o server -pthread server.c utils/base64.c
client: client.c
	gcc -o client client.c utils/base64.c
clean:
	@rm -rf server client 
