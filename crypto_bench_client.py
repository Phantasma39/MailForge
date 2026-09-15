#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
服务器内部对称加解密压测客户端
只调用 /api/cryptobench，不走 RSA、SMTP、POP3、磁盘。
用法：
  python crypto_bench_client.py --host 140.143.233.15 --user alice --password 123456 --algo all --count 100 --size-kb 1024 --threads 4
"""
import argparse
import json
import urllib.parse
import urllib.request

def http_post(host, port, path, data, timeout=30):
    body = urllib.parse.urlencode(data).encode("utf-8")
    req = urllib.request.Request("http://%s:%d%s" % (host, port, path), data=body)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))

def http_get(host, port, path, timeout=300):
    with urllib.request.urlopen("http://%s:%d%s" % (host, port, path), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))

def main():
    ap = argparse.ArgumentParser(description="MailForge internal crypto benchmark client")
    ap.add_argument("--host", default="140.143.233.15")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--user", default="alice")
    ap.add_argument("--password", default="123456")
    ap.add_argument("--algo", default="all", choices=["aes", "chacha", "chacha20", "all"])
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--size-kb", type=int, default=1024)
    ap.add_argument("--threads", type=int, default=4)
    args = ap.parse_args()

    print("=" * 72)
    print("MailForge internal crypto benchmark")
    print("target : %s:%d" % (args.host, args.http_port))
    print("algo   : %s  count=%d  size=%d KB  threads=%d" %
          (args.algo, args.count, args.size_kb, args.threads))
    print("=" * 72)

    login = http_post(args.host, args.http_port, "/api/login",
                      {"user": args.user, "pass": args.password})
    if not login.get("ok"):
        print("登录失败：", login.get("msg"))
        return 1
    token = login["token"]
    print("登录成功：", args.user)

    url = ("/api/cryptobench?token=" + urllib.parse.quote(token) +
           "&algo=" + urllib.parse.quote(args.algo) +
           "&count=" + str(args.count) +
           "&sizeKB=" + str(args.size_kb) +
           "&threads=" + str(args.threads))
    result = http_get(args.host, args.http_port, url, timeout=600)
    if not result.get("ok"):
        print("压测失败：", result.get("msg"))
        return 1

    print("\n%-10s %6s %6s %12s %12s %12s %12s %12s" %
          ("模式", "成功", "失败", "加密(ms)", "解密(ms)", "平均加密", "平均解密", "吞吐"))
    for m in result.get("modes", []):
        print("%-10s %6d %6d %12d %12d %12.1f %12.1f %12.1f" %
              (m["mode"], m["ok"], m["fail"], m["encryptMs"], m["decryptMs"],
               m["avgEncryptMs"], m["avgDecryptMs"], m["throughput"]))
    print("\n总用时：%.1f s" % (result.get("totalMs", 0) / 1000.0))
    print("说明：本测试只调用服务器 /api/cryptobench，不走 RSA、SMTP、POP3、磁盘。")
    print("\n原始 JSON：")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())