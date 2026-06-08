#include "unity.h"

/* 外部测试函数声明 */
extern void test_create_instance_with_static_buffer(void);
extern void test_init_fails_with_insufficient_buffer(void);
extern void test_multi_instance_isolation(void);
extern void test_calc_buffer_size(void);
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
extern void test_ack_interrupt_mode(void);
extern void test_long_data_complete_with_ack(void);
extern void test_multihop_ack_routing(void);
extern void test_broadcast_single_frame(void);
extern void test_broadcast_exceeds_mtu(void);
extern void test_receive_broadcast_and_forward(void);
extern void test_broadcast_ttl_zero_no_forward(void);
extern void test_broadcast_dedup(void);
extern void test_broadcast_parallel_with_unicast(void);
extern void test_receive_unicast_single_frame(void);
extern void test_forward_frame_to_next_hop(void);
extern void test_drop_frame_no_route(void);
extern void test_receive_multi_frame_assembly(void);
extern void test_rx_assembly_timeout(void);
extern void test_rx_slot_full_drops_new(void);
extern void test_rx_duplicate_frame_ignored(void);

/* test_extra.c */
extern void test_send_zero_length_data(void);
extern void test_broadcast_zero_length(void);
extern void test_send_exactly_mtu(void);
extern void test_send_mtu_plus_one(void);
extern void test_seq_counter_wraps(void);
extern void test_feed_frame_too_short(void);
extern void test_feed_frame_exactly_header(void);
extern void test_rx_out_of_order_frame_dropped(void);
extern void test_ack_wrong_seq_ignored(void);
extern void test_poll_no_task_no_crash(void);
extern void test_feed_frame_no_hal_set(void);
extern void test_interrupt_mode_no_timeout(void);
extern void test_init_null_instance(void);
extern void test_init_null_config(void);
extern void test_init_null_buffer(void);
extern void test_broadcast_busy_returns_error(void);
extern void test_rx_buffer_overflow_drops_slot(void);
extern void test_multihop_intermediate_no_consume_ack(void);
extern void test_star_topology_routing(void);
extern void test_dedup_table_ring_overwrite(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_instance_with_static_buffer);
    RUN_TEST(test_init_fails_with_insufficient_buffer);
    RUN_TEST(test_multi_instance_isolation);
    RUN_TEST(test_calc_buffer_size);
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
    RUN_TEST(test_ack_interrupt_mode);
    RUN_TEST(test_long_data_complete_with_ack);
    RUN_TEST(test_multihop_ack_routing);
    RUN_TEST(test_broadcast_single_frame);
    RUN_TEST(test_broadcast_exceeds_mtu);
    RUN_TEST(test_receive_broadcast_and_forward);
    RUN_TEST(test_broadcast_ttl_zero_no_forward);
    RUN_TEST(test_broadcast_dedup);
    RUN_TEST(test_broadcast_parallel_with_unicast);
    RUN_TEST(test_receive_unicast_single_frame);
    RUN_TEST(test_forward_frame_to_next_hop);
    RUN_TEST(test_drop_frame_no_route);
    RUN_TEST(test_receive_multi_frame_assembly);
    RUN_TEST(test_rx_assembly_timeout);
    RUN_TEST(test_rx_slot_full_drops_new);
    RUN_TEST(test_rx_duplicate_frame_ignored);
    /* test_extra.c */
    RUN_TEST(test_send_zero_length_data);
    RUN_TEST(test_broadcast_zero_length);
    RUN_TEST(test_send_exactly_mtu);
    RUN_TEST(test_send_mtu_plus_one);
    RUN_TEST(test_seq_counter_wraps);
    RUN_TEST(test_feed_frame_too_short);
    RUN_TEST(test_feed_frame_exactly_header);
    RUN_TEST(test_rx_out_of_order_frame_dropped);
    RUN_TEST(test_ack_wrong_seq_ignored);
    RUN_TEST(test_poll_no_task_no_crash);
    RUN_TEST(test_feed_frame_no_hal_set);
    RUN_TEST(test_interrupt_mode_no_timeout);
    RUN_TEST(test_init_null_instance);
    RUN_TEST(test_init_null_config);
    RUN_TEST(test_init_null_buffer);
    RUN_TEST(test_broadcast_busy_returns_error);
    RUN_TEST(test_rx_buffer_overflow_drops_slot);
    RUN_TEST(test_multihop_intermediate_no_consume_ack);
    RUN_TEST(test_star_topology_routing);
    RUN_TEST(test_dedup_table_ring_overwrite);
    return UNITY_END();
}
