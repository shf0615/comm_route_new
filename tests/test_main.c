#include "unity.h"

/* 外部测试函数声明 */
extern void test_create_instance_with_static_buffer(void);
extern void test_init_fails_with_insufficient_buffer(void);
extern void test_unicast_send_single_frame(void);
extern void test_send_queue_full(void);
extern void test_long_data_segmentation(void);
extern void test_frame_interval_pacing(void);
extern void test_ack_disabled_fire_and_forget(void);
extern void test_long_data_complete_callback_ack_off(void);
extern void test_ack_reply_normal(void);
extern void test_ack_timeout_retransmit(void);
extern void test_ack_max_retries_fail(void);
extern void test_receiver_auto_sends_ack(void);
extern void test_broadcast_single_frame(void);
extern void test_broadcast_exceeds_mtu(void);
extern void test_receive_broadcast_and_forward(void);
extern void test_broadcast_ttl_zero_no_forward(void);
extern void test_broadcast_dedup(void);
extern void test_receive_unicast_single_frame(void);
extern void test_forward_frame_to_next_hop(void);
extern void test_drop_frame_no_route(void);
extern void test_receive_multi_frame_assembly(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_instance_with_static_buffer);
    RUN_TEST(test_init_fails_with_insufficient_buffer);
    RUN_TEST(test_unicast_send_single_frame);
    RUN_TEST(test_send_queue_full);
    RUN_TEST(test_long_data_segmentation);
    RUN_TEST(test_frame_interval_pacing);
    RUN_TEST(test_ack_disabled_fire_and_forget);
    RUN_TEST(test_long_data_complete_callback_ack_off);
    RUN_TEST(test_ack_reply_normal);
    RUN_TEST(test_ack_timeout_retransmit);
    RUN_TEST(test_ack_max_retries_fail);
    RUN_TEST(test_receiver_auto_sends_ack);
    RUN_TEST(test_broadcast_single_frame);
    RUN_TEST(test_broadcast_exceeds_mtu);
    RUN_TEST(test_receive_broadcast_and_forward);
    RUN_TEST(test_broadcast_ttl_zero_no_forward);
    RUN_TEST(test_broadcast_dedup);
    RUN_TEST(test_receive_unicast_single_frame);
    RUN_TEST(test_forward_frame_to_next_hop);
    RUN_TEST(test_drop_frame_no_route);
    RUN_TEST(test_receive_multi_frame_assembly);
    return UNITY_END();
}
