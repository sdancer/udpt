/*
 * Project: udptunnel
 * File: client.c
 *
 * Copyright (C) 2009 Daniel Meekins
 * Contact: dmeekins - gmail
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#ifndef WIN32
#include <sys/time.h>
#endif /*WIN32*/

#ifndef WIN32
#include <inttypes.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif /*WIN32*/

#include "common.h"
#include "client.h"
#include "socket.h"
#include "sendqueue.h"

extern int debug_level;

/*
 * Allocates and initializes a new client object.
 * id - ID number for the client to have
 * tcp_sock/udp_sock - sockets attributed to the client. this function copies
 *   the structure, so the calling function can free the sockets passed to
 *   here.
 * connected - whether the TCP socket is connected or not.
 * Returns a pointer to the new structure. Call client_free() when done with
 * it.
 */
client_t *client_create(uint64_t id, socket_t *tcp_sock, socket_t *udp_sock,
                        int connected)
{
    client_t *c = NULL;

    c = calloc(1, sizeof(client_t));
    if(!c)
        goto error;
    
    c->id = id;
    c->tcp_sock = sock_copy(tcp_sock);
    c->udp_sock = sock_copy(udp_sock);
    c->tcp2udp_q = list_create(sizeof(data_buf_t), NULL, NULL, NULL, 0);
    c->udp2tcp_state = CLIENT_WAIT_HELLO;
    c->tcp2udp_state = CLIENT_WAIT_DATA0;
    c->connected = connected;

    timerclear(&c->keepalive);
    timerclear(&c->tcp2udp_timeout);
    c->resend_count = 0;

    if(!c->tcp_sock || !c->udp_sock || !c->tcp2udp_q)
        goto error;
    
    return c;
    
  error:
    if(c)
    {
        if(c->tcp_sock)
            sock_free(c->tcp_sock);
        if(c->udp_sock)
            sock_free(c->udp_sock);
        if(c->tcp2udp_q)
            list_free(c->tcp2udp_q);
        free(c);
    }

    return NULL;
}

/*
 * Performs a deep copy of the client structure.
 */
client_t *client_copy(client_t *dst, client_t *src, size_t len)
{
    if(!dst || !src)
        return NULL;

    memcpy(dst, src, sizeof(*src));

    dst->tcp_sock = NULL;
    dst->udp_sock = NULL;
    dst->tcp2udp_q = NULL;
    
    dst->tcp_sock = sock_copy(src->tcp_sock);
    if(!dst->tcp_sock)
        goto error;

    dst->udp_sock = sock_copy(src->udp_sock);
    if(!dst->udp_sock)
        goto error;

    dst->tcp2udp_q = list_copy(src->tcp2udp_q);
    if(!dst->tcp2udp_q)
        goto error;
    
    return dst;

  error:
    if(dst->tcp_sock)
        sock_free(dst->tcp_sock);
    if(dst->udp_sock)
        sock_free(dst->udp_sock);
    if(dst->tcp2udp_q)
        list_free(dst->tcp2udp_q);

    return NULL;
}

/*
 * Compares the ID of the two clients.
 */
int client_cmp(client_t *c1, client_t *c2, size_t len)
{
    return c1->id - c2->id;
}

/*
 * Connects the TCP socket of the client (wrapper for sock_connect()). Returns
 * 0 on success or -1 on error.
 */
int client_connect_tcp(client_t *c)
{
    if(!c->connected)
    {
        if(sock_connect(c->tcp_sock, 0) == 0)
        {
            c->connected = 1;
            return 0;
        }
    }

    return -1;
}

/*
 * Closes the TCP socket for the client (wrapper for sock_close()).
 */
void client_disconnect_tcp(client_t *c)
{
    if(c->connected)
    {
        sock_close(c->tcp_sock);
        c->connected = 0;
    }
}

/*
 * Closes the UDP socket for the client (wrapper for sock_close()).
 */
void client_disconnect_udp(client_t *c)
{
    sock_close(c->udp_sock);    
}

/*
 * Releases the memory used by the client.
 */
void client_free(client_t *c)
{
    if(c)
    {
        if(debug_level >= DEBUG_LEVEL2)
        {
            printf("Freeing client id %llu\n", (long long unsigned int)c->id);
            printf("  q cnt left: %d\n", LIST_LEN(c->tcp2udp_q));
        }
        
        sock_free(c->tcp_sock);
        sock_free(c->udp_sock);
        list_free(c->tcp2udp_q);
        free(c);
    }
}

int client_send_tcp_data(client_t *client)
{
    int ret;
    
    ret = sock_send(client->tcp_sock, client->udp2tcp, client->udp2tcp_len);

    if(ret < 0)
        return -1;
    else if(ret == 0)
        return -2;
    else    
        return 0;
}

int client_recv_tcp_data(client_t *client, list_t * send_queue)
{
    int bytes_recv;
    data_buf_t buf;
    
    bytes_recv = recv(client->tcp_sock->fd, buf.buf, sizeof(buf.buf), MSG_DONTWAIT);

    //ret = sock_recv(client->tcp_sock, NULL, buf.buf, sizeof(buf.buf));
    if(bytes_recv < 0)
    {
        if (bytes_recv == -1)
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
                return 0;
        printf("socket error: %d\n",errno);
        return -1;
    }
    if(bytes_recv == 0)
        return -2;

    buf.header.seq_id = next_req_id++;
    buf.header.client_id = client->id;
    buf.header.type = MSG_TYPE_DATA0;
    buf.header.length = bytes_recv;
    list_add(send_queue, &buf, 1, 0);

    if(debug_level >= DEBUG_LEVEL3)
        printf("received data (%d), adding to queue[%d] cid:%lld\n", 
            buf.header.length, LIST_LEN(send_queue), (long long int)client->id);

    return bytes_recv;
}

//    MSG_data0
//    ret = msg_send_msg(client->udp_sock, client->id, msg_type,
//                       buf->buf, buf->len);

/*
 * Sends a HELLO type message to the udpserver (proxy) to tell it to make a
 * TCP connection to the specified host:port.
 */
int client_send_hello(client_t *client, char *host, char *port,
                      uint64_t req_id, list_t * send_queue)
{
    printf("msg_send_hello(%lld, %s, %s, %lld)\n",(long long int)client->id, host, port, (long long int)req_id);
    return msg_send_hello(client->udp_sock, host, port, req_id, send_queue);
}

/*
 * Sends a Hello ACK to the UDP tunnel.
 */
int client_send_helloack(client_t *client, uint64_t req_id, list_t * send_queue)
{
    return msg_send_msg(client->udp_sock, client->id, MSG_TYPE_HELLOACK,
                        (char *)&req_id, sizeof(req_id), send_queue);
}

/*
 * Sends a goodbye message to the UDP server.
 */
int client_send_goodbye(client_t *client, list_t * send_queue)
{
    return msg_send_msg(client->udp_sock, client->id, MSG_TYPE_GOODBYE,
                        NULL, 0, send_queue);
}

