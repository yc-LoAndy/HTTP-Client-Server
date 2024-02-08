#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h> 
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netdb.h>
#include <unistd.h> 
#include <fcntl.h>
#include "utils/base64.h"

#define REQ_SIZE 210*1024*1024
#define CMD_LIMIT 50
#define MAX_PATH_LEN 150
#define ERR_EXIT(a){ perror(a); exit(1); }
#define BOUNDTEXT "WebKitFormBoundaryaW1sb3Zpbmdjbg"
#define BOUNDLEN strlen(BOUNDTEXT)

/* Some subroutines */
int host_need_solve(char* host);
void parse_command(char* input, char* cmd_type, char* param, char *encoded_param);
int parse_response(uint8_t* response, int* body_len, int *isclosed);
int download_file_from_server(char* file_path, uint8_t* response, int body_len);
int pack_file_to_body(char* file_path, uint8_t* content);
void set_proper_mime(char* filepath, char* mime);
void reconnect_to_server(int sockfd, struct sockaddr_in* addr);

/* Commands */
int GET_command(int sockfd, char* cmd_type, char *param, char *encoded_param);
int PUT_command(int sockfd, char* cmd_type, char* param, int vflag);

/* Global variables */
char SER_ADDR[50];
int PORT;
char *ENCODED_CREDENTIALS;


int main(int argc, char *argv[]) {   /* cmd: ./client 127.0.0.1 9999 demo:123 */
    if (argc < 4) {
        fprintf(stderr, "Usage: ./client [host] [port] [username:password] \n"); return 0;
    }
    strcpy(SER_ADDR, argv[1]);
    PORT = atoi(argv[2]);

    size_t encoded_len;
    char* credentials = argv[3];
    ENCODED_CREDENTIALS = base64_encode((unsigned char*) credentials, strlen(credentials), &encoded_len);

    // Solve Domain name
    if (host_need_solve(SER_ADDR)) {
        struct hostent* info = gethostbyname(SER_ADDR);
        if (!(inet_ntoa(*(struct in_addr *)info->h_name))) {
            fprintf(stderr, "Cannot solve host name. Exit.\n");
            return 0;
        }
        strcpy(SER_ADDR, inet_ntoa(*(struct in_addr *)info->h_name));
    }

    int sockfd;
    struct sockaddr_in addr;

    // Get socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        ERR_EXIT("Client failed to create socket. Exit.");
    }

    // Set server address
    bzero(&addr,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(SER_ADDR);
    addr.sin_port = htons(PORT);

    // Connect to the server
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        ERR_EXIT("Client failed to connect to the server. Exit.\n");
    }

    // Main loop
    while (1) {
        // Waiting for commands
        char input[CMD_LIMIT];
        printf("> ");
        fgets(input, CMD_LIMIT, stdin);
        if (input[0] == '\n') continue;

        // Parse command
        int n_input = strlen(input);
        char cmd_type[n_input];
        char param[50] = {}; char encoded_param[50] = {};
        parse_command(input, cmd_type, param, encoded_param);

        // Handle command
        int res = 0;
        // GET command: download a file from the server
        if (strncmp(cmd_type, "get", 3) == 0) {
            res = GET_command(sockfd, cmd_type, param, encoded_param);
        }
        // PUTV command: POST a video to the server
        else if ((strncmp(cmd_type, "putv", 4) == 0)) {
            res = PUT_command(sockfd, cmd_type, param, 1);
        }
        // PUT command: POST a file to the server
        else if ((strncmp(cmd_type, "put", 3) == 0)) {
            res = PUT_command(sockfd, cmd_type, param, 0);
        }
        // QUIT command
        else if (strncmp(cmd_type, "quit", 4) == 0) {
            printf("Bye.\n");
            break;
        }
        // UNKNOWN command
        else {
            fprintf(stderr, "Command not found.\n");
            // continue;
        }

        // If the server close the connection
        if (res < 0) {
            reconnect_to_server(sockfd, &addr);
        }
    }

    close(sockfd);
    return 0;
}

int GET_command(int sockfd, char* cmd_type, char *param, char *encoded_param) {
    // Incomplete command
    if (strlen(param) == 0) { fprintf(stderr, "Usage: get [file]\n"); return 0; }

    // Compose HTTP request
    char *request = (char *) malloc(REQ_SIZE*sizeof(char));
    snprintf(request, REQ_SIZE,
        "GET /api/file/%s HTTP/1.1\r\n"
        "Host: %s:%i\r\n"
        "Connection: keep-alive\r\n"
        "User-Agent: CN2023Client/1.0\r\n"
        "\r\n",
        encoded_param, SER_ADDR, PORT
    );

    ssize_t byt = send(sockfd, request, strlen(request), 0);
    free(request);

    // Wait for the response from the server
    uint8_t *response = (uint8_t *) malloc(REQ_SIZE*sizeof(uint8_t));
    byt = recv(sockfd, response, REQ_SIZE, 0);
    if (byt == 0) {
        free(response);
        return -1;
    }
    response[byt] = '\0';

    int body_len = 0; int is_closed = 0;
    int status = parse_response(response, &body_len, &is_closed);
    // printf("is_closed = %d\n", is_closed);

    // Keep receiving until all message arrives
    char *p = strstr((char *)response, "\r\n\r\n") + 4;
    int bcount = byt - ((uint8_t *)p - response);
    uint8_t *buffer = (uint8_t *) malloc(REQ_SIZE*sizeof(uint8_t));
    uint8_t *q = response + byt;
    // printf("bcount = %d, body_len = %d\n", bcount, body_len);
    while (bcount < body_len) {
        memset(buffer, 0, REQ_SIZE);
        byt = recv(sockfd, buffer, REQ_SIZE, 0);
        buffer[byt] = '\0';
        // printf("Get additional %d byte\n", (int)byt);
        memcpy(q, buffer, byt);

        bcount = bcount + byt;
        q = q + byt;
    }
    free(buffer);

    // printf("%s\n", response);

    if (status == 200) {
        // Command succedded. Download the file received.
        char file_path[MAX_PATH_LEN] = {};
        snprintf(file_path, MAX_PATH_LEN, "./files/%s", param);
        int res = download_file_from_server(file_path, response, body_len);

        if (res == 0) printf("Command succeeded.\n");
        else printf("Command failed.\n");

    } else if (status == 404) {
        // fprintf(stderr, "Command failed. The file is not found.\n");
        fprintf(stderr, "Command failed.\n");
    } else if (status == 500) {
        // fprintf(stderr, "Command failed. Server Internal error.\n");
        fprintf(stderr, "Command failed.\n");
    }

    free(response);
    if (is_closed) return -1; else return 0;
}

int PUT_command(int sockfd, char* cmd_type, char *param, int vflag) {
    // Incomplete command
    if (strlen(param) == 0) {
        if (vflag == 0)
            fprintf(stderr, "Usage: put [file]\n");
        else 
            fprintf(stderr, "Usage: putv [file]\n");
        return 0;
    }

    // Create the directory hw2/files if doesn't exist
    struct stat st = {0};
    if (stat("./files", &st) == -1) mkdir("./files", 0777);

    // Check if the file exists
    char file_path[2*MAX_PATH_LEN] = {}, filename[MAX_PATH_LEN] = {}, mime[50] = {};
    strcpy(filename, param);
    sprintf(file_path, "./%s", filename);
    char *p = strrchr(filename, '/'), pure_fname[MAX_PATH_LEN];
    if (!p) strcpy(pure_fname, filename);
    else strcpy(pure_fname, p+1);
    set_proper_mime(pure_fname, mime);

    if (access(file_path, F_OK) != 0) {
        // fprintf(stderr, "Command failed with filepath = %s.\n", file_path);
        fprintf(stderr, "Command failed.\n");
        return 0;
    }

    // Compose HTTP request
    char *request = (char *) calloc(REQ_SIZE, sizeof(char));
    uint8_t *content = (uint8_t *) calloc(REQ_SIZE, sizeof(uint8_t));
    int file_len = pack_file_to_body(file_path, content);

    if (file_len < 0) {
        // fprintf(stderr, "Command failed with packing failure.\n");
        fprintf(stderr, "Command failed.\n");
        free(request); free(content);
        return 0;
    }

    char boundary[500];
    snprintf(boundary, 500, 
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"upfile\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n"
        "\r\n",
        BOUNDTEXT, pure_fname, mime
    );

    char *dest;
    if (vflag == 1) dest = "video";
    else if (vflag == 0) dest = "file";

    snprintf(request, REQ_SIZE,
        "POST /api/%s HTTP/1.1\r\n"
        "Host: %s:%i\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %i\r\n"
        "User-Agent: CN2023Client/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "\r\n"
        "%s",
        dest, SER_ADDR, PORT, BOUNDTEXT, (int)(file_len+strlen(boundary)+BOUNDLEN+6),
        ENCODED_CREDENTIALS, boundary
    );
    int header_len = strlen(request);
    memcpy(request+header_len, content, file_len);

    char *d = "\r\n--"; char* dashes = "--"; char* bdtxt = BOUNDTEXT;
    memcpy(request+header_len+file_len, d, strlen(d));
    memcpy(request+header_len+file_len+strlen(d), bdtxt, strlen(bdtxt));
    memcpy(request+header_len+file_len+strlen(d)+strlen(bdtxt), dashes, strlen(dashes));

    send(sockfd, request, header_len+file_len+BOUNDLEN+strlen(d)+strlen(dashes), 0);
    // printf("REQUEST\n%s\n", request);

    free(request);
    free(content);

    // Wait for the response from the server
    uint8_t *response = (uint8_t *) malloc(REQ_SIZE*sizeof(uint8_t));
    ssize_t byt = recv(sockfd, response, REQ_SIZE, 0);
    if (byt == 0) {
        free(response);
        return -1;
    }

    response[byt] = '\0';
    int body_len = 0; int is_closed = 0;
    int status = parse_response(response, &body_len, &is_closed);
    // printf("is_closed = %d\n", is_closed);

    if (status == 200) {
        printf("Command succeeded.\n");
    } else if (status == 401) {
        // fprintf(stderr, "Command failed with status code 401.\n");
        fprintf(stderr, "Command failed.\n");
    } else {
        // fprintf(stderr, "Command failed with status code %d.\n", status);
        fprintf(stderr, "Command failed.\n");
    }

    free(response);
    if (is_closed) return -1; else return 0;
}

int host_need_solve(char* host) {
    char *p = host;
    while (*p != '\0') {
        if (*p != '.' && isalpha(*p)) return 1;
        p++;
    }
    return 0;
}

void parse_command(char* input, char* cmd_type, char* param, char* encoded_param) {
    // Parse the input command from the user
    int i, j = 0, k = 0, n_input = strlen(input);
    for (i = 0; i < n_input; i++) {
        if (input[i] == ' ') break;
        cmd_type[i] = input[i];
    }
    cmd_type[i++] = '\0';

    char c, code[4] = {}, buf[4] = {};
    for (; i < n_input-1; i++) {
        // Percentage encoding
        c = input[i]; param[k++] = c;
        if ((!isdigit(c)) && (!isalpha(c)) && (c != '-') && (c != '_') && (c != '.') && (c != '~')) {
            // clear buffer
            code[0] = '\0'; buf[0] = '\0';

            strcat(code, "\%");
            if (c < 16) strcat(code, "0");
            snprintf(buf, 4, "%x", c);
            strcat(code, buf);

            for (int k = 0; k < 3; k++) encoded_param[j++] = code[k];
        } else {
            encoded_param[j++] = c;
        }
    }
    param[k] = '\0';
    encoded_param[j] = '\0';
}

int parse_response(uint8_t* response, int* body_len, int *isclosed) {
    // Extract the status of the response message
    char status[4] = {};
    for (int i = 9; i <= 11; i++) status[i-9] = response[i];
    status[3] = '\0';

    // Check if the server close the connection
    char *q = strstr((char *)response, "Connection");
    *isclosed = 0;
    if (q) {
        q = q + 12;
        if (strncmp(q, "close", 5) == 0) *isclosed = 1;
    }

    // Get length of the file content
    char* p = strstr((char *)response, "Content-Length") + 16;
    char bdlen_c[10];
    int i = 0; for (; *p != '\r'; i++, p++) { bdlen_c[i] = *p; }
    bdlen_c[i] = '\0';
    *body_len = atoi(bdlen_c);

    return atoi(status);
}

int download_file_from_server(char* file_path, uint8_t* response, int body_len) {
    // Create the directory hw2/files if doesn't exist
    struct stat st = {0};
    if (stat("./files", &st) == -1) mkdir("./files", 0777);

    // Save the received text as a file
    int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    chmod(file_path, 0777);

    char *content_start = strstr((char *)response, "\r\n\r\n");
    if (!content_start) {close(fd); return -1;} else content_start = content_start + 4;

    int res = write(fd, content_start, body_len);
    close(fd);

    if (res < body_len) return -1;
    return 0;
}

int pack_file_to_body(char* file_path, uint8_t* content) {
    // Open the file and copy the content into the body of the HTTP request
    int fp = open(file_path, O_RDONLY);
    if (fp == -1) return -1;

    // size_t x = fread(content, sizeof(char), file_len, fp);
    int x = read(fp, content, REQ_SIZE);
    close(fp);

    return x;
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

void reconnect_to_server(int sockfd, struct sockaddr_in* addr) {
    close(sockfd);
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        ERR_EXIT("Client failed to create socket. Exit.");
    }

    // Set server address
    bzero(addr, sizeof(*addr));
    (*addr).sin_family = AF_INET;
    (*addr).sin_addr.s_addr = inet_addr(SER_ADDR);
    (*addr).sin_port = htons(PORT);

    // Try to reconnect to the server
    if (connect(sockfd, (struct sockaddr*)addr, sizeof(*addr)) < 0){
        ERR_EXIT("Client failed to connect to the server. Exit.\n");
    }
}
