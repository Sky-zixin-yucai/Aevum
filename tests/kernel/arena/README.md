# 调度基准场

一个用 Aevum 引擎循环驱动的可视化测试场，用来先看同一份任务集在不同调度策略下的：

- 每个任务拿到多少 CPU tick
- 片长和切换次数
- 最长等待
- 实际份额和理论权重的偏差

它**不是** QEMU 实测的替代品。这里的数据来自 `tests/kernel/model` 同一套 CPU-bound 模型；真实内核数据要等 QEMU 跑完后再从 `results/` 读入。

## 运行

在仓库根启动静态服务器：

```powershell
python -m http.server 8766 --directory D:\Aevum
```

打开：

```text
http://127.0.0.1:8766/tests/kernel/arena/index.html
```

页面通过 `../../../engine/core/index.js` 加载 `OrbEngine`，由 `OrbEngine.engineLoop` 的 `onUpdate` / `onRender` 驱动时间轴，不另起一套 `requestAnimationFrame`。

## 控件

- 任务数：2 / 4 / 8 / 12。
- 策略：
  - `A0`：当前内核代码，`weight = priority + 1`，固定 4 tick 片。
  - `CFS`：Linux CFS 目标模型，`weight = 8/4/2/1`，动态片长 10 tick。
  - `FIFO`：Linux `SCHED_FIFO` 模型，高优先级不阻塞就独占 CPU，用来标定控制类的最坏情况。
- 速度：时间轴推进速度。
- 运行 / 暂停 / 重置。

## 看什么

1. 先看 A0 的 `priority 0` 份额是不是只有 20%；如果是，说明权重方向确实反了。
2. 切到 CFS，看 `priority 0` 是否接近 88.9%。
3. 把任务数拉到 12，看低优先级是否还有非零 CPU，最大等待是否可接受。
4. 切到 FIFO，看高优先级任务独占时的最大等待：这是控制类要面对的最坏情况，不是公平份额。
5. 记录切换次数：CFS 动态片长在任务多时会变短，切换会变多；FIFO 在 CPU-bound 下几乎不切换，但代价是饿死。

## 边界

- 只模拟 CPU-bound，没有 pipe、sleep、信号、fork。
- 没有模拟 `min_vruntime`、睡眠补偿、唤醒抢占。
- 没有模拟 Linux CFS 的 PELT、红黑树、组调度。
- 模型里的 tick 是逻辑 tick，不是墙钟毫秒。

这些要等 `WORKFLOW.md` 的 K3/K4/K5 在 QEMU 里跑。
