#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构建期把 web_server.c 里的内嵌页面 PAGE_HTML 抽取出来并 gzip 压缩，生成 page_html_gz.h。

为什么要做：整页约 74KB，弱网（AP 信号差 / BLE 共存）下单页发送容易触发
httpd sock EAGAIN → send 超时 → uri handler execution failed，页面加载失败。
压缩后通常降到 ~22KB，显著降低传输时间与失败率。

说明：PAGE_HTML 现在是**纯静态**内容（品牌/导出前缀等已改为运行期由 /api/brand 下发），
因此这里只做“字符串字面量拼接 + 反转义 + gzip”，不再需要解析任何 C 宏。

用法：gen_page_gz.py <web_server.c> <输出 page_html_gz.h>
健壮性：任何解析/读写异常都会退化为“空压缩体”（PAGE_HTML_GZ_LEN=0），
web_server.c 检测到 0 时自动回退发送未压缩页面，绝不导致构建失败。
"""
import gzip
import io
import os
import sys


def c_unescape(s: str) -> str:
    """把 C 字符串字面量内容（不含两端引号）还原为原始文本/字节。"""
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '\\' and i + 1 < n:
            m = s[i + 1]
            if m == 'n':
                out.append('\n'); i += 2; continue
            if m == 't':
                out.append('\t'); i += 2; continue
            if m == 'r':
                out.append('\r'); i += 2; continue
            if m == '"':
                out.append('"'); i += 2; continue
            if m == '\\':
                out.append('\\'); i += 2; continue
            if m == "'":
                out.append("'"); i += 2; continue
            if m in '01234567':
                j = i + 1
                d = ''
                while j < n and len(d) < 3 and s[j] in '01234567':
                    d += s[j]; j += 1
                out.append(chr(int(d, 8) & 0xFF)); i = j; continue
            if m == 'x':
                j = i + 2
                d = ''
                while j < n and s[j] in '0123456789abcdefABCDEF':
                    d += s[j]; j += 1
                if d:
                    out.append(chr(int(d, 16) & 0xFF)); i = j; continue
            out.append(m); i += 2; continue
        out.append(c); i += 1
    return ''.join(out)


def tokenize_fragment(line: str):
    """把一行 C 拼接片段解析为字符串字面量列表（'id' 表示引号外的标识符）。"""
    tokens = []
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == '"':
            i += 1
            raw = []
            while i < n:
                ch = line[i]
                if ch == '\\' and i + 1 < n:
                    raw.append(ch); raw.append(line[i + 1]); i += 2; continue
                if ch == '"':
                    i += 1
                    break
                raw.append(ch); i += 1
            tokens.append(('str', ''.join(raw)))
        elif c.isalpha() or c == '_':
            j = i
            while j < n and (line[j].isalnum() or line[j] == '_'):
                j += 1
            tokens.append(('id', line[i:j]))
            i = j
        elif c == '/' and i + 1 < n and line[i + 1] in '/*':
            break   # 行尾 C 注释（// 或 /*），忽略其后内容
        else:
            i += 1
    return tokens


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write('用法: gen_page_gz.py <web_server.c> <out.h>\n')
        return 1
    csrc, out_path = sys.argv[1], sys.argv[2]

    gz_bytes = b''
    plain_len = 0
    try:
        with open(csrc, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
        start = None
        for idx, ln in enumerate(lines):
            if 'PAGE_HTML[]' in ln and '=' in ln:
                start = idx
                break
        if start is None:
            raise RuntimeError('未找到 PAGE_HTML 定义')

        chunks = []
        for idx in range(start + 1, len(lines)):
            s = lines[idx].strip()
            if not s.startswith('"'):
                break
            if s.endswith(';'):
                s = s[:-1]
            for kind, val in tokenize_fragment(s):
                if kind == 'str':
                    chunks.append(c_unescape(val))
                else:
                    # 页面应为纯静态：若出现引号外的标识符（宏），说明其未被替换，给出告警。
                    sys.stderr.write('警告：PAGE_HTML 中出现未替换的标识符 "%s"\n' % val)
        content = ''.join(chunks)
        plain_len = len(content.encode('utf-8'))

        buf = io.BytesIO()
        with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as gz:
            gz.write(content.encode('utf-8'))
        gz_bytes = buf.getvalue()
    except Exception as e:
        sys.stderr.write('页面 gzip 生成失败（将回退为未压缩发送）：%s\n' % e)
        gz_bytes = b''

    out = []
    out.append('/* 本文件由 gen_page_gz.py 自动生成，请勿手工修改。')
    out.append(' * 内嵌页面 PAGE_HTML 的 gzip 压缩体（明文 %d 字节 → gzip %d 字节）。' % (plain_len, len(gz_bytes)))
    out.append(' * PAGE_HTML_GZ_LEN==0 表示生成失败，服务端将回退发送未压缩页面。')
    out.append(' */')
    out.append('#pragma once')
    out.append('')
    out.append('#define PAGE_HTML_GZ_LEN %d' % len(gz_bytes))
    out.append('')
    if gz_bytes:
        out.append('static const unsigned char PAGE_HTML_GZ[] = {')
        line = '    '
        for i, b in enumerate(gz_bytes):
            line += '0x%02x,' % b
            if (i + 1) % 20 == 0:
                out.append(line)
                line = '    '
        if line.strip():
            out.append(line)
        out.append('};')
    else:
        out.append('static const unsigned char PAGE_HTML_GZ[] = { 0x00 };')
    out.append('')

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))

    sys.stderr.write('已生成 %s：明文 %d 字节 → gzip %d 字节\n' % (out_path, plain_len, len(gz_bytes)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
