#ifndef  TC_USER_INCLUDED
#define  TC_USER_INCLUDED

#include <xcopy.h>
#include <burn.h>

typedef struct frame_s {
    uint64_t interval;
    uint32_t seq;
    struct frame_s *next;
    struct frame_s *prev;
    unsigned int    belong_to_the_same_req:1;
    unsigned int    frame_len:17;
    unsigned char  *frame_data;
}frame_t;

typedef struct sess_data_s {
    frame_t *first_frame;
    frame_t *last_frame;
    uint32_t last_ack_seq;
    uint32_t frames;
    uint16_t orig_src_port;
    unsigned int end:1;
    unsigned int has_req:1;
    unsigned int status:16;
}sess_data_t, *p_sess_data_t;

typedef struct sess_entry_s{
    uint64_t key;
    sess_data_t data;
    struct sess_entry_s* next;
}sess_entry_t,*p_sess_entry;

typedef struct sess_table_s{                                                                           
    int size;
    int num_of_sess;
    p_sess_entry* entries;
}sess_table_t;

typedef struct tc_user_state_s{
    uint32_t status:16;
    uint32_t closed_pattern:8;
    uint32_t over:1;
    uint32_t over_recorded:1;
    uint32_t timestamped:1;
    uint32_t resp_syn_received:1;
    uint32_t resp_waiting:1;
    uint32_t last_ack_recorded:1;
}tc_user_state_t;


typedef struct tc_user_s {
    uint64_t key;
    tc_user_state_t  state;

    uint32_t orig_clt_addr;
    uint32_t src_addr;
    uint32_t dst_addr;

    uint16_t orig_clt_port;
    uint16_t src_port;
    uint16_t dst_port;

    uint16_t wscale;
    uint32_t last_seq;
    uint32_t last_ack_seq;
    uint32_t history_last_ack_seq;
    uint32_t exp_seq;
    uint32_t exp_ack_seq;
    
    uint32_t fast_retransmit_cnt:6;

    uint32_t ts_ec_r;
    uint32_t ts_value; 

    uint32_t srv_window;
    uint32_t total_packets_sent;

#if (TC_PCAP_SEND)
    unsigned char *src_mac;
    unsigned char *dst_mac;
#endif

    sess_data_t *orig_sess;
    frame_t        *orig_frame;
    frame_t        *orig_unack_frame;

    time_t   last_sent_time;
    long     last_recv_resp_cont_time;

}tc_user_t;

typedef struct tc_user_index_s {
    int index;
}tc_user_index_t;


int tc_build_sess_table(tc_pool_t *pool, int size);
bool tc_build_users(int port_prioritized, int num_users, uint32_t *ips,
        int num_ip);

uint64_t tc_get_key(uint32_t ip, uint16_t port);
tc_user_t *tc_retrieve_user(uint64_t key);
void tc_add_sess(p_sess_entry entry);
p_sess_entry tc_retrieve_sess(uint64_t key);

void process_outgress(unsigned char *packet);
void process_ingress();
void output_stat(); 
void tc_interval_dispose(tc_event_timer_t *evt);
void release_user_resources();

#endif   /* ----- #ifndef TC_USER_INCLUDED ----- */

