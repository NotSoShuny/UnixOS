#!/bin/bash

RESULT="$(pwd)/result.txt"
SERVER_LOG="/tmp/server.log"
SOCK_PATH="$(tr -d '\r\n' < config)"
CLIENT_LOGS="$(pwd)/client_logs"
NUMBERS="$(pwd)/numbers.txt"

SERVER_PID=""

pass() {
    echo "[PASS] $1" | tee -a "$RESULT"
}

fail() {
    echo "[FAIL] $1" | tee -a "$RESULT"
}

log() {
    echo "$1" | tee -a "$RESULT"
}

separation() {
    echo ""
}

start_server() {
    rm -f "$SOCK_PATH" "$SERVER_LOG"

    ./server -c config &
    SERVER_PID=$!

    i=0
    while [ ! -S "$SOCK_PATH" ] && [ "$i" -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        return 1
    fi

    if [ ! -S "$SOCK_PATH" ]; then
        return 1
    fi

    return 0
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
}

run_clients() {
    local count="$1"
    local delay="$2"
    local pids=""
    local i
    local errors=0

    rm -rf "$CLIENT_LOGS"
    mkdir -p "$CLIENT_LOGS"

    for i in $(seq 1 "$count"); do
        ./client -c config -f "$NUMBERS" -d "$delay" -i "$i" \
            -l "$CLIENT_LOGS/client_${i}.log" &
        pids="$pids $!"
    done

    for pid in $pids; do
        wait "$pid" || errors=1
    done

    return "$errors"
}

check_state() {
    printf "0\n" | ./client -c config -p 2>/dev/null | tail -n 1 | tr -d '\r'
}

extract_ts() {
    echo "$1" | sed -n 's/.*ts=\([0-9][0-9]*\).*/\1/p'
}

pkill -x server 2>/dev/null
make clean >/dev/null 2>&1
: > "$RESULT"

trap 'stop_server' EXIT

separation
log "1) Cборка проекта"
make >> "$RESULT" 2>&1 || {
    fail "Сборка не удалась"
    exit 1
}
pass "Сборка успешна"

separation
log "Генерация файла с 1000 числами (сумма = 0)"

./gen_numbers "$NUMBERS" >> "$RESULT" 2>&1 || {
    fail "Генерация чисел не удалась"
    exit 1
}
pass "Файл с числами \"numbers.txt\" создан"

separation
log "2) Запуск сервера, затем старт 100 клиентов"

start_server || {
    fail "Сервер не запустился"
    exit 1
}

log "Сервер запущен pid=$SERVER_PID"
log "Запущено 100 клиентов"

if run_clients 100 0.01; then
    log "Все клиенты завершились"
else
    fail "Произошла ошибка при ПЕРВОМ запуске клиентов"
fi

STATE1=$(check_state)
log "Тест: отправлено 0, ответ сервера: $STATE1"

if [ "$STATE1" = "0" ]; then
    pass "Состояние сервера = 0 после 100 клиентов"
else
    fail "Состояние сервера = $STATE1 (ожидалось 0)"
fi

separation
log "3) Повторный запуск 100 клиентов без перезапуска сервера"

if run_clients 100 0.01; then
    log "Все клиенты второго запуска завершились"
else
    fail "Произошла ошибка при ВТОРОМ запуске клиентов"
fi

STATE2=$(check_state)
log "Состояние сервера после повторного запуска: $STATE2"

if [ "$STATE2" = "0" ]; then
    pass "Состояние = 0 после повторного запуска"
else
    fail "Состояние = $STATE2 (ожидалось 0)"
fi

separation
log "Проверка утечек памяти и дескрипторов (Вывод первой и последней записи)"

FIRST_CONNECT=$(grep "client connected" "$SERVER_LOG" | head -1)
LAST_CONNECT=$(grep "client connected" "$SERVER_LOG" | tail -1)

log "Первая запись: $FIRST_CONNECT"
log "Последняя запись: $LAST_CONNECT"

if [ -n "$FIRST_CONNECT" ] && [ -n "$LAST_CONNECT" ]; then
    pass "Записи о подключениях в логе есть"
else
    fail "Записей о подключениях в логе нет"
fi

separation
log "4) Запуски с разным числом клиентов и задержками"
log "Ожидаемый результат: время сервера примерно равно времени самого медленного клиента"
log "Критерий: (время от первого RECV до последнего SEND) - delay самого медленного клиента"

printf "%-10s %-10s %-15s %-15s %-15s\n" \
    "clients" "delay" "server_time" "max_client_d" "overhead" | tee -a "$RESULT"
echo "-------------------------------------------------------------" | tee -a "$RESULT"

for NUM in 1 10 50 100; do
    for DELAY in 0 0.2 0.4 0.6 0.8 1.0; do
        max_client_delay=0

        stop_server
        start_server || {
            log "WARN: Сервер не запустился для clients=$NUM delay=$DELAY"
            continue
        }

        if ! run_clients "$NUM" "$DELAY"; then
            log "WARN: Ошибка клиентов для clients=$NUM delay=$DELAY"
            continue
        fi

        FIRST_RECV=$(grep "^RECV " "$SERVER_LOG" | head -1)
        LAST_SEND=$(grep "^SEND " "$SERVER_LOG" | tail -1)

        FIRST_TS=$(extract_ts "$FIRST_RECV")
        LAST_TS=$(extract_ts "$LAST_SEND")

        if [ -n "$FIRST_TS" ] && [ -n "$LAST_TS" ]; then
            SERVER_MS=$((LAST_TS - FIRST_TS))
        else
            SERVER_MS=0
        fi

        for LOGF in "$CLIENT_LOGS"/client_*.log; do
            [ -f "$LOGF" ] || continue

            D=$(sed -n 's/.*total_delay=\([0-9.]*\).*/\1/p' "$LOGF")
            if [ -n "$D" ]; then
                D_MS=$(awk -v x="$D" 'BEGIN { printf "%d", x * 1000 }')
                if [ "$D_MS" -gt "$max_client_delay" ]; then
                    max_client_delay="$D_MS"
                fi
            fi
        done

        OVERHEAD=$((SERVER_MS - max_client_delay))

        printf "%-10s %-10s %-15s %-15s %-15s\n" \
            "$NUM" "$DELAY" "${SERVER_MS}ms" "${max_client_delay}ms" "${OVERHEAD}ms" \
            | tee -a "$RESULT"
    done
done

separation
log "Остановка сервера. Конец тестирования"
stop_server