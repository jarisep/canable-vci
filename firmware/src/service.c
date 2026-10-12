/* SPDX-License-Identifier: Apache-2.0 */
#include "service.h"
#include "ciso_isotp.h"
#include "isotp_internal.h"
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

/* All protocol mutation runs in the main worker. ISR hooks only add metadata. */
struct event {
    uint16_t op, length;
    uint32_t request, generation;
    uint64_t time;
    uint8_t body[32];
    struct net_buf *payload;
    bool sent;
};
static struct {
    const struct device *can;
    ciso_write_fn write;
    uint32_t session, next_session, generation, last_generation, last_request;
    uint32_t lease_ms, seq, ack, sent_seq, rx_counter, route;
    uint32_t tx_request, tx_flags, tx_transfer, tx_abort_reason;
    uint32_t overflow, dropped_rx, reported_overflow, reported_dropped, reported_bus;
    uint64_t boot, lease_end, tx_deadline, parse_deadline;
    bool channel, bound, tx_active, rx_busy, stopping;
    struct isotp_msg_id rx_addr, tx_addr;
    struct isotp_fc_opts opts;
    struct isotp_send_ctx tx_ctx;
    struct isotp_recv_ctx rx_ctx;
    uint8_t tx[4095];
    uint8_t header[32], body[84];
    uint32_t header_used, body_used, body_size;
    bool discard_tx;
    struct event events[8];
    uint8_t head, count;
    struct net_buf *rx_chain;
    struct k_spinlock lock;
    atomic_t tx_result, tx_finished, forced_stop, rx_terminal;
    struct {
        uint32_t id, request, interval, flags, instance;
        uint64_t next;
        uint16_t length;
        uint8_t data[64];
    } periodic[2];
} s;

static uint64_t now_us(void) { return k_ticks_to_us_floor64(k_uptime_ticks()); }
K_MUTEX_DEFINE(can_control_lock);
static int stop_can(void)
{
    k_mutex_lock(&can_control_lock,K_FOREVER);
    int err=can_stop(s.can);
    k_mutex_unlock(&can_control_lock);
    return err;
}
static void renew_lease(void)
{
    k_spinlock_key_t key=k_spin_lock(&s.lock);
    s.lease_end=now_us()+(uint64_t)s.lease_ms*1000;
    k_spin_unlock(&s.lock,key);
}
static struct k_work_delayable watchdog;
static void watchdog_tick(struct k_work *work)
{
    ARG_UNUSED(work);
    uint64_t now=now_us();
    k_spinlock_key_t key=k_spin_lock(&s.lock);
    bool lost=s.session && now>=s.lease_end;
    k_spin_unlock(&s.lock,key);
    bool a_timeout=s.tx_active && atomic_get(&s.tx_ctx.pending_frames) &&
                   now-s.tx_ctx.can_started_us>=1000000;
    bool deadline=s.tx_active && s.tx_deadline && now>=s.tx_deadline;
    if((lost || a_timeout || deadline) && !atomic_get(&s.forced_stop)) {
        atomic_set(&s.forced_stop,lost ? 7 : 1);
        if(s.tx_active) { ciso_isotp_cancel(&s.tx_ctx); }
        stop_can();
    }
    k_work_reschedule(&watchdog,K_MSEC(10));
}
static uint32_t u32(const uint8_t *b) { return sys_get_le32(b); }
static bool valid_stmin(uint8_t b) { return b <= 0x7f || (b >= 0xf1 && b <= 0xf9); }
static uint32_t addr_status(void)
{
    return ((s.rx_addr.flags & ISOTP_MSG_EXT_ADDR) ? 0x80 : 0) |
           ((s.rx_addr.flags & ISOTP_MSG_IDE) ? 0x100 : 0);
}
static uint32_t reason(int err)
{
    switch (err) {
    case ISOTP_N_TIMEOUT_A: return 1;
    case ISOTP_N_TIMEOUT_BS: return 2;
    case ISOTP_N_TIMEOUT_CR: return 3;
    case ISOTP_N_WRONG_SN: return 4;
    case ISOTP_N_BUFFER_OVERFLW: return 5;
    case ISOTP_N_WFT_OVRN: return 6;
    case -15: return 7;
    default: return 10;
    }
}
static bool event_add(uint16_t op, uint32_t request, uint32_t transfer,
                      uint32_t status, uint16_t length, int error,
                      uint32_t why, struct net_buf *payload, uint64_t time,
                      bool terminal)
{
    k_spinlock_key_t key = k_spin_lock(&s.lock);
    unsigned limit = 8 - ((s.tx_active && !terminal) ? 1 : 0);
    if (s.count >= limit || s.seq == UINT32_MAX) {
        s.overflow++;
        k_spin_unlock(&s.lock, key);
        return false;
    }
    struct event *e = &s.events[(s.head + s.count++) % 8];
    memset(e, 0, sizeof(*e));
    e->op = op; e->request = request; e->generation = s.generation;
    e->time = time; e->length = op == 259 ? 32 : 24; e->payload = payload;
    sys_put_le32(++s.seq, e->body);
    sys_put_le32(s.route, e->body + 4);
    sys_put_le32(transfer, e->body + 8);
    sys_put_le32(status, e->body + 12);
    bool tx_event = request != 0;
    sys_put_le32(tx_event ? s.tx_addr.ext_id : s.rx_addr.ext_id, e->body + 16);
    e->body[20] = tx_event ? s.tx_addr.ext_addr : s.rx_addr.ext_addr;
    sys_put_le16(length, e->body + 22);
    if (op == 259) {
        sys_put_le32(why, e->body + 24);
        sys_put_le32((uint32_t)error, e->body + 28);
    }
    k_spin_unlock(&s.lock, key);
    return true;
}

bool ciso_rx_accept(struct isotp_recv_ctx *ctx, uint32_t length, bool multi)
{
    k_spinlock_key_t key = k_spin_lock(&s.lock);
    bool accept = !s.stopping && !s.rx_busy &&
                  s.count <= (s.tx_active ? 4 : 5) && s.rx_counter < UINT32_MAX;
    if (accept) {
        s.rx_busy = true;
        atomic_clear(&s.rx_terminal);
        ctx->ciso_transfer = ++s.rx_counter;
        ctx->ciso_time_us = now_us();
    } else { s.dropped_rx++; }
    k_spin_unlock(&s.lock, key);
    if (accept && multi) {
        event_add(257, 0, ctx->ciso_transfer, 2 | addr_status(), length,
                  0, 0, NULL, ctx->ciso_time_us, false);
    }
    return accept;
}
void ciso_rx_failed(struct isotp_recv_ctx *ctx, int error)
{
    if (!s.stopping && s.rx_busy && atomic_cas(&s.rx_terminal,0,1)) {
        event_add(259, 0, ctx->ciso_transfer, addr_status(), 0, error,
                  reason(error), NULL, now_us(), false);
        /* Released when the terminal event is acknowledged. */
    }
}
static void tx_complete(int error, void *unused)
{
    ARG_UNUSED(unused);
    atomic_set(&s.tx_result, error);
    atomic_set(&s.tx_finished, 1);
}
static int wire(uint8_t kind, uint16_t op, uint32_t session, uint32_t generation,
                uint32_t request, uint64_t time, const uint8_t *body, size_t length,
                struct net_buf *payload)
{
    uint8_t h[32] = {'C','I','S','O',1,kind};
    size_t data_length = payload ? net_buf_frags_len(payload) : 0;
    sys_put_le16(op,h+6); sys_put_le32(length+data_length,h+8);
    sys_put_le32(session,h+12); sys_put_le32(generation,h+16);
    sys_put_le32(request,h+20); sys_put_le64(time,h+24);
    if (s.write(h,32) || s.write(body,length)) { return -EIO; }
    for (struct net_buf *b=payload; b; b=b->frags) {
        if (s.write(b->data,b->len)) { return -EIO; }
    }
    return 0;
}
static void flush_events(void)
{
    for (unsigned i=0; i<s.count; i++) {
        struct event *e=&s.events[(s.head+i)%8];
        if (e->sent) { continue; }
        if (wire(3,e->op,s.session,e->generation,e->request,e->time,
                 e->body,e->length,e->payload)) { ciso_link_lost(); return; }
        e->sent=true; s.sent_seq=u32(e->body);
    }
}
static void state_events(void)
{
    if(!s.channel) { return; }
    enum can_state state; struct can_bus_err_cnt errors={0};
    if(can_get_state(s.can,&state,&errors)) { return; }
    uint32_t bus=state==CAN_STATE_STOPPED ? 0 : state==CAN_STATE_ERROR_ACTIVE ? 1 : state==CAN_STATE_ERROR_WARNING ? 2 : state==CAN_STATE_ERROR_PASSIVE ? 3 : state==CAN_STATE_BUS_OFF ? 4 : 5;
    k_spinlock_key_t key=k_spin_lock(&s.lock);
    if(s.count < 7 && s.seq<UINT32_MAX && bus!=s.reported_bus) {
        struct event *e=&s.events[(s.head+s.count++)%8]; memset(e,0,sizeof(*e));
        e->op=260; e->generation=s.generation; e->time=now_us(); e->length=16;
        sys_put_le32(++s.seq,e->body); sys_put_le32(bus,e->body+4);
        sys_put_le32(errors.tx_err_cnt,e->body+8); sys_put_le32(errors.rx_err_cnt,e->body+12);
        s.reported_bus=bus;
    }
    if(s.count < 7 && s.seq<UINT32_MAX && (s.overflow!=s.reported_overflow || s.dropped_rx!=s.reported_dropped)) {
        struct event *e=&s.events[(s.head+s.count++)%8]; memset(e,0,sizeof(*e));
        e->op=261; e->generation=s.generation; e->time=now_us(); e->length=12;
        sys_put_le32(++s.seq,e->body); sys_put_le32(s.overflow,e->body+4);sys_put_le32(s.dropped_rx,e->body+8);
        s.reported_overflow=s.overflow; s.reported_dropped=s.dropped_rx;
    }
    k_spin_unlock(&s.lock,key);
}
static void release_events(void)
{
    while (s.count) {
        struct event *e=&s.events[s.head];
        if (e->payload) { net_buf_unref(e->payload); }
        s.head=(s.head+1)%8; s.count--;
    }
    if (s.rx_chain) { net_buf_unref(s.rx_chain); s.rx_chain=NULL; }
    s.rx_busy=false;
}
static void finish_tx(void)
{
    if (!s.tx_active || !atomic_cas(&s.tx_finished,1,0)) { return; }
    int error=atomic_get(&s.tx_result);
    if(atomic_get(&s.forced_stop)) { s.tx_abort_reason=atomic_get(&s.forced_stop); error=-15; }
    uint32_t status=9 | (s.tx_flags & 0x180);
    event_add(error ? 259 : 256,s.tx_request,s.tx_transfer,status,0,error,
              s.tx_abort_reason ? s.tx_abort_reason : reason(error),NULL,
              now_us(),true);
    s.tx_active=false;
}
/* Stop purges hardware TX requests via the bounded M_CAN patch. No ACK is sent
 * until both the hardware callbacks and the ISO-TP worker have quiesced. */
static int abort_tx(uint32_t why)
{
    if (!s.tx_active) { return 0; }
    s.tx_abort_reason=why;
    ciso_isotp_cancel(&s.tx_ctx);
    int err=stop_can();
    if (err && err!=-EALREADY) { return err; }
    uint64_t limit=now_us()+1100000;
    while (!atomic_get(&s.tx_finished) && now_us()<limit) { k_sleep(K_MSEC(1)); }
    if (!atomic_get(&s.tx_finished)) { return -ETIMEDOUT; }
    finish_tx();
    if (s.channel && !s.stopping) { return can_start(s.can); }
    return 0;
}
static int stop_channel(void)
{
    s.stopping=true;
    memset(s.periodic,0,sizeof(s.periodic));
    int err;
    if (s.tx_active && (err=abort_tx(7))) { return err; }
    if (s.channel && (err=stop_can()) && err!=-EALREADY) { return err; }
    if (s.bound) { isotp_unbind(&s.rx_ctx); s.bound=false; }
    s.channel=false; s.route=0; s.stopping=false;
    return 0;
}
void ciso_link_lost(void)
{
    s.header_used=0; s.body_used=0;
    /* Preserve callback/context storage and reject new work if stop failed. */
    if(stop_channel()) { return; }
    release_events();
    atomic_clear(&s.forced_stop);
    s.session=0; s.generation=0; s.last_generation=0; s.last_request=0;
    s.header_used=0; s.body_used=0;
}
static int submit(uint32_t request, uint32_t transfer, uint32_t flags,
                  uint16_t length, uint32_t deadline)
{
    s.tx_request=request; s.tx_transfer=transfer;
    s.tx_flags=flags; s.tx_abort_reason=0;
    s.tx_deadline=deadline ? now_us()+(uint64_t)deadline*1000 : 0;
    atomic_clear(&s.tx_finished);
    s.tx_active=true;
    struct isotp_msg_id tx=s.tx_addr;
    tx.flags=(tx.flags & ~BIT(5)) | ((flags & 0x40) ? BIT(5) : 0);
    ciso_isotp_share_rx(&s.rx_ctx,&s.tx_ctx);
    return isotp_send(&s.tx_ctx,s.can,s.tx,length,&tx,&s.rx_addr,tx_complete,NULL);
}
static uint32_t session_scope(uint16_t op)
{
    return op<=5 || op==80 || op==81;
}
static void command(void)
{
    const uint8_t *h=s.header,*b=s.body;
    uint16_t op=sys_get_le16(h+6);
    uint32_t session=u32(h+12),gen=u32(h+16),req=u32(h+20),n=s.body_size;
    uint32_t result=0;
    uint8_t out[80]={0}; unsigned out_len=4;
    if (h[4]!=1) { result=1; goto reply; }
    if (h[5]!=1 || sys_get_le64(h+24)!=0 || req==0) { result=6; goto reply; }
    if (op==1) {
        if (n!=16) { result=2; goto reply; }
        if (session || gen || u32(b+12) || u32(b+8)<500 || u32(b+8)>30000) { result=6; goto reply; }
        if (s.session) { result=7; goto reply; }
        if (s.next_session==UINT32_MAX) { result=8; goto reply; }
        s.lease_ms=u32(b+8);
        /* First fresh host nonce assigns a boot-lifetime token. The token stays
         * constant across subsequent sessions until MCU reset. */
        if(!s.boot) { s.boot=sys_get_le64(b); if(!s.boot) { s.boot=now_us()|1; } }
        renew_lease(); s.session=++s.next_session;
        s.last_request=req; s.seq=0; s.ack=0; s.sent_seq=0; s.rx_counter=0;
        memcpy(out+4,b,8); sys_put_le64(s.boot,out+12);
        sys_put_le32(s.session,out+20); sys_put_le32(s.lease_ms,out+24);
        sys_put_le32(1,out+28); out_len=32;
        goto reply;
    }
    if (!s.session || session!=s.session) { result=3; goto reply; }
    if (req<=s.last_request) { result=11; goto reply; }
    s.last_request=req;
    if(s.stopping && op!=2 && op!=3 && op!=4 && op!=5 && op!=17 && op!=81) {
        result=10; goto reply;
    }
    if(op!=2 && op!=3 && op!=4 && op!=5 && op!=16 && op!=17 && op!=18 &&
       op!=32 && op!=33 && op!=48 && op!=49 && op!=50 && op!=64 && op!=65 && op!=80 && op!=81) {
        result=5; goto reply;
    }
    if (session_scope(op)) {
        if (gen) { result=4; goto reply; }
    } else if (op==16) {
        if (!gen || gen<=s.last_generation) { result=4; goto reply; }
    } else if (!s.channel || gen!=s.generation) { result=4; goto reply; }
    switch (op) {
    case 2: {
        if (n) { result=2; break; }
        sys_put_le64(0x7f,out+4); /* EA, IDE, RX_START, padding, cancel, periodic */
        uint32_t caps[16]={4095,1,1,1,1,1,2,64,8,4160,500000,1000,1000,1000,2,10};
        for (unsigned i=0;i<16;i++) { sys_put_le32(caps[i],out+12+i*4); }
        out_len=76; break;
    }
    case 3:
        if (n) { result=2; break; }
        renew_lease(); break;
    case 4:
        if (n) { result=2; break; }
        if(stop_channel()) { result=10; break; }
        flush_events(); break;
    case 5:
        if (n) { result=2; break; }
        hwinfo_get_device_id(out+4,12);
        memcpy(out+16,"ciso-0.2.1",10); memcpy(out+36,"stage2-development",18);
        memcpy(out+56,"mks-canable-v20",15); out_len=72; break;
    case 16:
        if (n!=12) { result=2; break; }
        if (s.channel) { result=7; break; }
        if ((u32(b)!=7 && u32(b)!=0x8005) || u32(b+4) || u32(b+8)!=500000) { result=5; break; }
        if (can_set_mode(s.can,CAN_MODE_NORMAL) || can_set_bitrate(s.can,500000) || can_start(s.can)) { result=10; break; }
        s.channel=true; s.reported_bus=UINT32_MAX; s.generation=gen; s.last_generation=gen; break;
    case 17:
        if (n) { result=2; break; }
        if(stop_channel()) { result=10; break; }
        flush_events(); sys_put_le32(s.seq,out+4); out_len=8; break;
    case 18:
        if (n!=20) { result=2; break; }
        if (s.tx_active || s.rx_busy) { result=7; break; }
        if (u32(b)!=1000 || u32(b+4)!=1000 || u32(b+8)!=1000 || sys_get_le16(b+12)!=10) { result=5; break; }
        if (!valid_stmin(b[15]) || u32(b+16)) { result=6; break; }
        s.opts.bs=b[14]; s.opts.stmin=b[15];
        if(s.bound) {
            isotp_unbind(&s.rx_ctx); memset(&s.rx_ctx,0,sizeof(s.rx_ctx));
            if(isotp_bind(&s.rx_ctx,s.can,&s.rx_addr,&s.tx_addr,&s.opts,K_NO_WAIT)) { s.bound=false; result=8; }
        }
        break;
    case 32: {
        if (n!=24) { result=2; break; }
        if (s.bound || s.tx_active || s.rx_busy) { result=7; break; }
        uint32_t af=u32(b+12),flags=u32(b+16),mask=(af&2)?0x1fffffff:0x7ff;
        if (!u32(b) || af>3 || u32(b+4)>mask || u32(b+8)>mask ||
            (flags&~0x1c0U) || ((flags&0x80)!=((af&1)?0x80:0)) ||
            ((flags&0x100)!=((af&2)?0x100:0)) || !valid_stmin(b[23]) ||
            (!(af&1) && (b[20] || b[21]))) { result=6; break; }
        memset(&s.rx_ctx,0,sizeof(s.rx_ctx));
        s.rx_addr=(struct isotp_msg_id){.ext_id=u32(b+4),.ext_addr=b[20],.flags=((af&1)?1:0)|((af&2)?4:0)};
        s.tx_addr=(struct isotp_msg_id){.ext_id=u32(b+8),.ext_addr=b[21],.flags=s.rx_addr.flags|((flags&0x40)?BIT(5):0)};
        s.opts.bs=b[22]; s.opts.stmin=b[23];
        if (isotp_bind(&s.rx_ctx,s.can,&s.rx_addr,&s.tx_addr,&s.opts,K_NO_WAIT)) { result=8; break; }
        s.route=u32(b); s.bound=true; break;
    }
    case 33:
        if (n!=4) { result=2; break; }
        if (!s.bound || u32(b)!=s.route) { result=6; break; }
        if (s.tx_active || s.rx_busy) { result=7; break; }
        isotp_unbind(&s.rx_ctx); s.bound=false;
        flush_events(); sys_put_le32(s.seq,out+4); out_len=8; s.route=0; break;
    case 48: {
        if (n<16 || sys_get_le16(b+10) || sys_get_le16(b+8)!=n-16 || n-16<1 || n-16>4095) { result=2; break; }
        if (!s.bound || u32(b)!=s.route) { result=6; break; }
        if (s.discard_tx || s.tx_active) { result=7; break; }
        if ((u32(b+4)&~0x1c0U) || ((u32(b+4)&0x180)!=addr_status())) { result=6; break; }
        if (s.count>=6 || s.rx_busy) { result=8; break; }
        int err=submit(req,req,u32(b+4),n-16,u32(b+12));
        if (err) { s.tx_active=false; result=10; }
        break;
    }
    case 49:
        if (n!=4) { result=2; break; }
        if (!s.tx_active || u32(b)!=s.tx_request) { result=6; break; }
        if (abort_tx(7)) { result=10; break; }
        flush_events(); sys_put_le32(s.seq,out+4); out_len=8; break;
    case 50: {
        if (n!=4) { result=2; break; }
        uint32_t bits=u32(b);
        if (!bits || (bits&~15U)) { result=6; break; }
        if ((bits&8) && s.tx_active && !(bits&1)) { result=7; break; }
        if ((bits&1) && abort_tx(7)) { result=10; break; }
        if (bits&4) { memset(s.periodic,0,sizeof(s.periodic)); }
        if ((bits&10) && s.bound) {
            if (bits&8) { isotp_unbind(&s.rx_ctx); s.bound=false; }
            if (s.rx_busy && atomic_cas(&s.rx_terminal,0,1)) {
                event_add(259,0,s.rx_ctx.ciso_transfer,addr_status(),0,-15,7,NULL,now_us(),false);
            }
            if (s.rx_chain) { net_buf_unref(s.rx_chain); s.rx_chain=NULL; }
            if (!(bits&8)) {
                if (ciso_isotp_reset_rx(&s.rx_ctx)) { result=10; break; }
                s.bound=true;
            } else { s.route=0; }
        }
        flush_events(); sys_put_le32(s.seq,out+4); out_len=8; break;
    }
    case 64: {
        if (n<20 || sys_get_le16(b+18) || sys_get_le16(b+16)!=n-20 || n-20<1 || n-20>64) { result=2; break; }
        if (!s.bound || u32(b+4)!=s.route || !u32(b) || u32(b+8)<5000 || u32(b+8)>65535000 ||
            (u32(b+12)&~0x1c0U) || ((u32(b+12)&0x180)!=addr_status())) { result=6; break; }
        int slot=-1;
        for (int i=0;i<2;i++) { if (s.periodic[i].id==u32(b)) { result=6; break; } if (!s.periodic[i].id) { slot=i; } }
        if (result) { break; }
        if (slot<0) { result=8; break; }
        s.periodic[slot].id=u32(b); s.periodic[slot].request=req;
        s.periodic[slot].interval=u32(b+8); s.periodic[slot].flags=u32(b+12);
        s.periodic[slot].length=n-20;
        memcpy(s.periodic[slot].data,b+20,n-20);
        s.periodic[slot].next=now_us()+s.periodic[slot].interval;
        break;
    }
    case 65:
        if (n!=4) { result=2; break; }
        result=6;
        for(int i=0;i<2;i++) { if(s.periodic[i].id==u32(b)) { memset(&s.periodic[i],0,sizeof(s.periodic[i])); result=0; } }
        break;
    case 80: {
        if (n!=4) { result=2; break; }
        uint32_t ack=u32(b);
        if (ack<s.ack || ack>s.sent_seq) { result=6; break; }
        k_spinlock_key_t key=k_spin_lock(&s.lock);
        while (s.count && u32(s.events[s.head].body)<=ack) {
            struct event *e=&s.events[s.head];
            if (e->payload) { net_buf_unref(e->payload); }
            if ((e->op==258 || e->op==259) && e->request==0) { s.rx_busy=false; }
            s.head=(s.head+1)%8; s.count--;
        }
        s.ack=ack; k_spin_unlock(&s.lock,key); break;
    }
    case 81: {
        if (n) { result=2; break; }
        enum can_state state; struct can_bus_err_cnt errors={0};
        if (can_get_state(s.can,&state,&errors)) { result=10; break; }
        uint32_t bus=state==CAN_STATE_STOPPED ? 0 : state==CAN_STATE_ERROR_ACTIVE ? 1 : state==CAN_STATE_ERROR_WARNING ? 2 : state==CAN_STATE_ERROR_PASSIVE ? 3 : state==CAN_STATE_BUS_OFF ? 4 : 5;
        uint32_t status[6]={bus,errors.tx_err_cnt,errors.rx_err_cnt,s.overflow,s.dropped_rx,(uint32_t)MAX(0,(int64_t)(s.lease_end-now_us())/1000)};
        for(unsigned i=0;i<6;i++) { sys_put_le32(status[i],out+4+i*4); } out_len=28; break;
    }
    default: result=5;
    }
reply:
    sys_put_le32(result,out);
    if (result) { out_len=4; }
    if (wire(2,op,session,gen,req,now_us(),out,out_len,NULL)) { ciso_link_lost(); }
    if (op==4 && !result) { ciso_link_lost(); }
}

void ciso_feed(const uint8_t *bytes, size_t length)
{
    /* A pending USB IN event can delay the main worker past expiry. Reset old
     * parser ownership before consuming the first fragment of a new HELLO. */
    if(s.session && now_us()>=s.lease_end) { ciso_link_lost(); }
    while (length) {
        if(!s.session && s.header_used<4) {
            /* A USB OUT packet queued by an expired owner may arrive late.
             * Search for a fresh header only while no session owns the stream. */
            uint8_t byte=*bytes++; length--;
            if(byte==(uint8_t)"CISO"[s.header_used]) {
                s.header[s.header_used++]=byte;
            } else {
                s.header_used=byte=='C' ? 1 : 0; s.header[0]='C';
            }
            if(s.header_used<4) { continue; }
            s.parse_deadline=now_us()+5000000;
        }
        if (s.header_used<32) {
            if(!s.header_used) { s.parse_deadline=now_us()+(s.session ? s.lease_ms : 5000)*1000ULL; }
            size_t take=MIN(length,32-s.header_used);
            memcpy(s.header+s.header_used,bytes,take);
            s.header_used+=take; bytes+=take; length-=take;
            if(s.header_used<32) { continue; }
            if(memcmp(s.header,"CISO",4) || u32(s.header+8)>4160) { ciso_link_lost(); return; }
            s.body_size=u32(s.header+8); s.body_used=0;
            s.discard_tx=s.tx_active;
            if(!s.session && sys_get_le16(s.header+6)!=1) {
                /* Reject a stale request now; its missing body must not consume
                 * the next owner's HELLO as if it were old TX payload. */
                command(); s.header_used=0; s.body_used=0;
                return;
            }
        }
        uint16_t op=sys_get_le16(s.header+6);
        unsigned prefix=op==48 ? 16 : 84;
        size_t take=MIN(length,s.body_size-s.body_used);
        for(size_t i=0;i<take;i++) {
            uint32_t at=s.body_used+i;
            if(at<prefix && at<84) { s.body[at]=bytes[i]; }
            else if (op==48 && !s.discard_tx && at-prefix<4095) { s.tx[at-prefix]=bytes[i]; }
        }
        s.body_used+=take; bytes+=take; length-=take;
        if(s.body_used==s.body_size) {
            command(); s.header_used=0; s.body_used=0;
        }
    }
}
void ciso_poll(void)
{
    if(s.header_used && now_us()>=s.parse_deadline) { ciso_link_lost(); return; }
    if(s.session && now_us()>=s.lease_end) { ciso_link_lost(); return; }
    if(s.tx_active && s.tx_deadline && now_us()>=s.tx_deadline) { abort_tx(1); }
    if(atomic_get(&s.forced_stop) && atomic_get(&s.tx_finished)) {
        finish_tx();
        if(s.channel && atomic_get(&s.forced_stop)!=7) { can_start(s.can); }
        atomic_clear(&s.forced_stop);
    }
    finish_tx();
    if(s.bound) {
        struct net_buf *buf=NULL;
        int rem=isotp_recv_net(&s.rx_ctx,&buf,K_NO_WAIT);
        if(rem>=0 && buf) {
            if(s.rx_chain) { net_buf_frag_add(s.rx_chain,buf); } else { s.rx_chain=buf; }
            if(rem==0) {
                atomic_set(&s.rx_terminal,1);
                uint16_t len=net_buf_frags_len(s.rx_chain);
                if(!event_add(258,0,s.rx_ctx.ciso_transfer,addr_status()|s.rx_ctx.ciso_status,
                              len,0,0,s.rx_chain,now_us(),false)) {
                    net_buf_unref(s.rx_chain); s.overflow++; s.rx_busy=false;
                }
                s.rx_chain=NULL;
            }
        }
    }
    if(s.session) { state_events(); flush_events(); }
    for(unsigned i=0;i<2;i++) {
        if(!s.periodic[i].id || now_us()<s.periodic[i].next) { continue; }
        s.periodic[i].next=now_us()+s.periodic[i].interval;
        if(s.tx_active || s.rx_busy || s.count>=6 || s.header_used) { s.overflow++; continue; }
        if(s.periodic[i].instance==UINT32_MAX) { s.overflow++; continue; }
        memcpy(s.tx,s.periodic[i].data,s.periodic[i].length);
        int err=submit(s.periodic[i].request,++s.periodic[i].instance,s.periodic[i].flags,s.periodic[i].length,1000);
        if(err) { s.tx_active=false; s.overflow++; }
    }
}
void ciso_init(const struct device *can, ciso_write_fn write_fn)
{
    memset(&s,0,sizeof(s)); s.can=can; s.write=write_fn;
    s.boot=0;
    k_work_init_delayable(&watchdog,watchdog_tick);
    k_work_reschedule(&watchdog,K_MSEC(10));
}
