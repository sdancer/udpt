#ifndef SENDQUEUE_H
#define SENDQUEUE_H

extern uint64_t next_req_id;
extern int stas_packets_sent;
extern int options_pps;
extern int sendqueue_resets;
int ptr_cmp(const void *c1, const void *c2, size_t len);

void proc_send_queue(
                list_t * send_queue, 
                int * send_queue_ptr, socket_t * transmisor,
                list_t * ack_queue, int * ack_queue_ptr);

void clean_send_queue_to(uint64_t ack_id, list_t * send_queue);
int remove_from_send_queue(uint64_t ack_id, list_t * send_queue, list_t * ack_queue);

void queue_ack_page(list_t * send_queue, list_t * ack_queue);

void flush_ack_queue(struct timeval * curr_time, list_t * ack_queue);
void add_to_ack_queue(list_t * ack_queue, uint64_t seq_id, struct timeval * curr_time);
void out_of_band_ack(socket_t * transmisor, uint64_t ack_id);

struct ack_record
{
    uint64_t seq_id;
    struct timeval lastseen;
    int sentcount;
    uint64_t ack_page_id;
};
typedef struct ack_record ack_record_t;

#endif