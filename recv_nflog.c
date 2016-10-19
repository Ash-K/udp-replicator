#include <sys/socket.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <libnetfilter_log/libnetfilter_log.h>
#include <libnfnetlink/libnfnetlink.h>
#include <stdlib.h>

#include "ntimed_tricks.h"
#include "recv_nflog.h"

/*
 * Callback from netfilter_log framework via nflog_handle_packet. Retrieve ip
 * src, proto, and payload and pass it on to regular processing.
 */
static int
cb_nflog(struct nflog_g_handle *group, struct nfgenmsg *nfmsg,
    struct nflog_data *nfad, void *ctx)
{
	struct recv_nflog *rcv;
	struct sockaddr_storage sastor;
	struct sockaddr_in *sin;
	socklen_t salen;
	struct nfulnl_msg_packet_hdr *pkt_hdr;
	int ethertype;
	char *packet;
	int packet_len;
	struct iphdr *ip;
	struct udphdr *udp;
	int proto;

	CAST_OBJ_NOTNULL(rcv, ctx, RECV_NFLOG_MAGIC);
	pkt_hdr = nflog_get_msg_packet_hdr(nfad);
	ethertype = htons(pkt_hdr->hw_protocol);
	packet_len = nflog_get_payload(nfad, &packet);
	AN(packet_len > 0);
	switch (ethertype) {
	case 0x0800:
		ip = (struct iphdr *)packet;
		AN(sizeof(*ip) <= packet_len);
		sin = (struct sockaddr_in *)&sastor;
		salen = sizeof(*sin);
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = ip->saddr;
		proto = ip->protocol;
		AN(ip->ihl * 4 <= packet_len);
		packet += ip->ihl * 4;
		packet_len -= ip->ihl * 4;
		AN(proto == 17 && sizeof(*udp) <= packet_len);
		udp = (struct udphdr *)packet;
		AN(ntohs(udp->uh_ulen) == packet_len);
		sin->sin_port = udp->uh_sport;
		packet += sizeof(*udp);
		packet_len -= sizeof(*udp);
		break;
	case 0x86dd:
		WRONG("IPv6 not implemented");
		return 0;
	default:
		WRONG("Bad ethertype received");
		return 0;
	}
	rcv->cb_func(packet, packet_len, (struct sockaddr *)&sastor, salen);
	return 0;
}

struct recv_nflog *
recv_nflog_new(int group, recv_nflog_cb_t cb_func, void *cb_ctx)
{
	struct recv_nflog *rcv;
	int opt;

	ALLOC_OBJ(rcv, RECV_NFLOG_MAGIC);
	AN(rcv);
	AN(rcv->nf_h = nflog_open());
	AZ(nflog_unbind_pf(rcv->nf_h, AF_INET));
	AZ(nflog_unbind_pf(rcv->nf_h, AF_INET6));
	AZ(nflog_bind_pf(rcv->nf_h, AF_INET));
	AN(rcv->nf_gh = nflog_bind_group(rcv->nf_h, group));
	AZ(nflog_set_mode(rcv->nf_gh, NFULNL_COPY_PACKET, 0xffff));
	AZ(nflog_set_nlbufsiz(rcv->nf_gh, 8192));
	AZ(nflog_set_timeout(rcv->nf_gh, 0));
	opt = 1;
	AZ(setsockopt(nflog_fd(rcv->nf_h), SOL_NETLINK, NETLINK_NO_ENOBUFS,
	    &opt, sizeof(opt)));
	AZ(nflog_callback_register(rcv->nf_gh, cb_nflog, rcv));
	rcv->cb_func = cb_func;
	rcv->cb_ctx = cb_ctx;
	return rcv;
}

void
recv_nflog_free(struct recv_nflog *rcv)
{

	CHECK_OBJ_NOTNULL(rcv, RECV_NFLOG_MAGIC);
	if (rcv->nf_h != NULL)
		nflog_close(rcv->nf_h);
	FREE_OBJ(rcv);
}

ssize_t
recv_nflog_packet(struct recv_nflog *rcv, char *packet, size_t packet_max,
    struct sockaddr *sa, socklen_t salen)
{
	char buf[8192];
	ssize_t len;

	CHECK_OBJ_NOTNULL(rcv, RECV_NFLOG_MAGIC);
	len = recv(nflog_fd(rcv->nf_h), buf, sizeof(buf), 0);
	AN(len > 0);
	nflog_handle_packet(rcv->nf_h, buf, len);
	return 0;
}
