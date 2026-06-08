#include "unity.h"

/* 外部测试函数声明 */
extern void test_create_instance_with_static_buffer(void);
extern void test_init_fails_with_insufficient_buffer(void);
extern void test_unicast_send_single_frame(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_instance_with_static_buffer);
    RUN_TEST(test_init_fails_with_insufficient_buffer);
    RUN_TEST(test_unicast_send_single_frame);
    return UNITY_END();
}
