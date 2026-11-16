/* Host stub of flipper/hal/hal.h: the USB part. */
int hal_usb_init(void);
int hal_usb_irq(void);
void hal_usb_poll(void);
int hal_usb_connected(void);
int hal_usb_awake(void);
unsigned hal_usb_write(const void *buf, unsigned n);
unsigned hal_usb_room(void);
int hal_usb_getc(void);
int hal_usb_peekc(void);
