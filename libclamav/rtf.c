/*
 *  Extract embedded objects from RTF files.
 *
 *  Copyright (C) 2013-2020 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *  Copyright (C) 2007-2013 Sourcefire, Inc.
 *
 *  Authors: Török Edvin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <ctype.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "others.h"
#include "rtf.h"
#include "clamav.h"
#include "table.h"
#include "scanners.h"
#include "vba_extract.h"

enum parse_state { PARSE_MAIN,
                   PARSE_CONTROL_,
                   PARSE_CONTROL_WORD,
                   PARSE_CONTROL_SYMBOL,
                   PARSE_CONTROL_WORD_PARAM,
                   PARSE_INTERPRET_CONTROLWORD };

enum rtf_action {
    RTF_OBJECT,
    RTF_OBJECT_DATA
};

struct rtf_state;
typedef int (*rtf_callback_begin)(struct rtf_state*, cli_ctx* ctx, const char* tmpdir);
typedef int (*rtf_callback_process)(struct rtf_state*, const unsigned char* data, const size_t len);
typedef int (*rtf_callback_end)(struct rtf_state*, cli_ctx*);

struct rtf_state {
    rtf_callback_begin cb_begin; /* must be non-null if you want cb_process, and cb_end to be called, also it must change cb_data to non-null */
    rtf_callback_process cb_process;
    rtf_callback_end cb_end;
    void* cb_data; /* data set up by cb_begin, used by cb_process, and cleaned up by cb_end. typically state data */
    size_t default_elements;
    size_t controlword_cnt;
    int64_t controlword_param;
    enum parse_state parse_state;
    int controlword_param_sign;
    int encounteredTopLevel; /* encountered top-level control words that we care about */
    char controlword[33];
};

static const struct rtf_state base_state = {
    NULL, NULL, NULL, NULL, 0, 0, 0, PARSE_MAIN, 0, 0, "                              "};

struct stack {
    struct rtf_state* states;
    size_t elements;
    size_t stack_cnt;
    size_t stack_size;
    int warned;
};

static const struct rtf_action_mapping {
    const char* controlword;
    const enum rtf_action action;
} rtf_action_mapping[] =
    {
        {"object", RTF_OBJECT},
        {"objdata ", RTF_OBJECT_DATA}};

static const size_t rtf_action_mapping_cnt = sizeof(rtf_action_mapping) / sizeof(rtf_action_mapping[0]);

enum rtf_objdata_state { WAIT_MAGIC,
                         WAIT_DESC_LEN,
                         WAIT_DESC,
                         WAIT_ZERO,
                         WAIT_DATA_SIZE,
                         DUMP_DATA,
                         DUMP_DISCARD };
static const unsigned char rtf_data_magic[] = {0x01, 0x05, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}; /* is this a magic number, or does it mean something */
static const size_t rtf_data_magic_len      = sizeof(rtf_data_magic);

struct rtf_object_data {
    char* name;
    int fd;
    int partial;
    int has_partial;
    enum rtf_objdata_state internal_state;
    char* desc_name;
    const char* tmpdir;
    cli_ctx* ctx;
    size_t desc_len;
    size_t bread;
};

#define BUFF_SIZE 8192
/* generated by contrib/phishing/generate_tables.c */
static const short int hextable[256] = {
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

static void init_rtf_state(struct rtf_state* state)
{
    *state                 = base_state;
    state->parse_state     = PARSE_MAIN;
    state->controlword_cnt = 0;
}

static int compare_state(const struct rtf_state* a, const struct rtf_state* b)
{
    return (a->parse_state == b->parse_state &&
            a->encounteredTopLevel == b->encounteredTopLevel &&
            a->cb_begin == b->cb_begin &&
            a->cb_process == b->cb_process &&
            a->cb_end == b->cb_end &&
            a->cb_data == b->cb_data);
}

static int push_state(struct stack* stack, struct rtf_state* state)
{
    int toplevel;
    size_t defelements;

    stack->elements++;
    if (compare_state(state, &base_state)) {
        state->default_elements++;
        return 0; /* this is default state, don't push it, we'll know when we pop it that it was the default one,
			  we store in the state how many default elements we have on the stack */
    }
    if (stack->stack_cnt >= stack->stack_size) {
        /* grow stack */
        struct rtf_state* states;
        stack->stack_size += 128;
        states = cli_realloc2(stack->states, stack->stack_size * sizeof(*stack->states));
        if (!states)
            return CL_EMEM;
        stack->states = states;
    }
    stack->states[stack->stack_cnt++] = *state;
    toplevel                          = state->encounteredTopLevel;
    defelements                       = state->default_elements;

    *state = base_state;

    state->encounteredTopLevel = toplevel;
    state->default_elements    = 0;
    return 0;
}

static int pop_state(struct stack* stack, struct rtf_state* state)
{
    stack->elements--;
    if (state->default_elements) {
        const size_t default_elements = state->default_elements - 1;
        const int toplevel            = state->encounteredTopLevel;
        *state                        = base_state;
        state->default_elements       = default_elements;
        state->encounteredTopLevel    = toplevel;
        return 0; /* this is a default 'state'*/
    }
    if (!stack->stack_cnt) {
        if (!stack->warned) {
            cli_dbgmsg("Warning: attempt to pop from empty stack!\n");
            stack->warned = 1;
        }
        *state = base_state; /* lets assume we give it a base state */
        return 0;
    }
    *state = stack->states[--stack->stack_cnt];
    return 0;
}

static int load_actions(table_t* t)
{
    size_t i;
    for (i = 0; i < rtf_action_mapping_cnt; i++)
        if (tableInsert(t, rtf_action_mapping[i].controlword, rtf_action_mapping[i].action) == -1)
            return -1;
    return 0;
}

static int rtf_object_begin(struct rtf_state* state, cli_ctx* ctx, const char* tmpdir)
{
    struct rtf_object_data* data = cli_malloc(sizeof(*data));
    if (!data) {
        cli_errmsg("rtf_object_begin: Unable to allocate memory for object data\n");
        return CL_EMEM;
    }
    data->fd             = -1;
    data->partial        = 0;
    data->has_partial    = 0;
    data->bread          = 0;
    data->internal_state = WAIT_MAGIC;
    data->tmpdir         = tmpdir;
    data->ctx            = ctx;
    data->name           = NULL;
    data->desc_name      = NULL;

    state->cb_data = data;
    return 0;
}

static int decode_and_scan(struct rtf_object_data* data, cli_ctx* ctx)
{
    int ret = CL_CLEAN;

    cli_dbgmsg("RTF:Scanning embedded object:%s\n", data->name);
    if (data->bread == 1 && data->fd > 0) {
        cli_dbgmsg("Decoding ole object\n");
        ret = cli_scan_ole10(data->fd, ctx);
    } else if (data->fd > 0)
        ret = cli_magic_scandesc(data->fd, data->name, ctx);
    if (data->fd > 0)
        close(data->fd);
    data->fd = -1;
    if (data->name) {
        if (!ctx->engine->keeptmp)
            if (cli_unlink(data->name)) ret = CL_EUNLINK;
        free(data->name);
        data->name = NULL;
    }

    if (ret != CL_CLEAN)
        return ret;
    return 0;
}

static int rtf_object_process(struct rtf_state* state, const unsigned char* input, const size_t len)
{
    struct rtf_object_data* data = state->cb_data;
    unsigned char outdata[BUFF_SIZE];
    const unsigned char* out_data;
    size_t out_cnt = 0;
    size_t i;
    int ret;

    if (!data || !len)
        return 0;

    if (data->has_partial) {
        for (i = 0; i < len && !isxdigit(input[i]); i++)
            ;
        if (i < len) {
            outdata[out_cnt++] = data->partial | hextable[input[i++]];
            data->has_partial  = 0;
        } else
            return 0;
    } else
        i = 0;

    for (; i < len; i++) {
        if (isxdigit(input[i])) {
            const unsigned char byte = hextable[input[i++]] << 4;
            while (i < len && !isxdigit(input[i]))
                i++;
            if (i == len) {
                data->partial     = byte;
                data->has_partial = 1;
                break;
            }
            outdata[out_cnt++] = byte | hextable[input[i]];
        }
    }

    out_data = outdata;
    while (out_data && out_cnt) {
        switch (data->internal_state) {
            case WAIT_MAGIC: {
                cli_dbgmsg("RTF: waiting for magic\n");
                for (i = 0; i < out_cnt && data->bread < rtf_data_magic_len; i++, data->bread++)
                    if (rtf_data_magic[data->bread] != out_data[i]) {
                        cli_dbgmsg("Warning: rtf objdata magic number not matched, expected:%d, got: %d, at pos:%lu\n", rtf_data_magic[i], out_data[i], (unsigned long int)data->bread);
                    }
                out_cnt -= i;
                if (data->bread == rtf_data_magic_len) {
                    out_data += i;
                    data->bread          = 0;
                    data->internal_state = WAIT_DESC_LEN;
                }
                break;
            }
            case WAIT_DESC_LEN: {
                if (data->bread == 0)
                    data->desc_len = 0;
                for (i = 0; i < out_cnt && data->bread < 4; i++, data->bread++)
                    data->desc_len |= ((size_t)out_data[i]) << (data->bread * 8);
                out_cnt -= i;
                if (data->bread == 4) {
                    out_data += i;
                    data->bread = 0;
                    if (data->desc_len > 64) {
                        cli_dbgmsg("Description length too big (%lu), showing only 64 bytes of it\n", (unsigned long int)data->desc_len);
                        data->desc_name = cli_malloc(65);
                    } else
                        data->desc_name = cli_malloc(data->desc_len + 1);
                    if (!data->desc_name) {
                        cli_errmsg("rtf_object_process: Unable to allocate memory for data->desc_name\n");
                        return CL_EMEM;
                    }
                    data->internal_state = WAIT_DESC;
                    cli_dbgmsg("RTF: description length:%lu\n", (unsigned long int)data->desc_len);
                }
                break;
            }
            case WAIT_DESC: {
                cli_dbgmsg("RTF: in WAIT_DESC\n");
                for (i = 0; i < out_cnt && data->bread < data->desc_len && data->bread < 64; i++, data->bread++)
                    data->desc_name[data->bread] = out_data[i];
                out_cnt -= i;
                out_data += i;
                if (data->bread < data->desc_len && data->bread < 64) {
                    cli_dbgmsg("RTF: waiting for more data(1)\n");
                    return 0; /* wait for more data */
                }
                data->desc_name[data->bread] = '\0';
                if (data->desc_len - data->bread > out_cnt) {
                    data->desc_len -= out_cnt;
                    cli_dbgmsg("RTF: waiting for more data(2)\n");
                    return 0; /* wait for more data */
                }
                out_cnt -= data->desc_len - data->bread;
                if (data->bread >= data->desc_len) {
                    out_data += data->desc_len - data->bread;
                    data->bread = 0;
                    cli_dbgmsg("Preparing to dump rtf embedded object, description:%s\n", data->desc_name);
                    free(data->desc_name);
                    data->desc_name      = NULL;
                    data->internal_state = WAIT_ZERO;
                }
                break;
            }
            case WAIT_ZERO: {
                if (out_cnt < 8 - data->bread) {
                    out_cnt = 0;
                    data->bread += out_cnt;
                } else {
                    out_cnt -= 8 - data->bread;
                    data->bread = 8;
                }
                if (data->bread == 8) {
                    out_data += 8;
                    data->bread = 0;
                    cli_dbgmsg("RTF: next state: wait_data_size\n");
                    data->internal_state = WAIT_DATA_SIZE;
                }
                break;
            }

            case WAIT_DATA_SIZE: {
                cli_dbgmsg("RTF: in WAIT_DATA_SIZE\n");
                if (data->bread == 0)
                    data->desc_len = 0;
                for (i = 0; i < out_cnt && data->bread < 4; i++, data->bread++)
                    data->desc_len |= ((size_t)out_data[i]) << (8 * data->bread);
                out_cnt -= i;
                if (data->bread == 4) {
                    out_data += i;
                    data->bread = 0;
                    cli_dbgmsg("Dumping rtf embedded object of size:%lu\n", (unsigned long int)data->desc_len);
                    if ((ret = cli_gentempfd(data->tmpdir, &data->name, &data->fd)))
                        return ret;
                    data->internal_state = DUMP_DATA;
                    cli_dbgmsg("RTF: next state: DUMP_DATA\n");
                }
                break;
            }
            case DUMP_DATA: {
                size_t out_want = (out_cnt < data->desc_len) ? out_cnt : data->desc_len;
                if (!data->bread) {
                    if (out_data[0] != 0xd0 || out_data[1] != 0xcf) {
                        /* this is not an ole2 doc, but some ole (stream?) to be
								 * decoded by cli_decode_ole_object*/
                        char out[4];
                        data->bread = 1; /* flag to indicate this needs to be scanned with cli_decode_ole_object*/
                        cli_writeint32(out, data->desc_len);
                        if (cli_writen(data->fd, out, 4) != 4)
                            return CL_EWRITE;
                    } else
                        data->bread = 2;
                }

                data->desc_len -= out_want;
                if (cli_writen(data->fd, out_data, out_want) != out_want) {
                    return CL_EWRITE;
                }
                out_data += out_want;
                out_cnt -= out_want;
                if (!data->desc_len) {
                    int rc;
                    if ((rc = decode_and_scan(data, data->ctx)))
                        return rc;
                    data->bread          = 0;
                    data->internal_state = WAIT_MAGIC;
                }
                break;
            }
            case DUMP_DISCARD:
            default:
                out_cnt = 0;
                ;
        }
    }
    return 0;
}

static int rtf_object_end(struct rtf_state* state, cli_ctx* ctx)
{
    struct rtf_object_data* data = state->cb_data;
    int rc                       = 0;
    if (!data)
        return 0;
    if (data->fd > 0) {
        rc = decode_and_scan(data, ctx);
    }
    if (data->name)
        free(data->name);
    if (data->desc_name)
        free(data->desc_name);
    free(data);
    state->cb_data = NULL;
    return rc;
}

static void rtf_action(struct rtf_state* state, long action)
{
    switch (action) {
        case RTF_OBJECT:
            state->encounteredTopLevel |= 1 << RTF_OBJECT;
            break;
        case RTF_OBJECT_DATA:
            if (state->encounteredTopLevel & (1 << RTF_OBJECT)) {
                state->cb_begin   = rtf_object_begin;
                state->cb_process = rtf_object_process;
                state->cb_end     = rtf_object_end;
            }
            break;
    };
}

static void cleanup_stack(struct stack* stack, struct rtf_state* state, cli_ctx* ctx)
{
    if (!stack || !stack->states)
        return;
    while (stack && stack->stack_cnt /* && state->default_elements*/) {
        pop_state(stack, state);
        if (state->cb_data && state->cb_end)
            state->cb_end(state, ctx);
    }
}

#define SCAN_CLEANUP                    \
    if (state.cb_data && state.cb_end)  \
        state.cb_end(&state, ctx);      \
    tableDestroy(actiontable);          \
    cleanup_stack(&stack, &state, ctx); \
    if (!ctx->engine->keeptmp)          \
        cli_rmdirs(tempname);           \
    free(tempname);                     \
    free(stack.states);

int cli_scanrtf(cli_ctx* ctx)
{
    char* tempname;
    const unsigned char* ptr;
    const unsigned char* ptr_end;
    int ret = CL_CLEAN;
    struct rtf_state state;
    struct stack stack;
    size_t bread;
    table_t* actiontable;
    uint8_t main_symbols[256];
    size_t offset = 0;

    cli_dbgmsg("in cli_scanrtf()\n");

    memset(main_symbols, 0, 256);
    main_symbols['{']  = 1;
    main_symbols['}']  = 1;
    main_symbols['\\'] = 1;

    stack.stack_cnt  = 0;
    stack.stack_size = 16;
    stack.elements   = 0;
    stack.warned     = 0;
    stack.states     = cli_malloc(stack.stack_size * sizeof(*stack.states));

    if (!stack.states) {
        cli_errmsg("ScanRTF: Unable to allocate memory for stack states\n");
        return CL_EMEM;
    }

    if (!(tempname = cli_gentemp(ctx->engine->tmpdir)))
        return CL_EMEM;

    if (mkdir(tempname, 0700)) {
        cli_dbgmsg("ScanRTF -> Can't create temporary directory %s\n", tempname);
        free(stack.states);
        free(tempname);
        return CL_ETMPDIR;
    }

    actiontable = tableCreate();
    if ((ret = load_actions(actiontable))) {
        cli_dbgmsg("RTF: Unable to load rtf action table\n");
        free(stack.states);
        if (!ctx->engine->keeptmp)
            cli_rmdirs(tempname);
        free(tempname);
        tableDestroy(actiontable);
        return ret;
    }

    init_rtf_state(&state);

    for (offset = 0; (ptr = fmap_need_off_once_len(*ctx->fmap, offset, BUFF_SIZE, &bread)) && bread; offset += bread) {
        ptr_end = ptr + bread;
        while (ptr < ptr_end) {
            switch (state.parse_state) {
                case PARSE_MAIN:
                    switch (*ptr++) {
                        case '{':
                            if ((ret = push_state(&stack, &state))) {
                                cli_dbgmsg("RTF:Push failure!\n");
                                SCAN_CLEANUP;
                                return ret;
                            }
                            break;
                        case '}':
                            if (state.cb_data && state.cb_end)
                                if ((ret = state.cb_end(&state, ctx))) {
                                    SCAN_CLEANUP;
                                    return ret;
                                }
                            if ((ret = pop_state(&stack, &state))) {
                                cli_dbgmsg("RTF:pop failure!\n");
                                SCAN_CLEANUP;
                                return ret;
                            }
                            break;
                        case '\\':
                            state.parse_state = PARSE_CONTROL_;
                            break;
                        default:
                            ptr--;
                            {
                                size_t i;
                                size_t left = ptr_end - ptr;
                                size_t use  = left;
                                for (i = 1; i < left; i++)
                                    if (main_symbols[ptr[i]]) {
                                        use = i;
                                        break;
                                    }
                                if (state.cb_begin) {
                                    if (!state.cb_data)
                                        if ((ret = state.cb_begin(&state, ctx, tempname))) {
                                            SCAN_CLEANUP;
                                            return ret;
                                        }
                                    if ((ret = state.cb_process(&state, ptr, use))) {
                                        if (state.cb_end) {
                                            state.cb_end(&state, ctx);
                                        }
                                        SCAN_CLEANUP;
                                        return ret;
                                    }
                                }
                                ptr += use;
                            }
                    }
                    break;
                case PARSE_CONTROL_:
                    if (isalpha(*ptr)) {
                        state.parse_state     = PARSE_CONTROL_WORD;
                        state.controlword_cnt = 0;
                    } else
                        state.parse_state = PARSE_CONTROL_SYMBOL;
                    break;
                case PARSE_CONTROL_SYMBOL:
                    ptr++; /* Do nothing */
                    state.parse_state = PARSE_MAIN;
                    break;
                case PARSE_CONTROL_WORD:
                    if (state.controlword_cnt == 32) {
                        cli_dbgmsg("Invalid control word: maximum size exceeded:%s\n", state.controlword);
                        state.parse_state = PARSE_MAIN;
                    } else if (isalpha(*ptr))
                        state.controlword[state.controlword_cnt++] = *ptr++;
                    else {
                        if (isspace(*ptr)) {
                            state.controlword[state.controlword_cnt++] = *ptr++;
                            state.parse_state                          = PARSE_INTERPRET_CONTROLWORD;
                        } else if (isdigit(*ptr)) {
                            state.parse_state            = PARSE_CONTROL_WORD_PARAM;
                            state.controlword_param      = 0;
                            state.controlword_param_sign = 1;
                        } else if (*ptr == '-') {
                            ptr++;
                            state.parse_state            = PARSE_CONTROL_WORD_PARAM;
                            state.controlword_param      = 0;
                            state.controlword_param_sign = -1;
                        } else {
                            state.parse_state = PARSE_INTERPRET_CONTROLWORD;
                        }
                    }
                    break;
                case PARSE_CONTROL_WORD_PARAM:
                    if (isdigit(*ptr)) {
                        if (((state.controlword_param) > INT64_MAX / 10) ||
                            (state.controlword_param * 10 > INT64_MAX - (*ptr - '0'))) {
                            cli_dbgmsg("Invalid control word param: maximum size exceeded.\n");
                            state.parse_state = PARSE_MAIN;
                        } else {
                            state.controlword_param = state.controlword_param * 10 + (*ptr - '0');
                            ptr++;
                        }
                    } else if (isalpha(*ptr)) {
                        ptr++;
                    } else {
                        if (state.controlword_param_sign < 0)
                            state.controlword_param = -state.controlword_param;
                        state.parse_state = PARSE_INTERPRET_CONTROLWORD;
                    }
                    break;
                case PARSE_INTERPRET_CONTROLWORD: {
                    int action;

                    state.controlword[state.controlword_cnt] = '\0';
                    action                                   = tableFind(actiontable, state.controlword);
                    if (action != -1) {
                        if (state.cb_data && state.cb_end) { /* premature end of previous block */
                            state.cb_end(&state, ctx);
                            state.cb_begin = NULL;
                            state.cb_end   = NULL;
                            state.cb_data  = NULL;
                        }
                        rtf_action(&state, action);
                    }
                    state.parse_state = PARSE_MAIN;
                    break;
                }
            }
        }
    }

    SCAN_CLEANUP;
    return ret;
}
