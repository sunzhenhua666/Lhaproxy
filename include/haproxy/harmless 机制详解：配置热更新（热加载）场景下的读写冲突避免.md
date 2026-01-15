harmless 机制详解：配置热更新（热加载）场景下的读写冲突避免
=============================
下面把 HAProxy 的 harmless 机制（无害期机制） 按照你提到的“配置热更新（热加载）”场景来做一次深入、逐行级别的解释，让你能够理解它是如何在多线程条件下保证 读写不会冲突 的。

1. 背景：为什么需要 harmless 机制？
HAProxy 的运行时环境（runtime）中，有一些数据结构是：

多线程同时读（如会话列表、监听器列表、运行队列等）

偶尔需要写（例如：热更新配置、更新 server 权重、修改 stick-table、移除监听器等）

并且这些数据结构有些无法加锁（或锁代价太大）。
为了减少锁开销，HAProxy 采用：


读线程无锁（lock-free），写线程需要一个特殊的安全窗口

这个安全窗口，就是 harmless（无害）机制。


2. harmless 的目标：

让一个线程可以在执行修改操作时，保证：

其他线程不会正在访问被修改的对象（或不会在临界区内）

从而避免 读写竞争 或 读到被释放的内存

也就是一种 轻量级的读侧保护机制（类似 RCU）。


3. 全局变量关系

关键变量：
tg_ctx->threads_harmless;   // bitmask: 当前处于 harmless 状态的线程集合
rdv_requests;               // 等待 rendez-vous 的写线程数量（通常为1）
每个线程自己的 bit：
ti->ltid_bit   // 当前线程在 mask 中对应的 bit
4. “无害期”如何开始？（读线程）

当线程打算进入某段可能被写线程用到的共享区域（例如 poll 循环），它会标记自己为 harmless：
static inline void thread_harmless_now() {
    HA_ATOMIC_OR(&tg_ctx->threads_harmless, ti->ltid_bit);
}
表示：

✔ 当前线程不会访问敏感结构
✔ 如果有写操作需要执行，我允许写线程继续

这通常放在 poll 前：
thread_harmless_now();
poll();
thread_harmless_end();
5. “无害期”如何结束？
static inline void thread_harmless_end()
{
	while (1) {
		HA_ATOMIC_AND(&tg_ctx->threads_harmless, ~ti->ltid_bit);
		if (likely(_HA_ATOMIC_LOAD(&rdv_requests) == 0))
			break;
		thread_harmless_till_end();
	}
}
含义：

步骤①：先清除 harmless 标记
我是无害的 → 现在要恢复正常运行

步骤②：如果没有写操作，退出
if (rdv_requests == 0) break;

步骤③：如果有写操作请求，需要等待写线程完成

调用：

thread_harmless_till_end();


会阻塞住，直到写线程操作完成。

6. 写线程如何请求所有线程进入 harmless？（写操作的触发）

当进行热更新（例如重新加载 server 配置）时，会调用：
thread_isolate()
内部会：

rdv_requests++

等待每个线程都进入 harmless 状态：
while (threads_harmless != all_threads_mask)
    wait();
当 所有线程都 harmless 时：
写线程就可以 安全修改共享结构
7. 整个流程示意图（非常重要）
线程1,2,3,...N 读线程：
----poll-loop---------
|  harmless_now()     ← 我暂时不触碰共享结构
|  poll()
|  harmless_end()      ← 写线程是否想更新？如果是，我等它
----------------------
线程X 写线程（触发热更新）：
thread_isolate()       ← 请求所有线程进入 harmless
    rdv_requests++ 
    wait until all threads harmless

修改共享结构            ← 安全！无读线程访问

thread_release()       ← 解除阻塞
    rdv_requests--
    所有线程 harmless_end() 会继续执行


8. 为什么“读写不冲突”？
原因 1：写线程只能在所有其他线程 harmless 时修改结构

意味着：
所有读线程 此时保证不会访问临界结构

写线程得到了一个 完全无并发的窗口

类似于手工实现的 RCU Grace Period。
原因 2：读线程不会在“有写请求”的情况下离开 harmless 状态

因为：
while (rdv_requests > 0)
    thread_harmless_till_end();
读线程会 卡在 harmless_end 内部，不会去读数据。
原因 3：无锁 + 内存屏障保障顺序
写线程只需要：

等待所有线程 harmless

修改数据

发布更新（使用内存屏障）

读线程确保：

在 harmless_end 之后才能看到新的数据

9. 热更新（runtime API）中用到 harmless 的典型场景
HAProxy 的运行时脚本，例如：
echo "set server web1 weight 50" | socat ...
此类操作涉及修改：

server 状态

server 权重

backend 链表

listener 状态

这些都可能被所有线程同时读
因此必须：
写线程 isolate

所有线程 harmless

修改链表/结构

释放

总结：harmless 机制＝轻量级的无锁同步框架
读线程（高频）

快速进入 harmless

poll 后快速解除 harmless

如果遇到写操作，等待

写线程（低频）

请求所有线程 harmless

获得独占窗口

安全更新结构

释放读线程
