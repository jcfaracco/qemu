/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  QEMU TCG monitor
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-types-machine.h"
#include "monitor/monitor.h"
#include "system/tcg.h"
#include "hw/core/cpu.h"
#include "tcg/tcg.h"
#include "internal-common.h"
#include "exec/translation-block.h"
#include "tb-context.h"
#include "qemu/qht.h"

HumanReadableText *qmp_x_query_jit(Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");

    if (!tcg_enabled()) {
        error_setg(errp, "JIT information is only available with accel=tcg");
        return NULL;
    }

    tcg_dump_stats(buf);

    return human_readable_text_from_str(buf);
}

static gint compare_tb_exec_count(gconstpointer a, gconstpointer b)
{
    const TranslationBlock *tba = *(const TranslationBlock **)a;
    const TranslationBlock *tbb = *(const TranslationBlock **)b;

    if (tba->exec_count > tbb->exec_count) {
        return -1;
    } else if (tba->exec_count < tbb->exec_count) {
        return 1;
    }
    return 0;
}

static void collect_tb(void *p, uint32_t hash, void *userp)
{
    GPtrArray *array = userp;
    g_ptr_array_add(array, p);
}

TCGHotBlockList *qmp_query_tcg_hot_blocks(bool has_top_n, int64_t top_n,
                                         bool has_vcpu, int64_t vcpu,
                                         bool has_min_exec_count,
                                         uint32_t min_exec_count,
                                         Error **errp)
{
    TCGHotBlockList *head = NULL, **tail = &head;
    g_autoptr(GPtrArray) array = g_ptr_array_new();
    int i, n;

    if (!tcg_enabled()) {
        error_setg(errp, "TCG hot blocks are only available with accel=tcg");
        return NULL;
    }

    if (has_vcpu && !qemu_get_cpu(vcpu)) {
        error_setg(errp, "Invalid vCPU index %" PRId64, vcpu);
        return NULL;
    }

    if (!has_top_n) {
        top_n = 20;
    }

    qht_iter(&tb_ctx.htable, collect_tb, array);

    g_ptr_array_sort(array, compare_tb_exec_count);

    for (i = 0, n = 0; i < array->len && n < top_n; i++) {
        TranslationBlock *tb = g_ptr_array_index(array, i);
        TCGHotBlock *info;

        if (has_min_exec_count && tb->exec_count < min_exec_count) {
            break;
        }

        if (has_vcpu && tb->creator_vcpu != vcpu) {
            continue;
        }

        info = g_new0(TCGHotBlock, 1);
        info->pc = tb->pc;
        info->exec_count = tb->exec_count;
        info->size = tb->size;
        info->host_size = tb->tc.size;
        info->num_insns = tb->icount;
        info->flags = tb->flags;
        info->vcpu = tb->creator_vcpu;

        QAPI_LIST_APPEND(tail, info);
        n++;
    }

    return head;
}

HumanReadableText *qmp_x_query_tcg_hot_blocks(bool has_top_n, int64_t top_n,
                                              bool has_vcpu, int64_t vcpu,
                                              bool has_min_exec_count,
                                              uint32_t min_exec_count,
                                              Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");
    TCGHotBlockList *list, *curr;

    list = qmp_query_tcg_hot_blocks(has_top_n, top_n, has_vcpu, vcpu,
                                    has_min_exec_count, min_exec_count, errp);
    if (!list) {
        return NULL;
    }

    g_string_append_printf(buf, "%-18s %-12s %-8s %-10s %-8s %-10s %-6s\n",
                           "Guest PC", "Exec Count", "Size", "Host Size", "Insns", "Flags", "vCPU");

    for (curr = list; curr; curr = curr->next) {
        TCGHotBlock *info = curr->value;
        g_string_append_printf(buf, "0x%016"PRIx64" %-12"PRIu32" %-8"PRIu16" %-10"PRIu64" %-8"PRIu16" 0x%08"PRIx32" %-6"PRId64"\n",
                               info->pc, info->exec_count, info->size, info->host_size,
                               info->num_insns, info->flags, info->vcpu);
    }

    qapi_free_TCGHotBlockList(list);
    return human_readable_text_from_str(buf);
}

static HumanReadableText *hmp_info_tcg_hot_blocks(Error **errp)
{
    return qmp_x_query_tcg_hot_blocks(false, 0, false, 0, false, 0, errp);
}

static void hmp_tcg_register(void)
{
    monitor_register_hmp_info_hrt("jit", qmp_x_query_jit);
    monitor_register_hmp_info_hrt("tcg-hot-blocks", hmp_info_tcg_hot_blocks);
}

type_init(hmp_tcg_register);
