/* SPDX-License-Identifier: MIT */
#ifndef CHRONY_CLIENT_H
#define CHRONY_CLIENT_H

#include <sys/time.h>

typedef struct chrony_client chrony_client_t;

/* Create a new chrony client
 * local_path_format: format string for local socket path (must contain %d for PID)
 * remote_path: path to chrony socket
 * Returns NULL on error
 */
chrony_client_t *chrony_client_create(const char *local_path_format, const char *remote_path);

/* Send a complete time sample to chrony: the source knows which second the
 * event belongs to, so chrony needs no other source to use it
 * client: chrony client instance
 * tv: system time when the event was detected
 * offset: true time minus system time (in seconds), chrony's convention
 *         for a complete SOCK sample and the same as satpulse sends; a PPS
 *         sample (pulse = 1) would carry the opposite sign, the fraction of
 *         the system second, which chrony negates
 * leap: 0 normal, 1 leap second to be inserted, 2 to be deleted
 * Returns 0 on success, -1 on error
 */
int chrony_client_send_sample(chrony_client_t *client, const struct timeval *tv, double offset, int leap);

/* Get the remote socket path */
const char *chrony_client_remote_path(chrony_client_t *client);

/* Get the local socket path */
const char *chrony_client_local_path(chrony_client_t *client);

/* Destroy chrony client and cleanup sockets */
void chrony_client_destroy(chrony_client_t *client);

#endif /* CHRONY_CLIENT_H */