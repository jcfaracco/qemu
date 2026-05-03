/*
 * TCG Hot Code Detection Test
 *
 * Copyright (c) 2026 Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

static void test_query_tcg_hot_blocks(void)
{
    QTestState *qts;
    QDict *resp;
    QList *list;

    qts = qtest_init("-machine none -accel tcg");

    resp = qtest_qmp(qts, "{ 'execute': 'query-tcg-hot-blocks', 'arguments': { 'top-n': 10 } }");
    g_assert(qdict_haskey(resp, "return"));
    list = qdict_get_qlist(resp, "return");
    g_assert(list);
    qobject_unref(resp);

    qtest_quit(qts);
}

static void test_query_tcg_hot_blocks_threshold(void)
{
    QTestState *qts;
    QDict *resp;

    qts = qtest_init("-machine none -accel tcg");

    resp = qtest_qmp(qts, "{ 'execute': 'query-tcg-hot-blocks', 'arguments': { 'min-exec-count': 100 } }");
    g_assert(qdict_haskey(resp, "return"));
    qobject_unref(resp);

    qtest_quit(qts);
}

static void test_query_tcg_hot_blocks_invalid_vcpu(void)
{
    QTestState *qts;
    QDict *resp;

    qts = qtest_init("-machine none -accel tcg");

    /* vCPU 999 should be invalid for -machine none */
    resp = qtest_qmp(qts, "{ 'execute': 'query-tcg-hot-blocks', 'arguments': { 'vcpu': 999 } }");
    g_assert(qdict_haskey(resp, "error"));
    qobject_unref(resp);

    qtest_quit(qts);
}

static void test_query_tcg_hot_blocks_vcpu_field(void)
{
    QTestState *qts;
    QDict *resp;
    QList *list;
    QObject *obj;

    qts = qtest_init("-machine none -accel tcg");

    resp = qtest_qmp(qts, "{ 'execute': 'query-tcg-hot-blocks' }");
    g_assert(qdict_haskey(resp, "return"));
    list = qdict_get_qlist(resp, "return");

    if (!qlist_empty(list)) {
        obj = qlist_peek(list);
        g_assert(qdict_haskey(qobject_to(QDict, obj), "vcpu"));
    }

    qobject_unref(resp);
    qtest_quit(qts);
}

static void test_query_tcg_hot_blocks_filter_vcpu(void)
{
    QTestState *qts;
    QDict *resp;
    QList *list;
    QListEntry *entry;

    qts = qtest_init("-machine none -accel tcg -smp 2");

    resp = qtest_qmp(qts, "{ 'execute': 'query-tcg-hot-blocks', 'arguments': { 'vcpu': 1 } }");
    g_assert(qdict_haskey(resp, "return"));
    list = qdict_get_qlist(resp, "return");

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *block = qobject_to(QDict, entry->value);
        g_assert_cmpint(qdict_get_int(block, "vcpu"), ==, 1);
    }

    qobject_unref(resp);
    qtest_quit(qts);
}

static void test_tcg_hotcode_config(void)
{
    QTestState *qts;

    qts = qtest_init("-machine none -accel tcg,cold-threshold=20,min-reclaim-size=2M");
    /* If it starts, the properties are recognized */
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/tcg/query-tcg-hot-blocks", test_query_tcg_hot_blocks);
    qtest_add_func("/tcg/query-tcg-hot-blocks-threshold", test_query_tcg_hot_blocks_threshold);
    qtest_add_func("/tcg/query-tcg-hot-blocks-invalid-vcpu", test_query_tcg_hot_blocks_invalid_vcpu);
    qtest_add_func("/tcg/query-tcg-hot-blocks-vcpu-field", test_query_tcg_hot_blocks_vcpu_field);
    qtest_add_func("/tcg/query-tcg-hot-blocks-filter-vcpu", test_query_tcg_hot_blocks_filter_vcpu);
    qtest_add_func("/tcg/tcg-hotcode-config", test_tcg_hotcode_config);

    return g_test_run();
}
