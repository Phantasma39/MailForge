#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MailForge 端到端回归（课程验收一键复现）

自动完成：
  1) 协议层  ：Python smtplib / poplib 与 SMTP(2525)/POP3(1110) 真实交互
               （中文+点开头行、错误密码、列表/收取/删除语义、多用户隔离）
  2) HTTP 层 ：注册 → 三档发送（明文 / 数字信封 encrypt=1 / AES-256 对称 encrypt=2）
               + 附件档；收件箱主题解密、正文还原、附件字节一致性、删除
  3) 加密安全：断言 mailbox/ 下加密邮件无任何明文主题/正文/附件字节
  4) 协议终端：/api/raw/{smtp,pop3} 一问一答长连接（demo_smtp/demo_pop3 后端）

用法：make server && python3 tests/e2e_regression.py   （或 make test-e2e）
脚本会自动启动服务器（若 8080 未监听）、结束后自动清理 mailbox/keys/users.txt。
"""
import base64
import glob
import json
import os
import smtplib
import poplib
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRV_DIR = os.path.join(REPO, "MailServer")
BASE = "http://127.0.0.1:8080"
SMTP_PORT, POP3_PORT, HTTP_PORT = 2525, 1110, 8080

CHECKS = []  # (name, ok, detail)


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name,
                             ("- " + detail) if detail else ""))


def port_open(port):
    s = socket.socket()
    s.settimeout(0.4)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def start_server():
    if port_open(HTTP_PORT):
        return None
    proc = subprocess.Popen(["./mail_server"], cwd=SRV_DIR,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):                      # 最多等 5 秒
        if port_open(HTTP_PORT):
            return proc
        time.sleep(0.1)
    raise RuntimeError("服务器启动超时，请先 make server")


def post(path, data):
    req = urllib.request.Request(BASE + path,
                                 data=urllib.parse.urlencode(data).encode())
    return json.loads(urllib.request.urlopen(req, timeout=60).read().decode())


def get(path):
    return urllib.request.urlopen(BASE + path, timeout=60).read()


def login(user, pwd):
    j = post("/api/login", {"user": user, "pass": pwd})
    if not j.get("ok"):
        raise RuntimeError("登录失败 " + user + ": " + str(j))
    return j["token"]


def cleanup(proc, users_txt_orig):
    if proc:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
    for d in ("mailbox", "keys"):
        p = os.path.join(SRV_DIR, d)
        if os.path.isdir(p):
            subprocess.run(["rm", "-rf", p])
    with open(os.path.join(SRV_DIR, "users.txt"), "w", encoding="utf-8") as f:
        f.write(users_txt_orig)


def main():
    users_txt_orig = open(os.path.join(SRV_DIR, "users.txt"),
                          encoding="utf-8").read()
    proc = None
    try:
        proc = start_server()
        run_suite()
        print("\n================ 回归汇总 ================")
        passed = sum(1 for _, ok, _ in CHECKS if ok)
        print("通过 %d/%d 项" % (passed, len(CHECKS)))
        sys.exit(0 if passed == len(CHECKS) else 1)
    finally:
        cleanup(proc, users_txt_orig)


def run_suite():
    print("== 1) 协议层（真实 SMTP/POP3 会话） ==")
    # 1.1 错误密码被拒
    try:
        p = poplib.POP3("127.0.0.1", POP3_PORT, timeout=10)
        p.user("bob"); p.pass_("wrong-pass")
        check("POP3 错误密码被拒", False, "竟然通过了")
        p.quit()
    except poplib.error_proto as e:
        check("POP3 错误密码被拒", True, str(e)[:60])

    # 1.2 alice 用 smtplib 发两封：ASCII(正文含 . 开头行→SMTP 点填充) + MIME 中文
    import email
    from email.header import decode_header, make_header
    from email.mime.text import MIMEText

    s = smtplib.SMTP("127.0.0.1", SMTP_PORT, timeout=10)
    s.ehlo()
    s.sendmail("alice@example.com", ["bob@example.com"],
               "From: alice@example.com\r\nTo: bob@example.com\r\n"
               "Subject: dotstuffing test\r\n\r\n"
               "line one\n.dot-prefixed line checks SMTP dot-stuffing\n")
    m = MIMEText("你好 bob，协议层回归测试。", "plain", "utf-8")
    m["From"] = "alice@example.com"
    m["To"] = "bob@example.com"
    m["Subject"] = "协议层回归测试"
    s.sendmail("alice@example.com", ["bob@example.com"], m.as_string())
    s.quit()
    check("SMTP smtplib 发送(点填充+中文MIME)", True)

    # 1.3 bob 用 poplib 收信：STAT / LIST / RETR 内容校验
    p = poplib.POP3("127.0.0.1", POP3_PORT, timeout=10)
    p.user("bob"); p.pass_("123456")
    n, _ = p.stat()
    check("POP3 登录+STAT", n >= 2)
    msgs = p.list()[1]
    dots_ok = zh_ok = False
    for entry in msgs:
        num = int(entry.split()[0])
        raw = b"\n".join(p.retr(num)[1]).decode("utf-8", "replace")
        if "dotstuffing test" in raw:
            dots_ok = ".dot-prefixed line checks SMTP dot-stuffing" in raw
        parsed = email.message_from_string(raw)
        subj = str(make_header(decode_header(parsed.get("Subject", ""))))
        if "协议层回归测试" in subj:
            for part in parsed.walk():
                if part.get_content_type() == "text/plain":
                    pl = part.get_payload(decode=True)
                    zh_ok = bool(pl) and ("你好 bob".encode() in pl)
    check("POP3 RETR 点填充还原", dots_ok)
    check("POP3 RETR 中文正文解码", zh_ok)
    p.quit()

    # 1.4 删除语义：DELE 标记 + QUIT 才真正删除
    p3 = poplib.POP3("127.0.0.1", POP3_PORT, timeout=10)
    p3.user("bob"); p3.pass_("123456")
    n0, _ = p3.stat()
    newest = int(p3.list()[1][-1].split()[0])
    p3.dele(newest)
    p3.quit()
    p4 = poplib.POP3("127.0.0.1", POP3_PORT, timeout=10)
    p4.user("bob"); p4.pass_("123456")
    n1, _ = p4.stat()
    p4.quit()
    check("POP3 DELE+QUIT 真正删除", n1 == n0 - 1, "删前 %d 删后 %d" % (n0, n1))

    print("\n== 2) HTTP 层（注册 + 三档加密 + 附件） ==")
    check("注册新用户 A", post("/api/register",
                              {"user": "reg_a", "pass": "passa1"})["ok"])
    check("注册新用户 B", post("/api/register",
                              {"user": "reg_b", "pass": "passb1"})["ok"])
    ta = login("reg_a", "passa1")
    tb = login("reg_b", "passb1")

    for mode, subj, body, enc in [("0", "明文邮件", "明文正文", False),
                                  ("1", "信封机密", "数字信封正文保密内容", True),
                                  ("2", "AES机密", "AES对称通道正文保密内容", True)]:
        r = post("/api/send", {"token": ta, "to": "reg_b@example.com",
                               "subject": subj, "body": body, "encrypt": mode})
        check("发送 encrypt=%s(%s)" % (mode, subj), r.get("ok"),
              str(r.get("msg")))

    # 附件（信封内 multipart 附件随正文一起加密）
    raw_file = b"REGRESSION-ATTACH-" + b"A" * 500
    r = post("/api/send", {"token": ta, "to": "reg_b@example.com",
                           "subject": "带附件加密", "body": "附件也在信封内",
                           "encrypt": "1", "filename": "note.txt",
                           "fileB64": base64.b64encode(raw_file).decode()})
    check("发送 加密+附件", r.get("ok"))

    # reg_b 收件箱：主题应全部还原
    inbox = json.loads(get("/api/inbox?token=" + tb).decode())
    subs = {m["subject"] for m in inbox["mails"]}
    check("收件箱主题解密(明/信封/AES/附件)",
          {"明文邮件", "信封机密", "AES机密", "带附件加密"} <= subs)
    check("加密邮件被标记", sum(1 for m in inbox["mails"] if m["encrypted"]) == 3)

    # 逐封阅读：正文还原
    got = {}
    for m in inbox["mails"]:
        mail = json.loads(get("/api/mail?token=%s&n=%d" % (tb, m["number"])).decode())
        got[m["subject"]] = mail["raw"]
    check("信封正文解密", "数字信封正文保密内容" in got.get("信封机密", ""))
    check("AES 正文解密", "AES对称通道正文保密内容" in got.get("AES机密", ""))

    # 附件下载字节一致性
    att_mail = next(m for m in inbox["mails"] if m["subject"] == "带附件加密")
    att = get("/api/attachment?token=%s&n=%d&i=0" % (tb, att_mail["number"]))
    check("加密邮件附件下载字节一致", att == raw_file)

    # 加密安全：mailbox/reg_b 下 .eml 不含明文
    leak = False
    plain_en = "数字信封正文保密内容".encode()
    plain_aes = "AES对称通道正文保密内容".encode()
    for f in glob.glob(os.path.join(SRV_DIR, "mailbox", "reg_b", "*.eml")):
        c = open(f, "rb").read()
        if (c.find(plain_en) >= 0 or c.find(plain_aes) >= 0
                or c.find(b"REGRESSION-ATTACH-") >= 0):
            leak = True
    check("落盘无明文(信封/AES/附件)", not leak)

    # 删除
    first = inbox["mails"][0]["number"]
    check("HTTP 删除邮件", post("/api/delete",
                               {"token": tb, "n": first}).get("ok"))
    inbox2 = json.loads(get("/api/inbox?token=" + tb).decode())
    check("删除后数量-1", len(inbox2["mails"]) == len(inbox["mails"]) - 1)

    print("\n== 3) 协议终端（/api/raw 一问一答，demo 页后端） ==")
    j = post("/api/raw/smtp/open", {"token": ta})
    check("raw-smtp open 得到 220 问候", j.get("ok") and j.get("lines")
          and "220" in j["lines"][0]["text"])
    conn = j["conn"]
    r = post("/api/raw/smtp/send", {"token": ta, "conn": conn,
                                    "line": "EHLO reg"})
    check("raw-smtp EHLO 应答 250",
          any("250" in ln["text"] for ln in r.get("lines", [])))
    post("/api/raw/smtp/send", {"token": ta, "conn": conn, "line": "QUIT"})
    post("/api/raw/smtp/close", {"token": ta, "conn": conn})

    j = post("/api/raw/pop3/open", {"token": tb})
    check("raw-pop3 open 得到 +OK 问候", j.get("ok") and j.get("lines")
          and "+OK" in j["lines"][0]["text"])
    conn = j["conn"]
    r = post("/api/raw/pop3/send", {"token": tb, "conn": conn,
                                    "line": "USER reg_b"})
    r2 = post("/api/raw/pop3/send", {"token": tb, "conn": conn,
                                     "line": "PASS passb1"})
    ok_user = any("+OK" in ln["text"] for ln in r.get("lines", []))
    ok_pass = any("+OK" in ln["text"] for ln in r2.get("lines", []))
    check("raw-pop3 登录交互", ok_user and ok_pass)
    post("/api/raw/pop3/close", {"token": tb, "conn": conn})

    # 多用户隔离
    ia = json.loads(get("/api/inbox?token=" + ta).decode())
    check("多用户隔离：发送方看不到发给对方加密信的主题",
          all("信封机密" != m["subject"] for m in ia["mails"]))


if __name__ == "__main__":
    main()
