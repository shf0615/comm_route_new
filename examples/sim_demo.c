/*
 * TP+路由库 综合模拟示例
 *
 * 在单进程中模拟多个节点，演示：
 *   场景1: 星型拓扑 — 单播(经中心转发) + 广播
 *   场景2: 链式拓扑 — 长数据多跳转发 + 广播(TTL递减)
 *
 * 编译: cd build && make sim_demo && ./sim_demo
 */

#include "comm_route.h"
#include <stdio.h>
#include <string.h>

/* ===== 颜色 ===== */
#define RST "\033[0m"
#define RED "\033[31m"
#define GRN "\033[32m"
#define YEL "\033[33m"
#define BLU "\033[34m"
#define MAG "\033[35m"
#define CYN "\033[36m"

/* ===== 虚拟网络 ===== */
#define MAX_NODES    8
#define INBOX_CAP    64
#define FRAME_MAX    128

typedef struct { uint8_t data[FRAME_MAX]; uint16_t len; } vframe_t;
typedef struct { vframe_t q[INBOX_CAP]; uint8_t h, t, n; } inbox_t;

static void inbox_put(inbox_t *b, const uint8_t *d, uint16_t l) {
    if (b->n >= INBOX_CAP) return;
    memcpy(b->q[b->t].data, d, l); b->q[b->t].len = l;
    b->t = (b->t + 1) % INBOX_CAP; b->n++;
}
static int inbox_get(inbox_t *b, uint8_t *d, uint16_t *l) {
    if (!b->n) return 0;
    memcpy(d, b->q[b->h].data, b->q[b->h].len);
    *l = b->q[b->h].len;
    b->h = (b->h + 1) % INBOX_CAP; b->n--;
    return 1;
}

/* ===== 节点 ===== */
typedef struct sim_node {
    uint8_t       addr;
    const char   *name;
    const char   *color;
    cr_instance_t inst;
    cr_hal_t      hal;
    uint8_t       buf[4096];
    inbox_t       inbox;
    /* 点对点连接: 每条链路指向对端节点指针 */
    struct sim_node *links[MAX_NODES];
    uint8_t       link_count;
} node_t;

static node_t  g_nodes[MAX_NODES];
static uint8_t g_nn;
static uint32_t g_tick;

static node_t *node_by_addr(uint8_t a) {
    for (uint8_t i = 0; i < g_nn; i++) if (g_nodes[i].addr == a) return &g_nodes[i];
    return NULL;
}

/* HAL send: 利用 next_hop 精确投递到对应邻居 */
static int hal_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    node_t *s = (node_t *)ctx;
    uint8_t ctl = data[2];
    uint8_t dst = data[0];
    const char *tp = (ctl & 0x80) ? "ACK" : (ctl & 0x40) ? "BCAST" : (ctl & 0x20) ? "FRAG" : "DATA";
    printf("    %s[%s]%s TX %-5s dst=0x%02X seq=%d", s->color, s->name, RST, tp, dst, data[3]);
    if (!(ctl & 0x80) && len > 5) {
        printf(" \"");
        for (uint16_t i = 5; i < len && i < 20; i++)
            printf("%c", (data[i] >= 0x20 && data[i] <= 0x7E) ? data[i] : '.');
        if (len > 20) printf("...");
        printf("\"");
    }
    printf("\n");

    if (next_hop == 0xFF) {
        /* 广播：投递给所有邻居 */
        for (uint8_t i = 0; i < s->link_count; i++)
            inbox_put(&s->links[i]->inbox, data, len);
    } else {
        /* 单播：投递给 next_hop 对应的邻居 */
        for (uint8_t i = 0; i < s->link_count; i++) {
            if (s->links[i]->addr == next_hop) {
                inbox_put(&s->links[i]->inbox, data, len);
                return 0;
            }
        }
    }
    return 0;
}
static uint32_t hal_tick(void) { return g_tick; }

static void on_recv(cr_instance_t *inst, uint8_t src, uint8_t biz,
                    const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst;
    node_t *n = (node_t *)ctx;
    printf("    %s[%s]%s " GRN "RX" RST " src=0x%02X biz=%d len=%d \"", n->color, n->name, RST, src, biz, len);
    for (uint16_t i = 0; i < len && i < 40; i++)
        printf("%c", (data[i] >= 0x20 && data[i] <= 0x7E) ? data[i] : '.');
    if (len > 40) printf("...");
    printf("\"\n");
}

static void on_done(uint8_t st, void *ctx) {
    printf("    %s%s%s\n", st ? RED : GRN, st ? "✗ SEND FAILED" : "✓ SEND COMPLETE", RST);
}

static void node_init(node_t *n, uint8_t addr, const char *name, const char *color,
                      const cr_route_entry_t *rt, uint8_t rc) {
    memset(n, 0, sizeof(*n));
    n->addr = addr; n->name = name; n->color = color;
    cr_config_t cfg = {
        .local_addr = addr, .mtu = 21, .frame_interval_ms = 10,
        .max_retries = 3, .ack_timeout_ms = 200, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 4,
        .tx_queue_depth = 8, .rx_assem_count = 4, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = rt, .route_count = rc,
    };
    cr_init(&n->inst, &cfg, n->buf, sizeof(n->buf));
    n->hal.send = hal_send; n->hal.get_tick_ms = hal_tick; n->hal.hw_ctx = n;
    cr_set_hal(&n->inst, &n->hal);
    cr_set_recv_callback(&n->inst, on_recv, n);
}

/* 建立双向点对点链路 */
static void link(node_t *a, node_t *b) {
    a->links[a->link_count++] = b;
    b->links[b->link_count++] = a;
}

/* 模拟一个 tick（10ms） */
static void tick(void) {
    for (uint8_t i = 0; i < g_nn; i++) cr_poll(&g_nodes[i].inst);
    for (uint8_t i = 0; i < g_nn; i++) {
        uint8_t f[FRAME_MAX]; uint16_t l;
        while (inbox_get(&g_nodes[i].inbox, f, &l))
            cr_feed_frame(&g_nodes[i].inst, f, l);
    }
    g_tick += 10;
}

/* 运行 N 个 tick */
static void run(int n) { for (int i = 0; i < n; i++) tick(); }

/* ================================================================ */
/*  场景 1: 星型拓扑                                                */
/*       B(0x02)                                                    */
/*       |                                                          */
/*  A ── Hub(0x10) ── C(0x03)                                       */
/*       |                                                          */
/*       D(0x04)                                                    */
/* ================================================================ */
static void demo_star(void) {
    printf(YEL "\n╔══════════════════════════════════════════╗\n"
               "║  场景1: 星型拓扑 (Hub 中心转发)          ║\n"
               "╚══════════════════════════════════════════╝\n" RST);

    g_nn = 0; g_tick = 0;

    /* Hub 知道如何到达所有叶子 (直连) */
    static const cr_route_entry_t rt_hub[] = {{0x01,0x01},{0x02,0x02},{0x03,0x03},{0x04,0x04}};
    /* 叶子到其他叶子都经 Hub */
    static const cr_route_entry_t rt_a[] = {{0x02,0x10},{0x03,0x10},{0x04,0x10},{0x10,0x10}};
    static const cr_route_entry_t rt_b[] = {{0x01,0x10},{0x03,0x10},{0x04,0x10},{0x10,0x10}};
    static const cr_route_entry_t rt_c[] = {{0x01,0x10},{0x02,0x10},{0x04,0x10},{0x10,0x10}};
    static const cr_route_entry_t rt_d[] = {{0x01,0x10},{0x02,0x10},{0x03,0x10},{0x10,0x10}};

    node_init(&g_nodes[0], 0x10, "Hub", MAG, rt_hub, 4); /* Hub 索引 0 */
    node_init(&g_nodes[1], 0x01, "A  ", CYN, rt_a, 4);
    node_init(&g_nodes[2], 0x02, "B  ", GRN, rt_b, 4);
    node_init(&g_nodes[3], 0x03, "C  ", BLU, rt_c, 4);
    node_init(&g_nodes[4], 0x04, "D  ", YEL, rt_d, 4);
    g_nn = 5;

    /* 星型链路: 每个叶子只和 Hub 相连 */
    link(&g_nodes[0], &g_nodes[1]); /* Hub ↔ A */
    link(&g_nodes[0], &g_nodes[2]); /* Hub ↔ B */
    link(&g_nodes[0], &g_nodes[3]); /* Hub ↔ C */
    link(&g_nodes[0], &g_nodes[4]); /* Hub ↔ D */

    /* Demo 1a: A → C 单播 */
    printf("\n" CYN "── A 发单帧给 C (经 Hub 转发, ACK 回传) ──" RST "\n");
    cr_send(&g_nodes[1].inst, 0x03, 1, (const uint8_t *)"Hello C!", 8, on_done, NULL);
    run(30);

    /* Demo 1b: Hub 广播 */
    printf("\n" MAG "── Hub 广播 (所有叶子接收) ──" RST "\n");
    cr_broadcast(&g_nodes[0].inst, 0, (const uint8_t *)"PING", 4);
    run(10);
}

/* ================================================================ */
/*  场景 2: 链式拓扑                                                */
/*  N1(0x01) ── N2(0x02) ── N3(0x03) ── N4(0x04) ── N5(0x05)       */
/* ================================================================ */
static void demo_chain(void) {
    printf(YEL "\n╔══════════════════════════════════════════╗\n"
               "║  场景2: 链式拓扑 (多跳长数据 + 广播)     ║\n"
               "╚══════════════════════════════════════════╝\n" RST);

    g_nn = 0; g_tick = 0;

    /* 路由: 往右走下一跳+1, 往左走下一跳-1 */
    static const cr_route_entry_t rt1[] = {{0x02,0x02},{0x03,0x02},{0x04,0x02},{0x05,0x02}};
    static const cr_route_entry_t rt2[] = {{0x01,0x01},{0x03,0x03},{0x04,0x03},{0x05,0x03}};
    static const cr_route_entry_t rt3[] = {{0x01,0x02},{0x02,0x02},{0x04,0x04},{0x05,0x04}};
    static const cr_route_entry_t rt4[] = {{0x01,0x03},{0x02,0x03},{0x03,0x03},{0x05,0x05}};
    static const cr_route_entry_t rt5[] = {{0x01,0x04},{0x02,0x04},{0x03,0x04},{0x04,0x04}};

    node_init(&g_nodes[0], 0x01, "N1", CYN, rt1, 4);
    node_init(&g_nodes[1], 0x02, "N2", GRN, rt2, 4);
    node_init(&g_nodes[2], 0x03, "N3", BLU, rt3, 4);
    node_init(&g_nodes[3], 0x04, "N4", MAG, rt4, 4);
    node_init(&g_nodes[4], 0x05, "N5", YEL, rt5, 4);
    g_nn = 5;

    /* 链式链路 */
    link(&g_nodes[0], &g_nodes[1]); /* N1 ↔ N2 */
    link(&g_nodes[1], &g_nodes[2]); /* N2 ↔ N3 */
    link(&g_nodes[2], &g_nodes[3]); /* N3 ↔ N4 */
    link(&g_nodes[3], &g_nodes[4]); /* N4 ↔ N5 */

    /* Demo 2a: N1 → N5 长数据 (48字节, MTU=16, 拆3帧, 经 N2→N3→N4 转发) */
    printf("\n" CYN "── N1 发长数据给 N5 (48B, 拆3帧, 跨4跳) ──" RST "\n");
    uint8_t payload[48];
    for (int i = 0; i < 48; i++) payload[i] = (uint8_t)('A' + i % 26);
    cr_send(&g_nodes[0].inst, 0x05, 2, payload, 48, on_done, NULL);
    run(200);

    /* Demo 2b: N3 广播 (TTL=4, 向两侧传播, 去重防止来回反弹) */
    printf("\n" BLU "── N3 广播 (TTL=4, 两侧传播) ──" RST "\n");
    cr_broadcast(&g_nodes[2].inst, 0, (const uint8_t *)"HELLO", 5);
    run(30);
}

/* ===== main ===== */
int main(void) {
    printf(YEL "\n══════════════════════════════════════════\n"
               "   TP+路由库 综合模拟演示\n"
               "══════════════════════════════════════════\n" RST);
    demo_star();
    demo_chain();
    printf(GRN "\n═══ 模拟完成 ═══\n\n" RST);
    return 0;
}
