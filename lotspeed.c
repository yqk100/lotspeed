// lotspeed.c  ——  2025 年的"锐速"复活版 v2.0
// Author: uk0 @ 2025-11-17
// 致敬经典 LotServer/ServerSpeeder，为新时代而生

#include <linux/module.h>
#include <linux/version.h>
#include <net/tcp.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>

// 可调参数（通过 sysfs 动态修改）
static unsigned long lotserver_rate = 1250000000ULL;  // 默认 10Gbps
static unsigned int lotserver_gain = 30;               // 3.0x 默认增益
static unsigned int lotserver_min_cwnd = 50;           // 最小拥塞窗口
static unsigned int lotserver_max_cwnd = 10000;        // 最大拥塞窗口
static bool lotserver_adaptive = true;                 // 自适应模式
static bool lotserver_turbo = false;                   // 涡轮模式

module_param(lotserver_rate, ulong, 0644);
MODULE_PARM_DESC(lotserver_rate, "Target rate in bytes/sec (default 10Gbps)");
module_param(lotserver_gain, uint, 0644);
MODULE_PARM_DESC(lotserver_gain, "Gain multiplier x10 (30 = 3.0x)");
module_param(lotserver_min_cwnd, uint, 0644);
MODULE_PARM_DESC(lotserver_min_cwnd, "Minimum congestion window");
module_param(lotserver_max_cwnd, uint, 0644);
MODULE_PARM_DESC(lotserver_max_cwnd, "Maximum congestion window");
module_param(lotserver_adaptive, bool, 0644);
MODULE_PARM_DESC(lotserver_adaptive, "Enable adaptive rate control");
module_param(lotserver_turbo, bool, 0644);
MODULE_PARM_DESC(lotserver_turbo, "Turbo mode - ignore all congestion signals");

struct lotspeed {
    u64 target_rate;
    u64 actual_rate;
    u32 cwnd_gain;
    u32 loss_count;
    u32 rtt_min;
    u32 rtt_cnt;
    u64 last_update;
    bool ss_mode;
    u32 probe_cnt;
};

static struct tcp_congestion_ops lotspeed_ops;

// 初始化连接
static void lotspeed_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    // 初始化状态
    tp->snd_ssthresh = lotserver_turbo ? TCP_INFINITE_SSTHRESH : tp->snd_cwnd * 2;
    ca->target_rate = lotserver_rate;
    ca->actual_rate = 0;
    ca->cwnd_gain = lotserver_gain;
    ca->loss_count = 0;
    ca->rtt_min = 0;
    ca->rtt_cnt = 0;
    ca->last_update = tcp_jiffies32;
    ca->ss_mode = true;
    ca->probe_cnt = 0;

    // 强制开启 pacing
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    pr_debug("lotspeed: init connection, target_rate=%llu gain=%u\n",
             ca->target_rate, ca->cwnd_gain);
}

// 更新 RTT 统计
static void lotspeed_update_rtt(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 rtt_us = tp->srtt_us >> 3;

    if (!rtt_us || rtt_us == 0)
        return;

    // 记录最小 RTT 作为基准
    if (!ca->rtt_min || rtt_us < ca->rtt_min) {
        ca->rtt_min = rtt_us;
    }

    ca->rtt_cnt++;
}

// 自适应速率调整
static void lotspeed_adapt_rate(struct sock *sk, const struct rate_sample *rs)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u64 bw = 0;

    if (!lotserver_adaptive)
        return;

    // 计算实际带宽
    if (rs->delivered > 0 && rs->interval_us > 0) {
        bw = (u64)rs->delivered * USEC_PER_SEC;
        do_div(bw, rs->interval_us);
        ca->actual_rate = bw;

        // 如果实际速率远低于目标，可能遇到瓶颈
        if (bw < ca->target_rate / 2 && ca->loss_count > 0) {
            // 温和降速
            ca->target_rate = max_t(u64, bw * 15 / 10, lotserver_rate / 4);
            ca->cwnd_gain = max_t(u32, ca->cwnd_gain - 5, 15);
            pr_debug("lotspeed: reduce rate to %llu, gain to %u\n",
                     ca->target_rate, ca->cwnd_gain);
        }
            // 如果表现良好，逐步恢复
        else if (bw > ca->target_rate * 8 / 10 && ca->loss_count == 0) {
            ca->target_rate = min_t(u64, ca->target_rate * 11 / 10, lotserver_rate);
            ca->cwnd_gain = min_t(u32, ca->cwnd_gain + 2, lotserver_gain);
        }
    }

    // RTT 膨胀检测
    if (ca->rtt_min && tp->srtt_us >> 3 > ca->rtt_min * 3 / 2) {
        // RTT 膨胀超过 50%，可能有队列堆积
        if (!lotserver_turbo) {
            ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 15);
        }
    }
}

// 主拥塞控制逻辑
static void lotspeed_cong_control(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 rate;
    u32 cwnd;
    u32 rtt_us = tp->srtt_us >> 3;
    u32 mss = tp->mss_cache;
    u32 target_cwnd;

    // 默认值处理
    if (!rtt_us) rtt_us = 1000;   // 1ms 默认
    if (!mss) mss = 1460;          // 标准以太网 MSS

    // 更新 RTT 统计
    lotspeed_update_rtt(sk);

    // 自适应调整
    lotspeed_adapt_rate(sk, rs);

    // 选择速率
    rate = ca->target_rate;

    // 核心公式：CWND = (rate × RTT) / MSS × gain
    target_cwnd = div64_u64(rate * (u64)rtt_us, (u64)mss * 1000000);
    target_cwnd = div_u64(target_cwnd * ca->cwnd_gain, 10);

    // 慢启动阶段特殊处理
    if (ca->ss_mode && tp->snd_cwnd < tp->snd_ssthresh) {
        // 指数增长
        cwnd = tp->snd_cwnd * 2;
        if (cwnd >= target_cwnd) {
            ca->ss_mode = false;
            cwnd = target_cwnd;
        }
    } else {
        // 正常阶段
        cwnd = target_cwnd;

        // 周期性探测更高速率
        ca->probe_cnt++;
        if (ca->probe_cnt >= 100) {  // 每 100 个 RTT 探测一次
            cwnd = cwnd * 11 / 10;   // 探测 +10%
            ca->probe_cnt = 0;
        }
    }

    // 应用安全限制
    cwnd = max_t(u32, cwnd, lotserver_min_cwnd);
    cwnd = min_t(u32, cwnd, lotserver_max_cwnd);
    cwnd = min_t(u32, cwnd, tp->snd_cwnd_clamp);

    // 设置拥塞窗口和 pacing 速率
    tp->snd_cwnd = cwnd;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    sk->sk_pacing_rate = rate;
#endif

    // 调试输出
    if (ca->rtt_cnt % 100 == 0) {
        pr_debug("lotspeed: cwnd=%u rate=%llu rtt=%u gain=%u losses=%u\n",
                 cwnd, rate, rtt_us, ca->cwnd_gain, ca->loss_count);
    }
}

// 处理状态变化
static void lotspeed_set_state(struct sock *sk, u8 new_state)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    switch (new_state) {
        case TCP_CA_Loss:
            // 涡轮模式完全无视丢包
            if (lotserver_turbo) {
                tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
                return;
            }
            // 记录丢包
            ca->loss_count++;
            ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 8 / 10, 10);
            break;

        case TCP_CA_Recovery:
            // 进入恢复阶段
            if (!lotserver_turbo) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 15);
            }
            break;

        case TCP_CA_Open:
            // 恢复正常
            ca->ss_mode = false;
            break;

        default:
            break;
    }
}

// 丢包时的 ssthresh
static u32 lotspeed_ssthresh(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    // 涡轮模式：永不降速
    if (lotserver_turbo) {
        return TCP_INFINITE_SSTHRESH;
    }

    // 温和降速
    ca->loss_count++;
    ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 8 / 10, 10);

    return max_t(u32, tp->snd_cwnd * 7 / 10, lotserver_min_cwnd);
}

// 恢复拥塞窗口
static u32 lotspeed_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    // 误判恢复，重置丢包计数
    ca->loss_count = 0;
    ca->ss_mode = false;

    return max(tp->snd_cwnd, tp->prior_cwnd);
}

// 处理拥塞事件
static void lotspeed_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct lotspeed *ca = inet_csk_ca(sk);

    switch (event) {
        case CA_EVENT_LOSS:
            // 发生丢包
            ca->loss_count++;
            if (!lotserver_turbo) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain - 5, 10);
            }
            break;

        case CA_EVENT_TX_START:
            // 开始传输
            ca->ss_mode = true;
            ca->probe_cnt = 0;
            break;

        case CA_EVENT_CWND_RESTART:
            // 重新开始
            ca->ss_mode = true;
            ca->loss_count = 0;
            ca->probe_cnt = 0;
            break;

        default:
            // 其他事件忽略
            break;
    }
}

static struct tcp_congestion_ops lotspeed_ops __read_mostly = {
        .name           = "lotspeed",
        .owner          = THIS_MODULE,
        .init           = lotspeed_init,
        .cong_control   = lotspeed_cong_control,
        .set_state      = lotspeed_set_state,
        .ssthresh       = lotspeed_ssthresh,
        .undo_cwnd      = lotspeed_undo_cwnd,
        .cwnd_event     = lotspeed_cwnd_event,
};

static int __init lotspeed_module_init(void)
{
    BUILD_BUG_ON(sizeof(struct lotspeed) > ICSK_CA_PRIV_SIZE);

    pr_info("╔════════════════════════════════════════════════════════╗\n");
    pr_info("║          LotSpeed v2.0 - 锐速复活版                    ║\n");
    pr_info("║          Created by uk0 @ 2025-11-17 12:24:30          ║\n");
    pr_info("║          Kernel: %u.%u                                   ║\n",
            LINUX_VERSION_CODE >> 16, (LINUX_VERSION_CODE >> 8) & 0xff);
    pr_info("╚════════════════════════════════════════════════════════╝\n");
    pr_info("Parameters: rate=%lu gain=%u adaptive=%d turbo=%d\n",
            lotserver_rate, lotserver_gain, lotserver_adaptive, lotserver_turbo);

    return tcp_register_congestion_control(&lotspeed_ops);
}

static void __exit lotspeed_module_exit(void)
{
    tcp_unregister_congestion_control(&lotspeed_ops);
    pr_info("LotSpeed v2.0 unloaded\n");
}

module_init(lotspeed_module_init);
module_exit(lotspeed_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("uk0 <github.com/uk0>");
MODULE_VERSION("2.0");
MODULE_DESCRIPTION("LotSpeed v2.0 - Modern LotServer/ServerSpeeder replacement for 1G~40G networks");
MODULE_ALIAS("tcp_lotspeed");