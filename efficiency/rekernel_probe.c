// Read-only protocol probe for the unmodified official Re:Kernel Generic Netlink ABI.
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int query(int fd, unsigned type, unsigned cmd, unsigned seq,
                 unsigned attr, const char *value, unsigned *family) {
    unsigned char request[256] = {0}, response[16384];
    struct nlmsghdr *header = (struct nlmsghdr *)request;
    struct genlmsghdr *generic = (struct genlmsghdr *)(request + NLMSG_HDRLEN);
    header->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
    header->nlmsg_type = type;
    header->nlmsg_flags = NLM_F_REQUEST;
    header->nlmsg_seq = seq;
    generic->cmd = cmd;
    generic->version = 1;
    if (value) {
        size_t length = strlen(value) + 1;
        struct nlattr *a = (struct nlattr *)(request + header->nlmsg_len);
        a->nla_type = attr;
        a->nla_len = NLA_HDRLEN + length;
        memcpy((unsigned char *)a + NLA_HDRLEN, value, length);
        header->nlmsg_len += NLA_ALIGN(a->nla_len);
    }
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(fd, request, header->nlmsg_len, 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
        perror("sendto"); return -1;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        ssize_t size = recv(fd, response, sizeof(response), 0);
        if (size < 0) { perror("recv"); return -1; }
        int remaining = size;
        for (struct nlmsghdr *h = (struct nlmsghdr *)response;
             NLMSG_OK(h, (unsigned int)remaining); h = NLMSG_NEXT(h, remaining)) {
            if (h->nlmsg_seq != seq) continue;
            if (h->nlmsg_type == NLMSG_ERROR) {
                if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -1;
                const struct nlmsgerr *error = (const struct nlmsgerr *)NLMSG_DATA(h);
                if (error->error) { errno = -error->error; perror("netlink"); return -1; }
                continue;
            }
            if (h->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) return -1;
            int left = h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
            const unsigned char *cursor = (const unsigned char *)NLMSG_DATA(h) + GENL_HDRLEN;
            while (left >= NLA_HDRLEN) {
                const struct nlattr *a = (const struct nlattr *)cursor;
                if (a->nla_len < NLA_HDRLEN || a->nla_len > left) return -1;
                unsigned attribute = a->nla_type & NLA_TYPE_MASK;
                size_t length = a->nla_len - NLA_HDRLEN;
                const unsigned char *data = cursor + NLA_HDRLEN;
                if (family && attribute == CTRL_ATTR_FAMILY_ID && length == 2) {
                    uint16_t id; memcpy(&id, data, 2); *family = id;
                    printf("family=rekernel id=%u\n", *family);
                    return 0;
                }
                if (!family && attribute == 1 && length) {
                    printf("version_response=%.*s\n", (int)length, data);
                    return 0;
                }
                unsigned step = NLA_ALIGN(a->nla_len);
                cursor += step; left -= step;
            }
        }
    }
    fprintf(stderr, "No matching ReKernel reply\n");
    return -1;
}

int main(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) { perror("socket"); return 1; }
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind"); close(fd); return 1;
    }
    unsigned family = 0;
    int rc = query(fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1,
                   CTRL_ATTR_FAMILY_NAME, "rekernel", &family);
    // Official enum: UNSPEC=0, EVENT=1, ADD=2, DEL=3, GET_VERSION=4.
    if (!rc) rc = query(fd, family, 4, 2, 0, NULL, NULL);
    close(fd);
    return rc ? 1 : 0;
}
