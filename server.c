#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/time.h>   //FD_SET, FD_ISSET, FD_ZERO macros
#include <dirent.h>
#include <pthread.h>
#include "utils/base64.h"

#define RES_SIZE 210*1024*1024
#define MAX_PATH_LEN 150
#define ERR_EXIT(a){ perror(a); exit(1); }
#define MAXCLIENT 100

/* Some subroutines */
int parse_request(uint8_t* request, char* method, char* path);
int get_file_for_client(char* filename, uint8_t* file_data, int flag);  // flag = 0: for client; flage = 1: for browser
int confirm_auth(uint8_t* request);
int upload_file_from_client(uint8_t* request, int body_len, int vflag);
void extract_vname(uint8_t* request, char* vname);
void* convert_dash(void* video_name);
void per_encode(char* name, char* encoded_name);
void set_proper_mime(char *filepath, char* mime);

/* Routings */
void GET_api_file(int connfd, uint8_t* request, char* method, char* url, int vflag);
void POST_api_file(int connfd, uint8_t* request, char* method, int body_len, int rcv_size, int vflag);
void GET_home_page(int connfd, char* method);
void GET_upload_webpage(int connfd, char* method, int vflag);
void GET_vpalyer_webpage(int connfd, char *method, char *url);
void GET_file_list(int connfd, char* method);
void GET_video_list(int connfd, char* method);
void Not_found_page(int connfd);


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: ./server [port]\n"); return 0;
    }
    int PORT = atoi(argv[1]);

    int listenfd, connfd;   // listenfd is the master socket
    int client_socket[MAXCLIENT], activity;
    int sd, maxsd;
    ssize_t byt = RES_SIZE;
    struct sockaddr_in server_addr, client_addr;
    int client_addr_len = sizeof(client_addr);

    // Initialization
    fd_set readfds;     // a set of socket descriptor
    for (int i = 0; i < MAXCLIENT; i++) client_socket[i] = 0;

    // Get socket file descriptor
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        ERR_EXIT("Server failed to create socket. Exit.");
    }

    // Set server address information
    bzero(&server_addr, sizeof(server_addr)); // erase the data
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    // Bind the server file descriptor to the server address
    if (bind(listenfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
        ERR_EXIT("Server failed to bind the socket with the address. Exit.");
    }

    // Listen on the server file descriptor
    if (listen(listenfd, 5) < 0){
        ERR_EXIT("Server failed to start listening. Exit.");
    }
    printf("[SYS] Listening on port %i. \n", PORT);

    // Main loop
    uint8_t *request = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    while (1) {
        // Clear the socket set and add master socket into it
        FD_ZERO(&readfds);
        FD_SET(listenfd, &readfds);
        maxsd = listenfd;

        // add child sockets to the set
        for (int i = 0; i < MAXCLIENT; i++) {
            sd = client_socket[i];
            if (sd > 0) FD_SET(sd, &readfds);
            if (sd > maxsd) maxsd = sd;
        }

        // Wait indefinitely for activity of any socket
        activity = select(maxsd+1, &readfds, NULL, NULL, NULL);
        if (activity < 0) { fprintf(stderr, "Select error.\n"); }

        // If something happens on the master socket, then new connection incomming
        if (FD_ISSET(listenfd, &readfds)) {
            if ((connfd = accept(listenfd, (struct sockaddr *)&client_addr, (socklen_t*)&client_addr_len)) < 0){
                ERR_EXIT("Server failed to accept connection. Exit.");
            }

            // Add new socket to the array of socket
            for (int i = 0; i < MAXCLIENT; i++) {
                // if position is empty 
                if (client_socket[i] == 0) {
                    client_socket[i] = connfd;
                    printf("[SYS] New connection adding to list of sockets as %d\n", i);
                    break;
                }
            }
        }

        // Deal with I/O operation requested by currnet sockets
        for (int i = 0; i < MAXCLIENT; i++) {
            sd = client_socket[i];

            if (FD_ISSET(sd, &readfds)) {
                // Read message from the socket
                memset(request, 0, RES_SIZE);  // Clear buffer for the new request
                if ((byt = recv(sd, request, RES_SIZE, 0)) < 0) {
                    ERR_EXIT("Server failed to receive message from the client. Exit.");
                }

                // The client disconnected
                if (byt == 0) {
                    getpeername(sd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_len);
					printf("[SYS] Host disconnected, ip %s, port %d\n",
						inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

                    client_socket[i] = 0;
                    close(sd);
                }
                // The client sends some request to the server
                request[byt] = '\0';

                printf("\n[SYS] Receive: %s, size = %i\n", request, (int) byt);

                // Parse requests
                char method[5] = {}; char url[MAX_PATH_LEN] = {};
                int body_len = parse_request(request, method, url);

                /* METHOD: GET  /api/file/{filename} */
                if (strncmp(url, "/api/file/", 10) == 0) {
                    GET_api_file(sd, request, method, url, 0);
                }
                /* METHOD: GET  /api/video/{filepath} */
                else if (strncmp(url, "/api/video/", 11) == 0) {
                    GET_api_file(sd, request, method, url, 1);
                }
                /* METHOD: POST /api/video */
                else if (strncmp(url, "/api/video", 10) == 0) {
                    POST_api_file(sd, request, method, body_len, byt, 1);

                    if (confirm_auth(request) >= 0) {
                        char vname[MAX_PATH_LEN] = {};
                        extract_vname(request, vname);

                        pthread_t thread; int res = 0;
                        if ((res = pthread_create(&thread, NULL, convert_dash, (void *) vname)) != 0) {
                            fprintf(stderr, "Fail to create thread to convert video file.\n");
                        }
                        if ((res = pthread_detach(thread)) != 0) {
                            fprintf(stderr, "Fail to deteach thread.\n");
                        }
                    }
                }
                /* METHOD: POST /api/file */
                else if (strncmp(url, "/api/file", 9) == 0) {
                    POST_api_file(sd, request, method, body_len, byt, 0);
                }
                /* METHOD: GET  /video/{videoname}, for video player only */
                else if ((strncmp(url, "/video/", 7) == 0) && (url[7] != '\0')) {
                    GET_vpalyer_webpage(sd, method, url);
                }
                /* METHOD: GET  /file */
                else if (strncmp(url, "/file/", 6) == 0) {
                    GET_file_list(sd, method);
                }
                /* METHOD: GET  /video */
                else if ((strncmp(url, "/video/", 7) == 0) && (url[7] == '\0')) {
                    GET_video_list(sd, method);
                }
                /* METHOD: GET  /upload/file */
                else if (strncmp(url, "/upload/file", 12) == 0) {
                    GET_upload_webpage(sd, method, 0);
                }
                /* METHOD: GET  /upload/video */
                else if (strncmp(url, "/upload/video", 13) == 0) {
                    GET_upload_webpage(sd, method, 1);
                }
                /* METHOD: GET  / */
                else if ((strncmp(url, "/", 1) == 0) && (strlen(url) <= 1)) {
                    GET_home_page(sd, method);
                }
                /* 404 Not Found */
                else {
                    Not_found_page(sd);
                }
            }
        }
    }

    printf("[SYS] Connection closed.\n");
    free(request);

    for (int i = 0; i < MAXCLIENT; i++) close(client_socket[i]);
    close(listenfd);

    return 0;
}

void GET_api_file(int connfd, uint8_t* request, char* method, char* url, int vflag) {
    /* GET /api/file/ or /api/video/XXX/{filename} */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    if (strncmp(method, "GET", 3) != 0) {
        // Method not allowed
        strncpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    char filename[MAX_PATH_LEN] = {};
    char mime[30] = {};
    int i, j = 0, flag;
    if (vflag == 0) {
        i = 10; flag = 0;   // Find ordinary file
    } else {
        i = 11; flag = 2;   // find .mpd file
    }
    for (; i < strlen(url); i++) filename[j++] = url[i];
    filename[j] = '\0';
    set_proper_mime(filename, mime);

    uint8_t *file_data = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    int res = get_file_for_client(filename, file_data, flag);

    if (res == -1) {
        // File doesn't exist
        strncpy(response,
            "HTTP/1.1 404 Not Found\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 10\r\n"
            "\r\n"
            "Not Found\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    } else if (res == -2) {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    } else if (res >= 0) {
        int file_len = res;
        snprintf(response, RES_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Server: CN2023Server\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "\r\n",
            mime, file_len
        );

        int header_len = strlen(response);
        memcpy(response+header_len, file_data, file_len);
        send(connfd, response, header_len+file_len, 0);
    }

    free(response);
    free(file_data);
}

void POST_api_file(int connfd, uint8_t* request, char* method, int body_len, int rcv_size, int vflag) {
    /* POST /api/file */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    // Method not allowed
    if (strncmp(method, "POST", 4) != 0) {
        strcpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: POST\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    // Keep receiving until all message arrives
    char *p = strstr((char *)request, "\r\n\r\n") + 4;
    int bcount = rcv_size - ((uint8_t *)p - request);
    uint8_t *buffer = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    ssize_t byt; uint8_t *q = request + rcv_size;
    // printf("bcount = %d, body_len = %d\n", bcount, body_len);
    while (bcount < body_len) {
        memset(buffer, 0, RES_SIZE);
        byt = recv(connfd, buffer, RES_SIZE, 0);
        buffer[byt] = '\0';
        memcpy(q, buffer, byt);

        bcount = bcount + byt;
        q = q + byt;
    }
    free(buffer);

    // Unauthorized Request
    int auth = confirm_auth(request);
    if (auth < 0) {
        strcpy(response,
            "HTTP/1.1 401 Unauthorized\r\n"
            "Server: CN2023Server/1.0\r\n"
            "WWW-Authenticate: Basic realm=\"B08704043\"\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Unauthorized\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    int res = upload_file_from_client(request, body_len, vflag);
    if (res >= 0) {
        if (vflag == 0) {
            strncpy(response,
                "HTTP/1.1 200 OK\r\n"
                "Server: CN2023Server\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 15\r\n"
                "\r\n"
                "File Uploaded.\n",
                RES_SIZE
            );
        } else {
            strncpy(response,
                "HTTP/1.1 200 OK\r\n"
                "Server: CN2023Server\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 16\r\n"
                "\r\n"
                "Video Uploaded.\n",
                RES_SIZE
            );
        }
    } else {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
    }

    // Send the response
    send(connfd, response, strlen(response), 0);
    free(response);
}

void GET_home_page(int connfd, char* method) {
    /* GET / */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    // Method not allowed
    if (strncmp(method, "GET", 3) != 0) {
        strcpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    char filename[10] = "index.html";
    uint8_t *file_data = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    int res = get_file_for_client(filename, file_data, 1);

    if (res >= 0) {
        int file_len = res;
        snprintf(response, RES_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Server: CN2023Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            file_len, file_data
        );
        send(connfd, response, strlen(response), 0);
    } else {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    }

    free(response);
    free(file_data);
}

void GET_upload_webpage(int connfd, char* method, int vflag) {
    /* GET /upload/file */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    // Method not allowed
    if (strncmp(method, "GET", 3) != 0) {
        strcpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    char filename[13];
    if (vflag == 0) strcpy(filename, "uploadf.html");
    else strcpy(filename, "uploadv.html");
    uint8_t *file_data = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    int res = get_file_for_client(filename, file_data, 1);

    if (res >= 0) {
        int file_len = res;
        snprintf(response, RES_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Server: CN2023Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            file_len, file_data
        );
        send(connfd, response, strlen(response), 0);
    } else {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    }

    free(response);
    free(file_data);
}

void GET_vpalyer_webpage(int connfd, char *method, char *url) {
    /* GET /video/{videoname} */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    // Method not allowed
    if (strncmp(method, "GET", 3) != 0) {
        strcpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    // Obtain the html
    char filename[13] = "player.rhtml";
    uint8_t *file_data = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    int res = get_file_for_client(filename, file_data, 1);
    int file_len = res;

    // Extract vedio name from the url
    char *vname = strstr(url, "video") + 6;    // no .mp4 extention here

    // Create /videos if not exists
    struct stat st = {0};
    if (stat("./web/videos", &st) == -1) mkdir("./web/videos", 0777);

    // Modify the player.rhtml to include the video file
    char *pseudotag1 = "<?VIDEO_NAME?>"; char *p1 = strstr((char *) file_data, pseudotag1);
    char *pseudotag2 = "<?MPD_PATH?>";   char *p2 = strstr((char *) file_data, pseudotag2);
    int l1 = p1 - (char *) file_data;
    int l2 = (p2 - p1) - strlen(pseudotag1);
    char *new_file_data = (char *) malloc(RES_SIZE*sizeof(char));
    char api_path[MAX_PATH_LEN] = {};
    strncat(new_file_data, (char *)file_data, l1);
    strcat(new_file_data, vname);
    strncat(new_file_data, p1+strlen(pseudotag1), l2);
    sprintf(api_path, "\"/api/video/%s/dash.mpd\"", vname); strcat(new_file_data, api_path);    // should include .mp4 here
    strcat(new_file_data, p2+strlen(pseudotag2));

    file_len = strlen(new_file_data);

    if (res >= 0) {
        snprintf(response, RES_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Server: CN2023Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            file_len, new_file_data
        );
        send(connfd, response, strlen(response), 0);
    } else {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    }

    free(response);
    free(file_data);
}

void GET_file_list(int connfd, char* method) {
    /* GET /file */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    // Method not allowed
    if (strncmp(method, "GET", 3) != 0) {
        strcpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    char filename[13] = "listf.rhtml";
    uint8_t *file_data = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    int res = get_file_for_client(filename, file_data, 1);
    int file_len = res;

    // Create /files if not exists
    struct stat st = {0};
    if (stat("./web/files", &st) == -1) mkdir("./web/files", 0777);

    DIR *d;
    struct dirent *dir;
    d = opendir("./web/files");

    // Modify the html file to contain file lists
    int current_size = 500;
    char* file_lists = (char *) malloc(current_size*sizeof(char));
    char row[300] = {}, encoded_name[200] = {};
    int word_count = 0;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            char* filename = dir->d_name;
            per_encode(filename, encoded_name);
            if ((strcmp(filename, ".") != 0) && (strcmp(filename, "..") != 0)) {
                sprintf(row, "<tr><td><a href=\"/api/file/%s\">%s</a></td></tr>\n", encoded_name, filename);

                // Expand the memory space if there are too many files
                word_count = word_count + strlen(row);
                if (word_count > 0.9 * current_size) {
                    current_size = current_size * 2;
                    file_lists = realloc(file_lists, current_size);
                }
                strcat(file_lists, row);
            }
        }
        closedir(d);
    }

    char *pseudotag = "<?FILE_LIST?>"; char *p = strstr((char *)file_data, pseudotag);
    int l = p - (char *)file_data;
    char *new_file_data = (char *) malloc(RES_SIZE*sizeof(char));
    strncat(new_file_data, (char *)file_data, l);
    strcat(new_file_data, file_lists);
    strcat(new_file_data, p+strlen(pseudotag)+1);

    file_len = strlen(new_file_data);
    free(file_lists);

    if (res >= 0) {
        snprintf(response, RES_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Server: CN2023Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            file_len, new_file_data
        );
        send(connfd, response, strlen(response), 0);
    } else {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    }

    free(response);
    free(file_data);

}

void GET_video_list(int connfd, char *method) {
    /* GET /video/ */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));

    // Method not allowed
    if (strncmp(method, "GET", 3) != 0) {
        strcpy(response,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Allow: GET\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
        send(connfd, response, strlen(response), 0);
        return;
    }

    char filename[13] = "listv.rhtml";
    uint8_t *file_data = (uint8_t *) malloc(RES_SIZE*sizeof(uint8_t));
    int res = get_file_for_client(filename, file_data, 1);
    int file_len = res;

    // Create /videos if not exists
    struct stat st = {0};
    if (stat("./web/videos", &st) == -1) mkdir("./web/videos", 0777);

    DIR *d;
    struct dirent *dir;
    d = opendir("./web/videos");

    // Modify the html file to contain file lists
    int current_size = 500;
    char* file_lists = (char *) malloc(current_size*sizeof(char));
    char row[300] = {}, new_name[200] = {};
    int word_count = 0;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            char* filename = dir->d_name;
            if ((strcmp(filename, ".") != 0) && (strcmp(filename, "..") != 0)) {
                per_encode(filename, new_name);
                sprintf(row, "<tr><td><a href=\"/video/%s\">%s</a></td></tr>\n", new_name, filename);

                // Expand the memory space if there are too many files
                word_count = word_count + strlen(row);
                if (word_count > 0.9 * current_size) {
                    current_size = current_size * 2;
                    file_lists = realloc(file_lists, current_size);
                }
                strcat(file_lists, row);
            }
        }
        closedir(d);
    }

    char *pseudotag = "<?VIDEO_LIST?>"; char *p = strstr((char *) file_data, pseudotag);
    int l = p - (char *) file_data;
    char *new_file_data = (char *) malloc(RES_SIZE*sizeof(char));
    strncat(new_file_data, (char *)file_data, l);
    strcat(new_file_data, file_lists);
    strcat(new_file_data, p+strlen(pseudotag)+1);

    file_len = strlen(new_file_data);
    free(file_lists);

    if (res >= 0) {
        snprintf(response, RES_SIZE,
            "HTTP/1.1 200 OK\r\n"
            "Server: CN2023Server\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            file_len, new_file_data
        );
        send(connfd, response, strlen(response), 0);
    } else {
        strncpy(response,
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Server: CN2023Server/1.0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            RES_SIZE
        );
        send(connfd, response, strlen(response), 0);
    }

    free(response);
    free(file_data);
}

void Not_found_page(int connfd) {
    /* Not handled by the rounting */
    char *response = (char *) malloc(RES_SIZE*sizeof(char));
    strncpy(response,
        "HTTP/1.1 404 Not Found\r\n"
        "Server: CN2023Server/1.0\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "Not Found\n",
        RES_SIZE
    );
    send(connfd, response, strlen(response), 0);
    free(response);
}


int parse_request(uint8_t* request, char* method, char* path) {
    // Extract the method and the path from the request
    // method: only POST or GET
    if (request[0] == '\0') return 0;

    int i, j = 0; char c, num, hexs[3];
    if (strncmp((char *)request, "GET", 3) == 0) {
        strcpy(method, "GET");
        method[3] = '\0';
        i = 4;
    } else if (strncmp((char *)request, "POST", 4) == 0) {
        strcpy(method, "POST");
        method[4] = '\0';
        i = 5;
    } else {
        return 0;
    }

    for (; request[i] != ' '; i++) {
        c = request[i];
        if (c != '\%') path[j++] = c;
        else {
            hexs[0] = request[i+1]; hexs[1] = request[i+2]; hexs[3] = '\0';
            num = (char) strtol(hexs, NULL, 16);     // convert from hex to decimal
            path[j++] = num;
            i = i + 2;      // done parsing, so skip the encoded part
        }
    }
    path[j] = '\0';

    // Find content length
    char *p = strstr((char *)request, "Content-Length");
    if (!p) return 0; else p = p + 16;

    char bdlen_c[10];
    for (i = 0; *p != '\r'; i++, p++) { bdlen_c[i] = *p; }
    bdlen_c[i] = '\0';
    return atoi(bdlen_c);
}

int get_file_for_client(char* filename, uint8_t* file_data, int flag) {
    // Create directory if /hw2/web/files doesn't exist
    /*
        flag = 0 --> Find ordinary file in ./web/files/
        flag = 1 --> Find HTML file in ./web/
        flag = 2 --> Find MPD file in ./web/videos/{videoname}/
    */
    struct stat st = {0};
    if (stat("./web/files", &st) == -1) mkdir("./web/files", 0777);

    // Check if the requested file exist
    char filepath[MAX_PATH_LEN];
    if (flag == 0) snprintf(filepath, MAX_PATH_LEN, "./web/files/%s", filename);
    else if (flag == 1) snprintf(filepath, MAX_PATH_LEN, "./web/%s", filename);
    else if (flag == 2) {
        // No .mp4 here
        snprintf(filepath, MAX_PATH_LEN, "./web/videos/%s", filename);
    }

    if (access(filepath, F_OK) != 0) {
        // File doesn't exist
        return -1;
    } else {
        // File exists, response with the requested file
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "Failed to open the file.\n"); return -2; }
        int x = read(fd, file_data, RES_SIZE);
        file_data[x] = '\0';
        close(fd);

        return x;
    }
}

int confirm_auth(uint8_t* request) {
    char *start = strstr((char *)request, "\r\n\r\n") + 4;
    int l = start - (char *)request;
    char *req_header = (char *) malloc(RES_SIZE*sizeof(char));
    strncpy(req_header, (char *)request, l);

    // Confirm authentication from client's request
    char* p = strstr((char *)req_header, "Authorization");
    if (!p) return -1;

    // Otherwise, locate username and password
    p = p + 21;
    char* end = strchr(p, '\r');
    int codelen = end-p; char code[codelen+1];
    for (int i = 0; i < codelen; i++) { code[i] = *p; p++; }
    code[codelen] = '\0';

    size_t decodelen;
    unsigned char* decode = base64_decode(code, strlen(code), &decodelen);
    decode[decodelen] = '\0';
    if (!decode) return -1;
    char* deli = strchr((char*) decode, ':');
    if (!deli) return -1;

    char user[21], password[21];
    int i = 0;
    while (decode[i] != ':') {
        user[i] = decode[i];
        i++;
    }
    user[i] = '\0';
    int j = ++i;
    while (i < decodelen) {
        password[i-j] = decode[i];
        i++;
    }
    password[i-j] = '\0';


    FILE* secretfd = fopen("./secret", "r");
    char secret_u[21], secret_p[21];
    int count = 1;
    while (count > 0) {
        count = fscanf(secretfd, " %20[^:]:%20s", secret_u, secret_p);
        if (strcmp(secret_u, user) == 0 && strcmp(secret_p, password) == 0) {
            fclose(secretfd);
            return 0;
        }
    }

    fclose(secretfd);
    return -1;
}

int upload_file_from_client(uint8_t* request, int body_len, int vflag) {
    // Create directory if /hw2/web/files or /hw2/web/tmp doesn't exist
    struct stat st1 = {0}, st2 = {0};
    if (stat("./web/files", &st1) == -1) mkdir("./web/files", 0777);
    if (stat("./web/tmp", &st2) == -1) mkdir("./web/tmp", 0777);

    // Save the received text as a file
    char *p, *q;
    p = strstr((char *)request, "filename") + 10;
    char filename[MAX_PATH_LEN];
    for (int i = 0; *p != '\"'; i++) {
        filename[i] = *p;
        p++;
        if (*p == '\"') { filename[i+1] = '\0'; break; }
    }

    char file_path[MAX_PATH_LEN];
    if (vflag == 0) strcpy(file_path, "./web/files/");
    else if (vflag == 1) strcpy(file_path, "./web/tmp/");
    strcat(file_path, filename);

    int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    chmod(file_path, 0777);

    // Get the boundary code
    q = strstr((char *)request, "boundary");
    if (!q) return -1; else q = q + 9;
    char boundary[75];
    int i = 0; for (; *q != '\r'; i++, q++) { boundary[i] = *q; }
    boundary[i] = '\0';
    int bound_len = strlen(boundary);

    uint8_t *content_start = (uint8_t *) strstr(p, "\r\n\r\n") + 4;
    uint8_t *r = content_start;
    char *dash = "-";
    while (1) {
        if ((memcmp(r, dash, 1) == 0) && (memcmp(r+1, dash, 1) == 0)) {
            int x = memcmp(r+2, boundary, bound_len);
            if (x == 0) break;
        }
        r++;
    }
    int word_count = r - content_start - 2;

    int res = write(fd, content_start, word_count);
    close(fd);

    if (res < word_count) return -1;
    return 0;
}

void extract_vname(uint8_t* request, char* vname) {
    int j = 0;
    char *p = strstr((char *)request, "filename") + 10;
    for (; *p != '\"'; p++) {
        if ((*p == '.') && (*(p+1) == 'm') && (*(p+2) == 'p') && (*(p+3) == '4')) {
            vname[j] = '\0';
            return;
        } else {
            vname[j++] = *p;
        }
    }
}

void* convert_dash(void* video_name) {
    /* Convert the video file to DASH type */
    char *comm = malloc(MAX_PATH_LEN * sizeof(char));
	char *vname = (char *) video_name;
    sprintf(comm, "./convert.sh %s", vname);
    system(comm);

    free(comm);
	pthread_exit(NULL);
}

void per_encode(char* name, char* encoded_name) {
    char c, code[4] = {}, buf[4] = {};
    int name_len = strlen(name);
    int i, j = 0;
    for (i = 0; i < name_len; i++) {
        // Percentage encoding
        c = name[i];
        if ((!isdigit(c)) && (!isalpha(c)) && (c != '-') && (c != '_') && (c != '.') && (c != '~')) {
            // clear buffer
            code[0] = '\0'; buf[0] = '\0';

            strcat(code, "\%");
            if (c < 16) strcat(code, "0");
            snprintf(buf, 4, "%x", c);
            strcat(code, buf);

            for (int k = 0; k < 3; k++) encoded_name[j++] = code[k];
        } else {
            encoded_name[j++] = c;
        }
    }
    encoded_name[j] = '\0';
}

void set_proper_mime(char *filepath, char *mime) {
    char *p = strrchr(filepath, '.');
    if (!p) { strcpy(mime, "text/plain"); return; }
    if (strcmp(p, ".html") == 0) strcpy(mime, "text/html");
    else if ((strcmp(p, ".mp4") == 0) || (strcmp(p, ".m4v") == 0)) strcpy(mime, "video/mp4");
    else if (strcmp(p, ".m4s") == 0) strcpy(mime, "video/iso.segment");
    else if (strcmp(p, ".m4a") == 0) strcpy(mime, "audio/mp4");
    else if (strcmp(p, ".mpd") == 0) strcpy(mime, "application/dash+xml");
    else if ((strcmp(p, ".jpg") == 0) || (strcmp(p, ".jpeg") == 0)) strcpy(mime, "image/jpeg");
    else if (strcmp(p, ".png") == 0) strcpy(mime, "image/png");
    else strcpy(mime, "text/plain");
}
