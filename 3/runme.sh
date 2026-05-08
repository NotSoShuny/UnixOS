#!/bin/bash

RESULT="$(pwd)/result.txt"
LOG="/tmp/myinit.log"
MYINIT="$(pwd)/myinit"
CFG="$(pwd)/config.txt"
TESTDIR="$(pwd)/testbin"
IN="/tmp/myinit_in"

pass() {
    echo "[PASS] $1" | tee -a "$RESULT"
}

fail() {
    echo "[FAIL] $1" | tee -a "$RESULT"
}

log() {
    echo "$1" | tee -a "$RESULT"
}

sep() {
    echo "" | tee -a "$RESULT"
    echo "--------------------------------------------" | tee -a "$RESULT"
}

get_children() {
    local ppid="$1"
    ps -eo pid,ppid,args | awk -v p="$ppid" '
        $2 == p && ($0 ~ /proc1.sh/ || $0 ~ /proc2.sh/ || $0 ~ /proc3.sh/) { print }
    '
}

count_children() {
    get_children "$1" | wc -l | tr -d ' '
}

get_myinit_pid() {
    ps -eo pid,comm | awk '$2=="myinit" { print $1; exit }'
}

cleanup_all() {
    pkill -x myinit 2>/dev/null
    pkill -f "$TESTDIR/proc1.sh" 2>/dev/null
    pkill -f "$TESTDIR/proc2.sh" 2>/dev/null
    pkill -f "$TESTDIR/proc3.sh" 2>/dev/null
}

make clean >/dev/null 2>&1
: > "$RESULT"

sep
echo "шаг 1: сборка проекта" | tee -a "$RESULT"
echo "ожидаемый результат: make завершается без ошибок" | tee -a "$RESULT"

make >> "$RESULT" 2>&1 || {
    fail "сборка не удалась"
    exit 1
}
pass "сборка успешна"

cleanup_all
sleep 1

rm -rf "$TESTDIR"
mkdir -p "$TESTDIR"
touch "$IN"
rm -f "$LOG"

cat > "$TESTDIR/proc1.sh" <<'EOF'
#!/bin/sh
while true
do
    sleep 1
done
EOF

cat > "$TESTDIR/proc2.sh" <<'EOF'
#!/bin/sh
while true
do
    sleep 1
done
EOF

cat > "$TESTDIR/proc3.sh" <<'EOF'
#!/bin/sh
while true
do
    sleep 1
done
EOF

chmod +x "$TESTDIR/proc1.sh"
chmod +x "$TESTDIR/proc2.sh"
chmod +x "$TESTDIR/proc3.sh"

cat > "$CFG" <<EOF
$TESTDIR/proc1.sh $IN /tmp/myinit_out1
$TESTDIR/proc2.sh $IN /tmp/myinit_out2
$TESTDIR/proc3.sh $IN /tmp/myinit_out3
EOF

sep
echo "шаг 2: запускаем myinit с конфигом на 3 процесса" | tee -a "$RESULT"
echo "ожидаемый результат: ps показывает 3 дочерних процесса myinit" | tee -a "$RESULT"

"$MYINIT" -c "$CFG"
sleep 2

MPID=$(get_myinit_pid)

# Если myinit не запустился, завершаем выполнение
[ -n "$MPID" ] || {
    fail "не удалось найти PID myinit после запуска"
    cleanup_all
    exit 1
}

echo "myinit pid=$MPID" | tee -a "$RESULT"
echo "вывод ps:" | tee -a "$RESULT"
get_children "$MPID" | tee -a "$RESULT"

cnt=$(count_children "$MPID")
if [ "$cnt" -eq 3 ]; then
    pass "запущено 3 процесса (найдено: $cnt)"
else
    fail "ожидалось 3 процесса, найдено $cnt"
fi

sep
echo "шаг 3: убиваем proc2.sh через pkill, через 1 сек проверяем рестарт" | tee -a "$RESULT"
echo "ожидаемый результат: ps снова показывает 3 процесса" | tee -a "$RESULT"

PID2=$(get_children "$MPID" | grep "proc2.sh" | awk '{print $1}' | head -1)
if [ -n "$PID2" ]; then
    pkill -f "$TESTDIR/proc2.sh"
    echo "proc2.sh (pid=$PID2) убит" | tee -a "$RESULT"
else
    echo "WARN: proc2.sh не найден" | tee -a "$RESULT"
fi

sleep 1

echo "вывод ps через 1 сек после убийства proc2:" | tee -a "$RESULT"
get_children "$MPID" | tee -a "$RESULT"

cnt=$(count_children "$MPID")
if [ "$cnt" -eq 3 ]; then
    pass "proc2 перезапущен, итого 3 (найдено: $cnt)"
else
    fail "ожидалось 3 процесса, найдено $cnt"
fi

sep
echo "шаг 4: заменяем конфиг на 1 процесс, отправляем SIGHUP" | tee -a "$RESULT"
echo "ожидаемый результат: 3 старых процесса завершатся, запустится 1 новый" | tee -a "$RESULT"

cat > "$CFG" <<EOF
$TESTDIR/proc1.sh $IN /tmp/myinit_out1
EOF

if [ -n "$MPID" ]; then
    kill -HUP "$MPID"
    echo "SIGHUP отправлен pid=$MPID" | tee -a "$RESULT"
else
    fail "pid myinit не найден"
fi

sleep 2

echo "вывод ps после SIGHUP:" | tee -a "$RESULT"
get_children "$MPID" | tee -a "$RESULT"

cnt=$(count_children "$MPID")
if [ "$cnt" -eq 1 ]; then
    pass "после SIGHUP работает 1 процесс (найдено: $cnt)"
else
    fail "после SIGHUP ожидался 1 процесс, найдено $cnt"
fi

sep
echo "шаг 5: проверяем лог $LOG" | tee -a "$RESULT"
echo "ожидаемый результат:" | tee -a "$RESULT"
echo "  - старт proc[0], proc[1], proc[2] в начале" | tee -a "$RESULT"
echo "  - завершение proc[1] (proc2.sh) и его рестарт" | tee -a "$RESULT"
echo "  - завершение proc[0], proc[1], proc[2] по SIGHUP (on reload)" | tee -a "$RESULT"
echo "  - старт proc[0] (proc1.sh) после SIGHUP, proc[1] и proc[2] не стартуют" | tee -a "$RESULT"

if [ ! -f "$LOG" ]; then
    fail "лог-файл не найден"
else
    echo
    echo "--- содержимое лога ---" | tee -a "$RESULT"
    cat "$LOG" | tee -a "$RESULT"
    echo "--- конец лога ---" | tee -a "$RESULT"
    echo

    START0=$(grep "started proc\[0\]" "$LOG" | grep "proc1.sh" | head -1)
    START1=$(grep "started proc\[1\]" "$LOG" | grep "proc2.sh" | head -1)
    START2=$(grep "started proc\[2\]" "$LOG" | grep "proc3.sh" | head -1)

    if [ -n "$START0" ] && [ -n "$START1" ] && [ -n "$START2" ]; then
        pass "все 3 процесса зафиксированы в логе при старте"
    else
        fail "не все 3 процесса зафиксированы в логе при старте"
    fi

    RESTART1=$(grep "proc\[1\]" "$LOG" | grep "restarting")
    if [ -n "$RESTART1" ]; then
        pass "завершение и рестарт proc[1] (proc2.sh) зафиксированы"
    else
        fail "рестарт proc[1] не найден в логе"
    fi

    RELOAD0=$(grep "proc\[0\]" "$LOG" | grep "on reload")
    RELOAD1=$(grep "proc\[1\]" "$LOG" | grep "on reload")
    RELOAD2=$(grep "proc\[2\]" "$LOG" | grep "on reload")

    if [ -n "$RELOAD0" ] && [ -n "$RELOAD1" ] && [ -n "$RELOAD2" ]; then
        pass "завершение всех 3 процессов по SIGHUP зафиксировано"
    else
        fail "не все процессы зафиксированы как завершённые при reload"
    fi

    HUP_LINE=$(grep -n "got SIGHUP" "$LOG" | tail -1 | cut -d: -f1)

    if [ -n "$HUP_LINE" ]; then
        AFTER_HUP_START0=$(tail -n +"$HUP_LINE" "$LOG" | grep -c "started proc\[0\].*proc1.sh")
        AFTER_HUP_BAD=$(tail -n +"$HUP_LINE" "$LOG" | grep "started proc\[1\]\|started proc\[2\]")
    else
        AFTER_HUP_START0=0
        AFTER_HUP_BAD=""
    fi

    if [ "$AFTER_HUP_START0" -ge 1 ] && [ -z "$AFTER_HUP_BAD" ]; then
        pass "после SIGHUP запущен только 1 процесс (proc1.sh)"
    else
        fail "после SIGHUP запущено не то что ожидалось"
    fi
fi

cleanup_all
rm -f /tmp/myinit_out* "$IN" "$LOG" "$CFG"
rm -rf "$TESTDIR"