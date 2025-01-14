// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <gromox/endian.hpp>
#include <gromox/pcl.hpp>
#include <gromox/guid.hpp>
#include <gromox/rop_util.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#define TIME_FIXUP_CONSTANT_INT				11644473600LL

using namespace gromox;

uint16_t rop_util_get_replid(eid_t eid)
{
	/* replid is kept in host-endian, see rop_util_make_eid for detail */
	return eid & 0xFFFF;
}

/**
 * The reverse of rop_util_make_eid, see there for details.
 */
uint64_t rop_util_get_gc_value(eid_t eid)
{
	return rop_util_gc_to_value(rop_util_get_gc_array(eid));
}

/**
 * Extract the GC portion of a value produced by rop_util_make_eid.
 */
GLOBCNT rop_util_get_gc_array(eid_t eid)
{
	GLOBCNT gc;
#if !GX_BIG_ENDIAN
	memcpy(gc.ab, reinterpret_cast<uint8_t *>(&eid) + 2, 6);
#else
	memcpy(gc.ab, &eid, 6);
#endif
	return gc;
}

/**
 * @value:	One of the following:
 * 		* Gromox-level change number like 0x800000000005, 0x800000000006, ...,
 * 		* Gromox-level folder identifier like 0xd (inbox), ...,
 * 		* Gromox-level message identifier
 * @gc:		Low 48 bits of @value, encoded as a 48-bit big-endian integer.
 * 		(pursuant to the requirements of MS-OXCFXICS v24 §3.1.5.3
 * 		“increase with time, when compared byte to byte”, which means MSB)
 */
GLOBCNT rop_util_value_to_gc(uint64_t value)
{
	GLOBCNT gc;
	value = cpu_to_be64(value);
	memcpy(gc.ab, reinterpret_cast<uint8_t *>(&value) + 2, 6);
	return gc;
}

/**
 * @gc:		48-bit big-endian encoded integer
 * Decodes the integer and returns it.
 */
uint64_t rop_util_gc_to_value(GLOBCNT gc)
{
	uint64_t value;
	auto v = reinterpret_cast<uint8_t *>(&value);

#if !GX_BIG_ENDIAN
	v[0] = gc.ab[5];
	v[1] = gc.ab[4];
	v[2] = gc.ab[3];
	v[3] = gc.ab[2];
	v[4] = gc.ab[1];
	v[5] = gc.ab[0];
	v[6] = 0;
	v[7] = 0;
#else
	v[0] = 0;
	v[1] = 0;
	memcpy(v + 2, gc.ab, 6);
#endif
	return value;
}

/**
 * @replid:	replica id as per OXCFXICS
 * @gc:		Gromox-level folder/message id/CN encoded as 48-bit big-endian
 *
 * Produces an Exchange-level folder/message identifier (MS-OXCDATA v17
 * §2.2.1.2) — later visible in e.g. PR_RECORD_KEY (see also
 * mapi_types.hpp:FOLDER_ENTRYID), which contains the 48-bit big-endian gc
 * _before_ the 16-bit replid.
 *
 * MFCMAPI does not treat PR_RECORD_KEY's gc part in any way (leaves it as
 * bytes), and does the same to replid. It is not clear if replid should be
 * big-endian. However, given the replid is replaced by a replguid in
 * message_object.cpp:common_util_to_folder_entryid and zeroed, it probably
 * does not matter which way.
 *
 * The return value is mixed endianess and mildly useless when printed as a
 * number. Consumers such as message_object.cpp:common_util_to_folder_entryid
 * just deconstruct it again for PR_RECORD_KEY.
 */
eid_t rop_util_make_eid(uint16_t replid, GLOBCNT gc)
{
	eid_t eid;
	auto e = reinterpret_cast<uint8_t *>(&eid);
	
#if !GX_BIG_ENDIAN
	e[0] = 0;
	e[1] = 0;
	memcpy(e + 2, gc.ab, 6);
#else
	memcpy(&eid, gc.ab, 6);
	e[6] = 0;
	e[7] = 0;
#endif
	return (eid | replid);
}

eid_t rop_util_make_eid_ex(uint16_t replid, uint64_t value)
{
	return rop_util_make_eid(replid, rop_util_value_to_gc(value));
}

GUID rop_util_make_user_guid(int user_id)
{
	/*
	 * {XXXXXXXX-18a5-6f7b-bcdc-ea1ed03c5657} Database GUID (within
	 * Exchange entryids) for Gromox private stores.
	 */
	GUID guid;
	
	guid.time_low = user_id;
	guid.time_mid = 0x18a5;
	guid.time_hi_and_version = 0x6f7b;
	guid.clock_seq[0] = 0xbc;
	guid.clock_seq[1] = 0xdc;
	guid.node[0] = 0xea;
	guid.node[1] = 0x1e;
	guid.node[2] = 0xd0;
	guid.node[3] = 0x3c;
	guid.node[4] = 0x56;
	guid.node[5] = 0x57;
	return guid;
}

GUID rop_util_make_domain_guid(int domain_id)
{
	/*
	 * {XXXXXXXX-0afb-7df6-9192-49886aa738ce}: Database GUID
	 * for Gromox public stores.
	 */
	GUID guid;
	
	guid.time_low = domain_id;
	guid.time_mid = 0x0afb;
	guid.time_hi_and_version = 0x7df6;
	guid.clock_seq[0] = 0x91;
	guid.clock_seq[1] = 0x92;
	guid.node[0] = 0x49;
	guid.node[1] = 0x88;
	guid.node[2] = 0x6a;
	guid.node[3] = 0xa7;
	guid.node[4] = 0x38;
	guid.node[5] = 0xce;
	return guid;
}

int rop_util_get_user_id(GUID guid)
{
	if (guid.time_mid != 0x18a5 ||
		guid.time_hi_and_version != 0x6f7b ||
		guid.clock_seq[0] != 0xbc ||
		guid.clock_seq[1] != 0xdc ||
		guid.node[0] != 0xea ||
		guid.node[1] != 0x1e ||
		guid.node[2] != 0xd0 ||
		guid.node[3] != 0x3c ||
		guid.node[4] != 0x56 ||
		guid.node[5] != 0x57) {
		return -1;
	}
	return guid.time_low;
}

int rop_util_get_domain_id(GUID guid)
{
	if (guid.time_mid != 0x0afb ||
		guid.time_hi_and_version != 0x7df6 ||
		guid.clock_seq[0] != 0x91 ||
		guid.clock_seq[1] != 0x92 ||
		guid.node[0] != 0x49 ||
		guid.node[1] != 0x88 ||
		guid.node[2] != 0x6a ||
		guid.node[3] != 0xa7 ||
		guid.node[4] != 0x38 ||
		guid.node[5] != 0xce) {
		return -1;
	}
	return guid.time_low;
}

uint64_t rop_util_unix_to_nttime(time_t unix_time)
{
	uint64_t nt_time; 
	
	nt_time = unix_time;
	nt_time += TIME_FIXUP_CONSTANT_INT;
	nt_time *= 10000000;
	return nt_time;
}

time_t rop_util_nttime_to_unix(uint64_t nt_time)
{
	uint64_t unix_time;
	
	unix_time = nt_time;
	unix_time /= 10000000;
	unix_time -= TIME_FIXUP_CONSTANT_INT;
	return (time_t)unix_time;
}

uint64_t rop_util_current_nttime()
{
	struct timeval tvl;
	
	gettimeofday(&tvl, NULL);
	return rop_util_unix_to_nttime(tvl.tv_sec) + tvl.tv_usec*10;
}

GUID rop_util_binary_to_guid(const BINARY *pbin)
{
	GUID guid;
	guid.time_low = le32p_to_cpu(&pbin->pb[0]);
	guid.time_mid = le16p_to_cpu(&pbin->pb[4]);
	guid.time_hi_and_version = le16p_to_cpu(&pbin->pb[6]);
	memcpy(guid.clock_seq, pbin->pb + 8, 2);
	memcpy(guid.node, pbin->pb + 10, 6);
	return guid;
}

void rop_util_guid_to_binary(GUID guid, BINARY *pbin)
{
	cpu_to_le32p(&pbin->pb[pbin->cb], guid.time_low);
	pbin->cb += sizeof(uint32_t);
	cpu_to_le16p(&pbin->pb[pbin->cb], guid.time_mid);
	pbin->cb += sizeof(uint16_t);
	cpu_to_le16p(&pbin->pb[pbin->cb], guid.time_hi_and_version);
	pbin->cb += sizeof(uint16_t);
	memcpy(pbin->pb + pbin->cb, guid.clock_seq, 2);
	pbin->cb += 2;
	memcpy(pbin->pb + pbin->cb, guid.node, 6);
	pbin->cb += 6;
}

void rop_util_free_binary(BINARY *pbin)
{
	free(pbin->pb);
	free(pbin);
}

XID::XID(GUID g, uint64_t change_num) : guid(g), size(22)
{
	memcpy(local_id, rop_util_get_gc_array(change_num).ab, 6);
}
