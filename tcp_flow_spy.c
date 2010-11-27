/*
 * tcpflowprobe - Observe the TCP flow summerized information with kprobes.
 *
 * The idea for this came from Werner Almesberger's umlsim
 * Copyright (C) 2004, Stephen Hemminger <shemminger@osdl.org>
 * Copyright (C) 2010, Soheil Hassas Yeganeh <soheil@cs.toronto.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/socket.h>
#include <linux/tcp.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/timer.h>
//#include <net/net_namespace.h>

#include <net/tcp.h>

//#define TCP_FLOW_PROBE_DEBUG
#define HASHSIZE_COEF 1

MODULE_AUTHOR("Stephen Hemminger <shemminger@linux-foundation.org>, Soheil Hassas Yeganeh <soheil@cs.toronto.edu>");
MODULE_DESCRIPTION("TCP cwnd snooper");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1-ALPHA");

static int port __read_mostly = 0;
MODULE_PARM_DESC(port, "Port to match (0=all)");
module_param(port, int, 0);

static unsigned int bufsize __read_mostly = 4096;
MODULE_PARM_DESC(bufsize, "Log buffer size in packets (4096)");
module_param(bufsize, uint, 0);

#define HASHTABLE_SIZE (HASHSIZE_COEF*bufsize)

//static int full __read_mostly;
//MODULE_PARM_DESC(full, "Full log (1=every ack packet received,  0=only cwnd changes)");
//module_param(full, int, 0);

static int bucket_length __read_mostly = 1;
MODULE_PARM_DESC(bucket_length, "Length of each bucket in the histogram (1) except the last bucket length is not bounded.");
module_param(bucket_length, int, 0);

static int number_of_buckets  __read_mostly = 1;
MODULE_PARM_DESC(number_of_buckets, "Number of buckets in the histogram (1)");
module_param(number_of_buckets, int, 0);

static int live __read_mostly = 0;
MODULE_PARM_DESC(live, "(0) stats of completed flows are printed, (1) stats of live flows are printed.");
module_param(live, int, 0);

static const char procname[] = "tcpflowprobe";

struct tcp_flow_log {
    struct timespec first_packet_tstamp;
    struct timespec last_packet_tstamp;
    struct timespec last_printed_tstamp;

    __be32	saddr, daddr;
    __be16	sport, dport;

    u32 recv_count; // No of received packets 
    u32 snd_count; // No of sent packets
    u64	recv_size; // Avg length of the packet
    u64 snd_size; // Total size of packets in byte

    u32 last_recv_seq;
    u32 last_snd_seq;

    u32 out_of_order_packets;
    u32 total_retransmissions;

    u32 snd_cwnd_clamp;
    u32	ssthresh;
    u32	srtt;
    u32 last_cwnd;

    int used;

    struct tcp_flow_log* next;
    struct tcp_flow_log* prev;

    u32* snd_cwnd_histogram;
//    u32* snd_wnd_histogram;
};

static struct {
    spinlock_t	lock;
    wait_queue_head_t wait;
   	struct timespec	start;
    struct timer_list timer;
    struct timespec last_update;
    struct timespec last_read;

    //unsigned long	head, tail;
    //struct tcp_flow_log	*live;
    struct tcp_flow_log *available;
    struct tcp_flow_log *storage;
    struct tcp_flow_log *finished;
} tcp_flow_probe;

struct hashtable_entry{
    //   spinlock_t	lock;

    struct tcp_flow_log* head; 
    struct tcp_flow_log* tail;
};

/*static struct tcp_flow_log* finished_flow_logs;
  static spinlock_t finished_flow_logs_lock;*/

static struct {
    //	spinlock_t	lock;
    u32 count;

    struct hashtable_entry* entries;   
} tcp_flow_hashtable;


static inline struct timespec get_time(void) {
    struct timespec ts;
    ktime_get_ts(&ts);
    return ts;
}


// Soheil: I could use inet hash function, but I prefer to have my own. 
static inline u32 skb_hash_function(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport) {
/*    if(unlikely(!skb)){
        return 0;
    } else {
*/
/*        const struct tcphdr* th = tcp_hdr(skb); 
        const struct iphdr* iph = ip_hdr(skb); */

        u32 hash = (((saddr >> 24) & 0xff) + ((daddr >> 24) & 0xff) + dport + sport) % HASHTABLE_SIZE;

#ifdef TCP_FLOW_PROBE_DEBUG
        printk(KERN_DEBUG "Hashcode %u, ip_src %u, src_port %u", hash, ((saddr >> 24) & 0xff), ntohs(sport)); 
#endif
        return hash;
    //}
    
}

static inline struct hashtable_entry* get_entry_for_skb(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport){
    struct hashtable_entry* entry;
    //spin_lock(&tcp_flow_hashtable.lock);
    entry = &(tcp_flow_hashtable.entries[skb_hash_function(saddr, daddr, sport, dport)]);                
    //spin_unlock(&tcp_flow_hashtable.lock);
    return entry;
}

static inline int is_log_for_skb(struct tcp_flow_log* log, __be32 saddr, __be32 daddr, __be16 sport, __be16 dport){
//    struct tcphdr* th = tcp_hdr(skb);
//    struct iphdr* iph = ip_hdr(skb);
    return (saddr == log->saddr && daddr == log->daddr && dport == log->dport && sport == log->sport) || 
        (daddr == log->saddr && daddr == log->saddr && dport == log->sport && sport == log->dport);
}

static inline struct tcp_flow_log* find_flow_log_for_skb(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport){
    struct hashtable_entry* entry = get_entry_for_skb(saddr, daddr, sport, dport);
    struct tcp_flow_log* log_element;
    if(unlikely(!entry)){
        return 0;
    }

    //spin_lock(&entry->lock);
    log_element = entry->head;

    while(log_element){
        if(is_log_for_skb(log_element, saddr, daddr, sport, dport)){
            goto ret;
        }
        log_element = log_element->next;
    }

ret:
    //spin_unlock(&entry->lock);
    return log_element;
}

static inline void remove_from_hashentry(struct hashtable_entry* entry, struct tcp_flow_log* log){
    if(unlikely(!log || !entry)){
        return;
    }

    if(log == entry->tail){
        entry->tail = log->prev;
        if(entry->tail){
            entry->tail->next = 0;
        }
    }

    if(log == entry->head){
        entry->head = log->next;
        if(entry->head){
            entry->head->prev = 0;
        }
    }

    if(log->next){
        log->next->prev = log->prev;
    }

    if(log->prev){
        log->prev->next = log->next;
    }

    log->prev = 0; 
    log->next = 0;
   
}

static inline void remove_from_hashtable(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport){
    struct hashtable_entry* entry = get_entry_for_skb(saddr, daddr, sport, dport);
    struct tcp_flow_log* log = find_flow_log_for_skb(saddr, daddr, sport, dport);

    remove_from_hashentry(entry, log);
    /*if(unlikely(!log)){
        return;
    }

    if(log == entry->tail){
        entry->tail = log->prev;
        if(entry->tail){
            entry->tail->next = 0;
        }
    }

    if(log == entry->head){
        entry->head = log->next;
        if(entry->head){
            entry->head->prev = 0;
        }
    }

    if(log->next){
        log->next->prev = log->prev;
    }

    if(log->prev){
        log->prev->next = log->next;
    }

    log->prev = 0; 
    log->next = 0;*/
}


static inline void reinitialize_tcp_flow_log(struct tcp_flow_log* log, __be32 saddr, __be32 daddr, __be16 sport, __be16 dport) {
    struct hashtable_entry* entry;
    if(unlikely(!log)){
        return;
    }

    memset(log, 0, sizeof(struct tcp_flow_log) - 4 * sizeof(u32*)); 

//    log->snd_cwnd_histogram[0] = 0;

    //memset(log->snd_wnd_histogram, 0, number_of_buckets * sizeof(u32)); 
    memset(log->snd_cwnd_histogram, 0, number_of_buckets * sizeof(u32)); 


    log->first_packet_tstamp = get_time();

    entry = get_entry_for_skb(saddr, daddr, sport, dport);
    //spin_lock(&entry->lock);
    if(unlikely(entry->tail)){
        entry->tail->next = log;
    } else {
        entry->head = log;
    }
    
    log->prev = entry->tail;
    log->next = 0;
    entry->tail = log; 
    //spin_unlock(&entry->lock);
}

static inline int is_finished(__be32 saddr, __be32 daddr, __be16 sport, __be16 dport){
    struct tcp_flow_log* finished = tcp_flow_probe.finished;
    while(finished){
        if(is_log_for_skb(finished, saddr, daddr, sport, dport)){
            return 1;
        }
        finished = finished->next;
    }
    return 0;
}

static inline struct hashtable_entry* initialize_hashtable(u32 size){
    int i;
    tcp_flow_hashtable.entries = kcalloc(size, sizeof(struct hashtable_entry), GFP_KERNEL);
    for(i = 0; i < size; i++){
        //spin_lock_init(&tcp_flow_hashtable.entries[i].lock);
    }
    //spin_lock_init(&tcp_flow_hashtable.lock);
    tcp_flow_hashtable.count = 0;
    return tcp_flow_hashtable.entries;
}

/*static inline int tcp_flow_log_used(void)
  {
  return tcp_flow_hashtable.count;
  }*/

static inline int tcp_flow_log_avail(void)
{
    return tcp_flow_probe.available != 0;
}

/*
 * Hook inserted to be called before each receive packet.
 * Note: arguments must match tcp_rcv_established()!
 * We should change this one to tcp_v4_do_rcv
 */
static int jtcp_v4_do_rcv(struct sock *sk, struct sk_buff *skb) {
    const struct tcp_sock *tp = tcp_sk(sk);
    const struct tcphdr* th = tcp_hdr(skb); 
    const struct iphdr* iph = ip_hdr(skb); 

    spin_lock_bh(&tcp_flow_probe.lock);
    /* Only update if port matches */
    if ((port == 0 || ntohs(th->dest) == port ||
                ntohs(th->source) == port)) {

        struct tcp_flow_log* p = find_flow_log_for_skb(iph->saddr, iph->daddr, th->source, th->dest);

        if(unlikely(!p)){
            if(!th->syn){
                goto ret;
            }
            //is_finished(skb);
            /* If log fills, just silently drop */
            if (tcp_flow_log_avail()) {
#ifdef TCP_FLOW_PROBE_DEBUG
                printk ( KERN_ERR " available %d -> %d for src_port %u \n", tcp_flow_probe.available, tcp_flow_probe.available->next, ntohs(th->source));
#endif
                p = tcp_flow_probe.available;
                tcp_flow_probe.available = tcp_flow_probe.available->next;
                reinitialize_tcp_flow_log(p, iph->saddr, iph->daddr, th->source, th->dest);
                p->used = 1;
            }else{
#ifdef TCP_FLOW_PROBE_DEBUG
                printk(KERN_ERR "==================================");
                int i = 0;
                for(; i < bufsize; i++){
                    printk (" SRC_poRT %u next %d prev %d self %d \n", ntohs(tcp_flow_probe.storage[i].sport), tcp_flow_probe.storage[i].next, tcp_flow_probe.storage[i].prev, &tcp_flow_probe.storage[i] );
                }
                printk (" finished %d available %d \n", tcp_flow_probe.finished, tcp_flow_probe.available);
            //    wake_up(&tcp_flow_probe.wait);
#endif            
                goto ret;
            }

        }

        p->last_packet_tstamp = get_time();

        p->saddr = iph->saddr;
        p->sport = th->source;
        p->daddr = iph->daddr;
        p->dport = th->dest;

        p->recv_count++;
        p->recv_size += skb->len; 
        if(likely(ntohl(th->seq) >= p->last_recv_seq)){
            p->last_recv_seq = ntohl(th->seq);
        }else{
            p->out_of_order_packets++;
        }

        if (sk->sk_state == TCP_ESTABLISHED) {
            int cwnd_index = tp->snd_cwnd / bucket_length;
            //int wnd_index = tp->snd_wnd / bucket_length;
            cwnd_index = min(number_of_buckets - 1, cwnd_index);
            //wnd_index = min(number_of_buckets - 1, wnd_index);
            p->snd_cwnd_histogram[cwnd_index]++;
            p->last_cwnd = tp->snd_cwnd;
            //p->snd_wnd_histogram[wnd_index]++;

#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_DEBUG "CWND %u RCV %u CWINDEX %d\n", p->snd_cwnd_histogram[cwnd_index], p->recv_count, cwnd_index); 
#endif

            p->snd_cwnd_clamp = tp->snd_cwnd_clamp; 
            p->ssthresh = tcp_current_ssthresh(sk);
            p->srtt = tp->srtt >> 3;
        }
        if(th->fin || th->rst){
            remove_from_hashtable(iph->saddr, iph->daddr, th->source, th->dest);

            p->next = tcp_flow_probe.finished;
            tcp_flow_probe.finished = p;
#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_ERR "finished %d -> %d \n", tcp_flow_probe.finished, tcp_flow_probe.finished->next);
#endif
            wake_up(&tcp_flow_probe.wait);
            
#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_DEBUG "Finished ip_src %u, src_port %u, finished logs %lX\n", ((iph->saddr >> 24) & 0xff), ntohs(th->source), tcp_flow_probe.finished); 
#endif

        }else if (live){
             wake_up(&tcp_flow_probe.wait);
        
        }

        tcp_flow_probe.last_update = get_time();
 
    }
ret:
    spin_unlock_bh(&tcp_flow_probe.lock);
    jprobe_return();
    return 0;
}

static struct jprobe tcp_recv_jprobe = {
    .kp = {
        .symbol_name	= "tcp_v4_do_rcv",
    },
    .entry	= (kprobe_opcode_t*) jtcp_v4_do_rcv,
};

static int jtcp_transmit_skb(struct sock *sk, struct sk_buff *skb) {
    const struct tcp_sock *tp = tcp_sk(sk);
    const struct inet_sock *inet = inet_sk(sk);

    struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
    __be16 sport = inet->sport,
           dport = inet->dport;
    __be32 saddr = inet->saddr, 
           daddr = inet->daddr;

    spin_lock_bh(&tcp_flow_probe.lock);
    /* Only update if port matches */
    if ((port == 0 || ntohs(sport) == port ||
                ntohs(dport) == port)) {

        struct tcp_flow_log* p = find_flow_log_for_skb(saddr, daddr, sport, dport);

        if(unlikely(!p)){
            if(!(tcb->flags & TCPCB_FLAG_SYN)){
                goto ret;
            }
            //is_finished(skb);
            /* If log fills, just silently drop */
            if (tcp_flow_log_avail()) {
#ifdef TCP_FLOW_PROBE_DEBUG
                printk ( KERN_ERR " available %d -> %d for src_port %u \n", tcp_flow_probe.available, tcp_flow_probe.available->next, ntohs(sport));
#endif
                p = tcp_flow_probe.available;
                tcp_flow_probe.available = tcp_flow_probe.available->next;
                reinitialize_tcp_flow_log(p,saddr, daddr, sport, dport);
                p->used = 1;
            }else{
#ifdef TCP_FLOW_PROBE_DEBUG
                printk(KERN_ERR "==================================");
                int i = 0;
                for(; i < bufsize; i++){
                    printk (" SRC_poRT %u next %d prev %d self %d \n", ntohs(tcp_flow_probe.storage[i].sport), tcp_flow_probe.storage[i].next, tcp_flow_probe.storage[i].prev, &tcp_flow_probe.storage[i] );
                }
                printk (" finished %d available %d \n", tcp_flow_probe.finished, tcp_flow_probe.available);
            //    wake_up(&tcp_flow_probe.wait);
#endif            
                goto ret;
            }
            p->saddr = saddr;
            p->sport = sport;
            p->daddr = daddr;
            p->dport = dport;
        }

        p->last_packet_tstamp = get_time();

        p->snd_count++;
        p->snd_size += skb->len; 
        p->total_retransmissions = tp->total_retrans;

        if (sk->sk_state == TCP_ESTABLISHED) {
            int cwnd_index = tp->snd_cwnd / bucket_length;
//            int wnd_index = tp->snd_wnd / bucket_length;
            cwnd_index = min(number_of_buckets - 1, cwnd_index);
//            wnd_index = min(number_of_buckets - 1, wnd_index);
            p->snd_cwnd_histogram[cwnd_index]++;
            p->last_cwnd = tp->snd_cwnd;
//            p->snd_wnd_histogram[wnd_index]++;

#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_DEBUG "CWND %u RCV %u CWINDEX %d\n", p->snd_cwnd_histogram[cwnd_index], p->recv_count, cwnd_index); 
#endif

            p->snd_cwnd_clamp = tp->snd_cwnd_clamp; 
            p->ssthresh = tcp_current_ssthresh(sk);
            p->srtt = tp->srtt >> 3;
        }
        if ( (tcb->flags & TCPCB_FLAG_FIN) || 
                (tcb->flags & TCPCB_FLAG_SYN) ) {
            remove_from_hashtable(saddr, daddr, sport, dport);

            p->next = tcp_flow_probe.finished;
            tcp_flow_probe.finished = p;
#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_ERR "finished %d -> %d \n", tcp_flow_probe.finished, tcp_flow_probe.finished->next);
#endif
            wake_up(&tcp_flow_probe.wait);
            
#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_DEBUG "Finished ip_src %u, src_port %u, finished logs %lX\n", ((saddr >> 24) & 0xff), sport, tcp_flow_probe.finished); 
#endif

        }else if (live){
             wake_up(&tcp_flow_probe.wait);
        }

        tcp_flow_probe.last_update = get_time();
 
    }
ret:
    spin_unlock_bh(&tcp_flow_probe.lock);
    jprobe_return();
    return 0;
}


static struct jprobe tcp_transmit_jprobe = {
    .kp = {
        .symbol_name = "tcp_transmit_skb",
    },
    .entry = (kprobe_opcode_t*) jtcp_transmit_skb,
};

static int tcpflowprobe_open(struct inode * inode, struct file * file)
{
    /* Reset (empty) log */
    spin_lock_bh(&tcp_flow_probe.lock);
    tcp_flow_probe.start = get_time();
    tcp_flow_probe.last_read = get_time();
    tcp_flow_probe.last_update = get_time();
    //wake_up(&tcp_flow_probe.wait);
    spin_unlock_bh(&tcp_flow_probe.lock);

    return 0;
}

static inline struct timespec tcpprobe_timespec_sub(struct timespec lhs, struct timespec rhs){
    struct timespec tv;
    tv.tv_sec = lhs.tv_sec - rhs.tv_sec;
    tv.tv_nsec = lhs.tv_nsec - rhs.tv_sec;
    return tv;
}

static inline int tcpprobe_timespec_larger(struct timespec lhs, struct timespec rhs){

    int ret = lhs.tv_sec > rhs.tv_sec || (lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec > rhs.tv_nsec);
    return ret;
}


static int last_printed_flow_index = 0;

static int tcpflowprobe_sprint(char *tbuf, int n)
{
    const struct tcp_flow_log *p = 0;
    struct timespec tv;
    int size = 0;
    int index = 0;
    int finished = 0;
    struct timespec duration;
    if (!tcp_flow_probe.finished && live) {
        int count = 0;
        //int last_index = last_printed_flow_index;
        do {
            last_printed_flow_index = (last_printed_flow_index + 1) % bufsize;
            if (tcp_flow_probe.storage[last_printed_flow_index].used && 
                    tcpprobe_timespec_larger( 
                        tcp_flow_probe.storage[last_printed_flow_index].last_packet_tstamp, 
                        tcp_flow_probe.storage[last_printed_flow_index].last_printed_tstamp) 
                ) {
                p = &tcp_flow_probe.storage[last_printed_flow_index];
                break;
            }
        } while (!p && ++count < bufsize);
    } else {
        finished = 1;
        p = tcp_flow_probe.finished;
    }

    if (!p) {
        goto ret;
    }

    tcp_flow_probe.storage[last_printed_flow_index].last_printed_tstamp = get_time();

    tv = tcpprobe_timespec_sub(p->last_packet_tstamp, tcp_flow_probe.start);
    // = ktime_to_timespec(ktime_sub(p->last_packet_tstamp, tcp_flow_probe.start));

    duration = tcpprobe_timespec_sub(p->last_packet_tstamp, p->first_packet_tstamp);
//        = ktime_to_timespec(ktime_sub(p->last_packet_tstamp, p->first_packet_tstamp));


#ifdef TCP_FLOW_PROBE_DEBUG
    printk(KERN_DEBUG 			"%lu.%09lu %pI4:%u %pI4:%u %u %u %lu %lu %u %u %u %u\n",
            (unsigned long) tv.tv_sec,
            (unsigned long) tv.tv_nsec,
            &p->saddr, ntohs(p->sport),
            &p->daddr, ntohs(p->dport),
            p->recv_count, p->snd_count, 
            (unsigned long) p->recv_size, (unsigned long) p->snd_size,
            p->retrans_packets, p->snd_cwnd_clamp,
            p->ssthresh, p->srtt 
          );
#endif


    size = snprintf(tbuf, n,
            "%lu.%09lu (%d) %x:%u %x:%u %lu.%09lu %u %lu %u %lu %u %u %u %u %u %u ",
            (unsigned long) tv.tv_sec,
            (unsigned long) tv.tv_nsec,
            finished,
            (unsigned int) ntohl(p->saddr), ntohs(p->sport),
            (unsigned int) ntohl(p->daddr), ntohs(p->dport),
            (unsigned long) duration.tv_sec,
            (unsigned long) duration.tv_nsec,
            p->recv_count, 
            (unsigned long) p->recv_size, 
            p->snd_count,
            (unsigned long) p->snd_size,
            p->total_retransmissions, 
            p->out_of_order_packets, p->snd_cwnd_clamp,
            p->ssthresh, p->srtt, p->last_cwnd
            );

    while (size < n-1 && index < number_of_buckets){
        size += min(n - size, snprintf(tbuf + size, n - size, "%u," , p->snd_cwnd_histogram[index++]));
    }

    tbuf[min(n-1,size)] = ' ';
    if(size < n ){
        size++;
    }
//    index = 0;
//    while (size < n-1 && index < number_of_buckets){
//        size += min(n - size, snprintf(tbuf + size, n - size, "%u,", p->snd_wnd_histogram[index++]));
//    }

    tbuf[min(n-1,size)] = '\n';
    
    if(size < n){
        size++;
    }
ret:
    return size;
}

#define PRINT_BUFF_SIZE 256

static ssize_t tcpflowprobe_read(struct file *file, char __user *buf,
        size_t len, loff_t *ppos)
{
    int error = 0;
    size_t cnt = 0;

    if (!buf)
        return -EINVAL;
    while (cnt < len) {
        char tbuf[PRINT_BUFF_SIZE];
        int width = 0;

#ifdef TCP_FLOW_PROBE_DEBUG
        printk(KERN_DEBUG "Read : %u %lX \n", (unsigned int) len, tcp_flow_probe.finished);
#endif

        /* Wait for data in buffer */
        error = wait_event_interruptible(tcp_flow_probe.wait,
                tcp_flow_probe.finished != 0 || tcpprobe_timespec_larger(tcp_flow_probe.last_update, tcp_flow_probe.last_read));

        tcp_flow_probe.last_read = get_time();
#ifdef TCP_FLOW_PROBE_DEBUG
        printk(KERN_DEBUG "error : %d \n", error);
#endif
        if (error)
            break;


        spin_lock_bh(&tcp_flow_probe.lock);
        if (!live && !tcp_flow_probe.finished) {
            /* multiple readers race? */
            spin_unlock_bh(&tcp_flow_probe.lock);
            continue;
        }

        width = tcpflowprobe_sprint(tbuf, sizeof(tbuf));

        if (width == 0){
            spin_unlock_bh(&tcp_flow_probe.lock);
            continue;
        }
        if (cnt + width < len && tcp_flow_probe.finished){
            struct tcp_flow_log* newly_printed = tcp_flow_probe.finished;
            tcp_flow_probe.finished = newly_printed->next;
#ifdef TCP_FLOW_PROBE_DEBUG  
            printk(KERN_ERR "Finished %d\n", tcp_flow_probe.finished);
#endif 
            newly_printed->next = tcp_flow_probe.available;
            newly_printed->used = 0;
            tcp_flow_probe.available = newly_printed;
#ifdef TCP_FLOW_PROBE_DEBUG
            printk(KERN_ERR "Available %d\n", tcp_flow_probe.available);
#endif
            //memset(newly_printed, 0, sizeof(struct tcp_flow_log)  - 2*sizeof(u32*)); 
        } else {
#ifdef TCP_FLOW_PROBE_DEBUG            
            printk(KERN_ERR "DISASTER\n");
#endif
        }


        spin_unlock_bh(&tcp_flow_probe.lock);

#ifdef TCP_FLOW_PROBE_DEBUG
        printk(KERN_DEBUG "Width %d\n",  width); 
#endif

        /* if record greater than space available
           return partial buffer (so far) */
        if (cnt + width >= len)
            break;

#ifdef TCP_FLOW_PROBE_DEBUG
        printk(KERN_DEBUG "Width 2 %d\n", width); 
#endif


        if (copy_to_user(buf + cnt, tbuf, width))
            return -EFAULT;
        cnt += width;
#ifdef TCP_FLOW_PROBE_DEBUG
        printk(KERN_DEBUG "Width 3 %d\n", width); 
#endif
    }
    return cnt == 0 ? error : cnt;
}

static const struct file_operations tcpflowprobe_fops = {
    .owner	 = THIS_MODULE,
    .open	 = tcpflowprobe_open,
    .read    = tcpflowprobe_read,
};

#define EXPIRE_TIMEOUT (jiffies + HZ )
#define EXPIRE_SKB (2*60)
static void prune_timer(unsigned long data)
{
    int i;
    struct timespec now = get_time(); 
    spin_lock_bh(&tcp_flow_probe.lock);
    for(i = 0; i < HASHTABLE_SIZE; i++){
        struct hashtable_entry* entry = &tcp_flow_hashtable.entries[i];
        struct tcp_flow_log* log = 0;
    	if(unlikely(!entry)){
	        continue;
	    }
        log = entry->head;
        while(log){
            struct timespec interval
                = tcpprobe_timespec_sub(now, log->last_packet_tstamp);
            struct tcp_flow_log* nextLog = log->next;
            if(interval.tv_sec > EXPIRE_SKB){
                remove_from_hashentry(entry, log); 
                log->next = tcp_flow_probe.finished;
                tcp_flow_probe.finished = log;
            }
            log = nextLog;
        }
    }

    tcp_flow_probe.timer.expires = EXPIRE_TIMEOUT;
    spin_unlock_bh(&tcp_flow_probe.lock);
    add_timer(&tcp_flow_probe.timer);
}


static __init int tcpflowprobe_init(void)
{
    int ret = -ENOMEM;
    int i = 0;
    init_waitqueue_head(&tcp_flow_probe.wait);
    spin_lock_init(&tcp_flow_probe.lock);

    if (bufsize == 0)
        return -EINVAL;

    bufsize = roundup_pow_of_two(bufsize);
    tcp_flow_probe.available = kcalloc(bufsize, sizeof(struct tcp_flow_log), GFP_KERNEL);
    if (!tcp_flow_probe.available)
        goto err0;
    tcp_flow_probe.storage = tcp_flow_probe.available; 
    tcp_flow_probe.finished = 0;
    //tcp_flow_probe.live = 0;

    setup_timer(&tcp_flow_probe.timer, prune_timer, 0);
    tcp_flow_probe.timer.expires = EXPIRE_TIMEOUT;

    if(!initialize_hashtable(HASHTABLE_SIZE))
        goto err0;

    if (!proc_net_fops_create(procname, S_IRUSR | S_IRGRP | S_IROTH, &tcpflowprobe_fops))
        goto err0;

    ret = register_jprobe(&tcp_recv_jprobe);
    ret = register_jprobe(&tcp_transmit_jprobe);
    if (ret)
        goto err1;

    for(i = 0; i < bufsize; i++){
        //        tcp_flow_probe.available[i]->prev = i > 0 ? &tcp_flow_probe.available[i-1] : 0; 
        tcp_flow_probe.available[i].snd_cwnd_histogram = kcalloc(number_of_buckets, sizeof(u32), GFP_KERNEL);
//        tcp_flow_probe.available[i].snd_wnd_histogram = kcalloc(number_of_buckets, sizeof(u32), GFP_KERNEL);

        tcp_flow_probe.available[i].next = i < bufsize - 1 ? &tcp_flow_probe.available[i+1] : 0;
    }

    pr_info("TCP probe registered (port=%d) bufsize=%u\n", port, bufsize);
    add_timer(&tcp_flow_probe.timer);
    return 0;
err1:
    proc_net_remove(procname);
err0:
    kfree(tcp_flow_probe.available);
    return ret;
}
module_init(tcpflowprobe_init);

static __exit void tcpflowprobe_exit(void)
{
    int i = 0;
    if (timer_pending(&tcp_flow_probe.timer)) {
        del_timer(&tcp_flow_probe.timer);
    }

    proc_net_remove(procname);
    unregister_jprobe(&tcp_recv_jprobe);
    unregister_jprobe(&tcp_transmit_jprobe);

    for (i = 0; i < bufsize; i++) {
        kfree(tcp_flow_probe.storage[i].snd_cwnd_histogram);
        //kfree(tcp_flow_probe.storage[i].snd_wnd_histogram);
    }
    kfree(tcp_flow_probe.available);
}
module_exit(tcpflowprobe_exit);