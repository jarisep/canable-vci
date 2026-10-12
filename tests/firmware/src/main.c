/* SPDX-License-Identifier: Apache-2.0 */
#include "service.h"
#include <zephyr/ztest.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

static bool started, hold;
static int inject_error,stop_error;
static struct can_frame frames[1024];
static uint64_t frame_times[1024];
static unsigned frame_count;
static struct { can_rx_callback_t cb; void *arg; struct can_filter filter; } filters[4];
static struct { can_tx_callback_t cb; void *arg; } pending[8];
static const struct device *fake;
static int caps(const struct device *d,can_mode_t *c) { *c=CAN_MODE_LOOPBACK; return 0; }
static int start(const struct device *d) { started=true; return 0; }
static int stop(const struct device *d)
{
    if(stop_error) { return stop_error; }
    started=false;
    for(unsigned i=0;i<8;i++) { if(pending[i].cb) { can_tx_callback_t cb=pending[i].cb; pending[i].cb=NULL; cb(d,-ENETDOWN,pending[i].arg); } }
    return 0;
}
static int mode(const struct device *d,can_mode_t m) { return 0; }
static int timing(const struct device *d,const struct can_timing *t) { return 0; }
static int clock(const struct device *d,uint32_t *c) { *c=16000000; return 0; }
static int state(const struct device *d,enum can_state *st,struct can_bus_err_cnt *cnt)
{
    *st=started ? CAN_STATE_ERROR_ACTIVE : CAN_STATE_STOPPED;
    if(cnt) { memset(cnt,0,sizeof(*cnt)); } return 0;
}
static int send(const struct device *d,const struct can_frame *f,k_timeout_t t,can_tx_callback_t cb,void *arg)
{
    if(!started) { return -ENETDOWN; }
    zassert_true(frame_count<ARRAY_SIZE(frames));
    frames[frame_count]=*f; frame_times[frame_count++]=k_ticks_to_us_floor64(k_uptime_ticks());
    if(hold) {
        for(unsigned i=0;i<8;i++) { if(!pending[i].cb) { pending[i].cb=cb; pending[i].arg=arg; return 0; } }
        return -EAGAIN;
    }
    cb(d,inject_error,arg); return 0;
}
static int add(const struct device *d,can_rx_callback_t cb,void *arg,const struct can_filter *f)
{
    for(unsigned i=0;i<4;i++) { if(!filters[i].cb) { filters[i].cb=cb; filters[i].arg=arg; filters[i].filter=*f; return i; } }
    return -ENOSPC;
}
static void remove_filter(const struct device *d,int id) { filters[id].cb=NULL; }
static const struct can_driver_config cfg={.min_bitrate=10000,.max_bitrate=1000000};
static struct can_driver_data data;
static DEVICE_API(can,fake_api)={.get_capabilities=caps,.start=start,.stop=stop,.set_mode=mode,
    .set_timing=timing,.send=send,.add_rx_filter=add,.remove_rx_filter=remove_filter,
    .get_state=state,.get_core_clock=clock,
    .timing_min={.sjw=1,.phase_seg1=1,.phase_seg2=1,.prescaler=1},
    .timing_max={.sjw=4,.phase_seg1=16,.phase_seg2=8,.prescaler=1024}};
DEVICE_DEFINE(fake_can,"fake_can",NULL,NULL,&data,&cfg,POST_KERNEL,80,&fake_api);
static uint8_t wire_bytes[30000];
static size_t wire_length;
static uint32_t session,request;
static int emit(const uint8_t *b,size_t n)
{
    zassert_true(wire_length+n<=sizeof(wire_bytes)); memcpy(wire_bytes+wire_length,b,n); wire_length+=n; return 0;
}
static uint8_t *find_message(uint16_t op,uint8_t kind)
{
    uint8_t *last=NULL;
    for(size_t at=0;at+32<=wire_length;) {
        uint8_t *h=wire_bytes+at; uint32_t n=sys_get_le32(h+8);
        zassert_true(at+32+n<=wire_length);
        if(sys_get_le16(h+6)==op && h[5]==kind) { last=h+32; }
        at+=32+n;
    }
    return last;
}
static void cmd(uint16_t op,uint32_t gen,const uint8_t *body,size_t n,bool fragmented)
{
    uint8_t h[32]={'C','I','S','O',1,1};
    sys_put_le16(op,h+6); sys_put_le32(n,h+8); sys_put_le32(op==1 ? 0 : session,h+12);
    sys_put_le32(gen,h+16); sys_put_le32(++request,h+20);
    if(fragmented) { for(unsigned i=0;i<32;i++) { ciso_feed(h+i,1); } for(size_t i=0;i<n;i++) { ciso_feed(body+i,1); } }
    else { ciso_feed(h,32); if(n) { ciso_feed(body,n); } }
}
static void ok(uint16_t op) { uint8_t *b=find_message(op,2); zassert_not_null(b); zassert_equal(sys_get_le32(b),0); }
static void tick(unsigned ms)
{
    for(unsigned i=0;i<ms;i++) { k_sleep(K_MSEC(1)); ciso_poll(); }
}
static void peer(const uint8_t *b,uint8_t n)
{
    struct can_frame f={.id=0x652,.dlc=n}; memcpy(f.data,b,n);
    for(unsigned i=0;i<4;i++) { if(filters[i].cb && can_frame_matches_filter(&f,&filters[i].filter)) { filters[i].cb(fake,&f,filters[i].arg); break; } }
}
static void channel(uint8_t bs,bool ext)
{
    uint8_t b[24]={0}; sys_put_le32(0x8005,b); sys_put_le32(500000,b+8);
    cmd(16,1,b,12,true); ok(16);
    memset(b,0,sizeof(b)); sys_put_le32(1,b); sys_put_le32(0x652,b+4); sys_put_le32(0x752,b+8);
    if(ext) { sys_put_le32(1,b+12); sys_put_le32(0x80,b+16); b[20]=0x41; b[21]=0x48; }
    b[22]=bs; cmd(32,1,b,24,true); ok(32);
}
static void tx(const uint8_t *payload,uint16_t n,uint32_t flags)
{
    static uint8_t b[4111]; memset(b,0,16); sys_put_le32(1,b); sys_put_le32(flags,b+4); sys_put_le16(n,b+8);
    memcpy(b+16,payload,n); cmd(48,1,b,n+16,true); ok(48);
}
static void *setup(void)
{
    fake=DEVICE_GET(fake_can); ciso_init(fake,emit); return NULL;
}
static void before(void *unused)
{
    stop_error=0;
    ciso_link_lost(); memset(filters,0,sizeof(filters)); memset(pending,0,sizeof(pending));
    wire_length=0; frame_count=0; hold=false; inject_error=0; request=0;
    uint8_t b[16]={0}; sys_put_le64(0x1122334455667788,b); sys_put_le32(5000,b+8);
    cmd(1,0,b,16,true); ok(1); session=sys_get_le32(find_message(1,2)+20);
}
ZTEST(ciso,test_caps_and_no_traffic)
{
    cmd(2,0,NULL,0,false); ok(2); zassert_equal(sys_get_le32(find_message(2,2)+12),4095);
    zassert_equal(frame_count,0);
}
ZTEST(ciso,test_sf_completion_and_padding)
{
    channel(0,false); uint8_t b[]={0x10,3}; tx(b,2,0x40); tick(3);
    zassert_equal(frame_count,1); zassert_equal(frames[0].dlc,8); zassert_equal(frames[0].data[7],0);
    uint8_t *e=find_message(256,3); zassert_not_null(e); zassert_equal(sys_get_le32(e+12),9);
}
ZTEST(ciso,test_can_error_is_not_success)
{
    channel(0,false); inject_error=-EIO; uint8_t b[]={0x10,3}; tx(b,2,0); tick(3);
    zassert_is_null(find_message(256,3)); zassert_not_null(find_message(259,3));
}
ZTEST(ciso,test_rx_sf_extended)
{
    channel(0,true); uint8_t b[]={0x41,3,0x7f,0x10,0x12}; peer(b,5); tick(3);
    uint8_t *e=find_message(258,3); zassert_not_null(e); zassert_equal(sys_get_le32(e+12),0x90);
    zassert_equal(sys_get_le16(e+22),3); zassert_mem_equal(e+24,b+2,3); zassert_is_null(find_message(257,3));
}
static void receive_long(uint8_t bs)
{
    channel(bs,false); uint8_t f[8]={0x1f,0xff,0,1,2,3,4,5}; peer(f,8); tick(2);
    zassert_not_null(find_message(257,3)); zassert_equal(frames[0].data[0],0x30);
    uint32_t at=6; uint8_t sn=1;
    while(at<4095) {
        f[0]=0x20 | (sn++ & 15); unsigned n=MIN(7,4095-at);
        for(unsigned i=0;i<n;i++) { f[1+i]=(at+i)&255; }
        peer(f,n+1); at+=n;
        if(bs && ((sn-1)%bs)==0) { tick(2); }
    }
    tick(3); uint8_t *e=find_message(258,3); zassert_not_null(e);
    zassert_equal(sys_get_le16(e+22),4095);
    for(unsigned i=0;i<4095;i++) { zassert_equal(e[24+i],i&255); }
    /* Holding RX until ACK rejects a second message without corrupting the first. */
    uint8_t sf[]={1,0x55}; peer(sf,2); tick(2);
    uint8_t ack[4]; sys_put_le32(sys_get_le32(e),ack); cmd(80,0,ack,4,false); ok(80);
    peer(sf,2); tick(2); e=find_message(258,3); zassert_equal(sys_get_le16(e+22),1); zassert_equal(e[24],0x55);
}
ZTEST(ciso,test_rx_4095_bs_zero) { receive_long(0); }
ZTEST(ciso,test_rx_4095_bs_eight) { receive_long(8); }
ZTEST(ciso,test_wrong_sequence)
{
    channel(0,false); uint8_t ff[]={0x10,9,1,2,3,4,5,6}; peer(ff,8); tick(2);
    uint8_t cf[]={0x22,7,8,9}; peer(cf,4); tick(2);
    zassert_is_null(find_message(258,3)); uint8_t *e=find_message(259,3); zassert_not_null(e); zassert_equal(sys_get_le32(e+24),4);
}
ZTEST(ciso,test_missing_fc)
{
    channel(0,false); uint8_t b[14]={0}; tx(b,14,0); tick(1100);
    zassert_is_null(find_message(256,3)); uint8_t *e=find_message(259,3); zassert_not_null(e); zassert_equal(sys_get_le32(e+24),2);
}
ZTEST(ciso,test_cancel_quiesces_hardware)
{
    channel(0,false); hold=true; uint8_t b[14]={0}; tx(b,14,0); uint32_t target=request; tick(2);
    uint8_t cancel[4]; sys_put_le32(target,cancel); cmd(49,1,cancel,4,false); ok(49);
    unsigned count=frame_count; uint8_t fc[]={0x30,0,0}; peer(fc,3); tick(10);
    zassert_equal(frame_count,count); zassert_is_null(find_message(256,3));
    uint8_t *e=find_message(259,3); zassert_not_null(e); zassert_equal(sys_get_le32(e+24),7);
}
ZTEST(ciso,test_lease_stops_pending_tx)
{
    channel(0,false); hold=true; uint8_t b[14]={0}; tx(b,14,0); tick(5100);
    zassert_false(started); unsigned count=frame_count; tick(20); zassert_equal(frame_count,count);
    cmd(2,0,NULL,0,false); zassert_equal(sys_get_le32(find_message(2,2)),3);
}
ZTEST(ciso,test_runtime_timeout_rejected)
{
    channel(0,false); uint8_t b[20]={0}; sys_put_le32(2000,b); sys_put_le32(1000,b+4); sys_put_le32(1000,b+8);sys_put_le16(10,b+12);
    cmd(18,1,b,20,false); zassert_equal(sys_get_le32(find_message(18,2)),5);
}
ZTEST_SUITE(ciso,NULL,setup,before,NULL,NULL);

ZTEST(ciso,test_tx_multiframe_block_and_stmin)
{
    channel(0,false); uint8_t b[40]={0}; tx(b,40,0); tick(2);
    zassert_equal(frame_count,1); zassert_equal(frames[0].data[0],0x10);
    uint8_t fc[]={0x30,2,5}; peer(fc,3); tick(10);
    zassert_equal(frame_count,3); zassert_true(frame_times[2]-frame_times[1]>=5000);
    peer(fc,3); tick(10); zassert_equal(frame_count,5);
    peer(fc,3); tick(10); zassert_not_null(find_message(256,3));
    zassert_is_null(find_message(259,3));
}
ZTEST(ciso,test_tx_4095_and_zero_padding)
{
    channel(0,false); static uint8_t b[4095]; for(unsigned i=0;i<4095;i++) { b[i]=i&255; }
    tx(b,4095,0x40); tick(2); uint8_t fc[]={0x30,0,0}; peer(fc,3); tick(5);
    zassert_not_null(find_message(256,3)); zassert_equal(frame_count,586);
    zassert_equal(frames[0].data[0],0x1f);zassert_equal(frames[0].data[1],0xff);
    zassert_equal(frames[585].dlc,8);
}
ZTEST(ciso,test_short_fc_does_not_disable_timeout)
{
    channel(0,false); uint8_t b[14]={0}; tx(b,14,0);tick(2);
    uint8_t fc[]={0x30};peer(fc,1);tick(60);
    zassert_not_null(find_message(259,3));zassert_is_null(find_message(256,3));
}
ZTEST(ciso,test_wait_limit_and_peer_overflow)
{
    channel(0,false);uint8_t b[14]={0};tx(b,14,0);tick(2);
    uint8_t fc[]={0x31,0,0};
    for(unsigned i=0;i<11;i++) { peer(fc,3);tick(1); }
    uint8_t *e=find_message(259,3);zassert_not_null(e);zassert_equal(sys_get_le32(e+24),6);
}
ZTEST(ciso,test_periodic_is_bounded_and_stops)
{
    channel(0,false);uint8_t b[22]={0};sys_put_le32(1,b);sys_put_le32(1,b+4);sys_put_le32(100000,b+8);sys_put_le16(2,b+16);b[20]=0x3e;
    cmd(64,1,b,22,true);ok(64);tick(20);zassert_true(frame_count>0);
    uint8_t id[4];sys_put_le32(1,id);cmd(65,1,id,4,false);ok(65);
    unsigned n=frame_count;tick(20);zassert_equal(frame_count,n);
}
ZTEST(ciso,test_request_replay_and_stale_generation)
{
    channel(0,false);uint8_t h[32]={'C','I','S','O',1,1};sys_put_le16(3,h+6);sys_put_le32(session,h+12);sys_put_le32(request,h+20);
    ciso_feed(h,32);zassert_equal(sys_get_le32(find_message(3,2)),11);
    cmd(17,1,NULL,0,false);ok(17);
    uint8_t b[12]={0};sys_put_le32(7,b);sys_put_le32(500000,b+8);cmd(16,1,b,12,false);
    zassert_equal(sys_get_le32(find_message(16,2)),4);
}
ZTEST(ciso,test_unplug_and_reconnect)
{
    channel(0,false);hold=true;uint8_t b[14]={0};tx(b,14,0);tick(2);ciso_link_lost();
    unsigned count=frame_count;tick(5);zassert_equal(frame_count,count);zassert_false(started);
    uint8_t hello[16]={0};sys_put_le32(5000,hello+8);cmd(1,0,hello,16,true);ok(1);
    uint32_t old=session;session=sys_get_le32(find_message(1,2)+20);zassert_not_equal(old,session);
    cmd(2,0,NULL,0,false);ok(2);
}
ZTEST(ciso,test_oversize_header_and_unknown_opcode)
{
    cmd(0x7777,0,NULL,0,false);zassert_equal(sys_get_le32(find_message(0x7777,2)),5);
    uint8_t h[32]={'C','I','S','O',1,1};sys_put_le16(48,h+6);sys_put_le32(4161,h+8);
    ciso_feed(h,32);cmd(2,0,NULL,0,false);zassert_equal(sys_get_le32(find_message(2,2)),3);zassert_equal(frame_count,0);
}
ZTEST(ciso,test_rx_timeout_after_first_frame)
{
    channel(0,false);uint8_t ff[]={0x10,9,1,2,3,4,5,6};peer(ff,8);tick(60);
    zassert_not_null(find_message(257,3));uint8_t *e=find_message(259,3);zassert_not_null(e);zassert_equal(sys_get_le32(e+24),3);zassert_is_null(find_message(258,3));
}
ZTEST(ciso,test_event_backpressure_preserves_tx_buffer)
{
    channel(0,false);uint8_t payload[]={0x10,3};
    for(unsigned i=0;i<5;i++) { tx(payload,2,0); tick(1); }
    uint8_t b[18]={0};sys_put_le32(1,b);sys_put_le16(2,b+8);b[16]=0x22;b[17]=0xff;
    unsigned before_count=frame_count;cmd(48,1,b,18,true);
    zassert_equal(sys_get_le32(find_message(48,2)),8);tick(2);zassert_equal(frame_count,before_count);
}
ZTEST(ciso,test_tx_extended_wrong_fc_address_times_out)
{
    channel(0,true);uint8_t payload[14]={0};tx(payload,14,0x80);tick(2);
    zassert_equal(frames[0].data[0],0x48);
    uint8_t fc[]={0x42,0x30,0,0};peer(fc,4);tick(60);
    uint8_t *e=find_message(259,3);zassert_not_null(e);zassert_equal(sys_get_le32(e+24),2);zassert_is_null(find_message(256,3));
}
ZTEST(ciso,test_peer_overflow_is_terminal)
{
    channel(0,false);uint8_t payload[14]={0};tx(payload,14,0);tick(2);uint8_t fc[]={0x32,0,0};peer(fc,3);tick(2);
    uint8_t *e=find_message(259,3);zassert_not_null(e);zassert_equal(sys_get_le32(e+24),5);zassert_is_null(find_message(256,3));
}
ZTEST(ciso,test_no_can_ack_times_out)
{
    channel(0,false);hold=true;uint8_t payload[]={0x22,0xf0,0x80};tx(payload,3,0);tick(60);
    uint8_t *e=find_message(259,3);zassert_not_null(e);zassert_equal(sys_get_le32(e+24),1);zassert_is_null(find_message(256,3));
}
ZTEST(ciso,test_clear_rx_preserves_active_tx)
{
    channel(0,false);hold=true;uint8_t payload[14]={0};tx(payload,14,0);tick(2);
    uint8_t mask[4];sys_put_le32(2,mask);cmd(50,1,mask,4,false);ok(50);
    zassert_is_null(find_message(259,3));zassert_true(started);
    uint8_t cancel[4];sys_put_le32(request-1,cancel); /* TX request preceded CLEAR */
    cmd(49,1,cancel,4,false);ok(49);
}
ZTEST(ciso,test_expired_lease_resets_parser_before_new_hello)
{
    ciso_link_lost();uint8_t hello[16]={0};sys_put_le64(77,hello);sys_put_le32(500,hello+8);
    cmd(1,0,hello,16,true);ok(1);session=sys_get_le32(find_message(1,2)+20);
    uint32_t old=session;
    k_sleep(K_MSEC(600)); /* Watchdog runs, main worker has not polled. */
    cmd(1,0,hello,16,true);ok(1);
    zassert_not_equal(sys_get_le32(find_message(1,2)+20),old);
}
ZTEST(ciso,test_stop_failure_is_not_acknowledged_as_success)
{
    channel(0,false);stop_error=-EIO;
    cmd(17,1,NULL,0,false);zassert_equal(sys_get_le32(find_message(17,2)),10);
    zassert_true(started);
    uint8_t payload[18]={0};sys_put_le32(1,payload);sys_put_le16(2,payload+8);
    cmd(48,1,payload,sizeof(payload),false);zassert_equal(sys_get_le32(find_message(48,2)),10);
    zassert_equal(frame_count,0);
    stop_error=0;cmd(17,1,NULL,0,false);ok(17);zassert_false(started);
}
ZTEST(ciso,test_clear_completed_rx_does_not_emit_second_terminal)
{
    channel(0,false);uint8_t sf[]={3,0x62,0xf0,0x80};peer(sf,4);tick(2);
    zassert_not_null(find_message(258,3));zassert_is_null(find_message(259,3));
    uint8_t mask[4];sys_put_le32(2,mask);cmd(50,1,mask,4,false);ok(50);
    zassert_is_null(find_message(259,3));
}
ZTEST(ciso,test_late_old_usb_packet_cannot_consume_new_hello)
{
    uint32_t old=session;ciso_link_lost();
    uint8_t late[39]={'C','I','S','O',1,1};sys_put_le16(48,late+6);
    sys_put_le32(100,late+8);sys_put_le32(old,late+12);sys_put_le32(1,late+16);sys_put_le32(99,late+20);
    ciso_feed(late,sizeof(late));
    zassert_equal(sys_get_le32(find_message(48,2)),3);
    uint8_t junk[7]={0};ciso_feed(junk,sizeof(junk));
    uint8_t hello[16]={0};sys_put_le64(78,hello);sys_put_le32(500,hello+8);
    cmd(1,0,hello,16,true);ok(1);zassert_equal(frame_count,0);
}

/* Synthetic requests only: emulate the controller's first-match RX delivery. */
ZTEST(ciso,test_shared_fc_filter_and_late_fc)
{
    channel(0,false);
    for (unsigned attempt=0; attempt<3; attempt++) {
        uint8_t b[21]={0}; tx(b,sizeof(b),0); tick(2);
        unsigned count=0;
        for (unsigned i=0;i<4;i++) { count += filters[i].cb != NULL; }
        zassert_equal(count,1,"TX must reuse the persistent RX filter");
        uint8_t fc[]={0x30,0,0}; peer(fc,3); tick(4);
        zassert_not_null(find_message(256,3));
        peer(fc,3); tick(2); /* Late FC must not become an RX error/message. */
        zassert_is_null(find_message(259,3));
        zassert_is_null(find_message(258,3));
        wire_length=0;
    }
    uint8_t sf[]={3,0x62,0xf0,0x80}; peer(sf,4); tick(2);
    zassert_not_null(find_message(258,3));
}
ZTEST(ciso,test_shared_fc_after_clear_rx)
{
    channel(0,false); uint8_t b[13]={0}; tx(b,sizeof(b),0); tick(2);
    uint8_t mask[4]; sys_put_le32(2,mask); cmd(50,1,mask,4,false); ok(50);
    uint8_t fc[]={0x30,0,0}; peer(fc,3); tick(4);
    zassert_not_null(find_message(256,3)); zassert_is_null(find_message(259,3));
}
ZTEST(ciso,test_shared_fc_extended_success)
{
    channel(0,true); uint8_t b[21]={0}; tx(b,sizeof(b),0x80); tick(2);
    uint8_t wrong[]={0x42,0x30,0,0}; peer(wrong,4); tick(2);
    zassert_equal(frame_count,1);
    uint8_t fc[]={0x41,0x30,0,0}; peer(fc,4); tick(4);
    zassert_not_null(find_message(256,3)); zassert_is_null(find_message(259,3));
}

ZTEST(ciso,test_shared_rx_clear_partial_response)
{
    channel(0,false);
    uint8_t ff[]={0x10,9,1,2,3,4,5,6}; peer(ff,8); tick(2);
    zassert_not_null(find_message(257,3));
    uint8_t mask[4]; sys_put_le32(2,mask); cmd(50,1,mask,4,false); ok(50);
    uint8_t *terminal=find_message(259,3); zassert_not_null(terminal);
    uint8_t ack[4]; sys_put_le32(sys_get_le32(terminal),ack);
    cmd(80,0,ack,4,false); ok(80); /* Terminal event owns RX until acknowledged. */
    uint8_t sf[]={3,0x62,0xf0,0x80}; peer(sf,4); tick(2);
    uint8_t *e=find_message(258,3); zassert_not_null(e);
    zassert_mem_equal(e+24,sf+1,3);
}
