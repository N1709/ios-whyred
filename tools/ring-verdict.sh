#!/bin/sh
# Classify a pulled SANTONI ring: which kernel generation produced it, whether
# XNU/SpringBoard stayed alive, and whether the new display-power code ran.
#   tools/ring-verdict.sh RING.txt
#
# The ring records the boot-args at launchd start. Use this before drawing any
# conclusion from a screenshot: the 2026-11-22 "0226 blanked the panel" report
# came from a ring whose args match the older 5985a74 image.
set -eu
ring=${1:?usage: ring-verdict.sh RING.txt}
[ -f "$ring" ] || { echo "no such ring: $ring" >&2; exit 1; }

argline=$(sed -n 's/^.*boot-args = //p' "$ring" | head -1)
has_arg() {
	case " $argline " in
	*" $1 "*) return 0 ;;
	*) return 1 ;;
	esac
}
count() { grep -c -- "$1" "$ring" 2>/dev/null || true; }

native=0; has_arg sweetfb_native=1 && native=1
noconsole=0; has_arg sweetfb_noconsole=1 && noconsole=1
stayon=0; has_arg sweetfb_stayon=1 && stayon=1
touchpower=0; has_arg sweetouchpower=1 && touchpower=1
qup=0; has_arg sweeti2c=1 && qup=1

spring=$(count '^exec SpringBoard$')
backboard=$(count '^exec backboardd$')
presents=$(count 'SWEET FB: present ')
last_present=$(grep -o 'SWEET FB: present [0-9]*' "$ring" | tail -1 | awk '{print $4}')
[ -n "$last_present" ] || last_present=0

old_veto=$(count 'display off ignored (sweetfb_stayon)')
new_veto=$(count 'display off ignored before the first frame')
sleep_line=$(count 'WHYRED TOUCH: display off, HID paused')
wake_line=$(count 'WHYRED TOUCH: display on, HID resumed')
nosleep=$(count 'NO_SLEEP_ON')

nxprov=$(count 'nxprov')
migrator=$(count '^exec com.apple.datamigrator$')
poster=$(count '^CRASH PosterBoard\[')

if [ "$new_veto" -gt 0 ] || [ "$sleep_line" -gt 0 ] || \
    [ "$wake_line" -gt 0 ] || [ "$nosleep" -gt 0 ]; then
	if [ "$old_veto" -gt 0 ]; then
		image="mixed pre/post-0225 markers (check the args)"
	else
		image="post-0225 (0225/0226 code present)"
	fi
elif [ "$old_veto" -gt 0 ]; then
	image="pre-0225 (old stayon veto; 0225/0226 did not run)"
else
	image="unknown (neither the old veto nor a 0225/0226 marker)"
fi

if [ "$spring" -gt 0 ] && [ "$presents" -gt 0 ]; then
	alive="yes (SpringBoard exec $spring, backboardd exec $backboard, presents $presents, last $last_present)"
else
	alive="NO (SpringBoard exec $spring, presents $presents: read the tail)"
fi

echo "ring:      $ring"
echo "image:     $image"
echo "args:      native=$native noconsole=$noconsole stayon=$stayon touchpower=$touchpower sweeti2c=$qup"
echo "kernel:    alive=$alive"
echo "display:   old-veto=$old_veto new-veto=$new_veto sleep=$sleep_line wake=$wake_line nosleep=$nosleep"
off_line=$(grep -n '^SWEET FB: display off$' "$ring" | tail -1 | cut -d: -f1)
if [ -n "$off_line" ]; then
	after=$(awk -v n="$off_line" 'NR > n && /SWEET FB: present / { c++ } END { print c + 0 }' "$ring")
	echo "off-guard: last display off at line $off_line; presents after it: $after"
	if [ "$after" -gt 0 ]; then
		echo "warning:    presents after display off; 0228 is not holding (or this ring predates it)"
	fi
fi
echo "console:   nxprov=$nxprov (normal Skywalk provider registration, not a stall)"
echo "userland:  datamigrator=$migrator PosterBoard-crash=$poster"

if [ "$noconsole" -eq 1 ] && [ "$nxprov" -gt 0 ]; then
	echo "warning:    sweetfb_noconsole=1 but nxprov is in the ring; the console may not have been muted"
fi
if [ "$migrator" -gt 0 ]; then
	echo "stage:     userland boot/migration daemon ran $migrator time(s); a progress screen is not a kernel hang"
fi
if [ "$poster" -gt 0 ]; then
	echo "poster:    PosterBoard trapped (known; 0216 sweetnopb=1 is the isolation lever)"
fi
if [ "$image" != "post-0225 (0225/0226 code present)" ] && [ "$image" != "mixed pre/post-0225 markers (check the args)" ]; then
	echo "verdict:   this ring does not test the 0225/0226 display-power path"
fi
