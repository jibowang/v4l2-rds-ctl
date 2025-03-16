/*
 * Copyright 2012 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 * Author: Konke Radlow <koradlow@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335  USA
 */

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <config.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#include "../include/libv4l2rds.h"

/* struct to encapsulate the private state information of the decoding process */
/* the fields (except for handle) are for internal use only - new information
 * is decoded and stored in them until it can be verified and copied to the
 * public part of the  rds structure (handle) */
/* for meaning of abbreviations check the library header libv4l2rds.h */
// 结构体用于封装解码过程的私有状态信息
// 其中的字段（handle 除外）仅供内部使用
// 新解码的信息会存储在这些字段中，直到它们被验证后再复制到 rds 结构的公共部分（handle）
struct rds_private_state {
	/* v4l2_rds has to be in first position, to allow typecasting between
	 * v4l2_rds and rds_private_state pointers */
	struct v4l2_rds handle;

	/* current state of rds group decoding */
	uint8_t decode_state;

	/* temporal storage locations for rds fields */
	uint16_t new_pi;
	uint8_t new_ps[8];
	uint8_t new_ps_valid[8];
	uint8_t new_pty;
	uint8_t new_ptyn[2][4];
	bool new_ptyn_valid[2];
	uint8_t new_rt[64];
	uint8_t next_rt_segment;
	uint8_t new_di;
	uint8_t next_di_segment;
	uint8_t new_ecc;
	uint8_t new_lc;
	/* RDS date / time representation */
	uint32_t new_mjd;	/* modified Julian Day code */
	uint8_t utc_hour;
	uint8_t utc_minute;
	uint8_t utc_offset;

	struct v4l2_rds_group rds_group;
	struct v4l2_rds_data rds_data_raw[4];
};

/* states of the RDS block into group decoding state machine */
enum rds_state {
	RDS_EMPTY,
	RDS_A_RECEIVED,
	RDS_B_RECEIVED,
	RDS_C_RECEIVED,
};

static inline uint8_t set_bit(uint8_t input, uint8_t bitmask, bool bitvalue)
{
	return bitvalue ? input | bitmask : input & ~bitmask;
}

/* rds_decode_a-d(..): group of functions to decode different RDS blocks
 * into the RDS group that's currently being received
 *
 * block A of RDS group always contains PI code of program */
// block A: PI
static uint32_t rds_decode_a(struct rds_private_state *priv_state, struct v4l2_rds_data *rds_data)
{
	struct v4l2_rds *handle = &priv_state->handle;
	uint32_t updated_fields = 0;
	uint16_t pi = (rds_data->msb << 8) | rds_data->lsb;

	/* data in RDS group is uninterpreted */
	priv_state->rds_group.pi = pi;

	/* compare PI values to detect PI update (Channel Switch)
	 * --> new PI is only accepted, if the same PI is received
	 * at least 2 times in a row */
	// 仅当收到相同的 PI 至少连续2次时 才接受新的 PI
	if (pi != handle->pi && pi == priv_state->new_pi) {
		handle->pi = pi;
		handle->valid_fields |= V4L2_RDS_PI;  // 标记 PI 字段为有效
		updated_fields |= V4L2_RDS_PI;        // 标记 PI 字段已更新
	} else if (pi != handle->pi && pi != priv_state->new_pi) {
		priv_state->new_pi = pi;
	}

	return updated_fields;
}

/* block B of RDS group always contains Group Type Code, Group Type information
 * Traffic Program Code and Program Type Code as well as 5 bits of Group Type
 * depending information */
// block B：组类型代码、组类型信息、TP、PTY
static uint32_t rds_decode_b(struct rds_private_state *priv_state, struct v4l2_rds_data *rds_data)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	bool traffic_prog;
	uint8_t pty;
	uint32_t updated_fields = 0;

	/* bits 12-15 (4-7 of msb) contain the Group Type Code */
	grp->group_id = rds_data->msb >> 4 ;

	/* bit 11 (3 of msb) defines Group Type info: 0 = A, 1 = B */
	grp->group_version = (rds_data->msb & 0x08) ? 'B' : 'A';

	/* bit 10 (2 of msb) defines Traffic program Code */
	// 更新 TP
	traffic_prog = (bool)rds_data->msb & 0x04;
	if (handle->tp != traffic_prog) {
		handle->tp = traffic_prog;
		updated_fields |= V4L2_RDS_TP;
	}
	handle->valid_fields |= V4L2_RDS_TP;

	/* bits 0-4 contains Group Type depending information */
	grp->data_b_lsb = rds_data->lsb & 0x1f;

	/* bits 5-9 contain the PTY code */
	pty = (rds_data->msb << 3) | (rds_data->lsb >> 5);
	pty &= 0x1f; /* mask out 3 irrelevant bits */

	/* only accept new PTY if same PTY is received twice in a row
	 * and filter out cases where the PTY is already known */
	if (handle->pty == pty) {
		priv_state->new_pty = pty;
		return updated_fields;
	}
	// 仅当连续两次接收到相同的 PTY 时才接受新的 PTY
	if (priv_state->new_pty == pty) {
		handle->pty = priv_state->new_pty;
		updated_fields |= V4L2_RDS_PTY;
		handle->valid_fields |= V4L2_RDS_PTY;
	} else {
		priv_state->new_pty = pty;
	}

	return updated_fields;
}

/* block C of RDS group contains either data or the PI code, depending
 * on the Group Type - store the raw data for later decoding */
static void rds_decode_c(struct rds_private_state *priv_state, struct v4l2_rds_data *rds_data)
{
	struct v4l2_rds_group *grp = &priv_state->rds_group;

	grp->data_c_msb = rds_data->msb;
	grp->data_c_lsb = rds_data->lsb;
	/* we could decode the PI code here, because we already know if the
	 * group is of type A or B, but it doesn't give any advantage because
	 * we only get here after the PI code has been decoded in the first
	 * state of the state machine */
}

/* block D of RDS group contains data - store the raw data for later decoding */
static void rds_decode_d(struct rds_private_state *priv_state, struct v4l2_rds_data *rds_data)
{
	struct v4l2_rds_group *grp = &priv_state->rds_group;

	grp->data_d_msb = rds_data->msb;
	grp->data_d_lsb = rds_data->lsb;
}

static bool rds_add_oda(struct rds_private_state *priv_state, struct v4l2_rds_oda oda)
{
	struct v4l2_rds *handle = &priv_state->handle;

	/* check if there was already an ODA announced for this group type */
	for (int i = 0; i < handle->rds_oda.size; i++) {
		if (handle->rds_oda.oda[i].group_id == oda.group_id)
			/* update the AID for this ODA */
			handle->rds_oda.oda[i].aid = oda.aid;
			return false;
	}
	/* add the new ODA */
	if (handle->rds_oda.size >= MAX_ODA_CNT)
		return false;
	handle->rds_oda.oda[handle->rds_oda.size++] = oda;
	return true;
}

/* add a new AF to the list, if it doesn't exist yet */
static bool rds_add_af_to_list(struct v4l2_rds_af_set *af_set, uint8_t af, bool is_vhf)
{
	uint32_t freq = 0;

	/* AF0 -> "Not to be used" */
	if (af == 0)
		return false;

	/* calculate the AF values in HZ */
	if (is_vhf)
		freq = 87500000 + af * 100000;
	else if (freq <= 15)
		freq = 152000 + af * 9000;
	else
		freq = 531000 + af * 9000;

	/* prevent buffer overflows */
	if (af_set->size >= MAX_AF_CNT || af_set->size >= af_set->announced_af)
		return false;
	/* check if AF already exists */
	for (int i = 0; i < af_set->size; i++) {
		if (af_set->af[i] == freq)
			return false;
	}
	/* it's a new AF, add it to the list */
	af_set->af[(af_set->size)++] = freq;
	return true;
}

/* extracts the AF information from Block 3 of type 0A groups, and tries
 * to add them to the AF list with a helper function */
static bool rds_add_af(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;

	/* AFs are submitted in Block 3 of type 0A groups */
	uint8_t c_msb = priv_state->rds_group.data_c_msb;
	uint8_t c_lsb = priv_state->rds_group.data_c_lsb;
	bool updated_af = false;
	struct v4l2_rds_af_set *af_set = &handle->rds_af;

	/* the 4 8-bit values in the block's data fields (c_msb/c_lsb,
	 * d_msb/d_lsb) represent either a carrier frequency (1..204)
	 * or a special meaning (205..255).
	 * Translation tables can be found in IEC 62106 section 6.2.1.6 */

	/* 250: LF / MF frequency follows */
	if (c_msb == 250) {
		if (rds_add_af_to_list(af_set, c_lsb, false))
			updated_af = true;
		c_lsb = 0; /* invalidate */
	}
	/* 224..249: announcement of AF count (224=0, 249=25)*/
	if (c_msb >= 224 && c_msb <= 249)
		af_set->announced_af = c_msb - 224;
	/* check if the data represents an AF (for 1 =< val <= 204 the
	 * value represents an AF) */
	if (c_msb < 205)
		if (rds_add_af_to_list(af_set, c_msb, true))
			updated_af = true;
	if (c_lsb < 205)
		if (rds_add_af_to_list(af_set, c_lsb, true))
			updated_af = true;
	/* did we receive all announced AFs? */
	if (af_set->size >= af_set->announced_af && af_set->announced_af != 0)
		handle->valid_fields |= V4L2_RDS_AF;
	return updated_af;
}

/* adds one char of the ps name to temporal storage, the value is validated
 * if it is received twice in a row
 * @pos:	position of the char within the PS name (0..7)
 * @ps_char:	the new character to be added
 * @return:	true, if all 8 temporal ps chars have been validated */
static bool rds_add_ps(struct rds_private_state *priv_state, uint8_t pos, uint8_t ps_char)
{
	if (ps_char == priv_state->new_ps[pos]) {
		priv_state->new_ps_valid[pos] = 1;
	} else {
		priv_state->new_ps[pos] = ps_char;
		memset(priv_state->new_ps_valid, 0, 8);
	}

	/* check if all ps positions have been validated */
	for (int i = 0; i < 8; i++)
		if (priv_state->new_ps_valid[i] != 1)
			return false;
	return true;
}

/* group of functions to decode successfully received RDS groups into
 * easily accessible data fields
 *
 * group 0: basic tuning and switching */
static uint32_t rds_decode_group0(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	bool new_ps = false;
	bool tmp;
	uint32_t updated_fields = 0;

	/* bit 4 of block B contains the TA flag */
	tmp = grp->data_b_lsb & 0x10;
	if (handle->ta != tmp) {
		handle->ta = tmp;
		updated_fields |= V4L2_RDS_TA;
	}
	handle->valid_fields |= V4L2_RDS_TA;

	/* bit 3 of block B contains the Music/Speech flag */
	tmp = grp->data_b_lsb & 0x08;
	if (handle->ms != tmp) {
		handle->ms = tmp;
		updated_fields |= V4L2_RDS_MS;
	}
	handle->valid_fields |= V4L2_RDS_MS;

	/* bit 0-1 of block b contain program service name and decoder
	 * control segment address */
	uint8_t segment = grp->data_b_lsb & 0x03;

	/* put the received station-name characters into the correct position
	 * of the station name, and check if the new PS is validated */
	rds_add_ps(priv_state, segment * 2, grp->data_d_msb);
	new_ps = rds_add_ps(priv_state, segment * 2 + 1, grp->data_d_lsb);
	if (new_ps) {
		/* check if new PS is the same as the old one */
		if (memcmp(priv_state->new_ps, handle->ps, 8) != 0) {
			memcpy(handle->ps, priv_state->new_ps, 8);
			updated_fields |= V4L2_RDS_PS;
		}
		handle->valid_fields |= V4L2_RDS_PS;
	}

	/* bit 2 of block B contains 1 bit of the Decoder Control Information (DI)
	 * the segment number defines the bit position
	 * New bits are only accepted the segments arrive in the correct order */
	bool bit2 = grp->data_b_lsb & 0x04;
	if (segment == 0 || segment == priv_state->next_di_segment) {
		switch (segment) {
		case 0:
			priv_state->new_di = set_bit(priv_state->new_di,
				V4L2_RDS_FLAG_STEREO, bit2);
			priv_state->next_di_segment = 1;
			break;
		case 1:
			priv_state->new_di = set_bit(priv_state->new_di,
				V4L2_RDS_FLAG_ARTIFICIAL_HEAD, bit2);
			priv_state->next_di_segment = 2;
			break;
		case 2:
			priv_state->new_di = set_bit(priv_state->new_di,
				V4L2_RDS_FLAG_COMPRESSED, bit2);
			priv_state->next_di_segment = 3;
			break;
		case 3:
			priv_state->new_di = set_bit(priv_state->new_di,
				V4L2_RDS_FLAG_STATIC_PTY, bit2);
			/* check if the value of DI has changed, and store
			 * and signal DI update in case */
			if (handle->di != priv_state->new_di) {
				handle->di = priv_state->new_di;
				updated_fields |= V4L2_RDS_DI;
			}
			priv_state->next_di_segment = 0;
			handle->valid_fields |= V4L2_RDS_DI;
			break;
		}
	} else {
		/* wrong order of DI segments -> restart */
		priv_state->next_di_segment = 0;
		priv_state->new_di = 0;
	}

	/* version A groups contain AFs in block C */
	if (grp->group_version == 'A')
		if (rds_add_af(priv_state))
			updated_fields |= V4L2_RDS_AF;

	return updated_fields;
}

/* group 1: slow labeling codes & program item number */
static uint32_t rds_decode_group1(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	uint32_t updated_fields = 0;
	uint8_t variant_code = 0;

	/* version A groups contain slow labeling codes,
	 * version B groups only contain program item number which is a
	 * very uncommonly used feature */
	if (grp->group_version != 'A')
		return 0;

	/* bit 14-12 of block c contain the variant code */
	variant_code = (grp->data_c_msb >> 4) & 0x07;
	if (variant_code == 0) {
		/* var 0 -> ECC, only accept if same lc is
		 * received twice */
		if (grp->data_c_lsb == priv_state->new_ecc) {
			handle->valid_fields |= V4L2_RDS_ECC;
			if (handle->ecc != grp->data_c_lsb)
				updated_fields |= V4L2_RDS_ECC;
			handle->ecc = grp->data_c_lsb;
		} else {
			priv_state->new_ecc = grp->data_c_lsb;
		}
	} else if (variant_code == 0x03) {
		/* var 0x03 -> Language Code, only accept if same lc is
		 * received twice */
		if (grp->data_c_lsb == priv_state->new_lc) {
			handle->valid_fields |= V4L2_RDS_LC;
			updated_fields |= V4L2_RDS_LC;
			handle->lc = grp->data_c_lsb;
		} else {
			priv_state->new_lc = grp->data_c_lsb;
		}
	}
	return updated_fields;
}

/* group 2: radio text */
// RT
static uint32_t rds_decode_group2(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	uint32_t updated_fields = 0;

	/* bit 0-3 of block B contain the segment code */
	uint8_t segment = grp->data_b_lsb & 0x0f;  // 保留 grp->data_b_lsb 的低 4 位
	/* bit 4 of block b contains the A/B text flag (new radio text
	 * will be transmitted) */
	bool rt_ab_flag_n = grp->data_b_lsb & 0x10;  // 检查 A/B 标志位，用于判断是否是新的广播文本

	/* new Radio Text will be transmitted */
	if (rt_ab_flag_n != handle->rt_ab_flag) {
		handle->rt_ab_flag = rt_ab_flag_n;
		memset(handle->rt, 0, 64);   // 初始化64字节数据
		handle->valid_fields &= ~V4L2_RDS_RT; // 标记文本无效
		updated_fields |= V4L2_RDS_RT;  // 标记更新
		priv_state->next_rt_segment = 0;   // 重置段计数器
	}

	/* further decoding of data depends on type of message (A or B)
	 * Type A allows RTs with a max length of 64 chars
	 * Type B allows RTs with a max length of 32 chars */
	// 数据的进一步解码取决于消息类型（A 或 B）
	// A 类允许 RT 的最大长度为 64 个字符
	// B 型允许 RT 最大长度为 32 个字符
	if (grp->group_version == 'A') {
		// 用于确保 RDS 文本片段按正确的顺序处理
		// 当收到第一个片段（segment = 0）时，这是新文本消息的开始
		// segment == priv_state->next_rt_segment, 保接收到的片段正是我们期待的下一个片段
		if (segment == 0 || segment == priv_state->next_rt_segment) {  
			priv_state->new_rt[segment * 4] = grp->data_c_msb;
			priv_state->new_rt[segment * 4 + 1] = grp->data_c_lsb;
			priv_state->new_rt[segment * 4 + 2] = grp->data_d_msb;
			priv_state->new_rt[segment * 4 + 3] = grp->data_d_lsb;
			priv_state->next_rt_segment = segment + 1;
			if (segment == 0x0f) {  // 到达了最后一个文本片段 15, 16个片段*4 = 64 字节
				handle->rt_length = 64;
				handle->valid_fields |= V4L2_RDS_RT;
				if (memcmp(handle->rt, priv_state->new_rt, 64)) {  // 当前文本和新文本不相同
					memcpy(handle->rt, priv_state->new_rt, 64);  // 复制新文本
					updated_fields |= V4L2_RDS_RT;
				}
				priv_state->next_rt_segment = 0;  // 重置段计数器
			}
		}
	} else {
		if (segment == 0 || segment == priv_state->next_rt_segment) {
			priv_state->new_rt[segment * 2] = grp->data_d_msb;
			priv_state->new_rt[segment * 2 + 1] = grp->data_d_lsb;
			/* PI code in block C will be ignored */
			priv_state->next_rt_segment = segment + 1;
			if (segment == 0x0f) {
				handle->rt_length = 32;
				handle->valid_fields |= V4L2_RDS_RT;
				updated_fields |= V4L2_RDS_RT;
				if (memcmp(handle->rt, priv_state->new_rt, 32)) {
					memcpy(handle->rt, priv_state->new_rt, 32);
					updated_fields |= V4L2_RDS_RT;
				}
				priv_state->next_rt_segment = 0;
			}
		}
	}

	/* determine if complete rt was received
	 * a carriage return (0x0d) can end a message early */
	// 判断是否收到完整的rt
	// 回车符（0x0d）可以提前结束消息
	for (int i = 0; i < 64; i++) {
		if (priv_state->new_rt[i] == 0x0d) {   // 0x0d 是回车符 CR
			/* replace CR with terminating character */
			priv_state->new_rt[i] = '\0';     // 将回车符替换为字符串结束符
			handle->rt_length = i;
			handle->valid_fields |= V4L2_RDS_RT;
			// 更新文本并标记变化
			if (memcmp(handle->rt, priv_state->new_rt, handle->rt_length)) {
					memcpy(handle->rt, priv_state->new_rt,
						handle->rt_length);
					updated_fields |= V4L2_RDS_RT;
				}
			priv_state->next_rt_segment = 0;
		}
	}
	return updated_fields;
}

/* group 3: Open Data Announcements */
static uint32_t rds_decode_group3(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	struct v4l2_rds_oda new_oda;
	uint32_t updated_fields = 0;

	if (grp->group_version != 'A')
		return 0;

	/* 0th bit of block b contains Group Type Info version of announced ODA
	 * Group Type info: 0 = A, 1 = B */
	new_oda.group_version = (grp->data_b_lsb & 0x01) ? 'B' : 'A';
	/* 1st to 4th bit contain Group ID of announced ODA */
	new_oda.group_id = (grp->data_b_lsb & 0x1e) >> 1;
	/* block D contains the 16bit Application Identification Code */
	new_oda.aid = (grp->data_d_msb << 8) | grp->data_d_lsb;

	/* try to add the new ODA to the set of defined ODAs */
	if (rds_add_oda(priv_state, new_oda)) {
		handle->decode_information |= V4L2_RDS_ODA;
		updated_fields |= V4L2_RDS_ODA;
	}
	return updated_fields;
}

/* decodes the RDS date/time representation into a standard c representation
 * that can be used with c-library functions */
static time_t rds_decode_mjd(const struct rds_private_state *priv_state)
{
	struct tm new_time;
	int y, m, d, k = 0;
	/* offset is given in multiples of half hrs */
	uint32_t offset = priv_state->utc_offset & 0x1f;
	uint32_t local_mjd = priv_state->new_mjd;
	uint8_t local_hour = priv_state->utc_hour;
	uint8_t local_minute = priv_state->utc_minute;

	/* add / subtract the local offset to get the local time.
	 * The offset is expressed in multiples of half hours */
	if (priv_state->utc_offset & 0x20) { /* bit 5 indicates -/+ */
		local_hour -= (offset * 2);
		local_minute -= (offset % 2) * 30;
	} else {
		local_hour += (offset * 2);
		local_minute += (offset % 2) * 30;
	}

	/* the formulas for the conversion are taken from Annex G of the
	 * IEC 62106 RDS standard */
	y = (int)((local_mjd - 15078.2) / 365.25);
	m = (int)((local_mjd - 14956.1 - (int)(y * 365.25)) / 30.6001);
	d = (int)(local_mjd - 14956 - (int)(y * 365.25) - (int)(m * 30.6001));
	if (m == 14 || m == 15)
		k = 1;
	y = y + k;
	m = m - 1 - k*12;

	/* put the values into a tm struct for conversion into time_t value */
	new_time.tm_sec = 0;
	new_time.tm_min = local_minute;
	new_time.tm_hour = local_hour;
	new_time.tm_mday = d;
	new_time.tm_mon = m;
	new_time.tm_year = y;
	/* offset (submitted by RDS) that was used to compute the local time,
	 * expressed in multiples of half hours, bit 5 indicates -/+ */
	if (priv_state->utc_offset & 0x20)
		new_time.tm_gmtoff = -2 * offset * 3600;
	else
		new_time.tm_gmtoff = 2 * offset * 3600;

	/* convert tm struct to time_t value and return it */
	return mktime(&new_time);
}

/* group 4: Date and Time */
static uint32_t rds_decode_group4(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	uint32_t mjd;
	uint32_t updated_fields = 0;

	if (grp->group_version != 'A')
		return 0;

	/* bits 0-1 of block b lsb contain bits 15 and 16 of Julian day code
	 * bits 0-7 of block c msb contain bits 7 to 14 of Julian day code
	 * bits 1-7 of block c lsb contain bits 0 to 6 of Julian day code */
	mjd = ((grp->data_b_lsb & 0x03) << 15) |
		(grp->data_c_msb << 7) | (grp->data_c_lsb >> 1);
	/* the same mjd has to be received twice in order to accept the data */
	if (priv_state->new_mjd != mjd) {
		priv_state->new_mjd = mjd;
		return 0;
	}
	/* same mjd received at least twice --> decode time & date */

	/* bit 0 of block c lsb contains bit 4 of utc_hour
	 * bits 4-7 of block d contains bits 0 to 3 of utc_hour */
	priv_state->utc_hour = ((grp->data_c_lsb & 0x01) << 4) |
		(grp->data_d_msb >> 4);

	/* bits 0-3 of block d msb contain bits 2 to 5 of utc_minute
	 * bits 6-7 of block d lsb contain bits 0 and 1 utc_minute */
	priv_state->utc_minute = ((grp->data_d_msb & 0x0f) << 2) |
		(grp->data_d_lsb >> 6);

	/* bits 0-5 of block d lsb contain bits 0 to 5 of local time offset */
	priv_state->utc_offset = grp->data_d_lsb & 0x3f;

	/* decode RDS time representation into commonly used c representation */
	handle->time = rds_decode_mjd(priv_state);
	updated_fields |= V4L2_RDS_TIME;
	handle->valid_fields |= V4L2_RDS_TIME;
	printf("\nLIB: time_t: %ld", handle->time);
	return updated_fields;
}

/* group 10: Program Type Name */
static uint32_t rds_decode_group10(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	struct v4l2_rds_group *grp = &priv_state->rds_group;
	uint32_t updated_fields = 0;
	uint8_t ptyn_tmp[4];

	/* bit 0 of block B contain the segment code */
	uint8_t segment_code = grp->data_b_lsb & 0x01;
	/* bit 4 of block b contains the A/B text flag (new ptyn
	 * will be transmitted) */
	bool ptyn_ab_flag_n = grp->data_b_lsb & 0x10;

	if (grp->group_version != 'A')
		return 0;

	/* new Program Type Text will be transmitted */
	if (ptyn_ab_flag_n != handle->ptyn_ab_flag) {
		handle->ptyn_ab_flag = ptyn_ab_flag_n;
		memset(handle->ptyn, 0, 8 * sizeof(char));
		memset(priv_state->new_ptyn, 0, 8 * sizeof(char));
		memset(priv_state->new_ptyn_valid, 0, 2 * sizeof(bool));
		handle->valid_fields &= ~V4L2_RDS_PTYN;
		updated_fields |= V4L2_RDS_PTYN;
	}
	/* copy chars to designated position within temp text field */
	ptyn_tmp[0] = grp->data_c_msb;
	ptyn_tmp[1] = grp->data_c_lsb;
	ptyn_tmp[2] = grp->data_d_msb;
	ptyn_tmp[3] = grp->data_d_lsb;

	/* only validate ptyn segment if the same data is received twice */
	if (memcmp(ptyn_tmp, priv_state->new_ptyn[segment_code], 4) == 0) {
		priv_state->new_ptyn_valid[segment_code] = true;
	} else {
		for (int i = 0; i < 4; i++)
			priv_state->new_ptyn[segment_code][i] = ptyn_tmp[i];
		priv_state->new_ptyn_valid[segment_code] = false;
	}

	/* if both ptyn segments have been validated, accept the new ptyn */
	if (priv_state->new_ptyn_valid[0] && priv_state->new_ptyn_valid[1]) {
		for (int i = 0; i < 4; i++) {
			handle->ptyn[i] = priv_state->new_ptyn[0][i];
			handle->ptyn[4 + i] = priv_state->new_ptyn[1][i];
		}
		handle->valid_fields |= V4L2_RDS_PTYN;
		updated_fields |= V4L2_RDS_PTYN;
	}
	return updated_fields;
}

typedef uint32_t (*decode_group_func)(struct rds_private_state *);

/* array of function pointers to contain all group specific decoding functions */
static const decode_group_func decode_group[16] = {
	[0] = rds_decode_group0,
	[1] = rds_decode_group1,
	[2] = rds_decode_group2,
	[3] = rds_decode_group3,
	[4] = rds_decode_group4,
	[10] = rds_decode_group10,
};

static uint32_t rds_decode_group(struct rds_private_state *priv_state)
{
	struct v4l2_rds *handle = &priv_state->handle;
	uint8_t group_id = priv_state->rds_group.group_id;

	/* count the group type, and decode it if it is supported */
	handle->rds_statistics.group_type_cnt[group_id]++;
	if (decode_group[group_id])
		return (*decode_group[group_id])(priv_state);
	return 0;
}

struct v4l2_rds *v4l2_rds_create(bool is_rbds)
{
	struct rds_private_state *internal_handle =
		calloc(1, sizeof(struct rds_private_state));
	internal_handle->handle.is_rbds = is_rbds;

	return (struct v4l2_rds *)internal_handle;
}

void v4l2_rds_destroy(struct v4l2_rds *handle)
{
	if (handle)
		free(handle);
}

void v4l2_rds_reset(struct v4l2_rds *handle, bool reset_statistics)
{
	/* treat the private & the public part of the handle */
	struct rds_private_state *priv_state = (struct rds_private_state *) handle;

	/* store members of handle that shouldn't be affected by reset */
	bool is_rbds = handle->is_rbds;
	struct v4l2_rds_statistics rds_statistics = handle->rds_statistics;

	/* reset the handle */
	memset(priv_state, 0, sizeof(*priv_state));
	/* re-initialize members */
	handle->is_rbds = is_rbds;
	if (!reset_statistics)
		handle->rds_statistics = rds_statistics;
}

/* function decodes raw RDS data blocks into complete groups. Once a full group is
 * successfully received, the group is decoded into the fields of the RDS handle.
 * Decoding is only done once a complete group was received. This is slower compared
 * to decoding the group type independent information up front, but adds a barrier
 * against corrupted data (happens regularly when reception is weak) */

/** 
 * 该函数将原始 RDS 数据块解码为完整的组。  
 * 一旦成功接收到完整的组，就会将其解码并存储到 RDS 句柄的字段中。  
 * 只有在接收到完整的组后才进行解码。  
 * 这种方式比直接解码与组类型无关的信息要慢，但能有效防止数据损坏，  
 * （在信号接收较弱时，数据损坏是常见情况）。 
 */
uint32_t v4l2_rds_add(struct v4l2_rds *handle, struct v4l2_rds_data *rds_data)
{
	struct rds_private_state *priv_state = (struct rds_private_state *) handle;
	struct v4l2_rds_data *rds_data_raw = priv_state->rds_data_raw;
	struct v4l2_rds_statistics *rds_stats = &handle->rds_statistics;

	uint32_t updated_fields = 0;
	uint8_t *decode_state = &(priv_state->decode_state);

	/* get the block id by masking out irrelevant bits */
	int block_id = rds_data->block & V4L2_RDS_BLOCK_MSK;

	rds_stats->block_cnt++;
	/* check for corrected / uncorrectable errors in the data */
	// 数据校验
	if (rds_data->block & V4L2_RDS_BLOCK_ERROR) {
		block_id = -1;
		rds_stats->block_error_cnt++;
	} else if (rds_data->block & V4L2_RDS_BLOCK_CORRECTED) {
		rds_stats->block_corrected_cnt++;
	}

	switch (*decode_state) {
	case RDS_EMPTY:
		if (block_id == 0) {
			*decode_state = RDS_A_RECEIVED;
			/* begin reception of a new data group, reset raw buffer to 0 */
			// 开始接收新的数据组，重置原始缓冲区为 0
			memset(rds_data_raw, 0, sizeof(rds_data_raw));
			rds_data_raw[0] = *rds_data;
		} else {
			/* ignore block if it is not the first block of a group */
			rds_stats->group_error_cnt++;
		}
		break;

	case RDS_A_RECEIVED:
		if (block_id == 1) {
			*decode_state = RDS_B_RECEIVED;
			rds_data_raw[1] = *rds_data;
		} else {
			/* received block with unexpected block id, reset state machine */
			rds_stats->group_error_cnt++;
			*decode_state = RDS_EMPTY;
		}
		break;

	case RDS_B_RECEIVED:
		/* handle type C and C' blocks alike */
		if (block_id == 2 || block_id ==  4) {
			*decode_state = RDS_C_RECEIVED;
			rds_data_raw[2] = *rds_data;
		} else {
			rds_stats->group_error_cnt++;
			*decode_state = RDS_EMPTY;
		}
		break;

	case RDS_C_RECEIVED:
		if (block_id == 3) {
			*decode_state = RDS_EMPTY;
			rds_data_raw[3] = *rds_data;
			/* a full group was received */
			rds_stats->group_cnt++;

			/* decode group type independent fields */
			// 解码与组类型无关的字段
			memset(&priv_state->rds_group, 0, sizeof(priv_state->rds_group));
			updated_fields |= rds_decode_a(priv_state, &rds_data_raw[0]);
			updated_fields |= rds_decode_b(priv_state, &rds_data_raw[1]);
			rds_decode_c(priv_state, &rds_data_raw[2]);
			rds_decode_d(priv_state, &rds_data_raw[3]);

			/* decode group type dependent fields */
			// 解码与组类型有关的字段
			updated_fields |= rds_decode_group(priv_state);
			return updated_fields;
		}
		rds_stats->group_error_cnt++;
		*decode_state = RDS_EMPTY;
		break;

	default:
		/* every unexpected block leads to a reset of the sm */
		rds_stats->group_error_cnt++;
		*decode_state = RDS_EMPTY;
	}
	/* if we reach here, no RDS group was completed */
	return 0;
}

const char *v4l2_rds_get_pty_str(const struct v4l2_rds *handle)
{
	const uint8_t pty = handle->pty;

	if (pty >= 32)
		return NULL;

	static const char *rds_lut[32] = {
		"None", "News", "Affairs", "Info", "Sport", "Education", "Drama",
		"Culture", "Science", "Varied Speech", "Pop Music",
		"Rock Music", "Easy Listening", "Light Classics M",
		"Serious Classics", "Other Music", "Weather", "Finance",
		"Children", "Social Affairs", "Religion", "Phone In",
		"Travel & Touring", "Leisure & Hobby", "Jazz Music",
		"Country Music", "National Music", "Oldies Music", "Folk Music",
		"Documentary", "Alarm Test", "Alarm!"
	};
	static const char *rbds_lut[32] = {
		"None", "News", "Information", "Sports", "Talk", "Rock",
		"Classic Rock", "Adult Hits", "Soft Rock", "Top 40", "Country",
		"Oldies", "Soft", "Nostalgia", "Jazz", "Classical",
		"R&B", "Soft R&B", "Foreign Language", "Religious Music",
		"Religious Talk", "Personality", "Public", "College",
		"Spanish Talk", "Spanish Music", "Hip-Hop", "Unassigned",
		"Unassigned", "Weather", "Emergency Test", "Emergency"
	};

	return handle->is_rbds ? rbds_lut[pty] : rds_lut[pty];
}

const char *v4l2_rds_get_country_str(const struct v4l2_rds *handle)
{
	/* defines the  region of the world
	 * 0x0e = Europe, 0x0d = Africa, 0x0a = ITU Region 2,
	 * 0x0f = ITU Region 3 */
	uint8_t ecc_h = handle->ecc >> 4;
	/* sub identifier for the region, valid range 0..4 */
	uint8_t ecc_l = handle->ecc & 0x0f;
	/* bits 12-15 pi contain the country code */
	uint8_t country_code = handle->pi >> 12;

	/* LUT for European countries
	 * the standard doesn't define every possible value but leaves some
	 * undefined. An exception is e4-7 which is defined as a dash ("-") */
	static const char *e_lut[5][16] = {
	{
		NULL, "DE", "DZ", "AD", "IL", "IT", "BE", "RU", "PS", "AL",
		"AT", "HU", "MT", "DE", NULL, "EG"
	}, {
		NULL, "GR", "CY", "SM", "CH", "JO", "FI", "LU", "BG", "DK",
		"GI", "IQ", "GB", "LY", "RO", "FR"
	}, {
		NULL, "MA", "CZ", "PL", "VA", "SK", "SY", "TN", NULL, "LI",
		"IS", "MC", "LT", "RS", "ES", "NO"
	}, {
		NULL, "ME", "IE", "TR", "MK", NULL, NULL, NULL, "NL", "LV",
		"LB", "AZ", "HR", "KZ", "SE", "BY"
	}, {
		NULL, "MD", "EE", "KG", NULL, NULL, "UA", "-", "PT", "SI",
		"AM", NULL, "GE", NULL, NULL, "BA"
	}
	};

	/* for now only European countries are supported -> ECC E0 - E4
	 * but the standard defines country codes for the whole world,
	 * that's the reason for returning "unknown" instead of a NULL
	 * pointer until all defined countries are supported */
	if (ecc_h == 0x0e && ecc_l <= 0x04)
		return e_lut[ecc_l][country_code];
	return "Unknown";
}

static const char *rds_language_lut(const uint8_t lc)
{
	const uint8_t max_lc = 127;
	const char *language;

	static const char *language_lut[128] = {
		"Unknown", "Albanian", "Breton", "Catalan",
		"Croatian", "Welsh", "Czech", "Danish",
		"German", "English", "Spanish", "Esperanto",
		"Estonian", "Basque", "Faroese", "French",
		"Frisian", "Irish", "Gaelic", "Galician",
		"Icelandic", "Italian", "Lappish", "Latin",
		"Latvian", "Luxembourgian", "Lithuanian", "Hungarian",
		"Maltese", "Dutch", "Norwegian", "Occitan",
		"Polish", "Portuguese", "Romanian", "Ramansh",
		"Serbian", "Slovak", "Slovene", "Finnish",
		"Swedish", "Turkish", "Flemish", "Walloon",
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, "Zulu", "Vietnamese", "Uzbek",
		"Urdu", "Ukrainian", "Thai", "Telugu",
		"Tatar", "Tamil", "Tadzhik", "Swahili",
		"Sranan Tongo", "Somali", "Sinhalese", "Shona",
		"Serbo-Croat", "Ruthenian", "Russian", "Quechua",
		"Pushtu", "Punjabi", "Persian", "Papamiento",
		"Oriya", "Nepali", "Ndebele", "Marathi",
		"Moldavian", "Malaysian", "Malagasay", "Macedonian",
		"Laotian", "Korean", "Khmer", "Kazahkh",
		"Kannada", "Japanese", "Indonesian", "Hindi",
		"Hebrew", "Hausa", "Gurani", "Gujurati",
		"Greek", "Georgian", "Fulani", "Dani",
		"Churash", "Chinese", "Burmese", "Bulgarian",
		"Bengali", "Belorussian", "Bambora", "Azerbaijani",
		"Assamese", "Armenian", "Arabic", "Amharic"
	};

	/* filter invalid values and undefined table entries */
	language = (lc > max_lc) ? "Unknown" : language_lut[lc];
	if (!language)
		return "Unknown";
	return language;
}

const char *v4l2_rds_get_language_str(const struct v4l2_rds *handle)
{
	return rds_language_lut(handle->lc);
}

const char *v4l2_rds_get_coverage_str(const struct v4l2_rds *handle)
{
	/* bits 8-11 contain the area coverage code */
	uint8_t coverage = (handle->pi >> 8) & 0X0f;
	static const char *coverage_lut[16] = {
		"Local", "International", "National", "Supra-Regional",
		"Regional 1", "Regional 2", "Regional 3", "Regional 4",
		"Regional 5", "Regional 6", "Regional 7", "Regional 8",
		"Regional 9", "Regional 10", "Regional 11", "Regional 12"
	};

	return coverage_lut[coverage];
}

const struct v4l2_rds_group *v4l2_rds_get_group
	(const struct v4l2_rds *handle)
{
	struct rds_private_state *priv_state = (struct rds_private_state *) handle;
	return &priv_state->rds_group;
}
