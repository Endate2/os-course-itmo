#!/usr/bin/env bash
# Прогон всех сценариев во всех конфигурациях кэша.
#
#   ./run.sh                          всё по умолчанию
#   REPEAT=5 ./run.sh                 больше повторов
#   WORKLOADS="hot rand" ./run.sh     только выбранные сценарии
#   CONFIGS="oscache vtpc" ./run.sh   только выбранные конфигурации
#   WARM=1 ./run.sh                   перед каждым прогоном файл прогревается в кэше ОС
#                                     (по умолчанию — наоборот, выкидывается: холодный старт)
#
# WORKDIR должен лежать на настоящей ФС (ext4/xfs/btrfs), НЕ на tmpfs:
# на tmpfs O_DIRECT не работает, и библиотека молча отдаёт файл без кэша
# (в выводе это видно как vtpc=off).
set -euo pipefail
export LC_ALL=C   # иначе в русской локали awk читает 0.1234 как 0

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH="$HERE/bench"
WORKDIR=${WORKDIR:-$HERE/data}
FILE_MB=${FILE_MB:-64}
REPEAT=${REPEAT:-3}
WARM=${WARM:-0}
WORKLOADS=${WORKLOADS:-"seq4k seqsmall hot smallwrite hotwrite rand seq1m"}
CONFIGS=${CONFIGS:-"nocache oscache vtpc vtpc_nora vtpc_os vtpc_os_nora"}

DATA="$WORKDIR/data.bin"
OUT="$WORKDIR/out.bin"
RES="$HERE/results"
RAW=${SUMMARY:-$RES/raw_$(date +%Y%m%d_%H%M%S).txt}

mkdir -p "$WORKDIR" "$RES"
for f in bench libvtpc.so libvtpc_nora.so libvtpc_os.so libvtpc_os_nora.so; do
  [[ -e "$HERE/$f" ]] || { echo "нет $f — сначала выполните make" >&2; exit 1; }
done

if [[ ! -f "$DATA" || $(stat -c %s "$DATA") -ne $((FILE_MB << 20)) ]]; then
  echo "создаю $DATA ($FILE_MB МиБ)..."
  "$BENCH" gen "$DATA" --size "${FILE_MB}M"
fi

# конфигурация -> библиотека для LD_PRELOAD и флаги нагрузчика
cfg_preload() {
  case $1 in
    nocache|oscache) echo "" ;;
    vtpc)            echo "$HERE/libvtpc.so" ;;
    vtpc_nora)       echo "$HERE/libvtpc_nora.so" ;;
    vtpc_os)         echo "$HERE/libvtpc_os.so" ;;
    vtpc_os_nora)    echo "$HERE/libvtpc_os_nora.so" ;;
    *) echo "неизвестная конфигурация $1" >&2; exit 1 ;;
  esac
}
cfg_flags() { [[ $1 == nocache ]] && echo "--direct" || echo ""; }

# сценарий -> аргументы нагрузчика
wl_args() {
  case $1 in
    seq4k)      echo "seq $DATA --bs 4096" ;;                          # readahead
    seqsmall)   echo "seq $DATA --bs 128 --ops 65536" ;;               # мелкие чтения подряд
    hot)        echo "hot $DATA --bs 4096 --hot 512K --ops 100000" ;;  # рабочее множество < кэша
    smallwrite) echo "write $OUT --bs 64 --total 2M" ;;                # мелкие записи подряд
    hotwrite)   echo "write $OUT --bs 64 --total 256K --ops 30000 --rand" ;; # перезапись горячей области
    rand)       echo "rand $DATA --bs 4096 --ops 20000" ;;             # неблагоприятный: файл >> кэша
    seq1m)      echo "seq $DATA --bs 1M" ;;                            # неблагоприятный: крупные блоки
    *) echo "неизвестный сценарий $1" >&2; exit 1 ;;
  esac
}

if [[ -n ${SUMMARY:-} ]]; then
  WORKLOADS=""   # только пересчитать сводку по готовому файлу
  REPEAT=$(awk '{for(i=1;i<=NF;i++) if($i~/^run=/){split($i,a,"=");if(a[2]>m)m=a[2]}} END{print m}' "$RAW")
fi
echo "результаты: $RAW"
for wl in $WORKLOADS; do
  read -r -a args <<< "$(wl_args "$wl")"
  for cfg in $CONFIGS; do
    pre=$(cfg_preload "$cfg")
    read -r -a flags <<< "$(cfg_flags "$cfg")"
    for ((r = 1; r <= REPEAT; r++)); do
      if [[ ${args[0]} == write ]]; then
        rm -f "$OUT"
      elif [[ $WARM == 1 ]]; then
        cat "$DATA" > /dev/null
      else
        "$BENCH" drop "$DATA"
      fi

      line=$(env ${pre:+LD_PRELOAD="$pre"} "$BENCH" "${args[@]}" "${flags[@]}")

      if [[ ${args[0]} == write ]]; then
        sum=$(md5sum "$OUT" | cut -c1-16)
        line=${line/cksum=0000000000000000/cksum=$sum}
      fi
      echo "cfg=$cfg case=$wl run=$r $line" | tee -a "$RAW"
    done
  done
done

echo
echo "Средние за $REPEAT прогон(а); disk_MB — реально прочитано+записано с диска (по ru_inblock/oublock)"
awk '
function kv(s,   i, a, n) { n = split(s, a, " "); for (i = 1; i <= n; i++) { split(a[i], p, "="); F[p[1]] = p[2] } }
{
  delete F; kv($0)
  k = F["case"] SUBSEP F["cfg"]
  if (!(k in N)) order[++no] = k
  N[k]++; W[k] += F["wall"]; U[k] += F["user"]; S[k] += F["sys"]
  D[k] += (F["inblk"] + F["outblk"]) * 512 / 1048576
  V[k] = F["vtpc"]
  if (!(F["case"] in C)) C[F["case"]] = F["cksum"]
  else if (C[F["case"]] != F["cksum"]) BAD[F["case"]] = 1
}
END {
  printf "%-11s %-13s %9s %9s %9s %9s %5s\n", "case", "config", "wall,s", "user,s", "sys,s", "disk_MB", "vtpc"
  prev = ""
  for (i = 1; i <= no; i++) {
    k = order[i]; split(k, kk, SUBSEP)
    if (prev != "" && kk[1] != prev) print ""
    prev = kk[1]
    printf "%-11s %-13s %9.4f %9.4f %9.4f %9.1f %5s\n", kk[1], kk[2], W[k]/N[k], U[k]/N[k], S[k]/N[k], D[k]/N[k], V[k]
  }
  print ""
  for (c in C) printf "%-11s контрольные суммы %s\n", c, (c in BAD) ? "РАЗЛИЧАЮТСЯ — ошибка!" : "совпадают"
}' "$RAW" | tee "${RAW%.txt}_summary.txt"