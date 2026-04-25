# LinuxClass LaTeX 模板使用说明

这个目录中的 `main.tex` 只负责报告格式，不预置封面、摘要、结论或参考文献。正文内容建议继续放在 `chapter/` 目录中，然后在 `main.tex` 里用 `\input` 引入。

## 编译方式

模板使用 `ctexart` 和 `minted`，推荐使用 XeLaTeX 编译：

```bash
xelatex -shell-escape -interaction=nonstopmode -synctex=1 main.tex
```

如果目录、交叉引用或图片编号没有更新完整，可以连续编译两次：

如果 `minted` 报错，通常需要先安装 Pygments：

```bash
pip install Pygments
```

如果使用 VS Code 的 LaTeX Workshop，需要在 recipe 中加入 `-shell-escape` 参数。

## 添加章节

把正文写在 `chapter/` 目录下，例如：

```text
chapter/task1.tex
chapter/tesk2.tex
chapter/task3.tex
```

然后在 `main.tex` 的正文区域加入：

```latex
\input{chapter/task3.tex}
```

章节文件中可以直接从一级标题开始：

```latex
\section{Linux 平台下的基本命令}

\subsection{文件和目录操作}

这里写正文内容。
```

一级标题默认黑色并居中显示。

## 插入代码

模板使用 `minted`，适合插入 Bash、C、Python、配置文件等代码。

```latex
\begin{minted}{bash}
uname -a
ls -al
ps aux | grep ssh
\end{minted}
```

也可以指定其他语言：

```latex
\begin{minted}{c}
#include <stdio.h>

int main(void) {
    printf("Hello, Linux!\n");
    return 0;
}
\end{minted}
```

行内命令可以使用模板提供的 `\cmd`：

```latex
使用 \cmd{ls -al} 查看当前目录下的文件。
```

## 插入图片

图片建议放在 `pic/` 目录下，例如 `pic/task1/image.png`。

```latex
\begin{figure}[H]
    \centering
    \includegraphics[width=0.8\textwidth]{pic/task1/image.png}
    \caption{命令执行结果}
\end{figure}
```

如果图片文件名包含空格，LaTeX 有时会更容易出问题，建议尽量使用英文、数字、下划线命名。

## 插入表格

```latex
\begin{table}[H]
    \centering
    \caption{常用命令示例}
    \begin{tabular}{lll}
        \toprule
        命令 & 功能 & 示例 \\
        \midrule
        ls & 查看目录 & \cmd{ls -al} \\
        pwd & 查看当前位置 & \cmd{pwd} \\
        cd & 切换目录 & \cmd{cd /tmp} \\
        \bottomrule
    \end{tabular}
\end{table}
```

## 使用提示框

普通说明可以使用 `notebox`：

```latex
\begin{notebox}[注意]
执行删除命令前，应确认当前所在目录，避免误删重要文件。
\end{notebox}
```

实验步骤可以使用 `stepbox`：

```latex
\begin{stepbox}[操作步骤]
\begin{enumerate}
    \item 登录 Linux 服务器。
    \item 创建实验目录。
    \item 执行命令并截图记录。
\end{enumerate}
\end{stepbox}
```

## 修改页眉标题

`main.tex` 中有一行：

```latex
\newcommand{\reporttitle}{Linux 课程报告}
```

如果想修改页眉左侧文字，直接改这里即可。
