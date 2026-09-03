/* SPDX-License-Identifier: MIT */
#include "chrony-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#define DEFAULT_LOCAL_PATH_FORMAT "/tmp/gptp-refclock%d.sock"
#define SOCK_MAGIC 0x534f434b

struct sock_sample {
    struct timeval tv;
    double offset;
    int pulse;
    int leap;
    int _pad;
    int magic;
};

struct chrony_client {
    int sock_fd;
    char local_path[256];
    char remote_path[256];
};

chrony_client_t *chrony_client_create(const char *local_path_format, const char *remote_path) {
    if (remote_path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (strlen(remote_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "Remote chrony socket path is too long (maximum %zu bytes)\n",
                sizeof(((struct sockaddr_un *)0)->sun_path) - 1);
        errno = ENAMETOOLONG;
        return NULL;
    }
    
    chrony_client_t *client = malloc(sizeof(chrony_client_t));
    if (client == NULL) {
        return NULL;
    }
    
    client->sock_fd = -1;
    client->local_path[0] = '\0';
    strcpy(client->remote_path, remote_path);
    
    if (local_path_format == NULL) {
        local_path_format = DEFAULT_LOCAL_PATH_FORMAT;
    }
    
    pid_t pid = getpid();
    int n = snprintf(client->local_path, sizeof(client->local_path), local_path_format, pid);
    if (n < 0 || (size_t)n >= sizeof(client->local_path) ||
        (size_t)n >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "Local chrony socket path is too long (maximum %zu bytes)\n",
                sizeof(((struct sockaddr_un *)0)->sun_path) - 1);
        free(client);
        errno = ENAMETOOLONG;
        return NULL;
    }
    
    /* Remove any existing socket */
    unlink(client->local_path);
    
    /* Create socket */
    client->sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (client->sock_fd < 0) {
        perror("socket");
        free(client);
        return NULL;
    }
    int flags = fcntl(client->sock_fd, F_GETFL);
    if (flags < 0 || fcntl(client->sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(O_NONBLOCK)");
        close(client->sock_fd);
        free(client);
        return NULL;
    }
    
    /* Bind to local path */
    struct sockaddr_un local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sun_family = AF_UNIX;
    strcpy(local_addr.sun_path, client->local_path);
    
    /* bind creates the socket with 0660 permissions, without a pathname
     * operation that could follow a replacement symlink. */
    mode_t old_umask = umask(0117);
    int bind_result = bind(client->sock_fd, (struct sockaddr *)&local_addr, sizeof(local_addr));
    int bind_errno = errno;
    umask(old_umask);
    if (bind_result < 0) {
        errno = bind_errno;
        perror("bind");
        close(client->sock_fd);
        unlink(client->local_path);
        free(client);
        return NULL;
    }

    return client;
}

static int send_sock_sample(chrony_client_t *client, const struct timeval *tv, double offset, int leap) {
    if (client == NULL || client->sock_fd < 0 || tv == NULL) {
        return -1;
    }
    
    struct sock_sample sample;
    memset(&sample, 0, sizeof(sample));
    sample.tv = *tv;
    sample.offset = offset;
    sample.pulse = 0;
    sample.leap = leap;
    sample.magic = SOCK_MAGIC;
    
    struct sockaddr_un remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sun_family = AF_UNIX;
    strcpy(remote_addr.sun_path, client->remote_path);
    
    ssize_t sent = sendto(client->sock_fd, &sample, sizeof(sample), 0,
                         (struct sockaddr *)&remote_addr, sizeof(remote_addr));
    if (sent < 0) {
        return -1;
    }
    
    return 0;
}

int chrony_client_send_sample(chrony_client_t *client, const struct timeval *tv, double offset, int leap) {
    /* A complete sample: chrony adds the offset to the system time to get
     * the true time, so the caller passes true minus system */
    return send_sock_sample(client, tv, offset, leap);
}

const char *chrony_client_remote_path(chrony_client_t *client) {
    return client ? client->remote_path : NULL;
}

const char *chrony_client_local_path(chrony_client_t *client) {
    return client ? client->local_path : NULL;
}

void chrony_client_destroy(chrony_client_t *client) {
    if (client == NULL) {
        return;
    }
    
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
    }
    
    if (strlen(client->local_path) > 0) {
        unlink(client->local_path);
    }
    
    free(client);
}
