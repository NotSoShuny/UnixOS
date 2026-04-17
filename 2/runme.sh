#!/bin/bash
set -e

LOGFILE="result.txt"
STATS="stats.txt"
SHARED_FILE="sharedfile"
TEST_DURATION=300
NUM_TASKS=10

{
    make clean 2>&1 || true
    make 2>&1
} > "$LOGFILE"

{
    touch "$SHARED_FILE"
    PIDS=()
    for i in $(seq 1 "$NUM_TASKS"); do
        ./locker -f "$SHARED_FILE" -s "$STATS" &
        PIDS+=($!)
    done

    echo "PIDs: ${PIDS[*]}" # Вывод 10 PID  
    sleep "$TEST_DURATION"

    for pid in "${PIDS[@]}"; do # Посылаем SIGINT
        kill -SIGINT "$pid" 2>/dev/null || true
    done

    for pid in "${PIDS[@]}"; do # Ждём завершения всех задач
        wait "$pid" 2>/dev/null || true
    done

    rm -f "${SHARED_FILE}.lck"
} >> "$LOGFILE"

{
    echo
    echo "--- PID STATS ---"
    if [ -f "$STATS" ]; then
        cat "$STATS"
    else
        echo "File with stats doesn't exist"
        exit 1
    fi
} >> "$LOGFILE"

{
    echo
    TASKS_COUNT=$(wc -l < "$STATS")
    if [ "$TASKS_COUNT" -eq "$NUM_TASKS" ]; then
        echo "All $NUM_TASKS tasks were completed"
    else
        echo "Expected $NUM_TASKS tasks, got $TASKS_COUNT"
    fi

    LOCKS=$(awk '{print $3}' "$STATS" | sort -n) # Считаем статистику блокировок
    MIN=$(echo "$LOCKS" | head -1)
    MAX=$(echo "$LOCKS" | tail -1)
    SUM=$(echo "$LOCKS" | awk '{s+=$1} END {print s}')
    AVG=$((SUM / TASKS_COUNT))

    echo "Min=$MIN Max=$MAX Avg=$AVG"

    if [ "$MIN" -gt 0 ]; then
        echo "All tasks acquired at least 1 lock"
    else
        echo "Some tasks got 0 locks (deadlock suspected)"
    fi

    if [ "$MIN" -gt 0 ]; then  # (нужно чтобы статистика для каждой задачи была примерно одинаковой)
        awk -v max="$MAX" -v avg="$AVG" 'BEGIN {
            ratio = max / avg 
            printf "Ratio Max/Average=%.2f\n", ratio
            if (ratio < 2)
                print "The number of locks does not differ much"
            else
                print "The number of locks varies" 
        }'
    else
        echo "Error, Min locks = 0, need 1 at least"
    fi
} >> "$LOGFILE"

cat "$LOGFILE"