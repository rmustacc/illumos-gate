/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2019, Joyent, Inc. 
 */

/*
 * This file implements general TCP large receive offload (LRO).
 *
 * XXX Expand
 */

#include <sys/types.h>
#include <sys/strsubr.h>
#include <sys/strsun.h>
#include <sys/pattr.h>
#include <sys/kmem.h>
#include <sys/mac_impl.h>
#include <inet/tcp_impl.h>
#include <sys/sdt.h>

struct mac_lro_state_s {
	/*
	 * These fields track relevant state.
	 */
	mblk_t		*mls_head;
	mblk_t		*mls_tail;
	tcpha_t		*mls_tcp;

	/*
	 * These fields are kept in network endianness and not modified from the
	 * packet.
	 */
	in6_addr_t	mls_source;
	in6_addr_t	mls_dest;
	uint16_t	mls_lport;
	uint16_t	mls_fport;
	uint32_t	mls_ack;
	uint16_t	mls_window;
	uint32_t	mls_ipecn;
	/*
	 * These fields are kept in host endianness.
	 */
	size_t		mls_len;
	boolean_t	mls_v4;
	uint_t		mls_count;
	uint32_t	mls_exp_seq;
	uint32_t	mls_tsval;
	uint32_t	mls_tsecr;
	uint8_t		mls_tcp_flags;
	boolean_t	mls_ts_valid;
	boolean_t	mls_valid;
};

/*
 * Number of LRO entries to allocate for a soft ring.
 */
uint_t mac_lro_cache_size = 8;

/*
 * Rough statistics. We don't try and serialize these across CPUs, so they will
 * be lossy.
 */
uint_t mac_lro_slot_misses;

void
mac_lro_free(mac_lro_state_t *lrop, uint_t count)
{
	kmem_free(lrop, sizeof (mac_lro_state_t) * count);
}

void
mac_lro_alloc(mac_lro_state_t **lropp, uint_t *countp)
{
	*countp = mac_lro_cache_size;
	*lropp = kmem_zalloc(sizeof (mac_lro_state_t) * *countp,
	    KM_SLEEP);
}

static inline void
mac_lro_append_bnext(mblk_t *mp, mblk_t **head, mblk_t **tail)
{
	ASSERT3P(mp->b_next, ==, NULL);

	if (*head == NULL) {
		*head = mp;
	}

	if (*tail != NULL) {
		(*tail)->b_next = mp;
	}

	*tail = mp;
}

static inline void
mac_lro_append_bcont(mblk_t *mp, mblk_t **head, mblk_t **tail)
{
	if (*head == NULL) {
		*head = mp;
	}

	if (*tail != NULL) {
		(*tail)->b_cont = mp;
	}

	while (mp->b_cont != NULL) {
		mp = mp->b_cont;
	}
	ASSERT3P(mp, !=, NULL);
	*tail = mp;
}

static mac_lro_state_t *
mac_lro_find_free_slot(mac_lro_state_t *lrop, uint_t count)
{
	uint_t i;

	for (i = 0; i < count; i++, lrop++) {
		if (!lrop->mls_valid)
			return (lrop);
	}

	return (NULL);
}

static void
mac_lro_commit(mac_lro_state_t *lro, mblk_t **headp, mblk_t **tailp)
{
	tcpha_t *tcp;

	ASSERT3S(lro->mls_valid, ==, B_TRUE);
	ASSERT3U(lro->mls_count, >, 0);
	if (lro->mls_count == 1) {
		goto done;
	}

	/*
	 * We've joined multiple segments. This means that we need to update the
	 * following fields:
	 * 
	 *  o IP Payload
	 *  o IP Checksum (zero)
	 *  o TCP ACK
	 *  o TCP Window Size
	 *  o TCP Checksum (zero)
	 *  o Timestamp
	 *  o TCP flags
	 */
	tcp = lro->mls_tcp;
	ASSERT3U(lro->mls_len, <=, IP_MAXPACKET);
	if (lro->mls_v4) {
		ipha_t *ip = (ipha_t *)lro->mls_head->b_rptr;
		ip->ipha_length = htons((uint16_t)lro->mls_len);
		ip->ipha_hdr_checksum = 0;
	} else {
		ip6_t *ip = (ip6_t *)lro->mls_head->b_rptr;
		ip->ip6_plen = htons((uint16_t)lro->mls_len);
	}
	tcp->tha_ack = lro->mls_ack;
	tcp->tha_win = lro->mls_window;
	tcp->tha_sum = 0;
	if (lro->mls_ts_valid) {
		uint32_t *ts = (uint32_t *)(tcp + 1);
		ts[1] = htonl(lro->mls_tsval);
		ts[2] = htonl(lro->mls_tsecr);
	}
	tcp->tha_flags = lro->mls_tcp_flags;

done:
	DTRACE_PROBE2(mac__lro__commit, mac_lro_state_t *, lro,
	    mblk_t *, lro->mls_head);
	mac_lro_append_bnext(lro->mls_head, headp, tailp);
	ASSERT3P(*tailp, ==, lro->mls_head);
	ASSERT3P(lro->mls_tail->b_cont, ==, NULL);
	ASSERT3P(lro->mls_tail->b_next, ==, NULL);
	bzero(lro, sizeof (*lro));
}

#ifdef DEBUG
static void
mac_lro_verify_chain(mblk_t **head, mblk_t **tail)
{
	mblk_t *mp = *head;

	if (mp == NULL) {
		VERIFY3P(*tail, ==, NULL);
		return;
	}

	while (mp != NULL) {
		mblk_t *cont;

		cont = mp->b_cont;
		while (cont != NULL) {
			VERIFY3P(cont->b_next, ==, NULL);
			cont = cont->b_cont;
		}

		if (mp->b_next == NULL) {
			VERIFY3P(mp, ==, *tail);
		}
		mp = mp->b_next;
	}

	VERIFY3P((*tail)->b_next, ==, NULL);
	mp = *tail;
	while (mp->b_cont != NULL) {
		mp = mp->b_cont;
		VERIFY3P(mp->b_next, ==, NULL);
	}

	mp = *head;
	while (mp != NULL) {
		if (mp == *tail)
			break;
		mp = mp->b_next;
	}
	VERIFY3P(mp, !=, NULL);
}
#endif

/*
 * Perform software LRO on a stream of message blocks that exist in a chain.
 * This is commonly called from soft ring processing after fanout has occurred
 * to a protocol ring. We make the following assumptions only about the message
 * blocks:
 *
 *  o The packet has an IP + L4 header in the first message block. This property
 *    is currently maintained by all callers today.
 *
 *  o The L2 header has already been consumed by the mac_rx path and so the
 *    message blocks b_rptr starts at the IP header.
 *
 *  o We do _not_ assume that we will only encounter TCP data. While callers
 *    today make sure that we have TCP, we should assume that UDP or other types
 *    of IP packets may show up.
 *
 *  o We are given some number of LRO state structures to use.
 *
 * We will join TCP packets together under the following circumstances:
 *
 *  o The packet is TCP
 *  o The TCP packet checksummed has been confirmed by hardware
 *  o The IP checksum, if IPv4, has been confirmed by hardware
 *  o There are no IP options or extensions
 *  o The packets have the same 4-tuple
 *  o We have not already joined more than 64k of data.
 *  o No TCP flags other than ACK are present or PUSH are present.
 *  o TCP Sequence numbers match
 *  o If TCP options are present, the TCP option is the timestamp and we only
 *    see timestamps that are larger than the current. The previous packet must
 *    match the current packet with respect to options.
 *  o The TCP packet isn't an empty ack.
 *  o There is no urgent window set.
 *
 * The combined message block has the following properties in its headers:
 *
 *  o The IP Packet Length value is updated
 *  o The IP Checksum header is zeroed
 *  o The TCP header ACK is set to the last ACK seen
 *  o The TCP header flags are set to the combination of seen ACK/PUSH flags.
 *  o The TCP window is set to the last seen TCP window
 *  o If a TCP timestamp is present, the last timestamp is used.
 *
 * Finally, we will store a number of in-use LRO states based upon the passed in
 * state. Because this is occurring after protocol fanout (in most cases where
 * we'd care to multiple software rings), we assume that the likelihood of this
 * being a higher hit rate and therefore will traverse the lro state linearly.
 */
void
mac_sw_lro(mac_lro_state_t *lrop, uint_t lrocnt, mblk_t **mp_chain,
    mblk_t **tailp, int *cntp, size_t *sizep)
{
	uint_t i;
	mblk_t *mp, *next;
	mblk_t *head = NULL, *tail = NULL;
	mblk_t *free_head = NULL, *free_tail = NULL;
	mac_lro_state_t *l;

	if (lrop == NULL || lrocnt == 0)
		return;

	for (i = 0; i < lrocnt; i++) {
		lrop[i].mls_valid = B_FALSE;
	}

	mp = *mp_chain;
	while (mp != NULL) {
		boolean_t v4, ip_valid, tcp_valid, ts_valid;
		uint32_t flags, tsval, tsecr, ip_ecn;
		size_t tcp_len, msg_len;
		tcpha_t *tcp = NULL;
		ipha_t *ip4 = NULL;
		ip6_t *ip6 = NULL;

		/* Assume eligible until proven otherwise */
		ip_valid = tcp_valid = B_TRUE;
		/* Assume no TCP Timestamp until proven otherwise */
		ts_valid = B_FALSE;
		tsval = tsecr = 0;

		next = mp->b_next;
		mp->b_next = NULL;

		/*
		 * XXX this is because we can't handle this. We should make sure
		 * this is properly handled and causes things to get kicked out
		 * or we deal with tails correctly when appending b_cont.
		 */
		ASSERT3P(mp->b_cont, ==, NULL);

		mac_hcksum_get(mp, NULL, NULL, NULL, NULL, &flags);

		switch (IPH_HDR_VERSION(mp->b_rptr)) {
		case IP_VERSION:
			v4 = B_TRUE;
			ip4 = (ipha_t *)mp->b_rptr;
			if (ip4->ipha_protocol != IPPROTO_TCP)
				goto skip;
			if (IPH_HDR_LENGTH(ip4) != IP_SIMPLE_HDR_LENGTH ||
			    IS_V4_FRAGMENT(ip4->ipha_fragment_offset_and_flags) ||
			    (flags & HCK_IPV4_HDRCKSUM_OK) == 0) {
				ip_valid = B_FALSE;
			}
			ip_ecn = ip4->ipha_type_of_service;
			tcp = (tcpha_t *)(mp->b_rptr + IPH_HDR_LENGTH(ip4));
			break;
		case IPV6_VERSION:
			v4 = B_FALSE;
			ip6 = (ip6_t *)mp->b_rptr;
			/*
			 * Grab the entire first word for comparison and
			 * eligibility for LRO. In general, these should be the
			 * same for packets in a stream.
			 */
			ip_ecn = ip6->ip6_vcf;
			/*
			 * In theory it's possible for an IP option to exist and
			 * then be followed by a TCP header. If it isn't, it's
			 * inelligible. However, if there are options present,
			 * followed by TCP, then we need to make sure we
			 * terminate any outstanding TCP instance.
			 */
			if (ip6->ip6_nxt != IPPROTO_TCP) {
				size_t len = MBLKL(mp);
				size_t off = sizeof (ip6_t);
				uint8_t next = ip6->ip6_nxt;
				ip_valid = B_FALSE;
				while (next != IPPROTO_TCP) {
					ip6_rthdr_t *route;
					ip6_hbh_t *hop;
					ip6_frag_t *frag;

					/*
					 * Make sure we have enough room for the
					 * next header and length.
					 */
					if (off + 2 > len)
						goto skip;

					switch (next) {
					case IPPROTO_HOPOPTS:
					case IPPROTO_DSTOPTS:
						hop = (ip6_hbh_t *)(mp->b_rptr +
						    off);
						next = hop->ip6h_nxt;
						off += 8 * (hop->ip6h_len + 1);
						if (off > len)
							goto skip;
						break;
					case IPPROTO_ROUTING:
						route =
						    (ip6_rthdr_t *)(mp->b_rptr +
							off);
						next = route->ip6r_nxt;
						off += 8 * (route->ip6r_len +
						    1);
						if (off > len)
							goto skip;
						break;
					case IPPROTO_FRAGMENT:
						frag =
						    (ip6_frag_t *)(mp->b_rptr +
							off);
						next = frag->ip6f_nxt;
						off += 8;
						if (off > len)
							goto skip;
						break;
					case IPPROTO_TCP:
						tcp = (tcpha_t *)(mp->b_rptr +
						    off);
						break;
					default:
						/*
						 * There's no chance of TCP
						 * showing up at this point, so
						 * we can just go ahead and stop
						 * considering this packet.
						 */
						goto skip;
					}
				}
			} else {
				tcp = (tcpha_t *)(mp->b_rptr + sizeof (ip6_t));
			}
			break;
		default:
			goto skip;
		}

		/*
		 * Calculate current length of TCP packet.
		 */
		msg_len = mp->b_cont == NULL ? MBLKL(mp) : msgsize(mp);
		tcp_len = msg_len - ((uintptr_t)tcp - (uintptr_t)mp->b_rptr);
		tcp_len -= TCP_HDR_LENGTH(tcp);

		if ((tcp->tha_offset_and_reserved & 0x01) != 0 ||
	    	    (flags & HCK_FULLCKSUM_OK) == 0 ||
		    (tcp->tha_flags & ~(TH_ACK | TH_PUSH)) != 0 ||
		    tcp->tha_urp != 0 ||
		    tcp_len == 0 || tcp_len > msg_len) {
			tcp_valid = B_FALSE;
			DTRACE_PROBE6(mac__lro__bad__tcp, mblk_t *,
			    mp, tcpha_t *, tcp, boolean_t, tcp_valid,
			    boolean_t, ip_valid, uint_t, flags & HCK_FULLCKSUM_OK,
			    size_t, tcp_len);
		}

		/* Check if options other than timestamp */
		if (TCP_HDR_LENGTH(tcp) == TCP_MIN_HEADER_LENGTH +
		    TCPOPT_REAL_TS_LEN) {
			uint32_t *tsp = (uint32_t *)((uintptr_t)tcp +
			    TCP_MIN_HEADER_LENGTH);
			if (*tsp == TCPOPT_NOP_NOP_TSTAMP) {
				ts_valid = B_TRUE;
				tsval = ntohl(*(tsp + 1));
				tsecr = ntohl(*(tsp + 2));
				DTRACE_PROBE3(mac__lro__ts__match, mblk_t *,
				    mp, boolean_t, tcp_valid,
				    boolean_t, ip_valid);
			} else {
				tcp_valid = B_FALSE;
				DTRACE_PROBE3(mac__lro__ts__mismatch, mblk_t *,
				    mp, boolean_t, tcp_valid,
				    boolean_t, ip_valid);
			}
		} else if (TCP_HDR_LENGTH(tcp) != TCP_MIN_HEADER_LENGTH) {
			DTRACE_PROBE3(mac__lro__hdr__mismatch, mblk_t *,
			    mp, boolean_t, tcp_valid,
			    boolean_t, ip_valid);
			tcp_valid = B_FALSE;
		}

		/*
		 * At this point, we have a TCP segment which may or may not be
		 * valid. We can't just skip this and instead have to see if
		 * there's already a record of its flow. There are a few
		 * different cases to consider from here:
		 *
		 * 1) There is no record of the flow. In this case, if its
		 * valid, we'll open a new flow record, otherwise, we'll just
		 * append it as is ('skip' label).
		 *
		 * 2) There is a record for this flow and this packet is invalid
		 * for LRO or there is a mismatch in the sequence numbers or
		 * timestamp data. In that case we need to commit the current flow's
		 * packets, append the committed data to the output chain, and
		 * then append this as is.
		 *
		 * 3) There is a record for this flow and this packet would push
		 * us beyond 64k of payload data. We will commit the current
		 * flow's data and then start a new flow.
		 *
		 * 4) There is a record for this flow and this packet fits in
		 * the current bounds, so append it to the current state.
		 */
		for (i = 0; i < lrocnt; i++) {
			uint32_t seq;
			mac_lro_state_t *l = &lrop[i];
			if (!l->mls_valid) {
				DTRACE_PROBE2(mac__lro__match__invalid,
				    mblk_t *, mp, mac_lro_state_t *, l);
				continue;
			}

			if (l->mls_lport != tcp->tha_lport ||
			    l->mls_fport != tcp->tha_fport ||
			    v4 != l->mls_v4) {
				DTRACE_PROBE2(mac__lro__match__port,
				    mblk_t *, mp, mac_lro_state_t *, l);
				continue;
			}
			if (v4 && (ip4->ipha_src != V4_PART_OF_V6(l->mls_source) ||
			    ip4->ipha_dst != V4_PART_OF_V6(l->mls_dest))) {
				DTRACE_PROBE2(mac__lro__match__addr__v4,
				    mblk_t *, mp, mac_lro_state_t *, l);
				continue;
			}
			if (!v4 && (!IN6_ARE_ADDR_EQUAL(&ip6->ip6_src, &l->mls_source) ||
			    !IN6_ARE_ADDR_EQUAL(&ip6->ip6_dst, &l->mls_dest))) {
				DTRACE_PROBE2(mac__lro__match__addr__v6,
				    mblk_t *, mp, mac_lro_state_t *, l);
				continue;
			}

			if (!tcp_valid || !ip_valid) {
				DTRACE_PROBE2(mac__lro__match__addr__valid,
				    mblk_t *, mp, mac_lro_state_t *, l);
				mac_lro_commit(l, &head, &tail);
				goto skip;
			}

			seq = ntohl(tcp->tha_seq);

			if (tcp_len > IP_MAXPACKET - l->mls_len ||
			    seq != l->mls_exp_seq ||
			    ts_valid != l->mls_ts_valid ||
			    (ts_valid && l->mls_tsval > tsval) ||
			    ip_ecn != l->mls_ipecn) {
				DTRACE_PROBE6(mac__lro__force__commit,
				    mblk_t *, mp, mac_lro_state_t *, l,
				    size_t, tcp_len, uint32_t, seq, uint32_t,
				    tsval, uint16_t, ip_ecn);
				/*
				 * In some cases it could make sense to try and
				 * use this new packet to start a new sequence,
				 * but for the time being, we'll just append
				 * this directly.
				 */
				mac_lro_commit(l, &head, &tail);
				goto skip;
			}

			DTRACE_PROBE3(mac__lro__append, mac_lro_state_t *, l,
			    mblk_t *, mp, size_t, tcp_len);
			l->mls_ack = tcp->tha_ack;
			l->mls_window = tcp->tha_win;
			l->mls_len += tcp_len;
			l->mls_count++;
			l->mls_exp_seq += tcp_len;
			if (ts_valid) {
				l->mls_tsval = tsval;
				l->mls_tsecr = tsecr;
			}
			l->mls_tcp_flags |= tcp->tha_flags;

			/*
			 * XXX Consider something with a b_cont as not being fit
			 * for inclusion rather than this
			 */
			mp->b_rptr += (msg_len - tcp_len);
			if (MBLKL(mp) == 0) {
				mblk_t *tmp = mp;
				mp = tmp->b_cont;
				VERIFY3P(mp, !=, NULL);
				tmp->b_cont = NULL;
				mac_lro_append_bnext(tmp, &free_head,
				    &free_tail);
			}
			ASSERT3P(mp->b_next, ==, NULL);
			mac_lro_append_bcont(mp, &l->mls_head, &l->mls_tail);
			(*cntp)--;
			ASSERT3S(*cntp, >=, 1);
			/*
			 * sizep may be zero if we're not under bandwidth
			 * control
			 */
			if (*sizep != 0) {
				ASSERT3S(*sizep, >, msg_len - tcp_len);
				(*sizep) -= (msg_len - tcp_len);
			}
			break;
		}

		DTRACE_PROBE4(mac__lro__loop, uint_t, i, boolean_t,
		    tcp_valid, boolean_t, ip_valid, boolean_t, ts_valid);
		if (i < lrocnt) {
			/* Processed above */
			mp = next;
			continue;
		} else if (i == lrocnt && tcp_valid && ip_valid) {
			mac_lro_state_t *l;

			l = mac_lro_find_free_slot(lrop, lrocnt);
			if (l == NULL) {
				mac_lro_slot_misses++;
				goto skip;
			}

			l->mls_valid = B_TRUE;
			l->mls_head = l->mls_tail = mp;
			l->mls_tcp = tcp;
			l->mls_v4 = v4;

			if (v4) {
				V6_SET_ZERO(l->mls_source);
				V4_PART_OF_V6(l->mls_source) = ip4->ipha_src;
				V6_SET_ZERO(l->mls_dest);
				V4_PART_OF_V6(l->mls_dest) = ip4->ipha_dst;
			} else {
				l->mls_source = ip6->ip6_src;
				l->mls_dest = ip6->ip6_dst;
			}
			l->mls_lport = tcp->tha_lport;
			l->mls_fport = tcp->tha_fport;

			l->mls_len = msg_len;
			l->mls_count = 1;
			l->mls_exp_seq = ntohl(tcp->tha_seq) + tcp_len;
			l->mls_ack = tcp->tha_ack;
			l->mls_window = tcp->tha_win;
			l->mls_ipecn = ip_ecn;
			l->mls_tsval = tsval;
			l->mls_tsecr = tsecr;
			l->mls_ts_valid = ts_valid;
			l->mls_tcp_flags = tcp->tha_flags;

			DTRACE_PROBE2(mac__lro__create, mac_lro_state_t *,
			    l, mblk_t *, mp);

			mp = next;
			continue;
		}

skip:
		DTRACE_PROBE1(mac__lro__skip, mblk_t *, mp);
		mac_lro_append_bnext(mp, &head, &tail);
		mp = next;
	}

	for (i = 0; i < lrocnt; i++) {
		if (lrop[i].mls_valid) {
			mac_lro_commit(&lrop[i], &head, &tail);
		}
	}

#ifdef DEBUG
	mac_lro_verify_chain(&head, &tail);
#endif
	*mp_chain = head;
	*tailp = tail;
	freemsgchain(free_head);
}
