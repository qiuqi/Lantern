/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "ofp-actions.h"
#include "bundle.h"
#include "byte-order.h"
#include "compiler.h"
#include "dynamic-string.h"
#include "learn.h"
#include "meta-flow.h"
#include "multipath.h"
#include "nx-match.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "util.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(ofp_actions);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

/* Converting OpenFlow 1.0 to ofpacts. */

static enum ofperr
output_from_openflow10(const struct ofp10_action_output *oao,
                       struct ofpbuf *out)
{
    struct ofpact_output *output;

    output = ofpact_put_OUTPUT(out);
    output->port = ntohs(oao->port);
    output->max_len = ntohs(oao->max_len);

    return ofputil_check_output_port(output->port, OFPP_MAX);
}

static enum ofperr
enqueue_from_openflow10(const struct ofp10_action_enqueue *oae,
                        struct ofpbuf *out)
{
    struct ofpact_enqueue *enqueue;

    enqueue = ofpact_put_ENQUEUE(out);
    enqueue->port = ntohs(oae->port);
    enqueue->queue = ntohl(oae->queue_id);
    if (enqueue->port >= OFPP_MAX && enqueue->port != OFPP_IN_PORT
        && enqueue->port != OFPP_LOCAL) {
        return OFPERR_OFPBAC_BAD_OUT_PORT;
    }
    return 0;
}

static void
resubmit_from_openflow(const struct nx_action_resubmit *nar,
                       struct ofpbuf *out)
{
    struct ofpact_resubmit *resubmit;

    resubmit = ofpact_put_RESUBMIT(out);
    resubmit->ofpact.compat = OFPUTIL_NXAST_RESUBMIT;
    resubmit->in_port = ntohs(nar->in_port);
    resubmit->table_id = 0xff;
}

static enum ofperr
resubmit_table_from_openflow(const struct nx_action_resubmit *nar,
                             struct ofpbuf *out)
{
    struct ofpact_resubmit *resubmit;

    if (nar->pad[0] || nar->pad[1] || nar->pad[2]) {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    resubmit = ofpact_put_RESUBMIT(out);
    resubmit->ofpact.compat = OFPUTIL_NXAST_RESUBMIT_TABLE;
    resubmit->in_port = ntohs(nar->in_port);
    resubmit->table_id = nar->table;
    return 0;
}

static enum ofperr
output_reg_from_openflow(const struct nx_action_output_reg *naor,
                         struct ofpbuf *out)
{
    struct ofpact_output_reg *output_reg;

    if (!is_all_zeros(naor->zero, sizeof naor->zero)) {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    output_reg = ofpact_put_OUTPUT_REG(out);
    output_reg->src.field = mf_from_nxm_header(ntohl(naor->src));
    output_reg->src.ofs = nxm_decode_ofs(naor->ofs_nbits);
    output_reg->src.n_bits = nxm_decode_n_bits(naor->ofs_nbits);
    output_reg->max_len = ntohs(naor->max_len);

    return mf_check_src(&output_reg->src, NULL);
}

static void
fin_timeout_from_openflow(const struct nx_action_fin_timeout *naft,
                          struct ofpbuf *out)
{
    struct ofpact_fin_timeout *oft;

    oft = ofpact_put_FIN_TIMEOUT(out);
    oft->fin_idle_timeout = ntohs(naft->fin_idle_timeout);
    oft->fin_hard_timeout = ntohs(naft->fin_hard_timeout);
}

static void
controller_from_openflow(const struct nx_action_controller *nac,
                         struct ofpbuf *out)
{
    struct ofpact_controller *oc;

    oc = ofpact_put_CONTROLLER(out);
    oc->max_len = ntohs(nac->max_len);
    oc->controller_id = ntohs(nac->controller_id);
    oc->reason = nac->reason;
}

static enum ofperr
metadata_from_nxast(const struct nx_action_write_metadata *nawm,
                    struct ofpbuf *out)
{
    struct ofpact_metadata *om;

    if (!is_all_zeros(nawm->zeros, sizeof nawm->zeros)) {
        return OFPERR_NXBRC_MUST_BE_ZERO;
    }

    om = ofpact_put_WRITE_METADATA(out);
    om->metadata = nawm->metadata;
    om->mask = nawm->mask;

    return 0;
}

static void
note_from_openflow(const struct nx_action_note *nan, struct ofpbuf *out)
{
    struct ofpact_note *note;
    unsigned int length;

    length = ntohs(nan->len) - offsetof(struct nx_action_note, note);
    note = ofpact_put(out, OFPACT_NOTE,
                      offsetof(struct ofpact_note, data) + length);
    note->length = length;
    memcpy(note->data, nan->note, length);
}

static enum ofperr
dec_ttl_from_openflow(struct ofpbuf *out, enum ofputil_action_code compat)
{
    uint16_t id = 0;
    struct ofpact_cnt_ids *ids;
    enum ofperr error = 0;

    ids = ofpact_put_DEC_TTL(out);
    ids->ofpact.compat = compat;
    ids->n_controllers = 1;
    ofpbuf_put(out, &id, sizeof id);
    ids = out->l2;
    ofpact_update_len(out, &ids->ofpact);
    return error;
}

static enum ofperr
dec_ttl_cnt_ids_from_openflow(const struct nx_action_cnt_ids *nac_ids,
                      struct ofpbuf *out)
{
    struct ofpact_cnt_ids *ids;
    size_t ids_size;
    int i;

    ids = ofpact_put_DEC_TTL(out);
    ids->ofpact.compat = OFPUTIL_NXAST_DEC_TTL_CNT_IDS;
    ids->n_controllers = ntohs(nac_ids->n_controllers);
    ids_size = ntohs(nac_ids->len) - sizeof *nac_ids;

    if (!is_all_zeros(nac_ids->zeros, sizeof nac_ids->zeros)) {
        return OFPERR_NXBRC_MUST_BE_ZERO;
    }

    if (ids_size < ids->n_controllers * sizeof(ovs_be16)) {
        VLOG_WARN_RL(&rl, "Nicira action dec_ttl_cnt_ids only has %zu bytes "
                     "allocated for controller ids.  %zu bytes are required for "
                     "%"PRIu16" controllers.", ids_size,
                     ids->n_controllers * sizeof(ovs_be16), ids->n_controllers);
        return OFPERR_OFPBAC_BAD_LEN;
    }

    for (i = 0; i < ids->n_controllers; i++) {
        uint16_t id = ntohs(((ovs_be16 *)(nac_ids + 1))[i]);
        ofpbuf_put(out, &id, sizeof id);
    }

    ids = out->l2;
    ofpact_update_len(out, &ids->ofpact);

    return 0;
}

static enum ofperr
decode_nxast_action(const union ofp_action *a, enum ofputil_action_code *code)
{
    const struct nx_action_header *nah = (const struct nx_action_header *) a;
    uint16_t len = ntohs(a->header.len);

    if (len < sizeof(struct nx_action_header)) {
        return OFPERR_OFPBAC_BAD_LEN;
    } else if (a->vendor.vendor != CONSTANT_HTONL(NX_VENDOR_ID)) {
        return OFPERR_OFPBAC_BAD_VENDOR;
    }

    switch (nah->subtype) {
#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME)    \
        case CONSTANT_HTONS(ENUM):                      \
            if (EXTENSIBLE                              \
                ? len >= sizeof(struct STRUCT)          \
                : len == sizeof(struct STRUCT)) {       \
                *code = OFPUTIL_##ENUM;                 \
                return 0;                               \
            } else {                                    \
                return OFPERR_OFPBAC_BAD_LEN;           \
            }                                           \
            NOT_REACHED();
#include "ofp-util.def"

    case CONSTANT_HTONS(NXAST_SNAT__OBSOLETE):
    case CONSTANT_HTONS(NXAST_DROP_SPOOFED_ARP__OBSOLETE):
    default:
        return OFPERR_OFPBAC_BAD_TYPE;
    }
}

/* Parses 'a' to determine its type.  On success stores the correct type into
 * '*code' and returns 0.  On failure returns an OFPERR_* error code and
 * '*code' is indeterminate.
 *
 * The caller must have already verified that 'a''s length is potentially
 * correct (that is, a->header.len is nonzero and a multiple of sizeof(union
 * ofp_action) and no longer than the amount of space allocated to 'a').
 *
 * This function verifies that 'a''s length is correct for the type of action
 * that it represents. */
static enum ofperr
decode_openflow10_action(const union ofp_action *a,
                         enum ofputil_action_code *code)
{
    switch (a->type) {
    case CONSTANT_HTONS(OFPAT10_VENDOR):
        return decode_nxast_action(a, code);

#define OFPAT10_ACTION(ENUM, STRUCT, NAME)                          \
        case CONSTANT_HTONS(ENUM):                                  \
            if (a->header.len == htons(sizeof(struct STRUCT))) {    \
                *code = OFPUTIL_##ENUM;                             \
                return 0;                                           \
            } else {                                                \
                return OFPERR_OFPBAC_BAD_LEN;                       \
            }                                                       \
            break;
#include "ofp-util.def"

    default:
        return OFPERR_OFPBAC_BAD_TYPE;
    }
}

static enum ofperr
ofpact_from_nxast(const union ofp_action *a, enum ofputil_action_code code,
                  struct ofpbuf *out)
{
    const struct nx_action_resubmit *nar;
    const struct nx_action_set_tunnel *nast;
    const struct nx_action_set_queue *nasq;
    const struct nx_action_note *nan;
    const struct nx_action_set_tunnel64 *nast64;
    const struct nx_action_write_metadata *nawm;
    struct ofpact_tunnel *tunnel;
    enum ofperr error = 0;

    switch (code) {
    case OFPUTIL_ACTION_INVALID:
#define OFPAT10_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#define OFPAT11_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        NOT_REACHED();

    case OFPUTIL_NXAST_RESUBMIT:
        resubmit_from_openflow((const struct nx_action_resubmit *) a, out);
        break;

    case OFPUTIL_NXAST_SET_TUNNEL:
        nast = (const struct nx_action_set_tunnel *) a;
        tunnel = ofpact_put_SET_TUNNEL(out);
        tunnel->ofpact.compat = code;
        tunnel->tun_id = ntohl(nast->tun_id);
        break;

    case OFPUTIL_NXAST_WRITE_METADATA:
        nawm = (const struct nx_action_write_metadata *) a;
        error = metadata_from_nxast(nawm, out);
        break;

    case OFPUTIL_NXAST_SET_QUEUE:
        nasq = (const struct nx_action_set_queue *) a;
        ofpact_put_SET_QUEUE(out)->queue_id = ntohl(nasq->queue_id);
        break;

    case OFPUTIL_NXAST_POP_QUEUE:
        ofpact_put_POP_QUEUE(out);
        break;

    case OFPUTIL_NXAST_REG_MOVE:
        error = nxm_reg_move_from_openflow(
            (const struct nx_action_reg_move *) a, out);
        break;

    case OFPUTIL_NXAST_REG_LOAD:
        error = nxm_reg_load_from_openflow(
            (const struct nx_action_reg_load *) a, out);
        break;

    case OFPUTIL_NXAST_NOTE:
        nan = (const struct nx_action_note *) a;
        note_from_openflow(nan, out);
        break;

    case OFPUTIL_NXAST_SET_TUNNEL64:
        nast64 = (const struct nx_action_set_tunnel64 *) a;
        tunnel = ofpact_put_SET_TUNNEL(out);
        tunnel->ofpact.compat = code;
        tunnel->tun_id = ntohll(nast64->tun_id);
        break;

    case OFPUTIL_NXAST_MULTIPATH:
        error = multipath_from_openflow((const struct nx_action_multipath *) a,
                                        ofpact_put_MULTIPATH(out));
        break;

    case OFPUTIL_NXAST_BUNDLE:
    case OFPUTIL_NXAST_BUNDLE_LOAD:
        error = bundle_from_openflow((const struct nx_action_bundle *) a, out);
        break;

    case OFPUTIL_NXAST_OUTPUT_REG:
        error = output_reg_from_openflow(
            (const struct nx_action_output_reg *) a, out);
        break;

    case OFPUTIL_NXAST_RESUBMIT_TABLE:
        nar = (const struct nx_action_resubmit *) a;
        error = resubmit_table_from_openflow(nar, out);
        break;

    case OFPUTIL_NXAST_LEARN:
        error = learn_from_openflow((const struct nx_action_learn *) a, out);
        break;

    case OFPUTIL_NXAST_EXIT:
        ofpact_put_EXIT(out);
        break;

    case OFPUTIL_NXAST_DEC_TTL:
        error = dec_ttl_from_openflow(out, code);
        break;

    case OFPUTIL_NXAST_DEC_TTL_CNT_IDS:
        error = dec_ttl_cnt_ids_from_openflow(
                    (const struct nx_action_cnt_ids *) a, out);
        break;

    case OFPUTIL_NXAST_FIN_TIMEOUT:
        fin_timeout_from_openflow(
            (const struct nx_action_fin_timeout *) a, out);
        break;

    case OFPUTIL_NXAST_CONTROLLER:
        controller_from_openflow((const struct nx_action_controller *) a, out);
        break;

    case OFPUTIL_NXAST_PUSH_MPLS: {
        struct nx_action_push_mpls *nxapm = (struct nx_action_push_mpls *)a;
        if (!eth_type_mpls(nxapm->ethertype)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_PUSH_MPLS(out)->ethertype = nxapm->ethertype;
        break;
    }

    case OFPUTIL_NXAST_POP_MPLS: {
        struct nx_action_pop_mpls *nxapm = (struct nx_action_pop_mpls *)a;
        if (eth_type_mpls(nxapm->ethertype)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_POP_MPLS(out)->ethertype = nxapm->ethertype;
        break;
    }

#ifdef _OFP_CENTEC_
    case OFPUTIL_NXAST_PUSH_L2:
        ofpact_put_PUSH_L2(out);
        break;
        
    case OFPUTIL_NXAST_POP_L2:
        ofpact_put_POP_L2(out);
        break;
#endif        
    }

    return error;
}

static enum ofperr
ofpact_from_openflow10(const union ofp_action *a, struct ofpbuf *out)
{
    enum ofputil_action_code code;
    enum ofperr error;

    error = decode_openflow10_action(a, &code);
    if (error) {
        return error;
    }

    switch (code) {
    case OFPUTIL_ACTION_INVALID:
#define OFPAT11_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        NOT_REACHED();

    case OFPUTIL_OFPAT10_OUTPUT:
        return output_from_openflow10(&a->output10, out);

    case OFPUTIL_OFPAT10_SET_VLAN_VID:
        if (a->vlan_vid.vlan_vid & ~htons(0xfff)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_VID(out)->vlan_vid = ntohs(a->vlan_vid.vlan_vid);
        break;

    case OFPUTIL_OFPAT10_SET_VLAN_PCP:
        if (a->vlan_pcp.vlan_pcp & ~7) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_PCP(out)->vlan_pcp = a->vlan_pcp.vlan_pcp;
        break;

    case OFPUTIL_OFPAT10_STRIP_VLAN:
        ofpact_put_STRIP_VLAN(out);
        break;

    case OFPUTIL_OFPAT10_SET_DL_SRC:
        memcpy(ofpact_put_SET_ETH_SRC(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT10_SET_DL_DST:
        memcpy(ofpact_put_SET_ETH_DST(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT10_SET_NW_SRC:
        ofpact_put_SET_IPV4_SRC(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT10_SET_NW_DST:
        ofpact_put_SET_IPV4_DST(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT10_SET_NW_TOS:
        if (a->nw_tos.nw_tos & ~IP_DSCP_MASK) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_IPV4_DSCP(out)->dscp = a->nw_tos.nw_tos;
        break;

    case OFPUTIL_OFPAT10_SET_TP_SRC:
        ofpact_put_SET_L4_SRC_PORT(out)->port = ntohs(a->tp_port.tp_port);
        break;

    case OFPUTIL_OFPAT10_SET_TP_DST:
        ofpact_put_SET_L4_DST_PORT(out)->port = ntohs(a->tp_port.tp_port);

        break;

    case OFPUTIL_OFPAT10_ENQUEUE:
        error = enqueue_from_openflow10((const struct ofp10_action_enqueue *) a,
                                        out);
        break;

#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
	return ofpact_from_nxast(a, code, out);
    }

    return error;
}

static inline union ofp_action *
action_next(const union ofp_action *a)
{
    return ((union ofp_action *) (void *)
            ((uint8_t *) a + ntohs(a->header.len)));
}

static inline bool
action_is_valid(const union ofp_action *a, size_t n_actions)
{
    uint16_t len = ntohs(a->header.len);
    return (!(len % OFP_ACTION_ALIGN)
            && len >= sizeof *a
            && len / sizeof *a <= n_actions);
}

/* This macro is careful to check for actions with bad lengths. */
#define ACTION_FOR_EACH(ITER, LEFT, ACTIONS, N_ACTIONS)                 \
    for ((ITER) = (ACTIONS), (LEFT) = (N_ACTIONS);                      \
         (LEFT) > 0 && action_is_valid(ITER, LEFT);                     \
         ((LEFT) -= ntohs((ITER)->header.len) / sizeof(union ofp_action), \
          (ITER) = action_next(ITER)))

static void
log_bad_action(const union ofp_action *actions, size_t n_actions, size_t ofs,
               enum ofperr error)
{
    if (!VLOG_DROP_WARN(&rl)) {
        struct ds s;

        ds_init(&s);
        ds_put_hex_dump(&s, actions, n_actions * sizeof *actions, 0, false);
        VLOG_WARN("bad action at offset %#zx (%s):\n%s",
                  ofs * sizeof *actions, ofperr_get_name(error), ds_cstr(&s));
        ds_destroy(&s);
    }
}

static enum ofperr
ofpacts_from_openflow(const union ofp_action *in, size_t n_in,
                      struct ofpbuf *out,
                      enum ofperr (*ofpact_from_openflow)(
                          const union ofp_action *a, struct ofpbuf *out))
{
    const union ofp_action *a;
    size_t left;

    ACTION_FOR_EACH (a, left, in, n_in) {
        enum ofperr error = ofpact_from_openflow(a, out);
        if (error) {
            log_bad_action(in, n_in, a - in, error);
            return error;
        }
    }
    if (left) {
        enum ofperr error = OFPERR_OFPBAC_BAD_LEN;
        log_bad_action(in, n_in, n_in - left, error);
        return error;
    }

    ofpact_pad(out);
    return 0;
}

static enum ofperr
ofpacts_from_openflow10(const union ofp_action *in, size_t n_in,
                        struct ofpbuf *out)
{
    return ofpacts_from_openflow(in, n_in, out, ofpact_from_openflow10);
}

static enum ofperr
ofpacts_pull_actions(struct ofpbuf *openflow, unsigned int actions_len,
                     struct ofpbuf *ofpacts,
                     enum ofperr (*translate)(const union ofp_action *actions,
                                              size_t n_actions,
                                              struct ofpbuf *ofpacts))
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    const union ofp_action *actions;
    enum ofperr error;

    ofpbuf_clear(ofpacts);

    if (actions_len % OFP_ACTION_ALIGN != 0) {
        VLOG_WARN_RL(&rl, "OpenFlow message actions length %u is not a "
                     "multiple of %d", actions_len, OFP_ACTION_ALIGN);
        return OFPERR_OFPBRC_BAD_LEN;
    }

    actions = ofpbuf_try_pull(openflow, actions_len);
    if (actions == NULL) {
        VLOG_WARN_RL(&rl, "OpenFlow message actions length %u exceeds "
                     "remaining message length (%zu)",
                     actions_len, openflow->size);
        return OFPERR_OFPBRC_BAD_LEN;
    }

    error = translate(actions, actions_len / OFP_ACTION_ALIGN, ofpacts);
    if (error) {
        ofpbuf_clear(ofpacts);
        return error;
    }

    error = ofpacts_verify(ofpacts->data, ofpacts->size);
    if (error) {
        ofpbuf_clear(ofpacts);
    }
    return error;
}

/* Attempts to convert 'actions_len' bytes of OpenFlow 1.0 actions from the
 * front of 'openflow' into ofpacts.  On success, replaces any existing content
 * in 'ofpacts' by the converted ofpacts; on failure, clears 'ofpacts'.
 * Returns 0 if successful, otherwise an OpenFlow error.
 *
 * The parsed actions are valid generically, but they may not be valid in a
 * specific context.  For example, port numbers up to OFPP_MAX are valid
 * generically, but specific datapaths may only support port numbers in a
 * smaller range.  Use ofpacts_check() to additional check whether actions are
 * valid in a specific context. */
enum ofperr
ofpacts_pull_openflow10(struct ofpbuf *openflow, unsigned int actions_len,
                        struct ofpbuf *ofpacts)
{
    return ofpacts_pull_actions(openflow, actions_len, ofpacts,
                                ofpacts_from_openflow10);
}

/* OpenFlow 1.1 actions. */

/* Parses 'a' to determine its type.  On success stores the correct type into
 * '*code' and returns 0.  On failure returns an OFPERR_* error code and
 * '*code' is indeterminate.
 *
 * The caller must have already verified that 'a''s length is potentially
 * correct (that is, a->header.len is nonzero and a multiple of sizeof(union
 * ofp_action) and no longer than the amount of space allocated to 'a').
 *
 * This function verifies that 'a''s length is correct for the type of action
 * that it represents. */
static enum ofperr
decode_openflow11_action(const union ofp_action *a,
                         enum ofputil_action_code *code)
{
    uint16_t len;

    switch (a->type) {
    case CONSTANT_HTONS(OFPAT11_EXPERIMENTER):
        return decode_nxast_action(a, code);

#define OFPAT11_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME)  \
        case CONSTANT_HTONS(ENUM):                      \
            len = ntohs(a->header.len);                 \
            if (EXTENSIBLE                              \
                ? len >= sizeof(struct STRUCT)          \
                : len == sizeof(struct STRUCT)) {       \
                *code = OFPUTIL_##ENUM;                 \
                return 0;                               \
            } else {                                    \
                return OFPERR_OFPBAC_BAD_LEN;           \
            }                                           \
            NOT_REACHED();
#include "ofp-util.def"

    default:
        return OFPERR_OFPBAC_BAD_TYPE;
    }
}

static enum ofperr
output_from_openflow11(const struct ofp11_action_output *oao,
                       struct ofpbuf *out)
{
    struct ofpact_output *output;
    enum ofperr error;

    output = ofpact_put_OUTPUT(out);
    output->max_len = ntohs(oao->max_len);

    error = ofputil_port_from_ofp11(oao->port, &output->port);
    if (error) {
        return error;
    }

    return ofputil_check_output_port(output->port, OFPP_MAX);
}

static enum ofperr
ofpact_from_openflow11(const union ofp_action *a, struct ofpbuf *out)
{
    enum ofputil_action_code code;
    enum ofperr error;

    error = decode_openflow11_action(a, &code);
    if (error) {
        return error;
    }

    switch (code) {
    case OFPUTIL_ACTION_INVALID:
#define OFPAT10_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        NOT_REACHED();

    case OFPUTIL_OFPAT11_OUTPUT:
        return output_from_openflow11((const struct ofp11_action_output *) a,
                                      out);

    case OFPUTIL_OFPAT11_SET_VLAN_VID:
        if (a->vlan_vid.vlan_vid & ~htons(0xfff)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_VID(out)->vlan_vid = ntohs(a->vlan_vid.vlan_vid);
        break;

    case OFPUTIL_OFPAT11_SET_VLAN_PCP:
        if (a->vlan_pcp.vlan_pcp & ~7) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_PCP(out)->vlan_pcp = a->vlan_pcp.vlan_pcp;
        break;

    case OFPUTIL_OFPAT11_PUSH_VLAN:
#ifndef _OFP_CENTEC_
        if (((const struct ofp11_action_push *)a)->ethertype !=
            htons(ETH_TYPE_VLAN_8021Q)) {
            /* XXX 802.1AD(QinQ) isn't supported at the moment */
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_PUSH_VLAN(out);
#else
#endif
        ofpact_put_PUSH_VLAN(out)->ethertype = ((const struct ofp11_action_push *)a)->ethertype;
        break;

    case OFPUTIL_OFPAT11_POP_VLAN:
        ofpact_put_STRIP_VLAN(out);
        break;

    case OFPUTIL_OFPAT11_SET_QUEUE:
        ofpact_put_SET_QUEUE(out)->queue_id =
            ntohl(((const struct ofp11_action_set_queue *)a)->queue_id);
        break;

    case OFPUTIL_OFPAT11_SET_DL_SRC:
        memcpy(ofpact_put_SET_ETH_SRC(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT11_SET_DL_DST:
        memcpy(ofpact_put_SET_ETH_DST(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT11_DEC_NW_TTL:
        dec_ttl_from_openflow(out, code);
        break;

    case OFPUTIL_OFPAT11_SET_NW_SRC:
        ofpact_put_SET_IPV4_SRC(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT11_SET_NW_DST:
        ofpact_put_SET_IPV4_DST(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT11_SET_NW_TOS:
        if (a->nw_tos.nw_tos & ~IP_DSCP_MASK) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_IPV4_DSCP(out)->dscp = a->nw_tos.nw_tos;
        break;

    case OFPUTIL_OFPAT11_SET_TP_SRC:
        ofpact_put_SET_L4_SRC_PORT(out)->port = ntohs(a->tp_port.tp_port);
        break;

    case OFPUTIL_OFPAT11_SET_TP_DST:
        ofpact_put_SET_L4_DST_PORT(out)->port = ntohs(a->tp_port.tp_port);
        break;

    case OFPUTIL_OFPAT12_SET_FIELD:
        return nxm_reg_load_from_openflow12_set_field(
            (const struct ofp12_action_set_field *)a, out);

    case OFPUTIL_OFPAT11_PUSH_MPLS: {
        struct ofp11_action_push *oap = (struct ofp11_action_push *)a;
#ifndef _OFP_CENTEC_
        if (!eth_type_mpls(oap->ethertype)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
#endif
        ofpact_put_PUSH_MPLS(out)->ethertype = oap->ethertype;
        break;
    }

    case OFPUTIL_OFPAT11_POP_MPLS: {
        struct ofp11_action_pop_mpls *oapm = (struct ofp11_action_pop_mpls *)a;
#ifndef _OFP_CENTEC_
        /* OF 1.3 specify the ethertype of pop_mpls action is the payload 
         * ethertype, however in L2VPN case, the payload ethertype can be
         * IP/ARP and others, so we ignore this parameter as in old version.
         * */
        if (eth_type_mpls(oapm->ethertype)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
#endif
        ofpact_put_POP_MPLS(out)->ethertype = oapm->ethertype;
        break;
    }

#ifdef _OFP_CENTEC_
    case OFPUTIL_OFPAT11_GROUP: {
        struct ofp11_action_group *oag = (struct ofp11_action_group *)a;
        ofpact_put_GROUP(out)->group_id = ntohl(oag->group_id);
        break;
    }

    case OFPUTIL_OFPAT11_SET_MPLS_TTL: {
        struct ofp11_action_mpls_ttl *oamt= (struct ofp11_action_mpls_ttl *)a;
        ofpact_put_SET_MPLS_TTL(out)->mpls_ttl = oamt->mpls_ttl;
        break;
    }
#endif

#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        return ofpact_from_nxast(a, code, out);
    }

    return error;
}

static enum ofperr
ofpacts_from_openflow11(const union ofp_action *in, size_t n_in,
                        struct ofpbuf *out)
{
    return ofpacts_from_openflow(in, n_in, out, ofpact_from_openflow11);
}

/* OpenFlow 1.1 instructions. */

#define DEFINE_INST(ENUM, STRUCT, EXTENSIBLE, NAME)             \
    static inline const struct STRUCT *                         \
    instruction_get_##ENUM(const struct ofp11_instruction *inst)\
    {                                                           \
        ovs_assert(inst->type == htons(ENUM));                  \
        return (struct STRUCT *)inst;                           \
    }                                                           \
                                                                \
    static inline void                                          \
    instruction_init_##ENUM(struct STRUCT *s)                   \
    {                                                           \
        memset(s, 0, sizeof *s);                                \
        s->type = htons(ENUM);                                  \
        s->len = htons(sizeof *s);                              \
    }                                                           \
                                                                \
    static inline struct STRUCT *                               \
    instruction_put_##ENUM(struct ofpbuf *buf)                  \
    {                                                           \
        struct STRUCT *s = ofpbuf_put_uninit(buf, sizeof *s);   \
        instruction_init_##ENUM(s);                             \
        return s;                                               \
    }
OVS_INSTRUCTIONS
#undef DEFINE_INST

struct instruction_type_info {
    enum ovs_instruction_type type;
    const char *name;
};

static const struct instruction_type_info inst_info[] = {
#define DEFINE_INST(ENUM, STRUCT, EXTENSIBLE, NAME)    {OVSINST_##ENUM, NAME},
OVS_INSTRUCTIONS
#undef DEFINE_INST
};

const char *
ofpact_instruction_name_from_type(enum ovs_instruction_type type)
{
    return inst_info[type].name;
}

int
ofpact_instruction_type_from_name(const char *name)
{
    const struct instruction_type_info *p;
    for (p = inst_info; p < &inst_info[ARRAY_SIZE(inst_info)]; p++) {
        if (!strcasecmp(name, p->name)) {
            return p->type;
        }
    }
    return -1;
}

static inline struct ofp11_instruction *
instruction_next(const struct ofp11_instruction *inst)
{
    return ((struct ofp11_instruction *) (void *)
            ((uint8_t *) inst + ntohs(inst->len)));
}

static inline bool
instruction_is_valid(const struct ofp11_instruction *inst,
                     size_t n_instructions)
{
    uint16_t len = ntohs(inst->len);
    return (!(len % OFP11_INSTRUCTION_ALIGN)
            && len >= sizeof *inst
            && len / sizeof *inst <= n_instructions);
}

/* This macro is careful to check for instructions with bad lengths. */
#define INSTRUCTION_FOR_EACH(ITER, LEFT, INSTRUCTIONS, N_INSTRUCTIONS)  \
    for ((ITER) = (INSTRUCTIONS), (LEFT) = (N_INSTRUCTIONS);            \
         (LEFT) > 0 && instruction_is_valid(ITER, LEFT);                \
         ((LEFT) -= (ntohs((ITER)->len)                                 \
                     / sizeof(struct ofp11_instruction)),               \
          (ITER) = instruction_next(ITER)))

static enum ofperr
decode_openflow11_instruction(const struct ofp11_instruction *inst,
                              enum ovs_instruction_type *type)
{
    uint16_t len = ntohs(inst->len);

    switch (inst->type) {
    case CONSTANT_HTONS(OFPIT11_EXPERIMENTER):
        return OFPERR_OFPBIC_BAD_EXPERIMENTER;

#define DEFINE_INST(ENUM, STRUCT, EXTENSIBLE, NAME)     \
        case CONSTANT_HTONS(ENUM):                      \
            if (EXTENSIBLE                              \
                ? len >= sizeof(struct STRUCT)          \
                : len == sizeof(struct STRUCT)) {       \
                *type = OVSINST_##ENUM;                 \
                return 0;                               \
            } else {                                    \
                return OFPERR_OFPBIC_BAD_LEN;           \
            }
OVS_INSTRUCTIONS
#undef DEFINE_INST

    default:
        return OFPERR_OFPBIC_UNKNOWN_INST;
    }
}

static enum ofperr
decode_openflow11_instructions(const struct ofp11_instruction insts[],
                               size_t n_insts,
                               const struct ofp11_instruction *out[])
{
    const struct ofp11_instruction *inst;
    size_t left;

    memset(out, 0, N_OVS_INSTRUCTIONS * sizeof *out);
    INSTRUCTION_FOR_EACH (inst, left, insts, n_insts) {
        enum ovs_instruction_type type;
        enum ofperr error;

        error = decode_openflow11_instruction(inst, &type);
        if (error) {
            return error;
        }

        if (out[type]) {
            return OFPERR_OFPBAC_UNSUPPORTED_ORDER; /* No specific code for
                                                     * a duplicate instruction
                                                     * exist */
        }
        out[type] = inst;
    }

    if (left) {
        VLOG_WARN_RL(&rl, "bad instruction format at offset %zu",
                     (n_insts - left) * sizeof *inst);
        return OFPERR_OFPBIC_BAD_LEN;
    }
    return 0;
}

static void
get_actions_from_instruction(const struct ofp11_instruction *inst,
                         const union ofp_action **actions,
                         size_t *n_actions)
{
    *actions = (const union ofp_action *) (inst + 1);
    *n_actions = (ntohs(inst->len) - sizeof *inst) / OFP11_INSTRUCTION_ALIGN;
}

/* Attempts to convert 'actions_len' bytes of OpenFlow 1.1 actions from the
 * front of 'openflow' into ofpacts.  On success, replaces any existing content
 * in 'ofpacts' by the converted ofpacts; on failure, clears 'ofpacts'.
 * Returns 0 if successful, otherwise an OpenFlow error.
 *
 * In most places in OpenFlow 1.1 and 1.2, actions appear encapsulated in
 * instructions, so you should call ofpacts_pull_openflow11_instructions()
 * instead of this function.
 *
 * The parsed actions are valid generically, but they may not be valid in a
 * specific context.  For example, port numbers up to OFPP_MAX are valid
 * generically, but specific datapaths may only support port numbers in a
 * smaller range.  Use ofpacts_check() to additional check whether actions are
 * valid in a specific context. */
enum ofperr
ofpacts_pull_openflow11_actions(struct ofpbuf *openflow,
                                unsigned int actions_len,
                                struct ofpbuf *ofpacts)
{
    return ofpacts_pull_actions(openflow, actions_len, ofpacts,
                                ofpacts_from_openflow11);
}

enum ofperr
ofpacts_pull_openflow11_instructions(struct ofpbuf *openflow,
                                     unsigned int instructions_len,
                                     struct ofpbuf *ofpacts)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    const struct ofp11_instruction *instructions;
    const struct ofp11_instruction *insts[N_OVS_INSTRUCTIONS];
    enum ofperr error;

    ofpbuf_clear(ofpacts);

    if (instructions_len % OFP11_INSTRUCTION_ALIGN != 0) {
        VLOG_WARN_RL(&rl, "OpenFlow message instructions length %u is not a "
                     "multiple of %d",
                     instructions_len, OFP11_INSTRUCTION_ALIGN);
        error = OFPERR_OFPBIC_BAD_LEN;
        goto exit;
    }

    instructions = ofpbuf_try_pull(openflow, instructions_len);
    if (instructions == NULL) {
        VLOG_WARN_RL(&rl, "OpenFlow message instructions length %u exceeds "
                     "remaining message length (%zu)",
                     instructions_len, openflow->size);
        error = OFPERR_OFPBIC_BAD_LEN;
        goto exit;
    }

    error = decode_openflow11_instructions(
        instructions, instructions_len / OFP11_INSTRUCTION_ALIGN,
        insts);
    if (error) {
        goto exit;
    }

#ifdef _OFP_CENTEC_
    if (insts[OVSINST_OFPIT13_METER]) {
        const struct ofp13_instruction_meter *oim;
        struct ofpact_meter *om;
        oim = instruction_get_OFPIT13_METER(
            insts[OVSINST_OFPIT13_METER]);
        om = ofpact_put_METER(ofpacts);
        om->meter_id = ntohl(oim->meter_id);
    }
#endif

    if (insts[OVSINST_OFPIT11_APPLY_ACTIONS]) {
        const union ofp_action *actions;
        size_t n_actions;

        get_actions_from_instruction(insts[OVSINST_OFPIT11_APPLY_ACTIONS],
                                     &actions, &n_actions);
        error = ofpacts_from_openflow11(actions, n_actions, ofpacts);
        if (error) {
            goto exit;
        }
    }
    if (insts[OVSINST_OFPIT11_CLEAR_ACTIONS]) {
        instruction_get_OFPIT11_CLEAR_ACTIONS(
            insts[OVSINST_OFPIT11_CLEAR_ACTIONS]);
        ofpact_put_CLEAR_ACTIONS(ofpacts);
    }
    /* XXX Write-Actions */
#ifdef _OFP_CENTEC_
    /* support write actions */
    if (insts[OVSINST_OFPIT11_WRITE_ACTIONS]) {
        const union ofp_action *actions;
        size_t n_actions;

        if (insts[OVSINST_OFPIT11_APPLY_ACTIONS]) {
            error = OFPERR_OFPBIC_UNSUP_INST;
            if (error) {
                VLOG_ERR("Instruction Apply-Actions and Write-Actions can not be executed at the same time, it is not supported.");
                goto exit;
            }
        }

        get_actions_from_instruction(insts[OVSINST_OFPIT11_WRITE_ACTIONS],
                                     &actions, &n_actions);
        /* we need to re-order the action list to action set in particular order specified in the spec1.3 */
        /*
        The output action in the action set is executed last. If both an output action and a group action are
        specified in an action set, the output action is ignored and the group action takes precedence. If no
        output action and no group action were specified in an action set, the packet is dropped. The execution
        of groups is recursive if the switch supports it; a group bucket may specify another group, in which case
        the execution of actions traverses all the groups specified by the group configuration.
        */
        /* TODO add translation function from action-list to action-set */
        error = ofpacts_from_openflow11(actions, n_actions, ofpacts);
        if (error) {
            goto exit;
        }
    }
#endif
    if (insts[OVSINST_OFPIT11_WRITE_METADATA]) {
#ifndef _OFP_CENTEC_
        const struct ofp11_instruction_write_metadata *oiwm;
        struct ofpact_metadata *om;

        oiwm = (const struct ofp11_instruction_write_metadata *)
            insts[OVSINST_OFPIT11_WRITE_METADATA];

        om = ofpact_put_WRITE_METADATA(ofpacts);
        om->metadata = oiwm->metadata;
        om->mask = oiwm->metadata_mask;
#else
        error = OFPERR_OFPBIC_UNSUP_INST;
        goto exit;
#endif
    }
    if (insts[OVSINST_OFPIT11_GOTO_TABLE]) {
#ifndef _OFP_CENTEC_
        const struct ofp11_instruction_goto_table *oigt;
        struct ofpact_goto_table *ogt;
        oigt = instruction_get_OFPIT11_GOTO_TABLE(
            insts[OVSINST_OFPIT11_GOTO_TABLE]);
        ogt = ofpact_put_GOTO_TABLE(ofpacts);
        ogt->table_id = oigt->table_id;
#else
        error = OFPERR_OFPBIC_UNSUP_INST;
        goto exit;
#endif
    }

#ifndef _OFP_CENTEC_
    if (insts[OVSINST_OFPIT11_WRITE_ACTIONS]) {
        error = OFPERR_OFPBIC_UNSUP_INST;
        goto exit;
    }
#endif

    error = ofpacts_verify(ofpacts->data, ofpacts->size);
exit:
    if (error) {
        ofpbuf_clear(ofpacts);
    }
    return error;
}

static enum ofperr
ofpact_check__(const struct ofpact *a, const struct flow *flow, int max_ports,
               ovs_be16 *dl_type)
{
    const struct ofpact_enqueue *enqueue;
    const struct ofpact_reg_load * load;

    switch (a->type) {
    case OFPACT_OUTPUT:
        return ofputil_check_output_port(ofpact_get_OUTPUT(a)->port,
                                         max_ports);

    case OFPACT_CONTROLLER:
        return 0;

    case OFPACT_ENQUEUE:
        enqueue = ofpact_get_ENQUEUE(a);
        if (enqueue->port >= max_ports && enqueue->port != OFPP_IN_PORT
            && enqueue->port != OFPP_LOCAL) {
            return OFPERR_OFPBAC_BAD_OUT_PORT;
        }
        return 0;

    case OFPACT_OUTPUT_REG:
        return mf_check_src(&ofpact_get_OUTPUT_REG(a)->src, flow);

    case OFPACT_BUNDLE:
        return bundle_check(ofpact_get_BUNDLE(a), max_ports, flow);

    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_PUSH_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
        return 0;

    case OFPACT_REG_MOVE:
        return nxm_reg_move_check(ofpact_get_REG_MOVE(a), flow);

    case OFPACT_REG_LOAD:
#ifdef _OFP_CENTEC_ /* skip this check, process it in adapt */
        load = ofpact_get_REG_LOAD(a);
        if ( MFF_MPLS_TC == load->dst.field->id
            || MFF_MPLS_LABEL == load->dst.field->id) {
            return 0;
        }
#endif /*_OFP_CENTEC_*/
        if (*dl_type != flow->dl_type) {
            struct flow updated_flow = *flow;
            updated_flow.dl_type = *dl_type;
            return nxm_reg_load_check(ofpact_get_REG_LOAD(a), &updated_flow);
        } else {
            return nxm_reg_load_check(ofpact_get_REG_LOAD(a), flow);
        }

    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
        return 0;

    case OFPACT_LEARN:
        return learn_check(ofpact_get_LEARN(a), flow);

    case OFPACT_MULTIPATH:
        return multipath_check(ofpact_get_MULTIPATH(a), flow);

    case OFPACT_NOTE:
    case OFPACT_EXIT:
        return 0;

    case OFPACT_PUSH_MPLS:
        *dl_type = ofpact_get_PUSH_MPLS(a)->ethertype;
        return 0;

    case OFPACT_POP_MPLS:
        *dl_type = ofpact_get_POP_MPLS(a)->ethertype;
        return 0;

#ifdef _OFP_CENTEC_
    case OFPACT_PUSH_L2:
    case OFPACT_POP_L2:
    case OFPACT_SET_MPLS_TTL:
        return 0;
#endif        

    case OFPACT_CLEAR_ACTIONS:
    case OFPACT_WRITE_METADATA:
    case OFPACT_GOTO_TABLE:
        return 0;
        
#ifdef _OFP_CENTEC_
    case OFPACT_GROUP:
    case OFPACT_METER:
        return 0;
#endif

    default:
        NOT_REACHED();
    }
}

/* Checks that the 'ofpacts_len' bytes of actions in 'ofpacts' are
 * appropriate for a packet with the prerequisites satisfied by 'flow' in a
 * switch with no more than 'max_ports' ports. */
enum ofperr
ofpacts_check(const struct ofpact ofpacts[], size_t ofpacts_len,
              const struct flow *flow, int max_ports)
{
    const struct ofpact *a;
    ovs_be16 dl_type = flow->dl_type;

    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        enum ofperr error = ofpact_check__(a, flow, max_ports, &dl_type);
        if (error) {
            return error;
        }
    }

    return 0;
}

/* Verifies that the 'ofpacts_len' bytes of actions in 'ofpacts' are
 * in the appropriate order as defined by the OpenFlow spec. */
enum ofperr
ofpacts_verify(const struct ofpact ofpacts[], size_t ofpacts_len)
{
    const struct ofpact *a;
    enum ovs_instruction_type inst;

    inst = OVSINST_OFPIT11_APPLY_ACTIONS;
    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        enum ovs_instruction_type next;

        if (a->type == OFPACT_CLEAR_ACTIONS) {
            next = OVSINST_OFPIT11_CLEAR_ACTIONS;
        } else if (a->type == OFPACT_WRITE_METADATA) {
            next = OVSINST_OFPIT11_WRITE_METADATA;
        } else if (a->type == OFPACT_GOTO_TABLE) {
            next = OVSINST_OFPIT11_GOTO_TABLE;
        } else {
            next = OVSINST_OFPIT11_APPLY_ACTIONS;
        }

        if (inst != OVSINST_OFPIT11_APPLY_ACTIONS && next <= inst) {
            const char *name = ofpact_instruction_name_from_type(inst);
            const char *next_name = ofpact_instruction_name_from_type(next);

            if (next == inst) {
                VLOG_WARN("duplicate %s instruction not allowed, for OpenFlow "
                          "1.1+ compatibility", name);
            } else {
                VLOG_WARN("invalid instruction ordering: %s must appear "
                          "before %s, for OpenFlow 1.1+ compatibility",
                          next_name, name);
            }
            return OFPERR_OFPBAC_UNSUPPORTED_ORDER;
        }

        inst = next;
    }

    return 0;
}

/* Converting ofpacts to Nicira OpenFlow extensions. */

static void
ofpact_output_reg_to_nxast(const struct ofpact_output_reg *output_reg,
                                struct ofpbuf *out)
{
    struct nx_action_output_reg *naor = ofputil_put_NXAST_OUTPUT_REG(out);

    naor->ofs_nbits = nxm_encode_ofs_nbits(output_reg->src.ofs,
                                           output_reg->src.n_bits);
    naor->src = htonl(output_reg->src.field->nxm_header);
    naor->max_len = htons(output_reg->max_len);
}

static void
ofpact_resubmit_to_nxast(const struct ofpact_resubmit *resubmit,
                         struct ofpbuf *out)
{
    struct nx_action_resubmit *nar;

    if (resubmit->table_id == 0xff
        && resubmit->ofpact.compat != OFPUTIL_NXAST_RESUBMIT_TABLE) {
        nar = ofputil_put_NXAST_RESUBMIT(out);
    } else {
        nar = ofputil_put_NXAST_RESUBMIT_TABLE(out);
        nar->table = resubmit->table_id;
    }
    nar->in_port = htons(resubmit->in_port);
}

static void
ofpact_set_tunnel_to_nxast(const struct ofpact_tunnel *tunnel,
                           struct ofpbuf *out)
{
    uint64_t tun_id = tunnel->tun_id;

    if (tun_id <= UINT32_MAX
        && tunnel->ofpact.compat != OFPUTIL_NXAST_SET_TUNNEL64) {
        ofputil_put_NXAST_SET_TUNNEL(out)->tun_id = htonl(tun_id);
    } else {
        ofputil_put_NXAST_SET_TUNNEL64(out)->tun_id = htonll(tun_id);
    }
}

static void
ofpact_write_metadata_to_nxast(const struct ofpact_metadata *om,
                               struct ofpbuf *out)
{
    struct nx_action_write_metadata *nawm;

    nawm = ofputil_put_NXAST_WRITE_METADATA(out);
    nawm->metadata = om->metadata;
    nawm->mask = om->mask;
}

static void
ofpact_note_to_nxast(const struct ofpact_note *note, struct ofpbuf *out)
{
    size_t start_ofs = out->size;
    struct nx_action_note *nan;
    unsigned int remainder;
    unsigned int len;

    nan = ofputil_put_NXAST_NOTE(out);
    out->size -= sizeof nan->note;

    ofpbuf_put(out, note->data, note->length);

    len = out->size - start_ofs;
    remainder = len % OFP_ACTION_ALIGN;
    if (remainder) {
        ofpbuf_put_zeros(out, OFP_ACTION_ALIGN - remainder);
    }
    nan = (struct nx_action_note *)((char *)out->data + start_ofs);
    nan->len = htons(out->size - start_ofs);
}

static void
ofpact_controller_to_nxast(const struct ofpact_controller *oc,
                           struct ofpbuf *out)
{
    struct nx_action_controller *nac;

    nac = ofputil_put_NXAST_CONTROLLER(out);
    nac->max_len = htons(oc->max_len);
    nac->controller_id = htons(oc->controller_id);
    nac->reason = oc->reason;
}

static void
ofpact_dec_ttl_to_nxast(const struct ofpact_cnt_ids *oc_ids,
                        struct ofpbuf *out)
{
    if (oc_ids->ofpact.compat == OFPUTIL_NXAST_DEC_TTL) {
        ofputil_put_NXAST_DEC_TTL(out);
    } else {
        struct nx_action_cnt_ids *nac_ids =
            ofputil_put_NXAST_DEC_TTL_CNT_IDS(out);
        int ids_len = ROUND_UP(2 * oc_ids->n_controllers, OFP_ACTION_ALIGN);
        ovs_be16 *ids;
        size_t i;

        nac_ids->len = htons(ntohs(nac_ids->len) + ids_len);
        nac_ids->n_controllers = htons(oc_ids->n_controllers);

        ids = ofpbuf_put_zeros(out, ids_len);
        for (i = 0; i < oc_ids->n_controllers; i++) {
            ids[i] = htons(oc_ids->cnt_ids[i]);
        }
    }
}

static void
ofpact_fin_timeout_to_nxast(const struct ofpact_fin_timeout *fin_timeout,
                            struct ofpbuf *out)
{
    struct nx_action_fin_timeout *naft = ofputil_put_NXAST_FIN_TIMEOUT(out);
    naft->fin_idle_timeout = htons(fin_timeout->fin_idle_timeout);
    naft->fin_hard_timeout = htons(fin_timeout->fin_hard_timeout);
}

static void
ofpact_to_nxast(const struct ofpact *a, struct ofpbuf *out)
{
    switch (a->type) {
    case OFPACT_CONTROLLER:
        ofpact_controller_to_nxast(ofpact_get_CONTROLLER(a), out);
        break;

    case OFPACT_OUTPUT_REG:
        ofpact_output_reg_to_nxast(ofpact_get_OUTPUT_REG(a), out);
        break;

    case OFPACT_BUNDLE:
        bundle_to_nxast(ofpact_get_BUNDLE(a), out);
        break;

    case OFPACT_REG_MOVE:
        nxm_reg_move_to_nxast(ofpact_get_REG_MOVE(a), out);
        break;

    case OFPACT_REG_LOAD:
        nxm_reg_load_to_nxast(ofpact_get_REG_LOAD(a), out);
        break;

    case OFPACT_DEC_TTL:
        ofpact_dec_ttl_to_nxast(ofpact_get_DEC_TTL(a), out);
        break;

    case OFPACT_SET_TUNNEL:
        ofpact_set_tunnel_to_nxast(ofpact_get_SET_TUNNEL(a), out);
        break;

    case OFPACT_WRITE_METADATA:
        ofpact_write_metadata_to_nxast(ofpact_get_WRITE_METADATA(a), out);
        break;

    case OFPACT_SET_QUEUE:
        ofputil_put_NXAST_SET_QUEUE(out)->queue_id
            = htonl(ofpact_get_SET_QUEUE(a)->queue_id);
        break;

    case OFPACT_POP_QUEUE:
        ofputil_put_NXAST_POP_QUEUE(out);
        break;

    case OFPACT_FIN_TIMEOUT:
        ofpact_fin_timeout_to_nxast(ofpact_get_FIN_TIMEOUT(a), out);
        break;

    case OFPACT_RESUBMIT:
        ofpact_resubmit_to_nxast(ofpact_get_RESUBMIT(a), out);
        break;

    case OFPACT_LEARN:
        learn_to_nxast(ofpact_get_LEARN(a), out);
        break;

    case OFPACT_MULTIPATH:
        multipath_to_nxast(ofpact_get_MULTIPATH(a), out);
        break;

    case OFPACT_NOTE:
        ofpact_note_to_nxast(ofpact_get_NOTE(a), out);
        break;

    case OFPACT_EXIT:
        ofputil_put_NXAST_EXIT(out);
        break;

    case OFPACT_PUSH_MPLS:
        ofputil_put_NXAST_PUSH_MPLS(out)->ethertype =
            ofpact_get_PUSH_MPLS(a)->ethertype;
        break;

    case OFPACT_POP_MPLS:
        ofputil_put_NXAST_POP_MPLS(out)->ethertype =
            ofpact_get_POP_MPLS(a)->ethertype;
        break;

#ifdef _OFP_CENTEC_
    case OFPACT_PUSH_L2:
        ofputil_put_NXAST_PUSH_L2(out);
        break;
        
    case OFPACT_POP_L2:
        ofputil_put_NXAST_POP_L2(out); 
        break;

    case OFPACT_SET_MPLS_TTL:
    case OFPACT_METER:
    case OFPACT_GROUP:
        NOT_REACHED();
#endif

    case OFPACT_OUTPUT:
    case OFPACT_ENQUEUE:
    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_PUSH_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
    case OFPACT_CLEAR_ACTIONS:
    case OFPACT_GOTO_TABLE:
        NOT_REACHED();
    }
}

/* Converting ofpacts to OpenFlow 1.0. */

static void
ofpact_output_to_openflow10(const struct ofpact_output *output,
                            struct ofpbuf *out)
{
    struct ofp10_action_output *oao;

    oao = ofputil_put_OFPAT10_OUTPUT(out);
    oao->port = htons(output->port);
    oao->max_len = htons(output->max_len);
}

static void
ofpact_enqueue_to_openflow10(const struct ofpact_enqueue *enqueue,
                             struct ofpbuf *out)
{
    struct ofp10_action_enqueue *oae;

    oae = ofputil_put_OFPAT10_ENQUEUE(out);
    oae->port = htons(enqueue->port);
    oae->queue_id = htonl(enqueue->queue);
}

static void
ofpact_to_openflow10(const struct ofpact *a, struct ofpbuf *out)
{
    switch (a->type) {
    case OFPACT_OUTPUT:
        ofpact_output_to_openflow10(ofpact_get_OUTPUT(a), out);
        break;

    case OFPACT_ENQUEUE:
        ofpact_enqueue_to_openflow10(ofpact_get_ENQUEUE(a), out);
        break;

    case OFPACT_SET_VLAN_VID:
        ofputil_put_OFPAT10_SET_VLAN_VID(out)->vlan_vid
            = htons(ofpact_get_SET_VLAN_VID(a)->vlan_vid);
        break;

    case OFPACT_SET_VLAN_PCP:
        ofputil_put_OFPAT10_SET_VLAN_PCP(out)->vlan_pcp
            = ofpact_get_SET_VLAN_PCP(a)->vlan_pcp;
        break;

    case OFPACT_STRIP_VLAN:
        ofputil_put_OFPAT10_STRIP_VLAN(out);
        break;

    case OFPACT_SET_ETH_SRC:
        memcpy(ofputil_put_OFPAT10_SET_DL_SRC(out)->dl_addr,
               ofpact_get_SET_ETH_SRC(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_ETH_DST:
        memcpy(ofputil_put_OFPAT10_SET_DL_DST(out)->dl_addr,
               ofpact_get_SET_ETH_DST(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_IPV4_SRC:
        ofputil_put_OFPAT10_SET_NW_SRC(out)->nw_addr
            = ofpact_get_SET_IPV4_SRC(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DST:
        ofputil_put_OFPAT10_SET_NW_DST(out)->nw_addr
            = ofpact_get_SET_IPV4_DST(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DSCP:
        ofputil_put_OFPAT10_SET_NW_TOS(out)->nw_tos
            = ofpact_get_SET_IPV4_DSCP(a)->dscp;
        break;

    case OFPACT_SET_L4_SRC_PORT:
        ofputil_put_OFPAT10_SET_TP_SRC(out)->tp_port
            = htons(ofpact_get_SET_L4_SRC_PORT(a)->port);
        break;

    case OFPACT_SET_L4_DST_PORT:
        ofputil_put_OFPAT10_SET_TP_DST(out)->tp_port
            = htons(ofpact_get_SET_L4_DST_PORT(a)->port);
        break;

    case OFPACT_PUSH_VLAN:
    case OFPACT_CLEAR_ACTIONS:
    case OFPACT_GOTO_TABLE:
        /* XXX */
        break;

#ifdef _OFP_CENTEC_
    case OFPACT_METER:
    case OFPACT_SET_MPLS_TTL:
    case OFPACT_GROUP:
        break;
#endif

    case OFPACT_CONTROLLER:
    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_REG_MOVE:
    case OFPACT_REG_LOAD:
    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_WRITE_METADATA:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
    case OFPACT_PUSH_MPLS:
    case OFPACT_POP_MPLS:
#ifdef _OFP_CENTEC_
    /* Because NX is not restricted to any openflow spec, so we 
     * have to handle it here for safety. */
    case OFPACT_PUSH_L2:
    case OFPACT_POP_L2:
#endif
        ofpact_to_nxast(a, out);
        break;
    }
}

/* Converts the 'ofpacts_len' bytes of ofpacts in 'ofpacts' into OpenFlow 1.0
 * actions in 'openflow', appending the actions to any existing data in
 * 'openflow'. */
void
ofpacts_put_openflow10(const struct ofpact ofpacts[], size_t ofpacts_len,
                       struct ofpbuf *openflow)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        ofpact_to_openflow10(a, openflow);
    }
}

/* Converting ofpacts to OpenFlow 1.1. */

static void
ofpact_output_to_openflow11(const struct ofpact_output *output,
                            struct ofpbuf *out)
{
    struct ofp11_action_output *oao;

    oao = ofputil_put_OFPAT11_OUTPUT(out);
    oao->port = ofputil_port_to_ofp11(output->port);
    oao->max_len = htons(output->max_len);
}

static void
ofpact_dec_ttl_to_openflow11(const struct ofpact_cnt_ids *dec_ttl,
                             struct ofpbuf *out)
{
    if (dec_ttl->n_controllers == 1 && dec_ttl->cnt_ids[0] == 0
        && (!dec_ttl->ofpact.compat ||
            dec_ttl->ofpact.compat == OFPUTIL_OFPAT11_DEC_NW_TTL)) {
        ofputil_put_OFPAT11_DEC_NW_TTL(out);
    } else {
        ofpact_dec_ttl_to_nxast(dec_ttl, out);
    }
}

static void
ofpact_to_openflow11(const struct ofpact *a, struct ofpbuf *out)
{
    switch (a->type) {
    case OFPACT_OUTPUT:
        return ofpact_output_to_openflow11(ofpact_get_OUTPUT(a), out);

    case OFPACT_ENQUEUE:
        /* XXX */
        break;

    case OFPACT_SET_VLAN_VID:
        ofputil_put_OFPAT11_SET_VLAN_VID(out)->vlan_vid
            = htons(ofpact_get_SET_VLAN_VID(a)->vlan_vid);
        break;

    case OFPACT_SET_VLAN_PCP:
        ofputil_put_OFPAT11_SET_VLAN_PCP(out)->vlan_pcp
            = ofpact_get_SET_VLAN_PCP(a)->vlan_pcp;
        break;

    case OFPACT_STRIP_VLAN:
        ofputil_put_OFPAT11_POP_VLAN(out);
        break;

    case OFPACT_PUSH_VLAN:
        /* XXX ETH_TYPE_VLAN_8021AD case */
#ifndef _OFP_CENTEC_
        ofputil_put_OFPAT11_PUSH_VLAN(out)->ethertype =
            htons(ETH_TYPE_VLAN_8021Q);
#else
        ofputil_put_OFPAT11_PUSH_VLAN(out)->ethertype =
            ofpact_get_PUSH_VLAN(a)->ethertype;
#endif
        break;

    case OFPACT_SET_QUEUE:
        ofputil_put_OFPAT11_SET_QUEUE(out)->queue_id
            = htonl(ofpact_get_SET_QUEUE(a)->queue_id);
        break;

    case OFPACT_SET_ETH_SRC:
        memcpy(ofputil_put_OFPAT11_SET_DL_SRC(out)->dl_addr,
               ofpact_get_SET_ETH_SRC(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_ETH_DST:
        memcpy(ofputil_put_OFPAT11_SET_DL_DST(out)->dl_addr,
               ofpact_get_SET_ETH_DST(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_IPV4_SRC:
        ofputil_put_OFPAT11_SET_NW_SRC(out)->nw_addr
            = ofpact_get_SET_IPV4_SRC(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DST:
        ofputil_put_OFPAT11_SET_NW_DST(out)->nw_addr
            = ofpact_get_SET_IPV4_DST(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DSCP:
        ofputil_put_OFPAT11_SET_NW_TOS(out)->nw_tos
            = ofpact_get_SET_IPV4_DSCP(a)->dscp;
        break;

    case OFPACT_SET_L4_SRC_PORT:
        ofputil_put_OFPAT11_SET_TP_SRC(out)->tp_port
            = htons(ofpact_get_SET_L4_SRC_PORT(a)->port);
        break;

    case OFPACT_SET_L4_DST_PORT:
        ofputil_put_OFPAT11_SET_TP_DST(out)->tp_port
            = htons(ofpact_get_SET_L4_DST_PORT(a)->port);
        break;

    case OFPACT_DEC_TTL:
        ofpact_dec_ttl_to_openflow11(ofpact_get_DEC_TTL(a), out);
        break;

    case OFPACT_WRITE_METADATA:
        /* OpenFlow 1.1 uses OFPIT_WRITE_METADATA to express this action. */
        break;

    case OFPACT_PUSH_MPLS:
        ofputil_put_OFPAT11_PUSH_MPLS(out)->ethertype =
            ofpact_get_PUSH_MPLS(a)->ethertype;
        break;

    case OFPACT_POP_MPLS:
        ofputil_put_OFPAT11_POP_MPLS(out)->ethertype =
            ofpact_get_POP_MPLS(a)->ethertype;

        break;

    case OFPACT_CLEAR_ACTIONS:
    case OFPACT_GOTO_TABLE:
        NOT_REACHED();

#ifdef _OFP_CENTEC_
    case OFPACT_METER:
        NOT_REACHED();

    case OFPACT_GROUP:
        ofputil_put_OFPAT11_GROUP(out)->group_id =
            htonl(ofpact_get_GROUP(a)->group_id);
        break;
        
    case OFPACT_SET_MPLS_TTL:
        ofputil_put_OFPAT11_SET_MPLS_TTL(out)->mpls_ttl = 
            ofpact_get_SET_MPLS_TTL(a)->mpls_ttl;
        break;
#endif

    case OFPACT_CONTROLLER:
    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_REG_MOVE:
    case OFPACT_REG_LOAD:
    case OFPACT_SET_TUNNEL:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
#ifdef _OFP_CENTEC_
    case OFPACT_PUSH_L2:
    case OFPACT_POP_L2:
#endif
        ofpact_to_nxast(a, out);
        break;
    }
}

/* Converts the ofpacts in 'ofpacts' (terminated by OFPACT_END) into OpenFlow
 * 1.1 actions in 'openflow', appending the actions to any existing data in
 * 'openflow'. */
size_t
ofpacts_put_openflow11_actions(const struct ofpact ofpacts[],
                               size_t ofpacts_len, struct ofpbuf *openflow)
{
    const struct ofpact *a;
    size_t start_size = openflow->size;

    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        ofpact_to_openflow11(a, openflow);
    }

    return openflow->size - start_size;
}

static void
ofpacts_update_instruction_actions(struct ofpbuf *openflow, size_t ofs)
{
    struct ofp11_instruction_actions *oia;

    /* Update the instruction's length (or, if it's empty, delete it). */
    oia = ofpbuf_at_assert(openflow, ofs, sizeof *oia);
    if (openflow->size > ofs + sizeof *oia) {
        oia->len = htons(openflow->size - ofs);
    } else {
        openflow->size = ofs;
    }
}

void
ofpacts_put_openflow11_instructions(const struct ofpact ofpacts[],
                                    size_t ofpacts_len,
                                    struct ofpbuf *openflow)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        /* XXX Write-Actions */

        if (a->type == OFPACT_CLEAR_ACTIONS) {
            instruction_put_OFPIT11_CLEAR_ACTIONS(openflow);
#ifdef _OFP_CENTEC_
        } else if (a->type == OFPACT_METER) {
            struct ofp13_instruction_meter *oim;

            oim = instruction_put_OFPIT13_METER(openflow);
            oim->meter_id = htonl(ofpact_get_METER(a)->meter_id);
#endif
        } else if (a->type == OFPACT_GOTO_TABLE) {
            struct ofp11_instruction_goto_table *oigt;

            oigt = instruction_put_OFPIT11_GOTO_TABLE(openflow);
            oigt->table_id = ofpact_get_GOTO_TABLE(a)->table_id;
            memset(oigt->pad, 0, sizeof oigt->pad);
        } else if (a->type == OFPACT_WRITE_METADATA) {
            const struct ofpact_metadata *om;
            struct ofp11_instruction_write_metadata *oiwm;

            om = ofpact_get_WRITE_METADATA(a);
            oiwm = instruction_put_OFPIT11_WRITE_METADATA(openflow);
            oiwm->metadata = om->metadata;
            oiwm->metadata_mask = om->mask;
        } else if (!ofpact_is_instruction(a)) {
            /* Apply-actions */
            const size_t ofs = openflow->size;
            const size_t ofpacts_len_left =
                (uint8_t*)ofpact_end(ofpacts, ofpacts_len) - (uint8_t*)a;
            const struct ofpact *action;
            const struct ofpact *processed = a;

            instruction_put_OFPIT11_APPLY_ACTIONS(openflow);
            OFPACT_FOR_EACH(action, a, ofpacts_len_left) {
                if (ofpact_is_instruction(action)) {
                    break;
                }
                ofpact_to_openflow11(action, openflow);
                processed = action;
            }
            ofpacts_update_instruction_actions(openflow, ofs);
            a = processed;
        }
    }
}

/* Returns true if 'action' outputs to 'port', false otherwise. */
static bool
ofpact_outputs_to_port(const struct ofpact *ofpact, uint16_t port)
{
    switch (ofpact->type) {
    case OFPACT_OUTPUT:
        return ofpact_get_OUTPUT(ofpact)->port == port;
    case OFPACT_ENQUEUE:
        return ofpact_get_ENQUEUE(ofpact)->port == port;
    case OFPACT_CONTROLLER:
        return port == OFPP_CONTROLLER;

    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_PUSH_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
    case OFPACT_REG_MOVE:
    case OFPACT_REG_LOAD:
    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_WRITE_METADATA:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
    case OFPACT_PUSH_MPLS:
    case OFPACT_POP_MPLS:
#ifdef _OFP_CENTEC_
    case OFPACT_PUSH_L2:
    case OFPACT_POP_L2:
    case OFPACT_SET_MPLS_TTL:
#endif        
    case OFPACT_CLEAR_ACTIONS:
    case OFPACT_GOTO_TABLE:
#ifdef _OFP_CENTEC_
    case OFPACT_GROUP:
    case OFPACT_METER:
#endif
    default:
        return false;
    }
}

/* Returns true if any action in the 'ofpacts_len' bytes of 'ofpacts' outputs
 * to 'port', false otherwise. */
bool
ofpacts_output_to_port(const struct ofpact *ofpacts, size_t ofpacts_len,
                       uint16_t port)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        if (ofpact_outputs_to_port(a, port)) {
            return true;
        }
    }

    return false;
}

#ifdef _OFP_CENTEC_
/* Returns true if rule has group_id */
bool
ofpacts_output_to_group(const struct ofpact *ofpacts, size_t ofpacts_len,
                        uint32_t group_id)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
        if (OFPACT_GROUP == a->type) {
            if (ofpact_get_GROUP(a)->group_id == group_id) {
                return true;
            }
        }
    }

    return false;
}
#endif

bool
ofpacts_equal(const struct ofpact *a, size_t a_len,
              const struct ofpact *b, size_t b_len)
{
    return a_len == b_len && !memcmp(a, b, a_len);
}

/* Formatting ofpacts. */

static void
print_note(const struct ofpact_note *note, struct ds *string)
{
    size_t i;

    ds_put_cstr(string, "note:");
    for (i = 0; i < note->length; i++) {
        if (i) {
            ds_put_char(string, '.');
        }
        ds_put_format(string, "%02"PRIx8, note->data[i]);
    }
}

#ifndef _OFP_CENTEC_
static void
print_dec_ttl(const struct ofpact_cnt_ids *ids,
              struct ds *s)
{
    size_t i;

    ds_put_cstr(s, "dec_ttl");
    if (ids->ofpact.compat == OFPUTIL_NXAST_DEC_TTL_CNT_IDS) {
        ds_put_cstr(s, "(");
        for (i = 0; i < ids->n_controllers; i++) {
            if (i) {
                ds_put_cstr(s, ",");
            }
            ds_put_format(s, "%"PRIu16, ids->cnt_ids[i]);
        }
        ds_put_cstr(s, ")");
    }
}
#endif

static void
print_fin_timeout(const struct ofpact_fin_timeout *fin_timeout,
                  struct ds *s)
{
    ds_put_cstr(s, "fin_timeout(");
    if (fin_timeout->fin_idle_timeout) {
        ds_put_format(s, "idle_timeout=%"PRIu16",",
                      fin_timeout->fin_idle_timeout);
    }
    if (fin_timeout->fin_hard_timeout) {
        ds_put_format(s, "hard_timeout=%"PRIu16",",
                      fin_timeout->fin_hard_timeout);
    }
    ds_chomp(s, ',');
    ds_put_char(s, ')');
}

#ifdef _OFP_CENTEC_
void
ofpact_format(const struct ofpact *a, struct ds *s)
#else
static void
ofpact_format(const struct ofpact *a, struct ds *s)
#endif
{
    const struct ofpact_enqueue *enqueue;
    const struct ofpact_resubmit *resubmit;
    const struct ofpact_controller *controller;
    const struct ofpact_metadata *metadata;
    const struct ofpact_tunnel *tunnel;
    uint16_t port;

    switch (a->type) {
    case OFPACT_OUTPUT:
        port = ofpact_get_OUTPUT(a)->port;
        if (port < OFPP_MAX) {
            ds_put_format(s, "output:%"PRIu16, port);
        } else {
            ofputil_format_port(port, s);
            if (port == OFPP_CONTROLLER) {
                ds_put_format(s, ":%"PRIu16, ofpact_get_OUTPUT(a)->max_len);
            }
        }
        break;

    case OFPACT_CONTROLLER:
        controller = ofpact_get_CONTROLLER(a);
        if (controller->reason == OFPR_ACTION &&
            controller->controller_id == 0) {
            ds_put_format(s, "CONTROLLER:%"PRIu16,
                          ofpact_get_CONTROLLER(a)->max_len);
        } else {
            enum ofp_packet_in_reason reason = controller->reason;

            ds_put_cstr(s, "controller(");
            if (reason != OFPR_ACTION) {
                ds_put_format(s, "reason=%s,",
                              ofputil_packet_in_reason_to_string(reason));
            }
            if (controller->max_len != UINT16_MAX) {
                ds_put_format(s, "max_len=%"PRIu16",", controller->max_len);
            }
            if (controller->controller_id != 0) {
                ds_put_format(s, "id=%"PRIu16",", controller->controller_id);
            }
            ds_chomp(s, ',');
            ds_put_char(s, ')');
        }
        break;

    case OFPACT_ENQUEUE:
        enqueue = ofpact_get_ENQUEUE(a);
        ds_put_format(s, "enqueue:");
        ofputil_format_port(enqueue->port, s);
        ds_put_format(s, "q%"PRIu32, enqueue->queue);
        break;

    case OFPACT_OUTPUT_REG:
        ds_put_cstr(s, "output:");
        mf_format_subfield(&ofpact_get_OUTPUT_REG(a)->src, s);
        break;

    case OFPACT_BUNDLE:
        bundle_format(ofpact_get_BUNDLE(a), s);
        break;

    case OFPACT_SET_VLAN_VID:
        ds_put_format(s, "mod_vlan_vid:%"PRIu16,
                      ofpact_get_SET_VLAN_VID(a)->vlan_vid);
        break;

    case OFPACT_SET_VLAN_PCP:
        ds_put_format(s, "mod_vlan_pcp:%"PRIu8,
                      ofpact_get_SET_VLAN_PCP(a)->vlan_pcp);
        break;

    case OFPACT_STRIP_VLAN:
        ds_put_cstr(s, "strip_vlan");
        break;

    case OFPACT_PUSH_VLAN:
        /* XXX 802.1AD case*/
#ifndef _OFP_CENTEC_
        ds_put_format(s, "push_vlan:%#"PRIx16, ETH_TYPE_VLAN_8021Q);
#else
        ds_put_format(s, "push_vlan:%#"PRIx16,
                ntohs(ofpact_get_PUSH_VLAN(a)->ethertype));
#endif
        break;

    case OFPACT_SET_ETH_SRC:
        ds_put_format(s, "mod_dl_src:"ETH_ADDR_FMT,
                      ETH_ADDR_ARGS(ofpact_get_SET_ETH_SRC(a)->mac));
        break;

    case OFPACT_SET_ETH_DST:
        ds_put_format(s, "mod_dl_dst:"ETH_ADDR_FMT,
                      ETH_ADDR_ARGS(ofpact_get_SET_ETH_DST(a)->mac));
        break;

    case OFPACT_SET_IPV4_SRC:
        ds_put_format(s, "mod_nw_src:"IP_FMT,
                      IP_ARGS(ofpact_get_SET_IPV4_SRC(a)->ipv4));
        break;

    case OFPACT_SET_IPV4_DST:
        ds_put_format(s, "mod_nw_dst:"IP_FMT,
                      IP_ARGS(ofpact_get_SET_IPV4_DST(a)->ipv4));
        break;

    case OFPACT_SET_IPV4_DSCP:
        ds_put_format(s, "mod_nw_tos:%d", ofpact_get_SET_IPV4_DSCP(a)->dscp);
        break;

    case OFPACT_SET_L4_SRC_PORT:
        ds_put_format(s, "mod_tp_src:%d", ofpact_get_SET_L4_SRC_PORT(a)->port);
        break;

    case OFPACT_SET_L4_DST_PORT:
        ds_put_format(s, "mod_tp_dst:%d", ofpact_get_SET_L4_DST_PORT(a)->port);
        break;

    case OFPACT_REG_MOVE:
        nxm_format_reg_move(ofpact_get_REG_MOVE(a), s);
        break;

    case OFPACT_REG_LOAD:
        nxm_format_reg_load(ofpact_get_REG_LOAD(a), s);
        break;

    case OFPACT_DEC_TTL:
#ifndef _OFP_CENTEC_
        print_dec_ttl(ofpact_get_DEC_TTL(a), s);
#else
        ds_put_cstr(s, "dec_nw_ttl");
#endif
        break;

    case OFPACT_SET_TUNNEL:
        tunnel = ofpact_get_SET_TUNNEL(a);
        ds_put_format(s, "set_tunnel%s:%#"PRIx64,
                      (tunnel->tun_id > UINT32_MAX
                       || a->compat == OFPUTIL_NXAST_SET_TUNNEL64 ? "64" : ""),
                      tunnel->tun_id);
        break;

    case OFPACT_SET_QUEUE:
        ds_put_format(s, "set_queue:%"PRIu32,
                      ofpact_get_SET_QUEUE(a)->queue_id);
        break;

    case OFPACT_POP_QUEUE:
        ds_put_cstr(s, "pop_queue");
        break;

    case OFPACT_FIN_TIMEOUT:
        print_fin_timeout(ofpact_get_FIN_TIMEOUT(a), s);
        break;

    case OFPACT_RESUBMIT:
        resubmit = ofpact_get_RESUBMIT(a);
        if (resubmit->in_port != OFPP_IN_PORT && resubmit->table_id == 255) {
            ds_put_cstr(s, "resubmit:");
            ofputil_format_port(resubmit->in_port, s);
        } else {
            ds_put_format(s, "resubmit(");
            if (resubmit->in_port != OFPP_IN_PORT) {
                ofputil_format_port(resubmit->in_port, s);
            }
            ds_put_char(s, ',');
            if (resubmit->table_id != 255) {
                ds_put_format(s, "%"PRIu8, resubmit->table_id);
            }
            ds_put_char(s, ')');
        }
        break;

    case OFPACT_LEARN:
        learn_format(ofpact_get_LEARN(a), s);
        break;

    case OFPACT_MULTIPATH:
        multipath_format(ofpact_get_MULTIPATH(a), s);
        break;

    case OFPACT_NOTE:
        print_note(ofpact_get_NOTE(a), s);
        break;

    case OFPACT_PUSH_MPLS:
        ds_put_format(s, "push_mpls:0x%04"PRIx16,
                      ntohs(ofpact_get_PUSH_MPLS(a)->ethertype));
        break;

    case OFPACT_POP_MPLS:
        ds_put_format(s, "pop_mpls:0x%04"PRIx16,
                      ntohs(ofpact_get_POP_MPLS(a)->ethertype));
        break;

#ifdef _OFP_CENTEC_
    case OFPACT_PUSH_L2:
        ds_put_cstr(s, "push_l2");
        break;
        
    case OFPACT_POP_L2:
        ds_put_cstr(s, "pop_l2");
        break;
    case OFPACT_SET_MPLS_TTL:
        ds_put_format(s, "set_mpls_ttl:%d", 
                      ofpact_get_SET_MPLS_TTL(a)->mpls_ttl);
        break;
#endif        

    case OFPACT_EXIT:
        ds_put_cstr(s, "exit");
        break;

    case OFPACT_CLEAR_ACTIONS:
        ds_put_format(s, "%s",
                      ofpact_instruction_name_from_type(
                          OVSINST_OFPIT11_CLEAR_ACTIONS));
        break;

    case OFPACT_WRITE_METADATA:
        metadata = ofpact_get_WRITE_METADATA(a);
        ds_put_format(s, "%s:%#"PRIx64,
                      ofpact_instruction_name_from_type(
                          OVSINST_OFPIT11_WRITE_METADATA),
                      ntohll(metadata->metadata));
        if (metadata->mask != htonll(UINT64_MAX)) {
            ds_put_format(s, "/%#"PRIx64, ntohll(metadata->mask));
        }
        break;

    case OFPACT_GOTO_TABLE:
        ds_put_format(s, "%s:%"PRIu8,
                      ofpact_instruction_name_from_type(
                          OVSINST_OFPIT11_GOTO_TABLE),
                      ofpact_get_GOTO_TABLE(a)->table_id);
        break;
        
#ifdef _OFP_CENTEC_
    case OFPACT_GROUP:
        ds_put_format(s, "group:%"PRIu32,
                      ofpact_get_GROUP(a)->group_id);
        break;
    case OFPACT_METER:
        ds_put_format(s, "%s:%"PRIu32,
                      ofpact_instruction_name_from_type(
                                          OVSINST_OFPIT13_METER),
                      ofpact_get_METER(a)->meter_id);
        break;
#endif
    }
}

/* Appends a string representing the 'ofpacts_len' bytes of ofpacts in
 * 'ofpacts' to 'string'. */
void
ofpacts_format(const struct ofpact *ofpacts, size_t ofpacts_len,
               struct ds *string)
{
    ds_put_cstr(string, "actions=");
    if (!ofpacts_len) {
        ds_put_cstr(string, "drop");
    } else {
        const struct ofpact *a;

        OFPACT_FOR_EACH (a, ofpacts, ofpacts_len) {
            if (a != ofpacts) {
                ds_put_cstr(string, ",");
            }

            /* XXX write-actions */
            ofpact_format(a, string);
        }
    }
}

/* Internal use by helpers. */

void *
ofpact_put(struct ofpbuf *ofpacts, enum ofpact_type type, size_t len)
{
    struct ofpact *ofpact;

    ofpact_pad(ofpacts);
    ofpact = ofpacts->l2 = ofpbuf_put_uninit(ofpacts, len);
    ofpact_init(ofpact, type, len);
    return ofpact;
}

void
ofpact_init(struct ofpact *ofpact, enum ofpact_type type, size_t len)
{
    memset(ofpact, 0, len);
    ofpact->type = type;
    ofpact->compat = OFPUTIL_ACTION_INVALID;
    ofpact->len = len;
}

/* Updates 'ofpact->len' to the number of bytes in the tail of 'ofpacts'
 * starting at 'ofpact'.
 *
 * This is the correct way to update a variable-length ofpact's length after
 * adding the variable-length part of the payload.  (See the large comment
 * near the end of ofp-actions.h for more information.) */
void
ofpact_update_len(struct ofpbuf *ofpacts, struct ofpact *ofpact)
{
    ovs_assert(ofpact == ofpacts->l2);
    ofpact->len = (char *) ofpbuf_tail(ofpacts) - (char *) ofpact;
}

/* Pads out 'ofpacts' to a multiple of OFPACT_ALIGNTO bytes in length.  Each
 * ofpact_put_<ENUM>() calls this function automatically beforehand, but the
 * client must call this itself after adding the final ofpact to an array of
 * them.
 *
 * (The consequences of failing to call this function are probably not dire.
 * OFPACT_FOR_EACH will calculate a pointer beyond the end of the ofpacts, but
 * not dereference it.  That's undefined behavior, technically, but it will not
 * cause a real problem on common systems.  Still, it seems better to call
 * it.) */
void
ofpact_pad(struct ofpbuf *ofpacts)
{
    unsigned int rem = ofpacts->size % OFPACT_ALIGNTO;
    if (rem) {
        ofpbuf_put_zeros(ofpacts, OFPACT_ALIGNTO - rem);
    }
}

void
ofpact_set_field_init(struct ofpact_reg_load *load, const struct mf_field *mf,
                      const void *src)
{
    load->ofpact.compat = OFPUTIL_OFPAT12_SET_FIELD;
    load->dst.field = mf;
    load->dst.ofs = 0;
    load->dst.n_bits = mf->n_bits;
    bitwise_copy(src, mf->n_bytes, load->dst.ofs,
                 &load->subvalue, sizeof load->subvalue, 0, mf->n_bits);
}
