# language: zh-CN
Feature: 帧格式有效数据长度字段
  帧头新增 LEN 字段（1字节，偏移5），指示有效载荷长度。
  用于底层硬件固定长度传输场景，接收端据此截取有效数据。

  Background:
    Given 帧头格式为 DST|SRC|CTL|SEQ|TTL|LEN（共6字节）

  Scenario: 发送端自动填充 LEN 字段
    Given 实例配置本机地址=0x01
    When 用户发送 5 字节 payload 到目标 0x03
    Then 发出的帧 frame[5]=5（LEN=有效载荷长度）
    And 帧头大小为 6 字节

  Scenario: 接收端根据 LEN 截取有效数据
    Given 实例配置本机地址=0x02
    When 收到一帧（底层传入总长=64, frame[5]=5）
    Then 实例取 LEN=5 作为有效载荷长度
    And 回调交付的数据长度为 5（非 64-6=58）

  Scenario: 长数据拆帧 - 每帧各自填充 LEN
    Given 实例配置 MTU=16（帧总大小=16, 单帧最大载荷=16-6=10）
    When 用户提交 25 字节的长数据发送请求
    Then 拆为 3 帧：第1帧 LEN=10, 第2帧 LEN=10, 第3帧 LEN=5

  Scenario: 长数据组装 - 按各帧 LEN 累计
    Given 实例配置本机地址=0x02
    When 依次收到 3 帧分片（LEN=10, LEN=10, LEN=5，第3帧带末帧标记）
    Then 组装后总有效数据长度为 25 字节

  Scenario: ACK 帧 LEN=0
    Given 实例配置 ACK=开启
    When 收到数据帧后发送 ACK 回复
    Then ACK 帧的 LEN=0（ACK 帧无有效载荷）

  Scenario: 广播帧携带 LEN
    Given 实例配置本机地址=0x01
    When 用户发送广播帧 payload="HI"（2字节）
    Then 发出的帧 frame[5]=2
    And 帧头目标地址为 0xFF

  Scenario: LEN=0 的数据帧
    Given 实例配置本机地址=0x02
    When 收到一帧（frame[5]=0, 非ACK帧）
    Then 回调交付的数据长度为 0
