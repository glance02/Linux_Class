#!/usr/bin/env bash

QUESTION_FILE="./questions.txt"
SCORE_FILE="./score.txt"
WRONG_LIMIT=2

# 初始化运行环境
init_env() {
    if [ ! -f "$SCORE_FILE" ]; then
        touch "$SCORE_FILE"
    fi
}

# 暂停，便于用户查看结果
pause_screen() {
    echo
    read -r -p "按 Enter 键返回主菜单..."
}

# 显示主菜单
show_menu() {
    echo
    echo "===================================="
    echo "       Linux 命令答题系统"
    echo "===================================="
    echo "1. 开始答题"
    echo "2. 查看历史成绩"
    echo "3. 清空历史成绩"
    echo "0. 退出系统"
    echo "===================================="
    echo
    echo "PS：答错 $WRONG_LIMIT 道题后系统会自动结束"
    echo
}

# 核心答题功能
start_quiz() {
    score=0
    wrong=0
    total=0
    answered=0

    if [ ! -f "$QUESTION_FILE" ]; then
        echo "题库文件不存在：$QUESTION_FILE"
        return
    fi

    # 使用文件描述符 3 读取题库，避免用户输入被题库文件提前消费。
    while IFS='|' read -r question correct_answer <&3; do
        # 跳过空行
        if [ -z "$question" ]; then
            continue
        fi

        total=$((total + 1))

        echo "------------------------------------"
        echo "第 $total 题：$question"
        read -r -p "请输入答案：" user_answer

        # 去掉用户输入前后的空格
        user_answer="$(echo "$user_answer" | xargs)"

        if [ -z "$user_answer" ]; then
            echo "答案不能为空，本题判定为错误。"
            wrong=$((wrong + 1))
        elif [ "$user_answer" = "$correct_answer" ]; then
            echo "回答正确，+1 分。"
            score=$((score + 1))
            answered=$((answered + 1))
        else
            echo "回答错误！正确答案是：$correct_answer"
            wrong=$((wrong + 1))
            answered=$((answered + 1))
        fi

        echo "当前得分：$score，错误次数：$wrong/$WRONG_LIMIT"
        echo

        if [ "$wrong" -ge "$WRONG_LIMIT" ]; then
            echo
            echo "错误次数达到 $WRONG_LIMIT 次，答题结束。"
            break
        fi

        read -r -p "按 Enter 继续下一题..."
        clear

    done 3< "$QUESTION_FILE"

    echo
    echo "========== 答题结果 =========="
    echo "本次得分：$score"
    echo "已答题数：$answered"
    echo "错误题数：$wrong"

    save_score "$score" "$answered" "$wrong"
}

# 保存成绩
save_score() {
    current_score="$1"
    answered_count="$2"
    wrong_count="$3"
    current_time="$(date '+%Y-%m-%d %H:%M:%S')"

    echo "$current_time|$current_score|$answered_count|$wrong_count" >> "$SCORE_FILE"

    if [ -w "$SCORE_FILE" ]; then
        echo "成绩已保存到 $SCORE_FILE"
    else
        echo "成绩文件不可写，保存失败。"
    fi
}

# 查看历史成绩，并在最上方显示最高分
show_history() {
    echo
    echo "========== 历史成绩 =========="

    if [ ! -f "$SCORE_FILE" ]; then
        echo "暂无成绩文件。"
        return
    fi

    if [ ! -s "$SCORE_FILE" ]; then
        echo "暂无历史成绩。"
        return
    fi

    highest_score="$(awk -F'|' 'BEGIN {max=0} {if ($2 > max) max=$2} END {print max}' "$SCORE_FILE")"

    echo "最高分：$highest_score"
    echo "------------------------------------"
    echo "时间 | 得分 | 已答题数 | 错误题数"
    echo "------------------------------------"

    while IFS='|' read -r time score answered wrong; do
        if [ -n "$time" ]; then
            echo "$time | $score | $answered | $wrong"
        fi
    done < "$SCORE_FILE"
}

# 清空历史成绩
clear_history() {
    echo
    read -r -p "确认清空历史成绩吗？输入 y 确认：" confirm

    if [ "$confirm" = "y" ] || [ "$confirm" = "Y" ]; then
        > "$SCORE_FILE"
        echo "历史成绩已清空。"
    else
        echo "已取消清空操作。"
    fi
}

# 主程序入口
init_env

while true; do
    clear
    show_menu
    read -r -p "请输入功能编号：" choice

    case "$choice" in
        1)
            clear
            start_quiz
            pause_screen
            ;;
        2)
            clear
            show_history
            pause_screen
            ;;
        3)
            clear_history
            pause_screen
            ;;
        0)
            echo "系统已退出。"
            exit 0
            ;;
        *)
            echo "输入错误，请输入 0 到 3 之间的数字。"
            ;;
    esac
done