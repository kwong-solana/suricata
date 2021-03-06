/* Copyright (C) 2007-2013 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * Implements the pcre keyword
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "pkt-var.h"
#include "flow-var.h"
#include "flow-util.h"

#include "detect-pcre.h"
#include "detect-flowvar.h"

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-sigorder.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"

#include "util-var-name.h"
#include "util-unittest-helper.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-print.h"
#include "util-pool.h"

#include "conf.h"
#include "app-layer.h"
#include "app-layer-htp.h"
#include "stream.h"
#include "stream-tcp.h"
#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "app-layer-protos.h"
#include "app-layer-parser.h"

#define PARSE_CAPTURE_REGEX "\\(\\?P\\<([A-z]+)\\_([A-z0-9_]+)\\>"
#define PARSE_REGEX         "(?<!\\\\)/(.*(?<!(?<!\\\\)\\\\))/([^\"]*)"

#define SC_MATCH_LIMIT_DEFAULT 3500
#define SC_MATCH_LIMIT_RECURSION_DEFAULT 1500

static int pcre_match_limit = 0;
static int pcre_match_limit_recursion = 0;

static pcre *parse_regex;
static pcre_extra *parse_regex_study;
static pcre *parse_capture_regex;
static pcre_extra *parse_capture_regex_study;

int DetectPcreMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectPcreALMatchCookie(ThreadVars *t, DetectEngineThreadCtx *det_ctx, Flow *f, uint8_t flags, void *state, Signature *s, SigMatch *m);
int DetectPcreALMatchMethod(ThreadVars *t, DetectEngineThreadCtx *det_ctx, Flow *f, uint8_t flags, void *state, Signature *s, SigMatch *m);
static int DetectPcreSetup (DetectEngineCtx *, Signature *, char *);
void DetectPcreFree(void *);
void DetectPcreRegisterTests(void);

void DetectPcreRegister (void)
{
    sigmatch_table[DETECT_PCRE].name = "pcre";
    sigmatch_table[DETECT_PCRE].desc = "match on regular expression";
    sigmatch_table[DETECT_PCRE].url = "https://redmine.openinfosecfoundation.org/projects/suricata/wiki/HTTP-keywords#Pcre-Perl-Compatible-Regular-Expressions";
    sigmatch_table[DETECT_PCRE].Match = NULL;
    sigmatch_table[DETECT_PCRE].AppLayerMatch = NULL;
    sigmatch_table[DETECT_PCRE].Setup = DetectPcreSetup;
    sigmatch_table[DETECT_PCRE].Free  = DetectPcreFree;
    sigmatch_table[DETECT_PCRE].RegisterTests  = DetectPcreRegisterTests;

    sigmatch_table[DETECT_PCRE].flags |= SIGMATCH_PAYLOAD;

    intmax_t val = 0;

    if (!ConfGetInt("pcre.match-limit", &val)) {
        pcre_match_limit = SC_MATCH_LIMIT_DEFAULT;
        SCLogDebug("Using PCRE match-limit setting of: %i", pcre_match_limit);
    }
    else    {
        pcre_match_limit = val;
        if (pcre_match_limit != SC_MATCH_LIMIT_DEFAULT) {
            SCLogInfo("Using PCRE match-limit setting of: %i", pcre_match_limit);
        } else {
            SCLogDebug("Using PCRE match-limit setting of: %i", pcre_match_limit);
        }
    }

    val = 0;

    if (!ConfGetInt("pcre.match-limit-recursion", &val)) {
        pcre_match_limit_recursion = SC_MATCH_LIMIT_RECURSION_DEFAULT;
        SCLogDebug("Using PCRE match-limit-recursion setting of: %i", pcre_match_limit_recursion);
    }
    else    {
        pcre_match_limit_recursion = val;
        if (pcre_match_limit_recursion != SC_MATCH_LIMIT_RECURSION_DEFAULT) {
            SCLogInfo("Using PCRE match-limit-recursion setting of: %i", pcre_match_limit_recursion);
        } else {
            SCLogDebug("Using PCRE match-limit-recursion setting of: %i", pcre_match_limit_recursion);
        }
    }

    DetectSetupParseRegexes(PARSE_REGEX, &parse_regex, &parse_regex_study);

    /* setup the capture regex, as it needs PCRE_UNGREEDY we do it manually */
    const char *eb;
    int eo;
    int opts = PCRE_UNGREEDY; /* pkt_http_ua should be pkt, http_ua, for this reason the UNGREEDY */

    parse_capture_regex = pcre_compile(PARSE_CAPTURE_REGEX, opts, &eb, &eo, NULL);
    if (parse_capture_regex == NULL)
    {
        FatalError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_CAPTURE_REGEX, eo, eb);
    }

    parse_capture_regex_study = pcre_study(parse_capture_regex, 0, &eb);
    if(eb != NULL)
    {
        FatalError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
    }

    DetectParseRegexAddToFreeList(parse_capture_regex, parse_capture_regex_study);
    return;
}

/**
 * \brief Match a regex on a single payload.
 *
 * \param det_ctx     Thread detection ctx.
 * \param s           Signature.
 * \param sm          Sig match to match against.
 * \param p           Packet to set PktVars if any.
 * \param f           Flow to set FlowVars if any.
 * \param payload     Payload to inspect.
 * \param payload_len Length of the payload.
 *
 * \retval  1 Match.
 * \retval  0 No match.
 */
int DetectPcrePayloadMatch(DetectEngineThreadCtx *det_ctx, Signature *s,
                           SigMatch *sm, Packet *p, Flow *f, uint8_t *payload,
                           uint32_t payload_len)
{
    SCEnter();
#define MAX_SUBSTRINGS 30
    int ret = 0;
    int ov[MAX_SUBSTRINGS];
    uint8_t *ptr = NULL;
    uint16_t len = 0;
    uint16_t capture_len = 0;

    DetectPcreData *pe = (DetectPcreData *)sm->ctx;

    if (pe->flags & DETECT_PCRE_RELATIVE) {
        ptr = payload + det_ctx->buffer_offset;
        len = payload_len - det_ctx->buffer_offset;
    } else {
        ptr = payload;
        len = payload_len;
    }

    int start_offset = 0;
    if (det_ctx->pcre_match_start_offset != 0) {
        start_offset = (payload + det_ctx->pcre_match_start_offset - ptr);
    }

    /* run the actual pcre detection */
    ret = pcre_exec(pe->re, pe->sd, (char *)ptr, len, start_offset, 0, ov, MAX_SUBSTRINGS);
    SCLogDebug("ret %d (negating %s)", ret, (pe->flags & DETECT_PCRE_NEGATE) ? "set" : "not set");

    if (ret == PCRE_ERROR_NOMATCH) {
        if (pe->flags & DETECT_PCRE_NEGATE) {
            /* regex didn't match with negate option means we
             * consider it a match */
            ret = 1;
        } else {
            ret = 0;
        }
    } else if (ret >= 0) {
        if (pe->flags & DETECT_PCRE_NEGATE) {
            /* regex matched but we're negated, so not
             * considering it a match */
            ret = 0;
        } else {
            /* regex matched and we're not negated,
             * considering it a match */

            SCLogDebug("ret %d capidx %u", ret, pe->capidx);

            /* see if we need to do substring capturing. */
            if (ret > 1 && pe->capidx != 0) {
                SCLogDebug("capturing");
                const char *str_ptr;
                ret = pcre_get_substring((char *)ptr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
                if (ret) {
                    if (pe->flags & DETECT_PCRE_CAPTURE_PKT) {
                        if (p != NULL) {
                            PktVarAdd(p, pe->capname, (uint8_t *)str_ptr, ret);
                        }
                    } else if (pe->flags & DETECT_PCRE_CAPTURE_FLOW) {
                        if (f != NULL) {
                            /* store max 64k. Errors are ignored */
                            capture_len = (ret < 0xffff) ? (uint16_t)ret : 0xffff;
                            (void)DetectFlowvarStoreMatch(det_ctx, pe->capidx,
                                    (uint8_t *)str_ptr, capture_len,
                                    DETECT_FLOWVAR_TYPE_POSTMATCH);
                        }
                    }
                }
            }

            /* update offset for pcre RELATIVE */
            det_ctx->buffer_offset = (ptr + ov[1]) - payload;
            det_ctx->pcre_match_start_offset = (ptr + ov[0] + 1) - payload;

            ret = 1;
        }

    } else {
        SCLogDebug("pcre had matching error");
        ret = 0;
    }
    SCReturnInt(ret);
}

static int DetectPcreSetList(int list, int set)
{
    if (list != DETECT_SM_LIST_NOTSET) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "only one pcre option to specify a buffer type is allowed");
        return -1;
    }
    return set;
}

static int DetectPcreHasUpperCase(const char *re)
{
    size_t len = strlen(re);
    int is_meta = 0;

    for (size_t i = 0; i < len; i++) {
        if (is_meta) {
            is_meta = 0;
        }
        else if (re[i] == '\\') {
            is_meta = 1;
        }
        else if (isupper((unsigned char)re[i])) {
            return 1;
        }
    }

    return 0;
}

static DetectPcreData *DetectPcreParse (DetectEngineCtx *de_ctx, char *regexstr, int *sm_list)
{
    int ec;
    const char *eb;
    int eo;
    int opts = 0;
    DetectPcreData *pd = NULL;
    char *op = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    /* take the size of the whole input as buffer size for the regex we will
     * extract below. Add 1 to please Coverity's alloc_strlen test. */
    size_t slen = strlen(regexstr) + 1;
    char re[slen];

    char op_str[64] = "";
    uint16_t pos = 0;
    uint8_t negate = 0;

    while (pos < slen && isspace((unsigned char)regexstr[pos])) {
        pos++;
    }

    if (regexstr[pos] == '!') {
        negate = 1;
        pos++;
    }

    ret = pcre_exec(parse_regex, parse_regex_study, regexstr + pos, slen-pos,
                    0, 0, ov, MAX_SUBSTRINGS);
    if (ret <= 0) {
        SCLogError(SC_ERR_PCRE_MATCH, "pcre parse error: %s", regexstr);
        goto error;
    }

    res = pcre_copy_substring((char *)regexstr + pos, ov, MAX_SUBSTRINGS,
            1, re, slen);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_copy_substring failed");
        return NULL;
    }

    if (ret > 2) {
        res = pcre_copy_substring((char *)regexstr + pos, ov, MAX_SUBSTRINGS,
                2, op_str, sizeof(op_str));
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_copy_substring failed");
            return NULL;
        }
        op = op_str;
    }
    //printf("ret %" PRId32 " re \'%s\', op \'%s\'\n", ret, re, op);

    pd = SCMalloc(sizeof(DetectPcreData));
    if (unlikely(pd == NULL))
        goto error;
    memset(pd, 0, sizeof(DetectPcreData));

    if (negate)
        pd->flags |= DETECT_PCRE_NEGATE;

    if (op != NULL) {
        while (*op) {
            SCLogDebug("regex option %c", *op);

            switch (*op) {
                case 'A':
                    opts |= PCRE_ANCHORED;
                    break;
                case 'E':
                    opts |= PCRE_DOLLAR_ENDONLY;
                    break;
                case 'G':
                    opts |= PCRE_UNGREEDY;
                    break;

                case 'i':
                    opts |= PCRE_CASELESS;
                    pd->flags |= DETECT_PCRE_CASELESS;
                    break;
                case 'm':
                    opts |= PCRE_MULTILINE;
                    break;
                case 's':
                    opts |= PCRE_DOTALL;
                    break;
                case 'x':
                    opts |= PCRE_EXTENDED;
                    break;

                case 'O':
                    pd->flags |= DETECT_PCRE_MATCH_LIMIT;
                    break;

                case 'B': /* snort's option */
                    if (*sm_list != DETECT_SM_LIST_NOTSET) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'B' inconsistent with chosen buffer");
                        goto error;
                    }
                    pd->flags |= DETECT_PCRE_RAWBYTES;
                    break;
                case 'R': /* snort's option */
                    pd->flags |= DETECT_PCRE_RELATIVE;
                    break;

                /* buffer selection */

                case 'U': /* snort's option */
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'U' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_UMATCH);
                    break;
                case 'V':
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'V' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HUADMATCH);
                    break;
                case 'W':
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'W' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HHHDMATCH);
                    break;
                case 'Z':
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'Z' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HRHHDMATCH);
                    break;
                case 'H': /* snort's option */
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'H' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HHDMATCH);
                    break;
                case 'I': /* snort's option */
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'I' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HRUDMATCH);
                    break;
                case 'D': /* snort's option */
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HRHDMATCH);
                    break;
                case 'M': /* snort's option */
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'M' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HMDMATCH);
                    break;
                case 'C': /* snort's option */
                    if (pd->flags & DETECT_PCRE_RAWBYTES) {
                        SCLogError(SC_ERR_INVALID_SIGNATURE, "regex modifier 'C' inconsistent with 'B'");
                        goto error;
                    }
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HCDMATCH);
                    break;
                case 'P':
                    /* snort's option (http request body inspection) */
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HCBDMATCH);
                    break;
                case 'Q':
                    /* suricata extension (http response body inspection) */
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_FILEDATA);
                    break;
                case 'Y':
                    /* snort's option */
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HSMDMATCH);
                    break;
                case 'S':
                    /* snort's option */
                    *sm_list = DetectPcreSetList(*sm_list, DETECT_SM_LIST_HSCDMATCH);
                    break;
                default:
                    SCLogError(SC_ERR_UNKNOWN_REGEX_MOD, "unknown regex modifier '%c'", *op);
                    goto error;
            }
            op++;
        }
    }
    if (*sm_list == -1)
        goto error;

    SCLogDebug("DetectPcreParse: \"%s\"", re);

    /* host header */
    if (*sm_list == DETECT_SM_LIST_HHHDMATCH) {
        if (pd->flags & DETECT_PCRE_CASELESS) {
            SCLogWarning(SC_ERR_INVALID_SIGNATURE, "http host pcre(\"W\") "
                         "specified along with \"i(caseless)\" modifier.  "
                         "Since the hostname buffer we match against "
                         "is actually lowercase, having a "
                         "nocase is redundant.");
        }
        else if (DetectPcreHasUpperCase(re)) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "pcre host(\"W\") "
                "specified has an uppercase char.  "
                "Since the hostname buffer we match against "
                "is actually lowercase, please specify an "
                "all lowercase based pcre.");
            goto error;
        }
    }

    /* Try to compile as if all (...) groups had been meant as (?:...),
     * which is the common case in most rules.
     * If we fail because a capture group is later referenced (e.g., \1),
     * PCRE will let us know.
     */
    pd->re = pcre_compile2(re, opts | PCRE_NO_AUTO_CAPTURE, &ec, &eb, &eo, NULL);
    if (pd->re == NULL && ec == 15) { // reference to non-existent subpattern
        pd->re = pcre_compile(re, opts, &eb, &eo, NULL);
    }

    if(pd->re == NULL)  {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", regexstr, eo, eb);
        goto error;
    }
#ifdef PCRE_HAVE_JIT
    pd->sd = pcre_study(pd->re, PCRE_STUDY_JIT_COMPILE, &eb);
    if(eb != NULL)  {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed : %s", eb);
        goto error;
    }

    int jit = 0;
    ret = pcre_fullinfo(pd->re, pd->sd, PCRE_INFO_JIT, &jit);
    if (ret != 0 || jit != 1) {
        /* warning, so we won't print the sig after this. Adding
         * file and line to the message so the admin can figure
         * out what sig this is about */
        SCLogDebug("PCRE JIT compiler does not support: %s. "
                "Falling back to regular PCRE handling (%s:%d)",
                regexstr, de_ctx->rule_file, de_ctx->rule_line);
    }
#else
    pd->sd = pcre_study(pd->re, 0, &eb);
    if(eb != NULL)  {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed : %s", eb);
        goto error;
    }
#endif /*PCRE_HAVE_JIT*/

    if (pd->sd == NULL)
        pd->sd = (pcre_extra *) SCCalloc(1,sizeof(pcre_extra));

    if (pd->sd) {
        if(pd->flags & DETECT_PCRE_MATCH_LIMIT) {
            if(pcre_match_limit >= -1)    {
                pd->sd->match_limit = pcre_match_limit;
                pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT;
            }
#ifndef NO_PCRE_MATCH_RLIMIT
            if(pcre_match_limit_recursion >= -1)    {
                pd->sd->match_limit_recursion = pcre_match_limit_recursion;
                pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT_RECURSION;
            }
#endif /* NO_PCRE_MATCH_RLIMIT */
        } else {
            pd->sd->match_limit = SC_MATCH_LIMIT_DEFAULT;
            pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT;
#ifndef NO_PCRE_MATCH_RLIMIT
            pd->sd->match_limit_recursion = SC_MATCH_LIMIT_RECURSION_DEFAULT;
            pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT_RECURSION;
#endif /* NO_PCRE_MATCH_RLIMIT */
        }
    } else {
        goto error;
    }

    return pd;

error:
    if (pd != NULL && pd->re != NULL)
        pcre_free(pd->re);
    if (pd != NULL && pd->sd != NULL)
        pcre_free_study(pd->sd);
    if (pd)
        SCFree(pd);
    return NULL;
}

/** \internal
 *  \brief check if we need to extract capture settings and set them up if needed
 */
static int DetectPcreParseCapture(char *regexstr, DetectEngineCtx *de_ctx, DetectPcreData *pd)
{
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];
    char type_str[16] = "";

    /* take the size of the whole input as buffer size for the string we will
     * extract below. Add 1 to please Coverity's alloc_strlen test. */
    size_t cap_buffer_len = strlen(regexstr) + 1;
    char capture_str[cap_buffer_len];
    memset(capture_str, 0x00, cap_buffer_len);

    if (de_ctx == NULL)
        goto error;

    SCLogDebug("\'%s\'", regexstr);

    ret = pcre_exec(parse_capture_regex, parse_capture_regex_study, regexstr, strlen(regexstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 3) {
        return 0;
    }

    res = pcre_copy_substring((char *)regexstr, ov, MAX_SUBSTRINGS, 1, type_str, sizeof(type_str));
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_copy_substring failed");
        goto error;
    }
    res = pcre_copy_substring((char *)regexstr, ov, MAX_SUBSTRINGS, 2, capture_str, cap_buffer_len);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_copy_substring failed");
        goto error;
    }
    if (strlen(capture_str) == 0 || strlen(type_str) == 0) {
        goto error;
    }

    SCLogDebug("type \'%s\'", type_str);
    SCLogDebug("capture \'%s\'", capture_str);

    pd->capname = SCStrdup(capture_str);
    if (unlikely(pd->capname == NULL))
        goto error;

    if (strcmp(type_str, "pkt") == 0) {
        pd->flags |= DETECT_PCRE_CAPTURE_PKT;
    } else if (strcmp(type_str, "flow") == 0) {
        pd->flags |= DETECT_PCRE_CAPTURE_FLOW;
        SCLogDebug("flow capture");
    }
    if (pd->capname != NULL) {
        if (pd->flags & DETECT_PCRE_CAPTURE_PKT)
            pd->capidx = VariableNameGetIdx(de_ctx, (char *)pd->capname, VAR_TYPE_PKT_VAR);
        else if (pd->flags & DETECT_PCRE_CAPTURE_FLOW)
            pd->capidx = VariableNameGetIdx(de_ctx, (char *)pd->capname, VAR_TYPE_FLOW_VAR);
    }

    SCLogDebug("pd->capname %s", pd->capname);
    return 0;

error:
    if (pd->capname != NULL) {
        SCFree(pd->capname);
        pd->capname = NULL;
    }
    return -1;
}

static int DetectPcreSetup (DetectEngineCtx *de_ctx, Signature *s, char *regexstr)
{
    SCEnter();
    DetectPcreData *pd = NULL;
    SigMatch *sm = NULL;
    int ret = -1;
    int parsed_sm_list = DETECT_SM_LIST_NOTSET;

    pd = DetectPcreParse(de_ctx, regexstr, &parsed_sm_list);
    if (pd == NULL)
        goto error;
    if (DetectPcreParseCapture(regexstr, de_ctx, pd) < 0)
        goto error;

    if (parsed_sm_list == DETECT_SM_LIST_UMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HRUDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HCBDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_FILEDATA ||
        parsed_sm_list == DETECT_SM_LIST_HHDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HRHDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HSMDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HSCDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HHHDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HRHHDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HMDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HCDMATCH ||
        parsed_sm_list == DETECT_SM_LIST_HUADMATCH)
    {
        if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_HTTP) {
            SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS, "Invalid option.  "
                       "Conflicting alprotos detected for this rule.  Http "
                       "pcre modifier found along with a different protocol "
                       "for the rule.");
            goto error;
        }
        if (s->list != DETECT_SM_LIST_NOTSET) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "pcre found with http "
                       "modifier set, with file_data/dce_stub_data sticky "
                       "option set.");
            goto error;
        }
    }

    int sm_list = -1;
    if (s->list != DETECT_SM_LIST_NOTSET) {
        if (s->list == DETECT_SM_LIST_FILEDATA) {
            SCLogDebug("adding to http server body list because of file data");
            AppLayerHtpEnableResponseBodyCallback();
        } else if (s->list == DETECT_SM_LIST_DMATCH) {
            SCLogDebug("adding to dmatch list because of dce_stub_data");
        } else if (s->list == DETECT_SM_LIST_DNSQUERYNAME_MATCH) {
            SCLogDebug("adding to DETECT_SM_LIST_DNSQUERYNAME_MATCH list because of dns_query");
        }
        s->flags |= SIG_FLAG_APPLAYER;
        sm_list = s->list;
    } else {
        switch(parsed_sm_list) {
            case DETECT_SM_LIST_HCBDMATCH:
                AppLayerHtpEnableRequestBodyCallback();
                s->flags |= SIG_FLAG_APPLAYER;
                s->alproto = ALPROTO_HTTP;
                sm_list = parsed_sm_list;
                break;

            case DETECT_SM_LIST_FILEDATA:
                AppLayerHtpEnableResponseBodyCallback();
                s->flags |= SIG_FLAG_APPLAYER;
                s->alproto = ALPROTO_HTTP;
                sm_list = parsed_sm_list;
                break;

            case DETECT_SM_LIST_UMATCH:
            case DETECT_SM_LIST_HRUDMATCH:
            case DETECT_SM_LIST_HHDMATCH:
            case DETECT_SM_LIST_HRHDMATCH:
            case DETECT_SM_LIST_HHHDMATCH:
            case DETECT_SM_LIST_HRHHDMATCH:
            case DETECT_SM_LIST_HSMDMATCH:
            case DETECT_SM_LIST_HSCDMATCH:
            case DETECT_SM_LIST_HCDMATCH:
            case DETECT_SM_LIST_HMDMATCH:
            case DETECT_SM_LIST_HUADMATCH:
                s->flags |= SIG_FLAG_APPLAYER;
                s->alproto = ALPROTO_HTTP;
                sm_list = parsed_sm_list;
                break;
            case DETECT_SM_LIST_NOTSET:
                sm_list = DETECT_SM_LIST_PMATCH;
                break;
        }
    }
    if (sm_list == -1)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;
    sm->type = DETECT_PCRE;
    sm->ctx = (void *)pd;
    SigMatchAppendSMToList(s, sm, sm_list);

    if (pd->capidx != 0) {
        if (DetectFlowvarPostMatchSetup(s, pd->capidx) < 0)
            goto error_nofree;
    }

    if (!(pd->flags & DETECT_PCRE_RELATIVE))
        goto okay;

    /* errors below shouldn't free pd */

    SigMatch *prev_pm = SigMatchGetLastSMFromLists(s, 4,
                                                   DETECT_CONTENT, sm->prev,
                                                   DETECT_PCRE, sm->prev);
    if (s->list == DETECT_SM_LIST_NOTSET && prev_pm == NULL) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "pcre with /R (relative) needs "
                "preceeding match in the same buffer");
        goto error_nofree;
    /* null is allowed when we use a sticky buffer */
    } else if (prev_pm == NULL)
        goto okay;
    if (prev_pm->type == DETECT_CONTENT) {
        DetectContentData *cd = (DetectContentData *)prev_pm->ctx;
        cd->flags |= DETECT_CONTENT_RELATIVE_NEXT;
    } else if (prev_pm->type == DETECT_PCRE) {
        DetectPcreData *tmp = (DetectPcreData *)prev_pm->ctx;
        tmp->flags |= DETECT_PCRE_RELATIVE_NEXT;
    }

 okay:
    ret = 0;
    SCReturnInt(ret);
 error:
    DetectPcreFree(pd);
 error_nofree:
    SCReturnInt(ret);
}

void DetectPcreFree(void *ptr)
{
    if (ptr == NULL)
        return;

    DetectPcreData *pd = (DetectPcreData *)ptr;

    if (pd->capname != NULL)
        SCFree(pd->capname);
    if (pd->re != NULL)
        pcre_free(pd->re);
    if (pd->sd != NULL)
        pcre_free_study(pd->sd);

    SCFree(pd);
    return;
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test DetectPcreParseTest01 make sure we don't allow invalid opts 7.
 */
static int DetectPcreParseTest01 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/7";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NOT_NULL(pd);

    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest02 make sure we don't allow invalid opts Ui$.
 */
static int DetectPcreParseTest02 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/Ui$";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NOT_NULL(pd);

    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest03 make sure we don't allow invalid opts UZi.
 */
static int DetectPcreParseTest03 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/UNi";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NOT_NULL(pd);

    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest04 make sure we allow escaped "
 */
static int DetectPcreParseTest04 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b\\\"lah/i";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NULL(pd);

    DetectPcreFree(pd);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest05 make sure we parse pcre with no opts
 */
static int DetectPcreParseTest05 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b(l|a)h/";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NULL(pd);

    DetectPcreFree(pd);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest06 make sure we parse pcre with smi opts
 */
static int DetectPcreParseTest06 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b(l|a)h/smi";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NULL(pd);

    DetectPcreFree(pd);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest07 make sure we parse pcre with /Ui opts
 */
static int DetectPcreParseTest07 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/Ui";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NULL(pd);

    DetectPcreFree(pd);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest08 make sure we parse pcre with O opts
 */
static int DetectPcreParseTest08 (void)
{
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b(l|a)h/O";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NULL(pd);

    DetectPcreFree(pd);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test DetectPcreParseTest09 make sure we parse pcre with a content
 *       that has slashes
 */
static int DetectPcreParseTest09 (void)
{
    DetectPcreData *pd = NULL;
    char *teststring = "/lala\\\\/";
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    pd = DetectPcreParse(de_ctx, teststring, &list);
    FAIL_IF_NULL(pd);

    DetectPcreFree(pd);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

/**
 * \test Test pcre option for dce sig(yeah I'm bored of writing test titles).
 */
int DetectPcreParseTest10(void)
{
    Signature *s = SigAlloc();
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    s->alproto = ALPROTO_DCERPC;

    FAIL_IF_NOT(DetectPcreSetup(de_ctx, s, "/bamboo/") == 0);
    FAIL_IF_NOT(s->sm_lists[DETECT_SM_LIST_DMATCH] == NULL && s->sm_lists[DETECT_SM_LIST_PMATCH] != NULL);

    SigFree(s);

    s = SigAlloc();
    FAIL_IF_NULL(s);

    /* failure since we have no preceding content/pcre/bytejump */
    FAIL_IF_NOT(DetectPcreSetup(de_ctx, s, "/bamboo/") == 0);
    FAIL_IF_NOT(s->sm_lists[DETECT_SM_LIST_DMATCH] == NULL && s->sm_lists[DETECT_SM_LIST_PMATCH] != NULL);

    SigFree(s);
    DetectEngineCtxFree(de_ctx);

    PASS;
}

/**
 * \test Test pcre option for dce sig.
 */
int DetectPcreParseTest11(void)
{
    DetectEngineCtx *de_ctx = NULL;
    Signature *s = NULL;
    DetectPcreData *data = NULL;

    de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(msg:\"Testing bytejump_body\"; "
                               "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                               "dce_stub_data; "
                               "pcre:/bamboo/R; sid:1;)");
    FAIL_IF(de_ctx == NULL);
    s = de_ctx->sig_list;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_DMATCH] == NULL);
    FAIL_IF_NOT(s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->type == DETECT_PCRE);
    data = (DetectPcreData *)s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->ctx;
    FAIL_IF(data->flags & DETECT_PCRE_RAWBYTES ||
        !(data->flags & DETECT_PCRE_RELATIVE));

    s->next = SigInit(de_ctx, "alert tcp any any -> any any "
                      "(msg:\"Testing bytejump_body\"; "
                      "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                      "dce_stub_data; "
                      "pcre:/bamboo/R; sid:1;)");
    FAIL_IF_NULL(s->next);
    s = s->next;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_DMATCH] == NULL);
    FAIL_IF_NOT(s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->type == DETECT_PCRE);
    data = (DetectPcreData *)s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->ctx;
    FAIL_IF(data->flags & DETECT_PCRE_RAWBYTES ||
        !(data->flags & DETECT_PCRE_RELATIVE));

    s->next = SigInit(de_ctx, "alert tcp any any -> any any "
                      "(msg:\"Testing bytejump_body\"; "
                      "dce_iface:3919286a-b10c-11d0-9ba8-00c04fd92ef5; "
                      "dce_stub_data; "
                      "pcre:/bamboo/RB; sid:1;)");
    FAIL_IF(s->next == NULL);
    s = s->next;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_DMATCH] == NULL);
    FAIL_IF_NOT(s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->type == DETECT_PCRE);
    data = (DetectPcreData *)s->sm_lists_tail[DETECT_SM_LIST_DMATCH]->ctx;
    FAIL_IF(!(data->flags & DETECT_PCRE_RAWBYTES) ||
        !(data->flags & DETECT_PCRE_RELATIVE));

    s->next = SigInit(de_ctx, "alert tcp any any -> any any "
                      "(msg:\"Testing bytejump_body\"; "
                      "content:\"one\"; pcre:/bamboo/; sid:1;)");
    FAIL_IF(s->next == NULL);
    s = s->next;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_DMATCH] != NULL);

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    PASS;
}

/**
 * \test Test pcre option with file data. pcre is relative to file_data,
 *       so relative flag should be unset.
 */
static int DetectPcreParseTest12(void)
{
    DetectEngineCtx *de_ctx = NULL;
    Signature *s = NULL;
    DetectPcreData *data = NULL;

    de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(file_data; pcre:/abc/R; sid:1;)");
    FAIL_IF (de_ctx->sig_list == NULL);

    s = de_ctx->sig_list;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_FILEDATA] == NULL);

    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->type != DETECT_PCRE);

    data = (DetectPcreData *)s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->ctx;
    FAIL_IF(data->flags & DETECT_PCRE_RAWBYTES ||
        !(data->flags & DETECT_PCRE_RELATIVE));

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    PASS;
}

/**
 * \test Test pcre option with file data.
 */
static int DetectPcreParseTest13(void)
{
    DetectEngineCtx *de_ctx = NULL;
    Signature *s = NULL;
    DetectPcreData *data = NULL;

    de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(file_data; content:\"abc\"; pcre:/def/R; sid:1;)");
    FAIL_IF(de_ctx->sig_list == NULL);

    s = de_ctx->sig_list;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_FILEDATA] == NULL);

    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->type != DETECT_PCRE);

    data = (DetectPcreData *)s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->ctx;
    FAIL_IF(data->flags & DETECT_PCRE_RAWBYTES ||
        !(data->flags & DETECT_PCRE_RELATIVE));

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    PASS;
}

/**
 * \test Test pcre option with file data.
 */
static int DetectPcreParseTest14(void)
{
    DetectEngineCtx *de_ctx = NULL;
    Signature *s = NULL;
    DetectPcreData *data = NULL;

    de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
                               "(file_data; pcre:/def/; sid:1;)");
    FAIL_IF(de_ctx->sig_list == NULL);

    s = de_ctx->sig_list;
    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_FILEDATA] == NULL);

    FAIL_IF(s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->type != DETECT_PCRE);

    data = (DetectPcreData *)s->sm_lists_tail[DETECT_SM_LIST_FILEDATA]->ctx;
    FAIL_IF(data->flags & DETECT_PCRE_RAWBYTES ||
        data->flags & DETECT_PCRE_RELATIVE);

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    PASS;
}

/** \test Check a signature with pcre relative method */
int DetectPcreParseTest15(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relative http_method\"; "
                               "content:\"GET\"; "
                               "http_method; pcre:\"/abc/RM\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}


/** \test Check a signature with pcre relative cookie */
int DetectPcreParseTest16(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relative http_cookie\"; "
                               "content:\"test\"; "
                               "http_cookie; pcre:\"/abc/RC\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with pcre relative raw header */
int DetectPcreParseTest17(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relative http_raw_header\"; "
                               "flow:to_server; content:\"test\"; "
                               "http_raw_header; pcre:\"/abc/RD\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with pcre relative header */
int DetectPcreParseTest18(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relative http_header\"; "
                               "content:\"test\"; "
                               "http_header; pcre:\"/abc/RH\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with pcre relative client-body */
int DetectPcreParseTest19(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relativie http_client_body\"; "
                               "content:\"test\"; "
                               "http_client_body; pcre:\"/abc/RP\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with pcre relative raw uri */
int DetectPcreParseTest20(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing http_raw_uri\"; "
                               "content:\"test\"; "
                               "http_raw_uri; pcre:\"/abc/RI\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with pcre relative uricontent */
int DetectPcreParseTest21(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relative uricontent\"; "
                               "uricontent:\"test\"; "
                               "pcre:\"/abc/RU\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with pcre relative http_uri */
int DetectPcreParseTest22(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing pcre relative http_uri\"; "
                               "content:\"test\"; "
                               "http_uri; pcre:\"/abc/RU\"; sid:1;)");
    FAIL_IF_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with inconsistent pcre relative  */
int DetectPcreParseTest23(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing inconsistent pcre relative\"; "
                               "content:\"GET\"; "
                               "http_cookie; pcre:\"/abc/RM\"; sid:1;)");
    FAIL_IF_NOT_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with inconsistent pcre modifiers  */
int DetectPcreParseTest24(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing inconsistent pcre modifiers\"; "
                               "pcre:\"/abc/UI\"; sid:1;)");
    FAIL_IF_NOT_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with inconsistent pcre modifiers  */
int DetectPcreParseTest25(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert tcp any any -> any any "
                               "(msg:\"Testing inconsistent pcre modifiers\"; "
                               "pcre:\"/abc/DH\"; sid:1;)");
    FAIL_IF_NOT_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Check a signature with inconsistent pcre modifiers  */
static int DetectPcreParseTest26(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert http any any -> any any "
                               "(msg:\"Testing inconsistent pcre modifiers\"; "
                               "pcre:\"/abc/F\"; sid:1;)");
    FAIL_IF_NOT_NULL(de_ctx->sig_list);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Bug 1098 */
static int DetectPcreParseTest27(void)
{
    DetectEngineCtx *de_ctx = NULL;

    FAIL_IF( (de_ctx = DetectEngineCtxInit()) == NULL);

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any 80 "
            "(content:\"baduricontent\"; http_raw_uri; "
            "pcre:\"/^[a-z]{5}\\.html/R\"; sid:2; rev:2;)");
    FAIL_IF_NOT(de_ctx->sig_list == NULL);

    if (de_ctx != NULL)
        SigCleanSignatures(de_ctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectPcreTestSig01(void)
{
    uint8_t *buf = (uint8_t *)
        "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n"
        "\r\n\r\n"
        "GET /two/ HTTP/1.1\r\n"
        "Host: two.example.org\r\n"
        "\r\n\r\n";
    uint16_t buflen = strlen((char *)buf);
    TcpSession ssn;
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    Flow f;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&th_v, 0, sizeof(th_v));
    memset(&ssn, 0, sizeof(TcpSession));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;
    f.alproto = ALPROTO_HTTP;

    p = UTHBuildPacket(buf, buflen, IPPROTO_TCP);
    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"HTTP TEST\"; pcre:\"/^gEt/i\"; pcre:\"/\\/two\\//U\"; pcre:\"/GET \\/two\\//\"; pcre:\"/\\s+HTTP/R\"; sid:1;)");
    FAIL_IF(de_ctx->sig_list == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, buf, buflen);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF_NOT(PacketAlertCheck(p, 1) == 1);

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);

    FLOW_DESTROY(&f);

    UTHFreePackets(&p, 1);
    PASS;
}

static int DetectPcreTestSig02(void)
{
    uint8_t *buf = (uint8_t *)
        "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n"
        "\r\n\r\n"
        "GET /two/ HTTP/1.1\r\n"
        "Host: two.example.org\r\n"
        "\r\n\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    Flow f;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));

    FLOW_INITIALIZE(&f);

    p = UTHBuildPacket(buf, buflen, IPPROTO_TCP);
    p->flow = &f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;

    pcre_match_limit = 100;
    pcre_match_limit_recursion = 100;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"HTTP TEST\"; pcre:\"/two/O\"; sid:2;)");
    FAIL_IF(de_ctx->sig_list == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF_NOT(PacketAlertCheck(p, 2) == 1);

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
    FLOW_DESTROY(&f);

    UTHFreePackets(&p, 1);
    PASS;
}

/**
 * \test DetectPcreTestSig03Real negation test ! outside of "" this sig should not match
 */
static int DetectPcreTestSig03(void)
{
    uint8_t *buf = (uint8_t *)
        "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n"
        "\r\n\r\n"
        "GET /two/ HTTP/1.1\r\n"
        "Host: two.example.org\r\n"
        "\r\n\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));

    p = UTHBuildPacket(buf, buflen, IPPROTO_TCP);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"HTTP TEST\"; content:\"GET\"; pcre:!\"/two/\"; sid:1;)");
    FAIL_IF(de_ctx->sig_list == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    UTHFreePackets(&p, 1);
    PASS;
}

/**
 * \test Check the signature with pcre modifier P (match with L7 to http body data)
 */
static int DetectPcreModifPTest04(void)
{
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "X-Powered-By: PHP/5.2.5\r\n"
        "P3P: CP=\"NOI ADM DEV PSAi COM NAV OUR OTRo STP IND DEM\"\r\n"
        "Expires: Mon, 1 Jan 2001 00:00:00 GMT\r\n"
        "Last-Modified: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n"
        "Pragma: no-cache\r\n"
        "Keep-Alive: timeout=15, max=100\r\n"
        "Connection: Keep-Alive\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "15"
        "\r\n"
        "<!DOCTYPE html PUBLIC\r\n"
        "0\r\n\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"Pcre modifier P\"; pcre:\"/DOCTYPE/P\"; "
                                   "sid:1;)");
    FAIL_IF(s == NULL);

    s->next = SigInit(de_ctx,"alert http any any -> any any (msg:\""
                          "Pcre modifier P (no match)\"; pcre:\"/blah/P\"; sid:2;)");
    FAIL_IF(s->next == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    HtpState *http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!(PacketAlertCheck(p, 1)));
    FAIL_IF(PacketAlertCheck(p, 2));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    PASS;
}

/**
 * \test Check the signature with pcre modifier P (match with L7 to http body data)
 *       over fragmented chunks (DOCTYPE fragmented)
 */
static int DetectPcreModifPTest05(void)
{
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "X-Powered-By: PHP/5.2.5\r\n"
        "P3P: CP=\"NOI ADM DEV PSAi COM NAV OUR OTRo STP IND DEM\"\r\n"
        "Expires: Mon, 1 Jan 2001 00:00:00 GMT\r\n"
        "Last-Modified: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n"
        "Pragma: no-cache\r\n"
        "Keep-Alive: timeout=15, max=100\r\n"
        "Connection: Keep-Alive\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "15"
        "\r\n"
        "<!DOC";

    uint8_t httpbuf2[] = "<!DOCTYPE html PUBLIC\r\n0\r\n\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p1 = NULL;
    Packet *p2 = NULL;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p1 = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    p2 = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p1->flow = &f;
    p1->flowflags |= FLOW_PKT_TOSERVER;
    p1->flowflags |= FLOW_PKT_ESTABLISHED;
    p1->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p2->flow = &f;
    p2->flowflags |= FLOW_PKT_TOSERVER;
    p2->flowflags |= FLOW_PKT_ESTABLISHED;
    p2->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"Pcre modifier P\"; pcre:\"/DOC/P\"; "
                                   "sid:1;)");
    FAIL_IF(s == NULL);

    s->next = SigInit(de_ctx,"alert http any any -> any any (msg:\""
                          "Pcre modifier P (no match)\"; pcre:\"/DOCTYPE/P\"; sid:2;)");
    FAIL_IF(s->next == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect for p1 */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p1);

    HtpState *http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    FAIL_IF(PacketAlertCheck(p1, 1));

    FAIL_IF(PacketAlertCheck(p1, 2));

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf2, httplen2);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect for p2 */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p2);

    FAIL_IF(!(PacketAlertCheck(p2, 1)));

    FAIL_IF(!(PacketAlertCheck(p2, 2)));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p1, 1);
    UTHFreePackets(&p2, 1);
    PASS;
}

int DetectPcreTestSig06()
{
    uint8_t *buf = (uint8_t *)
                    "lalala lalala\\ lala\n";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = UTHBuildPacket( buf, buflen, IPPROTO_TCP);
    int result = 0;

    char sig[] = "alert tcp any any -> any any (msg:\"pcre with an ending slash\"; pcre:\"/ lalala\\\\/\"; sid:1;)";
    if (UTHPacketMatchSig(p, sig) == 0) {
        result = 0;
        goto end;
    }
    result = 1;
end:
    if (p != NULL)
        UTHFreePacket(p);
    return result;
}

/** \test anchored pcre */
int DetectPcreTestSig07()
{
    uint8_t *buf = (uint8_t *)
                    "lalala\n";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = UTHBuildPacket( buf, buflen, IPPROTO_TCP);

    char sig[] = "alert tcp any any -> any any (msg:\"pcre with an ending slash\"; pcre:\"/^(la)+$/\"; sid:1;)";
    FAIL_IF(UTHPacketMatchSig(p, sig) == 0);

    if (p != NULL)
        UTHFreePacket(p);
    PASS;
}

/** \test anchored pcre */
int DetectPcreTestSig08()
{
    /* test it also without ending in a newline "\n" */
    uint8_t *buf = (uint8_t *)
                    "lalala";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = UTHBuildPacket( buf, buflen, IPPROTO_TCP);

    char sig[] = "alert tcp any any -> any any (msg:\"pcre with an ending slash\"; pcre:\"/^(la)+$/\"; sid:1;)";
    FAIL_IF(UTHPacketMatchSig(p, sig) == 0);

    if (p != NULL)
        UTHFreePacket(p);
    PASS;
}

/** \test Check the signature working to alert when cookie modifier is
 *       passed to pcre
 */
static int DetectPcreTestSig09(void)
{
    Flow f;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummy\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"HTTP cookie\"; pcre:\"/dummy/C\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when cookie modifier is
 *       passed to a negated pcre
 */
static int DetectPcreTestSig10(void)
{
    Flow f;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummoOOooooO\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"HTTP cookie\"; pcre:!\"/dummy/C\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when method modifier is
 *       passed to pcre
 */
static int DetectPcreTestSig11(void)
{
    Flow f;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummy\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"HTTP method\"; pcre:\"/POST/M\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when method modifier is
 *       passed to a negated pcre
 */
static int DetectPcreTestSig12(void)
{
    Flow f;
    uint8_t httpbuf1[] = "GET / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummoOOooooO\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"HTTP method\"; pcre:!\"/POST/M\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when header modifier is
 *       passed to pcre
 */
static int DetectPcreTestSig13(void)
{
    Flow f;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummy\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"HTTP header\"; pcre:\"/User[-_]Agent[:]?\\sMozilla/H\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when header modifier is
 *       passed to a negated pcre
 */
static int DetectPcreTestSig14(void)
{
    Flow f;
    uint8_t httpbuf1[] = "GET / HTTP/1.0\r\nUser-Agent: IEXPLORER/1.0\r\n"
        "Cookie: dummoOOooooO\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"HTTP header\"; pcre:!\"/User-Agent[:]?\\s+Mozilla/H\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when cookie and relative modifiers are
 *       passed to pcre
 */
static int DetectPcreTestSig15(void)
{
    Flow f;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummy 1234\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"pcre relative HTTP cookie\"; content:\"dummy\";"
                                   " http_cookie; pcre:\"/1234/RC\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Check the signature working to alert when method and relative modifiers are
 *       passed to pcre
 */
static int DetectPcreTestSig16(void)
{
    Flow f;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\n"
        "Cookie: dummy 1234\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"pcre relative HTTP method\"; content:\"PO\";"
                                   " http_method; pcre:\"/ST/RM\"; "
                                   " sid:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    FAIL_IF(!PacketAlertCheck(p, 1));

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    UTHFreePackets(&p, 1);
    PASS;
}

/** \test Test tracking of body chunks per transactions (on requests)
 */
static int DetectPcreTxBodyChunksTest01(void)
{
    Flow f;
    TcpSession ssn;
    Packet *p = NULL;
    uint8_t httpbuf1[] = "GET / HTTP/1.1\r\n";
    uint8_t httpbuf2[] = "User-Agent: Mozilla/1.0\r\nContent-Length: 10\r\n";
    uint8_t httpbuf3[] = "Cookie: dummy\r\n\r\n";
    uint8_t httpbuf4[] = "Body one!!";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */
    uint8_t httpbuf5[] = "GET /?var=val HTTP/1.1\r\n";
    uint8_t httpbuf6[] = "User-Agent: Firefox/1.0\r\n";
    uint8_t httpbuf7[] = "Cookie: dummy2\r\nContent-Length: 10\r\n\r\nBody two!!";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */
    uint32_t httplen7 = sizeof(httpbuf7) - 1; /* minus the \0 */
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    AppLayerHtpEnableRequestBodyCallback();

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER | STREAM_START, httpbuf1,
                                httplen1);
    FAIL_IF(r != 0);

    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf2, httplen2);
    FAIL_IF(r != 0);

    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf3, httplen3);
    FAIL_IF(r != 0);

    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf4, httplen4);
    FAIL_IF(r != 0);

    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf5, httplen5);
    FAIL_IF(r != 0);

    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf6, httplen6);
    FAIL_IF(r != 0);

    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf7, httplen7);
    FAIL_IF(r != 0);

    /* Now we should have 2 transactions, each with it's own list
     * of request body chunks (let's test it) */

    HtpState *htp_state = f.alstate;
    FAIL_IF(htp_state == NULL);

    /* hardcoded check of the transactions and it's client body chunks */
    FAIL_IF(AppLayerParserGetTxCnt(IPPROTO_TCP, ALPROTO_HTTP, htp_state) != 2);

    htp_tx_t *t1 = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, htp_state, 0);
    htp_tx_t *t2 = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, htp_state, 1);

    HtpTxUserData *htud = (HtpTxUserData *) htp_tx_get_user_data(t1);
    FAIL_IF(htud == NULL);

    HtpBodyChunk *cur = htud->request_body.first;
    FAIL_IF(htud->request_body.first == NULL);

    FAIL_IF(StreamingBufferSegmentCompareRawData(htud->request_body.sb, &cur->sbseg, (uint8_t *)"Body one!!", 10) != 1);

    htud = (HtpTxUserData *) htp_tx_get_user_data(t2);

    cur = htud->request_body.first;
    FAIL_IF(htud->request_body.first == NULL);

    FAIL_IF(StreamingBufferSegmentCompareRawData(htud->request_body.sb, &cur->sbseg, (uint8_t *)"Body two!!", 10) != 1);

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    FLOWLOCK_UNLOCK(&f);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    PASS;
}

/** \test test pcre P modifier with multiple pipelined http transactions */
static int DetectPcreTxBodyChunksTest02(void)
{
    Signature *s = NULL;
    DetectEngineThreadCtx *det_ctx = NULL;
    ThreadVars th_v;
    Flow f;
    TcpSession ssn;
    Packet *p = NULL;
    uint8_t httpbuf1[] = "POST / HTTP/1.1\r\n";
    uint8_t httpbuf2[] = "User-Agent: Mozilla/1.0\r\nContent-Length: 10\r\n";
    uint8_t httpbuf3[] = "Cookie: dummy\r\n\r\n";
    uint8_t httpbuf4[] = "Body one!!";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */
    uint8_t httpbuf5[] = "GET /?var=val HTTP/1.1\r\n";
    uint8_t httpbuf6[] = "User-Agent: Firefox/1.0\r\n";
    uint8_t httpbuf7[] = "Cookie: dummy2\r\nContent-Length: 10\r\n\r\nBody two!!";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */
    uint32_t httplen7 = sizeof(httpbuf7) - 1; /* minus the \0 */
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"POST\"; http_method; content:\"Mozilla\"; http_header; content:\"dummy\"; http_cookie; pcre:\"/one/P\"; sid:1; rev:1;)");
    FAIL_IF(s == NULL);
    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"GET\"; http_method; content:\"Firefox\"; http_header; content:\"dummy2\"; http_cookie; pcre:\"/two/P\"; sid:2; rev:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf2, httplen2);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf3, httplen3);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf4, httplen4);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(!(PacketAlertCheck(p, 1)));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf5, httplen5);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf6, httplen6);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF((PacketAlertCheck(p, 1)) || (PacketAlertCheck(p, 2)));
    p->alerts.cnt = 0;

    SCLogDebug("sending data chunk 7");

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf7, httplen7);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(!(PacketAlertCheck(p, 2)));
    p->alerts.cnt = 0;

    HtpState *htp_state = f.alstate;
    FAIL_IF(htp_state == NULL);

    /* hardcoded check of the transactions and it's client body chunks */
    FAIL_IF(AppLayerParserGetTxCnt(IPPROTO_TCP, ALPROTO_HTTP, htp_state) != 2);

    htp_tx_t *t1 = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, htp_state, 0);
    htp_tx_t *t2 = AppLayerParserGetTx(IPPROTO_TCP, ALPROTO_HTTP, htp_state, 1);

    HtpTxUserData *htud = (HtpTxUserData *) htp_tx_get_user_data(t1);

    HtpBodyChunk *cur = htud->request_body.first;
    FAIL_IF(htud->request_body.first == NULL);

    FAIL_IF(StreamingBufferSegmentCompareRawData(htud->request_body.sb, &cur->sbseg, (uint8_t *)"Body one!!", 10) != 1);

    htud = (HtpTxUserData *) htp_tx_get_user_data(t2);

    cur = htud->request_body.first;
    FAIL_IF(htud->request_body.first == NULL);

    FAIL_IF(StreamingBufferSegmentCompareRawData(htud->request_body.sb, &cur->sbseg, (uint8_t *)"Body two!!", 10) != 1);

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    PASS;
}

/** \test multiple http transactions and body chunks of request handling */
static int DetectPcreTxBodyChunksTest03(void)
{
    Signature *s = NULL;
    DetectEngineThreadCtx *det_ctx = NULL;
    ThreadVars th_v;
    Flow f;
    TcpSession ssn;
    Packet *p = NULL;
    uint8_t httpbuf1[] = "POST / HTTP/1.1\r\n";
    uint8_t httpbuf2[] = "User-Agent: Mozilla/1.0\r\nContent-Length: 10\r\n";
    uint8_t httpbuf3[] = "Cookie: dummy\r\n\r\n";
    uint8_t httpbuf4[] = "Body one!!";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */
    uint8_t httpbuf5[] = "GET /?var=val HTTP/1.1\r\n";
    uint8_t httpbuf6[] = "User-Agent: Firefox/1.0\r\n";
    uint8_t httpbuf7[] = "Cookie: dummy2\r\nContent-Length: 10\r\n\r\nBody two!!";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */
    uint32_t httplen7 = sizeof(httpbuf7) - 1; /* minus the \0 */
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);

    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"POST\"; http_method; content:\"Mozilla\"; http_header; content:\"dummy\"; http_cookie; pcre:\"/one/P\"; sid:1; rev:1;)");
    FAIL_IF(s == NULL);
    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"GET\"; http_method; content:\"Firefox\"; http_header; content:\"dummy2\"; http_cookie; pcre:\"/two/P\"; sid:2; rev:1;)");
    FAIL_IF(s == NULL);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf2, httplen2);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf3, httplen3);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf4, httplen4);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(!(PacketAlertCheck(p, 1)));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf5, httplen5);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(PacketAlertCheck(p, 1));
    p->alerts.cnt = 0;

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf6, httplen6);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF((PacketAlertCheck(p, 1)) || (PacketAlertCheck(p, 2)));
    p->alerts.cnt = 0;

    SCLogDebug("sending data chunk 7");

    FLOWLOCK_WRLOCK(&f);
    r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                            STREAM_TOSERVER, httpbuf7, httplen7);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    FAIL_IF(!(PacketAlertCheck(p, 2)));
    p->alerts.cnt = 0;

    HtpState *htp_state = f.alstate;
    FAIL_IF(htp_state == NULL);

    FAIL_IF(AppLayerParserGetTxCnt(IPPROTO_TCP, ALPROTO_HTTP, htp_state) != 2);

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    PASS;
}

/**
 * \test flowvar capture on http buffer
 */
static int DetectPcreFlowvarCapture01(void)
{
    uint8_t uabuf1[] =
        "Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13";
    uint32_t ualen1 = sizeof(uabuf1) - 1; /* minus the \0 */
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "\r\n"
        "<!DOCTYPE html PUBLIC\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p1 = NULL;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p1 = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;
    f.alproto = ALPROTO_HTTP;

    p1->flow = &f;
    p1->flowflags |= FLOW_PKT_TOSERVER;
    p1->flowflags |= FLOW_PKT_ESTABLISHED;
    p1->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"User-Agent: \"; http_header; pcre:\"/(?P<flow_ua>.*)\\r\\n/HR\"; sid:1;)");
    FAIL_IF(s == NULL);

    FAIL_IF(s->sm_lists[DETECT_SM_LIST_HHDMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->type != DETECT_PCRE);
    DetectPcreData *pd = (DetectPcreData *)s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->ctx;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    HtpState *http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect for p1 */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p1);

    FAIL_IF(!(PacketAlertCheck(p1, 1)));

    FlowVar *fv = FlowVarGet(&f, pd->capidx);
    FAIL_IF(fv == NULL);

    FAIL_IF(fv->data.fv_str.value_len != ualen1);

    if (memcmp(fv->data.fv_str.value, uabuf1, ualen1) != 0) {
        PrintRawDataFp(stdout, fv->data.fv_str.value, fv->data.fv_str.value_len);
        PrintRawDataFp(stdout, uabuf1, ualen1);

        printf("buffer mismatch: ");
        FAIL;
    }

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p1, 1);
    PASS;
}

/**
 * \test flowvar capture on http buffer, capture overwrite
 */
static int DetectPcreFlowvarCapture02(void)
{
    uint8_t uabuf1[] =
        "Apache";
    uint32_t ualen1 = sizeof(uabuf1) - 1; /* minus the \0 */
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "\r\n"
        "<!DOCTYPE html PUBLIC\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p1 = NULL;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p1 = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;
    f.alproto = ALPROTO_HTTP;

    p1->flow = &f;
    p1->flowflags |= FLOW_PKT_TOSERVER;
    p1->flowflags |= FLOW_PKT_ESTABLISHED;
    p1->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"User-Agent: \"; http_header; pcre:\"/(?P<flow_ua>.*)\\r\\n/HR\"; priority:1; sid:1;)");
    FAIL_IF(s == NULL);

    FAIL_IF(s->sm_lists[DETECT_SM_LIST_HHDMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->type != DETECT_PCRE);
    DetectPcreData *pd1 = (DetectPcreData *)s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->ctx;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"Server: \"; http_header; pcre:\"/(?P<flow_ua>.*)\\r\\n/HR\"; priority:3; sid:2;)");
    FAIL_IF(s == NULL);

    FAIL_IF(s->sm_lists[DETECT_SM_LIST_HHDMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->type != DETECT_PCRE);
    DetectPcreData *pd2 = (DetectPcreData *)s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->ctx;

    FAIL_IF(pd1->capidx != pd2->capidx);

    SCSigRegisterSignatureOrderingFuncs(de_ctx);
    SCSigOrderSignatures(de_ctx);
    SCSigSignatureOrderingModuleCleanup(de_ctx);
    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    HtpState *http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect for p1 */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p1);

    FAIL_IF(!(PacketAlertCheck(p1, 1)));

    FlowVar *fv = FlowVarGet(&f, pd1->capidx);
    FAIL_IF(fv == NULL);

    if (fv->data.fv_str.value_len != ualen1) {
        PrintRawDataFp(stdout, fv->data.fv_str.value, fv->data.fv_str.value_len);
        PrintRawDataFp(stdout, uabuf1, ualen1);
        printf("%u != %u: ", fv->data.fv_str.value_len, ualen1);
        FAIL;
    }

    if (memcmp(fv->data.fv_str.value, uabuf1, ualen1) != 0) {
        PrintRawDataFp(stdout, fv->data.fv_str.value, fv->data.fv_str.value_len);
        PrintRawDataFp(stdout, uabuf1, ualen1);

        printf("buffer mismatch: ");
        FAIL;
    }

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p1, 1);
    PASS;
}

/**
 * \test flowvar capture on http buffer, capture overwrite + no matching sigs, so flowvars should not be set.
 */
static int DetectPcreFlowvarCapture03(void)
{
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "\r\n"
        "<!DOCTYPE html PUBLIC\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p1 = NULL;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p1 = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;
    f.alproto = ALPROTO_HTTP;

    p1->flow = &f;
    p1->flowflags |= FLOW_PKT_TOSERVER;
    p1->flowflags |= FLOW_PKT_ESTABLISHED;
    p1->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"User-Agent: \"; http_header; pcre:\"/(?P<flow_ua>.*)\\r\\n/HR\"; content:\"xyz\"; http_header; priority:1; sid:1;)");
    FAIL_IF(s == NULL);

    FAIL_IF(s->sm_lists[DETECT_SM_LIST_HHDMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->type != DETECT_PCRE);
    DetectPcreData *pd1 = (DetectPcreData *)s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->ctx;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"Server: \"; http_header; pcre:\"/(?P<flow_ua>.*)\\r\\n/HR\"; content:\"xyz\"; http_header; priority:3; sid:2;)");
    FAIL_IF(s == NULL);

    FAIL_IF(s->sm_lists[DETECT_SM_LIST_HHDMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next == NULL ||
        s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->type != DETECT_PCRE);
    DetectPcreData *pd2 = (DetectPcreData *)s->sm_lists[DETECT_SM_LIST_HHDMATCH]->next->ctx;

    FAIL_IF(pd1->capidx != pd2->capidx);

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    FLOWLOCK_WRLOCK(&f);
    int r = AppLayerParserParse(NULL, alp_tctx, &f, ALPROTO_HTTP,
                                STREAM_TOSERVER, httpbuf1, httplen1);
    FAIL_IF(r != 0);
    FLOWLOCK_UNLOCK(&f);

    HtpState *http_state = f.alstate;
    FAIL_IF(http_state == NULL);

    /* do detect for p1 */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p1);

    FAIL_IF(PacketAlertCheck(p1, 1));

    FlowVar *fv = FlowVarGet(&f, pd1->capidx);
    FAIL_IF(fv != NULL);

    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p1, 1);
    PASS;
}

/**
 * \brief Test parsing of pcre's with the W modifier set.
 */
static int DetectPcreParseHttpHost(void)
{
    DetectPcreData *pd = NULL;
    int list = DETECT_SM_LIST_NOTSET;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();

    FAIL_IF(de_ctx == NULL);

    pd = DetectPcreParse(de_ctx, "/domain\\.com/W", &list);
    FAIL_IF(pd == NULL);
    DetectPcreFree(pd);

    list = DETECT_SM_LIST_NOTSET;
    pd = DetectPcreParse(de_ctx, "/dOmain\\.com/W", &list);
    FAIL_IF(pd != NULL);

    /* Uppercase meta characters are valid. */
    list = DETECT_SM_LIST_NOTSET;
    pd = DetectPcreParse(de_ctx, "/domain\\D+\\.com/W", &list);
    FAIL_IF(pd == NULL);
    DetectPcreFree(pd);

    /* This should not parse as the first \ escapes the second \, then
     * we have a D. */
    list = DETECT_SM_LIST_NOTSET;
    pd = DetectPcreParse(de_ctx, "/\\\\Ddomain\\.com/W", &list);
    FAIL_IF(pd != NULL);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectPcre
 */
void DetectPcreRegisterTests(void)
{
#ifdef UNITTESTS /* UNITTESTS */
    UtRegisterTest("DetectPcreParseTest01", DetectPcreParseTest01);
    UtRegisterTest("DetectPcreParseTest02", DetectPcreParseTest02);
    UtRegisterTest("DetectPcreParseTest03", DetectPcreParseTest03);
    UtRegisterTest("DetectPcreParseTest04", DetectPcreParseTest04);
    UtRegisterTest("DetectPcreParseTest05", DetectPcreParseTest05);
    UtRegisterTest("DetectPcreParseTest06", DetectPcreParseTest06);
    UtRegisterTest("DetectPcreParseTest07", DetectPcreParseTest07);
    UtRegisterTest("DetectPcreParseTest08", DetectPcreParseTest08);
    UtRegisterTest("DetectPcreParseTest09", DetectPcreParseTest09);
    UtRegisterTest("DetectPcreParseTest10", DetectPcreParseTest10);
    UtRegisterTest("DetectPcreParseTest11", DetectPcreParseTest11);
    UtRegisterTest("DetectPcreParseTest12", DetectPcreParseTest12);
    UtRegisterTest("DetectPcreParseTest13", DetectPcreParseTest13);
    UtRegisterTest("DetectPcreParseTest14", DetectPcreParseTest14);
    UtRegisterTest("DetectPcreParseTest15", DetectPcreParseTest15);
    UtRegisterTest("DetectPcreParseTest16", DetectPcreParseTest16);
    UtRegisterTest("DetectPcreParseTest17", DetectPcreParseTest17);
    UtRegisterTest("DetectPcreParseTest18", DetectPcreParseTest18);
    UtRegisterTest("DetectPcreParseTest19", DetectPcreParseTest19);
    UtRegisterTest("DetectPcreParseTest20", DetectPcreParseTest20);
    UtRegisterTest("DetectPcreParseTest21", DetectPcreParseTest21);
    UtRegisterTest("DetectPcreParseTest22", DetectPcreParseTest22);
    UtRegisterTest("DetectPcreParseTest23", DetectPcreParseTest23);
    UtRegisterTest("DetectPcreParseTest24", DetectPcreParseTest24);
    UtRegisterTest("DetectPcreParseTest25", DetectPcreParseTest25);
    UtRegisterTest("DetectPcreParseTest26", DetectPcreParseTest26);
    UtRegisterTest("DetectPcreParseTest27", DetectPcreParseTest27);

    UtRegisterTest("DetectPcreTestSig01 -- pcre test", DetectPcreTestSig01);
    UtRegisterTest("DetectPcreTestSig02 -- pcre test", DetectPcreTestSig02);
    UtRegisterTest("DetectPcreTestSig03 -- negated pcre test",
                   DetectPcreTestSig03);

    UtRegisterTest("DetectPcreModifPTest04 -- Modifier P",
                   DetectPcreModifPTest04);
    UtRegisterTest("DetectPcreModifPTest05 -- Modifier P fragmented",
                   DetectPcreModifPTest05);
    UtRegisterTest("DetectPcreTestSig06", DetectPcreTestSig06);
    UtRegisterTest("DetectPcreTestSig07 -- anchored pcre",
                   DetectPcreTestSig07);
    UtRegisterTest("DetectPcreTestSig08 -- anchored pcre",
                   DetectPcreTestSig08);
    UtRegisterTest("DetectPcreTestSig09 -- Cookie modifier",
                   DetectPcreTestSig09);
    UtRegisterTest("DetectPcreTestSig10 -- negated Cookie modifier",
                   DetectPcreTestSig10);
    UtRegisterTest("DetectPcreTestSig11 -- Method modifier",
                   DetectPcreTestSig11);
    UtRegisterTest("DetectPcreTestSig12 -- negated Method modifier",
                   DetectPcreTestSig12);
    UtRegisterTest("DetectPcreTestSig13 -- Header modifier",
                   DetectPcreTestSig13);
    UtRegisterTest("DetectPcreTestSig14 -- negated Header modifier",
                   DetectPcreTestSig14);
    UtRegisterTest("DetectPcreTestSig15 -- relative Cookie modifier",
                   DetectPcreTestSig15);
    UtRegisterTest("DetectPcreTestSig16 -- relative Method modifier",
                   DetectPcreTestSig16);

    UtRegisterTest("DetectPcreTxBodyChunksTest01",
                   DetectPcreTxBodyChunksTest01);
    UtRegisterTest("DetectPcreTxBodyChunksTest02 -- modifier P, body chunks per tx",
                   DetectPcreTxBodyChunksTest02);
    UtRegisterTest("DetectPcreTxBodyChunksTest03 -- modifier P, body chunks per tx",
                   DetectPcreTxBodyChunksTest03);

    UtRegisterTest("DetectPcreFlowvarCapture01 -- capture for http_header",
                   DetectPcreFlowvarCapture01);
    UtRegisterTest("DetectPcreFlowvarCapture02 -- capture for http_header",
                   DetectPcreFlowvarCapture02);
    UtRegisterTest("DetectPcreFlowvarCapture03 -- capture for http_header",
                   DetectPcreFlowvarCapture03);

    UtRegisterTest("DetectPcreParseHttpHost", DetectPcreParseHttpHost);

#endif /* UNITTESTS */
}

