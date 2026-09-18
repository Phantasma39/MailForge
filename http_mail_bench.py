#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MailForge HTTP 真实环境多账号收发压测
- 自动注册多个测试发件账号 + 1 个收件账号
- 通过 8080 /api/send 并发发送，可触发服务器内部 AES/ChaCha 加密
- 通过 POP3 1110 轮询收件账号，统计接收延迟
用法：
  python http_mail_bench.py --host 140.143.233.15 --count 100 --size-kb 1024 --threads 4 --accounts 4 --algo aes
"""
import argparse
import datetime
import html as html_mod
import json
import os
import subprocess
import sys
import webbrowser
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass
import poplib
import threading
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

# poplib 默认单行上限约 2KB；压测邮件正文可能是一整行，这里放大到 16MB。
poplib._MAXLINE = 16 * 1024 * 1024

def http_post(host, port, path, data, timeout=180):
    body = urllib.parse.urlencode(data).encode("utf-8")
    req = urllib.request.Request("http://%s:%d%s" % (host, port, path), data=body)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))

def ensure_account(host, port, user, password):
    r = http_post(host, port, "/api/register", {"user": user, "pass": password})
    if r.get("ok"):
        return r["token"]
    r = http_post(host, port, "/api/login", {"user": user, "pass": password})
    if r.get("ok"):
        return r["token"]
    raise RuntimeError("账号 %s 注册/登录失败: %s" % (user, r.get("msg")))

def pop3_uids(host, port, user, password, timeout=30):
    p = poplib.POP3(host, port, timeout=timeout)
    p.user(user)
    p.pass_(password)
    lines = p.uidl()[1]
    uids = set()
    for line in lines:
        parts = line.split()
        if len(parts) >= 2:
            uids.add(parts[1].decode("utf-8", "ignore"))
    p.quit()
    return uids

def pop3_uid_map(host, port, user, password, timeout=30):
    p = poplib.POP3(host, port, timeout=timeout)
    p.user(user)
    p.pass_(password)
    lines = p.uidl()[1]
    result = {}
    for line in lines:
        parts = line.split()
        if len(parts) >= 2:
            result[parts[1].decode("utf-8", "ignore")] = int(parts[0])
    p.quit()
    return result

def download_batch(host, port, user, password, uids, timeout=60):
    """一个线程只开一个 POP3 连接，顺序下载分配给自己的 UID。"""
    results = []
    t_conn0 = time.perf_counter()
    try:
        p = poplib.POP3(host, port, timeout=timeout)
        p.user(user)
        p.pass_(password)
        lines = p.uidl()[1]
        uid_map = {}
        for line in lines:
            parts = line.split()
            if len(parts) >= 2:
                uid_map[parts[1].decode("utf-8", "ignore")] = int(parts[0])

        for uid in uids:
            t0 = time.perf_counter()
            try:
                if uid not in uid_map:
                    results.append((False, 0.0, 0, "uid not found"))
                    continue
                num = uid_map[uid]
                lines = p.retr(num)[1]
                size = sum(len(x) + 2 for x in lines)
                results.append((True, (time.perf_counter() - t0) * 1000.0, size, ""))
            except Exception as e:
                results.append((False, (time.perf_counter() - t0) * 1000.0, 0, str(e)))
        p.quit()
    except Exception as e:
        # 连接阶段失败：把整个批次标记为失败
        for _ in uids:
            results.append((False, (time.perf_counter() - t_conn0) * 1000.0, 0, str(e)))
        try:
            p.quit()
        except Exception:
            pass
    return results

def tex_escape(s):
    s = str(s)
    return (s.replace('\\', r'\textbackslash{}')
             .replace('&', r'\&')
             .replace('%', r'\%')
             .replace('$', r'\$')
             .replace('#', r'\#')
             .replace('_', r'\_')
             .replace('{', r'\{')
             .replace('}', r'\}'))

def write_pdf_report(result, args, pdf_path):
    send = float(result.get('sendAvgMs', 0) or 0)
    detect = float(result.get('avgDetectionMs', 0) or 0)
    download = float(result.get('downloadAvgMs', 0) or 0)
    total = float(result.get('totalAvgMs', 0) or 0)
    send_pass = send < 2000
    detect_pass = detect < 2000
    download_pass = result.get('downloadOk', 0) > 0 and download < 2000
    total_pass = total > 0 and total < 2000

    def yesno(ok):
        return '是' if ok else '否'

    rows = []
    rows.append(('发送成功', '%d / %d' % (int(result.get('sendOk', 0)), int(result.get('count', 0))), ''))
    rows.append(('发送成功率', '%.1f%%' % (100.0 * int(result.get('sendOk', 0)) / max(1, int(result.get('count', 0)))), ''))
    rows.append(('丢包率', '%.1f%%' % float(result.get('lossRate', 0) or 0), ''))
    rows.append(('平均发送', '%.0f ms' % send, yesno(send_pass)))
    rows.append(('平均检测', '%.0f ms' % detect, yesno(detect_pass)))
    if result.get('downloadSample', 0) > 0:
        rows.append(('平均下载', '%.0f ms' % download, yesno(download_pass)))
        rows.append(('上传+下载合计', '%.0f ms' % total, yesno(total_pass)))
    else:
        rows.append(('平均下载', '未测试', ''))
        rows.append(('上传+下载合计', '未测试', ''))

    table_rows = []
    for name, value, ok in rows:
        table_rows.append('%s & %s & %s \\\\' % (tex_escape(name), tex_escape(value), tex_escape(ok)))
    table_body = '\n'.join(table_rows)

    per_rows = []
    for account, st in (result.get('perSender') or {}).items():
        per_rows.append('%s & %d & %d \\\\' % (tex_escape(account), int(st.get('ok', 0)), int(st.get('fail', 0))))
    per_body = '\n'.join(per_rows) or '无数据 & & \\\\'

    tex = r'''\documentclass[12pt]{article}
\usepackage[UTF8]{ctex}
\usepackage[margin=2.2cm]{geometry}
\begin{document}
\begin{center}
{\Large MailForge 压测结果}
\end{center}

目标：%(host)s\quad 邮件：%(count)d 封 × %(size)d KB\quad 线程：%(threads)d\quad 账号：%(accounts)d\quad 加密：%(algo)s

\vspace{0.6cm}
\begin{tabular}{|l|l|l|}
\hline
项目 & 结果 & 是否满足 <2s \\
\hline
%(table)s
\hline
\end{tabular}

\vspace{1cm}
\textbf{各发件账号统计}

\vspace{0.3cm}
\begin{tabular}{|l|l|l|}
\hline
账号 & 成功 & 失败 \\
\hline
%(per)s
\hline
\end{tabular}

\vspace{0.8cm}
说明：发送和检测反映服务器处理速度；完整下载受公网带宽影响。

\end{document}
''' % {
        'host': tex_escape(result.get('host', '')),
        'count': int(result.get('count', 0)),
        'size': int(result.get('sizeKB', 0)),
        'threads': int(result.get('threads', 0)),
        'accounts': int(result.get('accounts', 0)),
        'algo': tex_escape(result.get('algo', '')),
        'table': table_body,
        'per': per_body,
    }

    tex_path = pdf_path[:-4] + '.tex'
    with open(tex_path, 'w', encoding='utf-8') as f:
        f.write(tex)

    try:
        proc = subprocess.run(
            ['xelatex', '-interaction=nonstopmode', '-halt-on-error', os.path.basename(tex_path)],
            cwd=os.path.dirname(os.path.abspath(tex_path)),
            capture_output=True, text=True, timeout=60)
        if proc.returncode != 0:
            print('[PDF] xelatex 编译失败，保留 tex 文件：' + tex_path)
            print(proc.stdout[-1000:])
            return None
        for ext in ['.aux', '.log', '.out', '.toc']:
            try:
                os.remove(pdf_path[:-4] + ext)
            except OSError:
                pass
        return pdf_path
    except FileNotFoundError:
        print('[PDF] 未找到 xelatex，请安装 TeX Live 或 MiKTeX；已保留 tex 文件：' + tex_path)
        return None
    except Exception as e:
        print('[PDF] 生成失败：%s；已保留 tex 文件：%s' % (e, tex_path))
        return None

def write_html_report(result, args, path):
    send = float(result.get("sendAvgMs", 0) or 0)
    detect = float(result.get("avgDetectionMs", 0) or 0)
    download = float(result.get("downloadAvgMs", 0) or 0)
    total = float(result.get("totalAvgMs", 0) or 0)
    send_pass = send < 2000
    detect_pass = detect < 2000
    download_pass = result.get("downloadOk", 0) > 0 and download < 2000
    total_pass = total > 0 and total < 2000

    def mark(ok):
        return '<span class="ok">满足</span>' if ok else '<span class="bad">不满足</span>'

    rows = []
    rows.append(("发送成功", "%d / %d" % (int(result.get("sendOk", 0)), int(result.get("count", 0))), ""))
    rows.append(("发送成功率", "%.1f%%" % (100.0 * int(result.get("sendOk", 0)) / max(1, int(result.get("count", 0)))), ""))
    rows.append(("丢包率", "%.1f%%" % float(result.get("lossRate", 0) or 0), ""))
    rows.append(("平均发送", "%.0f ms" % send, mark(send_pass)))
    rows.append(("平均检测", "%.0f ms" % detect, mark(detect_pass)))
    if result.get("downloadSample", 0) > 0:
        rows.append(("平均下载", "%.0f ms" % download, mark(download_pass)))
        rows.append(("上传+下载合计", "%.0f ms" % total, mark(total_pass)))
    else:
        rows.append(("平均下载", "未测试", ""))
        rows.append(("上传+下载合计", "未测试", ""))

    body_rows = "".join(
        "<tr><td>%s</td><td>%s</td><td>%s</td></tr>" % (html_mod.escape(str(k)), html_mod.escape(str(v)), extra)
        for k, v, extra in rows
    )

    per_rows = []
    for account, st in (result.get("perSender") or {}).items():
        per_rows.append(
            "<tr><td>%s</td><td>%d</td><td>%d</td></tr>" %
            (html_mod.escape(str(account)), int(st.get("ok", 0)), int(st.get("fail", 0)))
        )
    per_table = "".join(per_rows) or '<tr><td colspan="3">无数据</td></tr>'

    report = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>MailForge 压测结果</title>
<style>
  body{margin:0;background:#f7f8fa;color:#222;font:15px/1.7 system-ui,"Microsoft YaHei",sans-serif}
  .wrap{max-width:760px;margin:40px auto;padding:0 16px}
  h1{font-size:22px;margin:0 0 6px}
  .info{color:#666;margin-bottom:18px}
  table{width:100%%;border-collapse:collapse;background:#fff;border:1px solid #ddd;margin-bottom:22px}
  th,td{border:1px solid #ddd;padding:10px 12px;text-align:left}
  th{background:#f0f2f5}
  .ok{color:#15803d;font-weight:600}
  .bad{color:#b91c1c;font-weight:600}
</style>
</head>
<body>
<div class="wrap">
  <h1>MailForge 压测结果</h1>
  <div class="info">目标：%s　|　%d 封 × %d KB　|　%d 线程　|　%d 账号　|　加密：%s</div>

  <table>
    <tr><th>项目</th><th>结果</th><th>是否满足 &lt;2s</th></tr>
    %s
  </table>

  <h2>各发件账号统计</h2>
  <table>
    <tr><th>账号</th><th>成功</th><th>失败</th></tr>
    %s
  </table>
</div>
</body>
</html>""" % (
        html_mod.escape(str(result.get('host', ''))),
        int(result.get('count', 0)),
        int(result.get('sizeKB', 0)),
        int(result.get('threads', 0)),
        int(result.get('accounts', 0)),
        html_mod.escape(str(result.get('algo', ''))),
        body_rows,
        per_table,
    )
    with open(path, 'w', encoding='utf-8') as f:
        f.write(report)
    return path

def pop3_delete_all(host, port, user, password, timeout=30):
    p = poplib.POP3(host, port, timeout=timeout)
    p.user(user)
    p.pass_(password)
    count, _ = p.stat()
    for n in range(1, count + 1):
        p.dele(n)
    p.quit()
    return count

def ask(prompt, default):
    s = input("%s [%s]: " % (prompt, default)).strip()
    return s if s else str(default)

def ask_int(prompt, default, low, high):
    while True:
        s = ask(prompt, default)
        try:
            v = int(s)
        except ValueError:
            print("  请输入数字。")
            continue
        if low <= v <= high:
            return v
        print("  请输入 %d~%d 之间的数字。" % (low, high))

def ask_choice(prompt, default, choices):
    cset = [c.lower() for c in choices]
    while True:
        s = ask(prompt, default).strip().lower()
        if s in cset:
            return s
        print("  请输入：" + " / ".join(choices))

def interactive_setup(args):
    print("=" * 60)
    print("MailForge 压测交互模式")
    print("直接回车使用默认值。")
    print("=" * 60)
    args.host = ask("服务器地址", args.host)
    args.http_port = ask_int("HTTP 端口", args.http_port, 1, 65535)
    args.count = ask_int("发送邮件数", args.count, 1, 10000)
    args.size_kb = ask_int("每封邮件大小 KB", args.size_kb, 1, 4096)
    args.threads = ask_int("客户端发送线程数", args.threads, 1, 64)
    args.accounts = ask_int("发件账号数量", args.accounts, 1, 8)
    args.algo = ask_choice("加密方式 none/aes/chacha", args.algo, ["none", "aes", "chacha"])
    args.download_count = ask_int("下载测速抽样封数（0=不下载）", args.download_count, 0, 10000)
    if args.download_count != 0:
        args.download_threads = ask_int("下载并发连接数", args.download_threads, 1, 16)
    print()
    print("配置确认：")
    print("  服务器：%s:%d" % (args.host, args.http_port))
    print("  邮件：%d 封 × %d KB" % (args.count, args.size_kb))
    print("  发送：%d 线程，%d 个账号" % (args.threads, args.accounts))
    print("  加密：%s" % args.algo)
    print("  下载抽样：%d 封，并发 %d" % (args.download_count, args.download_threads))
    input("按回车开始测试...")

# ===========================================================================
#  矩阵模式：账号数 × 客户端并发 × 加密方式 ×【服务器线程池】
#    - 服务器线程池通过 /api/admin/threads 在线调整，无需重启服务器进程
#    - 单次模式（原有的 --threads/--accounts/--algo）逻辑完全不变
# ===========================================================================

def http_get(host, port, path, timeout=10):
    with urllib.request.urlopen("http://%s:%d%s" % (host, port, path), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))

def get_server_threads(host, http_port, token, timeout=10):
    """读取服务器当前线程池 worker 数（GET /api/admin/threads）。"""
    r = http_get(host, http_port,
                 "/api/admin/threads?" + urllib.parse.urlencode({"adminToken": token}),
                 timeout=timeout)
    if not r.get("ok"):
        raise RuntimeError("读取服务器线程数失败: %s" % r.get("msg"))
    return int(r.get("threads"))

def set_server_threads(host, http_port, n, token, timeout=15):
    """在线设置服务器线程池大小（POST /api/admin/threads），不需要重启服务器：
    服务器会新建线程池接替旧池，正在处理的连接不受影响，新连接立即按新线程数处理。"""
    r = http_post(host, http_port, "/api/admin/threads",
                  {"adminToken": token, "threads": int(n)}, timeout=timeout)
    if not r.get("ok"):
        raise RuntimeError("设置服务器线程数失败: %s" % r.get("msg"))
    got = get_server_threads(host, http_port, token, timeout=timeout)
    if got != int(n):
        raise RuntimeError("线程数读回不一致：期望 %d，实际 %d" % (int(n), got))
    return got

def algo_label(algo):
    return {"none": "明文", "aes": "AES-256-CBC", "chacha": "ChaCha20",
            "chacha20": "ChaCha20"}.get(str(algo).lower(), str(algo))

def build_matrix_cases(accounts, server_thread_modes, algos, threads_override=None):
    """生成测试矩阵：账号数 × (服务器线程池) × 加密方式。

    ★ 并发规则：客户端并发数 = 发件账号数
        · 1 个账号 → 1 路并发（连续发送）
        · 4 个账号 → 4 路并发（4 个账号同时各发一部分，合计仍为 --count 封）
    threads_override 只在单账号需要额外并发档位时才用（一般不用）。
    """
    cases = []
    st_modes = list(server_thread_modes) if server_thread_modes else [None]
    for acc in accounts:
        th = int(threads_override) if threads_override else int(acc)
        for st in st_modes:
            for algo in algos:
                cases.append({"accounts": acc, "threads": th,
                              "algo": algo, "serverThreads": st})
    return cases

def write_matrix_latex(rows, host, count, size_kb, out_path, server_modes):
    """把矩阵结果写成一张 LaTeX 汇总表（xelatex 可直接编译）。

    列：加密方式 | 服务器线程池 | 发送速率(封/s) | 平均检测(ms) | 下载速率(MB/s) | 平均每封传输(ms)
    """
    def fmt(v, nd=1):
        try:
            return ("%%.%df" % nd) % float(v)
        except Exception:
            return "--"

    algo_order = [("none", "明文"), ("aes", "AES-256-CBC"), ("chacha", "ChaCha20")]
    sts = [int(s) for s in server_modes] if server_modes else [None]
    body = []
    for algo, label in algo_order:
        first = True
        for st in sts:
            row = None
            for r in rows:
                if (str(r.get("algo", "")).lower() == algo
                        and (r.get("serverThreads") == st)):
                    row = r
                    break
            if row is None:
                cells = ["--"] * 5
            else:
                tested = (row.get("downloadSample") or 0) > 0
                cells = [
                    fmt(row.get("sendThroughput"), 2),                       # 封/s
                    fmt(row.get("avgDetectionMs"), 0),                       # 检测 ms
                    fmt(row.get("downloadMBps"), 2) if tested else "--",     # 下载 MB/s
                    fmt(row.get("totalAvgMs"), 0) if tested else "--",        # 每封 ms
                    "%.1f" % (100.0 * row["sendOk"] / max(1, row["count"])) + r"\%",
                ]
            lbl = (r"\multirow{%d}{*}{%s}" % (len(sts), label)) if first else ""
            body.append("%s & %s & %s \\\\" % (lbl, (st if st else "不变"),
                                               " & ".join(cells)))
            first = False

    tex = r"""%% MailForge 邮件传输速率压测汇总表（由 http_mail_bench.py 自动生成）
\documentclass[11pt]{ctexart}
\usepackage[a4paper,landscape,margin=1.6cm]{geometry}
\usepackage{booktabs}
\usepackage{array}
\usepackage{multirow}
\pagestyle{empty}
\setlength{\parindent}{0pt}
\newcommand{\hd}[1]{\textbf{#1}}
\begin{document}
\begin{center}
{\Large\bfseries MailForge 邮件传输速率压测汇总表}\\[4pt]
{\small 每封 %(size)g MB，每组连续发送 %(count)d 封；目标 %(host)s；客户端并发 = 发件账号数（1 账号 1 并发 / 4 账号 4 并发）}
\end{center}
\vspace{0.3cm}
\begin{center}
\begin{tabular}{llccccc}
\toprule
\multirow{2}{*}{\hd{加密方式}} & \multirow{2}{*}{\hd{服务器线程池}} &
\multirow{2}{*}{\hd{发送速率\\(封/s)}} & \multirow{2}{*}{\hd{平均检测\\(ms)}} &
\multirow{2}{*}{\hd{下载速率\\(MB/s)}} & \multirow{2}{*}{\hd{平均每封传输\\(ms)}} &
\multirow{2}{*}{\hd{成功率}} \\
\midrule
%(body)s
\bottomrule
\end{tabular}
\end{center}
\vspace{0.3cm}
{\small 说明：发送速率＝成功封数÷发送墙钟；平均检测＝收件端 POP3 轮询发现邮件的平均延迟；\\
下载速率＝下载总字节÷下载墙钟；平均每封传输＝(开始发送→全部下载完成)÷接收封数（未测下载时为 --）。}
\end{document}
""" % {"size": float(size_kb) / 1024.0, "count": int(count),
       "host": host, "body": "\n".join(body)}

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(tex)
    return out_path

def test_one_case(args, recv_user, recv_email, send_users, sender_tokens,
                  run_id, password):
    """跑一组测试（发送 → 收件 → 抽样下载），返回统计字典。

    与单次模式使用同一套统计口径：sendWallMs / sendAvgMs / sendThroughput /
    avgDetectionMs / downloadAvgMs / downloadMBps / totalAvgMs 等字段名保持一致。
    """
    # 需要时先在线调整服务器线程池（不重启服务器）
    server_threads = None
    if getattr(args, "server_threads", None):
        try:
            server_threads = set_server_threads(args.host, args.http_port,
                                                args.server_threads, args.admin_token)
            print("  [服务器] 线程池已在线调整为 %d（进程未重启）" % server_threads)
        except Exception as e:
            print("  [warn] 调整服务器线程池失败（继续用原线程数）: %s" % e)

    # 清空收件账号，保证每组都从同一基线开始
    try:
        pop3_delete_all(args.host, args.pop3_port, recv_user, password)
    except Exception as e:
        print("  [warn] 清理收件箱失败: %s" % e)

    baseline = pop3_uids(args.host, args.pop3_port, recv_user, password)
    body = "A" * max(1, args.size_kb * 1024)

    send_results = [None] * args.count
    stop_flag = threading.Event()
    recv_events = []
    recv_lock = threading.Lock()

    def receiver():
        seen = set(baseline)
        while not stop_flag.is_set():
            try:
                now = pop3_uids(args.host, args.pop3_port, recv_user, password)
                new_uids = now - seen
                if new_uids:
                    with recv_lock:
                        for uid in new_uids:
                            recv_events.append((uid, time.perf_counter()))
                    seen |= new_uids
                if len(recv_events) >= args.count:
                    return
            except Exception:
                pass
            time.sleep(max(0.05, args.poll))

    recv_thread = threading.Thread(target=receiver, daemon=True)
    recv_thread.start()

    def send_one(i):
        token = sender_tokens[i % len(sender_tokens)]
        data = {
            "token": token,
            "to": recv_email,
            "subject": "[%s] #%d" % (run_id, i + 1),
            "body": body,
        }
        if args.algo != "none":
            data["encrypt"] = "1"
            data["algo"] = "chacha" if args.algo in ("chacha", "chacha20") else "aes"
        t0 = time.perf_counter()
        try:
            r = http_post(args.host, args.http_port, "/api/send", data,
                          timeout=max(60, int(args.timeout)))
            ok = bool(r.get("ok"))
            msg = r.get("msg", "")
        except Exception as e:
            ok = False
            msg = str(e)
        t1 = time.perf_counter()
        send_results[i] = {
            "ok": ok,
            "start": t0,
            "ms": (t1 - t0) * 1000.0,
            "sender": send_users[i % len(send_users)],
            "msg": msg,
        }

    send_wall_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=max(1, args.threads)) as ex:
        futs = [ex.submit(send_one, i) for i in range(args.count)]
        for f in as_completed(futs):
            f.result()
    send_wall_end = time.perf_counter()

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        with recv_lock:
            n = len(recv_events)
        if n >= args.count or not recv_thread.is_alive():
            break
        time.sleep(0.1)
    stop_flag.set()
    recv_thread.join(timeout=3.0)

    ok = sum(1 for r in send_results if r and r["ok"])
    send_ms = sum(r["ms"] for r in send_results if r and r["ok"])
    send_wall = send_wall_end - send_wall_start
    with recv_lock:
        received = len(recv_events)
        events = sorted(recv_events, key=lambda x: x[1])
    recv_done = events[-1][1] if events else None
    first_send = min((r["start"] for r in send_results if r), default=None)
    e2e_total = (recv_done - first_send) if (recv_done and first_send) else 0.0

    result = {
        "host": args.host,
        "count": args.count,
        "sizeKB": args.size_kb,
        "threads": args.threads,
        "accounts": args.accounts,
        "algo": args.algo,
        "sendOk": ok,
        "sendFail": args.count - ok,
        "sendWallMs": send_wall * 1000.0,
        "sendAvgMs": send_ms / max(1, ok),
        "sendThroughput": ok / max(0.001, send_wall),
        "received": received,
        "lossRate": (args.count - received) * 100.0 / max(1, args.count),
        "e2eTotalMs": e2e_total * 1000.0,
        "avgReceiveMs": (e2e_total / received * 1000.0) if received else 0.0,
        "serverThreads": server_threads,
    }

    # 抽样下载（与原版同一口径：downloadAvgMs / downloadMBps）
    download_ok = 0
    download_fail = 0
    download_ms_sum = 0.0
    download_bytes = 0
    download_wall_ms = 0.0
    download_sample = 0
    if args.download_count != 0 and received > 0:
        n_download = len(events) if args.download_count < 0 else min(args.download_count, len(events))
        if n_download > 0:
            uids = [uid for uid, _ in events[:n_download]]
            n_threads = max(1, min(args.download_threads, len(uids)))
            chunks = []
            for wi in range(n_threads):
                b = len(uids) * wi // n_threads
                e = len(uids) * (wi + 1) // n_threads
                if b < e:
                    chunks.append(uids[b:e])
            print("  [下载测速] 抽样 %d 封，%d 个 POP3 并发连接 ..." % (n_download, len(chunks)))
            dwall0 = time.perf_counter()
            all_results = []
            with ThreadPoolExecutor(max_workers=len(chunks)) as ex:
                futs = [ex.submit(download_batch, args.host, args.pop3_port, recv_user,
                                  password, chunk, max(60, int(args.timeout)))
                        for chunk in chunks]
                for f in as_completed(futs):
                    all_results.extend(f.result())
            download_wall_ms = (time.perf_counter() - dwall0) * 1000.0
            for okd, ms, nbytes, err in all_results:
                if okd:
                    download_ok += 1
                    download_ms_sum += ms
                    download_bytes += nbytes
                else:
                    download_fail += 1
            download_sample = n_download

    result["downloadSample"] = download_sample
    result["downloadOk"] = download_ok
    result["downloadFail"] = download_fail
    result["downloadWallMs"] = download_wall_ms
    result["downloadAvgMs"] = download_ms_sum / max(1, download_ok)
    result["downloadMBps"] = (download_bytes / 1048576.0) / max(0.001, download_wall_ms / 1000.0)
    result["totalAvgMs"] = (result["sendAvgMs"] + result["downloadAvgMs"]) if download_ok > 0 else 0.0
    # 与单次模式保持同名键：检测延迟 = 端到端平均接收延迟
    result["avgDetectionMs"] = result["avgReceiveMs"]
    result["detectionTotalMs"] = result["e2eTotalMs"]

    # 清理本组邮件，避免影响下一组
    try:
        pop3_delete_all(args.host, args.pop3_port, recv_user, password)
    except Exception:
        pass
    return result

def print_matrix_table(rows, host, count, size_kb):
    """把矩阵结果汇总成一张窄表（避免终端折行）。未测下载的列显示“未测”。"""
    print()
    print("=" * 96)
    print("MailForge 压测汇总（目标 %s，每封 %d KB，共 %d 封/组）" % (host, size_kb, count))
    print("=" * 96)
    print("%-2s %-4s %-4s %-11s %-7s %-8s %-9s %-7s %-6s" %
          ("#", "账号", "服务线", "加密", "封/s", "检测ms", "下载MB/s", "每封ms", "成功"))
    print("-" * 96)
    for i, r in enumerate(rows, 1):
        tested_dl = (r.get("downloadSample") or 0) > 0
        dl = ("%.2f" % r["downloadMBps"]) if tested_dl else "未测"
        per_mail = ("%.0f" % r["totalAvgMs"]) if tested_dl else "未测"
        print("%-2d %-4d %-4s %-11s %7.2f %8.0f %9s %7s %5.1f%%" %
              (i, r["accounts"],
               str(r.get("serverThreads") if r.get("serverThreads") else "-"),
               algo_label(r["algo"]),
               r["sendThroughput"], r["avgDetectionMs"], dl, per_mail,
               100.0 * r["sendOk"] / max(1, r["count"])))
    print("=" * 96)
    print("封/s = 发送速率；检测ms = POP3 发现邮件的平均延迟；下载MB/s = 下载字节÷下载耗时；")
    print("每封ms = 发送平均 + 下载平均（未测下载时无意义，用 --download-count -1 可测）。")
    print("=" * 96)

def run_matrix(args):
    """矩阵模式：遍历【账号数(=客户端并发) × 服务器线程池 × 加密方式】。"""
    try:
        account_modes = [int(x) for x in str(args.matrix_accounts).split(",") if x.strip()]
        algo_modes = [x.strip().lower() for x in str(args.matrix_algos).split(",") if x.strip()]
        server_thread_modes = [int(x) for x in str(args.matrix_server_threads).split(",") if x.strip()]
        # 兼容旧参数：--matrix-threads 只在单账号需要额外并发档位时才用
        thread_override = None
        if str(getattr(args, "matrix_threads", "")).strip() not in ("", "auto"):
            vals = [int(x) for x in str(args.matrix_threads).split(",") if x.strip()]
            if len(vals) == 1:
                thread_override = vals[0]
            else:
                print("[warn] 并发数已按“账号数=并发数”自动绑定，忽略 --matrix-threads=%s" %
                      args.matrix_threads)
    except ValueError:
        print("--matrix-accounts / --matrix-server-threads 需要形如 1,4 的数字列表")
        return 2
    if not account_modes or not algo_modes:
        print("矩阵参数不能为空")
        return 2

    cases = build_matrix_cases(account_modes, server_thread_modes, algo_modes, thread_override)
    tag = str(int(time.time()))[-8:]
    password = "Bench123456"
    recv_user = "netbench_recv"
    recv_email = recv_user + "@example.com"
    all_send_users = ["netbench_s%d" % (i + 1) for i in range(max(account_modes))]

    print("=" * 96)
    print("MailForge 压测矩阵：%d 组（账号 %s × %s × 加密 %s）" %
          (len(cases), account_modes,
           ("服务器线程池 %s" % server_thread_modes) if server_thread_modes else "服务器线程池不变",
           [algo_label(a) for a in algo_modes]))
    print("每组：%d 封 × %d KB   目标：%s:%d" %
          (args.count, args.size_kb, args.host, args.http_port))
    print("并发规则：客户端并发数 = 发件账号数（%s）" %
          "、".join("%d 账号→%d 并发" % (a, thread_override or a) for a in account_modes))
    if server_thread_modes:
        print("说明：服务器线程池通过 /api/admin/threads 在线调整，无需重启服务器进程（范围 1~32）")
    print("=" * 96)

    print("[准备] 注册/登录测试账号 ...")
    ensure_account(args.host, args.http_port, recv_user, password)
    token_cache = {}

    def token_of(u):
        if u not in token_cache:
            token_cache[u] = ensure_account(args.host, args.http_port, u, password)
        return token_cache[u]

    for u in all_send_users:
        token_of(u)
    print("       收件账号：%s" % recv_user)
    print("       发件账号：%s" % ", ".join(all_send_users))
    if server_thread_modes:
        try:
            print("       服务器当前线程池：%d（测试中按需在线调整）" %
                  get_server_threads(args.host, args.http_port, args.admin_token))
        except Exception as e:
            print("       [warn] 读取服务器线程数失败：%s" % e)

    rows = []
    for idx, case in enumerate(cases, 1):
        print()
        print("-" * 96)
        print("[%d/%d] 账号=%d（并发=%d）  服务器线程池=%s  加密=%s" %
              (idx, len(cases), case["accounts"], case["threads"],
               case["serverThreads"] if case["serverThreads"] else "不变",
               algo_label(case["algo"])))
        print("-" * 96)
        send_users = all_send_users[:case["accounts"]]
        tokens = [token_of(u) for u in send_users]
        args.accounts = case["accounts"]
        args.threads = case["threads"]
        args.algo = case["algo"]
        args.server_threads = case["serverThreads"]
        run_id = "m%s-a%d-c%d-s%s-%s" % (tag, case["accounts"], case["threads"],
                                         case["serverThreads"], case["algo"])
        r = test_one_case(args, recv_user, recv_email, send_users, tokens, run_id, password)
        rows.append(r)
        print("  → 发送 %d/%d  接收 %d  发送速度 %.2f 封/s  平均发送 %.0f ms%s" %
              (r["sendOk"], r["count"], r["received"], r["sendThroughput"], r["sendAvgMs"],
               ("  下载 %.0f ms / %.2f MB/s" % (r["downloadAvgMs"], r["downloadMBps"]))
               if r["downloadSample"] > 0 else ""))

    print_matrix_table(rows, args.host, args.count, args.size_kb)

    # ---- LaTeX 汇总表 ----
    if not args.no_tex:
        tex_path = args.tex_out or "MailForge_Bench_Table.tex"
        try:
            write_matrix_latex(rows, args.host, args.count, args.size_kb,
                               tex_path, server_thread_modes)
            print("LaTeX 汇总表已生成：%s" % os.path.abspath(tex_path))
            try:
                p = subprocess.run(["xelatex", "-interaction=nonstopmode", "-halt-on-error",
                                    os.path.basename(tex_path)],
                                   cwd=os.path.dirname(os.path.abspath(tex_path)) or ".",
                                   capture_output=True, text=True, timeout=180)
                if p.returncode == 0:
                    print("已编译出 PDF：%s" % os.path.abspath(tex_path[:-4] + ".pdf"))
                    for ext in (".aux", ".log", ".out"):
                        try:
                            os.remove(tex_path[:-4] + ext)
                        except OSError:
                            pass
                else:
                    print("（tex 已生成，但 xelatex 编译未成功；tex 可直接使用）")
            except Exception:
                print("（未找到 xelatex，tex 文件可直接拿去用）")
        except Exception as e:
            print("[warn] 生成 LaTeX 表失败：%s" % e)

    if not args.keep:
        try:
            deleted = pop3_delete_all(args.host, args.pop3_port, recv_user, password)
            print("[cleanup] 已清空收件账号 %d 封测试邮件" % deleted)
        except Exception as e:
            print("[cleanup] 清理失败: %s" % e)
    return 0

def main():
    ap = argparse.ArgumentParser(description="MailForge HTTP multi-account send/receive benchmark")
    ap.add_argument("--host", default="140.143.233.15")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--pop3-port", type=int, default=1110)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--size-kb", type=int, default=1024, help="每封邮件正文大小 KB")
    ap.add_argument("--threads", type=int, default=4, help="HTTP 并发发送线程数")
    ap.add_argument("--accounts", type=int, default=4, help="发件账号数量")
    ap.add_argument("--algo", default="aes", choices=["none", "aes", "chacha", "chacha20"])
    ap.add_argument("--poll", type=float, default=0.2, help="POP3 轮询间隔秒")
    ap.add_argument("--timeout", type=float, default=300.0, help="整体超时秒")
    ap.add_argument("--keep", action="store_true", help="保留测试账号和邮件，默认清理收件账号邮件")
    ap.add_argument("--download-count", type=int, default=10,
                    help="下载测速的邮件封数，0 表示全部下载")
    ap.add_argument("--download-threads", type=int, default=1,
                    help="下载测速并发连接数，默认 1 更接近单封真实下载时间")
    ap.add_argument("--json", action="store_true", help="额外打印原始 JSON")
    ap.add_argument("--report", default="", help="PDF 报告输出路径，默认按当前时间命名")
    ap.add_argument("--no-open", action="store_true", help="兼容参数，PDF 生成后不自动打开")
    ap.add_argument("--interactive", action="store_true", help="使用交互式问答模式")
    ap.add_argument("--matrix", action="store_true",
                    help="矩阵模式：账号数(=客户端并发) × 服务器线程池 × 加密方式，最后打印汇总表并生成 LaTeX 表")
    ap.add_argument("--matrix-accounts", default="1,4",
                    help="矩阵模式的账号数列表，默认 1,4；客户端并发自动等于账号数（1账号1并发、4账号4并发）")
    ap.add_argument("--matrix-threads", default="auto",
                    help="并发数，默认 auto＝跟随账号数；只有单账号要额外并发档位时才填（如 4）")
    ap.add_argument("--matrix-algos", default="none,aes,chacha",
                    help="矩阵模式的加密方式列表，默认 none,aes,chacha")
    ap.add_argument("--matrix-server-threads", default="",
                    help="矩阵模式下要在线设置的【服务器线程池】大小列表（如 1,4）；留空表示不改服务器线程池")
    ap.add_argument("--admin-token", default="mailforge-admin",
                    help="管理员令牌，用于在线调整服务器线程池（对应 MAILFORGE_ADMIN_TOKEN）")
    ap.add_argument("--tex-out", default="MailForge_Bench_Table.tex",
                    help="矩阵模式输出的 LaTeX 汇总表路径")
    ap.add_argument("--no-tex", action="store_true",
                    help="矩阵模式不生成 LaTeX 表（默认生成，并尝试用 xelatex 编译成 PDF）")
    args = ap.parse_args()

    if args.matrix:
        return run_matrix(args)

    return main_single(args)

def main_single(args):
    if args.interactive or len(sys.argv) == 1:
        interactive_setup(args)

    # 固定测试账号：第一次运行自动注册，后续复用，避免账号数量不断增加。
    tag = str(int(time.time()))[-8:]
    password = "Bench123456"
    recv_user = "netbench_recv"
    send_users = ["netbench_s%d" % (i + 1) for i in range(max(1, args.accounts))]
    recv_email = recv_user + "@example.com"

    print("=" * 76)
    print("MailForge HTTP multi-account benchmark")
    print("target: %s:%d  accounts=%d  threads=%d  count=%d  size=%dKB  algo=%s" %
          (args.host, args.http_port, args.accounts, args.threads,
           args.count, args.size_kb, args.algo))
    print("=" * 76)

    print("[1/4] 注册/登录测试账号 ...")
    recv_token = ensure_account(args.host, args.http_port, recv_user, password)
    sender_tokens = []
    for u in send_users:
        sender_tokens.append(ensure_account(args.host, args.http_port, u, password))
    print("      收件账号: %s" % recv_user)
    print("      发件账号: %s" % ", ".join(send_users))

    baseline = pop3_uids(args.host, args.pop3_port, recv_user, password)
    print("[2/4] POP3 基准 UID 数: %d" % len(baseline))

    body = "A" * max(1, args.size_kb * 1024)
    run_id = "netbench-" + tag
    subjects = ["[%s] #%d" % (run_id, i + 1) for i in range(args.count)]

    send_results = [None] * args.count
    stop_flag = threading.Event()
    recv_events = []
    recv_lock = threading.Lock()
    recv_error = []

    def receiver():
        seen = set(baseline)
        while not stop_flag.is_set():
            try:
                now = pop3_uids(args.host, args.pop3_port, recv_user, password)
                new_uids = now - seen
                if new_uids:
                    with recv_lock:
                        for uid in new_uids:
                            recv_events.append((uid, time.perf_counter()))
                    seen |= new_uids
                if len(recv_events) >= args.count:
                    return
            except Exception as e:
                with recv_lock:
                    recv_error.append(str(e))
            time.sleep(max(0.05, args.poll))

    recv_thread = threading.Thread(target=receiver, daemon=True)
    recv_thread.start()

    def send_one(i):
        token = sender_tokens[i % len(sender_tokens)]
        data = {
            "token": token,
            "to": recv_email,
            "subject": subjects[i],
            "body": body,
        }
        if args.algo != "none":
            data["encrypt"] = "1"
            data["algo"] = "chacha" if args.algo in ("chacha", "chacha20") else "aes"
        t0 = time.perf_counter()
        try:
            r = http_post(args.host, args.http_port, "/api/send", data,
                          timeout=max(60, int(args.timeout)))
            ok = bool(r.get("ok"))
            msg = r.get("msg", "")
        except Exception as e:
            ok = False
            msg = str(e)
        t1 = time.perf_counter()
        send_results[i] = {
            "index": i,
            "ok": ok,
            "start": t0,
            "end": t1,
            "ms": (t1 - t0) * 1000.0,
            "sender": send_users[i % len(send_users)],
            "msg": msg,
        }

    print("[3/4] 并发发送 %d 封，约 %d KB/封 ..." % (args.count, args.size_kb))
    send_wall_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=max(1, args.threads)) as ex:
        futs = [ex.submit(send_one, i) for i in range(args.count)]
        for f in as_completed(futs):
            f.result()
    send_wall_end = time.perf_counter()

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        with recv_lock:
            n = len(recv_events)
        if n >= args.count:
            break
        if not recv_thread.is_alive():
            break
        time.sleep(0.1)
    stop_flag.set()
    recv_thread.join(timeout=3.0)

    ok = sum(1 for r in send_results if r and r["ok"])
    fail = args.count - ok
    send_ms = sum(r["ms"] for r in send_results if r and r["ok"])
    send_wall = send_wall_end - send_wall_start
    with recv_lock:
        received = len(recv_events)
        events = list(recv_events)
    events.sort(key=lambda x: x[1])
    recv_done = events[-1][1] if events else None
    first_send = min(r["start"] for r in send_results if r)
    e2e_total = (recv_done - first_send) if recv_done else 0.0
    avg_e2e = e2e_total / received if received else 0.0

    # 发件账号统计
    per_sender = {}
    for r in send_results:
        if not r:
            continue
        k = r["sender"]
        if k not in per_sender:
            per_sender[k] = {"ok": 0, "fail": 0}
        per_sender[k]["ok" if r["ok"] else "fail"] += 1

    result = {
        "host": args.host,
        "count": args.count,
        "sizeKB": args.size_kb,
        "threads": args.threads,
        "accounts": args.accounts,
        "algo": args.algo,
        "sendOk": ok,
        "sendFail": fail,
        "sendWallMs": send_wall * 1000.0,
        "sendAvgMs": send_ms / max(1, ok),
        "sendThroughput": ok / max(0.001, send_wall),
        "received": received,
        "lossRate": (args.count - received) * 100.0 / max(1, args.count),
        "recvDone": recv_done,
        "e2eTotalMs": e2e_total * 1000.0,
        "avgReceiveMs": avg_e2e * 1000.0,
        "perSender": per_sender,
    }
    download_ok = 0
    download_fail = 0
    download_ms_sum = 0.0
    download_bytes = 0
    download_wall_ms = 0.0
    download_count_actual = 0
    if args.download_count != 0 and received > 0:
        n_download = len(events) if args.download_count < 0 else min(args.download_count, len(events))
        if n_download > 0:
            uids_to_download = [uid for uid, _ in events[:n_download]]
            n_threads = max(1, min(args.download_threads, len(uids_to_download)))
            chunks = []
            for wi in range(n_threads):
                begin = len(uids_to_download) * wi // n_threads
                end = len(uids_to_download) * (wi + 1) // n_threads
                if begin < end:
                    chunks.append(uids_to_download[begin:end])
            print("\n[下载测速] 抽样 %d 封，并发 %d 个 POP3 连接 ..." % (n_download, len(chunks)))
            dwall0 = time.perf_counter()
            all_results = []
            with ThreadPoolExecutor(max_workers=len(chunks)) as ex:
                futs = [ex.submit(download_batch, args.host, args.pop3_port, recv_user,
                                  password, chunk, max(60, int(args.timeout))) for chunk in chunks]
                for f in as_completed(futs):
                    all_results.extend(f.result())
            download_wall_ms = (time.perf_counter() - dwall0) * 1000.0
            for okd, ms, nbytes, err in all_results:
                if okd:
                    download_ok += 1
                    download_ms_sum += ms
                    download_bytes += nbytes
                else:
                    download_fail += 1
                    if download_fail <= 5:
                        print("  [download fail] " + err)
            download_count_actual = n_download
            print("[下载测速] 成功=%d/%d 平均=%.1f ms 下载墙钟=%.1f ms" %
                  (download_ok, n_download,
                   download_ms_sum / max(1, download_ok), download_wall_ms))

    result["detectionTotalMs"] = result.get("e2eTotalMs", 0)
    result["avgDetectionMs"] = result.get("avgReceiveMs", 0)
    result["downloadSample"] = download_count_actual
    result["downloadOk"] = download_ok
    result["downloadFail"] = download_fail
    result["downloadWallMs"] = download_wall_ms
    result["downloadAvgMs"] = download_ms_sum / max(1, download_ok)
    result["downloadMBps"] = (download_bytes / 1048576.0) / max(0.001, download_wall_ms / 1000.0)
    if download_ok > 0:
        result["totalAvgMs"] = result["sendAvgMs"] + result["downloadAvgMs"]
    else:
        result["totalAvgMs"] = 0.0

    def mark(ok):
        return "满足" if ok else "不满足"

    send_avg_s = result["sendAvgMs"] / 1000.0
    detect_avg_s = result["avgDetectionMs"] / 1000.0
    download_avg_s = result["downloadAvgMs"] / 1000.0
    send_pass = result["sendAvgMs"] < 2000.0
    detect_pass = result["avgDetectionMs"] < 2000.0
    download_pass = (result["downloadOk"] > 0 and download_avg_s < 2.0)

    print()
    print("=" * 60)
    print("测试结果")
    print("=" * 60)
    print("目标服务器：%s" % args.host)
    print("发送账号数：%d" % args.accounts)
    print("发送线程数：%d" % args.threads)
    print("发送邮件数：%d" % args.count)
    print("每封大小：%d KB" % args.size_kb)
    print("加密方式：%s" % args.algo)
    print()
    print("【发送结果】")
    print("  发送成功：%d" % result["sendOk"])
    print("  发送失败：%d" % result["sendFail"])
    print("  发送成功率：%.1f%%" % (100.0 * result["sendOk"] / max(1, result["count"])))
    print("  发送总耗时：%.2f 秒" % (result["sendWallMs"] / 1000.0))
    print("  平均每封发送：%.0f 毫秒（%.2f 秒）" % (result["sendAvgMs"], send_avg_s))
    print("  发送速度：%.2f 封/秒" % result["sendThroughput"])
    print()
    print("【对方检测结果】")
    print("  检测到邮件：%d" % result["received"])
    print("  丢包率：%.1f%%" % result["lossRate"])
    print("  从开始发送到全部检测完成：%.2f 秒" % (result["detectionTotalMs"] / 1000.0))
    print("  平均检测延迟：%.0f 毫秒（%.2f 秒）" % (result["avgDetectionMs"], detect_avg_s))
    print()
    print("【完整下载结果】")
    if result["downloadSample"] > 0:
        print("  抽样下载：%d 封" % result["downloadSample"])
        print("  下载成功：%d" % result["downloadOk"])
        print("  下载失败：%d" % result["downloadFail"])
        print("  平均每封下载：%.0f 毫秒（%.2f 秒）" % (result["downloadAvgMs"], download_avg_s))
        print("  下载总耗时：%.2f 秒" % (result["downloadWallMs"] / 1000.0))
        print("  下载速度：%.2f MB/s" % result["downloadMBps"])
        print()
        print("【上传 + 下载总平均用时】")
        print("  上传平均：%.0f 毫秒（%.2f 秒）" % (result["sendAvgMs"], result["sendAvgMs"] / 1000.0))
        print("  下载平均：%.0f 毫秒（%.2f 秒）" % (result["downloadAvgMs"], result["downloadAvgMs"] / 1000.0))
        print("  合计平均：%.0f 毫秒（%.2f 秒）" % (result["totalAvgMs"], result["totalAvgMs"] / 1000.0))
        print("  上传+下载 <2s：%s" % ("满足" if result["totalAvgMs"] < 2000.0 else "不满足"))
    else:
        print("  未进行下载测速")
    print()
    print("【是否满足 2 秒要求】")
    print("  发送 <2s：%s" % mark(send_pass))
    print("  对方检测 <2s：%s" % mark(detect_pass))
    if result["downloadSample"] > 0:
        print("  完整下载 <2s：%s" % mark(download_pass))
    print()
    print("说明：发送和检测主要反映服务器处理速度；完整下载受公网带宽影响。")
    print("=" * 60)

    if args.json:
        print()
        print("原始 JSON：")
        print(json.dumps(result, ensure_ascii=False, indent=2))

    if not args.keep:
        try:
            deleted = pop3_delete_all(args.host, args.pop3_port, recv_user, password)
            print("[cleanup] 已删除收件账号 %d 封测试邮件" % deleted)
        except Exception as e:
            print("[cleanup] 清理失败: %s" % e)

    with open("http_mail_bench_result.json", "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    print("结果已写入 http_mail_bench_result.json")

    if args.report:
        report_path = os.path.abspath(args.report)
    else:
        stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        report_path = os.path.abspath("mail_bench_report_" + stamp + ".pdf")
    pdf_path = write_pdf_report(result, args, report_path)
    if pdf_path:
        print("PDF 报告已生成：%s" % pdf_path)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())