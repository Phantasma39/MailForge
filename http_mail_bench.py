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
import json
import poplib
import threading
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

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

def pop3_delete_all(host, port, user, password, timeout=30):
    p = poplib.POP3(host, port, timeout=timeout)
    p.user(user)
    p.pass_(password)
    count, _ = p.stat()
    for n in range(1, count + 1):
        p.dele(n)
    p.quit()
    return count

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
    args = ap.parse_args()

    tag = str(int(time.time()))[-8:]
    password = "Bench123456"
    recv_user = "netbench_recv_" + tag
    send_users = ["netbench_s%d_%s" % (i + 1, tag) for i in range(max(1, args.accounts))]
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
    print("[4/4] 结果:")
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
    return 0

if __name__ == "__main__":
    raise SystemExit(main())