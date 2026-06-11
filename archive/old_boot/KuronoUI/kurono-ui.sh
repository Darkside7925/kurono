#!/bin/sh
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export TERM=linux
stty -echo -icanon time 0 min 0 2>/dev/null
printf "\033[2J\033[H"
echo "======================================="
echo "        Kurono UI (Linux drivers)       "
echo "======================================="
echo "Keyboard: type; 'q' to quit"
echo "Mouse: move to see '+' cursor"
echo "---------------------------------------"

x=40; y=12
mfifo=/tmp/mfifo
rm -f "$mfifo"; mkfifo "$mfifo" 2>/dev/null || true
if [ -r /dev/input/mice ]; then
    (cat /dev/input/mice > "$mfifo" 2>/dev/null &) 2>/dev/null
fi

draw_cursor() {
    [ "$x" -lt 1 ] && x=1
    [ "$y" -lt 1 ] && y=1
    [ "$x" -gt 78 ] && x=78
    [ "$y" -gt 22 ] && y=22
    printf "\033[%d;%dH+" "$y" "$x"
    printf "\033[%d;%dH" "$y" "$x"
}

draw_cursor

while :; do
    key=$(dd bs=1 count=1 2>/dev/null)
    if [ -n "$key" ]; then
        [ "$key" = "q" ] && break
        printf "%s" "$key"
    fi
    if [ -p "$mfifo" ]; then
        pkt=$(dd if="$mfifo" bs=3 count=1 2>/dev/null | od -An -t u1)
        if [ -n "$pkt" ]; then
            b1=$(echo "$pkt" | awk '{print $1}')
            dx=$(echo "$pkt" | awk '{print $2}')
            dy=$(echo "$pkt" | awk '{print $3}')
            [ "$dx" -gt 127 ] && dx=$((dx-256))
            [ "$dy" -gt 127 ] && dy=$((dy-256))
            x=$((x+dx)); y=$((y-dy))
            draw_cursor
        fi
    fi
    sleep 0.02
done

stty sane 2>/dev/null
printf "\033[0m" >/dev/tty 2>/dev/null
echo "\nExiting Kurono UI"
exec /bin/sh
