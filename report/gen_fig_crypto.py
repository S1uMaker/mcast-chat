#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""解析 _crypto.pcapng 中的 MCHT 报文，渲染"明文 vs 密文"对比图 fig_crypto.png。

数据来源：crypto_demo.ps1 抓取的真实报文（tshark，VirtualBox Host-Only 出接口）。
"""
import os
import subprocess
from PIL import Image, ImageDraw, ImageFont

TS = r"C:\Program Files\Wireshark\tshark.exe"
HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "_crypto.pcapng")
OUT = os.path.join(HERE, "figures", "fig_crypto.png")

MONO = r"C:\Windows\Fonts\CascadiaMono.ttf"
CJK = r"C:\Windows\Fonts\msyh.ttc"
FS = 14
LINE_H = int(FS * 1.45)
PAD = 16
TITLE_H = 30
BG = (12, 12, 12)
TITLE_BG = (32, 32, 32)
FG = (200, 200, 200)
GREEN = (78, 201, 116)
CYAN = (86, 182, 240)
YELLOW = (229, 208, 112)
RED = (240, 110, 110)
GRAY = (140, 140, 140)
WHITE = (235, 235, 235)
ORANGE = (255, 160, 70)

f_mono = ImageFont.truetype(MONO, FS)
f_cjk = ImageFont.truetype(CJK, FS)
CHAR_W = f_mono.getbbox("0")[2] - f_mono.getbbox("0")[0]


def parse_packets():
    out = subprocess.run(
        [TS, "-r", CAP, "-Y", "udp.port==54321", "-T", "fields", "-e", "udp.payload"],
        capture_output=True, text=True).stdout
    pkts = []
    for line in out.splitlines():
        h = line.strip().replace(":", "")
        if len(h) < 64:
            continue
        b = bytes.fromhex(h)
        if b[:4] != b"MCHT" or b[4] != 1:
            continue
        pkts.append({
            "type": b[5],
            "flags": int.from_bytes(b[6:8], "big"),
            "seq": int.from_bytes(b[24:28], "big"),
            "plen": int.from_bytes(b[28:32], "big"),
            "raw": b,
        })
    return pkts


def hexdump_lines(raw, payload_limit=None):
    """返回 [(col, text)] 的行列表；col 是按字段着色的 hexdump 行。"""
    n = len(raw)
    limit = n if payload_limit is None else min(n, 32 + payload_limit)
    lines = []
    for off in range(0, limit, 16):
        seg = raw[off:off + 16]
        hx, asc = [], ""
        for i, byt in enumerate(seg):
            pos = off + i
            if pos < 4:
                c = GREEN                      # magic
            elif pos == 5:
                c = CYAN                       # type
            elif 6 <= pos < 8:
                c = YELLOW                     # flags
            elif pos < 32:
                c = GRAY                       # sender/seq/plen
            else:
                c = None                       # 载荷：调用者决定
            hx.append((f"{byt:02x}", c))
            asc += chr(byt) if 32 <= byt < 127 else "."
        lines.append((off, hx, asc))
    if limit < n:
        lines.append((None, None, f"  ... 载荷其余 {n - limit} 字节省略（共 {n} 字节）"))
    return lines


def draw_panel(draw, y, title, lines, payload_color, notes):
    draw.rectangle([0, y, W, y + TITLE_H], fill=TITLE_BG)
    draw.text((PAD, y + 6), title, font=f_cjk, fill=WHITE)
    y += TITLE_H
    for item in lines:
        x = PAD
        if item[0] is None:
            draw.text((x + 4, y), item[2], font=f_mono, fill=GRAY)
            y += LINE_H
            continue
        off, cells, asc = item
        draw.text((x, y), f"{off:04x} ", font=f_mono, fill=GRAY)
        x += 5 * CHAR_W
        for hexs, c in cells:
            color = c if c else payload_color
            draw.text((x, y), hexs, font=f_mono, fill=color)
            x += 3 * CHAR_W
        x += CHAR_W
        draw.text((x, y), "|" + asc, font=f_mono, fill=GRAY)
        y += LINE_H
    y += 4
    for text, color in notes:
        draw.text((PAD + 4, y), text, font=f_cjk, fill=color)
        y += LINE_H
    return y + 14


pkts = parse_packets()
MARKER = "hello-plaintext-12345".encode()
SECRET = "secret-cipher-6789".encode()

plain = next(p for p in pkts if p["type"] == 0x10 and not (p["flags"] & 1) and MARKER in p["raw"])
enc = [p for p in pkts if p["type"] == 0x10 and (p["flags"] & 1)]
assert enc, "没有抓到加密 TEXT 报文"
enc_pkt = enc[-1]
# 安全校验：整个抓包不得出现任何明文密钥消息；密文包中不得出现两个标记
assert all(SECRET not in p["raw"] for p in pkts), "密钥消息明文泄漏！"
assert all(MARKER not in p["raw"] for p in enc), "加密报文中出现明文标记！"
kd = [p for p in pkts if p["type"] == 0x06]

panel1_title = "tshark 抓包 A：未开启加密的聊天报文（type=0x10 TEXT，flags=0x%04x）" % plain["flags"]
panel2_title = "tshark 抓包 B：开启加密后的聊天报文（flags=0x%04x = ENCRYPTED|SIGNED）" % enc_pkt["flags"]

# 明文面板只画 头+正文（其后 256B 是 RSA-PSS 签名，随机字节，省略）
lines1 = hexdump_lines(plain["raw"], payload_limit=plain["plen"] - 256)
lines2 = hexdump_lines(enc_pkt["raw"], payload_limit=48)

notes1 = [
    ("正文 %d B：utf8(昵称) + utf8(文本) —— 报文内容在网络上完全可读（ASCII 列可见 LiSi / hello-plaintext）"
     % (plain["plen"] - 256), ORANGE),
    ("载荷共 %d B = 正文 %d B + RSA-PSS 签名 256B（TEXT 恒签名，故 flags=0x%04x；签名为随机字节，已省略）"
     % (plain["plen"], plain["plen"] - 256, plain["flags"]), GRAY),
]
notes2 = [
    ("载荷 %d B = keyId(4B) + AES-256-GCM(密文+tag 16B) + RSA-PSS 签名(256B)" % enc_pkt["plen"], ORANGE),
    ("hex 区第 7-8 字节（黄色）= flags：bit0=ENCRYPTED bit1=SIGNED；密文部分呈随机分布", GRAY),
    ("对整个抓包检索 hello-plaintext / secret-cipher 的 UTF-8 字节：密文报文中 0 次命中", RED),
]

# 版面：先画到超高画布，再按内容裁剪
W = 1000
H = 2000
img = Image.new("RGB", (W, H), BG)
draw = ImageDraw.Draw(img)
y = PAD
y = draw_panel(draw, y, panel1_title, lines1, WHITE, notes1)
y = draw_panel(draw, y, panel2_title, lines2, (150, 150, 210), notes2)
img = img.crop((0, 0, W, y + PAD))
img.save(OUT)
print("saved:", OUT)
print("plaintext: seq=%d plen=%d flags=0x%04x" % (plain["seq"], plain["plen"], plain["flags"]))
print("encrypted: seq=%d plen=%d flags=0x%04x" % (enc_pkt["seq"], enc_pkt["plen"], enc_pkt["flags"]))
print("KEY_DIST packets captured:", len(kd))
print("TEXT packets: plaintext=%d encrypted=%d total-MCHT=%d" % (len(plain and [0]), len(enc), len(pkts)))
