/* Host stub of src/kernel/driver.h: the driver interface drvlib.c uses. */
struct device {
  unsigned long dv_base;
  unsigned long dv_size;
};
#define NIL_DEV	((struct device *) 0)
struct driver {
  char *(*dr_name)(void);
  struct device *(*dr_prepare)(int device);
};
