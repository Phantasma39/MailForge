# -*- coding: utf-8 -*-
"""mail_crypto_gui.py —— 邮件加密可视化演示工具

后端：调用 C++ 加密模块 crypto_cli（真实 OpenSSL 3.0 实现，非 Python 重写）
功能：生成密钥 / 加密邮件(明文→数字信封) / 解密还原 / 篡改检测 / 性能测试

运行前提：先编译 C++ 后端
  MSYS2 终端:  cd /c/Users/DELL/Desktop/计算机网络作业
               make cli
然后运行本脚本:
  Windows:  python tools/mail_crypto_gui.py
"""

import os
import subprocess
import sys
import tkinter as tk
from tkinter import scrolledtext, ttk

# ---------------------------------------------------------------------------
# 路径定位
# ---------------------------------------------------------------------------
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CLI_NAME = 'crypto_cli.exe' if sys.platform == 'win32' else 'crypto_cli'
CLI_PATH = os.path.join(TOOLS_DIR, CLI_NAME)
DEFAULT_KEYS = os.path.join(BASE_DIR, 'keys')

FONT_CN = ('Microsoft YaHei', 10)
FONT_MONO = ('Consolas', 9)


def run_cli(args, input_text=None):
    """调用 C++ 加密模块，返回 (退出码, stdout文本)。通信一律 UTF-8 字节。"""
    if not os.path.exists(CLI_PATH):
        return -1, '找不到加密模块: %s\n请先在 MSYS2 终端执行:  make cli' % CLI_PATH
    try:
        data = input_text.encode('utf-8') if input_text is not None else None
        p = subprocess.run([CLI_PATH] + args, input=data,
                           capture_output=True, timeout=120)
        return p.returncode, p.stdout.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        return -1, '加密模块执行超时'
    except Exception as exc:
        return -1, '调用加密模块失败: %s' % exc


class CryptoGui:
    def __init__(self, root):
        self.root = root
        root.title('邮件加密可视化演示 —— AES-256-CBC + RSA-2048 + SHA-256 数字信封')
        root.geometry('1000x720')
        root.minsize(800, 560)

        self.from_var = tk.StringVar(value='alice@test.com')
        self.to_var = tk.StringVar(value='bob@test.com')
        self.keys_var = tk.StringVar(value=DEFAULT_KEYS)
        self.status_var = tk.StringVar()

        self._build_ui()
        self.set_status('就绪。请先点击 [① 生成密钥]')

    # ------------------------------------------------------------------
    def _build_ui(self):
        title = ttk.Label(self.root,
                          text='邮件加密可视化演示  (数字信封: 明文 → 密文 → 还原)',
                          font=('Microsoft YaHei', 13, 'bold'))
        title.pack(pady=(8, 2))

        # 参数行
        param = ttk.Frame(self.root)
        param.pack(fill='x', padx=10)
        ttk.Label(param, text='发件人').pack(side='left')
        ttk.Entry(param, textvariable=self.from_var, width=20).pack(
            side='left', padx=4)
        ttk.Label(param, text='收件人').pack(side='left')
        ttk.Entry(param, textvariable=self.to_var, width=20).pack(
            side='left', padx=4)
        ttk.Label(param, text='密钥目录').pack(side='left')
        ttk.Entry(param, textvariable=self.keys_var, width=30).pack(
            side='left', padx=4)

        # 按钮行
        btns = ttk.Frame(self.root)
        btns.pack(fill='x', padx=10, pady=6)
        for text, cmd in [
            ('① 生成密钥', self.on_genkeys),
            ('② 加密邮件', self.on_encrypt),
            ('③ 解密还原', self.on_decrypt),
            ('④ 篡改演示', self.on_tamper),
            ('⑤ 性能测试', self.on_benchmark),
        ]:
            ttk.Button(btns, text=text, command=cmd).pack(side='left', padx=4)

        # 中部左右分栏：明文 / 信封
        mid = ttk.Frame(self.root)
        mid.pack(fill='both', expand=True, padx=10, pady=4)
        left = ttk.LabelFrame(mid, text='① 明文 —— 邮件正文（可编辑，支持中文）')
        right = ttk.LabelFrame(mid, text='② 数字信封 —— 加密后的密文（自动生成）')
        left.pack(side='left', fill='both', expand=True, padx=(0, 4))
        right.pack(side='right', fill='both', expand=True, padx=(4, 0))

        self.plain_text = scrolledtext.ScrolledText(left, wrap='word',
                                                    font=FONT_CN)
        self.plain_text.pack(fill='both', expand=True, padx=4, pady=4)
        self.plain_text.insert('1.0',
                               '计算机网络课程设计——邮件加密\n'
                               '这是使用数字信封加密的邮件正文，支持中文。\n'
                               'AES-256-CBC 加密正文 + RSA-2048 加密会话密钥 + SHA-256 签名。')

        self.envelope_text = scrolledtext.ScrolledText(right, wrap='none',
                                                       font=FONT_MONO)
        self.envelope_text.pack(fill='both', expand=True, padx=4, pady=4)

        # 底部结果区
        bottom = ttk.LabelFrame(self.root, text='③ 解密结果 / 检测报告')
        bottom.pack(fill='both', expand=True, padx=10, pady=6)
        self.result_text = scrolledtext.ScrolledText(bottom, height=7,
                                                     font=FONT_CN)
        self.result_text.pack(fill='both', expand=True, padx=4, pady=4)

    # ------------------------------------------------------------------
    def set_status(self, msg, color='#0066cc'):
        self.status_var.set(msg)

    def _show_result(self, text):
        self.result_text.delete('1.0', 'end')
        self.result_text.insert('1.0', text)

    def _get_envelope(self):
        return self.envelope_text.get('1.0', 'end')

    # ------------------------- 按钮动作 -------------------------
    def on_genkeys(self):
        code, out = run_cli(['genkeys', self.keys_var.get()])
        self._show_result(out)
        self.set_status('✓ RSA-2048 密钥对已生成' if code == 0
                        else '✗ 密钥生成失败')

    def on_encrypt(self):
        plain = self.plain_text.get('1.0', 'end')
        code, out = run_cli(['seal', self.from_var.get(), self.to_var.get(),
                             self.keys_var.get()], plain)
        if code == 0:
            self.envelope_text.delete('1.0', 'end')
            self.envelope_text.insert('1.0', out)
            self.set_status('✓ 加密成功！正文已变为密文，信封可直接进入 SMTP 传输')
        else:
            self._show_result(out)
            self.set_status('✗ 加密失败（可能未生成密钥）')

    def on_decrypt(self):
        code, out = run_cli(['open', self.keys_var.get()],
                            self._get_envelope())
        self._show_result(out)
        self.set_status('✓ 解密成功，签名验证通过，原文已还原' if code == 0
                        else '✗ 解密失败（密钥不匹配或信封被篡改）')

    def on_tamper(self):
        code, out = run_cli(['tamper', self.keys_var.get()],
                            self._get_envelope())
        self._show_result('【篡改检测演示】模拟中间人翻转密文第 1 字节…\n\n' + out)
        self.set_status('✓ 篡改已被拦截（内容完整性检测生效）' if code == 0
                        else '⚠ 检测结果异常')

    def on_benchmark(self):
        code, out = run_cli(['benchmark'])
        self._show_result(out)
        self.set_status('✓ 性能基准测试完成')


def main():
    root = tk.Tk()
    CryptoGui(root)
    root.mainloop()


if __name__ == '__main__':
    main()
