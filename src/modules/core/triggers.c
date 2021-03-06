/*
 * triggers.c
 * Purpose: Linear condition triggers.
 *
 * Copyright (c) 2009 - 2012, TortoiseLabs LLC.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <pcap.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "stdinc.h"
#include "protocols.h"
#include "packet.h"
#include "patricia.h"
#include "ipstate.h"
#include "hook.h"
#include "action.h"

typedef struct _triggeraction {
	struct _triggeraction *next;
	action_t *act;
} triggeraction_t;

typedef struct _trigger {
	struct _trigger *next;

	triggertype_t type;

	unsigned int target_pps;
	unsigned int target_mbps;
	unsigned int target_flowcount;
	unsigned int below_mbps;
	unsigned int expiry;
	unsigned char protocol;
	unsigned char tcp_synonly;

	triggeraction_t *list;
} trigger_t;

static trigger_t *t_list[IPPROTO_MAX + 1];
static patricia_tree_t *banrecord_trie = NULL;
static int expiry;

static void
run_triggers(actiontype_t at, trigger_t *t, packet_info_t *packet, banrecord_t *rec)
{
	triggeraction_t *i;

	for (i = t->list; i != NULL; i = i->next)
		i->act->act(at, t->type, packet, rec, i->act->data);
}

static banrecord_t *
ban_find(uint32_t ip)
{
	prefix_t *pfx;
	patricia_node_t *node;
	struct in_addr sin;

	sin.s_addr = ip;
	pfx = New_Prefix(AF_INET, &sin, 32);

	node = patricia_search_exact(banrecord_trie, pfx);

	Deref_Prefix(pfx);

	return node != NULL ? node->data : NULL;
}

static void
expire_trigger(void *data)
{
	banrecord_t *rec = data;
	struct in_addr sin;
	prefix_t *pfx;
	patricia_node_t *node;
	trigger_t *t = rec->trigger;

	run_triggers(ACTION_UNBAN, rec->trigger, &rec->pkt, rec);

	switch (t->type) {
	case TRIGGER_SRC:
		sin.s_addr = rec->pkt.pkt_src.s_addr;
		break;
	case TRIGGER_DST:
	default:
		sin.s_addr = rec->pkt.pkt_dst.s_addr;
		break;
	};

	pfx = New_Prefix(AF_INET, &sin, 32);

	node = patricia_lookup(banrecord_trie, pfx);
	patricia_remove(banrecord_trie, node);

	Deref_Prefix(pfx);

	free(rec);
}

static banrecord_t *
trigger_ban(trigger_t *t, packet_info_t *packet, iprecord_t *irec)
{
	banrecord_t *rec;
	prefix_t *pfx;
	patricia_node_t *node;
	struct in_addr sin;

	switch (t->type) {
	case TRIGGER_SRC:
		sin.s_addr = packet->pkt_src.s_addr;
		break;
	case TRIGGER_DST:
	default:
		sin.s_addr = packet->pkt_dst.s_addr;
		break;
	};

	if (ban_find(sin.s_addr) != NULL)
		return NULL;

	rec = calloc(sizeof(banrecord_t), 1);

	rec->trigger = t;
	memcpy(&rec->irec, irec, sizeof(iprecord_t));
	memcpy(&rec->pkt, packet, sizeof(packet_info_t));
	rec->added = mowgli_eventloop_get_time(eventloop);
	rec->expiry_ts = rec->added + (t->expiry ? t->expiry : expiry);

	pfx = New_Prefix(AF_INET, &sin, 32);

	node = patricia_lookup(banrecord_trie, pfx);
	node->data = rec;

	Deref_Prefix(pfx);

	run_triggers(ACTION_BAN, t, packet, rec);

	rec->timer = mowgli_timer_add_once(eventloop, "expire_trigger", expire_trigger, rec, (t->expiry ? t->expiry : expiry));

	return rec;
}

static void
check_trigger(packet_info_t *packet, iprecord_t *rec)
{
	trigger_t *i;
	flowdata_t *flow;

	flow = ipstate_lookup_flowdata(rec, packet->ip_type);
	if (flow == NULL)
		return;

	for (i = t_list[packet->ip_type]; i != NULL; i = i->next)
	{
		int pps, mbps;
		int do_trigger = 0;

		DPRINTF("check trigger packet %p record %p protocol %d pktproto %d\n", packet, rec, i->protocol, packet->ip_type);

		mbps = (int) floor((flow->flow / 1000000.));
		pps = flow->pps;

		DPRINTF("... pps %d mbps %d target_pps %d target_mbps %d\n", pps, mbps, i->target_pps, i->target_mbps);

		if (i->target_pps && (pps > i->target_pps))
			do_trigger = 1;

		if (i->target_mbps && (mbps > i->target_mbps))
			do_trigger = 1;

		if (i->target_mbps && (mbps < i->target_mbps))
			do_trigger = 0;

		if (i->below_mbps && (mbps > i->below_mbps))
			do_trigger = 0;

		if (i->tcp_synonly && packet->tcp_flags != TCP_SYN)
			do_trigger = 0;

		if (i->target_flowcount && flow->count < i->target_flowcount)
			do_trigger = 0;

		DPRINTF("trigger %p conditions %s for flow %p\n", i, do_trigger == 1 ? "met" : "not met", rec);

		if (do_trigger)
			HOOK_CALL(HOOK_CHECK_EXEMPT, packet, rec, &do_trigger);

		DPRINTF("HOOK_CHECK_EXEMPT result %d\n", do_trigger);

		if (do_trigger)
			trigger_ban(i, packet, rec);
	}
}

static void
parse_actions(trigger_t *t, mowgli_config_file_entry_t *entry)
{
	mowgli_config_file_entry_t *ce;
	triggeraction_t *ta;

	MOWGLI_ITER_FOREACH(ce, entry)
	{
		action_t *act;

		act = action_find(ce->varname);
		if (act == NULL)
			continue;

		ta = calloc(sizeof(triggeraction_t), 1);
		ta->act = act;
		ta->next = t->list;
		t->list = ta;
	}
}

static void
parse_trigger(mowgli_config_file_entry_t *entry)
{
	trigger_t *t;
	mowgli_config_file_entry_t *ce;

	t = calloc(sizeof(trigger_t), 1);

	MOWGLI_ITER_FOREACH(ce, entry)
	{
		if (!strcasecmp(ce->varname, "protocol"))
		{
			if (!strcasecmp(ce->vardata, "tcp"))
				t->protocol = 6;
			else if (!strcasecmp(ce->vardata, "tcp-syn"))
			{
				t->protocol = 6;
				t->tcp_synonly = 1;
			}
			else if (!strcasecmp(ce->vardata, "udp"))
				t->protocol = 17;
			else if (!strcasecmp(ce->vardata, "icmp"))
				t->protocol = 1;
		}
		else if (!strcasecmp(ce->varname, "target_mbps"))
			t->target_mbps = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "flowcount"))
			t->target_flowcount = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "below_mbps"))
			t->below_mbps = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "target_pps"))
			t->target_pps = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "expiry"))
			t->expiry = atoi(ce->vardata);
		else if (!strcasecmp(ce->varname, "actions"))
			parse_actions(t, ce->entries);
		else if (!strcasecmp(ce->varname, "action_direction"))
		{
			if (!strcasecmp(ce->vardata, "source"))
				t->type = TRIGGER_SRC;
		}
	}

	DPRINTF("t->protocol %d\n", t->protocol);

	t->next = t_list[t->protocol];
	t_list[t->protocol] = t;
}

void
module_cons(mowgli_eventloop_t *eventloop, mowgli_config_file_entry_t *entry)
{
	mowgli_config_file_entry_t *ce;

	memset(t_list, 0, sizeof(t_list));

	MOWGLI_ITER_FOREACH(ce, entry)
	{
		if (!strcasecmp(ce->varname, "trigger"))
			parse_trigger(ce->entries);
		else if (!strcasecmp(ce->varname, "expiry"))
			expiry = atoi(ce->vardata);
	}

	banrecord_trie = New_Patricia(32);

	HOOK_REGISTER(HOOK_CHECK_TRIGGER, check_trigger);
}
