/* Modified by CANable VCI contributors; see firmware/vendor/PROVENANCE.md. */
/* Derived from CANnectivity USB setup and Zephyr's loopback class.
 * Copyright (c) 2024-2026 Henrik Brix Andersen;
 * Copyright (c) 2022, 2025 Nordic Semiconductor ASA.
 * SPDX-License-Identifier: Apache-2.0 */
#include "service.h"
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
static struct {
    struct usb_if_descriptor interface;
    struct usb_ep_descriptor out, in;
    struct usb_desc_header end;
} desc={
    .interface={.bLength=sizeof(struct usb_if_descriptor),.bDescriptorType=USB_DESC_INTERFACE,
                .bNumEndpoints=2,.bInterfaceClass=0xff,.bInterfaceSubClass=0x49,.bInterfaceProtocol=1},
    .out={.bLength=sizeof(struct usb_ep_descriptor),.bDescriptorType=USB_DESC_ENDPOINT,
          .bEndpointAddress=0x01,.bmAttributes=USB_EP_TYPE_BULK,.wMaxPacketSize=64},
    .in={.bLength=sizeof(struct usb_ep_descriptor),.bDescriptorType=USB_DESC_ENDPOINT,
         .bEndpointAddress=0x81,.bmAttributes=USB_EP_TYPE_BULK,.wMaxPacketSize=64},
};
static const struct usb_desc_header *const descriptors[]={
    (void*)&desc.interface,(void*)&desc.out,(void*)&desc.in,(void*)&desc.end
};
static struct usbd_class_data *class_data;
static atomic_t enabled, in_error;
K_FIFO_DEFINE(out_fifo);
K_SEM_DEFINE(in_done,0,1);
static int out_start(void)
{
    if(!atomic_get(&enabled)) { return -ENODEV; }
    struct net_buf *buf=usbd_ep_buf_alloc(class_data,desc.out.bEndpointAddress,64);
    if(!buf) { return -ENOMEM; }
    int err=usbd_ep_enqueue(class_data,buf);
    if(err) { net_buf_unref(buf); }
    return err;
}
static int request(struct usbd_class_data *data, struct net_buf *buf, int err)
{
    ARG_UNUSED(data);
    if(udc_get_buf_info(buf)->ep==desc.out.bEndpointAddress) {
        if(!err && atomic_get(&enabled)) { k_fifo_put(&out_fifo,buf); }
        else { net_buf_unref(buf); }
    } else {
        atomic_set(&in_error,err); net_buf_unref(buf); k_sem_give(&in_done);
    }
    return 0;
}
static const void *get_desc(struct usbd_class_data *data, enum usbd_speed speed)
{
    ARG_UNUSED(data); ARG_UNUSED(speed); return descriptors;
}
static void enable(struct usbd_class_data *data)
{
    class_data=data; atomic_set(&enabled,1); out_start();
}
static void disable(struct usbd_class_data *data)
{
    atomic_clear(&enabled);
    usbd_ep_dequeue(usbd_class_get_ctx(data),desc.in.bEndpointAddress);
    usbd_ep_dequeue(usbd_class_get_ctx(data),desc.out.bEndpointAddress);
    struct net_buf *buf;
    while((buf=k_fifo_get(&out_fifo,K_NO_WAIT))) { net_buf_unref(buf); }
    atomic_set(&in_error,-ENODEV); k_sem_give(&in_done);
}
static int class_init(struct usbd_class_data *data)
{
    class_data=data;
    return 0;
}
static const struct usbd_class_api api={.init=class_init,.request=request,.get_desc=get_desc,.enable=enable,.disable=disable};
USBD_DEFINE_CLASS(ciso_0,&api,NULL,NULL);
/* Generic pid.codes test PID, private development only. */
USBD_DEVICE_DEFINE(device,DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),0x1209,0x0001);
USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(manufacturer,"CANable VCI development");
USBD_DESC_PRODUCT_DEFINE(product,"CANable ISO-TP development");
USBD_DESC_SERIAL_NUMBER_DEFINE(serial);
USBD_DESC_CONFIG_DEFINE(config_desc,"ISO-TP diagnostic interface");
USBD_CONFIGURATION_DEFINE(config,0,125,&config_desc);
static const struct usb_bos_capability_lpm usb_extension={
    .bLength=sizeof(struct usb_bos_capability_lpm),
    .bDescriptorType=USB_DESC_DEVICE_CAPABILITY,
    .bDevCapabilityType=USB_BOS_CAPABILITY_EXTENSION,
    .bmAttributes=0,
};
USBD_DESC_BOS_DEFINE(usbext,sizeof(usb_extension),&usb_extension);
struct os_descriptor {
    struct msosv2_descriptor_set_header header;
    struct msosv2_compatible_id compatible;
    struct msosv2_guids_property guid;
    struct msosv2_vendor_revision revision;
} __packed;
static struct os_descriptor os={
    .header={.wLength=sizeof(struct msosv2_descriptor_set_header),
             .wDescriptorType=MS_OS_20_SET_HEADER_DESCRIPTOR,.dwWindowsVersion=0x06030000,
             .wTotalLength=sizeof(struct os_descriptor)},
    .compatible={.wLength=sizeof(struct msosv2_compatible_id),.wDescriptorType=MS_OS_20_FEATURE_COMPATIBLE_ID,
                 .CompatibleID={'W','I','N','U','S','B',0,0}},
    .guid={.wLength=sizeof(struct msosv2_guids_property),.wDescriptorType=MS_OS_20_FEATURE_REG_PROPERTY,
           .wPropertyDataType=MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ,.wPropertyNameLength=42,
           .PropertyName={DEVICE_INTERFACE_GUIDS_PROPERTY_NAME},.wPropertyDataLength=80},
    .revision={.wLength=sizeof(struct msosv2_vendor_revision),.wDescriptorType=MS_OS_20_FEATURE_VENDOR_REVISION,
               .VendorRevision=1},
};
struct bos_msos { struct usb_bos_platform_descriptor platform; struct usb_bos_capability_msos cap; } __packed;
static const struct bos_msos bos={
    .platform={.bLength=sizeof(struct bos_msos),.bDescriptorType=USB_DESC_DEVICE_CAPABILITY,
               .bDevCapabilityType=USB_BOS_CAPABILITY_PLATFORM,
               .PlatformCapabilityUUID={0xdf,0x60,0xdd,0xd8,0x89,0x45,0xc7,0x4c,0x9c,0xd2,0x65,0x9d,0x9e,0x64,0x8a,0x9f}},
    .cap={.dwWindowsVersion=0x06030000,.wMSOSDescriptorSetTotalLength=sizeof(struct os_descriptor),.bMS_VendorCode=0x20},
};
static struct net_buf *os_handler(const struct usbd_context *ctx,const struct usb_setup_packet *setup)
{
    if(setup->bRequest!=0x20 || setup->wIndex!=MS_OS_20_DESCRIPTOR_INDEX) { return NULL; }
    uint16_t n=MIN(setup->wLength,sizeof(os));
    struct net_buf *buf=usbd_ep_ctrl_data_in_alloc(ctx,n);
    if(buf) { net_buf_add_mem(buf,&os,n); }
    return buf;
}
USBD_DESC_BOS_VREQ_DEFINE(msos,sizeof(bos),&bos,0x20,os_handler,NULL);
int ciso_usb_init(void)
{
    const char guid[]="{5D4E7C68-9BC1-4E2B-9D68-5C9AB801D9F2}";
    for(unsigned i=0;i<sizeof(guid)-1;i++) { os.guid.bPropertyData[i*2]=guid[i]; }
    int err;
    if((err=usbd_add_descriptor(&device,&lang)) ||
       (err=usbd_add_descriptor(&device,&manufacturer)) ||
       (err=usbd_add_descriptor(&device,&product)) ||
       (err=usbd_add_descriptor(&device,&serial)) ||
       (err=usbd_add_configuration(&device,USBD_SPEED_FS,&config)) ||
       (err=usbd_register_class(&device,"ciso_0",USBD_SPEED_FS,1)) ||
       (err=usbd_device_set_code_triple(&device,USBD_SPEED_FS,0,0,0)) ||
       (err=usbd_device_set_bcd_usb(&device,USBD_SPEED_FS,USB_SRN_2_0_1)) ||
       (err=usbd_add_descriptor(&device,&usbext)) ||
       (err=usbd_add_descriptor(&device,&msos)) ||
       (err=usbd_init(&device)) || (err=usbd_enable(&device))) { return err; }
    return 0;
}
bool ciso_usb_enabled(void) { return atomic_get(&enabled)!=0; }
struct net_buf *ciso_usb_read(k_timeout_t timeout) { return k_fifo_get(&out_fifo,timeout); }
void ciso_usb_read_done(struct net_buf *buf) { net_buf_unref(buf); out_start(); }
int ciso_usb_write(const uint8_t *bytes,size_t length)
{
    while(length) {
        if(!ciso_usb_enabled()) { return -ENODEV; }
        size_t n=MIN(length,64);
        struct net_buf *buf=usbd_ep_buf_alloc(class_data,desc.in.bEndpointAddress,n);
        if(!buf) { return -ENOMEM; }
        net_buf_add_mem(buf,bytes,n); k_sem_reset(&in_done);
        int err=usbd_ep_enqueue(class_data,buf);
        if(err) { net_buf_unref(buf); return err; }
        if(k_sem_take(&in_done,K_MSEC(1000))) {
            usbd_ep_dequeue(usbd_class_get_ctx(class_data),desc.in.bEndpointAddress);
            return -ETIMEDOUT;
        }
        if(atomic_get(&in_error)) { return atomic_get(&in_error); }
        bytes+=n; length-=n;
    }
    return 0;
}
