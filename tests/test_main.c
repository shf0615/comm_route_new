#include "unity.h"

/* 外部测试函数声明 */
extern void test_create_instance_with_static_buffer(void);
extern void test_init_fails_with_insufficient_buffer(void);
extern void test_unicast_send_single_frame(void);
extern void test_send_queue_full(void);
extern void test_broadcast_single_frame(void);
extern void test_broadcast_exceeds_mtu(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_instance_with_static_buffer);
    RUN_TEST(test_init_fails_with_insufficient_buffer);
    RUN_TEST(test_unicast_send_single_frame);
    RUN_TEST(test_send_queue_full);
    RUN_TEST(test_broadcast_single_frame);
    RUN_TEST(test_broadcast_exceeds_mtu);
    return UNITY_END();
}
