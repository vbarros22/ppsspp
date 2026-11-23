// Wii U (wut) socket compatibility shims.
//
// The Wii U network stack is IPv4-only and lacks a few BSD socket APIs. This header is
// force-included (see ext/CMakeLists.txt) into the third-party networking code that
// expects them. IPv6 types exist so the code compiles, but AF_INET6 sockets simply fail
// at runtime.

#pragma once

#if defined(__WIIU__)

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef PF_INET6
#define PF_INET6 AF_INET6
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 26
#endif
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0
#endif
#ifndef SOMAXCONN
#define SOMAXCONN 8
#endif
// Not supported by the Wii U stack. Deliberately invalid option numbers, so setsockopt
// fails cleanly instead of silently setting some other option.
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 0x7fff1005
#endif
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 0x7fff1006
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_TRUNC
#define MSG_TRUNC 0x10
#endif
#ifndef POLLRDNORM
#define POLLRDNORM POLLIN
#endif
#ifndef POLLWRNORM
#define POLLWRNORM POLLOUT
#endif


// Socket types, protocols and options the PSP knows but the Wii U doesn't. Protocol numbers
// are the standard IANA ones. Options and flags get deliberately invalid values, so passing
// one to the Wii U fails instead of silently meaning something else.
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif
#ifndef SOCK_RDM
#define SOCK_RDM 4
#endif
#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif
#ifndef IPPROTO_IGMP
#define IPPROTO_IGMP 2
#endif
#ifndef IPPROTO_EGP
#define IPPROTO_EGP 8
#endif
#ifndef IPPROTO_PUP
#define IPPROTO_PUP 12
#endif
#ifndef IPPROTO_IDP
#define IPPROTO_IDP 22
#endif
#ifndef IPPROTO_RAW
#define IPPROTO_RAW 255
#endif
#ifndef IP_OPTIONS
#define IP_OPTIONS 0x7fff0001
#endif
#ifndef IP_HDRINCL
#define IP_HDRINCL 0x7fff0002
#endif
#ifndef SO_DEBUG
#define SO_DEBUG 0x7fff0003
#endif
#ifndef SO_ACCEPTCONN
#define SO_ACCEPTCONN 0x7fff0004
#endif
#ifndef MSG_WAITALL
#define MSG_WAITALL 0x40000000
#endif
#ifndef MSG_CTRUNC
#define MSG_CTRUNC 0x20000000
#endif
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

#ifndef WIIU_HAVE_IN6_ADDR
#define WIIU_HAVE_IN6_ADDR
struct in6_addr {
	union {
		uint8_t s6_addr[16];
		uint16_t s6_addr16[8];
		uint32_t s6_addr32[4];
	};
};

struct sockaddr_in6 {
	sa_family_t sin6_family;
	in_port_t sin6_port;
	uint32_t sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t sin6_scope_id;
};

static const struct in6_addr in6addr_any = { { { 0 } } };
static const struct in6_addr in6addr_loopback = { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };
#endif

#ifndef WIIU_HAVE_IOVEC
#define WIIU_HAVE_IOVEC
struct iovec {
	void *iov_base;
	size_t iov_len;
};
#endif

struct msghdr {
	void *msg_name;
	socklen_t msg_namelen;
	struct iovec *msg_iov;
	size_t msg_iovlen;
	void *msg_control;
	size_t msg_controllen;
	int msg_flags;
};

// sendmsg/recvmsg emulated with a bounce buffer and sendto/recvfrom. Only what enet needs:
// datagram sockets, no ancillary data.
static inline ssize_t wiiu_compat_sendmsg(int fd, const struct msghdr *msg, int flags) {
	size_t total = 0;
	for (size_t i = 0; i < msg->msg_iovlen; i++)
		total += msg->msg_iov[i].iov_len;
	uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
	if (!buf)
		return -1;
	size_t off = 0;
	for (size_t i = 0; i < msg->msg_iovlen; i++) {
		memcpy(buf + off, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
		off += msg->msg_iov[i].iov_len;
	}
	ssize_t ret = sendto(fd, buf, total, flags, (const struct sockaddr *)msg->msg_name, msg->msg_namelen);
	free(buf);
	return ret;
}

static inline ssize_t wiiu_compat_recvmsg(int fd, struct msghdr *msg, int flags) {
	size_t total = 0;
	for (size_t i = 0; i < msg->msg_iovlen; i++)
		total += msg->msg_iov[i].iov_len;
	// One extra byte to detect truncation.
	uint8_t *buf = (uint8_t *)malloc(total + 1);
	if (!buf)
		return -1;
	socklen_t namelen = msg->msg_namelen;
	ssize_t ret = recvfrom(fd, buf, total + 1, flags, (struct sockaddr *)msg->msg_name, msg->msg_name ? &namelen : NULL);
	msg->msg_flags = 0;
	if (ret >= 0) {
		if ((size_t)ret > total) {
			msg->msg_flags |= MSG_TRUNC;
			ret = (ssize_t)total;
		}
		size_t off = 0;
		for (size_t i = 0; i < msg->msg_iovlen && off < (size_t)ret; i++) {
			size_t n = msg->msg_iov[i].iov_len;
			if (n > (size_t)ret - off)
				n = (size_t)ret - off;
			memcpy(msg->msg_iov[i].iov_base, buf + off, n);
			off += n;
		}
		msg->msg_namelen = namelen;
	}
	free(buf);
	return ret;
}

#define sendmsg wiiu_compat_sendmsg
#define recvmsg wiiu_compat_recvmsg

#ifdef __cplusplus
}
#endif

#endif  // __WIIU__
