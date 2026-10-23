#ifndef HERA_STORE_H
#define HERA_STORE_H

/* Persist / read the address of the last connected phone, so the device can
 * reconnect to it after a reboot without the user doing anything. */
void store_save_last_device(const char *address);

/* Returns a malloc'd address string, or NULL if none is stored. Caller frees. */
char *store_load_last_device(void);

/* ".../dev_AA_BB_CC_DD_EE_FF" -> malloc'd "AA:BB:CC:DD:EE:FF" (or NULL). */
char *store_address_from_path(const char *path);

/* "AA:BB:CC:DD:EE:FF" -> malloc'd "dev_AA_BB_CC_DD_EE_FF" (or NULL). */
char *store_path_tail_from_address(const char *address);

/* True when `s` contains only characters legal in a Bluetooth address or an
 * interface name, so it is safe to embed in a shell command. */
int store_is_safe_token(const char *s);

#endif /* HERA_STORE_H */
