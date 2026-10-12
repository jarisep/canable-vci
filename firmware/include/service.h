/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <stdint.h>
#include <stddef.h>
typedef int (*ciso_write_fn)(const uint8_t *bytes, size_t length);
void ciso_init(const struct device *can, ciso_write_fn write_fn);
void ciso_feed(const uint8_t *bytes, size_t length);
void ciso_poll(void);
void ciso_link_lost(void);
int ciso_usb_init(void);
struct net_buf *ciso_usb_read(k_timeout_t timeout);
void ciso_usb_read_done(struct net_buf *buf);
int ciso_usb_write(const uint8_t *bytes, size_t length);
bool ciso_usb_enabled(void);
