# 2023 Fall Computer Network - HTTP Server and Client Model
This is assignment 2 of the 2023 Fall Computer Network at National Taiwan University, implemented in C socket programming. This project is divided into two parts:
+ **HTTP Server**: A server that responds to HTTP requests from any source, e.g. the client in this project, or an Internet browser. The server can handle about 100 clients simultaneously, and clients can upload/download files and videos(through MPEG-DASH) to the server. The implementation can be found in *server.c*.
+ **HTTP Client**: A client program capable of requesting or uploading files or videos from the server with several commands, e.g. put, getv, etc. The implementation can be found in *client.c*.


For the full details of the behavior of the server and the client, please refer to [The Spec Sheet](https://github.com/yc-LoAndy/HTTP-Client-Server/blob/main/CN2023-HW2-ProblemSheet-1108.pdf).
