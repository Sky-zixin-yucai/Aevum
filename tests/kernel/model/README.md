# 离线调度模型

这里先放一个最小模型，不假装能替代 QEMU 实测。

## 现在能算什么

`sim.py` 只模拟 **CPU-bound 任务集**：

- 所有任务在 0 时刻就绪；
- 没有睡眠、pipe、信号、fork；
- 只比较权重份额、等待时间、片长和切换次数；
- `a0` 模型对应当前内核：`weight = priority + 1`，固定 4 tick 片；
- `cfs` 模型对应目标：`weight = 1 << (3 - priority)`，动态片长。

它适合先回答：

- 当前 `priority 0 -> weight 1` 的映射，实际份额是不是和文档写的反了；
- 如果改成 `8/4/2/1`，长期份额是多少；
- 固定 4 tick 片和动态片长，低优先级任务的最大等待差多少。

它不能回答：

- 唤醒延迟、周期抖动、deadline miss；
- 阻塞/唤醒的 lost wake；
- 上下文切换的真实成本；
- Linux 的 `SCHED_FIFO` 行为。

这些要等 QEMU 实测。

## 任务集格式

```json
{
  "name": "basic-fairness",
  "tick_hz": 100,
  "duration_ticks": 2000,
  "tasks": [
    {"id": "T0", "priority": 0, "period": 0, "cost": 1},
    {"id": "T3", "priority": 3, "period": 0, "cost": 1}
  ]
}
```

- `period=0`：CPU-bound，一直就绪。
- `period>0`：预留给后续周期任务模型，当前版本只做参数校验，不参与调度。
- `priority`：0 最高，3 最低。
- `cost`：单次执行 tick 数，当前版本按 1 处理，留字段给后续。

## 运行

```bash
python3 sim.py --task-set task_sets/basic.json --policy a0
python3 sim.py --task-set task_sets/basic.json --policy cfs
python3 sim.py --task-set task_sets/basic.json --policy cfs --json
```

输出包括：

- 每个任务的 `cpu_ticks`、`share`、`max_wait`
- 总切换次数
- 权重和理论份额

## 模型边界

- `a0` 用固定 4 tick 片，但模型里 `vruntime` 每 tick 记账，和内核的 PIT 记账一致。
- `cfs` 用 `CFS_LATENCY=10`、`CFS_MIN_GRANULARITY=1`，片长按总权重算。
- 模型没有实现 `min_vruntime`、唤醒抢占、睡眠补偿；这些在 QEMU 阶段再加。
- 所有数字是相对值，不是墙钟时间；不要拿模型输出和 Linux 的毫秒数直接比。
