#include "unity.h"

/* 外部测试函数声明 */
extern void test_create_instance_with_static_buffer(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_create_instance_with_static_buffer);
    return UNITY_END();
}
