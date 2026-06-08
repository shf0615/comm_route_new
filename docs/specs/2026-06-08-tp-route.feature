# language: zh-CN
Feature: 实例管理
  一份软件可拥有多个独立实例，每个实例有自己的配置和静态内存池

  Scenario: 创建实例并绑定静态内存池
    Given 用户提供一块 1024 字节的 buffer
    And 用户提供实例配置（本机地址=0x01, MTU=64, 帧间隔=5ms, 最大重传=3, ACK超时=100ms）
    When 用户调用初始化函数创建实例
    Then 实例创建成功
    And 实例的内存全部来自用户提供的 buffer

  Scenario: 多实例独立运行
    Given 已创建实例A（地址=0x01, buffer=1024字节）
    And 已创建实例B（地址=0x02, buffer=512字节）
    When 实例A发送数据
    Then 实例A不会使用实例B的内存
    And 实例B的状态不受影响

# language: zh-CN
Feature: 单帧发送与路由
  支持单播，中间节点透传转发，静态路由表

  Scenario: 单播发送单帧
    Given 实例配置本机地址=0x01
    And 路由表配置：目标0x03, 下一跳=0x02
    When 用户发送单帧数据 payload="HELLO" 目标=0x03
    Then 帧通过硬件接口发出
    And 帧头包含源地址=0x01, 目标地址=0x03

  Scenario: 中间节点透传转发
    Given 实例配置本机地址=0x02
    And 路由表配置：目标0x03, 下一跳=0x03（直连）
    When 从硬件接口收到一帧（源=0x01, 目标=0x03）
    Then 实例查路由表找到下一跳=0x03
    And 将该帧原样转发到下一跳

  Scenario: 收到发给自己的帧
    Given 实例配置本机地址=0x02
    When 从硬件接口收到一帧（源=0x01, 目标=0x02）
    Then 实例将该帧交给上层处理（不转发）

# language: zh-CN
Feature: 广播发送与转发
  广播仅支持单帧，通过 TTL 和去重机制防止无限传播

  Scenario: 发送广播帧
    Given 实例配置本机地址=0x01, 默认TTL=3
    When 用户发送广播帧 payload="BCAST"
    Then 帧通过硬件接口发出
    And 帧头目标地址为广播地址(0xFF), TTL=3, 源地址=0x01

  Scenario: 中间节点转发广播帧（TTL 递减）
    Given 实例配置本机地址=0x02
    When 收到广播帧（源=0x01, TTL=2）
    Then 实例将该帧交给上层处理
    And TTL 减 1 后转发（TTL=1）
    And 记录该帧的（源地址+序号）用于去重

  Scenario: TTL 耗尽停止转发
    Given 实例配置本机地址=0x03
    When 收到广播帧（源=0x01, TTL=0）
    Then 实例将该帧交给上层处理
    And 不再转发该帧

  Scenario: 去重 - 丢弃已见过的广播帧
    Given 实例配置本机地址=0x02
    And 已记录来自 0x01 的序号=5 的广播帧
    When 再次收到同一广播帧（源=0x01, 序号=5）
    Then 丢弃该帧（不处理、不转发）

  Scenario: 广播与单播并行
    Given 有一个正在发送的单播长数据任务（3帧，发到第2帧）
    When 用户提交一个广播单帧发送请求
    Then 广播帧可以在下一次 poll 中发出
    And 单播长数据任务继续推进不受影响

# language: zh-CN
Feature: 长数据拆帧与 Poll 发送
  长数据拆成多帧，在 poll 中逐帧发送，可设置帧间隔

  Scenario: 长数据拆帧发送
    Given 实例配置 MTU=8, 帧间隔=10ms
    When 用户提交 20 字节的长数据发送请求（目标=0x03）
    Then 数据被拆为 3 帧（8+8+4 字节）

  Scenario: Poll 中逐帧发送
    Given 有一个待发送的 3 帧长数据任务
    And 帧间隔=10ms
    When 第一次调用 poll 且距上次发送≥10ms
    Then 发送第 1 帧
    When 再次调用 poll 且距上次发送≥10ms
    Then 发送第 2 帧

  Scenario: 发送完成回调
    Given 有一个 3 帧长数据任务且 ACK=开启
    When 所有帧发送完成且 ACK 全部确认
    Then 触发发送完成回调，通知用户该长数据发送成功

  Scenario: 发送完成回调（ACK关闭）
    Given 有一个 3 帧长数据任务且 ACK=关闭
    When 所有帧已发出
    Then 触发发送完成回调，通知用户该长数据发送完成

# language: zh-CN
Feature: ACK 与重传
  单帧级别端到端 ACK，可配置开关，实例级配置重传参数

  Scenario: ACK 开启 - 正常确认
    Given 实例配置 ACK=开启, 超时=100ms, 最大重传=3
    When 发送一帧到目标 0x03
    And 在 100ms 内收到来自 0x03 的 ACK
    Then 该帧标记为发送成功

  Scenario: ACK 开启 - 超时重传
    Given 实例配置 ACK=开启, 超时=100ms, 最大重传=3
    When 发送一帧到目标 0x03
    And 100ms 内未收到 ACK
    Then 自动重传该帧
    And 重传计数+1

  Scenario: ACK 开启 - 达到最大重传次数
    Given 实例配置 ACK=开启, 超时=100ms, 最大重传=3
    When 一帧已重传 3 次仍未收到 ACK
    Then 该帧标记为发送失败
    And 通知上层（如果是长数据的一部分，整个长数据标记失败）

  Scenario: ACK 关闭 - 发完即忘
    Given 实例配置 ACK=关闭
    When 发送一帧到目标 0x03
    Then 帧发出后立即标记为发送成功（不等待ACK）

  Scenario: ACK 通过中断方式确认
    Given 硬件接口的 ACK 方式配置为"中断"
    When 发送一帧后硬件触发发送完成中断
    Then 实例通过中断回调确认该帧发送成功

  Scenario: ACK 通过目标回复确认
    Given 硬件接口的 ACK 方式配置为"目标回复"
    When 发送一帧后在 poll 中收到目标节点的 ACK 回复帧
    Then 实例确认该帧发送成功

# language: zh-CN
Feature: 接收端长数据组装
  库内部自动组装多帧长数据，完成后回调通知用户

  Scenario: 正常接收多帧长数据
    Given 实例配置本机地址=0x02
    When 依次收到来自 0x01 的 3 帧分片（序号 0,1,2，第3帧带末帧标记）
    Then 库内部缓存并组装
    And 收齐后触发接收完成回调，将完整数据交给用户

  Scenario: 接收单帧短数据
    Given 实例配置本机地址=0x02
    When 收到来自 0x01 的单帧数据（非分片标记）
    Then 直接触发接收回调，将数据交给用户

# language: zh-CN
Feature: 拓扑支持
  通过静态路由表支持星型、链式、树形、环形等多种拓扑

  Scenario: 星型拓扑 - 中心节点路由
    Given 中心节点地址=0x01
    And 路由表：0x02→直连, 0x03→直连, 0x04→直连
    When 收到来自 0x02 发给 0x03 的帧
    Then 中心节点转发该帧到 0x03

  Scenario: 链式拓扑 - 逐跳转发
    Given 节点 B 地址=0x02
    And 路由表：0x01→直连, 0x03→直连
    When B 收到来自 0x01 发给 0x03 的帧
    Then B 转发该帧到 0x03（直连）

  Scenario: 环形拓扑 - 无路由则丢弃
    Given 节点 B 地址=0x02
    And 路由表中无到 0x05 的条目
    When B 收到一帧（源=0x01, 目标=0x05）
    Then B 丢弃该帧（无路由，不转发）

# language: zh-CN
Feature: 多跳 ACK 回传
  ACK 帧在多跳场景下能正确经中间节点路由回传到原始发送端

  Scenario: 三节点链式拓扑 - ACK 经中间节点回传
    Given 节点 A 地址=0x01, 路由表：0x03→下一跳0x02
    And 节点 B 地址=0x02, 路由表：0x01→直连, 0x03→直连
    And 节点 C 地址=0x03, 路由表：0x01→下一跳0x02
    When A 通过 B 发送数据帧到 C
    And C 收到数据帧后自动发送 ACK（DST=0x01, SRC=0x03）
    Then B 收到该 ACK 帧，DST=0x01≠本机，查路由表转发到 A
    And A 收到 ACK 帧，DST=0x01==本机，匹配 SEQ 标记成功

  Scenario: 中间节点不消费非自己的 ACK 帧
    Given 节点 B 地址=0x02
    And B 有一个活跃发送任务（SEQ=5, 目标=0x04）
    When B 收到 ACK 帧（DST=0x01, SRC=0x03, SEQ=5）
    Then B 不匹配该 ACK（DST≠本机），按路由表转发

# language: zh-CN
Feature: 错误处理
  API 返回错误码的边界场景

  Scenario: 发送队列满时返回错误
    Given 实例配置 tx_queue_depth=2
    And 队列中已有 2 个待发送任务
    When 用户再次调用 cr_send 提交发送请求
    Then 返回 -1（队列满）

  Scenario: 广播数据超过 MTU 时返回错误
    Given 实例配置 MTU=8
    When 用户调用 cr_broadcast 发送 16 字节数据
    Then 返回 -2（参数错误，广播仅支持单帧）

  Scenario: Buffer 不足时初始化失败
    Given 用户提供一块 32 字节的 buffer（小于所需最小值）
    When 用户调用 cr_init 初始化实例
    Then 返回 -3（buffer 不足）

# language: zh-CN
Feature: 接收组装异常处理
  接收长数据组装的异常和边界场景

  Scenario: 组装超时释放槽位
    Given 实例配置本机地址=0x02, ACK超时=100ms, 最大重传=3
    And 正在组装来自 0x01 的长数据（已收到序号 0,1）
    When 超过组装超时时间仍未收到后续帧
    Then 释放该 RX 组装槽位
    And 已缓存的不完整数据被丢弃

  Scenario: 组装槽满时丢弃新长数据首帧
    Given 实例配置 rx_assem_count=2
    And 2 个组装槽均被占用（分别组装来自 0x01 和 0x03 的数据）
    When 收到来自 0x04 的新长数据首帧（序号=0）
    Then 该帧被丢弃（无可用槽位）

  Scenario: 重复帧不重复写入
    Given 正在组装来自 0x01 的长数据，已收到序号 0,1
    When 再次收到来自 0x01 的序号=1 的帧（重传帧）
    Then 不重复写入组装缓冲区
    And 仍发送 ACK 确认
