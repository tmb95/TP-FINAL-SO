#include <stdlib.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <stdio.h>
#include <netinet/in.h>
#include <memory.h>
#include <unistd.h>
#include <semaphore.h>
#include <strings.h>
#include "server.h"

#define PENDING_CONNECTIONS 10
#define DATABASE_PROC       "database"

struct server {
    int listen_socket;
    // Write in db_in, read from db_out
    int database_in, database_out;

    struct sockaddr_in address;
    socklen_t          address_len;

    // semaforo para sincronizar consultas a la base de datos
    sem_t              semaphore;
};

/** Forks database handler process and creates pipes for inter-process communication */
static int database_init(Server server, char * filename);

int create_master_socket(int protocol, struct sockaddr *addr, socklen_t addr_len) {
    int sock_opt = true;

    int sock = socket(addr->sa_family, SOCK_STREAM, protocol);

    if (sock < 0) {
        perror("socket() failed");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&sock_opt, sizeof(sock_opt)) < 0) {
        perror("setsockopt() failed");
        return -1;
    }
    
    if(bind(sock, addr, addr_len) < 0) {
        perror("bind() failed");
        return -1;
    }

    if(listen(sock, PENDING_CONNECTIONS) != 0) {
        perror("listen() failed");
        return -1;
    }

    return sock;
}

Server server_init(int port, char * db_filename) {
    Server server = malloc(sizeof(struct server));

    if (server == NULL) {
        return NULL;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(struct sockaddr);

    addr.sin_port = htons((uint16_t) port);
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;

    memcpy(&server->address, &addr, addr_len);
    server->address_len = addr_len;

    server->listen_socket = create_master_socket(IPPROTO_TCP, (struct sockaddr *)&server->address, server->address_len);

    if (server->listen_socket < 0) {
        free(server);
        return NULL;
    }

    if (database_init(server, db_filename) < 0) {
        free(server);
        return NULL;
    }

    if (sem_init(&server->semaphore, 0, 1) < 0) {
        free(server);
        return NULL;
    }

    return server;
}

ClientData * server_accept_connection(Server server) {
    int client_socket = accept(server->listen_socket, 0, 0);

    if (client_socket < 0) {
        perror("accept() failed");
        return NULL;
    }

    ClientData * ret = malloc(sizeof(*ret));
    if (ret != NULL) {
        ret->client_fd = client_socket;
    }
    
    return ret;
}

int database_init(Server server, char * filename) {

    //create pipes, bytes written on db_...[1] can be read from db_...[0]
    int db_in[2];
    int db_out[2];

    int r1 = pipe(db_in);
    int r2 = pipe(db_out);

    if (r1 < 0 || r2 < 0) {
        perror("pipe() failed");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "Error starting database\n");
        perror("fork() failed");
        return -1;
    } else if (pid == 0) {
        dup2(db_in[0], STDIN_FILENO);
        dup2(db_out[1], STDOUT_FILENO);

        close(db_in[1]);
        close(db_out[0]);

        char * argv[] = {DATABASE_PROC, filename, NULL};
        char * envp[] = {NULL};

        execve(DATABASE_PROC, argv, envp);
        perror("execve() failed");  /* execve() returns only on error */
        exit(EXIT_FAILURE);
    } else {
        close(db_in[0]);
        close(db_out[1]);

        server->database_in = db_in[1];
        server->database_out = db_out[0];
    }

    return 0;
}

ssize_t server_read_request(Server server, ClientData * data) {
    int client_fd = data->client_fd;
    char * buffer = data->buffer;
    ssize_t n;

    bzero(buffer, BUFFER_SIZE);
    n = recv(client_fd, buffer, BUFFER_SIZE, 0);
    if (n > 0) {
        sem_wait(&server->semaphore);
        n = write(server->database_in, buffer, strlen(buffer));
    }

    //printf("request: %s", buffer);

    return n;
}

bool check_response_end(char * buffer) {
    size_t size = strlen(buffer);
    return size >= 3 && strcmp(&buffer[size - 3], "\n.\n") == 0;
}

ssize_t server_send_response(Server server, ClientData * data) {
    int client_fd = data->client_fd;
    char * buffer = data->buffer;
    ssize_t n;

    bool done;
    //printf("response: ");
    do {
        bzero(buffer, BUFFER_SIZE);
        n = read(server->database_out, buffer, BUFFER_SIZE);
        //printf("%s", buffer);
        done = check_response_end(buffer);
        if (n > 0) { 
            n = send(client_fd, buffer, strlen(buffer), MSG_NOSIGNAL);
        }
    } while(n > 0 && !done);

    sem_post(&server->semaphore);
    return n;
}

void server_close_connection(Server server, ClientData * data) {
    close(data->client_fd);
    free(data);
}

void server_close(Server server) {
    close(server->listen_socket);
    close(server->database_in);
    close(server->database_out);
    sem_destroy(&server->semaphore);
    free(server);
}