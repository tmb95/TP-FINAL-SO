#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include <ctype.h>
#include "getnum.h"
#include "client.h"
#include "response_parser.h"
#include "../utils.h"
#include "../protocol.h"

#define CLEAR_BUFFER        while (getchar() != '\n')

typedef enum {
    LOGIN_ADMIN = 1,
    LOGIN_CLIENT,
    LOGIN_EXIT
} login_menu_option;

typedef enum {
    ADMIN_ADD_SHOWCASE = 1,
    ADMIN_REMOVE_SHOWCASE,

    ADMIN_EXIT,
} admin_menu_option;

typedef enum {
    CLIENT_BUY_TICKET = 1,
    CLIENT_VIEW_TICKETS,
    CLIENT_CANCEL_RESERVATION,
    CLIENT_EXIT
} client_menu_option;

int get_string(char * msg, char * buff, int max_len);
int get_option(char * msg, char ** options, int count);

/** Serializes a request and sends it to the server */
ssize_t send_request(Client client, int request_type, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char buffer[BUFFER_SIZE] = {0};
    char * aux = buffer;

    aux += sprintf(aux, "%d\n", request_type);
    while(*fmt != 0) {
        char c = *fmt++;
        if (c == '%') {
            c = *fmt++;
            switch (c) {
                case 's':
                    aux += sprintf(aux, "%s\n", va_arg(ap, char *));
                    break;
                case 'd':
                    aux += sprintf(aux, "%d\n", va_arg(ap, int));
                    break;
                default:
                    break;
            }
        }
    }

    va_end(ap);
    sprintf(aux, ".\n");

    //fprintf(stderr, "'%s'", buffer);

    return client_send(client, buffer);
}

/** Waits until the server response is finished */
Response * wait_response(Client client) {
    Response * response = new_response();
    if (response == NULL) {
        fprintf(stderr, "Memory error");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE] = {0};
    ResponseParser parser;
    response_parser_init(&parser, response);
    bool done;

    printf("Waiting server response...\n");
    do {
        bzero(buffer, BUFFER_SIZE);
        client_recv(client, buffer);
        response_parser_consume(&parser, buffer);
        done = response_parser_is_done(&parser, 0);
    } while(!done);

    response_parser_destroy(&parser);

    //printf("Server response: %s\n", get_response_type(response->status));
    //print_response(response);
    return response;
}

char * get_movie(Response * response) {
    List movies = response_extract_movies(response);

    if (list_size(movies) == 0) {
        list_destroy(movies);
        return NULL;
    }

    char * movie;
    for (int i = 0; (movie = list_get_next(movies)) != NULL; i++){
        printf("%d- %s\n", i + 1, movie);
    }

    int n;
    do {
        n = getint("Pick a movie: ");
        if (n <= 0 || n > list_size(movies)) {
            printf("Invalid option.\n");
        }
    } while (n <= 0 || n > list_size(movies));

    movie = list_get(movies, n - 1);
    char * ret =  calloc(strlen(movie) + 1, sizeof(char));
    if (ret == NULL) {
        fprintf(stderr, "Memory error.");
        exit(EXIT_FAILURE);
    }
    strcpy(ret, movie);
    list_destroy(movies);
    return ret;
}

#define HOUR    18
#define MINUTES 30

Showcase * get_showcase(Response * response, char * movie_name) {
    List showcases = response_extract_showcases(response);

    if (list_size(showcases) == 0) {
        list_destroy(showcases);
        return NULL;
    }

    Showcase * aux;
    printf("Showcases for '%s':\n", movie_name);
    printf(" # | DAY | TIME  | ROOM\n");
    printf("------------------------\n");
    for (int i = 0; (aux = list_get_next(showcases)) != NULL; i++) {
        printf(" %d | %s | %d:%d |  %d\n", i + 1, get_day(aux->day), HOUR, MINUTES, aux->room);
        printf("------------------------\n");
    }

    int n;
    do {
        n = getint("Pick a showcase: ");
        if (n <= 0 || n > list_size(showcases)) {
            printf("Invalid option.\n");
        }
    } while (n <= 0 || n > list_size(showcases));

    aux = list_get(showcases, n - 1);
    Showcase * showcase = new_showcase(aux->movie_name, aux->day, aux->room);

    while ((aux = list_get_next(showcases)) != NULL) {
        destroy_showcase(aux);
    }

    list_destroy(showcases);
    return showcase;
}

int get_seat(Response * response) {
    int seats[SEATS];
    response_extract_seats(response, seats);

    putchar('\t');
    for (int j = 0; j < COLS; j++) {
        printf(" %d\t", j+1);
    }

    putchar('\n');
    for (int i = 0; i < ROWS; i++) {
        printf("%d\t", i+1);
        for (int j = 0; j < COLS; j++) {
            printf("[%c]\t", seats[i * COLS + j] == EMPTY_SEAT ? ' ':'X');
        }
        putchar('\n');
    }

    int row, col;
    do {
        row = getint("Enter row: ");
        col = getint("Enter column: ");
        if (row <= 0 || row > ROWS || col <= 0 || col > COLS) {
            printf("Invalid option.\n");
        }
    } while (row <= 0 || row > ROWS || col <= 0 || col > COLS);

    //printf("SEAT: %d\n", ((row - 1) * COLS + col));

    return ((row - 1) * COLS + col) - 1;
}

#define GET_COL(seat) (((seat) % COLS) + 1)
#define GET_ROW(seat) (((seat) / COLS) + 1)

void print_ticket(Ticket * ticket) {
    printf("==================================\n");
    printf("TICKET FOR: %s\n", ticket->showcase.movie_name);
    printf("==================================\n");
    printf("%s, %d:%d\n", get_day(ticket->showcase.day), HOUR, MINUTES);
    printf("ROOM         ROW         SEAT\n");
    printf("%d            %d           %d\n", ticket->showcase.room, GET_ROW(ticket->seat), GET_COL(ticket->seat));
    printf("==================================\n\n");
}

void buy_ticket(Client client, char * client_name) {
    // GET_MOVIES
    Response * response;
    send_request(client, GET_MOVIES, "");
    response = wait_response(client);

    char * movie_name = get_movie(response);
    destroy_response(response);

    if (movie_name == NULL) {
        printf("There are no movies...\n");
        return;
    }

    // GET_SHOWCASES
    send_request(client, GET_SHOWCASES, "%s", movie_name);
    response = wait_response(client);

    Showcase * showcase  = get_showcase(response, movie_name);
    destroy_response(response);

    if (showcase == NULL) {
        printf("There are no showcases for the movie: '%s'...\n", movie_name);
        free(movie_name);
        return;
    }

    // GET_SEATS
    send_request(client, GET_SEATS, "%s%d%d", showcase->movie_name, showcase->day, showcase->room);
    response = wait_response(client);

    int seat = get_seat(response);
    destroy_response(response);

    // ask confirmation
    bool buy = yesNo("Press (y/n): ");

    if (buy) {
        // ADD_BOOKING
        send_request(client, ADD_BOOKING, "%s%s%d%d%d", client_name, showcase->movie_name, showcase->day, showcase->room, seat);
        response = wait_response(client);

        if (response->status == RESPONSE_OK) {
            printf("Ticket bought!\n");
            Ticket * ticket = new_ticket(*showcase, seat);
            print_ticket(ticket);
            destroy_ticket(ticket);
        } else if (response->status == ALREADY_EXIST){
            printf("Seat not available!\n");
        } else {
            printf("Error purchasing ticket!\n");
        }
        destroy_response(response);
    }

    destroy_showcase(showcase);
    free(movie_name);
}

void view_tickets(Client client, char * client_name) {
    // GET_BOOKING
    Response * response;
    send_request(client, GET_BOOKING, "%s", client_name);
    response = wait_response(client);

    List tickets = response_extract_tickets(response);
    Ticket * ticket;

    if (list_size(tickets) == 0) {
        printf("You don't have any tickets.\n");
    }

    for (int i = 0; (ticket = list_get_next(tickets)) != NULL; i++) {
        printf("-ticket %d-\n", i+1);
        print_ticket(ticket);
        destroy_ticket(ticket);
    }

    destroy_response(response);
    list_destroy(tickets);

    // press key to go back
    printf("Press any key... ");
    CLEAR_BUFFER;
}

Ticket * get_ticket(Response * response) {
    List tickets = response_extract_tickets(response);

    if (list_size(tickets) == 0) {
        list_destroy(tickets);
        printf("You don't have any tickets to cancel.\n");
        return 0;
    }

    Ticket * aux;

    for (int i = 0; (aux = list_get_next(tickets)) != NULL; i++) {
        printf("-ticket %d-\n", i+1);
        print_ticket(aux);
    }

    int n;
    do {
        n = getint("Pick a ticket: ");
        if (n <= 0 || n > list_size(tickets)) {
            printf("Invalid option.\n");
        }
    } while (n <= 0 || n > list_size(tickets));

    aux = list_get(tickets, n - 1);

    Ticket * ticket = new_ticket(aux->showcase, aux->seat);

    for (int i = 0; (aux = list_get_next(tickets)) != NULL; i++) {
        destroy_ticket(aux);
    }
    list_destroy(tickets);

    return ticket;
}

void cancel_reservation(Client client, char * client_name) {
    // GET_BOOKING
    Response * response;
    send_request(client, GET_BOOKING, "%s", client_name);
    response = wait_response(client);

    Ticket * ticket = get_ticket(response);
    destroy_response(response);
    if (ticket == NULL) {
        return;
    }

    bool cancel = yesNo("Press (y/n): ");

    if (cancel) {
        // REMOVE_BOOKING
        send_request(client, REMOVE_BOOKING, "%s%s%d%d%d", client_name,
                     ticket->showcase.movie_name, ticket->showcase.day,
                     ticket->showcase.room, ticket->seat);
        response = wait_response(client);

        if (response->status == RESPONSE_OK) {
            printf("Reservation cancelled.\n");
        } else {
            printf("Error cancelling reservation.\n");
        }
        destroy_response(response);
    }

    destroy_ticket(ticket);
}

#define ADD_SHOWCASE_ENTER_MOVIE_NAME   1
#define ADD_SHOWCASE_SEE_MOVIE_LIST     2
#define ADD_SHOWCASE_EXIT               3

void admin_add_showcase(Client client) {

    char * options[] = {"Enter movie name", "See movie list", "Exit"};
    int option = get_option("Choose an option: ", options, ADD_SHOWCASE_EXIT);

    char * movie_name;
    char new_movie[MOVIE_NAME_LENGTH];
    Response * response;
    switch (option) {
        case ADD_SHOWCASE_ENTER_MOVIE_NAME:
            get_string("Enter movie name: ", new_movie, MOVIE_NAME_LENGTH);
            movie_name = new_movie;
            break;
        case ADD_SHOWCASE_SEE_MOVIE_LIST:
            // GET_MOVIES
            send_request(client, GET_MOVIES, "");
            response = wait_response(client);
            movie_name = get_movie(response);
            destroy_response(response);
            if (movie_name == NULL) {
                printf("There are no movies...\n");
                return;
            }
            break;
        case ADD_SHOWCASE_EXIT:
            return;
        default:
            return;
    }

    char * days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int day = get_option("Pick a day: ", days, 7);
    int room;

    do {
        room = getint("Pick a room (between 1 and 5): ");
        if (room <= 0 || room > ROOMS) {
            printf("Invalid room number\n");
        }
    } while (room <= 0 || room > ROOMS);

    bool add = yesNo("Press (y/n): ");
    if (add) {
        // ADD_SHOWCASE
        send_request(client, ADD_SHOWCASE, "%s%d%d", movie_name, day - 1, room);
        response = wait_response(client);
        if (response->status == RESPONSE_OK) {
            printf("Showcase added..\n");
        } else if (response->status == ALREADY_EXIST) {
            printf("Showcase already exists.\n");
        } else {
            printf("Error adding showcase.\n");
        }
        destroy_response(response);
    }

    if (option == ADD_SHOWCASE_SEE_MOVIE_LIST) {
        free(movie_name);
    }
}

void admin_remove_showcase(Client client) {
    // GET_MOVIES
    Response * response;
    send_request(client, GET_MOVIES, "");
    response = wait_response(client);

    char * movie_name = get_movie(response);
    destroy_response(response);

    if (movie_name == NULL) {
        printf("There are no movies...\n");
        return;
    }

    // GET_SHOWCASES
    send_request(client, GET_SHOWCASES, "%s", movie_name);
    response = wait_response(client);

    Showcase * showcase = get_showcase(response, movie_name);
    destroy_response(response);

    if (showcase == NULL) {
        printf("There are no showcases for the movie: '%s'...\n", movie_name);
        free(movie_name);
        return;
    }

    bool remove = yesNo("Press (y/n): ");
    if (remove) {
        // REMOVE_SHOWCASE
        send_request(client, REMOVE_SHOWCASE, "%s%d%d", showcase->movie_name, showcase->day, showcase->room);
        response = wait_response(client);

        if (response->status == RESPONSE_OK) {
            printf("Showcase removed..\n");
        } else {
            printf("Error removing showcase.\n");
        }

        destroy_response(response);
    }

    destroy_showcase(showcase);
    free(movie_name);
}


/** Admin options */
void admin_menu(Client client) {

    char * options[] = {"Add showcase", "Remove showcase", "Exit"};

    printf("Logged in as Administrator.\n");

    while (true) {
        admin_menu_option option = (admin_menu_option) get_option("Choose an option: ", options, ADMIN_EXIT);

        switch (option) {
            case ADMIN_ADD_SHOWCASE:
                admin_add_showcase(client);
                break;
            case ADMIN_REMOVE_SHOWCASE:
                admin_remove_showcase(client);
                break;
            case ADMIN_EXIT:
                return;
        }
    }
}

void add_new_client(Client client, char * client_name) {
    send_request(client, ADD_CLIENT, "%s", client_name);
    Response * response = wait_response(client);

    int status = response->status;
    destroy_response(response);

    if (status == RESPONSE_OK) {
        printf("Welcome %s!\n", client_name);
    } else if (status == ALREADY_EXIST){
        printf("Welcome back %s!\n", client_name);
    } else {
        printf("Login error.\n");
        exit(EXIT_FAILURE);
    }
}

/** Client options */
void client_menu(Client client) {

    char client_name[CLIENT_NAME_LENGTH + 1] = {0};
    int ret = 0;
    do {
        ret = get_string("Enter your name: ", client_name, CLIENT_NAME_LENGTH + 1);
    } while (ret == 0);

    add_new_client(client, client_name);

    char * options[] = {"Buy a ticket", "View tickets", "Cancel a reservation", "Exit"};

    while (true) {
        client_menu_option option =  (client_menu_option) get_option("Choose an option: ", options, CLIENT_EXIT);
        switch (option) {
            case CLIENT_BUY_TICKET:
                buy_ticket(client, client_name);
                break;
            case CLIENT_VIEW_TICKETS:
                view_tickets(client, client_name);
                break;
            case CLIENT_CANCEL_RESERVATION:
                cancel_reservation(client, client_name);
                break;
            case CLIENT_EXIT:
                return;
        }
    }
}

/** Login */
void client_start(Client client) {
    char * options[] = {"Login as Admin", "Login as Client", "Exit"};

    //printf("");
    while (true) {
        login_menu_option option = (login_menu_option) get_option("Choose an option: ", options, LOGIN_EXIT);

        switch (option) {
            case LOGIN_ADMIN:
                admin_menu(client);
                break;
            case LOGIN_CLIENT:
                client_menu(client);
                break;
            case LOGIN_EXIT:
                return;
        }
    }
}

void parse_options(int argc, char **argv, char ** host, int * port) {
    opterr = 0;
    /* p: option e requires argument p:: optional argument */
    int c;
    while ((c = getopt (argc, argv, "h:p:")) != -1) {
        switch (c) {
            /* Host name */
            case 'h':
                *host = optarg;
                break;
            /* Server port number */
            case 'p':
                *port = parse_port(optarg);
                break;
            case '?':
                if (optopt == 'h' || optopt == 'p')
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                else if (isprint (optopt))
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                exit(1);
            default:
                abort();
        }
    }
}

int main(int argc, char*argv[]) {

    char * hostname = DEFAULT_HOST;
    int server_port = DEFAULT_PORT;

    parse_options(argc, argv, &hostname, &server_port);

    Client client = client_init(hostname, server_port);
    if (client == NULL) {
        return -1;
    }

    client_start(client);
    client_close(client);
    return 0;
}

int get_string(char * msg, char * buff, int max_len) {
    printf("%s",msg);
    int c;
    int i = 0;
    while((c=getchar())!='\n' && i < max_len){
        buff[i] = (char) c;
        i++;
    }
    if (i == max_len) i--;
    buff[i] = 0;

    if(c != '\n'){
        CLEAR_BUFFER;
    }

    return i;
}

int get_option(char * msg, char ** options, int count) {
    int option = 0;

    do {
        for (int i = 0; i < count; i++) {
            printf("%d- %s\n", i + 1, options[i]);
        }
        option = getint(msg);
    } while(option < 0 || option > count);

    return option;
}
