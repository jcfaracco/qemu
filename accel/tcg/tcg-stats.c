/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  QEMU TCG statistics
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 */

#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "qemu/qht.h"
#include "qapi/error.h"
#include "system/cpu-timers.h"
#include "exec/icount.h"
#include "hw/core/cpu.h"
#include "tcg/tcg.h"
#include "internal-common.h"
#include "tb-context.h"
#include <math.h>

static void dump_drift_info(GString *buf)
{
    if (!icount_enabled()) {
        return;
    }

    g_string_append_printf(buf, "Host - Guest clock  %"PRIi64" ms\n",
                           (cpu_get_clock() - icount_get()) / SCALE_MS);
    if (icount_align_option) {
        g_string_append_printf(buf, "Max guest delay     %"PRIi64" ms\n",
                               -max_delay / SCALE_MS);
        g_string_append_printf(buf, "Max guest advance   %"PRIi64" ms\n",
                               max_advance / SCALE_MS);
    } else {
        g_string_append_printf(buf, "Max guest delay     NA\n");
        g_string_append_printf(buf, "Max guest advance   NA\n");
    }
}

static void dump_accel_info(AccelState *accel, GString *buf)
{
    bool one_insn_per_tb = object_property_get_bool(OBJECT(accel),
                                                    "one-insn-per-tb",
                                                    &error_fatal);

    g_string_append_printf(buf, "Accelerator settings:\n");
    g_string_append_printf(buf, "one-insn-per-tb: %s\n\n",
                           one_insn_per_tb ? "on" : "off");
}

static void print_qht_statistics(struct qht_stats hst, GString *buf)
{
    uint32_t hgram_opts;
    size_t hgram_bins;
    char *hgram;
    double avg;

    if (!hst.head_buckets) {
        return;
    }
    g_string_append_printf(buf, "TB hash buckets     %zu/%zu "
                           "(%0.2f%% head buckets used)\n",
                           hst.used_head_buckets, hst.head_buckets,
                           (double)hst.used_head_buckets /
                           hst.head_buckets * 100);

    hgram_opts =  QDIST_PR_BORDER | QDIST_PR_LABELS;
    hgram_opts |= QDIST_PR_100X   | QDIST_PR_PERCENT;
    if (qdist_xmax(&hst.occupancy) - qdist_xmin(&hst.occupancy) == 1) {
        hgram_opts |= QDIST_PR_NODECIMAL;
    }
    hgram = qdist_pr(&hst.occupancy, 10, hgram_opts);
    avg = qdist_avg(&hst.occupancy);
    if (!isnan(avg)) {
        g_string_append_printf(buf, "TB hash occupancy   "
                                    "%0.2f%% avg chain occ. "
                                    "Histogram: %s\n",
                               avg * 100, hgram);
    }
    g_free(hgram);

    hgram_opts = QDIST_PR_BORDER | QDIST_PR_LABELS;
    hgram_bins = qdist_xmax(&hst.chain) - qdist_xmin(&hst.chain);
    if (hgram_bins > 10) {
        hgram_bins = 10;
    } else {
        hgram_bins = 0;
        hgram_opts |= QDIST_PR_NODECIMAL | QDIST_PR_NOBINRANGE;
    }
    hgram = qdist_pr(&hst.chain, hgram_bins, hgram_opts);
    avg = qdist_avg(&hst.chain);
    if (!isnan(avg)) {
        g_string_append_printf(buf, "TB hash avg chain   %0.3f buckets. "
                               "Histogram: %s\n",
                               avg, hgram);
    }
    g_free(hgram);
}

struct tb_tree_stats {
    size_t nb_tbs;
    size_t host_size;
    size_t target_size;
    size_t max_target_size;
    size_t direct_jmp_count;
    size_t direct_jmp2_count;
    size_t cross_page;
    uint64_t exec_dist[5];
    GArray *exec_counts;
};

static gint compare_uint32(gconstpointer a, gconstpointer b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;

    if (va > vb) {
        return -1;
    } else if (va < vb) {
        return 1;
    }
    return 0;
}

static gboolean tb_tree_stats_iter(gpointer key, gpointer value, gpointer data)
{
    const TranslationBlock *tb = value;
    struct tb_tree_stats *tst = data;

    tst->nb_tbs++;
    tst->host_size += tb->tc.size;
    tst->target_size += tb->size;
    if (tb->size > tst->max_target_size) {
        tst->max_target_size = tb->size;
    }
#ifndef CONFIG_USER_ONLY
    if (tb->page_addr[1] != -1) {
        tst->cross_page++;
    }
#endif
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        tst->direct_jmp_count++;
        if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
            tst->direct_jmp2_count++;
        }
    }

    if (tb->exec_count <= 1) {
        tst->exec_dist[0]++;
    } else if (tb->exec_count <= 10) {
        tst->exec_dist[1]++;
    } else if (tb->exec_count <= 100) {
        tst->exec_dist[2]++;
    } else if (tb->exec_count <= 1000) {
        tst->exec_dist[3]++;
    } else {
        tst->exec_dist[4]++;
    }

    if (tst->exec_counts) {
        g_array_append_val(tst->exec_counts, tb->exec_count);
    }

    return false;
}

static void tlb_flush_counts(size_t *pfull, size_t *ppart, size_t *pelide)
{
    CPUState *cpu;
    size_t full = 0, part = 0, elide = 0;

    CPU_FOREACH(cpu) {
        full += qatomic_read(&cpu->neg.tlb.c.full_flush_count);
        part += qatomic_read(&cpu->neg.tlb.c.part_flush_count);
        elide += qatomic_read(&cpu->neg.tlb.c.elide_flush_count);
    }
    *pfull = full;
    *ppart = part;
    *pelide = elide;
}

static void tcg_dump_flush_info(GString *buf)
{
    size_t flush_full, flush_part, flush_elide;

    g_string_append_printf(buf, "TB flush count      %u\n",
                           qatomic_read(&tb_ctx.tb_flush_count));
    g_string_append_printf(buf, "TB invalidate count %u\n",
                           qatomic_read(&tb_ctx.tb_phys_invalidate_count));

    tlb_flush_counts(&flush_full, &flush_part, &flush_elide);
    g_string_append_printf(buf, "TLB full flushes    %zu\n", flush_full);
    g_string_append_printf(buf, "TLB partial flushes %zu\n", flush_part);
    g_string_append_printf(buf, "TLB elided flushes  %zu\n", flush_elide);
}

static void dump_exec_info(GString *buf)
{
    struct tb_tree_stats tst = {};
    struct qht_stats hst;
    size_t nb_tbs;

    nb_tbs = tcg_nb_tbs();
    if (nb_tbs) {
        tst.exec_counts = g_array_sized_new(FALSE, FALSE, sizeof(uint32_t),
                                           nb_tbs);
    }

    tcg_tb_foreach(tb_tree_stats_iter, &tst);
    nb_tbs = tst.nb_tbs;
    /* XXX: avoid using doubles ? */
    g_string_append_printf(buf, "Translation buffer state:\n");
    /*
     * Report total code size including the padding and TB structs;
     * otherwise users might think "-accel tcg,tb-size" is not honoured.
     * For avg host size we use the precise numbers from tb_tree_stats though.
     */
    g_string_append_printf(buf, "gen code size       %zu/%zu\n",
                           tcg_code_size(), tcg_code_capacity());
    g_string_append_printf(buf, "TB count            %zu\n", nb_tbs);
    g_string_append_printf(buf, "TB avg target size  %zu max=%zu bytes\n",
                           nb_tbs ? tst.target_size / nb_tbs : 0,
                           tst.max_target_size);
    g_string_append_printf(buf, "TB avg host size    %zu bytes "
                           "(expansion ratio: %0.1f)\n",
                           nb_tbs ? tst.host_size / nb_tbs : 0,
                           tst.target_size ?
                           (double)tst.host_size / tst.target_size : 0);
    g_string_append_printf(buf, "cross page TB count %zu (%zu%%)\n",
                           tst.cross_page,
                           nb_tbs ? (tst.cross_page * 100) / nb_tbs : 0);
    g_string_append_printf(buf, "direct jump count   %zu (%zu%%) "
                           "(2 jumps=%zu %zu%%)\n",
                           tst.direct_jmp_count,
                           nb_tbs ? (tst.direct_jmp_count * 100) / nb_tbs : 0,
                           tst.direct_jmp2_count,
                           nb_tbs ? (tst.direct_jmp2_count * 100) / nb_tbs : 0);

    g_string_append_printf(buf, "Execution distribution:\n");
    g_string_append_printf(buf, "  1x:                %"PRIu64"\n", tst.exec_dist[0]);
    g_string_append_printf(buf, "  2-10x:             %"PRIu64"\n", tst.exec_dist[1]);
    g_string_append_printf(buf, "  11-100x:           %"PRIu64"\n", tst.exec_dist[2]);
    g_string_append_printf(buf, "  101-1000x:         %"PRIu64"\n", tst.exec_dist[3]);
    g_string_append_printf(buf, "  1001x+:            %"PRIu64"\n", tst.exec_dist[4]);

    if (tst.exec_counts) {
        uint64_t total_exec = 0;
        uint64_t top10_exec = 0;
        size_t top10_count = nb_tbs / 10;
        size_t i;

        g_array_sort(tst.exec_counts, compare_uint32);
        for (i = 0; i < tst.exec_counts->len; i++) {
            uint32_t count = g_array_index(tst.exec_counts, uint32_t, i);
            total_exec += count;
            if (i < top10_count) {
                top10_exec += count;
            }
        }
        g_string_append_printf(buf, "Hot TB ratio (top 10%%): %0.1f%%\n",
                               total_exec ? (double)top10_exec * 100 / total_exec : 0.0);
        g_array_free(tst.exec_counts, TRUE);
    }

    qht_statistics_init(&tb_ctx.htable, &hst);
    print_qht_statistics(hst, buf);
    qht_statistics_destroy(&hst);

    g_string_append_printf(buf, "\nStatistics:\n");
    tcg_dump_flush_info(buf);
}

void tcg_get_stats(AccelState *accel, GString *buf)
{
    dump_accel_info(accel, buf);
    dump_exec_info(buf);
    dump_drift_info(buf);
}

void tcg_dump_stats(GString *buf)
{
    tcg_get_stats(current_accel(), buf);
}
