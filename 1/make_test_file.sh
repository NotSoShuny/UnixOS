#!/bin/bash

OUTFILE="fileA"
SIZE=$((4*1024*1024 + 1))

echo "Create $OUTFILE with real zero's"

# Заполняем нулями
head -c "$SIZE" /dev/zero > "$OUTFILE"

# Пишем 1 по нужным смещениям
printf '\1' | dd of="$OUTFILE" bs=1 seek=0 conv=notrunc status=none
printf '\1' | dd of="$OUTFILE" bs=1 seek=10000 conv=notrunc status=none
printf '\1' | dd of="$OUTFILE" bs=1 seek=$((SIZE - 1)) conv=notrunc status=none