/* SPDX-License-Identifier: Apache-2.0 */
#include "service.h"
#include <zephyr/ztest.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/sys/byteorder.h>
#include <usbh_ch9.h>
#include <usbh_device.h>
USBH_CONTROLLER_DEFINE(host, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));
static struct usb_device *peer;
static void *setup(void)
{
    zassert_ok(usbh_init(&host));
    zassert_ok(usbh_enable(&host));
    zassert_ok(uhc_bus_reset(host.dev));
    zassert_ok(uhc_bus_resume(host.dev));
    zassert_ok(uhc_sof_enable(host.dev));
    zassert_ok(ciso_usb_init(), "USB initialization must succeed");
    k_msleep(200);
    peer=usbh_device_get_any(&host);
    zassert_not_null(peer, "Virtual host must enumerate the USB device");
    return NULL;
}
ZTEST(ciso_usb, test_device_descriptor)
{
    struct usb_device_descriptor descriptor;
    zassert_ok(k_mutex_lock(&peer->mutex,K_MSEC(200)));
    zassert_ok(usbh_req_desc_dev(peer,sizeof(descriptor),&descriptor));
    k_mutex_unlock(&peer->mutex);
    zassert_equal(sys_le16_to_cpu(descriptor.idVendor),0x1209);
    zassert_equal(sys_le16_to_cpu(descriptor.idProduct),0x0001);
    zassert_equal(sys_le16_to_cpu(descriptor.bcdUSB),USB_SRN_2_0_1);
}
ZTEST(ciso_usb, test_winusb_descriptors)
{
    struct net_buf *buf=usbh_xfer_buf_alloc(peer,256);
    zassert_not_null(buf);
    zassert_ok(k_mutex_lock(&peer->mutex,K_MSEC(200)));
    zassert_ok(usbh_req_desc(peer,USB_DESC_BOS,0,0,256,buf));
    zassert_equal(buf->data[4],2,"USB extension and Microsoft OS capability");
    net_buf_reset(buf);
    zassert_ok(usbh_req_setup(peer,0xc0,0x20,0,7,256,buf));
    zassert_equal(sys_get_le16(buf->data+8),buf->len,"MS OS set length");
    zassert_mem_equal(buf->data+14,"WINUSB",6);
    const char guid[]="{5D4E7C68-9BC1-4E2B-9D68-5C9AB801D9F2}";
    /* Set header 10, compatible ID 20, property header/name 52. */
    for(unsigned i=0;i<sizeof(guid)-1;i++) {
        zassert_equal(buf->data[82+i*2],guid[i]);
        zassert_equal(buf->data[83+i*2],0);
    }
    k_mutex_unlock(&peer->mutex);
    usbh_xfer_buf_free(peer,buf);
}
ZTEST(ciso_usb, test_configuration_lifecycle)
{
    zassert_ok(k_mutex_lock(&peer->mutex,K_MSEC(200)));
    zassert_ok(usbh_req_set_cfg(peer,1));
    zassert_true(ciso_usb_enabled());
    zassert_ok(usbh_req_set_cfg(peer,0));
    zassert_false(ciso_usb_enabled());
    zassert_is_null(ciso_usb_read(K_NO_WAIT));
    zassert_ok(usbh_req_set_cfg(peer,1));
    zassert_true(ciso_usb_enabled());
    k_mutex_unlock(&peer->mutex);
}
ZTEST_SUITE(ciso_usb, NULL, setup, NULL, NULL, NULL);
