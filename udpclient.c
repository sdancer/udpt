/*
 * Project: udptunnel
 * File: udpclient.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifndef WIN32
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#else
#include "helpers/winhelpers.h"
#endif

#include "common.h"
#include "message.h"
#include "socket.h"
#include "client.h"
#include "list.h"

#include "sendqueue.h"

static int MAX_TCP_QUEUE = 4096;

extern int ipver;
static int running = 1;

/* internal functions */
static int c_handle_message(client_t *c, uint64_t id, uint8_t msg_type,
                          char *data, int data_len, list_t * send_queue);
static void disconnect_and_remove_client(uint64_t id, list_t *clients,
                                         fd_set *fds, int full_disconnect, list_t * send_queue);
static void signal_handler(int sig);

static int proc_packet(char * pdata, list_t * clients, fd_set * client_fds, list_t * conn_clients, list_t * send_queue);

/*
int packet_cmp(const void *c1, const void *c2, size_t len)
{
    return ((msg_hdr_t *)c1)->seq_id - ((msg_hdr_t *)c2)->seq_id;
}
*/

void nofree(void * ignored)
{

}

int udpclient(int argc, char *argv[])
{
    int fresh_ack = 0;

    int stats_enqueued = 0;
    int stats_already_in_queue = 0;
    long long unsigned int packetscount = 0;
    int stats_truncated = 0;

    int send_queue_ptr = 0;
    uint64_t acked_seq = 0;
    char *lhost, *lport, *phost, *pport, *rhost, *rport;
    list_t *clients = NULL;
    list_t *conn_clients;
    list_t *queue;
    list_t *send_queue;

    uint64_t next_client_id = 500;

    client_t *client;
    client_t *client2;
    socket_t *tcp_serv = NULL;
    socket_t *tcp_sock = NULL;
    socket_t *udp_sock = NULL;
    socket_t *transmisor = NULL;
    char data[MSG_MAX_LEN + sizeof(msg_hdr_t)];
    char addrstr[ADDRSTRLEN];
    socket_t *udp_from = NULL;

    uint64_t remote_lastack = 0;

    struct timeval curr_time;
    struct timeval reset_time;
    struct timeval reset_interval;
    struct timeval stats_time;
    struct timeval stats_interval;
    struct timeval gflood_time;
    struct timeval gflood_interval;    
    struct timeval timeout;
    fd_set client_fds;
    fd_set read_fds;
    uint16_t tmp_len;
    int num_fds;
    
    int ret;
    int i, j;
    
    signal(SIGINT, &signal_handler);

    i = 0;    
    lhost = (argc - i == 5) ? NULL : argv[i++];
    lport = argv[i++];
    phost = argv[i++];
    pport = argv[i++];
    rhost = argv[i++];
    rport = argv[i++];

    /* Check validity of ports (can't check ip's b/c might be host names) */
    ERROR_GOTO(!isnum(lport), "Invalid local port.", done);
    ERROR_GOTO(!isnum(pport), "Invalid proxy port.", done);
    ERROR_GOTO(!isnum(rport), "Invalid remote port.", done);
    
    printf("local %s %s proxy %s %s rem end %s %s\n", lhost, lport, phost, pport, rhost, rport);

    srand(time(NULL));
    next_req_id = rand() % 0xffff;
    
    /* Create an empty list for the clients */
    clients = list_create(sizeof(client_t), p_client_cmp, p_client_copy,
                          p_client_free, 1);
    ERROR_GOTO(clients == NULL, "Error creating clients list.", done);

    /* Create and empty list for the connecting clients */
    conn_clients = list_create(sizeof(client_t), p_client_cmp, p_client_copy,
                               p_client_free, 1);
    ERROR_GOTO(conn_clients == NULL, "Error creating clients list.", done);

    queue = list_create(sizeof(data_buf_t), ptr_cmp, NULL,
                               NULL, 1);

    send_queue = list_create(sizeof(data_buf_t), ptr_cmp, NULL,
                               NULL, 0);


    list_t *ack_queue;
    ack_queue = list_create(sizeof(ack_record_t), NULL, NULL,
                               NULL, 0);
    int ack_queue_ptr = 0;

    /* Create a TCP server socket to listen for incoming connections */
    tcp_serv = sock_create(lhost, lport, ipver, SOCK_TYPE_TCP, 1, 1);
    ERROR_GOTO(tcp_serv == NULL, "Error creating TCP socket.", done);
    if(debug_level >= DEBUG_LEVEL1)
    {
        printf("Listening on TCP %s\n",
               sock_get_str(tcp_serv, addrstr, sizeof(addrstr)));
    }
    
    FD_ZERO(&client_fds);

    /* Initialize all the timers */
    timerclear(&timeout);

    reset_interval.tv_sec = 0;
    reset_interval.tv_usec = 100000;
    gettimeofday(&reset_time, NULL);

    stats_interval.tv_sec = 1;
    stats_interval.tv_usec = 0;
    gettimeofday(&stats_time, NULL);
    
    if (options_pps < 10)
        options_pps = 100;

    gflood_interval.tv_sec = 0;
    gflood_interval.tv_usec = 1000000 / options_pps;
    gettimeofday(&gflood_time, NULL);

    //transmisor = sock_create(phost, pport, ipver, SOCK_TYPE_UDP, 0, 1);

    udp_sock = sock_create(phost, pport, ipver, SOCK_TYPE_UDP, 0, 1);

    //udp_sock = sock_create('0.0.0.0', pport,
    //                       ipver, SOCK_TYPE_UDP, 1, 1);

    transmisor = udp_sock;

    if(udp_sock == NULL)
    {
        sock_close(tcp_sock);
        sock_free(tcp_sock);
        PERROR_GOTO(1, "cant make a udp socket", done);
        return 0;
    }


    while(running)
    {
        if(!timerisset(&timeout))
            timeout.tv_usec = 1000000 / options_pps;

        read_fds = client_fds;
        FD_SET(SOCK_FD(tcp_serv), &read_fds);
        FD_SET(SOCK_FD(udp_sock), &read_fds);

        ret = select(FD_SETSIZE, &read_fds, NULL, NULL, &timeout);
        PERROR_GOTO(ret < 0, "select", done);
        num_fds = ret;

        gettimeofday(&curr_time, NULL);

        if(timercmp(&curr_time, &reset_time, >))
        {
            sendqueue_resets = 0;
            timeradd(&curr_time, &reset_interval, &reset_time);
        }

        if(timercmp(&curr_time, &stats_time, >))
        {
            uint64_t next_p_seq = 0;
            if (LIST_LEN(send_queue) > 0)
            {
                data_buf_t * element = list_get_at(send_queue, 0);
                next_p_seq = element->header.seq_id;
            }
            printf("stats = udp packets: %llu | %d /s lastack %llu queue(%d) next seq: %lld\n", 
                (long long unsigned int)packetscount, 
                stas_packets_sent,
                (long long unsigned int)acked_seq,
                LIST_LEN(send_queue),
                (long long unsigned int)next_p_seq);
            printf("rcv queue: %d enqueued %d discarted[dup %d, siz %d] ack_queue[%d]\n", 
                LIST_LEN(queue),
                stats_enqueued,
                stats_already_in_queue,
                stats_truncated,
                LIST_LEN(ack_queue));

            stats_enqueued = 0;
            stats_already_in_queue = 0;
            packetscount = 0;
            stats_truncated = 0;
            stas_packets_sent = 0;

            timeradd(&curr_time, &stats_interval, &stats_time);
        
            flush_ack_queue(&curr_time, ack_queue);
        }
        
        if(num_fds == 0)
        {
            if(timercmp(&curr_time, &gflood_time, >))
            {
                timeradd(&curr_time, &gflood_interval, &gflood_time);
                proc_send_queue( send_queue, 
                            &send_queue_ptr, transmisor, 
                            ack_queue, &ack_queue_ptr);
            }
            
            continue;
        }

        /* Check if pending TCP connection to accept and create a new client
           and UDP connection if one is ready */
        if(FD_ISSET(SOCK_FD(tcp_serv), &read_fds))
        {
            tcp_sock = sock_accept(tcp_serv);
            if(tcp_sock == NULL)
                continue;

            if(debug_level >= DEBUG_LEVEL1)
                printf("accepted new tcp conn...\n");

            client = client_create(next_client_id++, tcp_sock, transmisor, 1);
            if(!client || !tcp_sock)
            {
                if(tcp_sock)
                    sock_close(tcp_sock);
            }
            else
            {
                client2 = list_add(conn_clients, client, 1, 0);
                client_free(client);
                client = NULL;
                
                client_send_hello(client2, rhost, rport, CLIENT_ID(client2), send_queue);
                client_add_tcp_fd_to_set(client2, &client_fds);
            }
            
            sock_free(tcp_sock);
            tcp_sock = NULL;

            num_fds--;
        }

        /* Get any data received on the UDP socket */
        if(FD_ISSET(SOCK_FD(udp_sock), &read_fds))
        {
            msg_hdr_t * header = NULL;

            ret = sock_recv(udp_sock, udp_from, data, sizeof(data));

            packetscount += 1;

            tmp_len = ret;

            if (ret < sizeof(msg_hdr_t))
                ret = -1; //invalid size

            if (ret > 0)
                header = (msg_hdr_t *) data;

            if ((ret > 0) && (tmp_len < header->length + sizeof(msg_hdr_t))) //invalid size
            {
                ret = -1;
                stats_truncated += 1 ;
            }

            if (ret > 0)
            {
                if (header->seq_id == 0)
                    ret = -1;
                if ((header->type != 0) && (header->seq_id != 0))
                {
                    add_to_ack_queue(ack_queue, header->seq_id, &curr_time);
                    fresh_ack ++;
                    if (fresh_ack == MSG_MAX_LEN / 8)
                    {
                        queue_ack_page(send_queue, ack_queue);
                        fresh_ack = 0;
                    }
                }
                if (header->seq_id <= acked_seq)
                    ret = -1;

                //process the remote acks

                remote_lastack = header->ack_id;

                remove_from_send_queue(header->ack_id, send_queue, ack_queue);

                if (header->type == 0) //garbage packet
                {
                    ret = -1;
                }
            }

            if (ret > 0) 
                if (debug_level >= DEBUG_LEVEL3)
                printf("udp socket has message cid:%llu sid:%lld ack:%lld\n", 
                    (long long unsigned int)header->client_id, 
                    (long long int)header->seq_id,
                    (long long int)header->ack_id
                    );

            if ((ret > 0) && (acked_seq == 0))
            {
                acked_seq = header->seq_id;
                printf("setting starting seq at: %llu\n", (long long unsigned int)acked_seq);
            }

            if (ret > 0)
            {
                if (header->type == MSG_TYPE_ACK_PAGE)
                {
                    uint64_t * pptr = (uint64_t *)(data + sizeof(msg_hdr_t));
                    int n = 0;
                    int found = 0;
                    for(n=0;n<header->length / 8; n++)
                    {
                        found += remove_from_send_queue(*pptr, send_queue, ack_queue);
                        pptr ++ ;
                    }
                }
                if (header->seq_id > acked_seq + 1)
                {
                    int already_have = 0;
                    for(j=0; j<LIST_LEN(queue);j++)
                    {
                        data_buf_t * buf = list_get_at(queue, j);
                        if (buf->header.seq_id == header->seq_id)
                        {
                            already_have = 1;
                        }
                    }

                    if (already_have == 0)
                    {
                        if(debug_level >= DEBUG_LEVEL3)
                            printf("queuing %llu\n", (long long unsigned int)header->seq_id);

                        data_buf_t buf;
                        buf.header.client_id = header->client_id;
                        buf.header.length = header->length;
                        buf.header.seq_id = header->seq_id;
                        buf.header.type = header->type;
                        memcpy(buf.buf, data + sizeof(msg_hdr_t), buf.header.length);
                        list_add(queue, &buf, 1, 0);
                        ret = 0; //enqueued

                        stats_enqueued += 1;
                    }
                    else {
                        stats_already_in_queue += 1;
                    }
                }
                else
                {
                    if(debug_level >= DEBUG_LEVEL3)
                        printf("procesing %llu\n", (long long unsigned int)header->seq_id);

                    proc_packet(data, clients, &client_fds, conn_clients, send_queue);

                    acked_seq = header->seq_id;

                    //sort it for god sake

                    int something_clicked = 1;
                    while (something_clicked != 0)
                    {
                        something_clicked = 0;
                        for(j=0; j<LIST_LEN(queue);j++)
                        {
                            char * bucket = list_get_at(queue, j);
                            msg_hdr_t * h_bucket = (msg_hdr_t *) bucket;
                            if (h_bucket->seq_id == acked_seq + 1)
                            {
                                proc_packet(bucket, clients, &client_fds, conn_clients, send_queue);

                                acked_seq = h_bucket->seq_id;

                                something_clicked = 1;
                            }
                            else {
                                break;
                            }
                        }
                    }
                    for(j=0; j<LIST_LEN(queue);j++)
                    {
                        data_buf_t * buf = list_get_at(queue, j);
                        if (buf->header.seq_id <= acked_seq)
                        {
                            list_delete_at(queue, j);
                            j--;
                        }
                    }
                }
            }

            num_fds--;
        }

        /* Check if data is ready from any of the clients */
        for(i = 0; i < LIST_LEN(clients); i++)
        {
            client = list_get_at(clients, i);

            /* Check for TCP data */
            if(num_fds > 0 && client_tcp_fd_isset(client, &read_fds))
            {
                ret = 0;

                while (LIST_LEN(queue) < MAX_TCP_QUEUE)
                {
                    ret = client_recv_tcp_data(client, send_queue);
                    if (ret < 1)
                        break;
                }

                if(ret == -1)
                {
                    disconnect_and_remove_client(CLIENT_ID(client), clients,
                                                 &client_fds, 1, send_queue);
                    i--;
                    continue;
                }
                else if(ret == -2)
                {
                    client_mark_to_disconnect(client);
                    disconnect_and_remove_client(CLIENT_ID(client),
                                                 clients, &client_fds, 0, send_queue);
                }

                num_fds--;
            }
        }

        if(timercmp(&curr_time, &gflood_time, >))
        {
            timeradd(&curr_time, &gflood_interval, &gflood_time);
            proc_send_queue(
                    send_queue, 
                    &send_queue_ptr, transmisor, 
                    ack_queue, &ack_queue_ptr);
        }
    }
    
  done:
    if(debug_level >= DEBUG_LEVEL1)
        printf("Cleaning up...\n");
    if(tcp_serv)
    {
        sock_close(tcp_serv);
        sock_free(tcp_serv);
    }
    if(udp_sock)
    {
        sock_close(udp_sock);
        sock_free(udp_sock);
    }
    if(clients)
        list_free(clients);
    if(conn_clients)
        list_free(conn_clients);
    if(debug_level >= DEBUG_LEVEL1)
        printf("Goodbye.\n");
    return 0;
}

int proc_packet(char * pdata, list_t * clients, fd_set * client_fds, list_t * conn_clients, list_t * send_queue)
{
    int i;
    int ret = -1;
    client_t * client;

    msg_hdr_t * header = (msg_hdr_t *) pdata;
    char * data = pdata + sizeof(msg_hdr_t);

    if (header->type == MSG_TYPE_HELLOACK)
    {
        for(i = 0; i < LIST_LEN(conn_clients); i++)
        {
            uint64_t tmp_req_id = * (uint64_t *) (data);
            if(debug_level >= DEBUG_LEVEL3)
                printf("MSG_TYPE_HELLOACK(%llu) %lu\n", (long long unsigned int)tmp_req_id, sizeof(msg_hdr_t));                
            client = list_get_at(conn_clients, i);
            if (client->id == tmp_req_id)
            {
                ret = c_handle_message(client, header->client_id, header->type, data, header->length, send_queue);
                if(ret < 0)
                {
                    disconnect_and_remove_client(header->client_id, conn_clients,
                                                 client_fds, 1, send_queue);
                    break;
                }
                else
                {
                    client = list_add(clients, client, 1, 0);
                    list_delete_at(conn_clients, i);
                    break;
                }
            }
        }
        return ret;
    }

    for(i = 0; i < LIST_LEN(clients); i++)
    {
        client = list_get_at(clients, i);
        if (client->id == header->client_id)
        {
            ret = c_handle_message(client, header->client_id, header->type, data, header->length,
                 send_queue);
            if(ret < 0)
                disconnect_and_remove_client(header->client_id, clients, client_fds, 1, send_queue);

            return 0;
        }
    }

    return ret;
}

/*
 * Closes the TCP and UDP connections for the client and remove its stuff from
 * the lists.
 */
void disconnect_and_remove_client(uint64_t id, list_t *clients,
                                  fd_set *fds, int full_disconnect, list_t * send_queue)
{
    client_t *c;

    c = list_get(clients, &id);
    if(!c)
        return;

    client_remove_tcp_fd_from_set(c, fds);
    client_disconnect_tcp(c);

    if(full_disconnect)
    {
        client_send_goodbye(c, send_queue);

        if(debug_level >= DEBUG_LEVEL1)
            printf("Client %llu disconnected.\n", (long long unsigned int)CLIENT_ID(c));

        list_delete(clients, &id);
    }
}


/*
        // Check for pending handshakes from UDP connection 
        for(i = 0; i < LIST_LEN(conn_clients) && num_fds > 0; i++)
        {
            client = list_get_at(conn_clients, i);
            
            if(client_udp_fd_isset(client, &read_fds))
            {
                num_fds--;
                tmp_req_id = CLIENT_ID(client);

                ret = client_recv_udp_msg(client, data, sizeof(data),
                                          &tmp_id, &tmp_type, &tmp_len);
                if(ret == 0)
                    ret = handle_message(client, tmp_id, tmp_type,
                                         data, tmp_len);

                if(ret < 0)
                {
                    disconnect_and_remove_client(tmp_req_id, conn_clients,
                                                 &client_fds, 1);
                    i--;
                }
                else
                {
                    client = list_add(clients, client, 1);
                    list_delete_at(conn_clients, i);
                    i--;
                }
            }
        }
*/

/*
 * Handles a message received from the UDP tunnel. Returns 0 if successful, -1
 * on some error it handled, or -2 if the client is to disconnect.
 */
int c_handle_message(client_t *c, uint64_t id, uint8_t msg_type,
                   char *data, int data_len, list_t * send_queue)
{
    int ret = 0;
    char addrstr[ADDRSTRLEN];
    
    switch(msg_type)
    {
        case MSG_TYPE_GOODBYE:
            ret = -2;
            break;
            
        case MSG_TYPE_HELLOACK:
            CLIENT_ID(c) = id;


            if(debug_level >= DEBUG_LEVEL1)
            {
                sock_get_str(c->tcp_sock, addrstr, sizeof(addrstr));
                printf("New connection(%llu): tcp://%s", 
                        (long long unsigned int)CLIENT_ID(c), 
                        addrstr);
                sock_get_str(c->udp_sock, addrstr, sizeof(addrstr));
                printf(" -> udp://%s\n", addrstr);
            }

            client_send_helloack(c, id, send_queue);
            break;
            
        case MSG_TYPE_DATA0:
        case MSG_TYPE_DATA1:
            //enqueue this to the tcp socket
                ret = sock_send(c->tcp_sock, data, data_len);
                if(debug_level >= DEBUG_LEVEL3)
                {
                    printf("relaying data, %d / %d\n", ret, data_len);
                    print_hexdump(data, data_len);
                }
                if(ret < 0)
                    return -1;
                else if(ret == 0)
                    return -2;
                else    
                    return 0;
            break;
            
        default:
            ret = -1;
            break;
    }

    return ret;
}

void signal_handler(int sig)
{
    switch(sig)
    {
        case SIGINT:
            running = 0;
    }
}
