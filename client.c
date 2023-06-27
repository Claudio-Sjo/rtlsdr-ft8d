/*
 **********************************************************
 * Program : CLIENT.c
 *
 * Description: This programs behaves as a client to UI
 *              CLIENT reads from stdin or from a file a
 *              list of couples <S|R> <MESSAGE> where
 *              MESSAGE is the name of an allowed message
 *              described in USI_API.h
 *              The first element of the couple gives to
 *              CLIENT the operation to perform on the
 *              MESSAGE, either Send or Receive.
 *              In case of Receive, CLIENT will wait until
 *              the required message will be received, other
 *              messages will be discarded as errors.
 *
 * Revision info: File client.c
 *                Rev. 1.8
 *                Date 97/04/15
 *                Time 10:57:36
 *
 **********************************************************
 */

/* Standard include files */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Application specific include files */
#include "./ft8tx/FT8Types.h"

/* Program constants */
#define SOCKNAME "/tmp/ft8S"
#define MAXCONNECT 5
#define FOREVER 1

#define SUCCESSFUL_RUN 0
#define FTOK_FAIL 1
#define MSGGET_FAIL 2

/* Maximum size for a string, normally it's useful :-) */
#define MAXSTRING 255

/* The first socket descriptor of the table is the listening one */
#define SD 0

/* Error values */
#define NO_ERROR 0
#define ERR_NULL_POINTER 1001
#define ERR_MSG_SEND 1002
#define ERR_MSG_RECEIVE 1003
#define ERR_MISSING_SOCK 1004
#define ERR_PARSER 1005

#define RECEIVE_CMD 2000
#define QUIT_PROGRAM 2001
#define WAIT_PROGRAM 2002
#define WAIT_KEYBD 2003

/* Valid actions */
#define MESSAGE_SEND 1
#define MESSAGE_RECV 2

  
int main(int argc, char const* argv[])
{
    int status, valread, client_fd;
    struct sockaddr serv_addr  = {AF_UNIX, SOCKNAME};

    FT8Msg Txletter, Rxletter;

    sprintf(Txletter.ft8Message,"FT8Tx 20m CQ SA0PRF JO99");
    Txletter.type = SEND_F8_REQ;

    if ((client_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

/*  
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
  
    // Convert IPv4 and IPv6 addresses from text to binary
    // form
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)
        <= 0) {
        printf(
            "\nInvalid address/ Address not supported \n");
        return -1;
    }
*/  
    if ((status
         = connect(client_fd, (struct sockaddr*)&serv_addr,
                   sizeof(serv_addr)))
        < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }
    send(client_fd, &Txletter, sizeof(Txletter), 0);
    printf("Hello message sent\n");
    valread = read(client_fd, &Rxletter, sizeof(Rxletter));
    if (!valread)
        {
            printf("Error, nothing read");
            return 1;
        }
    printf("%s\n", Rxletter.ft8Message);
    valread = read(client_fd, &Rxletter, sizeof(Rxletter));
    if (!valread)
        {
            printf("Error, nothing read");
            return 1;
        }
    printf("%s\n", Rxletter.ft8Message);
    valread = read(client_fd, &Rxletter, sizeof(Rxletter));
    if (!valread)
        {
            printf("Error, nothing read");
            return 1;
        }
    printf("%s\n", Rxletter.ft8Message);
    // closing the connected socket
    // closing the connected socket
    close(client_fd);
    return 0;
}