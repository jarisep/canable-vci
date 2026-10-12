/* SPDX-License-Identifier: Apache-2.0 */
#include "service.h"
#include <zephyr/devicetree.h>
int main(void)
{
    const struct device *can = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
    if (!device_is_ready(can)) { return -ENODEV; }
    ciso_init(can, ciso_usb_write);
    int err = ciso_usb_init();
    if (err) { return err; }
    bool previous = false;
    for (;;) {
        bool enabled = ciso_usb_enabled();
        if (previous && !enabled) { ciso_link_lost(); }
        previous = enabled;
        struct net_buf *buf = ciso_usb_read(K_MSEC(1));
        if (buf) {
            if (enabled) { ciso_feed(buf->data, buf->len); }
            ciso_usb_read_done(buf);
        }
        ciso_poll();
    }
}
