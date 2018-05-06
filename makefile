all: client server
client: client.c
        gcc client.c pingpong.c -libverbs -o client
server: server.c
        gcc server.c pingpong.c -libverbs -o server
update:
        rm -rf workshop2
        git clone https://github.com/regevbs/workshop2
        cp workshop2 ./*.c .
clean:
        rm -f client
