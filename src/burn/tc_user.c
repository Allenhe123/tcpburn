
#include <xcopy.h>
#include <tc_user.h>

static bool    init_phase       = true;
static time_t  record_time      = 0;
static time_t  last_resp_time   = 0;

static int size_of_user_index   = 0;
static int size_of_users        = 0;
static int base_user_seq        = 0;
static int relative_user_seq    = 0;

static tc_pool_t        *g_pool = NULL;
static tc_user_index_t  *user_index_array = NULL;
static tc_user_t        *user_array       = NULL;
static sess_table_t     *s_table          = NULL;

static void send_faked_rst(tc_user_t *u);

static uint32_t 
supplemental_hash(uint32_t value)                                                                 
{
    uint32_t h = 0;
    uint32_t tmp1 = value >> 20;
    uint32_t tmp2 = value >> 12;
    uint32_t tmp3 = tmp1 ^ tmp2;

    h = value ^ tmp3;
    tmp1 = h >> 7;
    tmp2 = h >> 4;
    tmp3 = tmp1 ^ tmp2;
    h= h ^ tmp3;

    return h;
}


static uint32_t table_index(uint32_t h, uint32_t len)
{
    return h & (len - 1);
}

int 
tc_build_sess_table(int size)
{
    g_pool = tc_create_pool(TC_DEFAULT_POOL_SIZE, 0);

    if (g_pool == NULL) {
        return TC_ERROR;
    }

    s_table = (sess_table_t *) tc_pcalloc(g_pool, sizeof(sess_table_t));
    if (s_table == NULL) {
        tc_log_info(LOG_WARN, 0, "calloc error for sess table");
        return TC_ERROR;
    }

    s_table->size = size;
    s_table->entries = (p_sess_entry *) tc_pcalloc(g_pool, 
            size * sizeof(p_sess_entry));

    if (s_table->entries == NULL) {
        tc_log_info(LOG_WARN, 0, "calloc error for sess entries");
        return TC_ERROR;
    }

    return TC_OK;
}


uint64_t 
tc_get_key(uint32_t ip, uint16_t port)
{
    uint64_t ip_l   = (uint64_t) ip;
    uint64_t port_l = (uint64_t) port;
    uint64_t key = (ip_l << 16) + (ip_l << 8) + port_l; 
    return key;
}


void 
tc_add_sess(p_sess_entry entry)
{
    uint32_t h = supplemental_hash((uint32_t) entry->key);
    uint32_t index = table_index(h, s_table->size);
    p_sess_entry e = NULL, last = NULL;

    for(e = s_table->entries[index]; e != NULL; e = e->next) { 
        last = e;
    } 

    if (last == NULL) {
        s_table->entries[index] = entry;
    } else {
        last->next = entry;
    }

    s_table->num_of_sess++;
    tc_log_debug2(LOG_DEBUG, 0, "index:%d,sess in table:%d",
            index, s_table->num_of_sess);
}


static void
tc_init_sess_for_users()
{
    bool            is_find = false;
    int             i, index = 0;
    tc_user_t      *u;
    tc_pool_t      *pool;
    sess_data_t    *sess;
    p_sess_entry    e = NULL, *aux_s_array;

    if (s_table->num_of_sess == 0) {
        tc_log_info(LOG_ERR, 0, "no sess for replay");
        tc_over = 1;
        return;
    }

    pool = tc_create_pool(TC_DEFAULT_POOL_SIZE, 0);

    aux_s_array = (p_sess_entry *) tc_pcalloc(pool, 
            s_table->num_of_sess * sizeof(p_sess_entry));

    if (aux_s_array == NULL) {
        tc_log_info(LOG_ERR, 0, "calloc error for aux_s_array");
        tc_over = 1;
        return;
    }

    e = s_table->entries[index];

    for (i = 0; i < s_table->num_of_sess; i++) {
        if (e == NULL) {
            is_find = false;
            do {
                index = (index + 1) % (s_table->size);
                e = s_table->entries[index];
                while (e != NULL) {
                    sess = &(e->data);
                    if (!sess->has_req) {
                        e = e->next;
                    } else {
                        is_find = true;
                        break;
                    }
                }

                if (is_find) {
                    break;
                }
            } while (e == NULL);
        }
        
        aux_s_array[i] = e;
        e = e->next;
        while (e != NULL) {
            sess = &(e->data);
            if (!sess->has_req) {
                e = e->next;
            } else {
                break;
            }
        }
    }

    index = 0;
    for (i = 0; i < size_of_users; i++) {
        e = aux_s_array[index];
        u = user_array + i;
        u->orig_sess = &(e->data);
        u->orig_frame = u->orig_sess->first_frame;
        u->orig_unack_frame = u->orig_sess->first_frame;
        tc_stat.orig_clt_packs_cnt += u->orig_sess->frames;
        tc_log_debug3(LOG_DEBUG, 0, "index:%d,frames:%u, orig src port:%u", 
                index, u->orig_sess->frames, 
                ntohs(u->orig_sess->orig_src_port));
        index = (index + 1) % s_table->num_of_sess;
    }

    tc_log_info(LOG_NOTICE, 0, 
            "users:%d, sess:%d, total packets needed sent:%llu",
            size_of_users, s_table->num_of_sess, tc_stat.orig_clt_packs_cnt);

    tc_destroy_pool(pool);
}


p_sess_entry 
tc_retrieve_sess(uint64_t key)
{
    uint32_t h = supplemental_hash((uint32_t) key);
    uint32_t index = table_index(h, s_table->size);
    p_sess_entry e = NULL, last = NULL;

    for(e = s_table->entries[index]; e != NULL; e = e->next) { 
        if (e->key == key) {   
            last = e;
        }   
    } 

    return last;
}


static tc_user_t *
tc_retrieve_active_user()
{
    int        total, speed;
    time_t     cur;
    tc_user_t *u; 

    cur = tc_time();

    if (record_time == 0) {
        record_time = cur;
    }

    if (init_phase) {
        total = base_user_seq + relative_user_seq;
        if (total >= size_of_users) {
           tc_log_info(LOG_NOTICE, 0, "total is larger than size of users");
           init_phase = false;
           u = user_array + 0;
           base_user_seq = 1 % size_of_users;
        } else {
            u = user_array + total;
            speed = clt_settings.conn_init_sp_fact;
            relative_user_seq = (relative_user_seq + 1) % speed;

            if (relative_user_seq == 0) {
                if (record_time != cur) {
                    base_user_seq += speed;
                    record_time = cur;
                    tc_log_info(LOG_NOTICE, 0, "change record time");
                    total = total + 1;
                    if (total == size_of_users) {
                        init_phase = false;
                        tc_log_info(LOG_NOTICE, 0, "set init phase false");
                    }
                }
            }
        }
      
    } else {
        u = user_array + base_user_seq;
        base_user_seq = (base_user_seq + 1) % size_of_users;
    }

    return u;
}


tc_user_t *
tc_retrieve_user(uint64_t key)
{
    int      index, i, min, max;

    index = key % size_of_user_index;

    min = user_index_array[index].index;

    if (index == (size_of_user_index -1)) {
        max = size_of_users;
    } else {
        max = user_index_array[index + 1].index;
    }

    tc_log_debug3(LOG_DEBUG, 0, "retrieve user,usr key :%llu,min=%d,max=%d", 
            key, min, max);
    for (i = min; i < max; i++) {
        if (user_array[i].key == key) {
            return user_array + i;
        }
    }

    return NULL;
}


static uint16_t 
get_port(int default_port)
{
    int value;

    if (clt_settings.port_seed) {
        value = (int) ((rand_r(&clt_settings.port_seed) / (RAND_MAX + 1.0)) * 
                VALID_PORTS_NUM);
        value += FIRST_PORT;
    } else {
        value = default_port;
    }

    return htons((uint16_t) value);
}

bool 
tc_build_users(int port_prioritized, int num_users, uint32_t *ips, int num_ip)
{
    int         i, j, k, count, sub_key, slot_avg,
               *stat, *accum, *slot_cnt, *sub_keys;
    uint16_t   *buf_ports, port;
    uint32_t    ip, *buf_ips;
    uint64_t    key, *keys;
    tc_pool_t  *pool;
    
    tc_log_info(LOG_INFO, 0, "enter tc_build_users");

    pool = tc_create_pool(TC_DEFAULT_POOL_SIZE, 0);

    if (pool == NULL) {
        return false;
    }

    size_of_users = num_users;

    slot_avg = SLOT_AVG;
    if (size_of_users < slot_avg) {
        slot_avg = size_of_users;
    }

    size_of_user_index = size_of_users / slot_avg;

    user_array = (tc_user_t *) tc_pcalloc (g_pool, 
            size_of_users * sizeof (tc_user_t));
    if (user_array == NULL) {
        tc_log_info(LOG_WARN, 0, "calloc error for users");
        tc_destroy_pool(pool);
        return false;
    }

    user_index_array = (tc_user_index_t *) tc_pcalloc (g_pool, 
            size_of_user_index * sizeof(tc_user_index_t));

    if (user_array == NULL) {
        tc_log_info(LOG_WARN, 0, "calloc error for users");
        tc_destroy_pool(pool);
        return false;
    }

    count     = 0;
    keys      = (uint64_t *) tc_palloc (pool, sizeof(uint64_t) * size_of_users);
    if (keys == NULL) {
        tc_destroy_pool(pool);
        return false;
    }

    sub_keys  = (int *) tc_palloc (pool, sizeof(int) * size_of_users);
    if (sub_keys == NULL) {
        tc_destroy_pool(pool);
        return false;
    }

    buf_ips   = (uint32_t *) tc_palloc (pool, sizeof(uint32_t) * size_of_users);
    if (buf_ips == NULL) {
        tc_destroy_pool(pool);
        return false;
    }

    buf_ports = (uint16_t *) tc_palloc (pool, sizeof(uint16_t) * size_of_users);
    if (buf_ports == NULL) {
        tc_destroy_pool(pool);
        return false;
    }

    accum = (int *) tc_palloc (pool, sizeof(int) * size_of_users);
    if (accum == NULL) {
        tc_destroy_pool(pool);
        return false;
    }
    
    stat = (int *) tc_palloc (pool, sizeof(int) * size_of_user_index);
    if (stat == NULL) {
        tc_destroy_pool(pool);
        return false;
    }

    slot_cnt  = (int *) tc_palloc (pool, sizeof(int) * size_of_user_index);
    if (slot_cnt == NULL) {
        tc_destroy_pool(pool);
        return false;
    }

    memset(stat, 0 ,sizeof(int) * size_of_user_index);
    memset(slot_cnt, 0 ,sizeof(int) * size_of_user_index);
    memset(sub_keys, 0, sizeof(int) * size_of_users);

    if (port_prioritized) {
        for ( i = 0; i < num_ip; i++) {
            ip = ips[i];
            for (j = FIRST_PORT; j <= LAST_PORT; j++) {
                port = get_port(j);
                key = tc_get_key(ip, port);
                if (count >= size_of_users) {
                    break;
                }

                sub_key = key % size_of_user_index;
                if (stat[sub_key] >= SLOT_MAX) {
                    continue;
                }
                buf_ips[count] = ip;
                buf_ports[count] = port;
                sub_keys[count] = sub_key;
                keys[count++] = key;
                stat[sub_key]++;
            }
        }
    } else {
        for (j = FIRST_PORT; j <= LAST_PORT; j++) {
            port = get_port(j);
            for ( i = 0; i < num_ip; i++) {
                ip = ips[i];

                key = tc_get_key(ip, port);
                if (count >= size_of_users) {
                    break;
                }

                sub_key = key % size_of_user_index;
                if (stat[sub_key] >= SLOT_MAX) {
                    continue;
                }
                buf_ips[count] = ip;
                buf_ports[count] = port;
                sub_keys[count] = sub_key;
                keys[count++] = key;
                stat[sub_key]++;
            }
        }
    }

    if (count < size_of_users) {
        tc_log_info(LOG_ERR, 0, "insuffient ips:%d for creating users:%d", 
                num_ip, size_of_users);
        tc_log_info(LOG_NOTICE, 0, "change users from %d to %d", 
                size_of_users, count); 
        size_of_users = count;
        size_of_user_index = size_of_users / slot_avg;
    }

    user_index_array[0].index = 0;
    for ( i = 1; i < size_of_user_index; i++) {
        user_index_array[i].index = stat[i - 1] + user_index_array[i - 1].index;
    }

    for ( i = 0; i < size_of_users; i++) {
        sub_key = sub_keys[i];
        if (sub_key > 0) {
            accum[i] = user_index_array[sub_key].index  + slot_cnt[sub_key];
        } else {
            accum[i] = slot_cnt[sub_key];

        }

        k = accum[i];
        user_array[k].src_addr = buf_ips[i];
        user_array[k].src_port = buf_ports[i];
        user_array[k].key = keys[i];
        tc_log_debug2(LOG_DEBUG, 0, "usr key :%llu,pos=%d", keys[i], k);

        slot_cnt[sub_key]++;
    }

    tc_destroy_pool(pool);

    tc_init_sess_for_users();

    tc_log_info(LOG_INFO, 0, "leave tc_build_users");

    return true;
}


static bool
send_stop(tc_user_t *u) 
{
    int       send_diff;
    long      app_resp_diff;
    uint32_t  srv_sk_buf_s;

    if (u->orig_frame == NULL) {
        tc_log_debug1(LOG_DEBUG, 0, "orig frame is null :%d", 
                ntohs(u->src_port));
        return true;
    }

    if (u->state.status & SYN_SENT) {
        if (!(u->state.status & SYN_CONFIRM)) {
            tc_log_debug1(LOG_DEBUG, 0, "client wait server handshake:%d", 
                    ntohs(u->src_port));
            return true;
        }
    }

    if (u->state.status & CLIENT_FIN) {
        if (!(u->state.status & SERVER_FIN)) {
            tc_log_debug1(LOG_DEBUG, 0, "client wait server fin:%d", 
                ntohs(u->src_port));
            return true;
        }
    }

    send_diff = tc_time() - u->last_sent_time;
    if (send_diff >= 3) {
        tc_log_debug1(LOG_DEBUG, 0, "timeout, need to send more packet:%d", 
                ntohs(u->src_port));
        u->state.resp_waiting = 0; 
        return false;
    }

    if (u->state.resp_waiting) {
            tc_log_debug1(LOG_DEBUG, 0, "client wait server resp:%d", 
                ntohs(u->src_port));
        return true;
    }

    if (u->state.status & SEND_REQ) {
        if (u->orig_frame->next != NULL) {
            srv_sk_buf_s = u->orig_frame->next->seq - u->orig_frame->seq;
            srv_sk_buf_s = srv_sk_buf_s + u->orig_frame->seq - u->last_ack_seq;
            if (srv_sk_buf_s > u->srv_window) {
                tc_log_debug3(LOG_DEBUG, 0, "wait,srv_sk_buf_s:%u,win:%u,p:%u",
                        srv_sk_buf_s, u->srv_window, ntohs(u->src_port));
                return true;
            }
        }

    }

    app_resp_diff = tc_milliscond_time() - u->last_recv_resp_cont_time;
    if (app_resp_diff <= 10) {
        tc_log_debug1(LOG_DEBUG, 0, "need to wait resp complete:%d", 
                ntohs(u->src_port));
        return true;
    }

    tc_log_debug1(LOG_DEBUG, 0, "last resort, set stop false:%d", 
                ntohs(u->src_port));

    return false;
}


#if (!TC_SINGLE)
static bool
send_router_info(tc_user_t *u, uint16_t type)
{
    int                      i, fd;
    bool                     result = false;
    msg_client_t             msg;
    connections_t           *connections;


    memset(&msg, 0, sizeof(msg_client_t));
    msg.client_ip = u->src_addr;
    msg.client_port = u->src_port;
    msg.type = htons(type);
    msg.target_ip = u->dst_addr;
    msg.target_port = u->dst_port;

    for (i = 0; i < clt_settings.real_servers.num; i++) {

        if (!clt_settings.real_servers.active[i]) {
            continue;
        }

        connections = &(clt_settings.real_servers.connections[i]);
        fd = connections->fds[connections->index];
        connections->index = (connections->index + 1) % connections->num;

        if (fd == -1) {
            tc_log_info(LOG_WARN, 0, "sock invalid");
            continue;
        }

        if (tc_socket_send(fd, (char *) &msg, MSG_CLIENT_SIZE) == TC_ERROR) {
            tc_log_info(LOG_ERR, 0, "fd:%d, msg client send error", fd); 
            if (clt_settings.real_servers.active[i] != 0) {
                clt_settings.real_servers.active[i] = 0;
                clt_settings.real_servers.active_num--;
            }

            continue;
        }
        result = true;
    }                                                                                                             
    return result;
}
#endif


static void
fill_timestamp(tc_user_t *u, tc_tcp_header_t *tcp_header)
{
    uint32_t         timestamp;
    unsigned char   *opt, *p; 

    p   = (unsigned char *) tcp_header;
    opt = p + sizeof(tc_tcp_header_t);
    opt[0] = 1;
    opt[1] = 1;
    opt[2] = 8;
    opt[3] = 10;
    timestamp = htonl(u->ts_value);
    bcopy((void *) &timestamp, (void *) (opt + 4), sizeof(timestamp));
    timestamp = htonl(u->ts_ec_r);
    bcopy((void *) &timestamp, (void *) (opt + 8), sizeof(timestamp));
    tc_log_debug3(LOG_DEBUG, 0, "fill ts:%u,%u,p:%u", 
            u->ts_value, u->ts_ec_r, ntohs(u->src_port));
}


static void 
update_timestamp(tc_user_t *u, tc_tcp_header_t *tcp_header)
{
    uint32_t       ts;
    unsigned int   opt, opt_len;
    unsigned char *p, *end;

    p = ((unsigned char *) tcp_header) + TCP_HEADER_MIN_LEN;
    end =  ((unsigned char *) tcp_header) + (tcp_header->doff << 2);  
    while (p < end) {
        opt = p[0];
        switch (opt) {
            case TCPOPT_TIMESTAMP:
                if ((p + 1) >= end) {
                    return;
                }
                opt_len = p[1];
                if ((p + opt_len) <= end) {
                    ts = htonl(u->ts_ec_r);
                    tc_log_debug2(LOG_DEBUG, 0, "set ts reply:%u,p:%u", 
                            u->ts_ec_r, ntohs(u->src_port));
                    bcopy((void *) &ts, (void *) (p + 6), sizeof(ts));
                    ts = EXTRACT_32BITS(p + 2);
                    if (ts < u->ts_value) {
                        tc_log_debug1(LOG_DEBUG, 0, "ts < history,p:%u",
                                ntohs(u->src_port));
                        ts = htonl(u->ts_value);
                        bcopy((void *) &ts, (void *) (p + 2), sizeof(ts));
                    } else {
                        u->ts_value = ts;
                    }
                }
                return;
            case TCPOPT_NOP:
                p = p + 1; 
                break;
            case TCPOPT_EOL:
                return;
            default:
                if ((p + 1) >= end) {
                    return;
                }
                opt_len = p[1];
                if (opt_len < 2) {
                    tc_log_info(LOG_WARN, 0, "opt len:%d", opt_len);
                    return;
                }
                p += opt_len;
                break;
        }    
    }
    return;
}


#if (TC_PCAP_SEND)
static void
fill_frame(struct ethernet_hdr *hdr, unsigned char *smac, unsigned char *dmac)
{
    memcpy(hdr->ether_shost, smac, ETHER_ADDR_LEN);
    memcpy(hdr->ether_dhost, dmac, ETHER_ADDR_LEN);
    hdr->ether_type = htons(ETH_P_IP); 
}
#endif


static bool 
process_packet(tc_user_t *u, unsigned char *frame) 
{
    bool                    result;
    uint16_t                size_ip, size_tcp, tot_len, cont_len;
    uint32_t                h_ack, h_last_ack;
    tc_ip_header_t         *ip_header;
    tc_tcp_header_t        *tcp_header;
    ip_port_pair_mapping_t *test;

    ip_header  = (tc_ip_header_t *) (frame + ETHERNET_HDR_LEN);
    size_ip    = ip_header->ihl << 2;
    tcp_header = (tc_tcp_header_t *) ((char *) ip_header + size_ip);
    size_tcp = tcp_header->doff << 2;
    tot_len  = ntohs(ip_header->tot_len);
    cont_len = tot_len - size_tcp - size_ip;

    if (u->dst_port == 0) {
        test = get_test_pair(&(clt_settings.transfer), 
                ip_header->daddr, tcp_header->dest);
        if (test == NULL) {
            tc_log_info(LOG_NOTICE, 0, " test null:%u", 
                    ntohs(tcp_header->dest));
            tc_log_trace(LOG_WARN, 0, TO_BAKEND_FLAG, ip_header, tcp_header);
            return false;
        }
        u->dst_addr = test->target_ip;
        u->dst_port = test->target_port;
#if (TC_PCAP_SEND)
        u->src_mac       = test->src_mac;
        u->dst_mac       = test->dst_mac;
#endif
    }

    if (u->state.last_ack_recorded) {
        if (u->state.status < SEND_REQ && (u->state.status & SYN_CONFIRM)) {
            h_ack = ntohl(tcp_header->ack_seq);
            h_last_ack = ntohl(u->history_last_ack_seq);
            if (after(h_ack, h_last_ack)) {
                tc_log_debug1(LOG_DEBUG, 0, "server resp first, wait, p:%u", 
                        ntohs(u->src_port));
                u->state.resp_waiting = 1;
                return false;
            }
        }

    }

    ip_header->saddr = u->src_addr;
    tcp_header->source = u->src_port;
    u->history_last_ack_seq = tcp_header->ack_seq;
    u->state.last_ack_recorded = 1;
    tcp_header->ack_seq = u->exp_ack_seq;
    ip_header->daddr = u->dst_addr;
    tcp_header->dest = u->dst_port;

    tc_log_debug2(LOG_DEBUG, 0, "set ack seq:%u, p:%u",
            ntohl(u->exp_ack_seq), ntohs(u->src_port));

    tc_stat.packs_sent_cnt++;
    if (tcp_header->syn) {
        tc_stat.syn_sent_cnt++;
#if (!TC_SINGLE)
        if (!send_router_info(u, CLIENT_ADD)) {
            return false;
        }
#endif
        u->state.last_ack_recorded = 0;
        u->state.status  |= SYN_SENT;
    } else if (tcp_header->fin) {
        tc_stat.fin_sent_cnt++;
        u->state.status  |= CLIENT_FIN;
    } else if (tcp_header->rst) {
        tc_stat.rst_sent_cnt++;
        u->state.status  |= CLIENT_FIN;
        tc_log_debug1(LOG_DEBUG, 0, "a reset packet to back:%u",
                ntohs(u->src_port));
    }

    if (cont_len > 0) {
        tc_stat.cont_sent_cnt++;
        u->state.status |= SEND_REQ;
    }
    if (u->state.timestamped) {
        update_timestamp(u, tcp_header);
    }

    tcp_header->check = 0;
    tcp_header->check = tcpcsum((unsigned char *) ip_header,
            (unsigned short *) tcp_header, (int) (tot_len - size_ip));
#if (TC_PCAP_SEND)
    ip_header->check = 0;
    ip_header->check = csum((unsigned short *) ip_header,size_ip);
#endif
    tc_log_debug_trace(LOG_DEBUG, 0, TO_BAKEND_FLAG, ip_header, tcp_header);

#if (!TC_PCAP_SEND)
    result = tc_raw_socket_send(tc_raw_socket_out, ip_header, tot_len,
            ip_header->daddr);
#else
    fill_frame((struct ethernet_hdr *) frame, u->src_mac, u->dst_mac);
    result = tc_pcap_send(frame, tot_len + ETHERNET_HDR_LEN);
#endif

    if (result == TC_OK) {
        u->last_sent_time = tc_time();
        return true;
    } else {
        tc_log_info(LOG_ERR, 0, "send to back error,tot_len is:%d,cont_len:%d",
                tot_len,cont_len);
#if (!TCPCOPY_PCAP_SEND)
        tc_raw_socket_out = TC_INVALID_SOCKET;
#endif
        tc_over = SIGRTMAX;
        return false;
    }
}


static
void process_user_packet(tc_user_t *u)
{
    unsigned char   frame[MAX_FRAME_LENGTH];

    if (send_stop(u)) {
        return;
    }

    while (true) {
        if (u->orig_frame->frame_len > MAX_FRAME_LENGTH) {
            tc_log_info(LOG_NOTICE, 0, " frame length may be damaged");
        }

        memcpy(frame, u->orig_frame->frame_data, u->orig_frame->frame_len);
        process_packet(u, frame);
        u->total_packets_sent++;
        u->orig_frame = u->orig_frame->next;


        if (send_stop(u)) {
            break;
        }
        tc_log_debug1(LOG_DEBUG, 0, "check resp waiting:%u",
                ntohs(u->src_port));
        if (!u->orig_frame->belong_to_the_same_req) {
            tc_log_debug2(LOG_DEBUG, 0, "user state:%d,port:%u",
                u->state.status, ntohs(u->src_port));
            if (u->state.status & SYN_CONFIRM) {
                tc_log_debug1(LOG_DEBUG, 0, "set resp waiting:%u",
                        ntohs(u->src_port));
                u->state.resp_waiting = 1;
            }
            break;
        } else {
            tc_log_debug1(LOG_DEBUG, 0, "the same req:%u",  ntohs(u->src_port));
        }
    }
}


static void 
send_faked_rst(tc_user_t *u)
{
    tc_ip_header_t   *ip_header;
    tc_tcp_header_t  *tcp_header;
    unsigned char    *p, frame[FAKE_FRAME_LEN];

    memset(frame, 0, FAKE_FRAME_LEN);
    p = frame + ETHERNET_HDR_LEN;
    ip_header  = (tc_ip_header_t *) p;
    tcp_header = (tc_tcp_header_t *) (p + IP_HEADER_LEN);

    ip_header->version  = 4;
    ip_header->ihl      = IP_HEADER_LEN/4;
    ip_header->frag_off = htons(IP_DF); 
    ip_header->ttl      = 64; 
    ip_header->protocol = IPPROTO_TCP;
    ip_header->tot_len  = htons(FAKE_MIN_IP_DATAGRAM_LEN);
    ip_header->saddr    = u->src_addr;
    ip_header->daddr    = u->dst_addr;
    tcp_header->source  = u->src_port;
    tcp_header->dest    = u->dst_port;
    tcp_header->seq     = u->exp_seq;
    tcp_header->ack_seq = u->exp_ack_seq;
    tcp_header->window  = 65535; 
    tcp_header->ack     = 1;
    tcp_header->rst     = 1;
    tcp_header->doff    = TCP_HEADER_DOFF_MIN_VALUE;

    process_packet(u, frame);
}


static void 
send_faked_ack(tc_user_t *u)
{
    tc_ip_header_t   *ip_header;
    tc_tcp_header_t  *tcp_header;
    unsigned char    *p, frame[FAKE_FRAME_LEN];

    memset(frame, 0, FAKE_FRAME_LEN);
    p = frame + ETHERNET_HDR_LEN;
    ip_header  = (tc_ip_header_t *) p;
    tcp_header = (tc_tcp_header_t *) (p + IP_HEADER_LEN);

    ip_header->version  = 4;
    ip_header->ihl      = IP_HEADER_LEN/4;
    ip_header->frag_off = htons(IP_DF); 
    ip_header->ttl      = 64; 
    ip_header->protocol = IPPROTO_TCP;
    ip_header->saddr    = u->src_addr;
    ip_header->daddr    = u->dst_addr;
    tcp_header->source  = u->src_port;
    tcp_header->dest    = u->dst_port;
    tcp_header->seq     = u->exp_seq;
    tcp_header->ack_seq = u->exp_ack_seq;
    tcp_header->window  = 65535; 
    tcp_header->ack     = 1;
    if (u->state.timestamped) {
        ip_header->tot_len  = htons(FAKE_IP_TS_DATAGRAM_LEN);
        tcp_header->doff    = TCP_HEADER_DOFF_TS_VALUE;
        fill_timestamp(u, tcp_header);
    } else {
        ip_header->tot_len  = htons(FAKE_MIN_IP_DATAGRAM_LEN);
        tcp_header->doff    = TCP_HEADER_DOFF_MIN_VALUE;
    }

    process_packet(u, frame);
}


static void
retransmit(tc_user_t *u, uint32_t cur_ack_seq)
{
    frame_t          *unack_frame, *next;

    unack_frame = u->orig_unack_frame;
    if (unack_frame == NULL) {
        return;
    }

    next = unack_frame->next;
    while (true) {
        if (unack_frame == u->orig_frame) {
            break;
        }
        if (unack_frame->seq == cur_ack_seq) {
            tc_log_debug1(LOG_DEBUG, 0, "packets retransmitted:%u", 
                    ntohs(u->src_port));
            process_packet(u, unack_frame->frame_data);
            break;
        } else if (before(unack_frame->seq, cur_ack_seq) && next != NULL &&
                before(cur_ack_seq, next->seq)) 
        {
            process_packet(u, unack_frame->frame_data);
            break;
        } else if (before(unack_frame->seq, cur_ack_seq)) {
            unack_frame = next;
            if (unack_frame == NULL) {
                break;
            }
            next = unack_frame->next;
        } else {
            tc_log_debug1(LOG_DEBUG, 0, "packets retransmitted not match:%u", 
                    ntohs(u->src_port));
            break;
        }
    }
}


static void
update_ack_packets(tc_user_t *u, uint32_t cur_ack_seq)
{
    frame_t          *unack_frame, *next;

    unack_frame = u->orig_unack_frame;
    if (unack_frame == NULL) {
        return;
    }

    next = unack_frame->next;
    while (true) {
        if (unack_frame == u->orig_frame) {
            break;
        }
        if (next != NULL) {
            if (next->seq == cur_ack_seq) {
                u->orig_unack_frame = unack_frame->next;
                break;
            } else if (before(cur_ack_seq, next->seq) && 
                    before(unack_frame->seq, cur_ack_seq)) 
            {
                tc_log_debug1(LOG_DEBUG, 0, "partially acked:%u", 
                        ntohs(u->src_port));
                break;
            } else {    
                tc_log_debug1(LOG_DEBUG, 0, "skipped:%u", 
                        ntohs(u->src_port));
                unack_frame = next;
                next = unack_frame->next;
                if (unack_frame == u->orig_sess->last_frame) {
                    break;
                }
            }
        } else {
            if (before(unack_frame->seq, cur_ack_seq)) {
                unack_frame = unack_frame->next;
            }
            u->orig_unack_frame = unack_frame;
            break;
        }
    }

}


static void         
retrieve_options(tc_user_t *u, int direction, tc_tcp_header_t *tcp_header)
{                   
    uint32_t       ts_value; 
    unsigned int   opt, opt_len;
    unsigned char *p, *end;

    p = ((unsigned char *) tcp_header) + TCP_HEADER_MIN_LEN;
    end =  ((unsigned char *) tcp_header) + (tcp_header->doff << 2);  
    while (p < end) {
        opt = p[0];
        switch (opt) {
            case TCPOPT_WSCALE:
                if ((p + 1) >= end) {
                    return;
                }
                opt_len = p[1];
                if ((p + opt_len) > end) {
                    return;
                }
                u->wscale = (uint16_t) p[2];
                p += opt_len;
            case TCPOPT_TIMESTAMP:
                if ((p + 1) >= end) {
                    return;
                }
                opt_len = p[1];
                if ((p + opt_len) > end) {
                    return;
                }
                if (direction == LOCAL) {
                    ts_value = EXTRACT_32BITS(p + 2);
                } else {
                    u->ts_ec_r  = EXTRACT_32BITS(p + 2);
                    ts_value = EXTRACT_32BITS(p + 6);
                    if (tcp_header->syn) {
                        u->state.timestamped = 1;
                        tc_log_debug1(LOG_DEBUG, 0, "timestamped,p=%u", 
                                ntohs(u->src_port));
                    }
                    tc_log_debug3(LOG_DEBUG, 0, 
                            "get ts(client viewpoint):%u,%u,p:%u", 
                            u->ts_value, u->ts_ec_r, ntohs(u->src_port));
                }
                if (ts_value > u->ts_value) {
                    tc_log_debug1(LOG_DEBUG, 0, "ts > history,p:%u",
                            ntohs(u->src_port));
                    u->ts_value = ts_value;
                }
                p += opt_len;
            case TCPOPT_NOP:
                p = p + 1; 
                break;                      
            case TCPOPT_EOL:
                return;
            default:
                if ((p + 1) >= end) {
                    return;
                }
                opt_len = p[1];
                p += opt_len;
                break;
        }    
    }

    return;
}


void 
process_outgress(unsigned char *packet)
{
    uint16_t           size_ip, size_tcp, tot_len, cont_len;
    uint32_t           seq, ack_seq;
    uint64_t           key;
    tc_user_t         *u;
    tc_ip_header_t    *ip_header;
    tc_tcp_header_t   *tcp_header;

    last_resp_time = tc_time();
    tc_stat.resp_cnt++;
    ip_header  = (tc_ip_header_t *) packet;
    size_ip    = ip_header->ihl << 2;
    tcp_header = (tc_tcp_header_t *) ((char *) ip_header + size_ip);


    key = tc_get_key(ip_header->daddr, tcp_header->dest);
    tc_log_debug1(LOG_DEBUG, 0, "key from bak:%llu", key);
    u = tc_retrieve_user(key);

    if (u != NULL) {

        tc_log_debug_trace(LOG_DEBUG, 0, BACKEND_FLAG, ip_header, tcp_header);
        u->srv_window = ntohs(tcp_header->window);
        if (u->wscale) {
            u->srv_window = u->srv_window << (u->wscale);
            tc_log_debug1(LOG_DEBUG, 0, "window size:%u", u->srv_window);
        }
        if (u->state.timestamped) {
            retrieve_options(u, REMOTE, tcp_header);
        }
        size_tcp = tcp_header->doff << 2;
        tot_len  = ntohs(ip_header->tot_len);
        cont_len = tot_len - size_tcp - size_ip;

        if (ip_header->daddr != u->src_addr || tcp_header->dest!= u->src_port) {
            tc_log_info(LOG_NOTICE, 0, " key conflict");
        }
        seq = ntohl(tcp_header->seq);
        u->exp_seq = tcp_header->ack_seq;
        ack_seq = ntohl(tcp_header->ack_seq);

        if (u->last_seq == seq && u->last_ack_seq == ack_seq) {
            u->fast_retransmit_cnt++;
            if (u->fast_retransmit_cnt == 3) {
                retransmit(u, ack_seq);
                return;
            }
        } else {
            update_ack_packets(u, ack_seq);
            u->fast_retransmit_cnt = 0;
        }

        u->last_ack_seq =  ack_seq;
        u->last_seq =  seq;


        if (cont_len > 0) {
            u->last_recv_resp_cont_time = tc_milliscond_time();
            tc_stat.resp_cont_cnt++;
            u->state.resp_waiting = 0;   
            u->exp_ack_seq = htonl(seq + cont_len);
            send_faked_ack(u);
        } else {
            u->exp_ack_seq = tcp_header->seq;
        }
        
        if (tcp_header->syn) {
            tc_log_debug1(LOG_DEBUG, 0, "recv syn from back:%u", 
                    ntohs(u->src_port));
            u->exp_ack_seq = htonl(ntohl(u->exp_ack_seq) + 1);
            if (!u->state.resp_syn_received) {
                tc_stat.conn_cnt++;
                tc_stat.active_conn_cnt++;
                u->state.resp_syn_received = 1;
                u->state.status |= SYN_CONFIRM;
                tc_log_debug2(LOG_DEBUG, 0, "exp ack seq:%u, p:%u",
                        ntohl(u->exp_ack_seq), ntohs(u->src_port));
                if (size_tcp > TCP_HEADER_MIN_LEN) {
                    retrieve_options(u, REMOTE, tcp_header);
                    if (u->wscale > 0) {
                        tc_log_debug2(LOG_DEBUG, 0, "wscale:%u, p:%u",
                                u->wscale, ntohs(u->src_port));
                    }
                }
                process_user_packet(u);

            } else {
                send_faked_ack(u);
                tc_log_debug1(LOG_DEBUG, 0, "syn, but already syn received:%u",
                    ntohs(u->src_port));
            }
        } else if (tcp_header->fin) {
            tc_log_debug1(LOG_DEBUG, 0, "recv fin from back:%u", 
                    ntohs(u->src_port));
            u->exp_ack_seq = htonl(ntohl(u->exp_ack_seq) + 1);
            u->state.status  |= SERVER_FIN;
            send_faked_ack(u);
            if (u->state.status & CLIENT_FIN) {
                u->state.over = 1;
            } else {
#if (TC_COMET)
                send_faked_rst(u);
#else
                if (u->orig_frame == NULL) {
                    send_faked_rst(u);
                } else {
                    process_user_packet(u);
                }
#endif
            }
            tc_stat.fin_recv_cnt++;

        } else if (tcp_header->rst) {
            tc_log_info(LOG_NOTICE, 0, "recv rst from back:%u", 
                    ntohs(u->src_port));
            tc_stat.rst_recv_cnt++;
            if (u->state.status == SYN_SENT) {
                if (!u->state.over) {
                    tc_stat.conn_reject_cnt++;
                }
            }

            u->state.over = 1;
            u->state.status  |= SERVER_FIN;
        }

    } else {
        tc_log_debug_trace(LOG_DEBUG, 0, BACKEND_FLAG, ip_header,
                tcp_header);
        tc_log_debug0(LOG_DEBUG, 0, "no active sess for me");
    }

}


static void 
check_replay_complete()
{
#if (!TC_COMET)
    int  diff;

    if (last_resp_time) {
        diff = tc_time() - last_resp_time;
        if (diff > DEFAULT_TIMEOUT) {
            tc_over = 1;
        }
    }
#endif
}


void 
process_ingress()
{
    tc_user_t  *u = NULL;

    u = tc_retrieve_active_user();

    if (!u->state.over) {
        process_user_packet(u);
        if ((u->state.status & CLIENT_FIN) && (u->state.status & SERVER_FIN)) {
            u->state.over = 1;
        }
    } else {
        if (!u->state.over_recorded) {
            u->state.over_recorded = 1;
            tc_stat.active_conn_cnt--;
            if (tc_stat.active_conn_cnt == 0) {
                tc_over = 1;
            }
        }
    }
    
    check_replay_complete();
}


void
output_stat()
{
    tc_log_info(LOG_NOTICE, 0, "active conns:%llu", tc_stat.active_conn_cnt);
    tc_log_info(LOG_NOTICE, 0, "reject:%llu, reset recv:%llu,fin recv:%llu",
            tc_stat.conn_reject_cnt, tc_stat.rst_recv_cnt, 
            tc_stat.fin_recv_cnt);
    tc_log_info(LOG_NOTICE, 0, "reset sent:%llu, fin sent:%llu",
            tc_stat.rst_sent_cnt, tc_stat.fin_sent_cnt);
    tc_log_info(LOG_NOTICE, 0, "conns:%llu,resp packs:%llu,c-resp packs:%llu",
            tc_stat.conn_cnt, tc_stat.resp_cnt, tc_stat.resp_cont_cnt);
    tc_log_info(LOG_NOTICE, 0, 
            "syn sent cnt:%llu,clt packs sent:%llu,clt cont sent:%llu",
            tc_stat.syn_sent_cnt, tc_stat.packs_sent_cnt, 
            tc_stat.cont_sent_cnt);
}

void
tc_interval_dispose(tc_event_timer_t *evt)
{
    output_stat();
    tc_event_update_timer(evt, 5000);
}

void 
release_user_resources()
{
    int                 i, rst_send_cnt = 0, valid_sess = 0;
    frame_t            *fr;
    tc_user_t          *u;
    p_sess_entry        e;
    struct sockaddr_in  targ_addr;

    memset(&targ_addr, 0, sizeof(targ_addr));
    targ_addr.sin_family = AF_INET;

    if (s_table && s_table->num_of_sess > 0) {
        if (user_array) {
            for (i = 0; i < size_of_users; i++) {
                u = user_array + i;
                if (!(u->state.status & SYN_CONFIRM)) {
                    targ_addr.sin_addr.s_addr = u->src_addr;
                    tc_log_info(LOG_NOTICE, 0, "connection fails:%s:%u", 
                            inet_ntoa(targ_addr.sin_addr), ntohs(u->src_port));
                }
                if (u->total_packets_sent < u->orig_sess->frames) {
                    tc_log_debug3(LOG_DEBUG, 0, 
                            "total sent frames:%u, total:%u, p:%u", 
                            u->total_packets_sent, u->orig_sess->frames, 
                            ntohs(u->src_port));
                }
                if (u->state.status && !u->state.over) {
                    send_faked_rst(u);
                    rst_send_cnt++;
                }
            }
        }

        tc_log_info(LOG_NOTICE, 0, "send %d resets to release tcp resources", 
                rst_send_cnt);
    }

    if (s_table) {
        for (i = 0; i < s_table->size; i++) {
            e = s_table->entries[i];
            while (e) {
                fr = e->data.first_frame;
                if (e->data.has_req) {
                    valid_sess++;
                }
                e = e->next;
            }
        }

        tc_log_info(LOG_NOTICE, 0, "valid sess:%d", valid_sess);
    }

    s_table = NULL;
    user_array = NULL;
    user_index_array = NULL;

    if (g_pool != NULL) {
        tc_destroy_pool(g_pool);
        g_pool = NULL;
    }
}

