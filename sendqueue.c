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

#include "common.h"
#include "list.h"
#include "client.h"
#include "message.h"
#include "socket.h"
#include "acl.h"

#include "sendqueue.h"

uint64_t next_req_id = 1;
int stas_packets_sent = 0;
int options_pps = 200;
int sendqueue_resets = 0;

int ptr_cmp(const void *c1, const void *c2, size_t len)
{
    return ((data_buf_t *)c1)->header.seq_id - ((data_buf_t *)c2)->header.seq_id;
}

static randomcounter;

void proc_send_queue(
                list_t * send_queue,
                int * send_queue_ptr, socket_t * transmisor,
                list_t * ack_queue, int * ack_queue_ptr)
{

    //msg_send_msg(transmisor, 0, 0, (char*)&curr_time, sizeof(curr_time), send_queue);

    int msglen = MSG_MAX_LEN + sizeof(msg_hdr_t);
    //wire_msg
    char sdata[msglen];

    msg_hdr_t * pheader = (msg_hdr_t *)sdata;

    if (LIST_LEN(send_queue) == 0)
    {
        if (sendqueue_resets > 0)
            return;
        sendqueue_resets += 1;

        int maxn = MSG_MAX_LEN / 8;
        if (maxn > LIST_LEN(ack_queue))
            maxn = LIST_LEN(ack_queue);

        //the next msg is MSG_TYPE_DISCART
        pheader->p_unique = randomcounter++;
        pheader->type = MSG_TYPE_ACK_PAGE_PARTIAL;
        pheader->client_id = randomcounter++;
        pheader->seq_id = 0;

        int partialcount = 0;

        uint64_t * pptr = (uint64_t *)(sdata + sizeof(msg_hdr_t));
        int i;
        for (i=LIST_LEN(ack_queue) - maxn; i<LIST_LEN(ack_queue); i++)
        {
            ack_record_t * element = list_get_at(ack_queue, i);
            if (element->ack_page_id == 0)
            {
                partialcount ++;
                * pptr = element->seq_id;
                pptr ++;
            }
        }

        pheader->length = partialcount * 8;
    }
    else
    {
        if ((* send_queue_ptr >= LIST_LEN(send_queue)) || (* send_queue_ptr > 600))
        {
            if (sendqueue_resets > 0)
                return;
            (*send_queue_ptr) = 0;
            sendqueue_resets += 1;
        }

        data_buf_t * queued_element = list_get_at( send_queue, * send_queue_ptr);

        pheader->p_unique = randomcounter++;
        pheader->type = queued_element->header.type;
        pheader->length = queued_element->header.length;
        pheader->client_id = queued_element->header.client_id;
        pheader->seq_id = queued_element->header.seq_id;
        memcpy(sdata + sizeof(msg_hdr_t), queued_element->buf, queued_element->header.length);

        (*send_queue_ptr) ++;
    }

    pheader->ack_id = 0;
    if (LIST_LEN(ack_queue) > 0)
    {
        ack_record_t * first_low = list_get_at(ack_queue, 0);
        int i;
        for (i =1; i<LIST_LEN(ack_queue); i++)
        {
            ack_record_t * titem = list_get_at(ack_queue, i);
            if (titem->sentcount < first_low->sentcount)
            {
                first_low = titem;
            }
        }
        pheader->ack_id = first_low->seq_id;
        (first_low->sentcount)++;
    }

    sock_send(transmisor, sdata, MSG_MAX_LEN + sizeof(msg_hdr_t));

    stas_packets_sent += 1;
}

void out_of_band_ack(socket_t * transmisor, uint64_t ack_id)
{
    char sdata[sizeof(msg_hdr_t)];
    msg_hdr_t * pheader = (msg_hdr_t *)sdata;

    pheader->type = MSG_TYPE_ACK_OOB;
    pheader->length = 0;
    pheader->client_id = 0;
    pheader->seq_id = 0;
    pheader->ack_id = ack_id;

    sock_send(transmisor, sdata, sizeof(msg_hdr_t));
}

void queue_ack_page(list_t * send_queue, list_t * ack_queue)
{
    if (LIST_LEN(ack_queue) < 1)
        return;

    int i;
    int queuedelements = 0;
    uint64_t thispage = next_req_id++;

    data_buf_t buf;

    buf.header.seq_id = thispage;
    buf.header.client_id = 0;
    buf.header.type = MSG_TYPE_ACK_PAGE;
    uint64_t * pptr = (uint64_t *)(&(buf.buf));

    for(i =0 ; i<LIST_LEN(ack_queue);i++)
    {
        ack_record_t * item = list_get_at(ack_queue, i);
        if(item->ack_page_id == 0)
        {
            item->ack_page_id = thispage;
            * pptr = item->seq_id;
            pptr ++;
            queuedelements ++;
            if (queuedelements == MSG_MAX_LEN / 8)
                break;
        }
    }

    buf.header.length = queuedelements * 8;

    list_add(send_queue, &buf, 1, 0);

    printf("made an ack page of %d elements, id = %lld\n", queuedelements, (long long int)thispage);
}

void flush_ack_page(list_t * ack_queue, uint64_t ack_page_id)
{
    int found = 0;
    int i;
    for(i =0 ; i<LIST_LEN(ack_queue);i++)
    {
        ack_record_t * item = list_get_at(ack_queue, i);
        if(item->ack_page_id == ack_page_id)
        {
            found ++;
            list_delete_at(ack_queue, i);
            i--;
        }
    }

    printf("flushing ack page id = %lld found = %d\n", (long long int)ack_page_id, found);
}

int remove_from_send_queue(uint64_t ack_id, list_t * send_queue, list_t * ack_queue)
{
    int i=0;
    for (i=0;i<LIST_LEN(send_queue);i++)
    {
        data_buf_t * element = list_get_at(send_queue, i);
        if ((element->header.seq_id == ack_id))
        {
            //is ack page?
            if (element->header.type == MSG_TYPE_ACK_PAGE)
            {
                flush_ack_page(ack_queue, element->header.seq_id);
            }

            if(debug_level >= DEBUG_LEVEL3)
                printf("deleting message in queue[%d] %lld\n", i, (long long int)element->header.seq_id);
            list_delete_at(send_queue, i);
            i--;

            return 1;
        }
    }
    return 0;
}

void clean_send_queue_to(uint64_t ack_id, list_t * send_queue)
{
    if(debug_level >= DEBUG_LEVEL3)
        printf("clean queue, start have: %d\n", LIST_LEN(send_queue));
    int i=0;
    for (i=0;i<LIST_LEN(send_queue);i++)
    {
        data_buf_t * element = list_get_at(send_queue, i);
        if ((element->header.seq_id != 0) && (element->header.seq_id <= ack_id))
        {
            if(debug_level >= DEBUG_LEVEL3)
                printf("deleting message in queue[%d] %lld\n", i, (long long int)element->header.seq_id);
            list_delete_at(send_queue, i);
            i--;
        }
    }
    if(debug_level >= DEBUG_LEVEL3)
        printf("clean queue, exit remaining: %d\n", LIST_LEN(send_queue));
}

void flush_ack_queue(struct timeval * curr_time, list_t * ack_queue)
{
    struct timeval flush_interval;
    flush_interval.tv_sec = 5;
    flush_interval.tv_usec = 0;
    struct timeval flush_time;
    timersub(curr_time, &flush_interval, &flush_time);

    int i;
    for(i =0 ; i<LIST_LEN(ack_queue);i++)
    {
        ack_record_t * item = list_get_at(ack_queue, i);
        if(timercmp(&flush_time, &(item->lastseen), >))
        {
            list_delete_at(ack_queue, i);
            i--;
        }
    }
}

void add_to_ack_queue(list_t * ack_queue, uint64_t seq_id, struct timeval * curr_time)
{
    ack_record_t ack_req;
    memset((void*)&ack_req, sizeof(ack_record_t), 0);

    int k = 0;
    int present = 0;

    for (k=0; k<LIST_LEN(ack_queue);k++)
    {
        ack_record_t * ack_req_ = list_get_at(ack_queue, k);
        if (ack_req_->seq_id == seq_id)
        {
            present = 1;
            ack_req_->lastseen = *curr_time;
            return;
        }
    }

    if (present == 0)
    {
        ack_req.seq_id = seq_id;
        ack_req.lastseen = *curr_time;
        ack_req.sentcount = 0;
        ack_req.ack_page_id = 0;
        list_add(ack_queue, &ack_req, 1, 0);
    }
}
