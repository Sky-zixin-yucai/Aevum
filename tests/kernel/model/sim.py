#!/usr/bin/env python3
"""最小 CPU-bound 调度模型：A0 固定片 vs CFS 动态片。

只算份额、片长、等待时间，不模拟阻塞/唤醒。输出用于测试前的数量级校准。
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


CFS_LATENCY = 10
CFS_MIN_GRANULARITY = 1
VRUNTIME_PERIOD = 64.0
A0_SLICE = 4


@dataclass
class Task:
    tid: str
    priority: int
    weight: float
    vruntime: float = 0.0
    cpu_ticks: int = 0
    wait_ticks: int = 0
    max_wait: int = 0


def load_task_set(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    if "tasks" not in data or not data["tasks"]:
        raise ValueError("task set needs a non-empty tasks array")
    if int(data.get("duration_ticks", 0)) <= 0:
        raise ValueError("duration_ticks must be > 0")
    return data


def weight_for(policy: str, priority: int) -> float:
    if policy == "a0":
        # 当前内核代码：task_weight = priority + 1
        return float(priority + 1)
    if policy == "cfs":
        # 0 最高 -> 8, 1 -> 4, 2 -> 2, 3 -> 1
        return float(1 << (3 - priority))
    raise ValueError(f"unknown policy: {policy}")


def slice_for(policy: str, weight: float, total_weight: float) -> int:
    if policy == "a0":
        return A0_SLICE
    raw = int(CFS_LATENCY * weight / total_weight)
    return max(CFS_MIN_GRANULARITY, raw)


def simulate(task_set: dict[str, Any], policy: str) -> dict[str, Any]:
    tasks = [
        Task(
            tid=str(item["id"]),
            priority=int(item["priority"]),
            weight=weight_for(policy, int(item["priority"])),
        )
        for item in task_set["tasks"]
    ]
    if any(t.priority < 0 or t.priority > 3 for t in tasks):
        raise ValueError("priority must be in 0..3")

    total_weight = sum(t.weight for t in tasks)
    duration = int(task_set["duration_ticks"])
    tick = 0
    switches = 0
    prev_tid: str | None = None

    while tick < duration:
        current = min(tasks, key=lambda t: (t.vruntime, t.tid))
        run = slice_for(policy, current.weight, total_weight)
        run = min(run, duration - tick)

        for other in tasks:
            if other is current:
                continue
            other.wait_ticks += run
            other.max_wait = max(other.max_wait, other.wait_ticks)

        current.cpu_ticks += run
        current.wait_ticks = 0
        current.vruntime += run * VRUNTIME_PERIOD / current.weight
        if prev_tid is not None and prev_tid != current.tid:
            switches += 1
        prev_tid = current.tid
        tick += run

    total_cpu = sum(t.cpu_ticks for t in tasks)
    result_tasks = []
    for t in tasks:
        result_tasks.append(
            {
                "id": t.tid,
                "priority": t.priority,
                "weight": t.weight,
                "cpu_ticks": t.cpu_ticks,
                "share": round(t.cpu_ticks / total_cpu, 6) if total_cpu else 0.0,
                "theory_share": round(t.weight / total_weight, 6),
                "max_wait_ticks": t.max_wait,
            }
        )

    return {
        "name": task_set.get("name", "unnamed"),
        "policy": policy,
        "tick_hz": int(task_set.get("tick_hz", 100)),
        "duration_ticks": duration,
        "total_cpu_ticks": total_cpu,
        "switches": switches,
        "tasks": result_tasks,
    }


def print_table(result: dict[str, Any]) -> None:
    print(
        f"{result['name']} policy={result['policy']} "
        f"duration={result['duration_ticks']} switches={result['switches']}"
    )
    print(f"{'task':<8} {'prio':>4} {'weight':>8} {'cpu':>8} {'share':>8} {'theory':>8} {'max_wait':>10}")
    for task in result["tasks"]:
        print(
            f"{task['id']:<8} {task['priority']:>4} {task['weight']:>8.1f} "
            f"{task['cpu_ticks']:>8} {task['share']:>8.3f} "
            f"{task['theory_share']:>8.3f} {task['max_wait_ticks']:>10}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task-set", required=True, type=Path)
    parser.add_argument("--policy", choices=("a0", "cfs"), default="a0")
    parser.add_argument("--json", action="store_true", help="print JSON instead of a table")
    args = parser.parse_args()

    task_set = load_task_set(args.task_set)
    result = simulate(task_set, args.policy)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print_table(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
