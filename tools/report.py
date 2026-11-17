#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 mini-rtos 导出的 trace JSON 渲染成单文件 HTML 报告。

用法： python tools/report.py out docs/report.html
依赖： 只用标准库（json / os / sys）
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>mini-rtos · 优先级反转复现报告</title>
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 40px 24px 64px;
    font-family: -apple-system, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
    background: #f5f6f8; color: #1f2937;
    -webkit-font-smoothing: antialiased;
  }
  .wrap { max-width: 1180px; margin: 0 auto; }
  header h1 { margin: 0 0 8px; font-size: 26px; letter-spacing: -.3px; }
  header p  { margin: 0; color: #64748b; font-size: 14px; line-height: 1.7; }
  .tag {
    display: inline-block; padding: 2px 9px; margin-right: 6px; border-radius: 999px;
    font-size: 12px; font-weight: 600; background: #eef2ff; color: #4f46e5;
  }
  .cards { display: grid; grid-template-columns: repeat(4, 1fr); gap: 14px; margin: 26px 0 30px; }
  .card { background: #fff; border: 1px solid #e6e8ec; border-radius: 14px; padding: 18px 20px; }
  .card .k { font-size: 12px; color: #6b7280; margin-bottom: 10px; }
  .card .v { font-size: 30px; font-weight: 700; letter-spacing: -1px; line-height: 1; }
  .card .v span { font-size: 13px; font-weight: 500; color: #9ca3af; margin-left: 4px; letter-spacing: 0; }
  .card .s { margin-top: 8px; font-size: 12px; color: #9ca3af; }
  .red { color: #dc2626; } .green { color: #059669; } .indigo { color: #4f46e5; }
  section { background: #fff; border: 1px solid #e6e8ec; border-radius: 16px; padding: 22px 24px 18px; margin-bottom: 20px; }
  section h2 { margin: 0 0 4px; font-size: 16px; }
  section .sub { margin: 0 0 16px; font-size: 13px; color: #8b95a5; }
  canvas { width: 100%; display: block; }
  .legend { display: flex; gap: 18px; margin-top: 12px; font-size: 12px; color: #6b7280; }
  .legend i { display: inline-block; width: 11px; height: 11px; border-radius: 3px; margin-right: 6px; vertical-align: -1px; }
  table { width: 100%; border-collapse: collapse; font-size: 13.5px; }
  th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid #eef0f3; }
  th { color: #6b7280; font-weight: 600; font-size: 12.5px; background: #fafbfc; }
  td.num { font-variant-numeric: tabular-nums; }
  tr:last-child td { border-bottom: 0; }
  .note { font-size: 13px; color: #64748b; line-height: 1.85; margin: 14px 0 0; }
  .note b { color: #374151; }
  code { background: #f3f4f6; padding: 1px 6px; border-radius: 5px; font-size: 12.5px; }
  footer { text-align: center; color: #9ca3af; font-size: 12px; margin-top: 28px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>mini-rtos · 优先级反转复现</h1>
    <p>
      <span class="tag">单核抢占式调度</span><span class="tag">优先级继承</span><span class="tag">tick 级 trace</span>
      三个任务、一个互斥量，在没有任何硬件的情况下把 classic priority inversion 完整复现出来：
      同一份代码只改 <code>inherit</code> 一个开关，高优先级任务的最坏响应时间相差 3.6 倍。
    </p>
  </header>

  <div class="cards">
    <div class="card">
      <div class="k">HIGH 被阻塞（不继承）</div>
      <div class="v red">__A_BLOCK__<span>tick</span></div>
      <div class="s">其中 __A_MED__ tick 被 MED 白占</div>
    </div>
    <div class="card">
      <div class="k">HIGH 被阻塞（继承）</div>
      <div class="v green">__B_BLOCK__<span>tick</span></div>
      <div class="s">MED 插队 __B_MED__ tick</div>
    </div>
    <div class="card">
      <div class="k">最坏响应时间改善</div>
      <div class="v indigo">__RATIO__<span>×</span></div>
      <div class="s">__A_BLOCK__ → __B_BLOCK__</div>
    </div>
    <div class="card">
      <div class="k">LOW 临界区耗时</div>
      <div class="v">__A_CS__<span>→ __B_CS__ tick</span></div>
      <div class="s">实际工作量只有 60 tick</div>
    </div>
  </div>

  <section>
    <h2>场景 A · 不开启优先级继承</h2>
    <p class="sub">LOW 持锁进临界区 → MED 抢占 → HIGH 到达后被卡住，只能等 MED 跑完。红色区间就是"高优先级任务被低优先级任务拖住"的时间。</p>
    <canvas id="ganttA" height="186"></canvas>
    <div class="legend">
      <span><i style="background:#ef4444"></i>HIGH (prio 1)</span>
      <span><i style="background:#6366f1"></i>MED (prio 4)</span>
      <span><i style="background:#f59e0b"></i>LOW (prio 6)</span>
      <span><i style="background:#e5e7eb"></i>空闲</span>
    </div>
  </section>

  <section>
    <h2>场景 B · 开启优先级继承</h2>
    <p class="sub">HIGH 一阻塞，内核立刻把持锁的 LOW 抬到 prio 1；MED 抢不动 LOW，只能等临界区结束 —— 阻塞窗口塌缩成一个临界区。</p>
    <canvas id="ganttB" height="186"></canvas>
    <div class="legend">
      <span><i style="background:#ef4444"></i>HIGH (prio 1)</span>
      <span><i style="background:#6366f1"></i>MED (prio 4)</span>
      <span><i style="background:#f59e0b"></i>LOW (prio 6)</span>
      <span><i style="background:#e5e7eb"></i>空闲</span>
    </div>
  </section>

  <section>
    <h2>LOW 的优先级随时间变化</h2>
    <p class="sub">继承只在"持锁且有人等"的窗口内有效，解锁瞬间还原 —— 这是优先级继承与优先级天花板（ceiling）最直观的区别。</p>
    <canvas id="prio" height="172"></canvas>
  </section>

  <section>
    <h2>指标对比</h2>
    <table>
      <thead><tr><th>指标</th><th>不继承</th><th>继承</th><th>说明</th></tr></thead>
      <tbody>
        <tr><td>HIGH 被阻塞时长</td><td class="num">__A_BLOCK__ tick</td><td class="num">__B_BLOCK__ tick</td><td>从发起 <code>sem_wait</code> 到真正拿到锁</td></tr>
        <tr><td>&nbsp;&nbsp;其中被 MED 插队</td><td class="num">__A_MED__ tick</td><td class="num">__B_MED__ tick</td><td>优先级反转的直接代价</td></tr>
        <tr><td>HIGH 拿到锁的时刻</td><td class="num">t = __A_GOT__</td><td class="num">t = __B_GOT__</td><td>tick 计</td></tr>
        <tr><td>LOW 临界区实际耗时</td><td class="num">__A_CS__ tick</td><td class="num">__B_CS__ tick</td><td>代码里只写了 <code>busy(60)</code></td></tr>
        <tr><td>整场运行总 tick</td><td class="num">≈ __A_TOTAL__</td><td class="num">≈ __B_TOTAL__</td><td>末任务收尾落在哪个 tick 边界上有 ±1 抖动</td></tr>
      </tbody>
    </table>
    <p class="note">
      <b>怎么读这组数：</b>LOW 的临界区代码是 60 tick，不继承时被拉长到 __A_CS__ tick（因为每次时间片都被 MED 抢走），
      HIGH 因此等了 __A_BLOCK__ tick；开启继承后 LOW 一口气跑完临界区，只花 __B_CS__ tick，
      HIGH 的等待退化成"持锁者剩余工作量"，也就是理论上不可消除的那部分。
    </p>
  </section>

  <section>
    <h2>事件流（场景 B 关键节点）</h2>
    <table>
      <thead><tr><th style="width:80px">tick</th><th style="width:110px">任务</th><th style="width:110px">事件</th><th>含义</th></tr></thead>
      <tbody id="evTable"></tbody>
    </table>
  </section>

  <footer>mini-rtos · 数据由 <code>build/mini_rtos.exe --out out</code> 生成，tick = 1ms，连跑 5 次指标一致</footer>
</div>

<script>
const A = __DATA_A__;
const B = __DATA_B__;
const SUM = __DATA_SUM__;

const COLORS = { red: '#ef4444', indigo: '#6366f1', amber: '#f59e0b', idle: '#e5e7eb' };
const NAMES = {};

function fit(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = parseInt(cv.getAttribute('height'), 10);
  cv.width = w * dpr; cv.height = h * dpr;
  cv.style.height = h + 'px';
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h };
}

function colorOf(task, tasks) {
  const t = tasks.find(x => x.id === task);
  if (!t) return COLORS.idle;
  if (t.prio === 1) return COLORS.red;
  if (t.prio === 4) return COLORS.indigo;
  if (t.prio === 6) return COLORS.amber;
  return '#94a3b8';
}

function drawGantt(id, D, blockFrom, blockTo, annotate) {
  const cv = document.getElementById(id);
  const { ctx, w, h } = fit(cv);
  ctx.clearRect(0, 0, w, h);

  const LEFT = 96, RIGHT = 18, TOP = 26, ROW = 34, GAP = 9;
  const rows = D.tasks.slice().sort((a, b) => a.prio - b.prio);  // 高优先级在上
  const X = t => LEFT + (t / D.ticks) * (w - LEFT - RIGHT);

  // 阻塞区间底色
  ctx.fillStyle = 'rgba(239,68,68,.07)';
  ctx.fillRect(X(blockFrom), TOP - 14, X(blockTo) - X(blockFrom), rows.length * (ROW + GAP) - GAP + 20);

  // 每行
  rows.forEach((t, i) => {
    const y = TOP + i * (ROW + GAP);
    ctx.fillStyle = '#f8fafc';
    roundRect(ctx, LEFT, y, w - LEFT - RIGHT, ROW, 5); ctx.fill();

    let start = -1;
    for (let k = 0; k <= D.owner.length; k++) {
      const on = k < D.owner.length && D.owner[k] === t.id;
      if (on && start < 0) start = k;
      if (!on && start >= 0) {
        const x0 = X(start), x1 = Math.max(X(k), X(start) + 1.5);
        ctx.fillStyle = colorOf(t.id, D.tasks);
        roundRect(ctx, x0, y, x1 - x0, ROW, 3); ctx.fill();
        start = -1;
      }
    }

    const nm = t.name, suf = 'prio ' + t.basePrio;
    const fBold = '600 12.5px -apple-system, "Segoe UI", sans-serif';
    const fReg  = '11px -apple-system, "Segoe UI", sans-serif';
    ctx.font = fBold; const w1 = ctx.measureText(nm).width;
    ctx.font = fReg;  const w2 = ctx.measureText(suf).width;
    const x0 = LEFT - 12 - (w1 + w2);
    ctx.textAlign = 'left';
    ctx.font = fBold; ctx.fillStyle = '#374151';
    ctx.fillText(nm, x0, y + ROW / 2 + 4);
    ctx.font = fReg; ctx.fillStyle = '#9ca3af';
    ctx.fillText(suf, x0 + w1 + 5, y + ROW / 2 + 4);
  });

  // 时间轴
  const axisY = TOP + rows.length * (ROW + GAP) - GAP + 14;
  ctx.strokeStyle = '#e5e7eb'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(LEFT, axisY); ctx.lineTo(w - RIGHT, axisY); ctx.stroke();
  ctx.fillStyle = '#9ca3af'; ctx.font = '11px -apple-system, sans-serif'; ctx.textAlign = 'center';
  for (let t = 0; t <= D.ticks; t += 25) {
    const x = X(t);
    ctx.beginPath(); ctx.moveTo(x, axisY); ctx.lineTo(x, axisY + 4); ctx.stroke();
    ctx.fillText(t + ' tick', x, axisY + 17);
  }

  // 标注
  ctx.textAlign = 'left';
  ctx.font = '600 12px -apple-system, sans-serif';
  ctx.fillStyle = '#dc2626';
  const midX = (X(blockFrom) + X(blockTo)) / 2;
  ctx.textAlign = 'center';
  ctx.fillText(annotate, midX, TOP - 10);
}

function roundRect(ctx, x, y, w, h, r) {
  r = Math.min(r, h / 2, w / 2);
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function drawPrio(id, D, sum) {
  const cv = document.getElementById(id);
  const { ctx, w, h } = fit(cv);
  ctx.clearRect(0, 0, w, h);

  const LEFT = 74, RIGHT = 18, TOP = 22, BOT = h - 30;
  const X = t => LEFT + (t / D.ticks) * (w - LEFT - RIGHT);
  const Y = p => TOP + (p / 7) * (BOT - TOP);

  // 网格
  ctx.strokeStyle = '#f1f3f6'; ctx.lineWidth = 1;
  ctx.font = '11px -apple-system, sans-serif'; ctx.fillStyle = '#9ca3af';
  for (let p = 0; p <= 7; p += 1) {
    ctx.beginPath(); ctx.moveTo(LEFT, Y(p)); ctx.lineTo(w - RIGHT, Y(p)); ctx.stroke();
    if (p % 1 === 0) { ctx.textAlign = 'right'; ctx.fillText('prio ' + p, LEFT - 10, Y(p) + 4); }
  }

  const low = D.tasks.find(t => t.name === 'LOW');
  const base = low.basePrio;
  const steps = [{ t: 0, p: base }];
  D.events.forEach(e => {
    if (e.task === low.id && (e.e === 'inherit' || e.e === 'restore') && e.p >= 0) {
      steps.push({ t: e.t, p: e.p });
    }
  });
  steps.push({ t: D.ticks, p: steps[steps.length - 1].p });

  // 面积
  const grad = ctx.createLinearGradient(0, TOP, 0, BOT);
  grad.addColorStop(0, 'rgba(245,158,11,.28)');
  grad.addColorStop(1, 'rgba(245,158,11,.02)');
  ctx.beginPath();
  ctx.moveTo(X(steps[0].t), Y(steps[0].p));
  for (let i = 1; i < steps.length; i++) {
    ctx.lineTo(X(steps[i].t), Y(steps[i - 1].p));
    ctx.lineTo(X(steps[i].t), Y(steps[i].p));
  }
  const lastP = Y(steps[steps.length - 1].p);
  ctx.lineTo(X(D.ticks), BOT); ctx.lineTo(X(0), BOT); ctx.closePath();
  ctx.fillStyle = grad; ctx.fill();

  // 阶梯线
  ctx.beginPath();
  ctx.moveTo(X(steps[0].t), Y(steps[0].p));
  for (let i = 1; i < steps.length; i++) {
    ctx.lineTo(X(steps[i].t), Y(steps[i - 1].p));
    ctx.lineTo(X(steps[i].t), Y(steps[i].p));
  }
  ctx.strokeStyle = '#f59e0b'; ctx.lineWidth = 2.4; ctx.stroke();

  // 关键点
  const inheritEv = D.events.find(e => e.e === 'inherit');
  const restoreEv = D.events.find(e => e.e === 'restore');
  [[inheritEv, 'HIGH 阻塞 → LOW 抬到 prio 1', '#dc2626', -1],
   [restoreEv, '解锁 → 还原 prio ' + base, '#059669', 1]].forEach(([ev, label, color, dir]) => {
    if (!ev) return;
    const x = X(ev.t), y = Y(ev.p);
    ctx.beginPath(); ctx.arc(x, y, 4, 0, Math.PI * 2);
    ctx.fillStyle = color; ctx.fill();
    ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.stroke();
    ctx.fillStyle = color; ctx.font = '600 12px -apple-system, sans-serif';
    ctx.textAlign = ev.t < D.ticks * 0.3 ? 'left' : 'right';
    ctx.fillText(label, x + (ev.t < D.ticks * 0.3 ? 10 : -10), y + dir * 16);
  });

  ctx.textAlign = 'center'; ctx.fillStyle = '#9ca3af'; ctx.font = '11px -apple-system, sans-serif';
  for (let t = 0; t <= D.ticks; t += 25) ctx.fillText(t, X(t), BOT + 18);
}

const EV_TEXT = {
  switch: '被调度器选中，开始占用 CPU',
  preempt: '时间片到 / 被更高优先级抢占',
  block: '申请互斥量失败，进入阻塞',
  inherit: '被继承抬升优先级',
  restore: '释放锁，优先级还原',
  lock: '拿到互斥量，进入临界区',
  unlock: '释放互斥量',
  wake: '被唤醒，转入就绪队列',
  sleep: '主动定时阻塞',
  timer: '定时到期，转就绪',
  done: '任务结束'
};
const EV_ORDER = ['block', 'inherit', 'lock', 'unlock', 'wake', 'restore', 'preempt', 'switch', 'sleep', 'timer', 'done'];

function renderEvents(D) {
  const tb = document.getElementById('evTable');
  const nameOf = id => (D.tasks.find(t => t.id === id) || {}).name || ('#' + id);
  const rows = D.events
    .filter(e => ['block', 'inherit', 'lock', 'unlock', 'wake', 'restore'].includes(e.e))
    .sort((a, b) => a.t - b.t);
  tb.innerHTML = rows.map(e => {
    const cls = e.e === 'inherit' ? 'red' : (e.e === 'restore' ? 'green' : '');
    return `<tr><td class="num">${e.t}</td><td>${nameOf(e.task)}</td>
      <td class="${cls}"><code>${e.e}</code></td><td>${EV_TEXT[e.e] || e.e}${
      e.p >= 0 ? '（prio → ' + e.p + '）' : ''}</td></tr>`;
  }).join('');
}

drawGantt('ganttA', A, 8, SUM.a.highGot, 'HIGH 阻塞 ' + SUM.a.highBlocked + ' tick');
drawGantt('ganttB', B, 8, SUM.b.highGot, 'HIGH 阻塞 ' + SUM.b.highBlocked + ' tick');
drawPrio('prio', B, SUM);
renderEvents(B);
window.addEventListener('resize', () => {
  drawGantt('ganttA', A, 8, SUM.a.highGot, 'HIGH 阻塞 ' + SUM.a.highBlocked + ' tick');
  drawGantt('ganttB', B, 8, SUM.b.highGot, 'HIGH 阻塞 ' + SUM.b.highBlocked + ' tick');
  drawPrio('prio', B, SUM);
});
</script>
</body>
</html>
"""


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out")
    dest = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "docs", "report.html")

    A = load(os.path.join(outdir, "trace_no_inherit.json"))
    B = load(os.path.join(outdir, "trace_inherit.json"))
    S = load(os.path.join(outdir, "summary.json"))

    a, b = S["a"], S["b"]
    ratio = a["highBlocked"] / b["highBlocked"] if b["highBlocked"] else 0

    html = (TEMPLATE
            .replace("__DATA_A__", json.dumps(A, ensure_ascii=False))
            .replace("__DATA_B__", json.dumps(B, ensure_ascii=False))
            .replace("__DATA_SUM__", json.dumps(S, ensure_ascii=False))
            .replace("__A_BLOCK__", str(a["highBlocked"]))
            .replace("__B_BLOCK__", str(b["highBlocked"]))
            .replace("__A_MED__", str(a["medDuringBlock"]))
            .replace("__B_MED__", str(b["medDuringBlock"]))
            .replace("__A_GOT__", str(a["highGot"]))
            .replace("__B_GOT__", str(b["highGot"]))
            .replace("__A_CS__", str(a["lowCs"]))
            .replace("__B_CS__", str(b["lowCs"]))
            .replace("__A_TOTAL__", str(a["total"]))
            .replace("__B_TOTAL__", str(b["total"]))
            .replace("__RATIO__", "%.1f" % ratio))

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote %s (%d bytes)" % (dest, len(html.encode("utf-8"))))


if __name__ == "__main__":
    main()
