#!/bin/bash
set -e

LOGFILE="result.txt" # Имя файла где будет результат

# Сборка программы
{
  make clean 2>&1 || true # Удаляем прошлые файлы перед выполнением
  make 2>&1
} > "$LOGFILE"

{
  echo
  echo "1) Create fileA"
  ./make_test_file.sh 2>&1
} >> "$LOGFILE"

{
  echo
  echo "2) Sparse fileB"
  ./myprogram fileA fileB 2>&1
} >> "$LOGFILE"

{
  echo
  echo "3) Compress files"
  gzip -c fileA > fileA.gz
  gzip -c fileB > fileB.gz
} >> "$LOGFILE"

{
  echo
  echo "4) Unzip fileB.gz into fileC"
  gzip -cd fileB.gz | ./myprogram fileC 2>&1
} >> "$LOGFILE"

{
  echo
  echo "5) Create fileD (with uncommon sizeblock = 100)"
  ./myprogram -b 100 fileA fileD 2>&1
} >> "$LOGFILE"

{
  echo
  echo "6) File stats:"
  stat -c "%n size:%s blocks:%b" fileA fileB fileC fileD fileA.gz fileB.gz
} >> "$LOGFILE"

{
  echo
  echo "Disk usage:"
  du -h fileA fileB fileC fileD fileA.gz fileB.gz
} >> "$LOGFILE"

cat "$LOGFILE"