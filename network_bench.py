#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MailForge 真实网络压测脚本
从本机通过公网连接你的 MailServer：
  SMTP 2525 发信
  POP3 1110 收信
用法示例：
  python network_bench.py --host 140.143.233.15 --count 100 --size-mb 1 --threads 4 --accounts 4
注意：服务器安全组/防火墙必须放行 2525 和 1110。
"""
import argparse
import base64
import json
import poplib
import smtplib
import socket
import sys
import time
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from email.message import EmailMessage

def port_open(host, port, timeout=3.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True, ""
    except Exception as e:
        return False, str(e)

def http_post(host, port, path, data, timeout=10):
    body = urllib.parse.urlencode(data).encode("utf-8")
    req = urllib.request.Request("http://%s:%d%s" % (host, port, path), data=body)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))

def make_raw_mail(sender, recipient, subject, size_mb):
    msg = EmailMessage()
    msg["From"] = sender
    msg["To"] = recipient
    msg["Subject"] = subject
    msg.set_content("MailForge real network benchmark body")
    size = max(1, int(size_mb * 1024 * 1024))
    msg.add_attachment(b"Z" * size, maintype="application",
                       subtype="octet-stream", filename="network_bench.bin")
    return msg.as_string()

def send_one(host, smtp_port, sender, recipient, raw_mail, timeout):
    t0 = time.perf_counter()
    try:
        s = smtplib.SMTP(host, smtp_port, timeout=timeout)
        s.ehlo()
        s.sendmail(sender, [recipient], raw_mail)
        s.quit()
        return True, (time.perf_counter() - t0) * 1000.0, ""
    except Exception as e:
        return False, (time.perf_counter() - t0) * 1000.0, str(e)

def fetch_one(host, pop3_port, user, password, msg_num, timeout):
    t0 = time.perf_counter()
    try:
        p = poplib.POP3(host, pop3_port, timeout=timeout)
        p.user(user)
        p.pass_(password)
        lines = p.retr(msg_num)[1]
        p.quit()
        return True, (time.perf_counter() - t0) * 1000.0, len(lines), ""
    except Exception as e:
        try:
            p.quit()
        except Exception:
            pass
        return False, (time.perf_counter() - t0) * 1000.0, 0, str(e)

def main():
    ap = argparse.ArgumentParser(description="MailForge real network benchmark")
    ap.add_argument("--host", default="140.143.233.15")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--smtp-port", type=int, default=2525)
    ap.add_argument("--pop3-port", type=int, default=1110)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--size-mb", type=float, default=1.0)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--accounts", type=int, default=4)
    ap.add_argument("--verify", type=int, default=0, help="POP3 下载验证封数，0 表示只统计收件数")
    ap.add_argument("--user", default="alice")
    ap.add_argument("--password", default="123456")
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    print("=" * 68)
    print("MailForge real network benchmark")
    print("target: %s" % args.host)
    print("ports : HTTP %d  SMTP %d  POP3 %d" % (args.http_port, args.smtp_port, args.pop3_port))
    print("=" * 68)

    checks = {}
    for name, port in (("http", args.http_port), ("smtp", args.smtp_port), ("pop3", args.pop3_port)):
        ok, err = port_open(args.host, port, timeout=5.0)
        checks[name] = ok
        print("[check] %-4s :%d  %s" % (name, port, "OPEN" if ok else "BLOCKED/TIMEOUT " + err))

    if not checks["smtp"] or not checks["pop3"]:
        print("\n无法进行真实 SMTP/POP3 压测：2525/1110 未放行。")
        print("请到云服务器安全组和系统防火墙放行 TCP 2525、TCP 1110，然后重试。")
        return 2

    # 尝试通过 HTTP 注册一个临时测试账号，避免污染已有邮箱
    test_user = args.user
    test_pass = args.password
    created_test_account = False
    if checks["http"]:
        test_user = "bench_" + str(int(time.time()))[-8:]
        test_pass = "Bench123456"
        try:
            r = http_post(args.host, args.http_port, "/api/register",
                          {"user": test_user, "pass": test_pass})
            if r.get("ok"):
                created_test_account = True
                print("[http] 已注册测试账号: %s" % test_user)
            else:
                print("[http] 注册测试账号失败，改用 %s: %s" % (args.user, r.get("msg")))
                test_user, test_pass = args.user, args.password
        except Exception as e:
            print("[http] 注册测试账号异常，改用 %s: %s" % (args.user, e))
            test_user, test_pass = args.user, args.password
    else:
        print("[http] 8080 不可达，直接使用已有账号 %s" % test_user)

    recipient = test_user + "@example.com"
    senders = ["account%d@example.com" % (i + 1) for i in range(max(1, args.accounts))]
    raw_mail = make_raw_mail(senders[0], recipient, "MailForge network benchmark", args.size_mb)

    # 记录 POP3 基准
    before_count = 0
    before_size = 0
    try:
        p0 = poplib.POP3(args.host, args.pop3_port, timeout=args.timeout)
        p0.user(test_user)
        p0.pass_(test_pass)
        before_count, before_size = p0.stat()
        p0.quit()
        print("[pop3] 测试前邮件数=%d size=%d" % (before_count, before_size))
    except Exception as e:
        print("[pop3] 无法登录测试账号 %s: %s" % (test_user, e))
        return 3

    print("\n[SMTP] 发送 %d 封，约 %.2f MB/封，线程=%d，账号=%d ..." %
          (args.count, args.size_mb, args.threads, args.accounts))
    send_ok = 0
    send_fail = 0
    send_lat_sum = 0.0
    send_wall_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=max(1, args.threads)) as ex:
        futs = []
        for i in range(args.count):
            sender = senders[i % len(senders)]
            futs.append(ex.submit(send_one, args.host, args.smtp_port, sender,
                                  recipient, raw_mail, args.timeout))
        for f in as_completed(futs):
            ok, ms, err = f.result()
            send_lat_sum += ms
            if ok:
                send_ok += 1
            else:
                send_fail += 1
                if send_fail <= 10:
                    print("  [send fail] %s" % err)
    send_wall_ms = (time.perf_counter() - send_wall_start) * 1000.0
    print("[SMTP] 成功=%d 失败=%d wall=%.1f ms 单封平均=%.1f ms 吞吐=%.2f 封/s" %
          (send_ok, send_fail, send_wall_ms,
           send_lat_sum / max(1, send_ok), send_ok * 1000.0 / max(1.0, send_wall_ms)))

    # POP3 统计
    time.sleep(1.0)
    after_count = before_count
    after_size = before_size
    try:
        p1 = poplib.POP3(args.host, args.pop3_port, timeout=args.timeout)
        p1.user(test_user)
        p1.pass_(test_pass)
        after_count, after_size = p1.stat()
        p1.quit()
    except Exception as e:
        print("[pop3] 统计失败: %s" % e)
    received = max(0, after_count - before_count)
    print("[POP3] 收件数=%d 新增=%d" % (after_count, received))

    verify_ok = 0
    verify_ms_sum = 0.0
    if args.verify > 0:
        n_verify = min(args.verify, received)
        print("\n[POP3] 并发下载验证 %d 封 ..." % n_verify)
        start_num = before_count + 1
        nums = list(range(start_num, start_num + n_verify))
        verify_wall_start = time.perf_counter()
        with ThreadPoolExecutor(max_workers=max(1, args.threads)) as ex:
            futs = [ex.submit(fetch_one, args.host, args.pop3_port, test_user,
                              test_pass, n, args.timeout) for n in nums]
            for f in as_completed(futs):
                ok, ms, nlines, err = f.result()
                verify_ms_sum += ms
                if ok:
                    verify_ok += 1
                elif err:
                    print("  [fetch fail] %s" % err)
        verify_wall_ms = (time.perf_counter() - verify_wall_start) * 1000.0
        print("[POP3] 下载验证成功=%d/%d wall=%.1f ms 单封平均=%.1f ms" %
              (verify_ok, n_verify, verify_wall_ms, verify_ms_sum / max(1, verify_ok)))

    result = {
        "target": args.host,
        "smtpPort": args.smtp_port,
        "pop3Port": args.pop3_port,
        "user": test_user,
        "count": args.count,
        "sizeMB": args.size_mb,
        "threads": args.threads,
        "accounts": args.accounts,
        "smtpOk": send_ok,
        "smtpFail": send_fail,
        "sendWallMs": send_wall_ms,
        "sendAvgMs": send_lat_sum / max(1, send_ok),
        "sendThroughput": send_ok * 1000.0 / max(1.0, send_wall_ms),
        "beforeCount": before_count,
        "afterCount": after_count,
        "received": received,
        "lossRate": (args.count - received) * 100.0 / max(1, args.count),
        "verifyCount": args.verify,
        "verifyOk": verify_ok,
    }
    if created_test_account and after_count > before_count:
        try:
            pc = poplib.POP3(args.host, args.pop3_port, timeout=args.timeout)
            pc.user(test_user)
            pc.pass_(test_pass)
            for n in range(before_count + 1, after_count + 1):
                pc.dele(n)
            pc.quit()
            print("[cleanup] 已删除临时测试账号的 %d 封测试邮件" % (after_count - before_count))
        except Exception as e:
            print("[cleanup] 清理测试邮件失败: %s" % e)

    print("\n" + json.dumps(result, ensure_ascii=False, indent=2))
    with open("network_bench_result.json", "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    print("\n结果已写入 network_bench_result.json")
    return 0

if __name__ == "__main__":
    sys.exit(main())