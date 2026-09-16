#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从工程根目录的 Note.txt 生成 default_text.h（内置默认文本）。

- 把 CRLF / CR 统一规范为 LF（这样“写文本”动作里 \n 只对应一次回车）。
- 逐字节转义为 C 字符串字面量：可打印 ASCII 原样输出，
  '\\' '"' 转义，'\n' '\t' 用简写，其余（含 UTF-8 多字节、控制字符）用三位八进制 \\ooo。
  八进制转义固定三位，不会与后续字符产生歧义（区别于 \\x 会吞掉后续十六进制字符）。
- 相邻字符串字面量由 C 自动拼接，故按固定长度折行，避免单行过长。

用法：gen_default_text.py <Note.txt 路径> <输出 default_text.h 路径>
"""
import os
import sys


def escape_bytes(data: bytes) -> list:
    """把字节串转换为若干行 C 字符串字面量片段（不含外层引号）。"""
    pieces = []
    for b in data:
        if b == 0x5C:            # '\'
            pieces.append('\\\\')
        elif b == 0x22:          # '"'
            pieces.append('\\"')
        elif b == 0x0A:          # '\n'
            pieces.append('\\n')
        elif b == 0x09:          # '\t'
            pieces.append('\\t')
        elif 0x20 <= b < 0x7F:
            pieces.append(chr(b))
        else:
            pieces.append('\\%03o' % b)
    return pieces


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("用法: gen_default_text.py <input.txt> <output.h>\n")
        return 1
    src, dst = sys.argv[1], sys.argv[2]

    with open(src, 'rb') as f:
        raw = f.read()
    data = raw.replace(b'\r\n', b'\n').replace(b'\r', b'\n')

    pieces = escape_bytes(data)

    lines = []
    cur = []
    cur_len = 0
    max_len = 100   # 单行片段最大源字符数
    for p in pieces:
        if cur_len + len(p) > max_len and cur:
            lines.append(''.join(cur))
            cur = []
            cur_len = 0
        cur.append(p)
        cur_len += len(p)
    if cur:
        lines.append(''.join(cur))

    out = []
    out.append('/* 本文件由 gen_default_text.py 自动生成，请勿手工修改。')
    out.append(' * 来源：Note.txt（换行已规范为 \\n）。')
    out.append(' */')
    out.append('#pragma once')
    out.append('')
    out.append('#define DEFAULT_TEXT_LEN %d' % len(data))
    out.append('')
    out.append('static const char DEFAULT_TEXT[] =')
    for ln in lines:
        out.append('    "%s"' % ln)
    out.append('    ;')
    out.append('')

    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))

    sys.stderr.write("已生成 %s：%d 字节\n" % (dst, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
