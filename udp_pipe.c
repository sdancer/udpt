/*
 * Project: udptunnel
 * File: udpserver.c
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

#ifndef WIN32
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#else
#include "helpers/winhelpers.h"
#endif

#include <stdio.h>
#include <execinfo.h>
#include "common.h"
#include "list.h"
#include "client.h"
#include "message.h"
#include "socket.h"
#include "acl.h"
#include "sendqueue.h"
#include <ev.h>



typedef struct tmaindata {
    struct timeval curr_time;

    struct timeval reset_time;
    struct timeval reset_interval;

    struct timeval stats_time;
    struct timeval stats_interval;
    long ticks;

    list_t *send_queue;

    int stats_enqueued;
    int stats_already_in_queue;
    int stats_truncated ;

    int packetscount;

    uint64_t acked_seq;
    int ack_queue_ptr;
    list_t *ack_queue;

    list_t *queue;

    uint64_t remote_lastack;

    socket_t * transmisor;

    socket_t *udp_sock;
    socket_t *udp_from;

    list_t *clients;
    list_t *acls;

    //int send_queue_ptr = 0; not used?
    int send_queue_ptr;

    int fresh_ack;
};

extern int debug_level;
extern int ipver;
static int next_client_id = 1;

static int MAX_TCP_QUEUE = 4096;

/* internal functions */
static void signal_handler(int sig);

static int c_handle_message(client_t * c, uint8_t msg_type, char *data, int data_len,
                   struct tmaindata * maindata);

static int proc_packet(char * pdata, struct tmaindata * maindata);

static void disconnect_and_remove_client(uint64_t id, struct tmaindata * maindata, int full_disconnect);


ev_timer timeout_watcher;
struct ev_loop * mainloop;

struct tmaindata maindata;

static void timeout_cb (EV_P_ struct ev_timer *w, int revents)
{
    maindata.ticks += 1;

    struct timeval curr_time;
    gettimeofday(&curr_time, NULL);

    if(timercmp(&curr_time, &maindata.reset_time, >))
    {
        sendqueue_resets = 0;
        timeradd(&curr_time, &maindata.reset_interval, &maindata.reset_time);
    }

    if(timercmp(&curr_time, &maindata.stats_time, >))
    {
        sendqueue_resets = 0;        

        uint64_t next_p_seq = 0;
        if (LIST_LEN(maindata.send_queue) > 0)
        {
            data_buf_t * element = (data_buf_t *) list_get_at(maindata.send_queue, 0);
            next_p_seq = element->header.seq_id;
        }

        printf("stats = udp packets: %llu | %d /s lastack %llu queue(%d) next seq: %lld\n", 
            (long long unsigned int)maindata.packetscount, 
            stas_packets_sent,
            (long long unsigned int)maindata.acked_seq,
            LIST_LEN(maindata.send_queue),
            (long long unsigned int)next_p_seq);
        printf("queue len = %d ackq len = %d\n",
            LIST_LEN(maindata.queue),
            LIST_LEN(maindata.ack_queue));
        maindata.packetscount = 0;
        stas_packets_sent = 0;
        maindata.stats_truncated = 0;

        timeradd(&curr_time, &maindata.stats_interval, &maindata.stats_time);

        printf ("timeout %d\r\n", maindata.ticks);
        maindata.ticks = 0;
    }

    proc_send_queue(
                maindata.send_queue, 
                &maindata.send_queue_ptr, maindata.transmisor, 
                maindata.ack_queue, &maindata.ack_queue_ptr);
}

static void udp_cb(socket_t * sock, int revents)
{
    gettimeofday(&maindata.curr_time, NULL);

    char data[MSG_MAX_LEN + sizeof(msg_hdr_t)];

    /* Get any data received on the UDP socket */
    msg_hdr_t * header = NULL;

    int ret = sock_recv(maindata.udp_sock, maindata.udp_from, data, sizeof(data));
    SIN(&maindata.transmisor->addr)->sin_addr = SIN(&maindata.udp_from->addr)->sin_addr ; 
      
    memcpy(&(maindata.transmisor->addr), &( maindata.udp_from->addr), maindata.udp_from->addr_len);

    maindata.packetscount += 1;

    int tmp_len = ret;

    if (ret < (int)sizeof(msg_hdr_t))
        ret = -1; //invalid size

    if (ret > 0)
        header = (msg_hdr_t *) data;

    if ((ret > 0) && (tmp_len < header->length + sizeof(msg_hdr_t))) //invalid size
    {
        if (header->type == MSG_TYPE_ACK_PAGE)
            printf("truncated ack page\n");
        maindata.stats_truncated += 1;
        ret = -1;
    }

    if (ret > 0)
    {
        if (header->seq_id <= maindata.acked_seq)
            ret = -1;

        if ((header->type == MSG_TYPE_ACK_PAGE_PARTIAL))
        {
            uint64_t * pptr = (uint64_t *)(data + sizeof(msg_hdr_t));
            int n = 0;
            int found = 0;
            for(n=0;n<header->length / 8; n++)
            {
                found += remove_from_send_queue(*pptr, maindata.send_queue, maindata.ack_queue);
                pptr ++ ;
            }
            /*
            if (found > 0)
            {
                const char * msgtype = "unknown";
                if (header->type == MSG_TYPE_ACK_PAGE)
                    msgtype = "MSG_TYPE_ACK_PAGE";
                if (header->type == MSG_TYPE_ACK_PAGE)
                    msgtype = "MSG_TYPE_ACK_PAGE_PARTIAL";
                printf("%s page processed %lld found = %d\n", 
                    msgtype,
                    (long long int)header->seq_id, found);
            }
            */
        }

        if ((header->type == MSG_TYPE_ACK_PAGE_PARTIAL))
        {
            uint64_t * pptr = (uint64_t *)(data + sizeof(msg_hdr_t));
            int n = 0;
            int found = 0;
            for(n=0;n<header->length / 8; n++)
            {
                found += remove_from_send_queue(*pptr, maindata.send_queue, maindata.ack_queue);
                pptr ++ ;
            }
            /*
            if (found > 0)
            {
                const char * msgtype = "unknown";
                if (header->type == MSG_TYPE_ACK_PAGE)
                    msgtype = "MSG_TYPE_ACK_PAGE";
                if (header->type == MSG_TYPE_ACK_PAGE)
                    msgtype = "MSG_TYPE_ACK_PAGE_PARTIAL";
                printf("%s page processed %lld found = %d\n", 
                    msgtype,
                    (long long int)header->seq_id, found);
            }
            */
        }

        if ((header->type != 0) && (header->seq_id != 0))
        {
            add_to_ack_queue(maindata.ack_queue, header->seq_id, &maindata.curr_time);
            maindata.fresh_ack ++;
            if (maindata.fresh_ack == MSG_MAX_LEN / 8)
            {
                queue_ack_page(maindata.send_queue, maindata.ack_queue);
                maindata.fresh_ack = 0;
            }
        }

        if (header->seq_id == 0)
            ret = -1;

        //process the remote acks

        maindata.remote_lastack = header->ack_id;

        remove_from_send_queue(header->ack_id, maindata.send_queue, maindata.ack_queue);

        if (header->type == 0) //garbage packet
            ret = -1;
    }

    
    if (ret > 0) 
        if (debug_level >= DEBUG_LEVEL3)
        printf("udp socket has message cid:%llu sid:%lld ack:%lld\n", 
            (long long unsigned int)header->client_id, 
            (long long int)header->seq_id,
            (long long int)header->ack_id
            );

    if ((ret > 0) && (maindata.acked_seq == 0))
    {
        maindata.acked_seq = header->seq_id;
        printf("setting starting seq at: %llu\n", (long long unsigned int)maindata.acked_seq);
    }
    if (ret > 0)
    {
        //should be out of band...
        if (header->type == MSG_TYPE_ACK_PAGE)
        {
            uint64_t * pptr = (uint64_t *)(data + sizeof(msg_hdr_t));
            int n = 0;
            int found = 0;
            for(n=0;n<header->length / 8; n++)
            {
                found += remove_from_send_queue(*pptr, maindata.send_queue, maindata.ack_queue);
                pptr ++ ;
            }
        }

        if (header->seq_id > maindata.acked_seq + 1)
        {
            int already_have = 0;
            int j;
            for(j=0; j<LIST_LEN(maindata.queue);j++)
            {
                data_buf_t * buf = (data_buf_t *)list_get_at(maindata.queue, j);
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
                list_add(maindata.queue, &buf, 1, 0);
                ret = 0; //enqueued

                maindata.stats_enqueued += 1;
            }
            else {
                maindata.stats_already_in_queue += 1;
            }
        }
        else
        {
            if(debug_level >= DEBUG_LEVEL3)
                printf("procesing %llu\n", (long long unsigned int)header->seq_id);

            proc_packet(data, &maindata);

            maindata.acked_seq = header->seq_id;

            //sort it for god sake

            int something_clicked = 1;
            while (something_clicked != 0)
            {
                something_clicked = 0;
                int j;
                for(j=0; j<LIST_LEN(maindata.queue);j++)
                {
                    data_buf_t * bucket = (data_buf_t *)list_get_at(maindata.queue, j);
                    if (bucket->header.seq_id == maindata.acked_seq + 1)
                    {
                        proc_packet((char*)bucket, &maindata);

                        maindata.acked_seq = bucket->header.seq_id;

                        something_clicked = 1;
                    }
                    else {
                        break;
                    }
                }
            }
            int j;
            for(j=0; j<LIST_LEN(maindata.queue);j++)
            {
                data_buf_t * buf = (data_buf_t *)list_get_at(maindata.queue, j);
                if (buf->header.seq_id <= maindata.acked_seq)
                {
                    list_delete_at(maindata.queue, j);
                    j--;
                }
            }
        }
    }

    /*

        if(ret == 0)
            ret = handle_message(tmp_id, tmp_type, data, tmp_len,
                                 udp_from, clients, &client_fds, acls, transmisor);
        if(ret < 0)
            disconnect_and_remove_client(tmp_id, clients, &client_fds, 1);
        */

}

static void tcp_cb(socket_t * sock, int revents)
{
    //printf("tcp received data\n");

    client_t * client = NULL;
    
    int i = 0;
    for(i = 0; i < LIST_LEN(maindata.clients); i++)
    {
        client_t * t = (client_t*)list_get_at(maindata.clients, i);

        if (t->tcp_sock == sock)
        {
            //printf("tcp sock found\n");
            client = t;
            break;
        }
    }

    if (client == NULL)
    {
        printf("client not found for tcp sock %X\n", sock);
        for(i = 0; i < LIST_LEN(maindata.clients); i++)
        {
            client_t * t = (client_t*)list_get_at(maindata.clients, i);

            printf("client: %X sock %x \n", t, (int)(t->tcp_sock));
        }

        exit(0);
        return;
    }

    int ret = 0;

    while (LIST_LEN(maindata.send_queue) < MAX_TCP_QUEUE)
    {
        ret = client_recv_tcp_data(client, maindata.send_queue);
        if (ret < 1)
            break;
    }

    if(ret == -1)
    {
        disconnect_and_remove_client(CLIENT_ID(client), &maindata, 1);
    }
    else if(ret == -2)
    {
        client_mark_to_disconnect(client);
        disconnect_and_remove_client(CLIENT_ID(client), &maindata, 0);
    }
}

/*
 * UDP Tunnel server main(). Handles program arguments, initializes everything,
 * and runs the main loop.
 */
int udppipe(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    memset(&maindata, 0, sizeof(maindata));

    char * phost;
    char * pport;
    char host_str[ADDRSTRLEN];
    //char remote_host_str[ADDRSTRLEN];
    char port_str[ADDRSTRLEN];
    
    acl_t *tmp_acl;
    
    struct timeval timeout;
    //struct timeval gflood_time;
    //struct timeval gflood_interval;

    if(argc == 1) /* only port specified */
    {
        ERROR_GOTO(!isnum(argv[0]), "invalid port format", done);
        strncpy(port_str, argv[0], sizeof(port_str));
        port_str[sizeof(port_str)-1] = 0;
        host_str[0] = 0;
        argv++;
        argc--;
    }
    else
    {
        ERROR_GOTO(!isnum(argv[0]), "invalid port format", done);
        // first arg is the local port
        ERROR_GOTO(!isipaddr(argv[1], ipver),
                       "invalid IP address format", done);
        // second arg is the remote ip
        ERROR_GOTO(!isnum(argv[2]), "invalid port format", done);
        // third arg is the remote port

        host_str[0] = 0;

        phost = argv[1];
        pport = argv[2];

        /* next arg will be the port */
        ERROR_GOTO(!isnum(argv[0]), "invalid port format", done);
        strncpy(port_str, argv[0], sizeof(port_str));
        port_str[sizeof(port_str)-1] = 0;
        argv++;
        argv++;
        argv++;
        argc--;
        argc--;
        argc--;
    }

    //maindata.transmisor = sock_create(phost, pport, ipver, SOCK_TYPE_UDP, 0, 1);

    /* create empty unsorted acl list */
    maindata.acls = list_create(sizeof(acl_t), NULL, NULL, p_acl_free, 0);
    ERROR_GOTO(maindata.acls == NULL, "creating acl list", done);
/*
    for(i = 0; i < argc; i++)
    {
        tmp_acl = acl_create(argv[i], ipver);
        ERROR_GOTO(tmp_acl == NULL, "creating acl", done);
        list_add(acls, tmp_acl, 0);

        if(debug_level >= DEBUG_LEVEL2)
        {
            printf("adding acl entry: ");
            acl_print(tmp_acl);
        }
    }
*/
    /* add ALLOW ALL entry at end of list */
    tmp_acl = acl_create(ACL_DEFAULT, ipver);
    ERROR_GOTO(tmp_acl == NULL, "creating acl", done);
    list_add(maindata.acls, tmp_acl, 0, 0);

    if(debug_level >= DEBUG_LEVEL2)
    {
        printf("adding acl entry: ");
        acl_print(tmp_acl);
    }
    
    /* Create an empty list for the clients */
    maindata.clients = list_create(sizeof(client_t), p_client_cmp, p_client_copy,
                          p_client_free, 1);
    if(!maindata.clients)
    {
        printf("cant create clients list\n");
        return 0;
    }

    maindata.queue = list_create(sizeof(data_buf_t), ptr_cmp, NULL,
                               NULL, 1);

    maindata.send_queue = list_create(sizeof(data_buf_t), ptr_cmp, NULL,
                               NULL, 0);


    maindata.ack_queue = list_create(sizeof(ack_record_t), NULL, NULL,
                               NULL, 0);

    /* Create the socket to receive UDP messages on the specified port */
    maindata.udp_sock = sock_create(NULL, port_str,
                           ipver, SOCK_TYPE_UDP, 1, 1);
   maindata.transmisor = maindata.udp_sock; //sock_create(phost, pport, ipver, SOCK_TYPE_UDP, 0, 1); 

    SIN(&maindata.transmisor->addr)->sin_addr.s_addr = 0x09090909;

    if(!maindata.udp_sock)
    {
        printf("error creating socket\n");
        return 0;
    }
    
    /* Create empty udp socket for getting source address of udp packets */
    maindata.udp_from = sock_create(NULL, NULL, ipver, SOCK_TYPE_UDP, 0, 0);
    if(!maindata.udp_from)
    {
        printf("cant create udp socket\n");
        return 0;
    }

    timerclear(&timeout);
    
    maindata.reset_interval.tv_sec = 0;
    maindata.reset_interval.tv_usec = 100000;
    gettimeofday(&maindata.reset_time, NULL);

    maindata.stats_interval.tv_sec = 1;
    maindata.stats_interval.tv_usec = 0;
    gettimeofday(&maindata.stats_time, NULL);

    sock_connect(maindata.udp_sock, 1);

    /*
    gflood_interval.tv_sec = 0;
    gflood_interval.tv_usec = 2000;
    gettimeofday(&gflood_time, NULL);
    */

    // use the default event loop unless you have special needs
    mainloop = ev_default_loop (0);

    maindata.udp_sock->read_cb = udp_cb;

    sock_evstart(maindata.udp_sock, mainloop);


    // initialise a timer watcher, then start it
    // simple non-repeating 5.5 second timeout
    ev_timer_init (&timeout_watcher, timeout_cb, 0, 0);
    timeout_watcher.repeat = 0.0001;
    ev_timer_again (mainloop, &timeout_watcher);

    // now wait for events to arrive
    ev_loop (mainloop, 0);

    // unloop was called, so exit
    return 0;
}

/*
 * Closes the client's TCP socket (not UDP, since it is shared) and remove from
 * the fd set. If full_disconnect is set, remove the list.
 */
void disconnect_and_remove_client(uint64_t id, struct tmaindata * maindata, int full_disconnect)
{
    client_t *c;

    if(id == 0)
        return;
    
    c = (client_t*)list_get(maindata->clients, &id);
    if(!c)
        return;

    //ev remove .
    client_disconnect_tcp(c);

    if(full_disconnect)
    {
        client_send_goodbye(c, maindata->send_queue);

        if(debug_level >= DEBUG_LEVEL1)
            printf("Client %lld disconnected.\n", (long long int)CLIENT_ID(c));

        list_delete(maindata->clients, &id);
    }
}

int proc_packet(char * pdata, struct tmaindata * maindata)
{
    int i;
    int ret = -1;
    client_t * client = NULL;

    msg_hdr_t * header = (msg_hdr_t *) pdata;
    char * data = pdata + sizeof(msg_hdr_t);

    if (header->type == MSG_TYPE_HELLO)
    {
        printf("processing msg_hello");
        ret = c_handle_message(NULL, header->type, data, header->length,
            maindata);
        return ret;
    }

    for(i = 0; i < LIST_LEN(maindata->clients); i++)
    {
        client = (client_t*)list_get_at(maindata->clients, i);
        if (client->id == header->client_id)
        {
            ret = c_handle_message(client, header->type, data, header->length, 
                maindata);
            if(ret < 0)
                disconnect_and_remove_client(header->client_id, maindata, 1);

            return ret;
        }
    }

    return ret;
}

/*
 * Handles the message received from the UDP tunnel. Returns 0 for success, -1
 * for some error that it handled, and -2 if the connection should be
 * disconnected.
 */
int c_handle_message(client_t * c, uint8_t msg_type, char *data, int data_len,
                   struct tmaindata * maindata)
{
    if(debug_level >= DEBUG_LEVEL3)
        printf("got a message type %d\n", msg_type);

    client_t *c2 = NULL;
    socket_t *tcp_sock = NULL;
    int ret = 0;
    
    if((c == NULL) && (msg_type != MSG_TYPE_HELLO))
        return -2;

    if(debug_level >= DEBUG_LEVEL3)
        printf("got a message type %d\n", msg_type);
   
    switch(msg_type)
    {
        case 0:
            return 0;

        case MSG_TYPE_GOODBYE:
            ret = -2;
            break;
            
        /* Data in the hello message will be like "hostname port", possibly
           without the null terminator. This will look for the space and
           parse out the hostname or ip address and port number */
        case MSG_TYPE_HELLO:
        {

            int i;
            char port[6]; /* need this so port str can have null term. */
            char src_addrstr[ADDRSTRLEN];
            char dst_addrstr[ADDRSTRLEN];
            uint16_t sport, dport;
            uint64_t req_id;
            
            req_id = *((uint64_t*)data);
            data += sizeof(req_id);
            data_len -= sizeof(req_id);
            
            /* look for the space separating the host and port */
            for(i = 0; i < data_len; i++)
                if(data[i] == ' ')
                    break;
            if(i == data_len)
            {
                printf("invalid ip/port\n");
                break;
            }
            /* null terminate the host and get the port number to the string */
            data[i++] = 0;
            strncpy(port, data+i, data_len-i);
            port[data_len-i] = 0;
            
            /* Create an unconnected TCP socket for the remote host, the
               client itself, add it to the list of clients */
            tcp_sock = sock_create(data, port, ipver, SOCK_TYPE_TCP, 0, 0);
            ERROR_GOTO(tcp_sock == NULL, "Error creating tcp socket", error);

            c = client_create(next_client_id++, tcp_sock, maindata->transmisor, 0);
            sock_free(tcp_sock);
            ERROR_GOTO(c == NULL, "Error creating client", error);

            c2 = (client_t*)list_add(maindata->clients, c, 1, 0);
            printf("created client id = %lld\n", (long long int)c2->id);
            ERROR_GOTO(c2 == NULL, "Error adding client to list", error);

            sock_get_addrstr(CLIENT_UDP_SOCK(c2), src_addrstr,
                             sizeof(src_addrstr));
            sock_get_addrstr(CLIENT_TCP_SOCK(c2), dst_addrstr,
                             sizeof(dst_addrstr));
            sport = sock_get_port(CLIENT_UDP_SOCK(c2));
            dport = sock_get_port(CLIENT_TCP_SOCK(c2));
            
            /*
            for(i = 0; i < LIST_LEN(acls); i++)
            {
                ret = acl_action(list_get_at(acls, i), src_addrstr, sport,
                                 dst_addrstr, dport);

                if(ret == ACL_ACTION_ALLOW)
                {
                    if(debug_level >= DEBUG_LEVEL2)
                        printf("Connection %s:%hu -> %s:%hu allowed\n",
                               src_addrstr, sport, dst_addrstr, dport);
                    break;
                }
                else if(ret == ACL_ACTION_DENY)
                {
                    if(debug_level >= DEBUG_LEVEL2)
                        printf("Connection to %s:%hu -> %s:%hu denied\n",
                               src_addrstr, sport, dst_addrstr, dport);

                    msg_send_msg(from, next_client_id, MSG_TYPE_GOODBYE,
                                 NULL, 0);
                    client_free(c);
                    return -2;
                }
            }
            */
            if(debug_level >= DEBUG_LEVEL1)
            {
                sock_get_str(CLIENT_UDP_SOCK(c2), src_addrstr,
                             sizeof(src_addrstr));
                sock_get_str(CLIENT_TCP_SOCK(c2), dst_addrstr,
                             sizeof(dst_addrstr));
                printf("New connection(%lld): udp://%s:%d -> tcp://%s:%d\n",
                       (long long int)CLIENT_ID(c2), src_addrstr, sport, dst_addrstr, dport);
            }
            
            /* Send the Hello ACK message if created client successfully */
            client_send_helloack(c2, req_id, maindata->send_queue);
            //client_reset_keepalive(c2);
            client_free(c);
            
            break;
        }

        /* Can connect to TCP connection once received the Hello ACK */
        case MSG_TYPE_HELLOACK:
            if(client_connect_tcp(c) != 0)
            {
                printf("MSG_TYPE_HELLOACK: client_connect_tcp(c) != 0 \n");
                return -2;
            }
            //client_got_helloack(c);
            printf("MSG_TYPE_HELLOACK: setting fds\n");
            c->tcp_sock->read_cb = tcp_cb;
            sock_evstart(c->tcp_sock, mainloop);
            break;

        /* Resets the timeout of the client's keep alive time */
        case MSG_TYPE_KEEPALIVE:
            //client_reset_keepalive(c);
            break;

        /* Receives the data it got from the UDP tunnel and sends it to the
           TCP connection. */
        case MSG_TYPE_DATA0:
        case MSG_TYPE_DATA1:

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

        /* Receives the ACK from the UDP tunnel to set the internal client
           state. */
        case MSG_TYPE_ACK0:
        case MSG_TYPE_ACK1:
            //client_got_ack(c, msg_type);
            break;

        default:
            ret = -1;
    }

    return ret;
}

void signal_handler(int sig)
{
    switch(sig)
    {
        case SIGINT:
            break;
    }
}
