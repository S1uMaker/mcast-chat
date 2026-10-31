#!/usr/bin/env bash
# 编译大作业报告（需要 TeX Live：xelatex + ctex + tikz）
cd "$(dirname "$0")"
set -e
if command -v latexmk >/dev/null; then
    latexmk -xelatex -interaction=nonstopmode -halt-on-error report.tex
else
    xelatex -interaction=nonstopmode report.tex
    xelatex -interaction=nonstopmode report.tex
fi
echo "PDF 生成: $(pwd)/report.pdf"
